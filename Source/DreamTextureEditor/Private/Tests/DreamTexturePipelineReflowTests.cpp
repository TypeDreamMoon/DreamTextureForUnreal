// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "DreamTextureApi.h"
#include "DreamTextureConnection.h"
#include "DreamTextureImporter.h"
#include "DreamTextureLiveTestUtils.h"
#include "DreamTextureMaterialBuilder.h"
#include "DreamTextureReflowTestUtils.h"
#include "DreamTextureSettings.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "TextureCompiler.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

/*
 * DreamTexture.Pipeline.Reflow：回流的整条链路，对着**真的 ComfyUI** 走一遍。
 *
 *   固定素材导入成「用户的材质」 -> 回流页模型选中它 -> 真导出 PNG -> 真上传 -> 真提交（已有底图 × 传统派生，
 *   UV 展开所以 tileable=false）-> 真后端出图 -> 真导入队列落资产 -> 新材质实例建出来
 *
 * 断言：
 *   - 新 MI 建出来了，在导入目录下，父材质是母材质；
 *   - 它的 BaseColor 参数指向**用户原来那张**贴图，工程里没有为底色另建资产（往返一圈重新编码的那张不要）；
 *   - Normal / ORM / Height 指向刚导入的贴图，UseNormal / UseORM 是开的；
 *   - 后端的 manifest 带 derived_from：kind / mode / asset_path / engine / maps_in.basecolor（= 送上去那份文件的指纹），
 *     tileable 为 false；
 *   - 原材质一根线都没动。
 * 第二半：同一套对着一个**完整**的实例做「原地覆盖」：只勾法线，实例的 Normal 参数换成新生成的，别的参数不动，
 * 一次 Ctrl+Z 退回。
 *
 * 只有 DREAMTEXTURE_PIPELINE_URL 有值才跑（Tools/Test.ps1 -Pipeline 会起隔离的 ComfyUI + 新后端并设好这个变量；
 * 它指向一个有真 ComfyUI 的后端），没有就 AddInfo 并通过。叶子名是 .Reflow：与 Pipeline.Generate 互不为前缀。
 * 用的是传统派生，不需要 CHORD 的模型，也不吃多少显存。
 *
 * 测试期间把 BackendURL 指到流水线后端、ImportRoot 指到每次唯一的测试目录（只改内存里的 CDO，不碰 ini），
 * 结束时一律还原并把测试资产连包带文件删掉。超时一律先取消任务（别把后端唯一的 worker 留在生成里）再判失败。
 */

namespace
{
	using namespace DreamTextureLive;
	using namespace DreamTextureReflowTest;

	constexpr double JobTimeoutSecs = 300.0;

	FString GetPipelineURL()
	{
		return FPlatformMisc::GetEnvironmentVariable(TEXT("DREAMTEXTURE_PIPELINE_URL")).TrimStartAndEnd();
	}

	/** 一份「用户的材质」：用固定素材导入、建好材质实例。 */
	struct FSourceMaterial
	{
		FString Root;
		UMaterialInstanceConstant* Instance = nullptr;
		UTexture* Base = nullptr;
		UTexture* Normal = nullptr;
		UTexture* Orm = nullptr;
		UTexture* Height = nullptr;
	};

	UTexture* GetParameter(const UMaterialInterface* Material, const FName& Parameter, bool bOverriddenOnly = false)
	{
		UTexture* Value = nullptr;
		Material->GetTextureParameterValue(FHashedMaterialParameterInfo(Parameter), Value, bOverriddenOnly);
		return Value;
	}

	struct FPipelineReflowState
	{
		FString URL;
		FString SavedBackendURL;
		FString SavedImportRoot;
		bool bSavedCreateMI = true;
		bool bSettingsOverridden = false;

		/** 唯一的测试目录：「用户的」材质、完整版材质、回流新建的东西都落在它下面。 */
		FString Root = TEXT("/Game/DreamTextureTests/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);

		TSharedPtr<FDreamTextureConnection> Conn;
		TSharedPtr<FDreamTextureJobTracker> Tracker;
		TSharedPtr<FTempStore> Store;
		TSharedPtr<FDreamTextureReflowModel> Model;
		TSharedPtr<FDreamTextureReflowActions> Actions;

		FSourceMaterial BaseOnly;
		FSourceMaterial Full;
		UMaterialInterface* Selected = nullptr;

		bool bAbort = false;
		FDreamTextureManifest FixtureManifest;
		TMap<FString, FString> FixtureFiles;

		TSharedPtr<FDreamTextureReflowRun> FirstRun;
		TSharedPtr<FDreamTextureReflowRun> SecondRun;

		void Cleanup()
		{
			// 贴图的异步编译还在读它们的源数据，删之前必须等完。
			FTextureCompilingManager::Get().FinishAllCompilation();

			TArray<UObject*> ToDelete;
			const FString Prefix = Root + TEXT("/");
			for (TObjectIterator<UPackage> It; It; ++It)
			{
				UPackage* Package = *It;
				if (Package->GetName().StartsWith(Prefix))
				{
					ForEachObjectWithPackage(Package, [&ToDelete](UObject* Object)
					{
						if (Object->IsAsset())
						{
							ToDelete.Add(Object);
						}
						return true;
					}, EGetObjectsFlags::None);
				}
			}
			if (ToDelete.Num() > 0)
			{
				ObjectTools::DeleteObjectsUnchecked(ToDelete);
			}

			const FString DiskDir = FPackageName::LongPackageNameToFilename(Root);
			IFileManager::Get().DeleteDirectory(*DiskDir, /*RequireExists*/ false, /*Tree*/ true);
			IFileManager::Get().DeleteDirectory(*FPaths::GetPath(DiskDir), /*RequireExists*/ false, /*Tree*/ false);
			if (FAssetRegistryModule* Module = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry")))
			{
				Module->Get().ScanPathsSynchronous({ Root }, /*bForceRescan*/ true);
			}
		}

		/** 取消还没结束的任务，别把后端唯一的 worker 留在生成里。 */
		void CancelUnfinished()
		{
			for (const TSharedPtr<FDreamTextureReflowRun>& Run : { FirstRun, SecondRun })
			{
				if (Run.IsValid() && Run->IsRunning())
				{
					Run->Cancel();
				}
			}
		}

		bool AllRunsFinished() const
		{
			for (const TSharedPtr<FDreamTextureReflowRun>& Run : { FirstRun, SecondRun })
			{
				if (Run.IsValid() && !Run->IsFinished())
				{
					return false;
				}
			}
			return true;
		}

		void End()
		{
			CancelUnfinished();
			Actions.Reset();
			Model.Reset();

			if (Tracker.IsValid())
			{
				Tracker->Unbind();
			}
			if (Conn.IsValid())
			{
				Conn->Stop();
			}
			if (bSettingsOverridden)
			{
				UDreamTextureSettings* Settings = GetMutableDefault<UDreamTextureSettings>();
				Settings->BackendURL = SavedBackendURL;
				Settings->ImportRoot = SavedImportRoot;
				Settings->bCreateMaterialInstance = bSavedCreateMI;
				bSettingsOverridden = false;
			}
			FDreamTextureImportQueue::Get().Shutdown();
			Cleanup();
		}

		~FPipelineReflowState()
		{
			End();
		}
	};

	/** 用固定素材导入一套（可以去掉几个通道），并建材质实例。 */
	bool BuildSource(FAutomationTestBase* Test, FPipelineReflowState& S, const FString& ContentRoot, std::initializer_list<const TCHAR*> Drop, FSourceMaterial& Out)
	{
		FDreamTextureImportRequest Request;
		Request.Manifest = S.FixtureManifest;
		Request.LocalFiles = S.FixtureFiles;
		for (const TCHAR* Channel : Drop)
		{
			Request.Manifest.Maps.Remove(Channel);
			Request.LocalFiles.Remove(Channel);
		}
		Request.ContentRoot = ContentRoot;
		Request.bSave = false;
		const FDreamTextureImportResult Imported = FDreamTextureImporter::Import(Request);
		if (!Test->TestTrue(FString::Printf(TEXT("导入固定素材成功（%s）"), *Imported.Error), Imported.bOK))
		{
			return false;
		}
		const FDreamTextureImportedTexture Created = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Request.Manifest, Imported, ContentRoot, /*bSave*/ false);
		Out.Root = ContentRoot;
		Out.Instance = FindObject<UMaterialInstanceConstant>(nullptr, *Created.ObjectPath);
		if (!Test->TestNotNull(FString::Printf(TEXT("「用户的」材质实例建出来了（%s）"), *Created.Note), Out.Instance))
		{
			return false;
		}
		Out.Base = GetParameter(Out.Instance, DreamTextureMaterialParam::BaseColor, true);
		Out.Normal = GetParameter(Out.Instance, DreamTextureMaterialParam::Normal, true);
		Out.Orm = GetParameter(Out.Instance, DreamTextureMaterialParam::ORM, true);
		Out.Height = GetParameter(Out.Instance, DreamTextureMaterialParam::Height, true);
		return Test->TestNotNull(TEXT("「用户的」底色贴图"), Out.Base);
	}

	FDreamTextureReflowRunHooks MakeHooks(const TSharedRef<FPipelineReflowState>& S)
	{
		// 真实的钩子，只换三处：后端已经由 Test.ps1 起好了（不走进程托管）；任务追踪与订阅用自己这套，
		// 不去碰模块全局那套（它挂着编辑器模块的自动导入，只认 generate 来源）。
		FDreamTextureReflowRunHooks Hooks = FDreamTextureReflowRunHooks::MakeDefault();
		Hooks.EnsureBackend = [](TFunction<void(bool, const FString&)> Done) { Done(true, FString()); };
		Hooks.GetTracker = [S]() { return S->Tracker; };
		Hooks.WatchJobs = [S](TFunction<void(const FDreamTextureJob&)> Callback) -> TFunction<void()>
		{
			const FDelegateHandle Handle = S->Tracker->OnJobChanged.AddLambda([Callback = MoveTemp(Callback)](const FDreamTextureJob& Job) { Callback(Job); });
			const TWeakPtr<FDreamTextureJobTracker> Weak = S->Tracker;
			return [Weak, Handle]()
			{
				if (const TSharedPtr<FDreamTextureJobTracker> Pinned = Weak.Pin())
				{
					Pinned->OnJobChanged.Remove(Handle);
				}
			};
		};
		return Hooks;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTexturePipelineReflowTest, "DreamTexture.Pipeline.Reflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTexturePipelineReflowTest::RunTest(const FString& Parameters)
{
	const FString URL = GetPipelineURL();
	if (URL.IsEmpty())
	{
		AddInfo(TEXT("已跳过：没有设置环境变量 DREAMTEXTURE_PIPELINE_URL（指向一个有真 ComfyUI 的后端）。"
			"Tools/Test.ps1 -Pipeline 会起隔离的 ComfyUI + 新后端（Tools/PipelineBackend/）并设好它；显存不够（默认空闲不足 10 GiB）会拒绝启动。"));
		return true;
	}

	FAutomationTestBase* Test = this;
	const TSharedRef<FPipelineReflowState> S = MakeShared<FPipelineReflowState>();
	S->URL = URL;

	// ---- 1. 设置指向流水线后端；连接、追踪器；导入固定素材、建两个「用户的」材质 ----
	EnqueueDo([S, Test]()
	{
		UDreamTextureSettings* Settings = GetMutableDefault<UDreamTextureSettings>();
		S->SavedBackendURL = Settings->BackendURL;
		S->SavedImportRoot = Settings->ImportRoot;
		S->bSavedCreateMI = Settings->bCreateMaterialInstance;
		Settings->BackendURL = S->URL;
		Settings->ImportRoot = S->Root;
		Settings->bCreateMaterialInstance = true;
		S->bSettingsOverridden = true;

		S->Conn = MakeShared<FDreamTextureConnection>();
		S->Tracker = MakeShared<FDreamTextureJobTracker>();
		S->Tracker->Bind(S->Conn);
		S->Conn->Start();

		const FString FixtureDir = FPaths::Combine(PluginDir(), TEXT("Tests"), TEXT("Fixtures"), TEXT("mat_20260827_777001_ref"));
		FString Json;
		FString Error;
		if (!FFileHelper::LoadFileToString(Json, *(FixtureDir / TEXT("manifest.json"))) || !FDreamTextureManifest::FromJsonString(Json, S->FixtureManifest, Error))
		{
			Test->AddError(FString::Printf(TEXT("读不到固定素材的 manifest：%s %s"), *FixtureDir, *Error));
			S->bAbort = true;
			return;
		}
		for (const TPair<FString, FDreamTextureMapInfo>& Pair : S->FixtureManifest.Maps)
		{
			S->FixtureFiles.Add(Pair.Key, FixtureDir / Pair.Value.File);
		}

		// 「用户的」材质一：只有底色（要补通道的）。二：整套都有（要原地覆盖的）。放在不同的目录，各有各的母材质。
		S->Store = MakeShared<FTempStore>();
		if (!BuildSource(Test, *S, S->Root / TEXT("BaseOnly"), { TEXT("normal"), TEXT("orm"), TEXT("height"), TEXT("roughness"), TEXT("metallic"), TEXT("ao") }, S->BaseOnly)
			|| !BuildSource(Test, *S, S->Root / TEXT("Full"), {}, S->Full))
		{
			S->bAbort = true;
			return;
		}
		Test->TestNull(TEXT("前提：只有底色的材质，Normal 参数没有被覆盖（挂的是母材质的默认占位）"), S->BaseOnly.Normal);
		Test->TestNotNull(TEXT("前提：完整的材质，Normal 参数挂着导入的法线"), S->Full.Normal);

		// 回流页模型：选区换成「用户的」材质，其余（解析、导出）都是真的。
		FDreamTextureReflowBackend Backend = FDreamTextureReflowBackend::MakeDefault();
		Backend.Overrides = &S->Store->Store;
		Backend.GetSelectedMaterials = [S]() { return TArray<UMaterialInterface*>{ S->Selected }; };
		Backend.GetSelectedActorSlots = []() { return TArray<FDreamTextureMaterialSlot>(); };
		S->Model = MakeShared<FDreamTextureReflowModel>(MoveTemp(Backend));

		FDreamTextureReflowActionsBackend ActionsBackend = FDreamTextureReflowActionsBackend::MakeDefault();
		ActionsBackend.GetConnection = [S]() -> TSharedPtr<FDreamTextureConnection> { return S->Conn; };
		ActionsBackend.MakeRun = [S](FDreamTextureReflowRunSpec&& Spec)
		{
			return MakeShared<FDreamTextureReflowRun>(MoveTemp(Spec), MakeHooks(S));
		};
		S->Actions = MakeShared<FDreamTextureReflowActions>(S->Model.ToSharedRef(), MoveTemp(ActionsBackend));
		S->Actions->Initialize(/*bBindExternal*/ false);
	});

	// ---- 2. 链路 Live、ComfyUI 就绪；目录读回来 ----
	EnqueueWait(Test, TEXT("链路到达 Live、ComfyUI 就绪（流水线后端的 ComfyUI 冷启动约 15~20 秒，Start 脚本已等过）"), 60.0,
		[S]() { return S->bAbort || (S->Conn->GetLink() == EDreamTextureLink::Live && S->Conn->GetHealth().bAlive); },
		nullptr, [S]() { S->bAbort = true; });
	EnqueueWait(Test, TEXT("工作流目录读回来"), 30.0,
		[S]() { return S->bAbort || S->Actions->GetCatalogState() != FDreamTextureReflowActions::ECatalogState::Loading; },
		[S, Test]()
		{
			if (S->bAbort)
			{
				return;
			}
			if (S->Actions->GetCatalogState() != FDreamTextureReflowActions::ECatalogState::Ready)
			{
				S->bAbort = true;
				Test->AddError(FString::Printf(TEXT("读取工作流目录失败：%s"), *S->Actions->GetCatalogError()));
				return;
			}
			if (S->Actions->FindUploadSource() == nullptr)
			{
				S->bAbort = true;
				Test->AddInfo(TEXT("已跳过：流水线后端的目录里没有「已有底图」出图段（后端早于「补齐通道」那一版）。"));
			}
		});

	// ---- 3. 补通道：选中只有底色的材质，UV 展开，开始 ----
	EnqueueDo([S, Test]()
	{
		if (S->bAbort)
		{
			return;
		}
		S->Selected = S->BaseOnly.Instance;
		S->Model->RefreshSelection();
		Test->TestTrue(TEXT("页面进入 Ready"), S->Model->GetViewState() == FDreamTextureReflowModel::EViewState::Ready);

		S->Actions->SetUVKind(EDreamTextureUVKind::Unwrapped);
		Test->TestEqual(TEXT("UV 展开默认选传统派生（不是 CHORD）"), S->Actions->GetDecomposeId(), FString(TEXT("dec-derive-v1")));
		Test->TestTrue(TEXT("缺的通道默认都勾上（法线、粗糙度/金属度/AO、高度），已有的底色不勾"),
			S->Actions->IsChannelChecked(TEXT("normal")) && S->Actions->IsChannelChecked(TEXT("height")) && S->Actions->IsChannelChecked(TEXT("roughness")) && !S->Actions->IsChannelChecked(TEXT("basecolor")));

		FText Why;
		if (!Test->TestTrue(FString::Printf(TEXT("能开始（%s）"), *Why.ToString()), S->Actions->CanRun(&Why)))
		{
			S->bAbort = true;
			return;
		}
		S->Actions->Run();
		S->FirstRun = S->Actions->GetRun();
		if (!Test->TestTrue(TEXT("运行开始了"), S->FirstRun.IsValid() && S->FirstRun->IsRunning()))
		{
			S->bAbort = true;
		}
	});
	EnqueueWait(Test, TEXT("补通道：运行走到终态（真上传、真生成、真导入）"), JobTimeoutSecs,
		[S]() { return S->bAbort || !S->FirstRun.IsValid() || S->FirstRun->IsFinished(); },
		[S, Test]()
		{
			if (S->bAbort || !S->FirstRun.IsValid())
			{
				return;
			}
			Test->AddInfo(FString::Printf(TEXT("补通道的运行：%s"), *S->FirstRun->GetMessage().ToString()));
			Test->TestTrue(FString::Printf(TEXT("补通道成功（%s）"), *S->FirstRun->GetMessage().ToString()), S->FirstRun->GetState() == FDreamTextureReflowRun::EState::Succeeded);
		},
		[S]()
		{
			// 超时：先取消任务，别把后端唯一的 worker 留在生成里。
			S->CancelUnfinished();
			S->bAbort = true;
		});

	// ---- 4. 核对补通道的产出（manifest 是运行结束之后才异步拉的，等它到） ----
	EnqueueWait(Test, TEXT("补通道：结果的 manifest 拉到了"), 20.0,
		[S]() { return S->bAbort || !S->FirstRun.IsValid() || S->FirstRun->GetState() != FDreamTextureReflowRun::EState::Succeeded || S->FirstRun->HasManifest(); });
	EnqueueDo([S, Test]()
	{
		if (S->bAbort || !S->FirstRun.IsValid() || S->FirstRun->GetState() != FDreamTextureReflowRun::EState::Succeeded)
		{
			return;
		}
		const FDreamTextureReflowRun& Run = *S->FirstRun;
		const FDreamTextureImportStatus* Import = FDreamTextureImportQueue::Get().FindStatus(Run.GetMaterialId());
		if (!Test->TestNotNull(TEXT("有导入状态"), Import))
		{
			return;
		}
		const FDreamTextureImportResult& Result = Import->Result;

		// ---- 新实例 ----
		UMaterialInstanceConstant* NewInstance = FindObject<UMaterialInstanceConstant>(nullptr, *Run.GetResultInstancePath());
		if (!Test->TestNotNull(FString::Printf(TEXT("新材质实例建出来了（%s）"), *Run.GetResultInstancePath()), NewInstance))
		{
			return;
		}
		Test->TestTrue(TEXT("新实例落在测试导入目录下"), NewInstance->GetPathName().StartsWith(S->Root));
		Test->TestFalse(TEXT("新实例不是原来那个"), NewInstance == S->BaseOnly.Instance);
		Test->TestTrue(TEXT("父材质是 ImportRoot 下的母材质"), NewInstance->Parent != nullptr && NewInstance->Parent->GetPathName() == FDreamTextureMasterMaterial::GetObjectPath(S->Root));

		// ---- BaseColor 指向用户原来那张；工程里没有为底色另建资产 ----
		Test->TestTrue(TEXT("新实例的 BaseColor 指向用户原来那张贴图"), GetParameter(NewInstance, DreamTextureMaterialParam::BaseColor, true) == S->BaseOnly.Base);
		for (const FDreamTextureImportedTexture& Texture : Result.Textures)
		{
			Test->TestFalse(TEXT("导入的通道里没有底色（往返一圈重新编码的那张不要）"), Texture.Channel == TEXT("basecolor"));
		}
		const FString Folder = FPackageName::GetLongPackagePath(FPackageName::ObjectPathToPackageName(Run.GetResultInstancePath()));
		TArray<FString> BaseColorAssets;
		for (TObjectIterator<UTexture2D> It; It; ++It)
		{
			if (It->GetPathName().StartsWith(Folder) && It->GetName().EndsWith(TEXT("_BC")))
			{
				BaseColorAssets.Add(It->GetPathName());
			}
		}
		Test->TestEqual(TEXT("新材质的目录里没有 _BC 贴图"), BaseColorAssets.Num(), 0);

		// ---- 其余通道指向刚导入的贴图 ----
		for (const FDreamTextureImportedTexture& Texture : Result.Textures)
		{
			const FName Parameter = Texture.Channel == TEXT("normal") ? DreamTextureMaterialParam::Normal
				: Texture.Channel == TEXT("orm") ? DreamTextureMaterialParam::ORM
				: Texture.Channel == TEXT("height") ? DreamTextureMaterialParam::Height : NAME_None;
			if (!Parameter.IsNone())
			{
				UTexture* Imported = FindObject<UTexture2D>(nullptr, *Texture.ObjectPath);
				Test->TestTrue(FString::Printf(TEXT("新实例的 %s 指向刚导入的 %s"), *Parameter.ToString(), *Texture.Channel), Imported != nullptr && GetParameter(NewInstance, Parameter, true) == Imported);
			}
		}

		// ---- 后端的 manifest：derived_from ----
		if (Test->TestTrue(TEXT("拿到了结果的 manifest"), Run.HasManifest() && Run.GetManifest().Raw.IsValid()))
		{
			const FDreamTextureManifest& Manifest = Run.GetManifest();
			const TSharedPtr<FJsonObject>* Derived = nullptr;
			if (Test->TestTrue(TEXT("manifest 带 derived_from"), Manifest.Raw->TryGetObjectField(TEXT("derived_from"), Derived) && Derived != nullptr))
			{
				FString Text;
				Test->TestTrue(TEXT("derived_from.kind"), (*Derived)->TryGetStringField(TEXT("kind"), Text) && Text == TEXT("ue_material"));
				Test->TestTrue(TEXT("derived_from.mode"), (*Derived)->TryGetStringField(TEXT("mode"), Text) && Text == TEXT("fill_channels"));
				Test->TestTrue(TEXT("derived_from.asset_path 是用户的材质"), (*Derived)->TryGetStringField(TEXT("asset_path"), Text) && Text == S->BaseOnly.Instance->GetPathName());
				Test->TestTrue(TEXT("derived_from.engine"), (*Derived)->TryGetStringField(TEXT("engine"), Text) && !Text.IsEmpty() && Text.StartsWith(TEXT("5.")));
				const TSharedPtr<FJsonObject>* MapsIn = nullptr;
				if (Test->TestTrue(TEXT("derived_from.maps_in"), (*Derived)->TryGetObjectField(TEXT("maps_in"), MapsIn) && MapsIn != nullptr))
				{
					FString Fingerprint;
					Test->TestTrue(TEXT("maps_in.basecolor 是送上去那份文件的指纹"), (*MapsIn)->TryGetStringField(TEXT("basecolor"), Fingerprint) && Fingerprint == Run.GetExportedSha());
					TArray<uint8> Sent;
					if (FFileHelper::LoadFileToArray(Sent, *Run.GetExportedFile()))
					{
						Test->TestEqual(TEXT("指纹确实是导出文件全部字节的 SHA-256 前 16 位"), FDreamTextureFetcher::HashBytes16(Sent), Run.GetExportedSha());
					}
				}
			}
			Test->TestFalse(TEXT("UV 展开：manifest 的 tileable 为 false"), Manifest.bTileable);
			Test->TestTrue(TEXT("补通道的材质没有提示词"), Manifest.Prompt.IsEmpty());
			Test->TestTrue(TEXT("manifest 里有法线与 ORM"), Manifest.FindMap(TEXT("normal")) != nullptr && Manifest.FindMap(TEXT("orm")) != nullptr);
		}

		// ---- 原材质一根线都没动 ----
		Test->TestTrue(TEXT("原材质的 BaseColor 没变"), GetParameter(S->BaseOnly.Instance, DreamTextureMaterialParam::BaseColor, true) == S->BaseOnly.Base);
		Test->TestNull(TEXT("原材质没有多出 Normal 覆盖"), GetParameter(S->BaseOnly.Instance, DreamTextureMaterialParam::Normal, true));
		Test->TestNull(TEXT("原材质没有多出 ORM 覆盖"), GetParameter(S->BaseOnly.Instance, DreamTextureMaterialParam::ORM, true));
	});

	// ---- 5. 原地覆盖：对完整的实例只换法线 ----
	EnqueueDo([S, Test]()
	{
		if (S->bAbort)
		{
			return;
		}
		S->Selected = S->Full.Instance;
		S->Model->RefreshSelection();
		Test->TestEqual(TEXT("完整的材质：补通道默认什么都不覆盖（都已有）"), S->Actions->GetSelection().Num(), 0);

		S->Actions->SetInPlace(true);
		Test->TestTrue(TEXT("原地覆盖选得上（材质实例）"), S->Actions->IsInPlace());
		S->Actions->SetChannelChecked(TEXT("normal"), true);
		FText Why;
		if (!Test->TestTrue(FString::Printf(TEXT("能开始（%s）"), *Why.ToString()), S->Actions->CanRun(&Why)))
		{
			S->bAbort = true;
			return;
		}
		S->Actions->Run();
		S->SecondRun = S->Actions->GetRun();
		if (!Test->TestTrue(TEXT("运行开始了"), S->SecondRun.IsValid() && S->SecondRun->IsRunning()))
		{
			S->bAbort = true;
		}
	});
	EnqueueWait(Test, TEXT("原地覆盖：运行走到终态"), JobTimeoutSecs,
		[S]() { return S->bAbort || !S->SecondRun.IsValid() || S->SecondRun->IsFinished(); },
		[S, Test]()
		{
			if (S->bAbort || !S->SecondRun.IsValid())
			{
				return;
			}
			Test->AddInfo(FString::Printf(TEXT("原地覆盖的运行：%s"), *S->SecondRun->GetMessage().ToString()));
			Test->TestTrue(FString::Printf(TEXT("原地覆盖成功（%s）"), *S->SecondRun->GetMessage().ToString()), S->SecondRun->GetState() == FDreamTextureReflowRun::EState::Succeeded);
		},
		[S]()
		{
			S->CancelUnfinished();
			S->bAbort = true;
		});

	EnqueueDo([S, Test]()
	{
		if (S->bAbort || !S->SecondRun.IsValid() || S->SecondRun->GetState() != FDreamTextureReflowRun::EState::Succeeded)
		{
			return;
		}
		UMaterialInstanceConstant* Instance = S->Full.Instance;
		UTexture* NewNormal = GetParameter(Instance, DreamTextureMaterialParam::Normal, true);
		Test->TestTrue(TEXT("Normal 参数换成了新生成的（不再是原来那张）"), NewNormal != nullptr && NewNormal != S->Full.Normal);
		Test->TestTrue(TEXT("新法线是新导入的资产（在测试导入目录里，不在用户材质的目录里）"), NewNormal != nullptr && NewNormal->GetPathName().StartsWith(S->Root) && !NewNormal->GetPathName().StartsWith(S->Root / TEXT("Full")));
		Test->TestTrue(TEXT("BaseColor 没动"), GetParameter(Instance, DreamTextureMaterialParam::BaseColor, true) == S->Full.Base);
		Test->TestTrue(TEXT("ORM 没动"), GetParameter(Instance, DreamTextureMaterialParam::ORM, true) == S->Full.Orm);
		Test->TestTrue(TEXT("Height 没动"), GetParameter(Instance, DreamTextureMaterialParam::Height, true) == S->Full.Height);
		Test->TestEqual(TEXT("结果指向被改的实例"), S->SecondRun->GetResultInstancePath(), Instance->GetPathName());
		Test->TestFalse(TEXT("原地覆盖没有另建材质实例"), S->SecondRun->GetImportResult().MaterialInstance.Action == EDreamTextureImportAction::Created);

		// ---- Ctrl+Z 退回 ----
		if (Test->TestNotNull(TEXT("编辑器的撤销栈"), GEditor != nullptr ? GEditor->Trans.Get() : nullptr))
		{
			Test->TestTrue(TEXT("UndoTransaction 成功"), GEditor->UndoTransaction());
			Test->TestTrue(TEXT("撤销后 Normal 退回原来那张"), GetParameter(Instance, DreamTextureMaterialParam::Normal, true) == S->Full.Normal);
		}
	});

	// ---- 6. 收尾：任务一定要是终态；还原设置、删测试资产 ----
	EnqueueDo([S]()
	{
		S->CancelUnfinished();
	});
	EnqueueWait(Test, TEXT("收尾：运行都已结束"), 60.0, [S]() { return S->AllRunsFinished(); });
	EnqueueDo([S]()
	{
		S->End();
	});

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
