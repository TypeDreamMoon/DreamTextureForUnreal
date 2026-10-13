// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureThumbCache.h"

#include "Async/Async.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "DreamTextureLog.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

FDreamTextureThumbCache::FDreamTextureThumbCache(int32 InMaxEntries, FFetchFn InFetch, int32 InThumbMaxEdge)
	: MaxEntries(FMath::Max(InMaxEntries, 8))
	, ThumbEdge(FMath::Max(InThumbMaxEdge, 16))
	, Fetch(MoveTemp(InFetch))
{
	if (!Fetch)
	{
		Fetch = [](const FString& Id, FDreamTextureApi::FOnBytes Done)
		{
			FDreamTextureApi::FetchFile(Id, TEXT("preview.png"), MoveTemp(Done));
		};
	}

	// 模块加载只能在游戏线程；后台任务里直接用这个指针，不再碰 FModuleManager。
	Wrapper = &FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
}

FDreamTextureThumbCache::~FDreamTextureThumbCache()
{
	// FSlateDynamicImageBrush 析构时会把渲染器资源的释放排到渲染线程，
	// 所以必须在游戏线程销毁。缓存是面板的成员，面板在游戏线程析构，天然满足。
	Brushes.Reset();
}

//////////////////////////////////////////////////////////////////////////
// 路径

FString FDreamTextureThumbCache::GetDiskDir()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamTexture") / TEXT("Thumbs"));
}

FString FDreamTextureThumbCache::GetDiskPath(const FString& Id)
{
	// id 是网络来的字符串，哪怕它「应该」是 mat_xxx 也不能直接拼进路径：
	// 带 ../ 的 id 会写到缓存目录外面去。
	return GetDiskDir() / (FPaths::MakeValidFileName(Id) + TEXT(".png"));
}

//////////////////////////////////////////////////////////////////////////
// 解码

bool FDreamTextureThumbCache::DecodeAndShrink(IImageWrapperModule& InWrapper, TConstArrayView<uint8> Source, int32 MaxEdge,
	int32& OutWidth, int32& OutHeight, TArray<uint8>& OutBGRA, TArray64<uint8>* OutPng)
{
	if (Source.Num() == 0 || MaxEdge <= 0)
	{
		return false;
	}

	// 按内容认格式，不按文件名：后端给的 preview 虽然叫 .png，但这是网络来的字节。
	const EImageFormat Format = InWrapper.DetectImageFormat(Source.GetData(), Source.Num());
	if (Format == EImageFormat::Invalid)
	{
		return false;
	}

	TSharedPtr<IImageWrapper> Decoder = InWrapper.CreateImageWrapper(Format);
	if (!Decoder.IsValid() || !Decoder->SetCompressed(Source.GetData(), Source.Num()))
	{
		return false;
	}

	const int64 SrcW = Decoder->GetWidth();
	const int64 SrcH = Decoder->GetHeight();
	if (SrcW <= 0 || SrcH <= 0 || SrcW > 32768 || SrcH > 32768)
	{
		return false;
	}

	TArray64<uint8> Raw;
	if (!Decoder->GetRaw(ERGBFormat::BGRA, 8, Raw) || Raw.Num() < SrcW * SrcH * 4)
	{
		return false;
	}

	// 只缩不放：原图比缩略图尺寸还小时原样用。
	const double Scale = FMath::Min(1.0, static_cast<double>(MaxEdge) / static_cast<double>(FMath::Max(SrcW, SrcH)));
	const int32 DstW = FMath::Max(1, FMath::RoundToInt32(SrcW * Scale));
	const int32 DstH = FMath::Max(1, FMath::RoundToInt32(SrcH * Scale));

	OutWidth = DstW;
	OutHeight = DstH;
	OutBGRA.SetNumUninitialized(static_cast<int64>(DstW) * DstH * 4);

	if (DstW == SrcW && DstH == SrcH)
	{
		FMemory::Memcpy(OutBGRA.GetData(), Raw.GetData(), OutBGRA.Num());
	}
	else
	{
		// bResizeSRGBinLinearSpace=false：缩略图不值得为精确的线性空间插值多花一倍时间。
		FImageUtils::ImageResize(
			static_cast<int32>(SrcW), static_cast<int32>(SrcH),
			TArrayView<const FColor>(reinterpret_cast<const FColor*>(Raw.GetData()), SrcW * SrcH),
			DstW, DstH,
			TArrayView<FColor>(reinterpret_cast<FColor*>(OutBGRA.GetData()), static_cast<int64>(DstW) * DstH),
			false);
	}

	if (OutPng)
	{
		TSharedPtr<IImageWrapper> Encoder = InWrapper.CreateImageWrapper(EImageFormat::PNG);
		if (Encoder.IsValid() && Encoder->SetRaw(OutBGRA.GetData(), OutBGRA.Num(), DstW, DstH, ERGBFormat::BGRA, 8))
		{
			*OutPng = Encoder->GetCompressed();
		}
		else
		{
			// 编码失败只是这张没法落盘，画刷照样能建，所以不算整体失败。
			OutPng->Reset();
		}
	}
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 取画刷

bool FDreamTextureThumbCache::HasBrush(const FString& Id) const
{
	return Brushes.Contains(Id);
}

const FSlateBrush* FDreamTextureThumbCache::GetBrush(const FString& Id)
{
	if (const TSharedPtr<FSlateDynamicImageBrush>* Found = Brushes.Find(Id))
	{
		Touch(Id);
		return Found->Get();
	}

	if (Id.IsEmpty() || InFlight.Contains(Id))
	{
		return nullptr;
	}

	if (const double* FailedAt = Failed.Find(Id))
	{
		if (FPlatformTime::Seconds() - *FailedAt < RetryAfterSeconds)
		{
			return nullptr;
		}
		Failed.Remove(Id);
	}

	StartLoad(Id);
	return nullptr;
}

void FDreamTextureThumbCache::ResetFailures()
{
	Failed.Reset();
}

void FDreamTextureThumbCache::Touch(const FString& Id)
{
	// 列表很短（≤MaxEntries），线性查找比维护链表简单，而且每帧每张可见卡片才一次。
	if (LruOrder.Num() > 0 && LruOrder.Last() == Id)
	{
		return;
	}
	LruOrder.Remove(Id);
	LruOrder.Add(Id);
}

void FDreamTextureThumbCache::Trim()
{
	// 淘汰最久没用的。可见的卡片每帧都在 Touch，所以不会被淘汰；
	// 滚出视野的卡片之后再滚回来，会从磁盘缓存重新解码，很快。
	while (Brushes.Num() > MaxEntries && LruOrder.Num() > 0)
	{
		const FString Oldest = LruOrder[0];
		LruOrder.RemoveAt(0);
		Brushes.Remove(Oldest);
	}
}

void FDreamTextureThumbCache::MarkFailed(const FString& Id, const FString& Reason)
{
	InFlight.Remove(Id);
	Failed.Add(Id, FPlatformTime::Seconds());
	UE_LOG(LogDreamTexture, Verbose, TEXT("Thumbnail %s was not loaded: %s"), *Id, *Reason);
}

//////////////////////////////////////////////////////////////////////////
// 加载

void FDreamTextureThumbCache::StartLoad(const FString& Id)
{
	InFlight.Add(Id);

	const FString DiskPath = GetDiskPath(Id);
	TWeakPtr<FDreamTextureThumbCache> WeakSelf = AsShared();
	IImageWrapperModule* WrapperPtr = Wrapper;
	const int32 Edge = ThumbEdge;

	// 第一步在后台：读磁盘缓存并解码。文件 IO 也不该占着游戏线程。
	Async(EAsyncExecution::ThreadPool, [WeakSelf, WrapperPtr, Id, DiskPath, Edge]()
	{
		TSharedPtr<FDecoded> Decoded;

		TArray<uint8> FileBytes;
		if (IFileManager::Get().FileExists(*DiskPath) && FFileHelper::LoadFileToArray(FileBytes, *DiskPath))
		{
			Decoded = MakeShared<FDecoded>();
			if (!DecodeAndShrink(*WrapperPtr, FileBytes, Edge, Decoded->Width, Decoded->Height, Decoded->BGRA, nullptr))
			{
				// 缓存文件坏了（半截、被别的程序动过）：删掉，走网络重拉。
				IFileManager::Get().Delete(*DiskPath, false, true, true);
				Decoded.Reset();
			}
		}

		AsyncTask(ENamedThreads::GameThread, [WeakSelf, Id, Decoded]()
		{
			TSharedPtr<FDreamTextureThumbCache> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}

			if (Decoded.IsValid())
			{
				Self->HandleDecoded(Id, Decoded, FString());
			}
			else
			{
				Self->BeginNetworkFetch(Id);
			}
		});
	});
}

void FDreamTextureThumbCache::BeginNetworkFetch(const FString& Id)
{
	const FString DiskPath = GetDiskPath(Id);
	TWeakPtr<FDreamTextureThumbCache> WeakSelf = AsShared();
	IImageWrapperModule* WrapperPtr = Wrapper;
	const int32 Edge = ThumbEdge;

	// FetchFile 的回调保证在游戏线程上跑。
	Fetch(Id, [WeakSelf, WrapperPtr, Id, DiskPath, Edge](bool bOK, TArray<uint8>&& Bytes, const FString& Error)
	{
		TSharedPtr<FDreamTextureThumbCache> Self = WeakSelf.Pin();
		if (!Self.IsValid())
		{
			return;
		}

		if (!bOK)
		{
			Self->MarkFailed(Id, Error);
			return;
		}

		// 第二步在后台：解码、缩小、把缩小后的 PNG 落盘，再回游戏线程建画刷。
		Async(EAsyncExecution::ThreadPool, [WeakSelf, WrapperPtr, Id, DiskPath, Edge, Bytes = MoveTemp(Bytes)]()
		{
			TSharedPtr<FDecoded> Decoded = MakeShared<FDecoded>();
			TArray64<uint8> Png;
			FString Failure;

			if (!DecodeAndShrink(*WrapperPtr, Bytes, Edge, Decoded->Width, Decoded->Height, Decoded->BGRA, &Png))
			{
				Decoded.Reset();
				Failure = TEXT("The preview image could not be decoded (it is not a valid image).");
			}
			else if (Png.Num() > 0)
			{
				// 先写临时名再改名：写到一半被打断，留下的半截文件不能被下次当成好文件。
				const FString Temp = DiskPath + TEXT(".tmp");
				IFileManager::Get().MakeDirectory(*FPaths::GetPath(DiskPath), true);
				if (FFileHelper::SaveArrayToFile(Png, *Temp))
				{
					IFileManager::Get().Move(*DiskPath, *Temp, true, true);
				}
			}

			AsyncTask(ENamedThreads::GameThread, [WeakSelf, Id, Decoded, Failure]()
			{
				if (TSharedPtr<FDreamTextureThumbCache> Self = WeakSelf.Pin())
				{
					Self->HandleDecoded(Id, Decoded, Failure);
				}
			});
		});
	});
}

void FDreamTextureThumbCache::HandleDecoded(const FString& Id, TSharedPtr<FDecoded> Decoded, const FString& FailureReason)
{
	if (!Decoded.IsValid())
	{
		MarkFailed(Id, FailureReason);
		return;
	}

	InFlight.Remove(Id);

	// 没有 Slate 渲染器（-nullrhi 的自动化测试、或者编辑器正在关闭）建不出画刷。
	// 当作失败记下，不抛错；有渲染器的正常编辑器里不会走到这里。
	if (!FSlateApplication::IsInitialized())
	{
		MarkFailed(Id, TEXT("Slate is not initialized, so a brush cannot be created."));
		return;
	}

	// 资源名带递增序号：同一张被淘汰后重建时，旧资源的释放可能还排在渲染线程上，
	// 重名会让新建的那个撞上将死的旧资源。
	static uint32 Serial = 0;
	TSharedPtr<FSlateDynamicImageBrush> Brush = FSlateDynamicImageBrush::CreateWithImageData(
		FName(*FString::Printf(TEXT("DreamTextureThumb/%s/%u"), *Id, ++Serial)),
		FVector2f(static_cast<float>(Decoded->Width), static_cast<float>(Decoded->Height)),
		Decoded->BGRA);

	if (!Brush.IsValid())
	{
		MarkFailed(Id, TEXT("The renderer rejected this image (there may be no renderer)."));
		return;
	}

	Brushes.Add(Id, Brush);
	Touch(Id);
	Trim();
}
