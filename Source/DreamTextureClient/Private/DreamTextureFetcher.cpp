// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureApi.h"

#include "DreamTextureDownloadStore.h"
#include "DreamTextureHttpUtil.h"
#include "DreamTextureLog.h"
#include "DreamTextureSettings.h"
#include "DreamTextureSha256.h"

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

/*
 * 线程模型
 *
 * 状态（FFetchState）只在游戏线程上读写：HTTP 完成委托在游戏线程（见 DreamTextureHttpUtil.h），
 * 校验与落盘（几 MB 到几十 MB 的哈希和写文件）丢到线程池，做完再 AsyncTask 回游戏线程汇报。
 * 工作线程拿到的全是值拷贝和应答对象，不碰 FFetchState，所以状态不用加锁。
 */

namespace
{
	/**
	 * 单个贴图文件的下载超时（秒）。
	 *
	 * 后端在本机，1K 的 PNG 是毫秒级，4K 也就几十 MB。给 120 秒是留给磁盘被占满、
	 * 或者用户把后端地址指到了局域网另一台机器的情况；再长就该告诉用户出问题了。
	 */
	constexpr float FileTimeoutSecs = 120.0f;

	/** 一次 DownloadMaps 调用的全部可变状态。各个请求的回调共享它。 */
	struct FFetchState
	{
		FDreamTextureFetchResult Result;

		/**
		 * 真正失败了的通道（按计划顺序）。和 Result.Skipped 的区别：
		 * Skipped 里还包括「manifest 本来就没这个通道」这种正常降级，那不算失败、不影响 bOK。
		 */
		TArray<FString> Failed;

		FDreamTextureFetcher::FOnFetched Done;
		FDreamTextureFetcher::FOnProgress Progress;

		/** 要走网络下载的文件数（UpToDate 的和被跳过的不算）。 */
		int32 FilesTotal = 0;

		/** 已经有结论（成功或失败）的下载数。 */
		int32 FilesDone = 0;

		/**
		 * Done 已经回调过了。
		 *
		 * 「全部结束后回调一次」这条承诺靠它兜底：就算以后有人改出了第二条收尾路径，
		 * 调用方也不会收到两次——两次回调会让导入队列把同一批资产建两遍。
		 */
		bool bSettled = false;
	};

	/** 收尾：算 bOK、拼 Error，回调 Done。只会生效一次。 */
	void FinishFetch(const TSharedRef<FFetchState>& State)
	{
		if (State->bSettled)
		{
			return;
		}
		State->bSettled = true;

		FDreamTextureFetchResult& Result = State->Result;

		// 提前就带着 Error 的（ID 非法之类）原样保留，不覆盖。
		if (Result.Error.IsEmpty())
		{
			if (State->Failed.Num() > 0)
			{
				TArray<FString> Parts;
				for (const FString& Channel : State->Failed)
				{
					Parts.Add(FText::Format(NSLOCTEXT("DreamTextureFetcher", "ChannelFailure", "{0} ({1})"), FText::FromString(Channel), FText::FromString(Result.Skipped.FindRef(Channel))).ToString());
				}
				Result.Error = FText::Format(NSLOCTEXT("DreamTextureFetcher", "FilesFailed", "Could not fetch {0} {0}|plural(one=file,other=files): {1}"), State->Failed.Num(), FText::FromString(FString::Join(Parts, *NSLOCTEXT("DreamTextureFetcher", "ListSeparator", "; ").ToString()))).ToString();
			}
			else if (Result.LocalFiles.IsEmpty() && Result.UpToDate.IsEmpty())
			{
				// 一个失败都没有，却什么都没有：要的通道 manifest 里全没有。
				// 没有东西可导入，不能报成功，否则导入端会对着空列表建出一个空套装。
				Result.Error = NSLOCTEXT("DreamTextureFetcher", "NoChannels", "This material set has no channels to download.").ToString();
			}
		}
		Result.bOK = Result.Error.IsEmpty();

		FDreamTextureFetcher::FOnFetched Done = MoveTemp(State->Done);
		State->Progress = nullptr;
		if (Done)
		{
			Done(MoveTemp(State->Result));
		}
	}

	/** 一个文件有结论了（在游戏线程）。 */
	void CompleteFile(const TSharedRef<FFetchState>& State, const FString& Channel, bool bOK, const FString& PathOrError)
	{
		if (State->bSettled)
		{
			return;
		}

		if (bOK)
		{
			State->Result.LocalFiles.Add(Channel, PathOrError);
		}
		else
		{
			State->Result.Skipped.Add(Channel, PathOrError);
			State->Failed.AddUnique(Channel);
			UE_LOG(LogDreamTexture, Warning, TEXT("Material %s: channel %s was not fetched: %s"), *FPaths::GetCleanFilename(State->Result.LocalDir), *Channel, *PathOrError);
		}

		++State->FilesDone;
		if (State->Progress)
		{
			State->Progress(State->FilesDone, State->FilesTotal);
		}

		if (State->FilesDone >= State->FilesTotal)
		{
			FinishFetch(State);
		}
	}

	/** 计划里的一项：要下哪个通道、manifest 对它的描述。 */
	struct FPlannedFile
	{
		FString Channel;
		FDreamTextureMapInfo Info;
		FString FinalPath;
	};
}

bool FDreamTextureFetcher::IsSafePathSegment(const FString& Segment)
{
	// "." 和 ".." 本身的字符都在白名单里，却正好是「当前目录」和「上一级」，必须单独挡；
	// 文件名里夹着的 ".."（如 "a..b"）本身无害，但后端的 safeSegment 也一并拒绝，这里对齐，
	// 省得出现「后端不让、本地却放行」的灰色地带。
	if (Segment.IsEmpty() || Segment == TEXT(".") || Segment == TEXT("..") || Segment.Contains(TEXT("..")))
	{
		return false;
	}

	for (const TCHAR Char : Segment)
	{
		const bool bAllowed = (Char >= 'A' && Char <= 'Z') || (Char >= 'a' && Char <= 'z')
			|| (Char >= '0' && Char <= '9') || Char == '_' || Char == '.' || Char == '-';
		if (!bAllowed)
		{
			return false;
		}
	}
	return true;
}

TArray<FString> FDreamTextureFetcher::DefaultChannels()
{
	// ORM 已经覆盖了 roughness / metallic / ao 三张，母材质也只采样 ORM。
	// 分通道图默认不拉，省下三个文件的下载和导入。
	TArray<FString> Channels = {
		DreamTextureChannel::BaseColor,
		DreamTextureChannel::Normal,
		DreamTextureChannel::ORM,
		DreamTextureChannel::Height,
	};

	if (UDreamTextureSettings::Get().bImportSeparateChannels)
	{
		Channels.Add(DreamTextureChannel::Roughness);
		Channels.Add(DreamTextureChannel::Metallic);
		Channels.Add(DreamTextureChannel::AO);
	}
	return Channels;
}

FString FDreamTextureFetcher::GetDownloadDir(const FString& MaterialId)
{
	// 不干净的 ID 不给路径：返回空串，调用方拿去拼路径时只会得到「当前目录下的文件」而不是穿越出去。
	if (!IsSafePathSegment(MaterialId))
	{
		return FString();
	}
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamTexture") / TEXT("Downloads") / MaterialId);
}

FString FDreamTextureFetcher::HashBytes16(TConstArrayView<uint8> Bytes)
{
	uint8 Digest[32];
	DreamTexture::Sha256(Bytes, Digest);

	// 前 16 个十六进制字符 = 摘要的前 8 个字节，小写。
	FString Hex;
	Hex.Reserve(16);
	for (int32 Index = 0; Index < 8; ++Index)
	{
		Hex.Appendf(TEXT("%02x"), Digest[Index]);
	}
	return Hex;
}

bool DreamTexture::StoreVerifiedFile(TConstArrayView<uint8> Bytes, int64 ExpectedBytes, const FString& ExpectedSha256, const FString& FinalPath, FString& OutError)
{
	// 先比大小再哈希：大小不对时不用白算一遍，报错也更具体——
	// 「少了多少字节」几乎总是意味着下载被截断。
	if (ExpectedBytes > 0 && static_cast<int64>(Bytes.Num()) != ExpectedBytes)
	{
		OutError = FText::Format(NSLOCTEXT("DreamTextureFetcher", "SizeMismatch", "Size mismatch: the manifest says {0} bytes, but {1} bytes were received."), FText::AsNumber(ExpectedBytes), FText::AsNumber(Bytes.Num())).ToString();
		return false;
	}

	const FString Actual = FDreamTextureFetcher::HashBytes16(Bytes);
	if (!Actual.Equals(ExpectedSha256, ESearchCase::IgnoreCase))
	{
		OutError = FText::Format(NSLOCTEXT("DreamTextureFetcher", "HashMismatch", "The sha256 check failed: the manifest says {0}, but the received content hashes to {1}."), FText::FromString(ExpectedSha256), FText::FromString(Actual)).ToString();
		return false;
	}

	// 先写到临时名，写完整了再改名。直接写正式名的话，写到一半进程被杀（或磁盘满了）
	// 就会留下一个半截的 PNG，而它的文件名看起来跟好文件一模一样。
	const FString TempPath = FString::Printf(TEXT("%s.%s.part"), *FinalPath, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	IFileManager& FileManager = IFileManager::Get();

	if (!FFileHelper::SaveArrayToFile(TArrayView64<const uint8>(Bytes.GetData(), Bytes.Num()), *TempPath))
	{
		FileManager.Delete(*TempPath, false, true, true);
		OutError = FText::Format(NSLOCTEXT("DreamTextureFetcher", "TempWriteFailed", "Could not write the temporary file (the disk may be full, or there is no write permission): {0}"), FText::FromString(TempPath)).ToString();
		return false;
	}

	// Replace=true：重新下载同一套材质时正式名已经存在，要能盖掉旧的。
	if (!FileManager.Move(*FinalPath, *TempPath, /*Replace*/ true, /*EvenIfReadOnly*/ true, /*Attributes*/ false, /*bDoNotRetryOrError*/ true))
	{
		FileManager.Delete(*TempPath, false, true, true);
		OutError = FText::Format(NSLOCTEXT("DreamTextureFetcher", "RenameFailed", "Could not rename the temporary file into place (the target file may be in use by another program): {0}"), FText::FromString(FinalPath)).ToString();
		return false;
	}
	return true;
}

void FDreamTextureFetcher::DownloadMaps(const FDreamTextureManifest& Manifest, const FDreamTextureFetchOptions& Options, FOnFetched Done, FOnProgress Progress)
{
	const TSharedRef<FFetchState> State = MakeShared<FFetchState>();
	State->Done = MoveTemp(Done);
	State->Progress = MoveTemp(Progress);

	/*
	 * 「回调永远异步」：下面所有不需要走网络的收尾（ID 非法、全部 UpToDate、没有可下的通道），
	 * 都推迟一帧再回调。调用方于是可以放心地在 DownloadMaps() 返回之后才去登记状态，
	 * 不用担心 Done 比自己先跑。
	 */

	// ID 来自后端返回的数据，要拼进本地路径。不能默认它是干净的。
	if (!IsSafePathSegment(Manifest.Id))
	{
		State->Result.Error = FText::Format(NSLOCTEXT("DreamTextureFetcher", "UnsafeMaterialId", "The material id '{0}' contains characters that are not allowed, so the download was refused (only letters, digits, underscores, hyphens and dots are allowed)."), FText::FromString(Manifest.Id)).ToString();
		DreamTexture::RunNextTick([State]() { FinishFetch(State); });
		return;
	}

	const FString Dir = GetDownloadDir(Manifest.Id);
	State->Result.LocalDir = Dir;

	// ---- 计划：逐个通道判断是跳过、已是最新、还是要下 ----
	TArray<FPlannedFile> Planned;
	TSet<FString> Seen;
	const TArray<FString> Channels = Options.Channels.IsEmpty() ? DefaultChannels() : Options.Channels;

	for (const FString& Channel : Channels)
	{
		if (Seen.Contains(Channel))
		{
			continue;
		}
		Seen.Add(Channel);

		const FDreamTextureMapInfo* Info = Manifest.FindMap(Channel);
		if (Info == nullptr)
		{
			// 缺通道是合法的（manifest 兼容性约定），降级而不是报错：记一笔，但不算失败。
			State->Result.Skipped.Add(Channel, FText::Format(NSLOCTEXT("DreamTextureFetcher", "MapMissing", "the manifest has no {0} channel"), FText::FromString(Channel)).ToString());
			continue;
		}

		// 文件名同样来自后端数据，同样要挡路径穿越。这是失败，不是正常降级：
		// 一份带着 "../" 的 manifest 不该被默默吞掉。
		if (!IsSafePathSegment(Info->File))
		{
			State->Result.Skipped.Add(Channel, FText::Format(NSLOCTEXT("DreamTextureFetcher", "UnsafeFileName", "the file name '{0}' contains characters that are not allowed, so the download was refused"), FText::FromString(Info->File)).ToString());
			State->Failed.Add(Channel);
			continue;
		}

		// 没有哈希就没法验证完整性。宁可不拿，也不收一个无从核对的文件——
		// 校验这道门要是对缺字段的 manifest 自动放行，等于没有。
		if (Info->Sha256.IsEmpty())
		{
			State->Result.Skipped.Add(Channel, NSLOCTEXT("DreamTextureFetcher", "MissingSha256", "the manifest gives no sha256, so the file cannot be checked for completeness").ToString());
			State->Failed.Add(Channel);
			continue;
		}

		const FString* Known = Options.KnownHashes.Find(Channel);
		if (Known != nullptr && Known->Equals(Info->Sha256, ESearchCase::IgnoreCase))
		{
			State->Result.UpToDate.Add(Channel);
			continue;
		}

		FPlannedFile File;
		File.Channel = Channel;
		File.Info = *Info;
		File.FinalPath = FPaths::Combine(Dir, Info->File);
		Planned.Add(MoveTemp(File));
	}

	State->FilesTotal = Planned.Num();
	if (Planned.IsEmpty())
	{
		DreamTexture::RunNextTick([State]() { FinishFetch(State); });
		return;
	}

	if (!IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true))
	{
		// 一个文件都落不了地。逐个报失败，让每个通道的 Skipped 里都有原因。
		for (const FPlannedFile& File : Planned)
		{
			const FString Channel = File.Channel;
			DreamTexture::RunNextTick([State, Channel, Dir]()
			{
				CompleteFile(State, Channel, false, FText::Format(NSLOCTEXT("DreamTextureFetcher", "CannotCreateDir", "Could not create the download folder: {0}"), FText::FromString(Dir)).ToString());
			});
		}
		return;
	}

	// ---- 发请求。所有文件并发，数量最多 7 个，对本机后端不是负担 ----
	for (const FPlannedFile& File : Planned)
	{
		const FString URL = FDreamTextureApi::MakeFileURL(Manifest.Id, File.Info.File);

		DreamTexture::SendRequest(TEXT("GET"), URL, FileTimeoutSecs, FString(),
			[State, File](const DreamTexture::FHttpOutcome& Outcome)
			{
				if (!Outcome.bOK)
				{
					CompleteFile(State, File.Channel, false, FText::Format(NSLOCTEXT("DreamTextureFetcher", "DownloadFailed", "Download failed: {0}"), FText::FromString(Outcome.Error)).ToString());
					return;
				}

				// 哈希和写盘丢给线程池，别在游戏线程上卡一帧。
				// State 用 TSharedPtr 带进去再整个移进回程的任务里：这样工作线程这头不会留着一份引用，
				// FFetchState 的最后一次释放一定发生在游戏线程。
				TSharedPtr<FFetchState> StatePtr = State;
				const FHttpResponsePtr Response = Outcome.Response;
				Async(EAsyncExecution::ThreadPool, [StatePtr = MoveTemp(StatePtr), File, Response]() mutable
				{
					FString Error;
					const bool bStored = DreamTexture::StoreVerifiedFile(
						Response->GetContent(), File.Info.Bytes, File.Info.Sha256, File.FinalPath, Error);

					AsyncTask(ENamedThreads::GameThread,
						[StatePtr = MoveTemp(StatePtr), Channel = File.Channel, FinalPath = File.FinalPath, bStored, Error]()
						{
							CompleteFile(StatePtr.ToSharedRef(), Channel, bStored, bStored ? FinalPath : Error);
						});
				});
			});
	}
}
