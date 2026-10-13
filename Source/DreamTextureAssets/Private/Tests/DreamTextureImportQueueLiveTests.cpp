// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureApi.h"
#include "DreamTextureImportQueue.h"
#include "DreamTextureMaterialBuilder.h"
#include "DreamTextureMaterialTestUtils.h"
#include "DreamTextureSettings.h"
#include "DreamTextureTestUtils.h"
#include "MaterialEditingLibrary.h"
#include "HAL/PlatformMisc.h"

using namespace DreamTextureTestUtils;

/*
 * 连固定数据后端的导入队列测试（DreamTexture.Live.ImportQueue）。
 *
 * 只在环境变量 DREAMTEXTURE_LIVE_URL 有值时才跑，没有就 AddInfo("已跳过：…") 并通过。
 * Tools/Test.ps1 -Live 会起 Tools/FixtureBackend 里的后端（127.0.0.1:18777，固定素材 mat_20260827_777001_ref）
 * 并设好这个变量。
 *
 * 测试期间把 UDreamTextureSettings 的 BackendURL 指到它、ImportRoot 指到一个每次唯一的测试目录，
 * 结束时（或中途失败时）还原并把测试资产连包带文件删掉。只改内存里的 CDO，不写任何 ini。
 */

namespace
{
	struct FLiveQueueState
	{
		FString MaterialId = FixtureMaterialId;
		FScopedTestContent Content;
		TArray<FDreamTextureImportStatus> Events;
		FDelegateHandle Handle;
		FString SavedURL;
		FString SavedRoot;
		bool bSavedCreateMaterialInstance = true;
		bool bOverridden = false;

		void Begin(const FString& LiveURL)
		{
			FDreamTextureImportQueue& Queue = FDreamTextureImportQueue::Get();
			Queue.Shutdown();

			UDreamTextureSettings* Settings = GetMutableDefault<UDreamTextureSettings>();
			SavedURL = Settings->BackendURL;
			SavedRoot = Settings->ImportRoot;
			bSavedCreateMaterialInstance = Settings->bCreateMaterialInstance;
			Settings->BackendURL = LiveURL;
			Settings->ImportRoot = Content.Root;
			bOverridden = true;

			Handle = Queue.OnStatusChanged.AddLambda([this](const FDreamTextureImportStatus& Status)
			{
				Events.Add(Status);
			});
		}

		/** 还原设置、摘掉订阅、删测试资产。可以重复调。 */
		void End()
		{
			if (Handle.IsValid())
			{
				FDreamTextureImportQueue::Get().OnStatusChanged.Remove(Handle);
				Handle.Reset();
			}
			if (bOverridden)
			{
				UDreamTextureSettings* Settings = GetMutableDefault<UDreamTextureSettings>();
				Settings->BackendURL = SavedURL;
				Settings->ImportRoot = SavedRoot;
				Settings->bCreateMaterialInstance = bSavedCreateMaterialInstance;
				bOverridden = false;
			}
			FDreamTextureImportQueue::Get().Shutdown();
			Content.Cleanup();
		}

		~FLiveQueueState()
		{
			End();
		}

		bool IsFinished() const
		{
			const FDreamTextureImportStatus* Status = FDreamTextureImportQueue::Get().FindStatus(MaterialId);
			return Status != nullptr && Status->IsFinished();
		}

		int32 IndexOf(EDreamTextureImportStage Stage) const
		{
			for (int32 Index = 0; Index < Events.Num(); ++Index)
			{
				if (Events[Index].Stage == Stage)
				{
					return Index;
				}
			}
			return INDEX_NONE;
		}
	};

	int32 QueueCountAction(const FDreamTextureImportResult& Result, EDreamTextureImportAction Action)
	{
		int32 Count = 0;
		for (const FDreamTextureImportedTexture& Entry : Result.Textures)
		{
			Count += (Entry.Action == Action) ? 1 : 0;
		}
		return Count;
	}

	int32 CountFiles(const FString& Dir)
	{
		TArray<FString> Found;
		IFileManager::Get().FindFiles(Found, *(Dir / TEXT("*.png")), /*Files*/ true, /*Directories*/ false);
		return Found.Num();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureLiveImportQueueTest, "DreamTexture.Live.ImportQueue", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureLiveImportQueueTest::RunTest(const FString& Parameters)
{
	const FString LiveURL = FPlatformMisc::GetEnvironmentVariable(TEXT("DREAMTEXTURE_LIVE_URL")).TrimStartAndEnd();
	if (LiveURL.IsEmpty())
	{
		AddInfo(TEXT("已跳过：没有设置环境变量 DREAMTEXTURE_LIVE_URL。需要先起固定数据后端（Tools/FixtureBackend/Start-FixtureBackend.ps1），"
			"再把它设成 http://127.0.0.1:18777；Tools/Test.ps1 -Live 会替你做这两件事。"));
		return true;
	}

	const TSharedRef<FLiveQueueState> State = MakeShared<FLiveQueueState>();
	const TSharedRef<bool> bAborted = MakeShared<bool>(false);
	const FString DownloadDir = FDreamTextureFetcher::GetDownloadDir(State->MaterialId);

	// 这一轮要导的通道，与队列里用的是同一个函数：默认 4 张，打开分通道导入后 7 张。
	const int32 ExpectedCount = FDreamTextureFetcher::DefaultChannels().Num();

	State->Begin(LiveURL);

	// 之前的下载目录会让「这次到底下没下」说不清，先清掉。
	IFileManager::Get().DeleteDirectory(*DownloadDir, /*RequireExists*/ false, /*Tree*/ true);

	TestTrue(TEXT("排入固定素材"), FDreamTextureImportQueue::Get().Enqueue(State->MaterialId));
	TestFalse(TEXT("已在队列里时再排返回 false"), FDreamTextureImportQueue::Get().Enqueue(State->MaterialId));

	AddLatentWait(this, TEXT("首次导入走到终态"), 90.0, [State]() { return State->IsFinished(); }, bAborted);

	// ---- 首次：下载并新建 ----
	AddLatentStep([this, State, bAborted, ExpectedCount, DownloadDir]()
	{
		if (*bAborted)
		{
			return;
		}

		const FDreamTextureImportStatus* Status = FDreamTextureImportQueue::Get().FindStatus(State->MaterialId);
		if (!TestNotNull(TEXT("有状态"), Status))
		{
			*bAborted = true;
			return;
		}
		TestTrue(FString::Printf(TEXT("首次导入成功（%s / %s）"), *Status->Message, *Status->Result.Error), Status->Stage == EDreamTextureImportStage::Succeeded && Status->Result.bOK);
		TestFalse(TEXT("终态之后不再 active"), FDreamTextureImportQueue::Get().IsActive(State->MaterialId));

		// 阶段顺序：Queued → FetchingManifest → Downloading → Importing → Succeeded。
		const int32 Queued = State->IndexOf(EDreamTextureImportStage::Queued);
		const int32 Fetching = State->IndexOf(EDreamTextureImportStage::FetchingManifest);
		const int32 Downloading = State->IndexOf(EDreamTextureImportStage::Downloading);
		const int32 Importing = State->IndexOf(EDreamTextureImportStage::Importing);
		const int32 Succeeded = State->IndexOf(EDreamTextureImportStage::Succeeded);
		TestTrue(TEXT("阶段事件齐全且有序"), Queued != INDEX_NONE && Queued < Fetching && Fetching < Downloading && Downloading < Importing && Importing < Succeeded);

		// 下载阶段带文件计数，最后一次计满。
		int32 MaxDone = 0;
		int32 MaxTotal = 0;
		for (const FDreamTextureImportStatus& Event : State->Events)
		{
			if (Event.Stage == EDreamTextureImportStage::Downloading)
			{
				MaxDone = FMath::Max(MaxDone, Event.FilesDone);
				MaxTotal = FMath::Max(MaxTotal, Event.FilesTotal);
				TestFalse(TEXT("下载阶段的 Message 不为空"), Event.Message.IsEmpty());
			}
		}
		TestEqual(TEXT("下载总数是要拉的通道数"), MaxTotal, ExpectedCount);
		TestEqual(TEXT("下载完成数计满"), MaxDone, ExpectedCount);

		TestEqual(TEXT("每个通道都是新建"), QueueCountAction(Status->Result, EDreamTextureImportAction::Created), ExpectedCount);
		TestEqual(TEXT("没有失败或跳过的通道"), Status->Result.Textures.Num(), ExpectedCount);
		TestEqual(TEXT("资产目录在测试根下"), Status->Result.AssetFolder, State->Content.Root / TEXT("MossyRiverRocks_ref"));

		for (const FDreamTextureImportedTexture& Entry : Status->Result.Textures)
		{
			const UTexture2D* Texture = FindTexture(Entry.ObjectPath);
			if (TestNotNull(FString::Printf(TEXT("%s 的资产在"), *Entry.Channel), Texture))
			{
				TestEqual(FString::Printf(TEXT("%s 的 sRGB"), *Entry.Channel), Texture->SRGB != 0, Entry.Channel == DreamTextureChannel::BaseColor);
				TestFalse(FString::Printf(TEXT("%s 不翻 G"), *Entry.Channel), Texture->bFlipGreenChannel != 0);
			}
		}

		TestEqual(TEXT("下载目录里有对应数量的 PNG"), CountFiles(DownloadDir), ExpectedCount);

		// ---- 材质实例（M2）：贴图落完之后自动建出 MI_<Slug>，指向刚导入的贴图 ----
		const FDreamTextureImportedTexture& MI = Status->Result.MaterialInstance;
		TestEqual(FString::Printf(TEXT("首次导入新建了材质实例（%s）"), *MI.Note), static_cast<int32>(MI.Action), static_cast<int32>(EDreamTextureImportAction::Created));
		TestEqual(TEXT("材质实例与贴图同目录"), MI.ObjectPath, State->Content.Root / TEXT("MossyRiverRocks_ref") / TEXT("MI_MossyRiverRocks_ref.MI_MossyRiverRocks_ref"));
		TestTrue(TEXT("Message 提到了材质实例"), Status->Message.Contains(TEXT("Material instance")));
		if (const UMaterialInstanceConstant* Instance = FindObject<UMaterialInstanceConstant>(nullptr, *MI.ObjectPath))
		{
			TestTrue(TEXT("父材质是 ImportRoot 下的母材质"), Instance->Parent != nullptr && Instance->Parent->GetPathName() == FDreamTextureMasterMaterial::GetObjectPath(State->Content.Root));
			UMaterialInstanceConstant* Mutable = const_cast<UMaterialInstanceConstant*>(Instance);
			for (const FDreamTextureImportedTexture& Entry : Status->Result.Textures)
			{
				const FName Parameter = Entry.Channel == DreamTextureChannel::BaseColor ? DreamTextureMaterialParam::BaseColor
					: Entry.Channel == DreamTextureChannel::Normal ? DreamTextureMaterialParam::Normal
					: Entry.Channel == DreamTextureChannel::ORM ? DreamTextureMaterialParam::ORM
					: Entry.Channel == DreamTextureChannel::Height ? DreamTextureMaterialParam::Height : NAME_None;
				if (!Parameter.IsNone())
				{
					TestTrue(FString::Printf(TEXT("材质实例的 %s 参数指向导入的贴图"), *Parameter.ToString()),
						UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Mutable, Parameter) == FindTexture(Entry.ObjectPath));
				}
			}
			TestTrue(TEXT("UseNormal 开"), UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Mutable, DreamTextureMaterialParam::UseNormal));
			TestTrue(TEXT("UseORM 开"), UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Mutable, DreamTextureMaterialParam::UseORM));
			TestTrue(TEXT("材质实例已存盘"), FPackageName::DoesPackageExist(Instance->GetOutermost()->GetName()));
		}
		else
		{
			AddError(FString::Printf(TEXT("工程里找不到材质实例 %s"), *MI.ObjectPath));
		}
		TestTrue(TEXT("母材质已存盘"), FPackageName::DoesPackageExist(State->Content.Root / FDreamTextureMasterMaterial::GetAssetName()));

		// 为第二轮做准备：删掉下载目录，这样第二轮「什么都没下载」就能用目录是否重新出现来证明。
		IFileManager::Get().DeleteDirectory(*DownloadDir, false, true);
		State->Events.Reset();
		TestTrue(TEXT("终态之后可以再排"), FDreamTextureImportQueue::Get().Enqueue(State->MaterialId));
	});

	AddLatentWait(this, TEXT("第二次导入走到终态"), 90.0, [State]() { return State->IsFinished(); }, bAborted);

	// ---- 第二次：全部 Unchanged，什么都没下载 ----
	AddLatentStep([this, State, bAborted, ExpectedCount, DownloadDir]()
	{
		if (*bAborted)
		{
			return;
		}

		const FDreamTextureImportStatus* Status = FDreamTextureImportQueue::Get().FindStatus(State->MaterialId);
		if (!TestNotNull(TEXT("第二次有状态"), Status))
		{
			return;
		}
		TestTrue(FString::Printf(TEXT("第二次导入成功（%s / %s）"), *Status->Message, *Status->Result.Error), Status->Stage == EDreamTextureImportStage::Succeeded && Status->Result.bOK);
		TestEqual(TEXT("全部 Unchanged"), QueueCountAction(Status->Result, EDreamTextureImportAction::Unchanged), ExpectedCount);
		TestEqual(TEXT("没有新建也没有更新"), QueueCountAction(Status->Result, EDreamTextureImportAction::Created) + QueueCountAction(Status->Result, EDreamTextureImportAction::Updated), 0);
		TestEqual(TEXT("结果里没有多余的条目"), Status->Result.Textures.Num(), ExpectedCount);
		TestTrue(TEXT("Message 说明已经是最新的"), Status->Message.Contains(TEXT("up to date")));

		// 材质实例的「未变」：已存在、父材质和所有插件管的参数都已是对的，不新建、不改、不标脏。
		const FDreamTextureImportedTexture& MI = Status->Result.MaterialInstance;
		TestEqual(FString::Printf(TEXT("第二次材质实例是 Unchanged（%s）"), *MI.Note), static_cast<int32>(MI.Action), static_cast<int32>(EDreamTextureImportAction::Unchanged));
		if (const UMaterialInstanceConstant* Instance = FindObject<UMaterialInstanceConstant>(nullptr, *MI.ObjectPath))
		{
			TestFalse(TEXT("Unchanged 的材质实例没有被标脏"), Instance->GetOutermost()->IsDirty());
		}

		// 什么都没下载：没有任何一次下载进度的总数大于 0，下载目录里也没有重新出现 PNG。
		for (const FDreamTextureImportStatus& Event : State->Events)
		{
			if (Event.Stage == EDreamTextureImportStage::Downloading)
			{
				TestEqual(TEXT("第二次的下载总数为 0"), Event.FilesTotal, 0);
			}
		}
		TestEqual(TEXT("第二次没有重新下载任何文件"), CountFiles(DownloadDir), 0);
	});

	// ---- 第三次：设置里关掉自动建材质实例 → 只导贴图，材质实例 Skipped 并说明原因，已有的那份不动 ----
	AddLatentStep([this, State, bAborted]()
	{
		if (*bAborted)
		{
			return;
		}
		State->Events.Reset();
		GetMutableDefault<UDreamTextureSettings>()->bCreateMaterialInstance = false;
		TestTrue(TEXT("第三次可以再排"), FDreamTextureImportQueue::Get().Enqueue(State->MaterialId));
	});

	AddLatentWait(this, TEXT("第三次导入走到终态"), 90.0, [State]() { return State->IsFinished(); }, bAborted);

	AddLatentStep([this, State, bAborted]()
	{
		if (*bAborted)
		{
			return;
		}
		const FDreamTextureImportStatus* Status = FDreamTextureImportQueue::Get().FindStatus(State->MaterialId);
		if (!TestNotNull(TEXT("第三次有状态"), Status))
		{
			return;
		}
		TestTrue(FString::Printf(TEXT("第三次导入成功（%s / %s）"), *Status->Message, *Status->Result.Error), Status->Stage == EDreamTextureImportStage::Succeeded && Status->Result.bOK);

		const FDreamTextureImportedTexture& MI = Status->Result.MaterialInstance;
		TestEqual(TEXT("设置关着时材质实例是 Skipped"), static_cast<int32>(MI.Action), static_cast<int32>(EDreamTextureImportAction::Skipped));
		TestTrue(TEXT("Skipped 带原因，点出是设置关的"), MI.Note.Contains(TEXT("setting")));
		TestFalse(TEXT("设置关着时 Message 不提材质实例"), Status->Message.Contains(TEXT("Material instance")));
		TestTrue(TEXT("上一轮建好的材质实例原样还在"),
			FindObject<UMaterialInstanceConstant>(nullptr, *(State->Content.Root / TEXT("MossyRiverRocks_ref") / TEXT("MI_MossyRiverRocks_ref.MI_MossyRiverRocks_ref"))) != nullptr);
	});

	// 收尾放在最后一步：还原设置、删测试资产。即使前面超时中止了也会走到这里。
	AddLatentStep([State]()
	{
		State->End();
	});

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
