// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureConnection.h"
#include "DreamTextureReflowActions.h"
#include "DreamTextureReflowModel.h"
#include "DreamTextureReflowRun.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

/**
 * 回流（M8/M9）Editor 层自动化测试共用的小工具：假的选区与解析、假的连接、假的运行钩子。
 *
 * 全部不碰真实选区、不连后端、不写工程目录：解析结果是手搓的，上传 / 提交 / 导入的回调都攥在手里
 * （或按配置当场回），由测试决定时序。真的连后端的部分见 DreamTexture.Live.Reflow。
 */
namespace DreamTextureReflowTest
{
	// ---- 手搓的解析结果 ----

	inline FDreamTextureMaterialTexture MakeRow(const TCHAR* Name, EDreamTextureRole Role, std::initializer_list<const TCHAR*> Parameters = {},
		bool bExportable = true, int32 Width = 1024, int32 Height = 1024)
	{
		FDreamTextureMaterialTexture Row;
		Row.TexturePath = FString::Printf(TEXT("/Game/Test/%s.%s"), Name, Name);
		Row.AssetName = Name;
		Row.SourceWidth = Width;
		Row.SourceHeight = Height;
		Row.Role = Role;
		Row.Evidence = EDreamTextureEvidence::MaterialGraph;
		Row.bExportable = bExportable;
		if (!bExportable)
		{
			Row.ExportBlocker = TEXT("资产里没有源数据。");
		}
		for (const TCHAR* Parameter : Parameters)
		{
			Row.Parameters.Add(FName(Parameter));
		}
		return Row;
	}

	inline FDreamTextureMaterialTexture MakeOrmRow(const TCHAR* Name = TEXT("T_ORM"))
	{
		FDreamTextureMaterialTexture Row = MakeRow(Name, EDreamTextureRole::Packed, { TEXT("ORM") });
		Row.Layout = FDreamTextureChannelLayout::MakeORM();
		return Row;
	}

	inline FDreamTextureMaterialAnalysis MakeAnalysis(UMaterialInterface* Material, const TCHAR* AssetPath, TArray<FDreamTextureMaterialTexture> Rows, bool bInstance = true)
	{
		FDreamTextureMaterialAnalysis Analysis;
		Analysis.bOK = true;
		Analysis.Material = Material;
		Analysis.MaterialPath = AssetPath;
		Analysis.AssetPath = AssetPath;
		Analysis.DisplayName = FPaths::GetBaseFilename(AssetPath);
		Analysis.bIsInstance = bInstance;
		Analysis.Textures = MoveTemp(Rows);
		return Analysis;
	}

	/** 典型的一套：底色、法线（参数）、标准 ORM；没有高度。 */
	inline TArray<FDreamTextureMaterialTexture> TypicalRows()
	{
		return { MakeRow(TEXT("T_Base"), EDreamTextureRole::BaseColor, { TEXT("BaseColor") }), MakeRow(TEXT("T_N"), EDreamTextureRole::Normal, { TEXT("Normal") }), MakeOrmRow() };
	}

	/** 只有底色。 */
	inline TArray<FDreamTextureMaterialTexture> BaseOnlyRows()
	{
		return { MakeRow(TEXT("T_Base"), EDreamTextureRole::BaseColor, { TEXT("BaseColor") }) };
	}

	// ---- 模型的假后端 ----

	struct FFakeSelection
	{
		TArray<UMaterialInterface*> Materials;
		TMap<FString, FDreamTextureMaterialAnalysis> Results;
		FDreamTextureOverrideStore* Store = nullptr;
		int32 AnalyzeCalls = 0;

		FDreamTextureReflowBackend Make()
		{
			FDreamTextureReflowBackend Out;
			Out.Overrides = Store;
			Out.GetSelectedMaterials = [this]() { return Materials; };
			Out.GetSelectedActorSlots = []() { return TArray<FDreamTextureMaterialSlot>(); };
			Out.Analyze = [this](UMaterialInterface* Material, const FDreamTextureAnalyzeOptions& Options)
			{
				++AnalyzeCalls;
				const FDreamTextureMaterialAnalysis* Preset = Results.Find(Material->GetPathName());
				if (Preset == nullptr)
				{
					FDreamTextureMaterialAnalysis Failed;
					Failed.Error = TEXT("测试：没有预设的解析结果");
					return Failed;
				}
				FDreamTextureMaterialAnalysis Copy = *Preset;
				// 按真解析的契约套用户指认，这样「指认之后重新解析、通道现状跟着变」可以单测。
				if (Options.bApplyOverrides && Options.Overrides != nullptr)
				{
					for (FDreamTextureMaterialTexture& Row : Copy.Textures)
					{
						FDreamTextureRoleOverride Override;
						if (Options.Overrides->Find(Copy.AssetPath, Row.TexturePath, Override))
						{
							Row.Role = Override.Role;
							Row.Layout = Override.Layout;
							Row.Evidence = EDreamTextureEvidence::UserOverride;
						}
					}
				}
				return Copy;
			};
			Out.Export = [](const FDreamTextureMaterialAnalysis&, const FDreamTextureExportOptions&) { return TArray<FDreamTextureExportedImage>(); };
			Out.MakeTempDir = []() { return FString(TEXT("C:/Fake/Reflow")); };
			return Out;
		}
	};

	/** 连接的假身：基类把状态字段留给子类可写，getter 都是非虚的，直接读成员。 */
	class FFakeConnection : public FDreamTextureConnection
	{
	public:
		void Set(EDreamTextureLink InLink, bool bReachable)
		{
			Link = InLink;
			bBackendReachable = bReachable;
		}
		void SetHealth(const FDreamTextureHealth& InHealth) { Health = InHealth; }

		void SetAllReady()
		{
			Set(EDreamTextureLink::Live, true);
			FDreamTextureHealth Ready;
			Ready.Mode = TEXT("attach");
			Ready.bAlive = true;
			Ready.bReady = true;
			SetHealth(Ready);
		}
	};

	inline FString PluginDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamTextureForUnreal"));
		return Plugin.IsValid() ? Plugin->GetBaseDir() : FString();
	}

	/** Tests/Data/Reflow/workflows.json：新后端（带「已有底图」出图段、tileable）真实 /api/workflows 应答的截取。 */
	inline bool LoadCatalog(FAutomationTestBase& Test, FDreamTextureWorkflowCatalog& Out, const TCHAR* FileName = TEXT("workflows.json"))
	{
		const FString Path = FPaths::Combine(PluginDir(), TEXT("Tests"), TEXT("Data"), TEXT("Reflow"), FileName);
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			Test.AddError(FString::Printf(TEXT("读不到目录固定数据：%s"), *Path));
			return false;
		}
		if (!FDreamTextureWorkflowCatalog::FromJsonString(Text, Out))
		{
			Test.AddError(TEXT("目录固定数据解析失败"));
			return false;
		}
		return true;
	}

	/** 去掉「已有底图」出图段与它的两个组合：模拟还没有「补齐通道」的旧后端。 */
	inline FDreamTextureWorkflowCatalog WithoutUpload(const FDreamTextureWorkflowCatalog& Full)
	{
		FDreamTextureWorkflowCatalog Out = Full;
		Out.Segments.RemoveAll([](const FDreamTextureWorkflowMeta& Meta) { return Meta.Id == TEXT("src-upload-v1"); });
		Out.Workflows.RemoveAll([](const FDreamTextureWorkflowMeta& Meta) { return Meta.SourceSegment == TEXT("src-upload-v1"); });
		return Out;
	}

	/** 指认库的临时文件。 */
	struct FTempStore
	{
		FString Path;
		FDreamTextureOverrideStore Store;

		FTempStore()
			: Path(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamTextureTest") / TEXT("ReflowActions") / (FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".json"))))
			, Store(Path)
		{
		}

		~FTempStore()
		{
			IFileManager::Get().Delete(*Path, false, true);
			IFileManager::Get().Delete(*(Path + TEXT(".tmp")), false, true);
		}
	};

	// ---- 运行的假世界 ----

	inline FDreamTextureJob MakeJob(const FString& Id, EDreamTextureJobStatus Status, float Progress = 0.0f, const TCHAR* Stage = TEXT(""), const TCHAR* Error = TEXT(""))
	{
		FDreamTextureJob Job;
		Job.Id = Id;
		Job.MaterialId = TEXT("mat_") + Id;
		Job.WorkflowId = TEXT("src-upload-v1.dec-derive-v1");
		Job.BatchId = TEXT("batch1");
		Job.Status = Status;
		Job.Progress = Progress;
		Job.Stage = Stage;
		Job.Error = Error;
		return Job;
	}

	inline FDreamTextureImportStatus MakeImportStatus(const FString& MaterialId, EDreamTextureImportStage Stage, const TCHAR* Message = TEXT(""), int32 FilesDone = 0, int32 FilesTotal = 0)
	{
		FDreamTextureImportStatus Status;
		Status.MaterialId = MaterialId;
		Status.Stage = Stage;
		Status.Message = Message;
		Status.FilesDone = FilesDone;
		Status.FilesTotal = FilesTotal;
		return Status;
	}

	/** 一次导入成功的结果：新建三张贴图和一个材质实例。 */
	inline FDreamTextureImportResult MakeImportedResult(const TCHAR* InstancePath = TEXT("/Game/DreamTexture/Rock_ab12/MI_Rock_ab12.MI_Rock_ab12"))
	{
		FDreamTextureImportResult Result;
		Result.bOK = true;
		Result.AssetFolder = TEXT("/Game/DreamTexture/Rock_ab12");
		for (const TCHAR* Channel : { TEXT("normal"), TEXT("orm"), TEXT("height") })
		{
			FDreamTextureImportedTexture Texture;
			Texture.Channel = Channel;
			Texture.Action = EDreamTextureImportAction::Created;
			Texture.ObjectPath = FString::Printf(TEXT("/Game/DreamTexture/Rock_ab12/T_Rock_ab12_%s.T_Rock_ab12_%s"), Channel, Channel);
			Result.Textures.Add(Texture);
		}
		Result.MaterialInstance.Action = EDreamTextureImportAction::Created;
		Result.MaterialInstance.ObjectPath = InstancePath;
		return Result;
	}

	/**
	 * 假世界：运行的所有外部能力都在这里，调用被记下来，回调按配置当场回或攥在手里。
	 * 追踪器是真的（带假网络层）：任务状态的变化经 Track() 走真实的 OnJobChanged。
	 */
	struct FRunWorld
	{
		// 后端
		int32 BackendCalls = 0;
		bool bBackendOK = true;
		FString BackendError = TEXT("测试：后端起不来");
		bool bHoldBackend = false;
		TArray<TFunction<void(bool, const FString&)>> HeldBackend;

		// 导出
		int32 ExportCalls = 0;
		bool bExportOK = true;
		bool bExportReturnsNothing = false;
		FString ExportReason = TEXT("测试：贴图没有源数据");
		FString Sha = TEXT("0123456789abcdef");
		TArray<FDreamTextureExportOptions> ExportOptions;
		TArray<FDreamTextureMaterialAnalysis> ExportAnalyses;

		// 上传
		bool bUploadOK = true;
		FString UploadError = TEXT("上传失败：后端返回 HTTP 502：转存到 ComfyUI 失败");
		FString StoredName = TEXT("dt_ref_fake.png");
		bool bHoldUpload = false;
		TArray<FString> UploadedFiles;
		TArray<FDreamTextureUploadApi::FOnUploaded> HeldUploads;

		// 提交
		bool bSubmitOK = true;
		FString SubmitError = TEXT("提交失败：后端返回 HTTP 400：参数 source_image 必填");
		bool bHoldSubmit = false;
		TArray<FDreamTextureGenerateRequest> SubmitRequests;
		TArray<FDreamTextureGenerateApi::FOnSubmitted> HeldSubmits;
		FString JobId = TEXT("job_1");
		TArray<FString> CancelIds;
		TArray<FDreamTextureGenerateApi::FOnDone> CancelDones;

		// 导入
		bool bEnqueueResult = true;
		TArray<FString> EnqueuedIds;
		TArray<FDreamTextureImportOptions> EnqueuedOptions;
		TMap<int32, TFunction<void(const FDreamTextureImportStatus&)>> ImportWatchers;
		int32 NextWatcher = 1;

		// manifest
		bool bManifestOK = true;
		int32 ManifestCalls = 0;
		FDreamTextureManifest ManifestToReturn;

		// 原地覆盖
		int32 ApplyCalls = 0;
		FDreamTextureReflowApplyResult ApplyResult;
		TArray<FDreamTextureReflowOverwrite> AppliedItems;
		FDreamTextureImportResult AppliedImport;

		TSharedRef<FDreamTextureJobTracker> Tracker;

		FRunWorld()
			: Tracker(MakeShared<FDreamTextureJobTracker>(MakeTrackerHooks()))
		{
			ApplyResult.bOK = true;
			ApplyResult.Changed = { TEXT("Normal → T_Rock_ab12_normal") };
			ApplyResult.bTransacted = true;
			ManifestToReturn.Id = TEXT("mat_job_1");
			ManifestToReturn.Preview = TEXT("preview.png");
			for (const TCHAR* Channel : { TEXT("basecolor"), TEXT("normal"), TEXT("height") })
			{
				FDreamTextureMapInfo Info;
				Info.Channel = Channel;
				Info.File = FString(Channel) + TEXT(".png");
				ManifestToReturn.Maps.Add(Channel, Info);
			}
		}

		FDreamTextureJobTracker::FHooks MakeTrackerHooks()
		{
			FDreamTextureJobTracker::FHooks Out;
			Out.GetJob = [](const FString&, FDreamTextureGenerateApi::FOnJob) {};
			Out.Submit = [this](const FDreamTextureGenerateRequest& Request, FDreamTextureGenerateApi::FOnSubmitted Done)
			{
				SubmitRequests.Add(Request);
				if (bHoldSubmit)
				{
					HeldSubmits.Add(MoveTemp(Done));
					return;
				}
				if (!bSubmitOK)
				{
					Done(false, TArray<FDreamTextureJob>(), SubmitError);
					return;
				}
				Done(true, { MakeJob(JobId, EDreamTextureJobStatus::Queued) }, FString());
			};
			Out.Cancel = [this](const FString& Id, FDreamTextureGenerateApi::FOnDone Done)
			{
				CancelIds.Add(Id);
				CancelDones.Add(MoveTemp(Done));
			};
			return Out;
		}

		FDreamTextureReflowRunHooks Make()
		{
			FDreamTextureReflowRunHooks Out;
			Out.EnsureBackend = [this](TFunction<void(bool, const FString&)> Done)
			{
				++BackendCalls;
				if (bHoldBackend)
				{
					HeldBackend.Add(MoveTemp(Done));
					return;
				}
				Done(bBackendOK, bBackendOK ? FString() : BackendError);
			};
			Out.Export = [this](const FDreamTextureMaterialAnalysis& Analysis, const FDreamTextureExportOptions& Options)
			{
				++ExportCalls;
				ExportOptions.Add(Options);
				ExportAnalyses.Add(Analysis);
				TArray<FDreamTextureExportedImage> Results;
				if (bExportReturnsNothing)
				{
					return Results;
				}
				for (const FDreamTextureMaterialTexture& Row : Analysis.Textures)
				{
					FDreamTextureExportedImage Image;
					Image.TexturePath = Row.TexturePath;
					if (bExportOK)
					{
						Image.bExported = true;
						Image.FilePath = Options.OutputDir / (Row.AssetName + TEXT(".png"));
						Image.Sha256 = Sha;
						Image.Width = Image.Height = Options.MaxEdge > 0 ? FMath::Min(Options.MaxEdge, Row.SourceWidth) : Row.SourceWidth;
					}
					else
					{
						Image.Reason = ExportReason;
					}
					Results.Add(MoveTemp(Image));
				}
				return Results;
			};
			Out.MakeTempDir = []() { return FString(TEXT("C:/Fake/Reflow/run1")); };
			Out.Upload = [this](const FString& FilePath, FDreamTextureUploadApi::FOnUploaded Done)
			{
				UploadedFiles.Add(FilePath);
				if (bHoldUpload)
				{
					HeldUploads.Add(MoveTemp(Done));
					return;
				}
				if (!bUploadOK)
				{
					Done(false, FString(), UploadError);
					return;
				}
				FString Name = StoredName;
				Done(true, MoveTemp(Name), FString());
			};
			Out.GetTracker = [this]() -> TSharedPtr<FDreamTextureJobTracker> { return Tracker; };
			Out.Enqueue = [this](const FString& MaterialId, const FDreamTextureImportOptions& Options)
			{
				EnqueuedIds.Add(MaterialId);
				EnqueuedOptions.Add(Options);
				return bEnqueueResult;
			};
			Out.GetManifest = [this](const FString&, FDreamTextureApi::FOnManifest Done)
			{
				++ManifestCalls;
				FDreamTextureManifest Copy = ManifestToReturn;
				Done(bManifestOK, MoveTemp(Copy), bManifestOK ? FString() : FString(TEXT("测试：没拉到")));
			};
			Out.ApplyInPlace = [this](UMaterialInstanceConstant*, const TArray<FDreamTextureReflowOverwrite>& Items, const FDreamTextureImportResult& Imported)
			{
				++ApplyCalls;
				AppliedItems = Items;
				AppliedImport = Imported;
				return ApplyResult;
			};
			Out.WatchJobs = [this](TFunction<void(const FDreamTextureJob&)> Callback) -> TFunction<void()>
			{
				const FDelegateHandle Handle = Tracker->OnJobChanged.AddLambda([Callback = MoveTemp(Callback)](const FDreamTextureJob& Job) { Callback(Job); });
				const TWeakPtr<FDreamTextureJobTracker> Weak = Tracker;
				return [Weak, Handle]()
				{
					if (const TSharedPtr<FDreamTextureJobTracker> Pinned = Weak.Pin())
					{
						Pinned->OnJobChanged.Remove(Handle);
					}
				};
			};
			Out.WatchImports = [this](TFunction<void(const FDreamTextureImportStatus&)> Callback) -> TFunction<void()>
			{
				const int32 Key = NextWatcher++;
				ImportWatchers.Add(Key, MoveTemp(Callback));
				return [this, Key]() { ImportWatchers.Remove(Key); };
			};
			return Out;
		}

		/** 把一条导入状态喂给所有还在订阅的运行（复制一份再遍历：回调里可能退订）。 */
		void PushImport(const FDreamTextureImportStatus& Status)
		{
			TArray<TFunction<void(const FDreamTextureImportStatus&)>> Copies;
			for (const TPair<int32, TFunction<void(const FDreamTextureImportStatus&)>>& Pair : ImportWatchers)
			{
				Copies.Add(Pair.Value);
			}
			for (const TFunction<void(const FDreamTextureImportStatus&)>& Fn : Copies)
			{
				Fn(Status);
			}
		}

		/** 后端推来任务的新状态：走追踪器的 Track，由它广播 OnJobChanged。 */
		void PushJob(const FDreamTextureJob& Job)
		{
			Tracker->Track(Job, FDreamTextureReflowRun::GetOrigin());
		}
	};

	/** 一份能直接跑的运行输入（补通道、新建）。 */
	inline FDreamTextureReflowRunSpec MakeFillSpec(const TCHAR* Workflow = TEXT("src-upload-v1.dec-derive-v1"))
	{
		FDreamTextureReflowRunSpec Spec;
		Spec.Mode = EDreamTextureReflowMode::FillChannels;
		Spec.Title = TEXT("补齐缺失通道 · MI_Rock");
		Spec.MaterialAssetPath = TEXT("/Game/Test/MI_Rock.MI_Rock");
		Spec.MaterialName = TEXT("MI_Rock");
		const FDreamTextureMaterialTexture Base = MakeRow(TEXT("T_Base"), EDreamTextureRole::BaseColor, { TEXT("BaseColor") });
		Spec.ExportAnalysis = MakeAnalysis(nullptr, TEXT("/Game/Test/MI_Rock.MI_Rock"), { Base });
		Spec.SourceTexturePath = Base.TexturePath;
		Spec.SourceTextureName = Base.AssetName;
		Spec.ExportMaxEdge = 2048;
		Spec.Request.WorkflowId = Workflow;
		Spec.Request.Variants = 1;
		Spec.Request.Name = TEXT("Rock 补通道");
		Spec.Request.DerivedFrom.Kind = FDreamTextureDerivedFrom::KindUEMaterial();
		Spec.Request.DerivedFrom.AssetPath = Spec.MaterialAssetPath;
		Spec.Request.DerivedFrom.Engine = TEXT("5.8.2");
		Spec.Request.DerivedFrom.Mode = FDreamTextureDerivedFrom::ModeFillChannels();
		Spec.UploadParamKey = TEXT("source_image");
		Spec.ImportOptions.bExplicitChannels = true;
		Spec.ImportOptions.Channels = { TEXT("normal"), TEXT("orm"), TEXT("height") };
		Spec.ImportOptions.ReuseTextures.Add(TEXT("basecolor"), Base.TexturePath);
		return Spec;
	}

	/** 一个选好材质、目录已加载、三级都正常的页面。大多数测试的起点。 */
	struct FPageFixture
	{
		FTempStore Store;
		FFakeSelection Selection;
		TSharedRef<FFakeConnection> Conn = MakeShared<FFakeConnection>();
		FRunWorld World;
		FDreamTextureWorkflowCatalog Catalog;
		TArray<FDreamTextureGenerateApi::FOnCatalog> CatalogCalls;
		TArray<FDreamTextureReflowRunSpec> RunSpecs;
		TArray<TSharedRef<FDreamTextureReflowRun>> Runs;

		UMaterialInstanceConstant* Instance = nullptr;
		UMaterial* Master = nullptr;
		TSharedPtr<FDreamTextureReflowModel> Model;
		TSharedPtr<FDreamTextureReflowActions> Actions;

		FPageFixture()
		{
			Conn->SetAllReady();
			Selection.Store = &Store.Store;
			Master = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
			Instance = NewObject<UMaterialInstanceConstant>(GetTransientPackage(), NAME_None, RF_Transient);
			Model = MakeShared<FDreamTextureReflowModel>(Selection.Make());
		}

		/** 把材质摆进选区并取一遍。 */
		void Select(UMaterialInterface* Material, const TCHAR* AssetPath, TArray<FDreamTextureMaterialTexture> Rows, bool bInstance = true)
		{
			Selection.Results.Add(Material->GetPathName(), MakeAnalysis(Material, AssetPath, MoveTemp(Rows), bInstance));
			Selection.Materials = { Material };
			Model->RefreshSelection();
		}

		void Build(FAutomationTestBase& Test, bool bCompleteCatalog = true, const TCHAR* CatalogFile = TEXT("workflows.json"))
		{
			LoadCatalog(Test, Catalog, CatalogFile);

			FDreamTextureReflowActionsBackend Backend;
			Backend.ListWorkflows = [this](FDreamTextureGenerateApi::FOnCatalog Done) { CatalogCalls.Add(MoveTemp(Done)); };
			Backend.GetConnection = [this]() -> TSharedPtr<FDreamTextureConnection> { return Conn; };
			Backend.GetEngineVersion = []() { return FString(TEXT("5.8.2")); };
			Backend.MakeRun = [this](FDreamTextureReflowRunSpec&& Spec)
			{
				RunSpecs.Add(Spec);
				const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MoveTemp(Spec), World.Make());
				Runs.Add(Run);
				return Run;
			};
			Actions = MakeShared<FDreamTextureReflowActions>(Model.ToSharedRef(), MoveTemp(Backend));
			Actions->Initialize(/*bBindExternal*/ false);
			if (bCompleteCatalog)
			{
				CompleteCatalog(Catalog);
			}
		}

		void CompleteCatalog(const FDreamTextureWorkflowCatalog& InCatalog, int32 Index = -1)
		{
			FDreamTextureWorkflowCatalog Copy = InCatalog;
			CatalogCalls[Index < 0 ? CatalogCalls.Num() - 1 : Index](true, MoveTemp(Copy), FString());
		}

		/** 典型的一个实例（底色、法线、ORM，缺高度），选好，页面在默认状态。 */
		void SelectTypical(FAutomationTestBase& Test)
		{
			Select(Instance, TEXT("/Game/Test/MI_Rock.MI_Rock"), TypicalRows());
			Build(Test);
		}
	};

	/** 记下运行每次广播时的步骤，用来断言步骤的顺序。 */
	struct FStepLog
	{
		TArray<EDreamTextureReflowStep> Steps;
		TArray<FString> Messages;

		void Bind(FDreamTextureReflowRun& Run)
		{
			Run.OnChanged.AddLambda([this, &Run]()
			{
				if (Steps.Num() == 0 || Steps.Last() != Run.GetStep())
				{
					Steps.Add(Run.GetStep());
				}
				Messages.Add(Run.GetMessage().ToString());
			});
		}

		bool Contains(EDreamTextureReflowStep Step) const { return Steps.Contains(Step); }
	};
}

#endif // WITH_DEV_AUTOMATION_TESTS
