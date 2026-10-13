// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "DreamTextureClientModule.h"
#include "DreamTextureConnection.h"
#include "DreamTextureGenerate.h"
#include "DreamTextureGenerateModel.h"
#include "DreamTextureImportNotifier.h"
#include "DreamTextureImportQueue.h"
#include "DreamTextureJobTracker.h"
#include "DreamTextureLiveTestUtils.h"
#include "DreamTextureMaterialBuilder.h"
#include "DreamTextureSettings.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "TextureCompiler.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

/*
 * DreamTexture.Pipeline.Generate：生成页的整条链路，对着**真的 ComfyUI** 走一遍。
 *
 *   生成页模型提交 -> 追踪器收 job.* 事件 -> 任务 Succeeded -> 自动导入（真的 FDreamTextureAutoImporter 订阅
 *   真的模块追踪器，真的导入队列）-> 贴图与 MI_<名字> 落进工程 -> 材质实例的父材质与贴图参数核对。
 *
 * 分两半，各自独立（一半失败不挡着另一半）：
 *   1. src-local-stylized-v1.dec-derive-v1    手绘 + 传统派生。最快、不吃 CHORD、可商用。
 *   2. src-local-realistic-v1.dec-chord-v1    写实 + CHORD。manifest 的 license_flags.commercial_use=false，
 *                                             导入完成的通知必须把「不可商用」说出来，生成页提交前的许可提示也要亮。
 * 两半都取候选里**最小的分辨率**、1 个变体、固定种子（要出图是真出图，能省则省）。
 *
 * 用的是模块自己那套（FDreamTextureClientModule 的连接与追踪器、编辑器模块里的自动导入、全局导入队列），
 * 不自己另建——「真自动导入」就是要验证这一套在真实事件下真的触发。为了让它触发，测试期间把 bAutoImportGenerated
 * 打开（只改内存里的 CDO，不碰 DefaultEditor.ini）、ImportRoot 指到每次唯一的测试目录，结束时一律还原并连包带文件删掉测试资产。
 *
 * 只有 DREAMTEXTURE_PIPELINE_URL 有值才跑（Tools/Test.ps1 -Pipeline 会起隔离的 ComfyUI + 新后端并设好这个变量），
 * 没有就 AddInfo 并通过。叶子名是 .Generate：以后别的 Pipeline 测试（如 .Reflow）的全名不能与它互为前缀。
 *
 * 缺模型的那一半跳过并说明缺什么（查 /api/models 的逐工作流清单，同时把 /api/checks 的结论打进日志）；
 * 超时一律先取消任务（别把后端唯一的 worker 留在生成里）再判失败。
 */

namespace
{
	using namespace DreamTextureLive;

	struct FPipelineHttpResult
	{
		bool bDone = false;
		bool bGotResponse = false;
		int32 Code = 0;
		FString Body;
	};

	void FirePipelineGet(const FString& BaseURL, const FString& Path, const TSharedRef<FPipelineHttpResult>& Out)
	{
		const FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(BaseURL + Path);
		Request->SetVerb(TEXT("GET"));
		Request->SetTimeout(20.0f);
		Request->OnProcessRequestComplete().BindLambda([Out](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
		{
			Out->bGotResponse = bSucceeded && Response.IsValid();
			Out->Code = Out->bGotResponse ? Response->GetResponseCode() : 0;
			Out->Body = Out->bGotResponse ? Response->GetContentAsString() : FString();
			Out->bDone = true;
		});
		if (!Request->ProcessRequest() && !Out->bDone)
		{
			Out->bDone = true;
		}
	}

	/** 把后端上一个文件原样存到本地（预览图，给人看的产物）。失败只记日志，不影响测试结论。 */
	void FirePipelineDownload(const FString& Url, const FString& DestPath, FAutomationTestBase* Test, const FString& What)
	{
		const FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Url);
		Request->SetVerb(TEXT("GET"));
		Request->SetTimeout(30.0f);
		Request->OnProcessRequestComplete().BindLambda([DestPath, Test, What](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
		{
			if (bSucceeded && Response.IsValid() && Response->GetResponseCode() == 200 && FFileHelper::SaveArrayToFile(Response->GetContent(), *DestPath))
			{
				Test->AddInfo(FString::Printf(TEXT("%s：%s（%d 字节）"), *What, *DestPath, Response->GetContent().Num()));
			}
			else
			{
				Test->AddInfo(FString::Printf(TEXT("%s：没存成（HTTP %d）"), *What, Response.IsValid() ? Response->GetResponseCode() : 0));
			}
		});
		Request->ProcessRequest();
	}

	TSharedPtr<FJsonObject> ParsePipelineJson(const FString& Text)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, Root) ? Root : nullptr;
	}

	/**
	 * /api/models 的 inventory.requirements[]：{dir, file, workflow_ids[], present}。
	 * 返回这些工作流 id 之一用到、却没就位的模型（"dir/file"）。解析不了返回 false（调用方当「不知道」，不跳过）。
	 */
	bool FindMissingModels(const FString& Body, const TSet<FString>& WorkflowIds, TArray<FString>& OutMissing)
	{
		const TSharedPtr<FJsonObject> Root = ParsePipelineJson(Body);
		const TSharedPtr<FJsonObject>* Inventory = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Requirements = nullptr;
		if (!Root.IsValid() || !Root->TryGetObjectField(TEXT("inventory"), Inventory) || Inventory == nullptr
			|| !(*Inventory)->TryGetArrayField(TEXT("requirements"), Requirements) || Requirements == nullptr)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Requirements)
		{
			const TSharedPtr<FJsonObject> Req = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Req.IsValid())
			{
				continue;
			}
			bool bPresent = true;
			Req->TryGetBoolField(TEXT("present"), bPresent);
			if (bPresent)
			{
				continue;
			}

			bool bUsed = false;
			const TArray<TSharedPtr<FJsonValue>>* Users = nullptr;
			if (Req->TryGetArrayField(TEXT("workflow_ids"), Users) && Users != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& User : *Users)
				{
					FString Id;
					if (User.IsValid() && User->TryGetString(Id) && WorkflowIds.Contains(Id))
					{
						bUsed = true;
						break;
					}
				}
			}
			if (bUsed)
			{
				FString Dir;
				FString File;
				Req->TryGetStringField(TEXT("dir"), Dir);
				Req->TryGetStringField(TEXT("file"), File);
				OutMissing.Add(Dir.IsEmpty() ? File : Dir + TEXT("/") + File);
			}
		}
		return true;
	}

	/** /api/checks 里 key=nodes 的 items：每项形如 "节点类（用到它的工作流、工作流）"。只留这些工作流 id 之一用到的。 */
	bool FindMissingNodes(const FString& Body, const TSet<FString>& WorkflowIds, TArray<FString>& OutMissing)
	{
		const TSharedPtr<FJsonObject> Root = ParsePipelineJson(Body);
		const TArray<TSharedPtr<FJsonValue>>* Checks = nullptr;
		if (!Root.IsValid() || !Root->TryGetArrayField(TEXT("checks"), Checks) || Checks == nullptr)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Checks)
		{
			const TSharedPtr<FJsonObject> Check = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Key;
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (!Check.IsValid() || !Check->TryGetStringField(TEXT("key"), Key) || Key != TEXT("nodes")
				|| !Check->TryGetArrayField(TEXT("items"), Items) || Items == nullptr)
			{
				continue;
			}

			for (const TSharedPtr<FJsonValue>& Item : *Items)
			{
				FString Text;
				if (!Item.IsValid() || !Item->TryGetString(Text))
				{
					continue;
				}
				FString Class;
				FString Users;
				if (!Text.Split(TEXT("（"), &Class, &Users))
				{
					continue;
				}
				Users.RemoveFromEnd(TEXT("）"));
				TArray<FString> UserList;
				Users.ParseIntoArray(UserList, TEXT("、"));
				for (const FString& User : UserList)
				{
					if (WorkflowIds.Contains(User.TrimStartAndEnd()))
					{
						OutMissing.Add(Class);
						break;
					}
				}
			}
		}
		return true;
	}

	/** 一半（一条管线）。 */
	struct FPipelineHalf
	{
		// ---- 配置 ----
		FString Label;
		FString SourceId;
		FString DecomposeId;
		FString Prompt;
		bool bExpectNonCommercial = false;
		double JobTimeoutSecs = 300.0;

		// ---- 状态 ----
		/** 环境缺东西（模型）：整半跳过。 */
		bool bSkip = false;

		/** 这一半已经失败（提交被拒、超时、任务失败……）：后面的步骤不再做。 */
		bool bFailed = false;

		FString WorkflowId;
		int32 Resolution = 0;
		int64 Seed = 0;
		FString JobId;
		FString MaterialId;
		FString LastStage;

		/** 提交前的批次 id：页面模型上一批的行还在，要等批次 id 变了才是这一次提交的结果。 */
		FString PrevBatchId;

		double SubmitAt = 0.0;
		double JobDoneAt = 0.0;
		double ImportDoneAt = 0.0;

		FString FinalSummary;
	};

	struct FPipelineState
	{
		FString URL;

		// ---- 测试期间改掉的设置（内存里的 CDO，不写 ini），结束时还原 ----
		FString SavedBackendURL;
		FString SavedImportRoot;
		bool bSavedAutoImport = false;
		bool bSavedCreateMI = true;
		bool bSettingsOverridden = false;
		bool bBackendURLChanged = false;

		/** 唯一的测试导入目录，测完连包带文件清掉。 */
		FString Root = TEXT("/Game/DreamTextureTests/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);

		TSharedPtr<FDreamTextureConnection> Conn;
		TSharedPtr<FDreamTextureJobTracker> Tracker;
		TSharedPtr<FDreamTextureGenerateModel> Model;
		FDelegateHandle JobHandle;

		/** 前面出了没法继续的问题（后端不通、目录读不到……）：所有后续步骤跳过，收尾照常。 */
		bool bAbort = false;

		TSharedRef<FPipelineHttpResult> ModelsHttp = MakeShared<FPipelineHttpResult>();
		TSharedRef<FPipelineHttpResult> ChecksHttp = MakeShared<FPipelineHttpResult>();

		TArray<FPipelineHalf> Halves;

		/** 当前半的下标，给任务事件的日志用。 */
		int32 CurrentHalf = INDEX_NONE;
		FAutomationTestBase* Test = nullptr;

		bool IsActive(int32 Index) const
		{
			return !bAbort && Halves.IsValidIndex(Index) && !Halves[Index].bSkip && !Halves[Index].bFailed;
		}

		const FDreamTextureJob* FindJob(int32 Index) const
		{
			return Tracker.IsValid() && Halves.IsValidIndex(Index) && !Halves[Index].JobId.IsEmpty() ? Tracker->Find(Halves[Index].JobId) : nullptr;
		}

		const FDreamTextureImportStatus* FindImport(int32 Index) const
		{
			return Halves.IsValidIndex(Index) && !Halves[Index].MaterialId.IsEmpty()
				? FDreamTextureImportQueue::Get().FindStatus(Halves[Index].MaterialId) : nullptr;
		}

		void CancelUnfinished()
		{
			if (!Tracker.IsValid())
			{
				return;
			}
			for (const FPipelineHalf& Half : Halves)
			{
				if (Half.JobId.IsEmpty())
				{
					continue;
				}
				const FDreamTextureJob* Job = Tracker->Find(Half.JobId);
				if (Job != nullptr && !Job->IsTerminal())
				{
					Tracker->Cancel(Half.JobId);
				}
			}
		}

		bool AllJobsTerminal() const
		{
			if (!Tracker.IsValid())
			{
				return true;
			}
			for (const FPipelineHalf& Half : Halves)
			{
				if (Half.JobId.IsEmpty())
				{
					continue;
				}
				const FDreamTextureJob* Job = Tracker->Find(Half.JobId);
				if (Job != nullptr && !Job->IsTerminal())
				{
					return false;
				}
			}
			return true;
		}

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

		/** 摘订阅、还原设置、删测试资产。可以重复调；测试中途崩溃（析构）时也走它。 */
		void End()
		{
			if (Tracker.IsValid() && JobHandle.IsValid())
			{
				Tracker->OnJobChanged.Remove(JobHandle);
				JobHandle.Reset();
			}
			Model.Reset();

			if (bSettingsOverridden)
			{
				UDreamTextureSettings* Settings = GetMutableDefault<UDreamTextureSettings>();
				Settings->ImportRoot = SavedImportRoot;
				Settings->bAutoImportGenerated = bSavedAutoImport;
				Settings->bCreateMaterialInstance = bSavedCreateMI;
				bSettingsOverridden = false;
			}
			if (bBackendURLChanged)
			{
				GetMutableDefault<UDreamTextureSettings>()->BackendURL = SavedBackendURL;
				bBackendURLChanged = false;
				if (Conn.IsValid())
				{
					Conn->Reconnect();
				}
			}

			FDreamTextureImportQueue::Get().Shutdown();
			Cleanup();
		}

		~FPipelineState()
		{
			End();
		}
	};

	constexpr int64 PipelineSeed = 20261012;

	/** 导入完成后逐项核对：贴图、材质实例、父材质、参数指向、许可。 */
	void VerifyImported(FAutomationTestBase* Test, FPipelineState& S, int32 Index)
	{
		FPipelineHalf& Half = S.Halves[Index];
		const FDreamTextureImportStatus* Status = S.FindImport(Index);
		const FDreamTextureJob* Job = S.FindJob(Index);
		if (!Test->TestNotNull(TEXT("有导入状态"), Status) || !Test->TestNotNull(TEXT("有任务"), Job))
		{
			Half.bFailed = true;
			return;
		}

		const FString Prefix = FString::Printf(TEXT("[%s] "), *Half.Label);
		const FDreamTextureImportResult& Result = Status->Result;

		if (!Test->TestTrue(*(Prefix + FString::Printf(TEXT("导入成功（阶段 %d，说明：%s，错误：%s）"),
			(int32)Status->Stage, *Status->Message, *Result.Error)), Status->Stage == EDreamTextureImportStage::Succeeded && Result.bOK))
		{
			Half.bFailed = true;
			return;
		}
		Test->AddInfo(Prefix + TEXT("导入队列的说法：") + Status->Message);

		// ---- 落点：测试目录下，目录名由「展示名 + 材质 id」决定（后端 manifest 的 name 要等于任务的展示名）----
		Test->TestTrue(*(Prefix + TEXT("资产目录在测试 ImportRoot 之下")), Result.AssetFolder.StartsWith(S.Root + TEXT("/")));
		FDreamTextureManifest NameProbe;
		NameProbe.Name = Job->Name;
		NameProbe.Id = Job->MaterialId;
		const FString Slug = FDreamTextureImporter::MakeSlug(NameProbe);
		Test->TestEqual(*(Prefix + TEXT("资产目录 = ImportRoot/<展示名+id 拼出的 slug>")), Result.AssetFolder, S.Root / Slug);
		Test->TestTrue(*(Prefix + TEXT("目录名里带着任务的材质 id（manifest 的 id 就是任务的材质 id）")), Result.AssetFolder.EndsWith(Slug));

		// ---- 贴图 ----
		Test->TestTrue(*(Prefix + TEXT("导入了贴图")), Result.Textures.Num() > 0);
		TMap<FString, UTexture2D*> ByChannel;
		for (const FDreamTextureImportedTexture& Tex : Result.Textures)
		{
			Test->TestTrue(*(Prefix + FString::Printf(TEXT("通道 %s 新建（空目录里不会是更新或未变；说明：%s）"), *Tex.Channel, *Tex.Note)),
				Tex.Action == EDreamTextureImportAction::Created);
			UTexture2D* Loaded = LoadObject<UTexture2D>(nullptr, *Tex.ObjectPath);
			if (Test->TestNotNull(*(Prefix + FString::Printf(TEXT("通道 %s 的贴图能加载：%s"), *Tex.Channel, *Tex.ObjectPath)), Loaded))
			{
				Test->TestTrue(*(Prefix + FString::Printf(TEXT("通道 %s 的贴图在资产目录里"), *Tex.Channel)), Tex.ObjectPath.StartsWith(Result.AssetFolder + TEXT("/")));
				Test->TestTrue(*(Prefix + FString::Printf(TEXT("通道 %s 的贴图有尺寸"), *Tex.Channel)), Loaded->Source.GetSizeX() > 0 && Loaded->Source.GetSizeY() > 0);
				ByChannel.Add(Tex.Channel, Loaded);
			}
		}
		for (const TCHAR* Channel : { TEXT("basecolor"), TEXT("normal"), TEXT("orm") })
		{
			Test->TestTrue(*(Prefix + FString::Printf(TEXT("有 %s 贴图"), Channel)), ByChannel.Contains(Channel));
		}

		// ---- 材质实例 ----
		const FString ExpectedMIPath = FString::Printf(TEXT("%s/%s.%s"), *Result.AssetFolder,
			*FDreamTextureMaterialBuilder::GetInstanceAssetName(Slug), *FDreamTextureMaterialBuilder::GetInstanceAssetName(Slug));
		Test->TestTrue(*(Prefix + FString::Printf(TEXT("材质实例新建（说明：%s）"), *Result.MaterialInstance.Note)),
			Result.MaterialInstance.Action == EDreamTextureImportAction::Created);
		Test->TestEqual(*(Prefix + TEXT("材质实例叫 MI_<名字>，与贴图同目录")), Result.MaterialInstance.ObjectPath, ExpectedMIPath);

		UMaterialInstanceConstant* MI = LoadObject<UMaterialInstanceConstant>(nullptr, *Result.MaterialInstance.ObjectPath);
		if (Test->TestNotNull(*(Prefix + TEXT("材质实例能加载")), MI))
		{
			const FString MasterPath = FDreamTextureMasterMaterial::GetObjectPath(S.Root);
			const UMaterialInterface* Parent = MI->Parent;
			if (Test->TestNotNull(*(Prefix + TEXT("材质实例有父材质")), Parent))
			{
				Test->TestEqual(*(Prefix + TEXT("父材质是 M_DreamTexture_Master")), Parent->GetName(), FString(FDreamTextureMasterMaterial::GetAssetName()));
				Test->TestEqual(*(Prefix + TEXT("父材质生成在测试 ImportRoot 下")), Parent->GetPathName(), MasterPath);
				Test->TestTrue(*(Prefix + TEXT("父材质是 UMaterial")), Cast<UMaterial>(const_cast<UMaterialInterface*>(Parent)) != nullptr);
			}

			const struct { FName Param; const TCHAR* Channel; } Params[] = {
				{ DreamTextureMaterialParam::BaseColor, TEXT("basecolor") },
				{ DreamTextureMaterialParam::Normal, TEXT("normal") },
				{ DreamTextureMaterialParam::ORM, TEXT("orm") },
			};
			for (const auto& Pair : Params)
			{
				// 只看实例自己覆盖的值（MaterialEditor 模块的 UMaterialEditingLibrary 不在本模块的依赖里，直接问实例）。
				UTexture* Actual = nullptr;
				MI->GetTextureParameterValue(FHashedMaterialParameterInfo(Pair.Param), Actual, /*bOveriddenOnly*/ true);
				UTexture2D* const* Expected = ByChannel.Find(Pair.Channel);
				if (Test->TestNotNull(*(Prefix + FString::Printf(TEXT("参数 %s 有贴图"), *Pair.Param.ToString())), Actual) && Expected != nullptr)
				{
					Test->TestTrue(*(Prefix + FString::Printf(TEXT("参数 %s 指向刚导入的 %s 贴图（实际 %s）"), *Pair.Param.ToString(), Pair.Channel, *Actual->GetPathName())),
						Actual == *Expected);
				}
			}
		}

		// ---- 许可：manifest 的 license_flags 要体现在导入结果与通知文案里 ----
		Test->AddInfo(Prefix + FString::Printf(TEXT("manifest 的许可：%s%s"),
			Result.License.bKnown ? (Result.License.bCommercialUse ? TEXT("commercial_use=true") : TEXT("commercial_use=false")) : TEXT("没有 license_flags"),
			Result.License.Reason.IsEmpty() ? TEXT("") : *(TEXT("，原因：") + Result.License.Reason)));
		if (Half.bExpectNonCommercial)
		{
			Test->TestTrue(*(Prefix + TEXT("导入结果里知道许可（manifest 带了 license_flags）")), Result.License.bKnown);
			Test->TestFalse(*(Prefix + TEXT("commercial_use 应为 false")), Result.License.bCommercialUse);
			Test->TestFalse(*(Prefix + TEXT("原因不是空的")), Result.License.Reason.IsEmpty());
		}
		else
		{
			// 后端只在组合里有许可限制的组件时才写 license_flags（runner.go：len(Licenses) > 0）。传统派生与手绘 LoRA 都没有，
			// 所以这一半的 manifest 里**没有**这个字段——「没写」不是「可商用」，也不是「不可商用」，只要没被标成不可商用就对。
			Test->TestFalse(*(Prefix + TEXT("不能被标成不可商用")), Result.License.bKnown && !Result.License.bCommercialUse);
		}
		const FDreamTextureImportNotifier::FNoticeContent Notice = FDreamTextureImportNotifier::Describe(*Status);
		const FString NoticeText = Notice.Message.ToString();
		Test->AddInfo(Prefix + TEXT("通知文案：") + NoticeText.Replace(TEXT("\n"), TEXT(" | ")));
		Test->TestFalse(*(Prefix + TEXT("通知不是错误")), Notice.bIsError);
		Test->TestTrue(*(Prefix + FString::Printf(TEXT("通知标不可商用应为 %s"), Half.bExpectNonCommercial ? TEXT("是") : TEXT("否"))),
			Notice.bNonCommercial == Half.bExpectNonCommercial);
		if (Half.bExpectNonCommercial)
		{
			Test->TestTrue(*(Prefix + TEXT("通知里说了「不可商用」")), NoticeText.Contains(TEXT("Not for commercial use")));
			Test->TestTrue(*(Prefix + TEXT("通知里带着 manifest 的原因（点名 CHORD）")), NoticeText.Contains(TEXT("CHORD")));
		}
		else
		{
			Test->TestFalse(*(Prefix + TEXT("可商用的组合，通知里不提商用")), NoticeText.Contains(TEXT("commercial")));
		}

		// ---- 生成页那张卡片：完成、已导入 ----
		if (S.Model.IsValid() && S.Model->GetRows().Num() == 1)
		{
			const FDreamTextureGenerateRowView View = S.Model->GetRowView(S.Model->GetRows()[0]);
			Test->TestTrue(*(Prefix + TEXT("卡片显示完成")), View.Kind == FDreamTextureGenerateRowView::EKind::Done);
			Test->TestTrue(*(Prefix + TEXT("卡片显示已导入")), View.Import == FDreamTextureGenerateRowView::EImport::Succeeded);
		}

		// 后端出的预览图存一份到这一次运行的产物目录（和 Render 测试的预览图同一个根），给人看。
		{
			FString OutDir = FPlatformMisc::GetEnvironmentVariable(TEXT("DREAMTEXTURE_TEST_OUTPUT_DIR")).TrimStartAndEnd();
			if (OutDir.IsEmpty())
			{
				OutDir = FPaths::ProjectSavedDir() / TEXT("DreamTextureTest/Manual");
			}
			OutDir = FPaths::ConvertRelativePathToFull(OutDir) / TEXT("pipeline");
			FirePipelineDownload(FString::Printf(TEXT("%s/api/materials/%s/files/preview.png"), *S.URL, *Half.MaterialId),
				OutDir / FString::Printf(TEXT("%s_preview.png"), *Slug), Test, Prefix + TEXT("预览图"));
		}

		Half.FinalSummary = FString::Printf(TEXT("材质 %s；目录 %s；材质实例 %s；贴图 %d 张"),
			*Half.MaterialId, *Result.AssetFolder, *Result.MaterialInstance.ObjectPath, Result.Textures.Num());
	}

	/** 把一半的全部步骤排进 latent 队列。 */
	void EnqueueHalf(FAutomationTestBase* Test, const TSharedRef<FPipelineState>& S, int32 Index)
	{
		// ---- 提交 ----
		EnqueueDo([S, Test, Index]()
		{
			if (!S->IsActive(Index))
			{
				return;
			}
			FPipelineHalf& Half = S->Halves[Index];
			S->CurrentHalf = Index;
			const FString Prefix = FString::Printf(TEXT("[%s] "), *Half.Label);

			FDreamTextureGenerateModel& Model = *S->Model;
			Model.SetSourceId(Half.SourceId);
			Model.SetDecomposeId(Half.DecomposeId);
			const FDreamTextureWorkflowMeta* Workflow = Model.GetCurrentWorkflow();
			if (!Test->TestNotNull(*(Prefix + TEXT("目录里有这个组合")), Workflow))
			{
				Half.bFailed = true;
				return;
			}
			Half.WorkflowId = Workflow->Id;
			Test->TestEqual(*(Prefix + TEXT("组合工作流 id")), Half.WorkflowId, Half.SourceId + TEXT(".") + Half.DecomposeId);

			// 最小的分辨率。
			TArray<int32> Resolutions = Model.GetResolutionOptions();
			if (Resolutions.Num() > 0)
			{
				Resolutions.Sort();
				Model.SetResolution(Resolutions[0]);
			}
			Half.Resolution = Model.GetResolution();
			Model.SetPrompt(Half.Prompt);
			Model.SetVariants(1);
			Model.SetRandomSeed(false);
			Model.SetFixedSeed(PipelineSeed);
			Half.Seed = Model.GetFixedSeed();

			// 提交前的许可提示（生成页按钮旁边那条）：不可商用的组合必须亮。
			const FDreamTextureGenerateLicenseHint Hint = Model.GetLicenseHint();
			Test->TestTrue(*(Prefix + FString::Printf(TEXT("提交前的许可提示：不可商用应为 %s（%s）"), Half.bExpectNonCommercial ? TEXT("是") : TEXT("否"), *Hint.Reason)),
				Hint.bNonCommercial == Half.bExpectNonCommercial);

			FText Why;
			if (!Test->TestTrue(*(Prefix + FString::Printf(TEXT("现在能提交（%s）"), *Why.ToString())), Model.CanSubmit(&Why)))
			{
				Test->AddError(*(Prefix + TEXT("不能提交的原因：") + Why.ToString()));
				Half.bFailed = true;
				return;
			}

			FDreamTextureGenerateRequest Request;
			if (Model.BuildRequest(Request))
			{
				FString Body;
				FString Error;
				if (Request.ToJsonString(Body, Error))
				{
					Test->AddInfo(Prefix + TEXT("请求体：") + Body);
				}
			}
			Test->AddInfo(Prefix + FString::Printf(TEXT("提交：%s，分辨率 %d，种子 %lld，1 个变体"), *Half.WorkflowId, Half.Resolution, Half.Seed));

			Half.SubmitAt = FPlatformTime::Seconds();
			Half.PrevBatchId = Model.GetBatchId();
			Model.Submit();
		});
		EnqueueWait(Test, TEXT("提交的应答（新的一批出现，或给出失败原因）"), 30.0,
			[S, Index]()
			{
				return !S->IsActive(Index) || S->Model->GetBatchId() != S->Halves[Index].PrevBatchId || !S->Model->GetSubmitError().IsEmpty();
			},
			[S, Test, Index]()
			{
				if (!S->IsActive(Index))
				{
					return;
				}
				FPipelineHalf& Half = S->Halves[Index];
				const FString Prefix = FString::Printf(TEXT("[%s] "), *Half.Label);
				if (S->Model->GetBatchId() == Half.PrevBatchId || S->Model->GetRows().Num() != 1)
				{
					Test->AddError(*(Prefix + TEXT("提交被拒：") + S->Model->GetSubmitError().ToString()));
					Half.bFailed = true;
					return;
				}
				Half.JobId = S->Model->GetRows()[0]->JobId;
				for (int32 Other = 0; Other < S->Halves.Num(); ++Other)
				{
					if (Other != Index)
					{
						Test->TestTrue(*(Prefix + TEXT("是新任务，不是上一半的")), S->Halves[Other].JobId != Half.JobId);
					}
				}
				const FDreamTextureJob* Job = S->FindJob(Index);
				if (!Test->TestNotNull(*(Prefix + TEXT("追踪器里有这个任务")), Job))
				{
					Half.bFailed = true;
					return;
				}
				Half.MaterialId = Job->MaterialId;
				Test->TestEqual(*(Prefix + TEXT("工作流是组合 id")), Job->WorkflowId, Half.WorkflowId);
				Test->TestTrue(*(Prefix + TEXT("种子原样往返")), Job->bHasSeed && Job->Seed == Half.Seed);
				Test->TestEqual(*(Prefix + TEXT("分辨率")), Job->Resolution, Half.Resolution);
				Test->AddInfo(Prefix + FString::Printf(TEXT("任务 %s，材质 %s"), *Job->Id, *Job->MaterialId));
			},
			[S, Index]() { if (S->Halves.IsValidIndex(Index)) { S->Halves[Index].bFailed = true; } });

		// ---- 等任务结束（超时先取消）----
		EnqueueWait(Test, FString::Printf(TEXT("任务结束（%s）"), *S->Halves[Index].Label), S->Halves[Index].JobTimeoutSecs,
			[S, Index]()
			{
				if (!S->IsActive(Index))
				{
					return true;
				}
				const FDreamTextureJob* Job = S->FindJob(Index);
				return Job == nullptr || Job->IsTerminal();
			},
			[S, Test, Index]()
			{
				if (!S->IsActive(Index))
				{
					return;
				}
				FPipelineHalf& Half = S->Halves[Index];
				const FString Prefix = FString::Printf(TEXT("[%s] "), *Half.Label);
				Half.JobDoneAt = FPlatformTime::Seconds();
				const FDreamTextureJob* Job = S->FindJob(Index);
				if (Job == nullptr)
				{
					Half.bFailed = true;
					return;
				}
				Test->AddInfo(Prefix + FString::Printf(TEXT("任务结束：%s，用时 %.1f 秒"), LexToString(Job->Status), Half.JobDoneAt - Half.SubmitAt));
				if (Job->Status != EDreamTextureJobStatus::Succeeded)
				{
					Test->AddError(*(Prefix + FString::Printf(TEXT("任务没成功：%s，阶段「%s」，原因：%s"), LexToString(Job->Status), *Job->Stage, *Job->Error)));
					Half.bFailed = true;
					return;
				}
				Test->TestEqual(*(Prefix + TEXT("进度 1")), Job->Progress, 1.0f);
			},
			[S, Test, Index]()
			{
				// 超时：先取消，别把后端唯一的 worker 留在这个任务里；这一半判失败。
				if (!S->Halves.IsValidIndex(Index))
				{
					return;
				}
				FPipelineHalf& Half = S->Halves[Index];
				Half.bFailed = true;
				if (const FDreamTextureJob* Job = S->FindJob(Index))
				{
					Test->AddInfo(FString::Printf(TEXT("[%s] 超时时任务停在：%s，阶段「%s」，进度 %.2f；现在取消它"),
						*Half.Label, LexToString(Job->Status), *Job->Stage, Job->Progress));
					if (!Job->IsTerminal())
					{
						S->Tracker->Cancel(Half.JobId);
					}
				}
			});
		// 超时取消之后给它一点时间进终态。
		EnqueueWait(Test, TEXT("超时取消后任务进入终态"), 45.0,
			[S, Index]()
			{
				const FDreamTextureJob* Job = S->FindJob(Index);
				return Job == nullptr || Job->IsTerminal();
			});

		// ---- 自动导入：先要有状态（说明真的触发了），再等它结束 ----
		EnqueueWait(Test, TEXT("自动导入被触发（导入队列里出现这个材质的状态）。任务成功了却没有导入，说明追踪器 -> 自动导入这一段没通"), 30.0,
			[S, Index]() { return !S->IsActive(Index) || S->FindImport(Index) != nullptr; },
			nullptr,
			[S, Index]() { if (S->Halves.IsValidIndex(Index)) { S->Halves[Index].bFailed = true; } });
		EnqueueWait(Test, TEXT("导入结束"), 150.0,
			[S, Index]()
			{
				if (!S->IsActive(Index))
				{
					return true;
				}
				const FDreamTextureImportStatus* Status = S->FindImport(Index);
				return Status == nullptr || Status->IsFinished();
			},
			[S, Test, Index]()
			{
				if (!S->IsActive(Index))
				{
					return;
				}
				FPipelineHalf& Half = S->Halves[Index];
				Half.ImportDoneAt = FPlatformTime::Seconds();
				Test->AddInfo(FString::Printf(TEXT("[%s] 导入结束，用时 %.1f 秒（任务结束后）"), *Half.Label, Half.ImportDoneAt - Half.JobDoneAt));
				VerifyImported(Test, *S, Index);
			},
			[S, Test, Index]()
			{
				if (!S->Halves.IsValidIndex(Index))
				{
					return;
				}
				S->Halves[Index].bFailed = true;
				if (const FDreamTextureImportStatus* Status = S->FindImport(Index))
				{
					Test->AddInfo(FString::Printf(TEXT("[%s] 超时时导入停在阶段 %d：%s"), *S->Halves[Index].Label, (int32)Status->Stage, *Status->Message));
				}
			});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTexturePipelineGenerateTest, "DreamTexture.Pipeline.Generate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTexturePipelineGenerateTest::RunTest(const FString& Parameters)
{
	const FString PipelineURL = FPlatformMisc::GetEnvironmentVariable(TEXT("DREAMTEXTURE_PIPELINE_URL")).TrimStartAndEnd();
	if (PipelineURL.IsEmpty())
	{
		AddInfo(TEXT("已跳过：没有设置环境变量 DREAMTEXTURE_PIPELINE_URL。这个测试要真的 ComfyUI：先 "
			"pwsh -NoProfile -File Tools/Test.ps1 -Pipeline -Filter DreamTexture.Pipeline.Generate，它会起隔离的 ComfyUI + 新后端并设好这个变量（吃显存，空闲不足 10 GiB 会拒绝）。"));
		return true;
	}
	if (!FDreamTextureClientModule::IsAvailable())
	{
		AddError(TEXT("客户端模块没有加载"));
		return false;
	}

	FAutomationTestBase* Test = this;
	const TSharedRef<FPipelineState> S = MakeShared<FPipelineState>();
	S->URL = PipelineURL;
	S->Test = this;

	{
		FPipelineHalf Derive;
		Derive.Label = TEXT("手绘+传统派生");
		Derive.SourceId = TEXT("src-local-stylized-v1");
		Derive.DecomposeId = TEXT("dec-derive-v1");
		Derive.Prompt = TEXT("hand painted mossy stone wall, flat lighting");
		Derive.bExpectNonCommercial = false;
		// 冷启动要加载 SDXL（几十秒），采样本身最小分辨率下十几秒；给足。
		Derive.JobTimeoutSecs = 300.0;
		S->Halves.Add(MoveTemp(Derive));

		FPipelineHalf Chord;
		Chord.Label = TEXT("写实+CHORD");
		Chord.SourceId = TEXT("src-local-realistic-v1");
		Chord.DecomposeId = TEXT("dec-chord-v1");
		Chord.Prompt = TEXT("mossy river rocks, photographic, top-down flat lighting");
		Chord.bExpectNonCommercial = true;
		// 换工作流时 ComfyUI 先卸掉上一条管线的模型，再加载 SDXL 和 CHORD。
		Chord.JobTimeoutSecs = 420.0;
		S->Halves.Add(MoveTemp(Chord));
	}

	// ---- 1. 改设置、起页面模型 ----
	EnqueueDo([S, Test]()
	{
		UDreamTextureSettings* Settings = GetMutableDefault<UDreamTextureSettings>();
		S->SavedImportRoot = Settings->ImportRoot;
		S->bSavedAutoImport = Settings->bAutoImportGenerated;
		S->bSavedCreateMI = Settings->bCreateMaterialInstance;
		Settings->ImportRoot = S->Root;
		Settings->bAutoImportGenerated = true;
		Settings->bCreateMaterialInstance = true;
		S->bSettingsOverridden = true;

		S->Conn = FDreamTextureClientModule::Get().GetConnection();
		S->Tracker = FDreamTextureClientModule::Get().GetJobTracker();
		if (!S->Conn.IsValid() || !S->Tracker.IsValid())
		{
			Test->AddError(TEXT("客户端模块没有连接或追踪器"));
			S->bAbort = true;
			return;
		}

		// Tools/Test.ps1 -Pipeline 已经把全局连接指到流水线后端；和 -Live 同用时指的是固定数据后端，此时临时改过来。
		S->SavedBackendURL = Settings->BackendURL;
		const auto Normalize = [](FString Url)
		{
			Url.TrimStartAndEndInline();
			while (Url.RemoveFromEnd(TEXT("/"))) {}
			return Url;
		};
		if (!Normalize(Settings->BackendURL).Equals(Normalize(S->URL), ESearchCase::IgnoreCase))
		{
			Test->AddInfo(FString::Printf(TEXT("全局连接指向 %s，临时改到流水线后端 %s"), *Settings->BackendURL, *S->URL));
			Settings->BackendURL = S->URL;
			S->bBackendURLChanged = true;
			S->Conn->Reconnect();
		}

		// 记下每个任务阶段的变化，报告里能看到时间线。
		const TWeakPtr<FPipelineState> Weak = S;
		S->JobHandle = S->Tracker->OnJobChanged.AddLambda([Weak](const FDreamTextureJob& Job)
		{
			const TSharedPtr<FPipelineState> State = Weak.Pin();
			if (!State.IsValid() || State->Test == nullptr)
			{
				return;
			}
			for (FPipelineHalf& Half : State->Halves)
			{
				if (!Half.JobId.IsEmpty() && Half.JobId == Job.Id)
				{
					const FString Line = FString::Printf(TEXT("%s/%s"), LexToString(Job.Status), *Job.Stage);
					if (Line != Half.LastStage)
					{
						Half.LastStage = Line;
						State->Test->AddInfo(FString::Printf(TEXT("[%s] +%.1fs 状态：%s，进度 %.2f"), *Half.Label,
							FPlatformTime::Seconds() - Half.SubmitAt, *Line, Job.Progress));
					}
				}
			}
		});

		// 页面模型用默认的后端（真的 API、真的模块追踪器、真的导入队列）；bBindExternal=true 订阅真连接。
		S->Model = MakeShared<FDreamTextureGenerateModel>();
		S->Model->Initialize(/*bBindExternal*/ true);
	});

	// ---- 2. 链路通、ComfyUI 就绪（Start 脚本等过了，这里再确认一次）----
	EnqueueWait(Test, TEXT("链路到达 Live、且后端报告 ComfyUI 就绪。流水线后端要先起来：Tools/Test.ps1 -Pipeline，或 Tools/PipelineBackend/Start-PipelineBackend.ps1"), 90.0,
		[S]() { return S->bAbort || (S->Conn->GetLink() == EDreamTextureLink::Live && S->Conn->GetHealth().bReady); },
		[S, Test]()
		{
			if (!S->bAbort)
			{
				const FDreamTextureHealth& Health = S->Conn->GetHealth();
				Test->AddInfo(FString::Printf(TEXT("ComfyUI：%s，设备 %s，显存空闲 %lld / %lld MB，队列 %d"),
					*Health.ComfyUIVersion, *Health.Device, Health.VRAMFreeMB, Health.VRAMTotalMB, Health.QueueDepth));
			}
		},
		[S]() { S->bAbort = true; });

	// ---- 3. 工作流目录 ----
	EnqueueDo([S]()
	{
		if (!S->bAbort)
		{
			S->Model->LoadCatalog();
		}
	});
	EnqueueWait(Test, TEXT("工作流目录读回来"), 30.0,
		[S]() { return S->bAbort || S->Model->GetCatalogState() != FDreamTextureGenerateModel::ECatalogState::Loading; },
		[S, Test]()
		{
			if (S->bAbort)
			{
				return;
			}
			if (S->Model->GetCatalogState() != FDreamTextureGenerateModel::ECatalogState::Ready)
			{
				Test->AddError(FString::Printf(TEXT("读取工作流目录失败：%s"), *S->Model->GetCatalogError()));
				S->bAbort = true;
				return;
			}
			Test->AddInfo(FString::Printf(TEXT("目录：%d 个出图模型 × %d 个分解模型"),
				S->Model->GetSourceOptions().Num(), S->Model->GetDecomposeOptions().Num()));
		},
		[S]() { S->bAbort = true; });

	// ---- 4. 缺什么：/api/models 逐工作流的模型清单 + /api/checks 的节点与模型结论 ----
	EnqueueDo([S]()
	{
		if (S->bAbort)
		{
			return;
		}
		FirePipelineGet(S->URL, TEXT("/api/models"), S->ModelsHttp);
		FirePipelineGet(S->URL, TEXT("/api/checks"), S->ChecksHttp);
	});
	EnqueueWait(Test, TEXT("/api/models 与 /api/checks 的应答"), 30.0,
		[S]() { return S->bAbort || (S->ModelsHttp->bDone && S->ChecksHttp->bDone); },
		[S, Test]()
		{
			if (S->bAbort)
			{
				return;
			}

			// /api/checks 的结论先打进日志，报告里看得见这台机器缺不缺东西。
			if (const TSharedPtr<FJsonObject> Checks = ParsePipelineJson(S->ChecksHttp->Body))
			{
				const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
				if (Checks->TryGetArrayField(TEXT("checks"), List) && List != nullptr)
				{
					for (const TSharedPtr<FJsonValue>& Value : *List)
					{
						const TSharedPtr<FJsonObject> Check = Value.IsValid() ? Value->AsObject() : nullptr;
						if (!Check.IsValid())
						{
							continue;
						}
						FString Key, Status, Detail;
						Check->TryGetStringField(TEXT("key"), Key);
						Check->TryGetStringField(TEXT("status"), Status);
						Check->TryGetStringField(TEXT("detail"), Detail);
						TArray<FString> Items;
						const TArray<TSharedPtr<FJsonValue>>* ItemValues = nullptr;
						if (Check->TryGetArrayField(TEXT("items"), ItemValues) && ItemValues != nullptr)
						{
							for (const TSharedPtr<FJsonValue>& Item : *ItemValues)
							{
								FString Text;
								if (Item.IsValid() && Item->TryGetString(Text))
								{
									Items.Add(Text);
								}
							}
						}
						Test->AddInfo(FString::Printf(TEXT("/api/checks %s：%s %s%s"), *Key, *Status, *Detail,
							Items.Num() > 0 ? *(TEXT("；条目：") + FString::Join(Items, TEXT("，"))) : TEXT("")));
					}
				}
			}
			else
			{
				Test->AddInfo(FString::Printf(TEXT("/api/checks 没读到（HTTP %d），不据此跳过"), S->ChecksHttp->Code));
			}

			for (FPipelineHalf& Half : S->Halves)
			{
				const FString Prefix = FString::Printf(TEXT("[%s] "), *Half.Label);
				const FString Composed = Half.SourceId + TEXT(".") + Half.DecomposeId;
				const TSet<FString> Ids = { Composed, Half.SourceId, Half.DecomposeId };

				const bool bInCatalog = S->Model->GetCatalog().FindWorkflow(Composed) != nullptr;
				if (!bInCatalog)
				{
					Test->AddError(*(Prefix + FString::Printf(TEXT("后端目录里没有 %s：这个后端不是带「工作流分段」的新后端，或工作流文件缺失"), *Composed)));
					Half.bFailed = true;
					continue;
				}

				TArray<FString> MissingModels;
				if (!FindMissingModels(S->ModelsHttp->Body, Ids, MissingModels))
				{
					Test->AddInfo(Prefix + FString::Printf(TEXT("/api/models 没读到（HTTP %d），不据此跳过"), S->ModelsHttp->Code));
				}
				else if (MissingModels.Num() > 0)
				{
					Half.bSkip = true;
					Test->AddInfo(Prefix + TEXT("已跳过：这台机器缺这条管线要的模型：") + FString::Join(MissingModels, TEXT("，")));
					continue;
				}

				// 缺节点不跳过：隔离的 ComfyUI 本来就该装齐这条管线的节点，缺了是环境故障，要让人看见。
				TArray<FString> MissingNodes;
				if (FindMissingNodes(S->ChecksHttp->Body, Ids, MissingNodes) && MissingNodes.Num() > 0)
				{
					Test->AddError(*(Prefix + TEXT("ComfyUI 里没有这条管线要的节点：") + FString::Join(MissingNodes, TEXT("，"))));
					Half.bFailed = true;
				}
			}
		},
		[S]() { S->bAbort = true; });

	// ---- 5. 两半 ----
	EnqueueHalf(Test, S, 0);
	EnqueueHalf(Test, S, 1);

	// ---- 6. 收尾：没结束的任务一律取消，等进终态，再还原设置、删测试资产 ----
	EnqueueDo([S]()
	{
		S->CancelUnfinished();
	});
	EnqueueWait(Test, TEXT("收尾：所有任务进入终态（不能把后端唯一的 worker 留在生成里）"), 60.0,
		[S]() { return S->AllJobsTerminal(); });
	EnqueueDo([S, Test]()
	{
		for (const FPipelineHalf& Half : S->Halves)
		{
			if (Half.bSkip)
			{
				continue;
			}
			if (!Half.FinalSummary.IsEmpty())
			{
				Test->AddInfo(FString::Printf(TEXT("[%s] 结果：任务用时 %.1f 秒，导入用时 %.1f 秒；%s"), *Half.Label,
					Half.JobDoneAt - Half.SubmitAt, Half.ImportDoneAt - Half.JobDoneAt, *Half.FinalSummary));
			}
		}
		S->End();
		Test->AddInfo(TEXT("已还原设置（ImportRoot、自动导入开关、材质实例开关）并删除测试资产"));
	});

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
