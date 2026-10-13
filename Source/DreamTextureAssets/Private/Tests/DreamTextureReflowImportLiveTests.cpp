// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureAnalyzeTestUtils.h"
#include "DreamTextureApi.h"
#include "DreamTextureImportQueue.h"
#include "DreamTextureMaterialBuilder.h"
#include "DreamTextureMaterialTestUtils.h"
#include "DreamTextureSettings.h"
#include "DreamTextureTestUtils.h"
#include "HAL/PlatformMisc.h"
#include "MaterialEditingLibrary.h"

using namespace DreamTextureTestUtils;

/*
 * DreamTexture.Live.ReuseImport：回流靠导入队列的选项做的两件事，对着固定数据后端真跑一遍。
 *
 *   1. 补通道：底色不导入，新材质实例的 BaseColor 指向用户工程里**原来那张**贴图；
 *   2. 原地覆盖：只导入要换的通道，不新建材质实例。
 *
 * 只有 DREAMTEXTURE_LIVE_URL 有值才跑。跑法和约定见 DreamTexture.Live.ImportQueue。
 */

namespace
{
	struct FReuseState
	{
		FString MaterialId = FixtureMaterialId;
		FScopedTestContent Content;
		FString SavedURL;
		FString SavedRoot;
		bool bSavedCreateMaterialInstance = true;
		bool bOverridden = false;

		void Begin(const FString& LiveURL)
		{
			FDreamTextureImportQueue::Get().Shutdown();
			UDreamTextureSettings* Settings = GetMutableDefault<UDreamTextureSettings>();
			SavedURL = Settings->BackendURL;
			SavedRoot = Settings->ImportRoot;
			bSavedCreateMaterialInstance = Settings->bCreateMaterialInstance;
			Settings->BackendURL = LiveURL;
			Settings->ImportRoot = Content.Root;
			Settings->bCreateMaterialInstance = true;
			bOverridden = true;
		}

		void End()
		{
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

		~FReuseState()
		{
			End();
		}

		bool IsFinished() const
		{
			const FDreamTextureImportStatus* Status = FDreamTextureImportQueue::Get().FindStatus(MaterialId);
			return Status != nullptr && Status->IsFinished();
		}

		FString Folder() const
		{
			return Content.Root / TEXT("MossyRiverRocks_ref");
		}
	};

	bool HasChannel(const FDreamTextureImportResult& Result, const TCHAR* Channel)
	{
		return Result.Textures.ContainsByPredicate([Channel](const FDreamTextureImportedTexture& Entry) { return Entry.Channel == Channel; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureLiveReuseImportTest, "DreamTexture.Live.ReuseImport", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureLiveReuseImportTest::RunTest(const FString& Parameters)
{
	const FString LiveURL = FPlatformMisc::GetEnvironmentVariable(TEXT("DREAMTEXTURE_LIVE_URL")).TrimStartAndEnd();
	if (LiveURL.IsEmpty())
	{
		AddInfo(TEXT("已跳过：没有设置环境变量 DREAMTEXTURE_LIVE_URL。需要先起固定数据后端（Tools/FixtureBackend/Start-FixtureBackend.ps1），"
			"再把它设成 http://127.0.0.1:18777；Tools/Test.ps1 -Live 会替你做这两件事。"));
		return true;
	}

	const TSharedRef<FReuseState> State = MakeShared<FReuseState>();
	const TSharedRef<bool> bAborted = MakeShared<bool>(false);
	const FString DownloadDir = FDreamTextureFetcher::GetDownloadDir(State->MaterialId);

	State->Begin(LiveURL);
	IFileManager::Get().DeleteDirectory(*DownloadDir, /*RequireExists*/ false, /*Tree*/ true);

	// 「用户工程里原来的底色」：一张带 sRGB 的彩色贴图，采样器与母材质的 BaseColor 参数匹配。
	UTexture2D* UserBase = MakeColorTexture(State->Content.Root, TEXT("T_UserBase"), 7);
	if (!TestNotNull(TEXT("造好用户原来的底色贴图"), UserBase))
	{
		State->End();
		return false;
	}

	// ---- 1. 补通道：只导入法线、ORM、高度；底色沿用用户原来那张 ----
	FDreamTextureImportOptions Fill;
	Fill.bExplicitChannels = true;
	Fill.Channels = { DreamTextureChannel::Normal, DreamTextureChannel::ORM, DreamTextureChannel::Height };
	Fill.ReuseTextures.Add(DreamTextureChannel::BaseColor, UserBase->GetPathName());
	TestTrue(TEXT("带选项排入固定素材"), FDreamTextureImportQueue::Get().Enqueue(State->MaterialId, Fill));
	TestFalse(TEXT("已在队列里时再排（不论带不带选项）返回 false"), FDreamTextureImportQueue::Get().Enqueue(State->MaterialId, Fill));

	AddLatentWait(this, TEXT("补通道的导入走到终态"), 90.0, [State]() { return State->IsFinished(); }, bAborted);

	AddLatentStep([this, State, bAborted, DownloadDir, UserBase]()
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
		TestTrue(FString::Printf(TEXT("导入成功（%s / %s）"), *Status->Message, *Status->Result.Error), Status->Stage == EDreamTextureImportStage::Succeeded && Status->Result.bOK);
		TestEqual(TEXT("只导入了三个通道"), Status->Result.Textures.Num(), 3);
		TestFalse(TEXT("底色没有导入（往返一圈重新编码的那张不要）"), HasChannel(Status->Result, TEXT("basecolor")));
		TestTrue(TEXT("法线、ORM、高度导入了"), HasChannel(Status->Result, TEXT("normal")) && HasChannel(Status->Result, TEXT("orm")) && HasChannel(Status->Result, TEXT("height")));

		// 下载阶段也只拉了三个文件，没有 basecolor.png。
		TArray<FString> Downloaded;
		IFileManager::Get().FindFiles(Downloaded, *(DownloadDir / TEXT("*.png")), true, false);
		TestEqual(TEXT("只下载了三个文件"), Downloaded.Num(), 3);
		TestFalse(TEXT("没下载底色"), Downloaded.Contains(TEXT("basecolor.png")));

		// 工程里没有 T_*_BC：底色这个资产根本没建。
		TestNull(TEXT("没有为底色建资产"), FindTexture(State->Folder() / TEXT("T_MossyRiverRocks_ref_BC.T_MossyRiverRocks_ref_BC")));
		TestNotNull(TEXT("法线的资产建了"), FindTexture(State->Folder() / TEXT("T_MossyRiverRocks_ref_N.T_MossyRiverRocks_ref_N")));

		// 材质实例：BaseColor 指向用户原来那张，其余指向刚导入的。
		const FDreamTextureImportedTexture& MI = Status->Result.MaterialInstance;
		TestEqual(FString::Printf(TEXT("新建了材质实例（%s）"), *MI.Note), static_cast<int32>(MI.Action), static_cast<int32>(EDreamTextureImportAction::Created));
		TestEqual(TEXT("材质实例与导入的贴图同目录"), MI.ObjectPath, State->Folder() / TEXT("MI_MossyRiverRocks_ref.MI_MossyRiverRocks_ref"));
		UMaterialInstanceConstant* Instance = FindObject<UMaterialInstanceConstant>(nullptr, *MI.ObjectPath);
		if (TestNotNull(TEXT("工程里找得到材质实例"), Instance))
		{
			TestTrue(TEXT("BaseColor 指向用户原来的贴图"), UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, DreamTextureMaterialParam::BaseColor) == UserBase);
			TestTrue(TEXT("BaseColor 参数是实例上的覆盖值（不是母材质的默认占位）"), UMaterialEditingLibrary::IsMaterialInstanceParameterOverridden(Instance, DreamTextureMaterialParam::BaseColor));
			const FDreamTextureImportedTexture* Normal = Status->Result.Textures.FindByPredicate([](const FDreamTextureImportedTexture& E) { return E.Channel == TEXT("normal"); });
			if (Normal != nullptr)
			{
				TestTrue(TEXT("Normal 指向刚导入的法线"), UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, DreamTextureMaterialParam::Normal) == FindTexture(Normal->ObjectPath));
			}
			TestTrue(TEXT("UseNormal 开"), UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Instance, DreamTextureMaterialParam::UseNormal));
			TestTrue(TEXT("UseORM 开"), UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Instance, DreamTextureMaterialParam::UseORM));
		}

		// 摘要里「新建 N 张」只数这次导入的三张，沿用的底色不算。
		TestTrue(TEXT("摘要里新建 3 张"), Status->Message.Contains(TEXT("3 created")));

		// 为下一轮做准备：删掉下载目录，让「只下载要换的通道」能用文件数证明。
		IFileManager::Get().DeleteDirectory(*DownloadDir, false, true);
	});

	// ---- 2. 原地覆盖：只导入法线，不新建材质实例（这套的材质实例上一轮已经建好，不能被动） ----
	AddLatentStep([this, State, bAborted]()
	{
		if (*bAborted)
		{
			return;
		}
		FDreamTextureImportOptions InPlace;
		InPlace.bExplicitChannels = true;
		InPlace.Channels = { DreamTextureChannel::Normal };
		InPlace.bCreateMaterialInstance = false;
		TestTrue(TEXT("终态之后带选项再排"), FDreamTextureImportQueue::Get().Enqueue(State->MaterialId, InPlace));
	});
	AddLatentWait(this, TEXT("原地覆盖的导入走到终态"), 90.0, [State]() { return State->IsFinished(); }, bAborted);
	AddLatentStep([this, State, bAborted]()
	{
		if (*bAborted)
		{
			return;
		}
		const FDreamTextureImportStatus* Status = FDreamTextureImportQueue::Get().FindStatus(State->MaterialId);
		if (!TestNotNull(TEXT("有状态"), Status))
		{
			return;
		}
		TestTrue(FString::Printf(TEXT("导入成功（%s / %s）"), *Status->Message, *Status->Result.Error), Status->Stage == EDreamTextureImportStage::Succeeded && Status->Result.bOK);
		TestEqual(TEXT("只处理了法线一个通道"), Status->Result.Textures.Num(), 1);
		TestEqual(TEXT("法线上一轮已经导入过：未变"), static_cast<int32>(Status->Result.Textures[0].Action), static_cast<int32>(EDreamTextureImportAction::Unchanged));
		TestEqual(TEXT("这一轮不建材质实例"), static_cast<int32>(Status->Result.MaterialInstance.Action), static_cast<int32>(EDreamTextureImportAction::Skipped));
		TestTrue(TEXT("原因说明是调用方自己处理"), Status->Result.MaterialInstance.Note.Contains(TEXT("the caller")));
		TestFalse(TEXT("摘要里不提材质实例"), Status->Message.Contains(TEXT("Material instance")));
	});

	// ---- 3. 显式给了空的通道表：不能悄悄退回默认（那会多下一堆东西），要明说 ----
	AddLatentStep([this, State, bAborted]()
	{
		if (*bAborted)
		{
			return;
		}
		FDreamTextureImportOptions Empty;
		Empty.bExplicitChannels = true;
		TestTrue(TEXT("再排一次"), FDreamTextureImportQueue::Get().Enqueue(State->MaterialId, Empty));
	});
	AddLatentWait(this, TEXT("空通道表的导入走到终态"), 60.0, [State]() { return State->IsFinished(); }, bAborted);
	AddLatentStep([this, State, bAborted]()
	{
		if (*bAborted)
		{
			return;
		}
		const FDreamTextureImportStatus* Status = FDreamTextureImportQueue::Get().FindStatus(State->MaterialId);
		if (TestNotNull(TEXT("有状态"), Status))
		{
			TestTrue(TEXT("失败"), Status->Stage == EDreamTextureImportStage::Failed);
			TestTrue(TEXT("原因点明没有要导入的通道"), Status->Message.Contains(TEXT("no channels to import")));
		}
	});

	// ---- 4. 选项不残留：再排一个不带选项的，走设置里的默认通道 ----
	AddLatentStep([this, State, bAborted]()
	{
		if (*bAborted)
		{
			return;
		}
		TestTrue(TEXT("不带选项再排"), FDreamTextureImportQueue::Get().Enqueue(State->MaterialId));
	});
	AddLatentWait(this, TEXT("默认选项的导入走到终态"), 90.0, [State]() { return State->IsFinished(); }, bAborted);
	AddLatentStep([this, State, bAborted]()
	{
		if (*bAborted)
		{
			return;
		}
		const FDreamTextureImportStatus* Status = FDreamTextureImportQueue::Get().FindStatus(State->MaterialId);
		if (TestNotNull(TEXT("有状态"), Status))
		{
			TestTrue(FString::Printf(TEXT("导入成功（%s）"), *Status->Message), Status->Stage == EDreamTextureImportStage::Succeeded && Status->Result.bOK);
			TestEqual(TEXT("按设置里的默认通道导入（上一轮的选项没有残留）"), Status->Result.Textures.Num(), FDreamTextureFetcher::DefaultChannels().Num());
			TestTrue(TEXT("这回底色导入了"), HasChannel(Status->Result, TEXT("basecolor")));
		}
	});

	AddLatentStep([State]()
	{
		State->End();
	});
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
