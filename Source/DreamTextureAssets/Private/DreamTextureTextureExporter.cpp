// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureTextureExporter.h"

#include "DreamTextureApi.h"
#include "DreamTextureLog.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace
{
	const TCHAR* FormatName(ETextureSourceFormat Format)
	{
		switch (Format)
		{
		case TSF_G8: return TEXT("G8");
		case TSF_G16: return TEXT("G16");
		case TSF_BGRA8: return TEXT("BGRA8");
		case TSF_RGBA16: return TEXT("RGBA16");
		case TSF_RGBA16F: return TEXT("RGBA16F");
		case TSF_RGBA32F: return TEXT("RGBA32F");
		case TSF_R16F: return TEXT("R16F");
		case TSF_R32F: return TEXT("R32F");
		case TSF_BGRE8: return TEXT("BGRE8");
		default: return TEXT("unknown");
		}
	}

	bool IsSupportedFormat(ETextureSourceFormat Format)
	{
		return Format == TSF_G8 || Format == TSF_G16 || Format == TSF_BGRA8 || Format == TSF_RGBA16;
	}

	/**
	 * 把 G 通道翻过来，与引擎构建派生数据时的 FlipGreenChannel 一致（TextureCompressorModule.cpp：G = 1 - clamp(G)）。
	 * 在贴图自己的编码里直接取反：8 位 255 - G，16 位 65535 - G，不经过 sRGB↔线性（法线本来就是线性）。
	 * 只处理带绿通道的两种彩色格式；调用方已经确认过格式。
	 */
	void FlipGreen(FImage& Image)
	{
		const int64 NumPixels = static_cast<int64>(Image.SizeX) * Image.SizeY * Image.NumSlices;
		if (Image.Format == ERawImageFormat::BGRA8)
		{
			// 字节顺序 B G R A，绿在第 1 个。
			uint8* Data = Image.RawData.GetData();
			for (int64 Index = 0; Index < NumPixels; ++Index)
			{
				Data[Index * 4 + 1] = static_cast<uint8>(255 - Data[Index * 4 + 1]);
			}
		}
		else if (Image.Format == ERawImageFormat::RGBA16)
		{
			// 通道顺序 R G B A，每个通道 16 位，绿在第 1 个。
			uint16* Data = reinterpret_cast<uint16*>(Image.RawData.GetData());
			for (int64 Index = 0; Index < NumPixels; ++Index)
			{
				Data[Index * 4 + 1] = static_cast<uint16>(65535 - Data[Index * 4 + 1]);
			}
		}
	}

	/** Windows 保留设备名（不看扩展名、不分大小写）：CON PRN AUX NUL COM1-9 LPT1-9。 */
	bool IsReservedDeviceName(const FString& Stem)
	{
		// Windows 对「CON 」「CON.」这种带尾随空格或点的写法也当成设备名，比较前先去掉。
		FString Name = Stem;
		Name.TrimEndInline();
		while (Name.EndsWith(TEXT(".")))
		{
			Name.LeftChopInline(1);
			Name.TrimEndInline();
		}

		static const TCHAR* const Fixed[] = { TEXT("CON"), TEXT("PRN"), TEXT("AUX"), TEXT("NUL") };
		for (const TCHAR* Reserved : Fixed)
		{
			if (Name.Equals(Reserved, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		if (Name.Len() == 4 && (Name.StartsWith(TEXT("COM"), ESearchCase::IgnoreCase) || Name.StartsWith(TEXT("LPT"), ESearchCase::IgnoreCase)))
		{
			// COM0 / LPT0 不是保留名；只有 1~9。
			return Name[3] >= TEXT('1') && Name[3] <= TEXT('9');
		}
		return false;
	}

	/** 反复说同一句：不退化。写进每条「导不出」的原因里，让界面上看到原因的人明白这不是偷懒。 */
	FText NoFallbackNote()
	{
		return NSLOCTEXT("DreamTextureTextureExporter", "NoFallbackNote", "To keep block-compression artifacts out of the decomposition that follows, the decompressed mip 0 is not used instead.");
	}
}

bool FDreamTextureTextureExporter::CanExport(const UTexture* Texture, FString& OutReason)
{
	if (Texture == nullptr)
	{
		OutReason = NSLOCTEXT("DreamTextureTextureExporter", "NoTexture", "The texture does not exist.").ToString();
		return false;
	}

	const UTexture2D* Texture2D = Cast<UTexture2D>(Texture);
	if (Texture2D == nullptr)
	{
		OutReason = FText::Format(NSLOCTEXT("DreamTextureTextureExporter", "NotTexture2D", "This is not a regular 2D texture ({0}), so it cannot be exported."), FText::FromString(Texture->GetClass()->GetName())).ToString();
		return false;
	}

	const FTextureSource& Source = Texture->Source;
	if (!Source.IsValid())
	{
		if (Texture->VirtualTextureStreaming)
		{
			OutReason = FText::Format(NSLOCTEXT("DreamTextureTextureExporter", "VirtualNoSource", "This is a virtual texture and the asset has no source data (only baked VT data is left). {0}"), NoFallbackNote()).ToString();
		}
		else
		{
			OutReason = FText::Format(NSLOCTEXT("DreamTextureTextureExporter", "NoSource", "The asset has no source data (a procedural texture, or the source data was stripped after import). {0}"), NoFallbackNote()).ToString();
		}
		return false;
	}

	if (Source.GetNumBlocks() > 1)
	{
		OutReason = FText::Format(NSLOCTEXT("DreamTextureTextureExporter", "Udim", "This is a UDIM texture ({0} tiles), so it cannot be exported."), Source.GetNumBlocks()).ToString();
		return false;
	}
	if (Source.GetNumLayers() > 1)
	{
		OutReason = FText::Format(NSLOCTEXT("DreamTextureTextureExporter", "MultiLayer", "This is a multi-layer virtual texture ({0} layers), so it cannot be exported."), Source.GetNumLayers()).ToString();
		return false;
	}
	if (Source.GetNumSlices() > 1)
	{
		OutReason = NSLOCTEXT("DreamTextureTextureExporter", "MultiSlice", "The source data has multiple slices (an array or volume), so it cannot be exported.").ToString();
		return false;
	}

	const ETextureSourceFormat Format = Source.GetFormat();
	if (!IsSupportedFormat(Format))
	{
		OutReason = FText::Format(NSLOCTEXT("DreamTextureTextureExporter", "FloatFormat", "The source data is {0} (a floating-point or HDR format) that PNG cannot hold, and no lossy conversion is done."), FText::FromString(FormatName(Format))).ToString();
		return false;
	}
	return true;
}

FDreamTextureExportedImage FDreamTextureTextureExporter::Export(UTexture* Texture, const FString& FilePath, int32 MaxEdge)
{
	check(IsInGameThread());

	FDreamTextureExportedImage Out;
	Out.TexturePath = Texture != nullptr ? Texture->GetPathName() : FString();

	FString Reason;
	if (!CanExport(Texture, Reason))
	{
		Out.Reason = Reason;
		return Out;
	}

	FTextureSource& Source = Texture->Source;
	Out.SourceWidth = static_cast<int32>(Source.GetSizeX());
	Out.SourceHeight = static_cast<int32>(Source.GetSizeY());
	Out.SourceFormat = FormatName(Source.GetFormat());
	Out.bSRGB = Texture->SRGB;

	FImage Image;
	if (!Source.GetMipImage(Image, /*Block*/ 0, /*Layer*/ 0, /*Mip*/ 0))
	{
		Out.Reason = FText::Format(NSLOCTEXT("DreamTextureTextureExporter", "ReadSourceFailed", "Could not read the source data. {0}"), NoFallbackNote()).ToString();
		return Out;
	}
	if (Image.SizeX <= 0 || Image.SizeY <= 0 || Image.RawData.Num() == 0)
	{
		Out.Reason = FText::Format(NSLOCTEXT("DreamTextureTextureExporter", "EmptySource", "The source data is empty. {0}"), NoFallbackNote()).ToString();
		return Out;
	}

	switch (Image.Format)
	{
	case ERawImageFormat::G8:
		Out.BitsPerChannel = 8;
		Out.NumChannels = 1;
		break;
	case ERawImageFormat::G16:
		Out.BitsPerChannel = 16;
		Out.NumChannels = 1;
		break;
	case ERawImageFormat::BGRA8:
		Out.BitsPerChannel = 8;
		Out.NumChannels = 4;
		break;
	case ERawImageFormat::RGBA16:
		Out.BitsPerChannel = 16;
		Out.NumChannels = 4;
		break;
	default:
		Out.Reason = NSLOCTEXT("DreamTextureTextureExporter", "UnsupportedPixelFormat", "The pixel format of the source data is not supported.").ToString();
		return Out;
	}

	// ---- Flip Green Channel ----
	// 引擎只在构建派生数据时应用它，Source 里还是翻之前的像素；这里翻读出来的副本，让导出的图与 UE 里实际用的一致。
	// 灰度格式没有绿通道，引擎翻了也不影响单通道的结果，不翻。先翻再缩放。
	if (Texture->bFlipGreenChannel && Out.NumChannels == 4)
	{
		FlipGreen(Image);
		Out.bFlippedGreen = true;
	}

	// ---- 缩放（可选）----
	// 只缩小，不放大：放大不会多出信息，只会让后端以为拿到了高分辨率输入。
	FImage Resized;
	const FImage* ToWrite = &Image;
	if (MaxEdge > 0 && FMath::Max(Image.SizeX, Image.SizeY) > MaxEdge)
	{
		const double Scale = static_cast<double>(MaxEdge) / static_cast<double>(FMath::Max(Image.SizeX, Image.SizeY));
		const int32 NewX = FMath::Max(1, FMath::RoundToInt(Image.SizeX * Scale));
		const int32 NewY = FMath::Max(1, FMath::RoundToInt(Image.SizeY * Scale));

		// 不能用默认滤波：Default 在缩小时是 Mitchell，有负瓣，会在硬边（金属度的 0/1 交界）旁边振铃，
		// 存成 8 位后过冲被截断，边缘就多出一圈不存在的值。Triangle 没有负瓣。
		FImageCore::ResizeImageAllocDest(Image, Resized, NewX, NewY, Image.Format, Image.GammaSpace, FImageCore::EResizeImageFilter::Triangle);
		if (Resized.RawData.Num() == 0)
		{
			Out.Reason = NSLOCTEXT("DreamTextureTextureExporter", "ResizeFailed", "Resizing failed.").ToString();
			return Out;
		}
		ToWrite = &Resized;
		Out.bResized = true;
	}
	Out.Width = ToWrite->SizeX;
	Out.Height = ToWrite->SizeY;

	// ---- 编码 ----
	// CompressImage 对 PNG 不做任何 sRGB↔线性转换：8 位面按字节原样写（见 ImageWrapperModule 里那段注释）。
	const FImageView View(const_cast<uint8*>(ToWrite->RawData.GetData()), ToWrite->SizeX, ToWrite->SizeY, ToWrite->NumSlices, ToWrite->Format, ToWrite->GammaSpace);
	TArray64<uint8> Png;
	if (!FImageUtils::CompressImage(Png, TEXT("png"), View) || Png.Num() == 0)
	{
		Out.Reason = NSLOCTEXT("DreamTextureTextureExporter", "EncodeFailed", "PNG encoding failed.").ToString();
		return Out;
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), /*Tree*/ true);
	if (!FFileHelper::SaveArrayToFile(Png, *FilePath))
	{
		Out.Reason = FText::Format(NSLOCTEXT("DreamTextureTextureExporter", "WriteFailed", "Could not write the file: {0}"), FText::FromString(FilePath)).ToString();
		return Out;
	}

	Out.FilePath = FPaths::ConvertRelativePathToFull(FilePath);
	Out.Sha256 = FDreamTextureFetcher::HashBytes16(TConstArrayView<uint8>(Png.GetData(), static_cast<int32>(Png.Num())));
	Out.bExported = true;
	return Out;
}

FString FDreamTextureTextureExporter::MakeSafeFileStem(const FString& AssetName)
{
	FString Stem = FPaths::MakeValidFileName(AssetName.IsEmpty() ? FString(TEXT("Texture")) : AssetName);
	if (Stem.IsEmpty())
	{
		Stem = TEXT("Texture");
	}
	if (IsReservedDeviceName(Stem))
	{
		Stem = TEXT("Texture_") + Stem;
	}
	return Stem;
}

FString FDreamTextureTextureExporter::MakeTempDir()
{
	const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"));
	const FString Random = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(6);
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamTexture") / TEXT("Reflow") / FString::Printf(TEXT("%s-%s"), *Stamp, *Random));
	IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
	return Dir;
}

TArray<FDreamTextureExportedImage> FDreamTextureTextureExporter::ExportAnalysis(const FDreamTextureMaterialAnalysis& Analysis, const FDreamTextureExportOptions& Options)
{
	TArray<FDreamTextureExportedImage> Out;
	Out.Reserve(Analysis.Textures.Num());

	const FString Dir = Options.OutputDir.IsEmpty() ? MakeTempDir() : Options.OutputDir;
	TSet<FString> UsedNames;

	for (const FDreamTextureMaterialTexture& Row : Analysis.Textures)
	{
		UTexture* Texture = Row.Texture.Get();
		if (Texture == nullptr)
		{
			FDreamTextureExportedImage Missing;
			Missing.TexturePath = Row.TexturePath;
			Missing.Reason = NSLOCTEXT("DreamTextureTextureExporter", "TextureGone", "The texture is no longer in memory (it was unloaded or deleted); analyze again.").ToString();
			Out.Add(MoveTemp(Missing));
			continue;
		}

		// 文件名取资产名；不同目录下的同名贴图加序号，别互相覆盖。
		const FString BaseName = MakeSafeFileStem(Row.AssetName);
		FString Name = BaseName;
		for (int32 Suffix = 2; UsedNames.Contains(Name.ToLower()); ++Suffix)
		{
			Name = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix);
		}
		UsedNames.Add(Name.ToLower());

		Out.Add(Export(Texture, Dir / (Name + TEXT(".png")), Options.MaxEdge));
	}
	return Out;
}
