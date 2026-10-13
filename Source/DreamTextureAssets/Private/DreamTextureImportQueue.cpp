// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureImportQueue.h"

#include "Containers/Ticker.h"
#include "DreamTextureLog.h"
#include "DreamTextureMaterialBuilder.h"
#include "DreamTextureSettings.h"
#include "Misc/Paths.h"

/*
 * 流程（每个 id 一轮）：
 *
 *   GetManifest → FindImportedHashes → DownloadMaps(KnownHashes) → [下一帧] Import
 *
 * 线程：所有状态只在游戏线程读写。Api / Fetcher 的回调按契约都在游戏线程上跑，
 * Import 也必须在游戏线程，所以这里没有锁。
 *
 * 回调可能同步触发（比如参数非法时 Api 直接回调失败）。因此每个 Begin 之后
 * 不能再假设「这一轮还在跑」——所有状态改动都放在回调自己里面做。
 */

namespace
{
	/**
	 * 材质实例那一步的一句话说明；不必说的（设置里关了、没有任何要说的）返回空串。
	 *
	 * 材质这一步失败不影响贴图的导入结果，但整次导入要看得出来——
	 * 否则用户只看到「导入完成」，打开一看没有材质实例，不知道该怪谁。
	 */
	FString DescribeMaterialInstance(const FDreamTextureImportedTexture& MI, bool bEnabled)
	{
		if (!bEnabled)
		{
			return FString();
		}

		const FString Name = FPaths::GetBaseFilename(MI.ObjectPath);
		switch (MI.Action)
		{
		case EDreamTextureImportAction::Created: return FText::Format(NSLOCTEXT("DreamTextureImportQueue", "InstanceCreated", "Material instance {0} created"), FText::FromString(Name)).ToString();
		case EDreamTextureImportAction::Updated: return FText::Format(NSLOCTEXT("DreamTextureImportQueue", "InstanceUpdated", "Material instance {0} updated"), FText::FromString(Name)).ToString();
		case EDreamTextureImportAction::Unchanged: return FText::Format(NSLOCTEXT("DreamTextureImportQueue", "InstanceUnchanged", "Material instance {0} unchanged"), FText::FromString(Name)).ToString();
		case EDreamTextureImportAction::Failed: return FText::Format(NSLOCTEXT("DreamTextureImportQueue", "InstanceFailed", "Material instance creation failed: {0}"), FText::FromString(MI.Note)).ToString();
		default: return MI.Note.IsEmpty() ? FString() : FText::Format(NSLOCTEXT("DreamTextureImportQueue", "InstanceNotCreated", "No material instance was created: {0}"), FText::FromString(MI.Note)).ToString();
		}
	}

	/**
	 * 结果摘要。成功时给界面显示的 Message。
	 *
	 * 「全部 Unchanged」单独说一句：用户再点一次导入，看到的是「已经是最新的」，
	 * 而不是一串全零的计数，知道是没事而不是没动。材质实例也得没变才算——
	 * 贴图都没变、但材质实例这次才新建（比如升级插件后第一次导入旧素材），就不能说「已经是最新的」。
	 */
	FString Summarize(const FDreamTextureImportResult& Result, bool bMaterialInstanceEnabled)
	{
		int32 Created = 0;
		int32 Updated = 0;
		int32 Unchanged = 0;
		int32 Skipped = 0;
		for (const FDreamTextureImportedTexture& Texture : Result.Textures)
		{
			switch (Texture.Action)
			{
			case EDreamTextureImportAction::Created: ++Created; break;
			case EDreamTextureImportAction::Updated: ++Updated; break;
			case EDreamTextureImportAction::Unchanged: ++Unchanged; break;
			case EDreamTextureImportAction::Skipped: ++Skipped; break;
			default: break;
			}
		}

		const EDreamTextureImportAction MIAction = Result.MaterialInstance.Action;
		const bool bMaterialInstanceSettled = !bMaterialInstanceEnabled || MIAction == EDreamTextureImportAction::Unchanged || MIAction == EDreamTextureImportAction::Skipped;
		const FString MaterialPart = DescribeMaterialInstance(Result.MaterialInstance, bMaterialInstanceEnabled);

		if (Created + Updated == 0 && Unchanged > 0 && bMaterialInstanceSettled)
		{
			FString Message = FText::Format(NSLOCTEXT("DreamTextureImportQueue", "UpToDate", "This material set is already up to date; there is nothing to import (all {0} {0}|plural(one=texture,other=textures) unchanged)."), Unchanged).ToString();
			if (MIAction == EDreamTextureImportAction::Skipped && !MaterialPart.IsEmpty())
			{
				Message += NSLOCTEXT("DreamTextureImportQueue", "ClauseSeparator", "; ").ToString() + MaterialPart;
			}
			return Message;
		}

		FString Message = FText::Format(NSLOCTEXT("DreamTextureImportQueue", "Finished", "Import finished: {0} created, {1} updated, {2} unchanged"), Created, Updated, Unchanged).ToString();
		if (Skipped > 0)
		{
			Message += FText::Format(NSLOCTEXT("DreamTextureImportQueue", "FinishedSkipped", ", {0} skipped"), Skipped).ToString();
		}
		if (!MaterialPart.IsEmpty())
		{
			Message += NSLOCTEXT("DreamTextureImportQueue", "ClauseSeparator", "; ").ToString() + MaterialPart;
		}
		return Message;
	}
}

FDreamTextureImportQueue& FDreamTextureImportQueue::Get()
{
	static FDreamTextureImportQueue Instance;
	return Instance;
}

bool FDreamTextureImportQueue::Enqueue(const FString& MaterialId)
{
	return Enqueue(MaterialId, FDreamTextureImportOptions());
}

bool FDreamTextureImportQueue::Enqueue(const FString& MaterialId, const FDreamTextureImportOptions& Options)
{
	check(IsInGameThread());

	if (MaterialId.IsEmpty() || IsActive(MaterialId))
	{
		return false;
	}

	const int32 Ahead = Pending.Num() + (Running.IsEmpty() ? 0 : 1);

	FDreamTextureImportStatus& Status = ResetStatus(MaterialId);
	Status.Message = Ahead > 0 ? FText::Format(NSLOCTEXT("DreamTextureImportQueue", "Queued", "Added to the import queue; {0} {0}|plural(one=job,other=jobs) ahead"), Ahead).ToString() : NSLOCTEXT("DreamTextureImportQueue", "Preparing", "Preparing to import...").ToString();
	Pending.Add(MaterialId);
	PendingOptions.Add(MaterialId, Options);

	OnStatusChanged.Broadcast(Status);
	Pump();
	return true;
}

bool FDreamTextureImportQueue::IsActive(const FString& MaterialId) const
{
	return (!Running.IsEmpty() && Running.Equals(MaterialId, ESearchCase::CaseSensitive)) ||
		Pending.ContainsByPredicate([&MaterialId](const FString& Id) { return Id.Equals(MaterialId, ESearchCase::CaseSensitive); });
}

const FDreamTextureImportStatus* FDreamTextureImportQueue::FindStatus(const FString& MaterialId) const
{
	const TUniquePtr<FDreamTextureImportStatus>* Found = Statuses.Find(MaterialId);
	return Found != nullptr ? Found->Get() : nullptr;
}

void FDreamTextureImportQueue::Shutdown()
{
	// 先加代数再清队列：任何已经发出去、之后才回来的回调拿着旧代数，一律被 IsCurrent 挡掉。
	// 不广播事件——这时界面模块多半也在关闭，没人听，听了也只会往一个正在销毁的面板里写东西。
	++Generation;

	auto MarkCancelled = [this](const FString& Id)
	{
		if (const TUniquePtr<FDreamTextureImportStatus>* Found = Statuses.Find(Id))
		{
			FDreamTextureImportStatus& Status = **Found;
			Status.Stage = EDreamTextureImportStage::Failed;
			Status.Message = NSLOCTEXT("DreamTextureImportQueue", "CancelledShutdown", "Cancelled: the plugin is shutting down.").ToString();
			Status.Result.Error = Status.Message;
		}
	};
	for (const FString& Id : Pending)
	{
		MarkCancelled(Id);
	}
	if (!Running.IsEmpty())
	{
		MarkCancelled(Running);
	}

	Pending.Reset();
	PendingOptions.Reset();
	RunningOptions = FDreamTextureImportOptions();
	Running.Reset();
}

void FDreamTextureImportQueue::Pump()
{
	if (!Running.IsEmpty() || Pending.IsEmpty())
	{
		return;
	}

	// 先把 Running 立起来再开跑：Begin 里广播的事件、或同步触发的回调里如果有人再 Enqueue，
	// 看到的是「忙」，只会排队，不会重入开出第二轮。
	Running = Pending[0];
	Pending.RemoveAt(0);

	// 选项跟着 id 走：这一轮用它，跑完（Finish）就丢，下一轮重新取。
	FDreamTextureImportOptions Taken;
	PendingOptions.RemoveAndCopyValue(Running, Taken);
	RunningOptions = MoveTemp(Taken);
	Begin(Running);
}

void FDreamTextureImportQueue::Begin(const FString& MaterialId)
{
	const uint32 Gen = Generation;
	SetStage(MaterialId, EDreamTextureImportStage::FetchingManifest, NSLOCTEXT("DreamTextureImportQueue", "FetchingManifest", "Fetching material info...").ToString());

	// SetStage 的广播里有人调了 Shutdown 的话，这一轮已经作废，别再发请求。
	if (!IsCurrent(MaterialId, Gen))
	{
		return;
	}

	FDreamTextureApi::GetManifest(MaterialId, [this, MaterialId, Gen](bool bOK, FDreamTextureManifest&& Manifest, const FString& Error)
	{
		OnManifest(MaterialId, Gen, bOK, MoveTemp(Manifest), Error);
	});
}

bool FDreamTextureImportQueue::IsCurrent(const FString& MaterialId, uint32 InGeneration) const
{
	return InGeneration == Generation && !Running.IsEmpty() && Running.Equals(MaterialId, ESearchCase::CaseSensitive);
}

void FDreamTextureImportQueue::OnManifest(const FString& MaterialId, uint32 InGeneration, bool bOK, FDreamTextureManifest&& Manifest, const FString& Error)
{
	if (!IsCurrent(MaterialId, InGeneration))
	{
		return;
	}

	if (!bOK)
	{
		Fail(MaterialId, FText::Format(NSLOCTEXT("DreamTextureImportQueue", "ManifestFailed", "Could not get the material info ({0})"), FText::FromString(Error)).ToString());
		return;
	}

	// manifest 里的 id 决定资产路径和来源记录，必须就是我们要的那一套。
	// 后端返回别的 id（路由错了、或被代理改写）时，若照单全收，会把别的材质导进这一套的名下。
	if (Manifest.Id.IsEmpty())
	{
		Manifest.Id = MaterialId;
	}
	else if (!Manifest.Id.Equals(MaterialId, ESearchCase::CaseSensitive))
	{
		Fail(MaterialId, FText::Format(NSLOCTEXT("DreamTextureImportQueue", "IdMismatch", "The material id the backend returned does not match (expected {0}, got {1})."), FText::FromString(MaterialId), FText::FromString(Manifest.Id)).ToString());
		return;
	}

	// 这一轮要的通道在开头定死，下载和导入共用同一份：设置在中途被改了也不会让两边各说各话。
	const TArray<FString> Channels = RunningOptions.bExplicitChannels ? RunningOptions.Channels : FDreamTextureFetcher::DefaultChannels();
	if (Channels.Num() == 0)
	{
		// 显式给了空的通道表：Fetcher 会把空表当成「用默认」，悄悄多下一堆东西。这里直接说清楚。
		Fail(MaterialId, NSLOCTEXT("DreamTextureImportQueue", "NoChannels", "There are no channels to import.").ToString());
		return;
	}
	const TSharedRef<FDreamTextureManifest> Shared = MakeShared<FDreamTextureManifest>(MoveTemp(Manifest));

	FDreamTextureFetchOptions Options;
	Options.Channels = Channels;
	Options.KnownHashes = FDreamTextureImporter::FindImportedHashes(*Shared);

	// 先估一个要下载的文件数，界面一开始就能画「0/N」；真数字以 Fetcher 的进度回调为准。
	int32 Expected = 0;
	for (const FString& Channel : Channels)
	{
		const FDreamTextureMapInfo* Map = Shared->FindMap(Channel);
		const FString* Known = Options.KnownHashes.Find(Channel);
		if (Map != nullptr && !(Known != nullptr && Known->Equals(Map->Sha256, ESearchCase::IgnoreCase)))
		{
			++Expected;
		}
	}

	FDreamTextureImportStatus& Status = ResetStatus(MaterialId);
	Status.Stage = EDreamTextureImportStage::Downloading;
	Status.FilesDone = 0;
	Status.FilesTotal = Expected;
	Status.Message = Expected > 0 ? FText::Format(NSLOCTEXT("DreamTextureImportQueue", "DownloadingStart", "Downloading textures 0/{0}"), Expected).ToString() : NSLOCTEXT("DreamTextureImportQueue", "NothingToDownload", "Textures are already up to date; nothing to download.").ToString();
	OnStatusChanged.Broadcast(Status);

	// 广播里有人 Shutdown 了就别再发请求。
	if (!IsCurrent(MaterialId, InGeneration))
	{
		return;
	}

	FDreamTextureFetcher::DownloadMaps(*Shared, Options,
		[this, MaterialId, InGeneration, Shared, Channels](FDreamTextureFetchResult&& Fetch)
		{
			OnFetched(MaterialId, InGeneration, Shared, Channels, MoveTemp(Fetch));
		},
		[this, MaterialId, InGeneration](int32 FilesDone, int32 FilesTotal)
		{
			OnProgress(MaterialId, InGeneration, FilesDone, FilesTotal);
		});
}

void FDreamTextureImportQueue::OnProgress(const FString& MaterialId, uint32 InGeneration, int32 FilesDone, int32 FilesTotal)
{
	if (!IsCurrent(MaterialId, InGeneration))
	{
		return;
	}

	FDreamTextureImportStatus& Status = ResetStatus(MaterialId);
	Status.Stage = EDreamTextureImportStage::Downloading;
	Status.FilesDone = FilesDone;
	Status.FilesTotal = FilesTotal;
	Status.Message = FText::Format(NSLOCTEXT("DreamTextureImportQueue", "Downloading", "Downloading textures {0}/{1}"), FilesDone, FilesTotal).ToString();
	OnStatusChanged.Broadcast(Status);
}

void FDreamTextureImportQueue::OnFetched(const FString& MaterialId, uint32 InGeneration, TSharedRef<FDreamTextureManifest> Manifest, TArray<FString> Channels, FDreamTextureFetchResult&& Fetch)
{
	if (!IsCurrent(MaterialId, InGeneration))
	{
		return;
	}

	if (!Fetch.bOK)
	{
		Fail(MaterialId, FText::Format(NSLOCTEXT("DreamTextureImportQueue", "DownloadIncomplete", "The download did not complete: {0}"), FText::FromString(Fetch.Error)).ToString());
		return;
	}

	SetStage(MaterialId, EDreamTextureImportStage::Importing, NSLOCTEXT("DreamTextureImportQueue", "Importing", "Importing into the project...").ToString());
	if (!IsCurrent(MaterialId, InGeneration))
	{
		return;
	}

	// 推迟到下一帧再落资产：Import 是同步的，建贴图、存包要占游戏线程一阵子。
	// 先让「正在导入」这一帧画出来，用户看到的是在干活，而不是整个编辑器静止后突然跳到完成。
	const TSharedRef<FDreamTextureFetchResult> SharedFetch = MakeShared<FDreamTextureFetchResult>(MoveTemp(Fetch));
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[this, MaterialId, InGeneration, Manifest, Channels, SharedFetch](float)
		{
			RunImport(MaterialId, InGeneration, Manifest, Channels, *SharedFetch);
			return false;
		}), 0.0f);
}

void FDreamTextureImportQueue::RunImport(const FString& MaterialId, uint32 InGeneration, TSharedRef<FDreamTextureManifest> Manifest, TArray<FString> Channels, FDreamTextureFetchResult Fetch)
{
	if (!IsCurrent(MaterialId, InGeneration))
	{
		return;
	}

	FDreamTextureImportRequest Request;
	Request.Manifest = *Manifest;
	Request.LocalFiles = Fetch.LocalFiles;
	Request.bSave = true;

	// 只把这一轮要的通道交给导入器。manifest 里还有 roughness / metallic / ao 但设置里没打开分通道导入时，
	// 让导入器看见它们只会得到三条「没文件」的 Skipped，界面会据此提示「有几张没导入」——
	// 可那是用户的设置，不是问题。
	for (auto It = Request.Manifest.Maps.CreateIterator(); It; ++It)
	{
		if (!Channels.Contains(It.Key()))
		{
			It.RemoveCurrent();
		}
	}

	FDreamTextureImportResult Result = FDreamTextureImporter::Import(Request);

	// 把下载阶段知道的事并进结果：
	// - Fetcher 判为 UpToDate 的通道没有本地文件，导入器靠工程里记的哈希独立得出 Unchanged；
	//   这里只兜底：导入器没给这个通道记条目时补一条 Unchanged。
	//   如果导入器给的是 Skipped（下载前还在、到导入时却不见了的怪情况），不能谎报成 Unchanged，
	//   换成一句能说明问题的话。
	// - Skipped 的通道，Fetcher 知道的原因（设置里没开、下载失败…）比导入器那句通用的「没文件」具体，用它的。
	for (const FString& Channel : Fetch.UpToDate)
	{
		FDreamTextureImportedTexture* Entry = Result.Textures.FindByPredicate([&Channel](const FDreamTextureImportedTexture& Texture)
		{
			return Texture.Channel.Equals(Channel, ESearchCase::CaseSensitive);
		});
		if (Entry == nullptr)
		{
			FDreamTextureImportedTexture Added;
			Added.Channel = Channel;
			Added.Action = EDreamTextureImportAction::Unchanged;
			const FString Name = FDreamTextureImporter::GetTextureAssetName(FDreamTextureImporter::MakeSlug(*Manifest), Channel);
			if (!Name.IsEmpty())
			{
				Added.ObjectPath = FDreamTextureImporter::GetAssetFolder(*Manifest) / Name + TEXT(".") + Name;
			}
			Result.Textures.Add(MoveTemp(Added));
		}
		else if (Entry->Action == EDreamTextureImportAction::Skipped)
		{
			Entry->Note = NSLOCTEXT("DreamTextureImportQueue", "TextureVanished", "The project still had this texture before the download, but it could not be found at import time. Import again.").ToString();
		}
	}
	for (FDreamTextureImportedTexture& Texture : Result.Textures)
	{
		if (Texture.Action == EDreamTextureImportAction::Skipped)
		{
			if (const FString* Reason = Fetch.Skipped.Find(Texture.Channel))
			{
				if (!Reason->IsEmpty())
				{
					Texture.Note = *Reason;
				}
			}
		}
	}

	// 材质实例（M2）：贴图落完之后，给这一套建（或就地更新）MI_<Slug>。
	// 这一步失败不改变贴图的结果（bOK 照旧），只记在 Result.MaterialInstance 里，并在 Message 里说明。
	// 贴图没有全部成功时不建：缺一半贴图的实例，用户看到的是一块怪材质，比没有更难排查。
	const bool bSettingAllowsInstance = UDreamTextureSettings::Get().bCreateMaterialInstance;
	const bool bCreateMaterialInstance = bSettingAllowsInstance && RunningOptions.bCreateMaterialInstance;
	if (!bCreateMaterialInstance)
	{
		Result.MaterialInstance = FDreamTextureImportedTexture();
		Result.MaterialInstance.Action = EDreamTextureImportAction::Skipped;
		Result.MaterialInstance.Note = bSettingAllowsInstance
			? NSLOCTEXT("DreamTextureImportQueue", "InstanceSkippedByCaller", "No material instance is created this time (the caller handles where the textures go).").ToString()
			: NSLOCTEXT("DreamTextureImportQueue", "InstanceSkippedBySetting", "The 'Create Material Instance' setting is off, so only the textures were imported.").ToString();
	}
	else if (!Result.bOK)
	{
		Result.MaterialInstance = FDreamTextureImportedTexture();
		Result.MaterialInstance.Action = EDreamTextureImportAction::Skipped;
		Result.MaterialInstance.Note = NSLOCTEXT("DreamTextureImportQueue", "InstanceSkippedTexturesFailed", "Not all textures were imported, so no material instance was created.").ToString();
	}
	else
	{
		// 复用工程里已有的贴图（回流：底色沿用用户原来那张）：以「未变」的条目并进给材质实例的那份清单里，
		// 但不并进 Result 本身——它们不是这一次导入的东西，不该算进「新建 N 张、未变 N 张」的摘要。
		FDreamTextureImportResult ForInstance = Result;
		for (const TPair<FString, FString>& Reuse : RunningOptions.ReuseTextures)
		{
			ForInstance.Textures.RemoveAll([&Reuse](const FDreamTextureImportedTexture& Texture)
			{
				return Texture.Channel.Equals(Reuse.Key, ESearchCase::CaseSensitive);
			});
			FDreamTextureImportedTexture Kept;
			Kept.Channel = Reuse.Key;
			Kept.Action = EDreamTextureImportAction::Unchanged;
			Kept.ObjectPath = Reuse.Value;
			ForInstance.Textures.Add(MoveTemp(Kept));
		}
		Result.MaterialInstance = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(*Manifest, ForInstance, FString(), /*bSave*/ true);
		if (Result.MaterialInstance.Action == EDreamTextureImportAction::Failed)
		{
			UE_LOG(LogDreamTexture, Warning, TEXT("Importing material %s: the material instance was not created: %s"), *MaterialId, *Result.MaterialInstance.Note);
		}
	}

	FDreamTextureImportStatus& Status = ResetStatus(MaterialId);
	Status.FilesDone = Status.FilesTotal;
	Status.Result = MoveTemp(Result);

	if (Status.Result.bOK)
	{
		Status.Stage = EDreamTextureImportStage::Succeeded;
		Status.Message = Summarize(Status.Result, bCreateMaterialInstance);
		Finish(MaterialId);
	}
	else
	{
		Fail(MaterialId, FText::Format(NSLOCTEXT("DreamTextureImportQueue", "AssetStageFailed", "An error occurred while creating the assets: {0}"), FText::FromString(Status.Result.Error)).ToString());
	}
}

FDreamTextureImportStatus& FDreamTextureImportQueue::ResetStatus(const FString& MaterialId)
{
	TUniquePtr<FDreamTextureImportStatus>& Slot = Statuses.FindOrAdd(MaterialId);
	if (!Slot.IsValid())
	{
		Slot = MakeUnique<FDreamTextureImportStatus>();
	}

	// ResetStatus 既用来开新一轮，也用来在一轮里更新进度。
	// 只有 Stage 还是上一轮留下的终态（Succeeded/Failed）时才是「开新一轮」：整块清空。
	// 进行中的更新保留 Result 以外的字段由调用方各自覆盖。
	FDreamTextureImportStatus& Status = *Slot;
	if (Status.IsFinished() || Status.MaterialId != MaterialId)
	{
		Status = FDreamTextureImportStatus();
		Status.MaterialId = MaterialId;
		Status.Stage = EDreamTextureImportStage::Queued;
	}
	return Status;
}

void FDreamTextureImportQueue::SetStage(const FString& MaterialId, EDreamTextureImportStage Stage, const FString& Message)
{
	FDreamTextureImportStatus& Status = ResetStatus(MaterialId);
	Status.Stage = Stage;
	Status.Message = Message;
	OnStatusChanged.Broadcast(Status);
}

void FDreamTextureImportQueue::Fail(const FString& MaterialId, const FString& Reason)
{
	FDreamTextureImportStatus& Status = ResetStatus(MaterialId);
	Status.Stage = EDreamTextureImportStage::Failed;
	Status.Message = Reason;
	if (Status.Result.Error.IsEmpty())
	{
		Status.Result.Error = Reason;
	}
	UE_LOG(LogDreamTexture, Warning, TEXT("Importing material %s failed: %s"), *MaterialId, *Reason);
	Finish(MaterialId);
}

void FDreamTextureImportQueue::Finish(const FString& MaterialId)
{
	// 先放掉 Running 再广播：订阅者在终态事件里查 IsActive 应当得到 false，
	// 想在回调里立刻重排同一个 id（比如失败后自动重试）也要排得进去。
	Running.Reset();
	RunningOptions = FDreamTextureImportOptions();

	if (const TUniquePtr<FDreamTextureImportStatus>* Found = Statuses.Find(MaterialId))
	{
		OnStatusChanged.Broadcast(**Found);
	}

	Pump();
}
