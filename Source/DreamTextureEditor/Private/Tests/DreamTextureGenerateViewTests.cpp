// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureAutoImporter.h"
#include "DreamTextureConnection.h"
#include "DreamTextureGenerateModel.h"
#include "DreamTextureJobTracker.h"
#include "Framework/Application/SlateApplication.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SDreamTextureGeneratePanel.h"

/*
 * 生成页：页面模型的异步分支、一张变体卡片在各状态下怎么说、三级状态怎么挡提交、自动导入、面板构造不崩。
 *
 * 全部不连后端：模型的取数函数换成假的，追踪器的网络层也是假的，回调攥在手里，
 * 由测试决定什么时候、以什么顺序回。这样「慢的旧目录晚于新的回来」「提交的应答回来之前面板就关了」
 * 这类真实世界里偶发的时序，在这里可以稳定复现。
 * 目录用 Tests/Data/Generate/workflows.json（从真实 /api/workflows 的应答里截出来的）。
 */

namespace
{
	FString GenViewPluginDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamTextureForUnreal"));
		return Plugin.IsValid() ? Plugin->GetBaseDir() : FString();
	}

	bool LoadCatalogFixture(FAutomationTestBase& Test, FDreamTextureWorkflowCatalog& Out, const TCHAR* FileName = TEXT("workflows.json"))
	{
		const FString Path = FPaths::Combine(GenViewPluginDir(), TEXT("Tests"), TEXT("Data"), TEXT("Generate"), FileName);
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

	/** 连接的假身：基类把状态字段留给子类可写，getter 都是非虚的，直接读成员。 */
	class FFakeConnection : public FDreamTextureConnection
	{
	public:
		void Set(EDreamTextureLink InLink, bool bReachable, const FString& InError = FString())
		{
			Link = InLink;
			bBackendReachable = bReachable;
			LastError = InError;
		}
		void SetHealth(const FDreamTextureHealth& InHealth) { Health = InHealth; }

		/** 三级都正常：后端在、长连接通、ComfyUI 就绪。 */
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

	FDreamTextureJob GenViewMakeJob(const FString& Id, EDreamTextureJobStatus Status, float Progress = 0.0f, const TCHAR* Batch = TEXT("bA"))
	{
		FDreamTextureJob Job;
		Job.Id = Id;
		Job.MaterialId = TEXT("mat_") + Id;
		Job.WorkflowId = TEXT("src-local-realistic-v1.dec-chord-v1");
		Job.BatchId = Batch;
		Job.Status = Status;
		Job.Progress = Progress;
		Job.bHasSeed = true;
		Job.Seed = 42;
		Job.Resolution = 1024;
		return Job;
	}

	/** 假后端：回调攥在手里，测试想什么时候回就什么时候回。 */
	struct FFakeBackend
	{
		struct FGet
		{
			FString Id;
			FDreamTextureGenerateApi::FOnJob Done;
		};
		struct FRefineCall
		{
			FString Prompt;
			EDreamTextureRefinePurpose Purpose = EDreamTextureRefinePurpose::Texture;
			FString Model;
			FDreamTextureGenerateApi::FOnRefined Done;
		};

		// 模型自己的后端
		TArray<FDreamTextureGenerateApi::FOnCatalog> CatalogCalls;
		TArray<FDreamTextureGenerateApi::FOnRefineModels> ModelCalls;
		TArray<FDreamTextureGenerateApi::FOnImagenProviders> CloudCalls;

		// 假的进程管理器：EnsureBackend 的回调攥在手里，由测试决定什么时候、成功还是失败。
		TArray<FDreamTextureProcess::FEnsureDone> EnsureCalls;
		bool bHasExecutable = true;
		TArray<FRefineCall> RefineCalls;
		TArray<FString> Enqueued;
		bool bEnqueueResult = true;
		TMap<FString, FDreamTextureImportStatus> ImportStatuses;
		bool bAutoImport = false;

		// 追踪器的网络层
		TArray<FGet> Gets;
		TArray<FDreamTextureGenerateRequest> SubmitRequests;
		TArray<FDreamTextureGenerateApi::FOnSubmitted> SubmitDones;
		TArray<FString> CancelIds;
		TArray<FDreamTextureGenerateApi::FOnDone> CancelDones;

		TSharedRef<FFakeConnection> Conn = MakeShared<FFakeConnection>();
		TSharedRef<FDreamTextureJobTracker> Tracker;

		FFakeBackend()
			: Tracker(MakeShared<FDreamTextureJobTracker>(MakeHooks()))
		{
			Conn->SetAllReady();
		}

		FDreamTextureJobTracker::FHooks MakeHooks()
		{
			FDreamTextureJobTracker::FHooks Out;
			Out.GetJob = [this](const FString& Id, FDreamTextureGenerateApi::FOnJob Done)
			{
				Gets.Add({ Id, MoveTemp(Done) });
			};
			Out.Submit = [this](const FDreamTextureGenerateRequest& Request, FDreamTextureGenerateApi::FOnSubmitted Done)
			{
				SubmitRequests.Add(Request);
				SubmitDones.Add(MoveTemp(Done));
			};
			Out.Cancel = [this](const FString& Id, FDreamTextureGenerateApi::FOnDone Done)
			{
				CancelIds.Add(Id);
				CancelDones.Add(MoveTemp(Done));
			};
			return Out;
		}

		FDreamTextureGenerateBackend Make()
		{
			FDreamTextureGenerateBackend Out;
			Out.ListWorkflows = [this](FDreamTextureGenerateApi::FOnCatalog Done)
			{
				CatalogCalls.Add(MoveTemp(Done));
			};
			Out.ListRefineModels = [this](FDreamTextureGenerateApi::FOnRefineModels Done)
			{
				ModelCalls.Add(MoveTemp(Done));
			};
			Out.ListImagenProviders = [this](FDreamTextureGenerateApi::FOnImagenProviders Done)
			{
				CloudCalls.Add(MoveTemp(Done));
			};
			Out.EnsureBackend = [this](FDreamTextureProcess::FEnsureDone Done)
			{
				EnsureCalls.Add(MoveTemp(Done));
			};
			Out.HasBackendExecutable = [this]() { return bHasExecutable; };
			Out.Refine = [this](const FString& Prompt, EDreamTextureRefinePurpose Purpose, const FString& Model, FDreamTextureGenerateApi::FOnRefined Done)
			{
				RefineCalls.Add({ Prompt, Purpose, Model, MoveTemp(Done) });
			};
			Out.Enqueue = [this](const FString& MaterialId)
			{
				Enqueued.Add(MaterialId);
				return bEnqueueResult;
			};
			Out.FindImportStatus = [this](const FString& MaterialId) -> const FDreamTextureImportStatus*
			{
				return ImportStatuses.Find(MaterialId);
			};
			Out.GetTracker = [this]() -> TSharedPtr<FDreamTextureJobTracker>
			{
				return Tracker;
			};
			Out.GetConnection = [this]() -> TSharedPtr<FDreamTextureConnection>
			{
				return Conn;
			};
			Out.GetAutoImport = [this]() { return bAutoImport; };
			Out.SetAutoImport = [this](bool bAuto) { bAutoImport = bAuto; };
			return Out;
		}

		void CompleteCatalog(int32 Index, const FDreamTextureWorkflowCatalog& Catalog)
		{
			FDreamTextureWorkflowCatalog Copy = Catalog;
			CatalogCalls[Index](true, MoveTemp(Copy), FString());
		}
		void FailCatalog(int32 Index, const FString& Error)
		{
			CatalogCalls[Index](false, FDreamTextureWorkflowCatalog(), Error);
		}
		void CompleteEnsure(int32 Index)
		{
			EnsureCalls[Index](true, FString());
		}
		void FailEnsure(int32 Index, const FString& Error)
		{
			EnsureCalls[Index](false, Error);
		}
		void CompleteCloud(int32 Index, FDreamTextureImagenProviders Providers)
		{
			CloudCalls[Index](true, MoveTemp(Providers), FString());
		}
		void FailCloud(int32 Index, const FString& Error)
		{
			CloudCalls[Index](false, FDreamTextureImagenProviders(), Error);
		}
		void CompleteSubmit(int32 Index, TArray<FDreamTextureJob> Jobs)
		{
			SubmitDones[Index](true, MoveTemp(Jobs), FString());
		}
		void CompleteGet(int32 Index, const FDreamTextureJob& Job)
		{
			FDreamTextureJob Copy = Job;
			Gets[Index].Done(true, MoveTemp(Copy), FString(), false);
		}
	};

	/** 目录已加载、三级都正常、写好提示词的模型：大多数测试的起点。 */
	TSharedRef<FDreamTextureGenerateModel> MakeReadyModel(FAutomationTestBase& Test, FFakeBackend& Fake, FDreamTextureWorkflowCatalog& OutCatalog)
	{
		TSharedRef<FDreamTextureGenerateModel> Model = MakeShared<FDreamTextureGenerateModel>(Fake.Make());
		Model->Initialize(/*bBindExternal*/ false);
		LoadCatalogFixture(Test, OutCatalog);
		Model->LoadCatalog();
		Fake.CompleteCatalog(Fake.CatalogCalls.Num() - 1, OutCatalog);
		Model->SetPrompt(TEXT("长满青苔的碎裂岩石"));
		return Model;
	}

	/** 提交一批 N 个变体并让应答回来，返回任务 id。 */
	TArray<FString> SubmitBatch(FFakeBackend& Fake, FDreamTextureGenerateModel& Model, int32 Count, const TCHAR* Batch = TEXT("bA"))
	{
		Model.SetVariants(Count);
		Model.Submit();
		TArray<FDreamTextureJob> Jobs;
		TArray<FString> Ids;
		for (int32 i = 0; i < Count; ++i)
		{
			const FString Id = FString::Printf(TEXT("%s_j%d"), Batch, i + 1);
			Ids.Add(Id);
			Jobs.Add(GenViewMakeJob(Id, EDreamTextureJobStatus::Queued, 0.0f, Batch));
		}
		Fake.CompleteSubmit(Fake.SubmitDones.Num() - 1, MoveTemp(Jobs));
		return Ids;
	}
}

// ---------------------------------------------------------------------------
// 模型：目录与选择
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateModelCatalogTest, "DreamTexture.Editor.Generate.Model.Catalog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateModelCatalogTest::RunTest(const FString& Parameters)
{
	FDreamTextureWorkflowCatalog Fixture;
	if (!LoadCatalogFixture(*this, Fixture))
	{
		return false;
	}

	FFakeBackend Fake;
	TSharedRef<FDreamTextureGenerateModel> Model = MakeShared<FDreamTextureGenerateModel>(Fake.Make());
	Model->Initialize(false);

	int32 FormChanges = 0;
	Model->OnFormChanged.AddLambda([&FormChanges]() { ++FormChanges; });

	using ECatalog = FDreamTextureGenerateModel::ECatalogState;
	TestTrue(TEXT("还没请求时是未加载"), Model->GetCatalogState() == ECatalog::NotLoaded);

	Model->LoadCatalog();
	TestEqual(TEXT("发出了一次目录请求"), Fake.CatalogCalls.Num(), 1);
	TestTrue(TEXT("读取中"), Model->GetCatalogState() == ECatalog::Loading);

	Fake.CompleteCatalog(0, Fixture);
	TestTrue(TEXT("就绪"), Model->GetCatalogState() == ECatalog::Ready);
	TestTrue(TEXT("没有错误"), Model->GetCatalogError().IsEmpty());
	TestTrue(TEXT("广播了表单变化"), FormChanges >= 2);

	// 默认：出图模型优先本地（云端要令牌、按量计费，第一次打开别替用户选一个会花钱的），分解模型取排序第一个。
	TestEqual(TEXT("默认出图模型是本地写实"), Model->GetSourceId(), FString(TEXT("src-local-realistic-v1")));
	TestEqual(TEXT("默认分解模型是排序第一个"), Model->GetDecomposeId(), FString(TEXT("dec-chord-v1")));
	TestEqual(TEXT("出图模型四个选项"), Model->GetSourceOptions().Num(), 4);
	TestEqual(TEXT("分解模型两个选项"), Model->GetDecomposeOptions().Num(), 2);

	const FDreamTextureWorkflowMeta* Current = Model->GetCurrentWorkflow();
	if (TestNotNull(TEXT("组合出来的工作流找得到"), Current))
	{
		TestEqual(TEXT("工作流 id 是 <出图段>.<分解段>"), Current->Id, FString(TEXT("src-local-realistic-v1.dec-chord-v1")));
	}

	// 改选。
	Model->SetSourceId(TEXT("src-api-stylized-v1"));
	Model->SetDecomposeId(TEXT("dec-chord-v1"));
	if (TestNotNull(TEXT("改选后的组合找得到"), Model->GetCurrentWorkflow()))
	{
		TestEqual(TEXT("改选后的组合"), Model->GetCurrentWorkflow()->Id, FString(TEXT("src-api-stylized-v1.dec-chord-v1")));
	}
	const int32 ChangesBefore = FormChanges;
	Model->SetSourceId(TEXT("src-api-stylized-v1"));
	TestEqual(TEXT("选同一个不重复广播"), FormChanges, ChangesBefore);

	// 目录里没有这个组合（段是新加的、后端还没重载）：找不到工作流，提交不了，并且说清楚是哪两个。
	Model->SetSourceId(TEXT("src-api-realistic-v1"));
	Model->SetDecomposeId(TEXT("dec-chord-v1"));
	TestTrue(TEXT("没有这个组合"), Model->GetCurrentWorkflow() == nullptr);
	{
		FDreamTextureGenerateRequest Request;
		FText Error;
		TestFalse(TEXT("拼不出请求"), Model->BuildRequest(Request, &Error));
		TestTrue(TEXT("说明点出了这个组合"), Error.ToString().Contains(TEXT("src-api-realistic-v1")));
	}

	// 重新加载目录：选择保留（还在的话）。
	Model->SetSourceId(TEXT("src-local-stylized-v1"));
	Model->SetDecomposeId(TEXT("dec-derive-v1"));
	Model->LoadCatalog();
	Fake.CompleteCatalog(1, Fixture);
	TestEqual(TEXT("重载后出图模型保留"), Model->GetSourceId(), FString(TEXT("src-local-stylized-v1")));
	TestEqual(TEXT("重载后分解模型保留"), Model->GetDecomposeId(), FString(TEXT("dec-derive-v1")));

	// 新目录里选中的段没了：落回默认，不留悬空的 id。
	{
		FDreamTextureWorkflowCatalog Reduced = Fixture;
		Reduced.Segments.RemoveAll([](const FDreamTextureWorkflowMeta& Meta) { return Meta.Id == TEXT("src-local-stylized-v1"); });
		Model->LoadCatalog();
		Fake.CompleteCatalog(2, Reduced);
		TestEqual(TEXT("选中的段没了，回到默认"), Model->GetSourceId(), FString(TEXT("src-local-realistic-v1")));
	}

	// 过期的结果：连发两次，新的先回、旧的后回，旧的不能盖掉新的。
	{
		Model->LoadCatalog();
		Model->LoadCatalog();
		const int32 Newer = Fake.CatalogCalls.Num() - 1;
		Fake.CompleteCatalog(Newer, Fixture);
		Fake.FailCatalog(Newer - 1, TEXT("旧请求失败"));
		TestTrue(TEXT("旧的失败结果被丢弃"), Model->GetCatalogState() == ECatalog::Ready);
		TestTrue(TEXT("错误没被旧请求污染"), Model->GetCatalogError().IsEmpty());
	}

	// 失败：首次失败是 Failed 态，带原因；之后重试成功恢复。
	{
		FFakeBackend Fake2;
		TSharedRef<FDreamTextureGenerateModel> Model2 = MakeShared<FDreamTextureGenerateModel>(Fake2.Make());
		Model2->Initialize(false);
		Model2->LoadCatalog();
		Fake2.FailCatalog(0, TEXT("读取工作流列表失败：连不上"));
		TestTrue(TEXT("首次失败是失败态"), Model2->GetCatalogState() == ECatalog::Failed);
		TestEqual(TEXT("原因原样记下"), Model2->GetCatalogError(), FString(TEXT("读取工作流列表失败：连不上")));
		Model2->SetPrompt(TEXT("x"));
		FText Why;
		TestFalse(TEXT("目录失败时提交不了"), Model2->CanSubmit(&Why));
		TestTrue(TEXT("原因是读不到列表"), Why.ToString().Contains(TEXT("工作流列表")));

		Model2->LoadCatalog();
		TestTrue(TEXT("重试回到读取中"), Model2->GetCatalogState() == ECatalog::Loading);
		Fake2.CompleteCatalog(1, Fixture);
		TestTrue(TEXT("重试成功"), Model2->GetCatalogState() == ECatalog::Ready);
	}

	// 已有目录时刷新失败：保留旧目录，仍是就绪态——一次重连失败不该把正在用的选择清空。
	{
		Model->LoadCatalog();
		Fake.FailCatalog(Fake.CatalogCalls.Num() - 1, TEXT("超时"));
		TestTrue(TEXT("刷新失败仍是就绪态"), Model->GetCatalogState() == ECatalog::Ready);
		TestEqual(TEXT("旧目录还在"), Model->GetSourceOptions().Num(), 4);
		TestEqual(TEXT("原因记下了"), Model->GetCatalogError(), FString(TEXT("超时")));
	}

	// 模型先销毁、目录后到：不能崩。
	{
		FFakeBackend Fake3;
		{
			TSharedPtr<FDreamTextureGenerateModel> Short = MakeShared<FDreamTextureGenerateModel>(Fake3.Make());
			Short->Initialize(false);
			Short->LoadCatalog();
			Short.Reset();
		}
		Fake3.CompleteCatalog(0, Fixture);
		TestTrue(TEXT("模型销毁后回调到达没有崩溃"), true);
	}

	return true;
}

// ---------------------------------------------------------------------------
// 模型：下拉里只列生成页填得了的段
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateModelSegmentsTest, "DreamTexture.Editor.Generate.Model.Segments",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateModelSegmentsTest::RunTest(const FString& Parameters)
{
	// 新后端的目录带「已有底图」(src-upload-v1)：没有提示词，唯一的输入是必填的上传图。
	FDreamTextureWorkflowCatalog Catalog;
	if (!LoadCatalogFixture(*this, Catalog, TEXT("workflows.upload.json")))
	{
		return false;
	}

	FFakeBackend Fake;
	TSharedRef<FDreamTextureGenerateModel> Model = MakeShared<FDreamTextureGenerateModel>(Fake.Make());
	Model->Initialize(false);
	Model->LoadCatalog();
	Fake.CompleteCatalog(0, Catalog);

	TestEqual(TEXT("目录里七个段"), Model->GetCatalog().Segments.Num(), 7);
	const TArray<const FDreamTextureWorkflowMeta*> Sources = Model->GetSourceOptions();
	TestEqual(TEXT("出图模型下拉只有四个"), Sources.Num(), 4);
	for (const FDreamTextureWorkflowMeta* Meta : Sources)
	{
		TestFalse(*FString::Printf(TEXT("%s 不是已有底图"), *Meta->Id), Meta->Id == TEXT("src-upload-v1"));
	}
	TestEqual(TEXT("分解模型两个"), Model->GetDecomposeOptions().Num(), 2);
	TestEqual(TEXT("默认仍是本地写实"), Model->GetSourceId(), FString(TEXT("src-local-realistic-v1")));

	// 即便有人把选择设成了它（旧的记忆、外部代码），目录一到位也会被纠回：选了必然 400。
	{
		FFakeBackend Fake2;
		TSharedRef<FDreamTextureGenerateModel> Model2 = MakeShared<FDreamTextureGenerateModel>(Fake2.Make());
		Model2->Initialize(false);
		Model2->SetSourceId(TEXT("src-upload-v1"));
		Model2->LoadCatalog();
		Fake2.CompleteCatalog(0, Catalog);
		TestEqual(TEXT("选中的已有底图被纠回默认"), Model2->GetSourceId(), FString(TEXT("src-local-realistic-v1")));
	}

	// 面板的下拉选项也一样（选项来自模型）。
	if (FSlateApplication::IsInitialized())
	{
		TSharedPtr<SDreamTextureGeneratePanel> Panel = SNew(SDreamTextureGeneratePanel)
			.Model(Model)
			.bBindExternal(false)
			.bAutoLoad(false);
		Panel->BuildWidgetsForTest();
		TestEqual(TEXT("面板里模型给的出图段仍是四个"), Model->GetSourceOptions().Num(), 4);
	}

	return true;
}

// ---------------------------------------------------------------------------
// 模型：表单与请求
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateModelFormTest, "DreamTexture.Editor.Generate.Model.Form",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateModelFormTest::RunTest(const FString& Parameters)
{
	FFakeBackend Fake;
	FDreamTextureWorkflowCatalog Catalog;
	TSharedRef<FDreamTextureGenerateModel> Model = MakeReadyModel(*this, Fake, Catalog);

	// 分辨率：候选来自所选管线，默认值取参数默认。
	{
		const TArray<int32> Options = Model->GetResolutionOptions();
		if (TestEqual(TEXT("分辨率两个候选"), Options.Num(), 2))
		{
			TestEqual(TEXT("候选 0"), Options[0], 768);
			TestEqual(TEXT("候选 1"), Options[1], 1024);
		}
		TestEqual(TEXT("默认 1024"), Model->GetResolution(), 1024);

		Model->SetResolution(768);
		TestEqual(TEXT("选了 768"), Model->GetResolution(), 768);

		// 选的值不在候选里（换了管线之后）：回到默认，别提交一个会被后端拒掉的值。
		Model->SetResolution(512);
		TestEqual(TEXT("不在候选里回到默认"), Model->GetResolution(), 1024);
	}

	// 变体数夹在 1~8。
	Model->SetVariants(0);
	TestEqual(TEXT("下限 1"), Model->GetVariants(), 1);
	Model->SetVariants(20);
	TestEqual(TEXT("上限 8"), Model->GetVariants(), 8);
	Model->SetVariants(4);

	// 种子：界面上限与 Client 校验是同一个口径。
	TestEqual(TEXT("界面上限就是 8 个变体时的固定种子上限"),
		FDreamTextureGenerateModel::GetMaxSeedForUI(), FDreamTextureGenerateRequest::MaxFixedSeed(DreamTextureGenerateLimits::MaxVariants));
	TestTrue(TEXT("界面上限低于 2^53"), FDreamTextureGenerateModel::GetMaxSeedForUI() < static_cast<int64>(9007199254740992LL));
	Model->SetFixedSeed(-5);
	TestEqual(TEXT("下限 0"), Model->GetFixedSeed(), static_cast<int64>(0));
	Model->SetFixedSeed(MAX_int64);
	TestEqual(TEXT("填 int64 最大值被夹到界面上限"), Model->GetFixedSeed(), FDreamTextureGenerateModel::GetMaxSeedForUI());
	Model->SetFixedSeed(42);

	// 请求：随机种子。
	Model->SetPrompt(TEXT("  苔藓岩石 \n"));
	FDreamTextureGenerateRequest Request;
	if (!TestTrue(TEXT("拼得出请求"), Model->BuildRequest(Request)))
	{
		return false;
	}
	TestEqual(TEXT("工作流是组合 id"), Request.WorkflowId, FString(TEXT("src-local-realistic-v1.dec-chord-v1")));
	TestEqual(TEXT("变体数"), Request.Variants, 4);
	TestEqual(TEXT("提示词去掉首尾空白"), Request.StringParams.FindRef(TEXT("prompt")), FString(TEXT("苔藓岩石")));
	TestEqual(TEXT("展示名取自提示词"), Request.Name, FString(TEXT("苔藓岩石")));
	TestEqual(TEXT("分辨率"), Request.IntParams.FindRef(TEXT("resolution")), static_cast<int64>(1024));
	TestTrue(TEXT("默认是随机种子"), Request.IsRandomSeed());

	// 固定种子。
	Model->SetRandomSeed(false);
	TestTrue(TEXT("拼得出请求"), Model->BuildRequest(Request));
	TestFalse(TEXT("不再是随机"), Request.IsRandomSeed());
	TestEqual(TEXT("种子"), Request.IntParams.FindRef(TEXT("seed")), static_cast<int64>(42));

	// 序列化出来的请求体。
	{
		FString Json;
		FString Error;
		TestTrue(TEXT("请求合法"), Request.ToJsonString(Json, Error));
		AddInfo(FString::Printf(TEXT("请求体：%s"), *Json));
		TestTrue(TEXT("workflow_id"), Json.Contains(TEXT("\"workflow_id\":\"src-local-realistic-v1.dec-chord-v1\"")));
		TestTrue(TEXT("variants"), Json.Contains(TEXT("\"variants\":4")));
		TestTrue(TEXT("seed"), Json.Contains(TEXT("\"seed\":42")));
		TestTrue(TEXT("resolution"), Json.Contains(TEXT("\"resolution\":1024")));
	}

	// 固定种子填满界面上限、8 个变体：仍然合法（上限为此留了余量）。
	Model->SetVariants(8);
	Model->SetFixedSeed(FDreamTextureGenerateModel::GetMaxSeedForUI());
	TestTrue(TEXT("界面上限 × 8 个变体合法"), Model->CanSubmit());

	// 工作流没有 resolution 参数时不提交这个参数。
	{
		FDreamTextureWorkflowCatalog NoResolution = Catalog;
		for (FDreamTextureWorkflowMeta& Meta : NoResolution.Workflows)
		{
			Meta.Params.RemoveAll([](const FDreamTextureParamSpec& Spec) { return Spec.Key == TEXT("resolution"); });
		}
		Model->LoadCatalog();
		Fake.CompleteCatalog(Fake.CatalogCalls.Num() - 1, NoResolution);
		TestEqual(TEXT("没有分辨率候选"), Model->GetResolutionOptions().Num(), 0);
		TestEqual(TEXT("分辨率不提交"), Model->GetResolution(), 0);
		TestTrue(TEXT("拼得出请求"), Model->BuildRequest(Request));
		TestFalse(TEXT("请求里没有 resolution"), Request.IntParams.Contains(TEXT("resolution")));
	}

	return true;
}

// ---------------------------------------------------------------------------
// 模型：三级状态怎么挡提交
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateModelReadyTest, "DreamTexture.Editor.Generate.Model.Ready",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateModelReadyTest::RunTest(const FString& Parameters)
{
	using ETier = EDreamTextureGenerateTier;

	// 没有连接对象：第 1 级。
	{
		const FDreamTextureGenerateReadiness R = FDreamTextureGenerateModel::EvaluateReadiness(nullptr);
		TestFalse(TEXT("无连接不能提交"), R.bCanSubmit);
		TestTrue(TEXT("无连接算第 1 级"), R.Tier == ETier::Backend);
		TestFalse(TEXT("有原因"), R.Reason.IsEmpty());
	}

	TSharedRef<FFakeConnection> Conn = MakeShared<FFakeConnection>();
	TSharedPtr<FDreamTextureConnection> Base = Conn;

	// 还没开始连：是「正在连接」，不是「没应答」。
	Conn->Set(EDreamTextureLink::Offline, false);
	{
		const FDreamTextureGenerateReadiness R = FDreamTextureGenerateModel::EvaluateReadiness(Base);
		TestFalse(TEXT("还没连上不能提交"), R.bCanSubmit);
		TestTrue(TEXT("第 1 级"), R.Tier == ETier::Backend);
		TestTrue(TEXT("说的是正在连接"), R.Reason.ToString().Contains(TEXT("Connecting to the backend")));
		TestTrue(TEXT("标着只是还在连，不是确实没应答"), R.bConnecting);
	}

	// 连过又失败：第 1 级「没应答」，且指向「后端进程」的修法。
	Conn->Set(EDreamTextureLink::Offline, false, TEXT("连接被拒绝"));
	{
		const FDreamTextureGenerateReadiness R = FDreamTextureGenerateModel::EvaluateReadiness(Base);
		TestFalse(TEXT("后端没应答不能提交"), R.bCanSubmit);
		TestTrue(TEXT("第 1 级"), R.Tier == ETier::Backend);
		TestTrue(TEXT("说的是后端没应答"), R.Reason.ToString().Contains(TEXT("not responding")));
		TestTrue(TEXT("点明是第 1 级"), R.Reason.ToString().Contains(TEXT("tier 1")));
		TestFalse(TEXT("连过了确实没应答，不是还在连"), R.bConnecting);
	}

	// 后端通、长连接没连上：第 2 级，只是提醒，不挡提交——所有操作走 REST。
	Conn->Set(EDreamTextureLink::Connecting, true);
	{
		const FDreamTextureGenerateReadiness R = FDreamTextureGenerateModel::EvaluateReadiness(Base);
		TestTrue(TEXT("长连接断了仍可提交"), R.bCanSubmit);
		TestTrue(TEXT("第 2 级"), R.Tier == ETier::Link);
		TestTrue(TEXT("有提醒"), R.Note.ToString().Contains(TEXT("tier 2")));
		TestTrue(TEXT("没有「不能提交」的原因"), R.Reason.IsEmpty());
	}

	// 长连接 Stale 同理。
	Conn->Set(EDreamTextureLink::Stale, true);
	TestTrue(TEXT("失联仍可提交"), FDreamTextureGenerateModel::EvaluateReadiness(Base).bCanSubmit);

	// 长连接通了但 ComfyUI 的状态还没推过来：状态未知，不拦也不说话。
	Conn->Set(EDreamTextureLink::Live, true);
	{
		const FDreamTextureGenerateReadiness R = FDreamTextureGenerateModel::EvaluateReadiness(Base);
		TestTrue(TEXT("状态未知不拦"), R.bCanSubmit);
		TestTrue(TEXT("也没有提醒"), R.Tier == ETier::None && R.Note.IsEmpty());
	}

	FDreamTextureHealth Health;
	Health.Mode = TEXT("attach");

	// 第 3 级：冷启动。
	Health.bStarting = true;
	Health.StartingSecs = 37;
	Conn->SetHealth(Health);
	{
		const FDreamTextureGenerateReadiness R = FDreamTextureGenerateModel::EvaluateReadiness(Base);
		TestFalse(TEXT("启动中不能提交"), R.bCanSubmit);
		TestTrue(TEXT("第 3 级"), R.Tier == ETier::Comfy);
		TestTrue(TEXT("带着已启动的秒数"), R.Reason.ToString().Contains(TEXT("37")));
		TestTrue(TEXT("说别去重启"), R.Reason.ToString().Contains(TEXT("do not restart")));
	}

	// 第 3 级：手动停的。
	Health.bStarting = false;
	Health.bUserStopped = true;
	Conn->SetHealth(Health);
	{
		const FDreamTextureGenerateReadiness R = FDreamTextureGenerateModel::EvaluateReadiness(Base);
		TestFalse(TEXT("手动停止不能提交"), R.bCanSubmit);
		TestTrue(TEXT("第 3 级"), R.Tier == ETier::Comfy);
		TestTrue(TEXT("说是手动停的"), R.Reason.ToString().Contains(TEXT("stopped manually")));
	}

	// 第 3 级：没在运行，且后端是好的。
	Health.bUserStopped = false;
	Health.bAlive = false;
	Health.Reason = TEXT("无法连接 ComfyUI");
	Conn->SetHealth(Health);
	{
		const FDreamTextureGenerateReadiness R = FDreamTextureGenerateModel::EvaluateReadiness(Base);
		TestFalse(TEXT("ComfyUI 没运行不能提交"), R.bCanSubmit);
		TestTrue(TEXT("第 3 级"), R.Tier == ETier::Comfy);
		TestTrue(TEXT("点明是 ComfyUI"), R.Reason.ToString().Contains(TEXT("ComfyUI is not running")));
		TestTrue(TEXT("别去重启错误的东西：后端是好的"), R.Reason.ToString().Contains(TEXT("do not restart the backend")));
		TestTrue(TEXT("带着后端给的原因"), R.Reason.ToString().Contains(TEXT("无法连接 ComfyUI")));
	}

	// Alive 但不 Ready 是常态（队列积压），不是故障：能提交，只带一句提醒。
	Health.bAlive = true;
	Health.bReady = false;
	Health.Reason = TEXT("队列积压 9 超过阈值 8");
	Conn->SetHealth(Health);
	{
		const FDreamTextureGenerateReadiness R = FDreamTextureGenerateModel::EvaluateReadiness(Base);
		TestTrue(TEXT("积压时仍可提交"), R.bCanSubmit);
		TestTrue(TEXT("第 3 级"), R.Tier == ETier::Comfy);
		TestTrue(TEXT("提醒里带着原因"), R.Note.ToString().Contains(TEXT("队列积压 9")));
	}

	// 全部就绪：什么都不说。
	Health.bReady = true;
	Health.Reason.Reset();
	Conn->SetHealth(Health);
	{
		const FDreamTextureGenerateReadiness R = FDreamTextureGenerateModel::EvaluateReadiness(Base);
		TestTrue(TEXT("就绪可提交"), R.bCanSubmit);
		TestTrue(TEXT("没有层级"), R.Tier == ETier::None);
		TestTrue(TEXT("没有提醒"), R.Note.IsEmpty() && R.Reason.IsEmpty());
	}

	// ── 模型层面：连接问题排在表单问题前面，且点明是哪一级 ──
	{
		FFakeBackend Fake;
		FDreamTextureWorkflowCatalog Catalog;
		TSharedRef<FDreamTextureGenerateModel> Model = MakeReadyModel(*this, Fake, Catalog);
		TestTrue(TEXT("都正常时可以提交"), Model->CanSubmit());

		// 后端掉了：原因是第 1 级，而不是「读不到工作流列表」之类下游的现象。
		Fake.Conn->Set(EDreamTextureLink::Offline, false, TEXT("连接被拒绝"));
		FText Why;
		TestFalse(TEXT("后端掉了不能提交"), Model->CanSubmit(&Why));
		TestTrue(TEXT("原因是第 1 级"), Why.ToString().Contains(TEXT("tier 1")));
		TestTrue(TEXT("层级是后端"), Model->GetReadiness().Tier == ETier::Backend);

		// 后端回来、ComfyUI 没起来：第 3 级。
		Fake.Conn->SetAllReady();
		FDreamTextureHealth Down;
		Down.Mode = TEXT("attach");
		Fake.Conn->SetHealth(Down);
		TestFalse(TEXT("ComfyUI 不在不能提交"), Model->CanSubmit(&Why));
		TestTrue(TEXT("原因是第 3 级"), Why.ToString().Contains(TEXT("tier 3")));
		TestTrue(TEXT("层级是 ComfyUI"), Model->GetReadiness().Tier == ETier::Comfy);

		// Submit 被挡住时不发请求，只给一条错误提示。
		int32 ErrorNotices = 0;
		Model->OnNotice.AddLambda([&ErrorNotices](const FText&, bool bIsError) { ErrorNotices += bIsError ? 1 : 0; });
		Model->Submit();
		TestEqual(TEXT("没有发请求"), Fake.SubmitRequests.Num(), 0);
		TestEqual(TEXT("提示了一次错误"), ErrorNotices, 1);
		TestFalse(TEXT("记下了原因"), Model->GetSubmitError().IsEmpty());

		// 没有连接对象的假后端：同样第 1 级。
		FDreamTextureGenerateBackend NoConn = Fake.Make();
		NoConn.GetConnection = []() -> TSharedPtr<FDreamTextureConnection> { return nullptr; };
		TSharedRef<FDreamTextureGenerateModel> Model2 = MakeShared<FDreamTextureGenerateModel>(MoveTemp(NoConn));
		TestTrue(TEXT("没有连接对象是第 1 级"), Model2->GetReadiness().Tier == ETier::Backend);
	}

	return true;
}

// ---------------------------------------------------------------------------
// 模型：许可提示
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateModelLicenseTest, "DreamTexture.Editor.Generate.Model.License",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateModelLicenseTest::RunTest(const FString& Parameters)
{
	FFakeBackend Fake;
	FDreamTextureWorkflowCatalog Catalog;
	TSharedRef<FDreamTextureGenerateModel> Model = MakeReadyModel(*this, Fake, Catalog);

	// 默认是 CHORD：research-only，不可商用，并且告诉人换哪个分解模型就没有这个限制。
	{
		const FDreamTextureGenerateLicenseHint Hint = Model->GetLicenseHint();
		TestTrue(TEXT("CHORD 不可商用"), Hint.bNonCommercial);
		TestTrue(TEXT("原因是后端原文（组件: 许可）"), Hint.Reason.StartsWith(TEXT("CHORD (dt.pbr_estimate / dt.chord_model): Ubisoft Machine Learning License")));
		TestTrue(TEXT("原因里点明 research-only"), Hint.Reason.Contains(TEXT("Research-Only")));
		TestEqual(TEXT("一个不可商用的组件"), Hint.Components.Num(), 1);
		const FDreamTextureWorkflowMeta* Derive = Catalog.FindSegment(TEXT("dec-derive-v1"));
		if (TestNotNull(TEXT("目录里有传统派生"), Derive) && TestEqual(TEXT("可商用的分解模型一个"), Hint.CommercialDecomposeNames.Num(), 1))
		{
			TestEqual(TEXT("建议换成它"), Hint.CommercialDecomposeNames[0], Derive->Name);
		}
		TestTrue(TEXT("写实 × CHORD 没有搭不上的提醒"), Hint.Mismatch.IsEmpty());
	}

	// 换成传统派生：没有许可限制。
	Model->SetDecomposeId(TEXT("dec-derive-v1"));
	{
		const FDreamTextureGenerateLicenseHint Hint = Model->GetLicenseHint();
		TestFalse(TEXT("传统派生可商用"), Hint.bNonCommercial);
		TestTrue(TEXT("原因为空"), Hint.Reason.IsEmpty());
		TestEqual(TEXT("没有组件"), Hint.Components.Num(), 0);
		TestEqual(TEXT("可商用时不给建议"), Hint.CommercialDecomposeNames.Num(), 0);
	}

	// 云端底图 × CHORD：许可来自分解段，出图换成云端不能把它弄丢。搭不上的提醒（手绘 × CHORD）原样带出来。
	Model->SetSourceId(TEXT("src-api-stylized-v1"));
	Model->SetDecomposeId(TEXT("dec-chord-v1"));
	{
		const FDreamTextureGenerateLicenseHint Hint = Model->GetLicenseHint();
		TestTrue(TEXT("云端 × CHORD 仍不可商用"), Hint.bNonCommercial);
		TestFalse(TEXT("手绘 × CHORD 带提醒"), Hint.Mismatch.IsEmpty());
	}

	// 目录里暂时没有这个组合：自己把两段的许可并起来，不能因为目录没到齐就不提醒。
	Model->SetSourceId(TEXT("src-api-realistic-v1"));
	Model->SetDecomposeId(TEXT("dec-chord-v1"));
	TestTrue(TEXT("确实没有这个组合"), Model->GetCurrentWorkflow() == nullptr);
	TestTrue(TEXT("没有组合时用两段的并集，仍提醒"), Model->GetLicenseHint().bNonCommercial);

	// 目录还没加载：什么都没有，不报不可商用。
	{
		FFakeBackend Fake2;
		TSharedRef<FDreamTextureGenerateModel> Empty = MakeShared<FDreamTextureGenerateModel>(Fake2.Make());
		TestFalse(TEXT("目录没加载时不提醒"), Empty->GetLicenseHint().bNonCommercial);
	}

	return true;
}

// ---------------------------------------------------------------------------
// 模型：云端底图
// ---------------------------------------------------------------------------

namespace
{
	FDreamTextureImagenProviders MakeCloudProviders(bool bConfigured, const TArray<FString>& ModelIds, const FString& Error = FString())
	{
		FDreamTextureImagenProvider Provider;
		Provider.Id = TEXT("openai");
		Provider.Label = TEXT("OpenAI");
		Provider.bConfigured = bConfigured;
		Provider.Error = Error;
		for (const FString& Id : ModelIds)
		{
			FDreamTextureImagenModel CloudModel;
			CloudModel.Id = Id;
			CloudModel.Label = Id;
			CloudModel.bKnown = true;
			Provider.Models.Add(CloudModel);
		}
		FDreamTextureImagenProviders Providers;
		Providers.Providers.Add(Provider);
		return Providers;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateModelCloudTest, "DreamTexture.Editor.Generate.Model.Cloud",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateModelCloudTest::RunTest(const FString& Parameters)
{
	using ECloud = FDreamTextureGenerateModel::ECloudState;

	FFakeBackend Fake;
	FDreamTextureWorkflowCatalog Catalog;
	TSharedRef<FDreamTextureGenerateModel> Model = MakeReadyModel(*this, Fake, Catalog);

	// 本地出图：不涉及云端，不去问清单。
	TestFalse(TEXT("默认是本地出图"), Model->IsCloudSource());
	TestEqual(TEXT("没有请求云端清单"), Fake.CloudCalls.Num(), 0);
	TestTrue(TEXT("本地出图可以提交"), Model->CanSubmit());
	{
		FDreamTextureGenerateRequest Request;
		TestTrue(TEXT("拼得出请求"), Model->BuildRequest(Request));
		TestFalse(TEXT("本地请求里没有云端模型"), Request.StringParams.Contains(TEXT("api_model")));
		TestFalse(TEXT("也没有画质"), Request.StringParams.Contains(TEXT("api_quality")));
	}

	// 选了云端底图：去拉清单，拉到之前提交不了。
	Model->SetSourceId(TEXT("src-api-stylized-v1"));
	Model->SetDecomposeId(TEXT("dec-chord-v1"));
	TestTrue(TEXT("云端出图"), Model->IsCloudSource());
	if (!TestEqual(TEXT("去请求了云端清单"), Fake.CloudCalls.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("读取中"), Model->GetCloudState() == ECloud::Loading);
	{
		FText Why;
		TestFalse(TEXT("清单没到不能提交"), Model->CanSubmit(&Why));
		TestTrue(TEXT("原因说在查云端模型"), Why.ToString().Contains(TEXT("cloud models")));
	}

	// 没填令牌：不是错误，是要去设置——指路。
	Fake.CompleteCloud(0, MakeCloudProviders(/*bConfigured*/ false, {}));
	TestTrue(TEXT("就绪"), Model->GetCloudState() == ECloud::Ready);
	TestEqual(TEXT("没有可选模型"), Model->GetCloudModels().Num(), 0);
	{
		FText Why;
		TestFalse(TEXT("没填令牌不能提交"), Model->CheckCloudReady(&Why));
		TestTrue(TEXT("点名是 OpenAI 的 API Key"), Why.ToString().Contains(TEXT("OpenAI")) && Why.ToString().Contains(TEXT("API Key")));
		TestTrue(TEXT("指路去 Web 端设置"), Why.ToString().Contains(TEXT("Settings")));
		FText Submit;
		TestFalse(TEXT("CanSubmit 给的是同一个原因"), Model->CanSubmit(&Submit));
		TestEqual(TEXT("同一句"), Submit.ToString(), Why.ToString());
	}

	// 填了令牌但连不上网关：带着后端给的原因。
	Model->LoadCloudModels();
	Fake.CompleteCloud(1, MakeCloudProviders(true, {}, TEXT("组织未完成身份验证")));
	{
		FText Why;
		TestFalse(TEXT("连不上不能提交"), Model->CheckCloudReady(&Why));
		TestTrue(TEXT("带着原因"), Why.ToString().Contains(TEXT("组织未完成身份验证")));
	}

	// 令牌有效但一个模型都没有。
	Model->LoadCloudModels();
	Fake.CompleteCloud(2, MakeCloudProviders(true, {}));
	{
		FText Why;
		TestFalse(TEXT("没有模型不能提交"), Model->CheckCloudReady(&Why));
		TestTrue(TEXT("说没有可用的模型"), Why.ToString().Contains(TEXT("has no models available")));
	}

	// 有模型：默认选第一个（后端把认得的、推荐的排在前面）。
	Model->LoadCloudModels();
	Fake.CompleteCloud(3, MakeCloudProviders(true, { TEXT("gpt-image-2"), TEXT("gpt-image-1.5") }));
	TestEqual(TEXT("两个模型"), Model->GetCloudModels().Num(), 2);
	TestEqual(TEXT("默认第一个"), Model->GetCloudModelId(), FString(TEXT("gpt-image-2")));
	TestTrue(TEXT("云端这一路通了"), Model->CheckCloudReady());
	TestTrue(TEXT("可以提交"), Model->CanSubmit());

	Model->SetCloudModel(TEXT("gpt-image-1.5"));
	TestEqual(TEXT("改选"), Model->GetCloudModelId(), FString(TEXT("gpt-image-1.5")));
	Model->SetCloudModel(TEXT("bogus"));
	TestEqual(TEXT("不在清单里的被忽略"), Model->GetCloudModelId(), FString(TEXT("gpt-image-1.5")));

	// 画质：候选与默认值取自所选管线的 api_quality 参数。
	{
		const TArray<FString> Qualities = Model->GetCloudQualityOptions();
		if (TestEqual(TEXT("三档画质"), Qualities.Num(), 3))
		{
			TestEqual(TEXT("第一档"), Qualities[0], FString(TEXT("low")));
		}
		TestEqual(TEXT("默认 medium"), Model->GetCloudQuality(), FString(TEXT("medium")));
		Model->SetCloudQuality(TEXT("high"));
		TestEqual(TEXT("改成 high"), Model->GetCloudQuality(), FString(TEXT("high")));
		Model->SetCloudQuality(TEXT("ultra"));
		TestEqual(TEXT("不在候选里回到默认"), Model->GetCloudQuality(), FString(TEXT("medium")));
		Model->SetCloudQuality(TEXT("high"));
	}

	// 请求里带上云端模型与画质。
	{
		FDreamTextureGenerateRequest Request;
		TestTrue(TEXT("拼得出请求"), Model->BuildRequest(Request));
		TestEqual(TEXT("工作流是云端组合"), Request.WorkflowId, FString(TEXT("src-api-stylized-v1.dec-chord-v1")));
		TestEqual(TEXT("云端模型填进 api_model"), Request.StringParams.FindRef(TEXT("api_model")), FString(TEXT("gpt-image-1.5")));
		TestEqual(TEXT("画质"), Request.StringParams.FindRef(TEXT("api_quality")), FString(TEXT("high")));
	}

	// 清单刷新后选的模型不在了：落到第一个，不提交一个不存在的。
	Model->LoadCloudModels();
	Fake.CompleteCloud(4, MakeCloudProviders(true, { TEXT("gpt-image-3") }));
	TestEqual(TEXT("选的没了，落到第一个"), Model->GetCloudModelId(), FString(TEXT("gpt-image-3")));

	// 过期的结果：连发两次，新的先回、旧的后回，旧的不能盖掉新的。
	Model->LoadCloudModels();
	Model->LoadCloudModels();
	const int32 Newer = Fake.CloudCalls.Num() - 1;
	Fake.CompleteCloud(Newer, MakeCloudProviders(true, { TEXT("new-model") }));
	Fake.FailCloud(Newer - 1, TEXT("旧请求失败"));
	TestTrue(TEXT("旧的失败结果被丢弃"), Model->GetCloudState() == ECloud::Ready);
	TestEqual(TEXT("留下的是新请求的结果"), Model->GetCloudModelId(), FString(TEXT("new-model")));

	// 失败：状态 Failed，提交挡住且给原因；链路重新变 Live 时自动再拉。
	Model->LoadCloudModels();
	Fake.FailCloud(Fake.CloudCalls.Num() - 1, TEXT("读取云端模型列表失败：连不上"));
	TestTrue(TEXT("失败"), Model->GetCloudState() == ECloud::Failed);
	{
		FText Why;
		TestFalse(TEXT("失败时不能提交"), Model->CheckCloudReady(&Why));
		TestTrue(TEXT("原因里有后端的话"), Why.ToString().Contains(TEXT("连不上")));
	}
	const int32 CallsBefore = Fake.CloudCalls.Num();
	Model->HandleLinkChanged(EDreamTextureLink::Live);
	TestEqual(TEXT("Live 时补拉云端清单"), Fake.CloudCalls.Num(), CallsBefore + 1);

	// 换回本地出图：云端那些检查不再拦。
	Model->SetSourceId(TEXT("src-local-realistic-v1"));
	TestFalse(TEXT("换回本地"), Model->IsCloudSource());
	TestTrue(TEXT("本地出图又能提交"), Model->CanSubmit());

	// 先选云端、目录后到：目录到位后才发现是云端，也去拉清单。
	{
		FFakeBackend Fake2;
		TSharedRef<FDreamTextureGenerateModel> Model2 = MakeShared<FDreamTextureGenerateModel>(Fake2.Make());
		Model2->Initialize(false);
		Model2->SetSourceId(TEXT("src-api-stylized-v1"));
		TestFalse(TEXT("目录没到，不知道它是云端"), Model2->IsCloudSource());
		TestEqual(TEXT("所以不拉"), Fake2.CloudCalls.Num(), 0);
		Model2->LoadCatalog();
		Fake2.CompleteCatalog(0, Catalog);
		TestEqual(TEXT("目录到位后选中的是云端：去拉清单"), Fake2.CloudCalls.Num(), 1);
	}

	// 模型先销毁、清单后到：不能崩。
	{
		FFakeBackend Fake3;
		FDreamTextureWorkflowCatalog Catalog3;
		TSharedPtr<FDreamTextureGenerateModel> Short = MakeReadyModel(*this, Fake3, Catalog3);
		Short->SetSourceId(TEXT("src-api-stylized-v1"));
		Short.Reset();
		Fake3.CompleteCloud(0, MakeCloudProviders(true, { TEXT("x") }));
		TestTrue(TEXT("模型销毁后回调到达没有崩溃"), true);
	}

	return true;
}

// ---------------------------------------------------------------------------
// 模型：启动后端并生成
// ---------------------------------------------------------------------------

namespace
{
	/** 连接说「后端没应答」（连过了确实没有），目录没拉到，写好了提示词：用户在后端没开时点生成的起点。 */
	TSharedRef<FDreamTextureGenerateModel> MakeOfflineModel(FFakeBackend& Fake)
	{
		Fake.Conn->Set(EDreamTextureLink::Offline, false, TEXT("连接被拒绝"));
		TSharedRef<FDreamTextureGenerateModel> Model = MakeShared<FDreamTextureGenerateModel>(Fake.Make());
		Model->Initialize(false);
		Model->SetPrompt(TEXT("长满青苔的碎裂岩石"));
		Model->SetVariants(2);
		Model->SetRandomSeed(false);
		Model->SetFixedSeed(99);
		return Model;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateModelAutoStartTest, "DreamTexture.Editor.Generate.Model.AutoStart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateModelAutoStartTest::RunTest(const FString& Parameters)
{
	using EAuto = FDreamTextureGenerateModel::EAutoStartState;
	using EPrimary = FDreamTextureGenerateModel::EPrimaryAction;
	using ECatalog = FDreamTextureGenerateModel::ECatalogState;

	FDreamTextureWorkflowCatalog Fixture;
	if (!LoadCatalogFixture(*this, Fixture))
	{
		return false;
	}

	// ── 主按钮是什么：只在「连过了确实没应答」且配了可执行文件时才是「启动后端并生成」 ──
	{
		FFakeBackend Fake;
		TSharedRef<FDreamTextureGenerateModel> Model = MakeOfflineModel(Fake);
		TestTrue(TEXT("后端没应答 + 配了可执行文件：启动后端并生成"), Model->GetPrimaryAction() == EPrimary::StartAndSubmit);
		TestTrue(TEXT("这时左栏该显示表单"), Model->ShouldShowOfflineForm());
		TestFalse(TEXT("不需要去设置里配"), Model->NeedsExecutableSetting());
		TestTrue(TEXT("主按钮可点"), Model->CanActivatePrimary());
		TestFalse(TEXT("普通提交仍被第 1 级挡着"), Model->CanSubmit());

		// 只是还在连：几秒内多半自己就好，不抢着拉起进程。
		Fake.Conn->Set(EDreamTextureLink::Offline, false);
		TestTrue(TEXT("还在连：仍是普通提交"), Model->GetPrimaryAction() == EPrimary::Submit);
		TestFalse(TEXT("还在连：不显示离线表单"), Model->ShouldShowOfflineForm());
		TestFalse(TEXT("还在连：也不催人去设置"), Model->NeedsExecutableSetting());

		// 后端好了：普通提交。
		Fake.Conn->SetAllReady();
		TestTrue(TEXT("后端在：普通提交"), Model->GetPrimaryAction() == EPrimary::Submit);
		TestFalse(TEXT("后端在：不显示离线表单"), Model->ShouldShowOfflineForm());
	}

	// ── 没配可执行文件：保持原来的说明，不提供启动；界面给一个跳到设置的入口 ──
	{
		FFakeBackend Fake;
		Fake.bHasExecutable = false;
		TSharedRef<FDreamTextureGenerateModel> Model = MakeOfflineModel(Fake);
		TestTrue(TEXT("没配：仍是普通提交"), Model->GetPrimaryAction() == EPrimary::Submit);
		TestTrue(TEXT("没配：该给设置入口"), Model->NeedsExecutableSetting());
		TestFalse(TEXT("没配：不显示离线表单（左栏仍是原来的失败态）"), Model->ShouldShowOfflineForm());

		FText Why;
		TestFalse(TEXT("没配：不能启动"), Model->CanAutoStart(&Why));
		TestTrue(TEXT("原因点名可执行文件"), Why.ToString().Contains(TEXT("backend executable")));
		TestFalse(TEXT("主按钮也不可点"), Model->CanActivatePrimary());
		FText SubmitWhy;
		Model->CanSubmit(&SubmitWhy);
		TestTrue(TEXT("普通提交的说明还是第 1 级"), SubmitWhy.ToString().Contains(TEXT("tier 1")));

		int32 ErrorNotices = 0;
		Model->OnNotice.AddLambda([&ErrorNotices](const FText&, bool bIsError) { ErrorNotices += bIsError ? 1 : 0; });
		Model->StartBackendAndSubmit();
		TestEqual(TEXT("没配：不会去启动"), Fake.EnsureCalls.Num(), 0);
		TestEqual(TEXT("提示了一次"), ErrorNotices, 1);
		TestTrue(TEXT("原因记下了"), Model->GetSubmitError().ToString().Contains(TEXT("backend executable")));
		TestFalse(TEXT("没进入启动流程"), Model->IsAutoStarting());
	}

	// ── 启动成功 → 补拉目录 → 按当时的表单提交；这一次提交不看连接的三级 ──
	{
		FFakeBackend Fake;
		TSharedRef<FDreamTextureGenerateModel> Model = MakeOfflineModel(Fake);

		// 后端不在时目录请求失败过。
		Model->LoadCatalog();
		Fake.FailCatalog(0, TEXT("读取工作流列表失败：连不上，后端进程可能没在跑"));
		TestTrue(TEXT("目录失败"), Model->GetCatalogState() == ECatalog::Failed);

		int32 InfoNotices = 0;
		Model->OnNotice.AddLambda([&InfoNotices](const FText&, bool bIsError) { InfoNotices += bIsError ? 0 : 1; });

		Model->ActivatePrimary();
		if (!TestEqual(TEXT("发出一个启动请求"), Fake.EnsureCalls.Num(), 1))
		{
			return false;
		}
		TestTrue(TEXT("正在启动后端"), Model->GetAutoStartState() == EAuto::StartingBackend);
		TestTrue(TEXT("IsAutoStarting"), Model->IsAutoStarting());
		TestFalse(TEXT("启动中主按钮不可点"), Model->CanActivatePrimary());

		// 启动中再点：不重复起；Submit() 也静默忽略（那条流程自己会提交）。
		Model->ActivatePrimary();
		Model->StartBackendAndSubmit();
		Model->Submit();
		TestEqual(TEXT("启动中再点不重复启动"), Fake.EnsureCalls.Num(), 1);
		TestEqual(TEXT("Submit 被忽略，没有请求"), Fake.SubmitRequests.Num(), 0);
		TestTrue(TEXT("Submit 被忽略，没有错误"), Model->GetSubmitError().IsEmpty());

		// 后端应答：补拉目录，目录到了才提交。
		const int32 CatalogCallsBefore = Fake.CatalogCalls.Num();
		Fake.CompleteEnsure(0);
		TestTrue(TEXT("等目录"), Model->GetAutoStartState() == EAuto::LoadingCatalog);
		if (!TestEqual(TEXT("补拉了目录"), Fake.CatalogCalls.Num(), CatalogCallsBefore + 1))
		{
			return false;
		}
		TestEqual(TEXT("目录没到之前不提交"), Fake.SubmitRequests.Num(), 0);

		// 这时连接对象还没追上：仍说后端没应答（它要过几秒才会重连上）。再叠一个 ComfyUI 启动中——
		// 平时这会被第 3 级挡住，这次提交放行。
		TestFalse(TEXT("连接对象还在说后端没应答"), Fake.Conn->IsBackendReachable());
		FDreamTextureHealth Starting;
		Starting.Mode = TEXT("managed");
		Starting.bStarting = true;
		Starting.StartingSecs = 3;
		Fake.Conn->SetHealth(Starting);

		Fake.CompleteCatalog(Fake.CatalogCalls.Num() - 1, Fixture);
		TestTrue(TEXT("启动流程走完"), Model->GetAutoStartState() == EAuto::Idle);
		if (!TestEqual(TEXT("按表单提交了"), Fake.SubmitRequests.Num(), 1))
		{
			return false;
		}
		{
			const FDreamTextureGenerateRequest& Request = Fake.SubmitRequests[0];
			TestEqual(TEXT("默认选择拼出的组合"), Request.WorkflowId, FString(TEXT("src-local-realistic-v1.dec-chord-v1")));
			TestEqual(TEXT("用户当时填的提示词"), Request.StringParams.FindRef(TEXT("prompt")), FString(TEXT("长满青苔的碎裂岩石")));
			TestEqual(TEXT("用户当时填的变体数"), Request.Variants, 2);
			TestEqual(TEXT("用户当时填的固定种子"), Request.IntParams.FindRef(TEXT("seed")), static_cast<int64>(99));
		}
		TestTrue(TEXT("提交在途"), Model->IsSubmitting());

		// 同样的状态下，普通提交仍然被第 3 级挡着——放行只限这一次。
		Fake.Conn->SetAllReady();
		Fake.Conn->SetHealth(Starting);
		FText Why;
		TestFalse(TEXT("平时 ComfyUI 启动中仍然挡"), Model->CanSubmit(&Why));
		TestTrue(TEXT("原因是第 3 级"), Why.ToString().Contains(TEXT("tier 3")));

		// 应答回来：本批的变体出现，卡片上阶段看得出来。
		{
			TArray<FDreamTextureJob> Jobs;
			Jobs.Add(GenViewMakeJob(TEXT("a1"), EDreamTextureJobStatus::Queued, 0.0f, TEXT("bAuto")));
			Jobs.Add(GenViewMakeJob(TEXT("a2"), EDreamTextureJobStatus::Queued, 0.0f, TEXT("bAuto")));
			Fake.CompleteSubmit(0, MoveTemp(Jobs));
		}
		TestEqual(TEXT("本批两个变体"), Model->GetRows().Num(), 2);
		FDreamTextureJob Waiting = GenViewMakeJob(TEXT("a1"), EDreamTextureJobStatus::Running, 0.0f, TEXT("bAuto"));
		Waiting.Stage = TEXT("等待 ComfyUI");
		Fake.Tracker->Track(Waiting);
		{
			const FDreamTextureGenerateRowView View = Model->GetRowView(Model->GetRows()[0]);
			TestTrue(TEXT("卡片显示运行中"), View.Kind == FDreamTextureGenerateRowView::EKind::Running);
			TestEqual(TEXT("阶段是「等待 ComfyUI」的本地化文字"), View.Stage.ToString(), FString(TEXT("Waiting for ComfyUI")));
			TestFalse(TEXT("没有假进度"), View.Percent.IsSet());
		}
		TestTrue(TEXT("成功提示过"), InfoNotices >= 1);
	}

	// ── 目录已经在了（后端是后来掉的）：启动成功后直接提交，不再拉目录 ──
	{
		FFakeBackend Fake;
		FDreamTextureWorkflowCatalog Catalog;
		TSharedRef<FDreamTextureGenerateModel> Model = MakeReadyModel(*this, Fake, Catalog);
		Fake.Conn->Set(EDreamTextureLink::Offline, false, TEXT("连接被拒绝"));
		TestTrue(TEXT("目录在、后端掉了：启动后端并生成"), Model->GetPrimaryAction() == EPrimary::StartAndSubmit);

		const int32 CatalogCallsBefore = Fake.CatalogCalls.Num();
		Model->ActivatePrimary();
		Fake.CompleteEnsure(0);
		TestEqual(TEXT("不再拉目录"), Fake.CatalogCalls.Num(), CatalogCallsBefore);
		TestEqual(TEXT("直接提交了"), Fake.SubmitRequests.Num(), 1);
		TestTrue(TEXT("流程走完"), Model->GetAutoStartState() == EAuto::Idle);
	}

	// ── 启动失败：进程管理器给的中文原因原样显示，可以重试 ──
	{
		FFakeBackend Fake;
		TSharedRef<FDreamTextureGenerateModel> Model = MakeOfflineModel(Fake);
		TArray<FString> ErrorNotices;
		Model->OnNotice.AddLambda([&ErrorNotices](const FText& Message, bool bIsError)
		{
			if (bIsError)
			{
				ErrorNotices.Add(Message.ToString());
			}
		});

		Model->ActivatePrimary();
		const FString Reason = TEXT("后端进程拉起来了，但 90 秒内 /api/health 一直没应答。日志在 D:/x/logs/dreamtexture.log。");
		Fake.FailEnsure(0, Reason);
		TestTrue(TEXT("回到空闲"), Model->GetAutoStartState() == EAuto::Idle);
		TestEqual(TEXT("原因原样显示"), Model->GetSubmitError().ToString(), Reason);
		if (TestEqual(TEXT("错误提示一次"), ErrorNotices.Num(), 1))
		{
			TestEqual(TEXT("提示内容原样"), ErrorNotices[0], Reason);
		}
		TestEqual(TEXT("没去拉目录"), Fake.CatalogCalls.Num(), 0);
		TestEqual(TEXT("没提交"), Fake.SubmitRequests.Num(), 0);

		// 重试：主按钮仍可点，再发一个启动请求，并清掉旧错误。
		TestTrue(TEXT("可以重试"), Model->CanActivatePrimary());
		Model->ActivatePrimary();
		TestEqual(TEXT("又发了一个启动请求"), Fake.EnsureCalls.Num(), 2);
		TestTrue(TEXT("旧错误清掉"), Model->GetSubmitError().IsEmpty());

		// 进程管理器连原因都没给：兜底有说法。
		Fake.FailEnsure(1, FString());
		TestFalse(TEXT("空原因也有说法"), Model->GetSubmitError().IsEmpty());
	}

	// ── 没写提示词：不启动 ──
	{
		FFakeBackend Fake;
		TSharedRef<FDreamTextureGenerateModel> Model = MakeOfflineModel(Fake);
		Model->SetPrompt(TEXT("   "));
		FText Why;
		TestFalse(TEXT("没提示词不能启动"), Model->CanAutoStart(&Why));
		TestTrue(TEXT("原因是缺提示词"), Why.ToString().Contains(TEXT("prompt")));
		TestFalse(TEXT("主按钮不可点"), Model->CanActivatePrimary());
		Model->StartBackendAndSubmit();
		TestEqual(TEXT("没发启动请求"), Fake.EnsureCalls.Num(), 0);
	}

	// ── 取消：只是不再提交，不去停后端 ──
	{
		FFakeBackend Fake;
		TSharedRef<FDreamTextureGenerateModel> Model = MakeOfflineModel(Fake);
		int32 InfoNotices = 0;
		Model->OnNotice.AddLambda([&InfoNotices](const FText&, bool bIsError) { InfoNotices += bIsError ? 0 : 1; });

		Model->CancelAutoStart();
		TestEqual(TEXT("没在启动时取消什么都不做"), InfoNotices, 0);

		// 启动中取消：启动请求之后回来也不再继续。
		Model->ActivatePrimary();
		Model->CancelAutoStart();
		TestFalse(TEXT("取消后不在启动流程里"), Model->IsAutoStarting());
		TestEqual(TEXT("说了一声"), InfoNotices, 1);
		Fake.CompleteEnsure(0);
		TestEqual(TEXT("迟到的启动结果不再拉目录"), Fake.CatalogCalls.Num(), 0);
		TestEqual(TEXT("也不提交"), Fake.SubmitRequests.Num(), 0);
		TestTrue(TEXT("没有错误"), Model->GetSubmitError().IsEmpty());

		// 取消后可以重新来，是一次全新的启动请求。
		Model->ActivatePrimary();
		TestEqual(TEXT("重新启动发了新请求"), Fake.EnsureCalls.Num(), 2);

		// 等目录时取消：目录照样落进来（它本来就是该拉的），但不提交。
		Fake.CompleteEnsure(1);
		TestTrue(TEXT("等目录"), Model->GetAutoStartState() == EAuto::LoadingCatalog);
		Model->CancelAutoStart();
		Fake.CompleteCatalog(Fake.CatalogCalls.Num() - 1, Fixture);
		TestTrue(TEXT("目录落进来了"), Model->GetCatalogState() == ECatalog::Ready);
		TestEqual(TEXT("但没提交"), Fake.SubmitRequests.Num(), 0);
	}

	// ── 启动了，但读不到工作流列表：把原因摆出来，不悄悄停在半路 ──
	{
		FFakeBackend Fake;
		TSharedRef<FDreamTextureGenerateModel> Model = MakeOfflineModel(Fake);
		Model->ActivatePrimary();
		Fake.CompleteEnsure(0);
		Fake.FailCatalog(Fake.CatalogCalls.Num() - 1, TEXT("读取工作流列表失败：后端返回 HTTP 500"));
		TestTrue(TEXT("回到空闲"), Model->GetAutoStartState() == EAuto::Idle);
		TestTrue(TEXT("原因里有后端的话"), Model->GetSubmitError().ToString().Contains(TEXT("HTTP 500")));
		TestEqual(TEXT("没提交"), Fake.SubmitRequests.Num(), 0);
	}

	// ── 所选是云端出图模型：目录之后还要等云端清单，到了才提交；清单走不通就说明原因 ──
	{
		FFakeBackend Fake;
		FDreamTextureWorkflowCatalog Catalog;
		TSharedRef<FDreamTextureGenerateModel> Model = MakeReadyModel(*this, Fake, Catalog);
		Model->SetSourceId(TEXT("src-api-stylized-v1"));
		Model->SetDecomposeId(TEXT("dec-chord-v1"));
		TestEqual(TEXT("选云端时去拉了清单"), Fake.CloudCalls.Num(), 1);

		Fake.Conn->Set(EDreamTextureLink::Offline, false, TEXT("连接被拒绝"));
		Model->ActivatePrimary();
		Fake.CompleteEnsure(0);
		TestTrue(TEXT("等云端清单"), Model->GetAutoStartState() == EAuto::LoadingCloud);
		TestEqual(TEXT("没提交"), Fake.SubmitRequests.Num(), 0);
		TestEqual(TEXT("在途的清单请求没有重复发"), Fake.CloudCalls.Num(), 1);

		Fake.CompleteCloud(0, MakeCloudProviders(true, { TEXT("gpt-image-2") }));
		TestTrue(TEXT("云端清单到了，流程走完"), Model->GetAutoStartState() == EAuto::Idle);
		if (TestEqual(TEXT("提交了"), Fake.SubmitRequests.Num(), 1))
		{
			TestEqual(TEXT("带着云端模型"), Fake.SubmitRequests[0].StringParams.FindRef(TEXT("api_model")), FString(TEXT("gpt-image-2")));
		}
	}
	{
		FFakeBackend Fake;
		FDreamTextureWorkflowCatalog Catalog;
		TSharedRef<FDreamTextureGenerateModel> Model = MakeReadyModel(*this, Fake, Catalog);
		Model->SetSourceId(TEXT("src-api-stylized-v1"));
		Model->SetDecomposeId(TEXT("dec-chord-v1"));
		Fake.Conn->Set(EDreamTextureLink::Offline, false, TEXT("连接被拒绝"));
		Model->ActivatePrimary();
		Fake.CompleteEnsure(0);
		Fake.CompleteCloud(0, MakeCloudProviders(/*bConfigured*/ false, {}));
		TestTrue(TEXT("云端走不通：流程结束"), Model->GetAutoStartState() == EAuto::Idle);
		TestEqual(TEXT("没提交"), Fake.SubmitRequests.Num(), 0);
		TestTrue(TEXT("原因是没填 API Key"), Model->GetSubmitError().ToString().Contains(TEXT("API Key")));
	}

	// ── 启动中模型销毁、结果后到：不能崩 ──
	{
		FFakeBackend Fake;
		{
			TSharedRef<FDreamTextureGenerateModel> Short = MakeOfflineModel(Fake);
			Short->ActivatePrimary();
		}
		Fake.CompleteEnsure(0);
		TestTrue(TEXT("模型销毁后回调到达没有崩溃"), true);
	}

	return true;
}

// ---------------------------------------------------------------------------
// 模型：提交
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateModelSubmitTest, "DreamTexture.Editor.Generate.Model.Submit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateModelSubmitTest::RunTest(const FString& Parameters)
{
	FFakeBackend Fake;
	FDreamTextureWorkflowCatalog Catalog;
	TSharedRef<FDreamTextureGenerateModel> Model = MakeReadyModel(*this, Fake, Catalog);

	int32 BatchChanges = 0;
	int32 InfoNotices = 0;
	int32 ErrorNotices = 0;
	Model->OnBatchChanged.AddLambda([&BatchChanges]() { ++BatchChanges; });
	Model->OnNotice.AddLambda([&](const FText&, bool bIsError)
	{
		(bIsError ? ErrorNotices : InfoNotices) += 1;
	});

	// 没写提示词：提交不了，说清楚缺什么。
	Model->SetPrompt(TEXT("   "));
	{
		FText Why;
		TestFalse(TEXT("空提示词提交不了"), Model->CanSubmit(&Why));
		TestTrue(TEXT("原因是缺提示词"), Why.ToString().Contains(TEXT("prompt")));
	}
	Model->Submit();
	TestEqual(TEXT("没有发请求"), Fake.SubmitRequests.Num(), 0);
	TestEqual(TEXT("提示了一次错误"), ErrorNotices, 1);

	// 提交：发出的请求对得上表单。
	Model->SetPrompt(TEXT("mossy rocks"));
	Model->SetVariants(3);
	Model->SetRandomSeed(false);
	Model->SetFixedSeed(777);
	Model->Submit();
	if (!TestEqual(TEXT("发出一个提交请求"), Fake.SubmitRequests.Num(), 1))
	{
		return false;
	}
	{
		const FDreamTextureGenerateRequest& Request = Fake.SubmitRequests[0];
		TestEqual(TEXT("组合 id"), Request.WorkflowId, FString(TEXT("src-local-realistic-v1.dec-chord-v1")));
		TestEqual(TEXT("变体数"), Request.Variants, 3);
		TestEqual(TEXT("提示词"), Request.StringParams.FindRef(TEXT("prompt")), FString(TEXT("mossy rocks")));
		TestEqual(TEXT("种子"), Request.IntParams.FindRef(TEXT("seed")), static_cast<int64>(777));
		TestEqual(TEXT("分辨率"), Request.IntParams.FindRef(TEXT("resolution")), static_cast<int64>(1024));
	}
	TestTrue(TEXT("提交在途"), Model->IsSubmitting());
	TestEqual(TEXT("应答前还没有行"), Model->GetRows().Num(), 0);

	// 在途时不能再提交，也不重复发。
	{
		FText Why;
		TestFalse(TEXT("在途时按钮不可用"), Model->CanSubmit(&Why));
		TestTrue(TEXT("原因是正在提交"), Why.ToString().Contains(TEXT("Submitting")));
	}
	Model->Submit();
	TestEqual(TEXT("在途时不重复发"), Fake.SubmitRequests.Num(), 1);

	// 应答回来：三个变体，按序号排。
	TArray<FDreamTextureJob> Jobs;
	for (int32 i = 1; i <= 3; ++i)
	{
		Jobs.Add(GenViewMakeJob(FString::Printf(TEXT("j%d"), i), EDreamTextureJobStatus::Queued, 0.0f, TEXT("batchX")));
	}
	Fake.CompleteSubmit(0, MoveTemp(Jobs));
	TestFalse(TEXT("提交结束"), Model->IsSubmitting());
	TestEqual(TEXT("批次 id"), Model->GetBatchId(), FString(TEXT("batchX")));
	if (TestEqual(TEXT("三行"), Model->GetRows().Num(), 3))
	{
		TestEqual(TEXT("第一行序号"), Model->GetRows()[0]->Number, 1);
		TestEqual(TEXT("第三行任务"), Model->GetRows()[2]->JobId, FString(TEXT("j3")));
	}
	TestTrue(TEXT("广播了批次变化"), BatchChanges >= 1);
	TestEqual(TEXT("成功提示一次"), InfoNotices, 1);
	TestTrue(TEXT("错误提示没增加"), ErrorNotices == 1);
	TestEqual(TEXT("追踪器里登记了三个"), Fake.Tracker->GetNumTracked(), 3);
	TestEqual(TEXT("生成页的最近一批"), Fake.Tracker->GetLatestBatchId(TEXT("generate")), FString(TEXT("batchX")));
	TestTrue(TEXT("没有提交错误"), Model->GetSubmitError().IsEmpty());
	TestTrue(TEXT("本批有任务没结束"), Model->HasActiveJobs());
	TestEqual(TEXT("都没结束"), Model->GetNumFinished(), 0);

	// 失败：不动已有的批，记下原因，错误提示一次。
	Model->Submit();
	TestEqual(TEXT("又发了一个请求"), Fake.SubmitRequests.Num(), 2);
	Fake.SubmitDones[1](false, TArray<FDreamTextureJob>(), TEXT("提交失败：任务队列已满"));
	TestFalse(TEXT("失败后不再在途"), Model->IsSubmitting());
	TestEqual(TEXT("上一批还在"), Model->GetRows().Num(), 3);
	TestEqual(TEXT("记下原因"), Model->GetSubmitError().ToString(), FString(TEXT("提交失败：任务队列已满")));
	TestEqual(TEXT("错误提示又一次"), ErrorNotices, 2);

	// 再次提交成功：清掉旧的错误，换成新的一批。
	Model->Submit();
	{
		TArray<FDreamTextureJob> NewJobs;
		NewJobs.Add(GenViewMakeJob(TEXT("k1"), EDreamTextureJobStatus::Queued, 0.0f, TEXT("batchY")));
		Fake.CompleteSubmit(2, MoveTemp(NewJobs));
	}
	TestTrue(TEXT("旧错误清掉"), Model->GetSubmitError().IsEmpty());
	TestEqual(TEXT("换成新一批"), Model->GetBatchId(), FString(TEXT("batchY")));
	TestEqual(TEXT("一行"), Model->GetRows().Num(), 1);
	TestEqual(TEXT("生成页的最近一批更新"), Fake.Tracker->GetLatestBatchId(TEXT("generate")), FString(TEXT("batchY")));

	// 面板先关、应答后到：不能崩，任务仍然登记在追踪器里。
	{
		FFakeBackend Fake2;
		FDreamTextureWorkflowCatalog Catalog2;
		TSharedPtr<FDreamTextureGenerateModel> Short = MakeReadyModel(*this, Fake2, Catalog2);
		Short->Submit();
		Short.Reset();
		TArray<FDreamTextureJob> Late;
		Late.Add(GenViewMakeJob(TEXT("late1"), EDreamTextureJobStatus::Queued, 0.0f, TEXT("batchZ")));
		Fake2.CompleteSubmit(0, MoveTemp(Late));
		TestEqual(TEXT("模型没了，任务仍在追踪器里"), Fake2.Tracker->GetNumTracked(), 1);
	}

	return true;
}

// ---------------------------------------------------------------------------
// 一张变体卡片在各状态下怎么说
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateRowViewTest, "DreamTexture.Editor.Generate.Model.Row",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateRowViewTest::RunTest(const FString& Parameters)
{
	using EKind = FDreamTextureGenerateRowView::EKind;
	using EImport = FDreamTextureGenerateRowView::EImport;

	auto Eval = [](int32 Number, const FDreamTextureJob* Job, bool bCancel = false, const FDreamTextureImportStatus* Import = nullptr)
	{
		return FDreamTextureGenerateModel::EvaluateRow(Number, Job, bCancel, Import);
	};

	// 追踪器里没有这个任务。
	{
		const FDreamTextureGenerateRowView View = Eval(2, nullptr);
		TestTrue(TEXT("没有任务是未知"), View.Kind == EKind::Unknown);
		TestTrue(TEXT("序号在标题里"), View.Title.ToString().Contains(TEXT("2")));
		TestFalse(TEXT("不能取消也不能导入"), View.bCanCancel || View.bCanImport);
	}

	// 排队：没有进度条，能取消。
	{
		FDreamTextureJob Job = GenViewMakeJob(TEXT("j"), EDreamTextureJobStatus::Queued);
		const FDreamTextureGenerateRowView View = Eval(1, &Job);
		TestTrue(TEXT("排队"), View.Kind == EKind::Waiting);
		TestTrue(TEXT("阶段是排队中"), View.Stage.ToString().Contains(TEXT("Queued")));
		TestFalse(TEXT("排队不画进度条"), View.bShowProgress);
		TestTrue(TEXT("可取消"), View.bCanCancel);
		TestFalse(TEXT("不可导入"), View.bCanImport);
		TestTrue(TEXT("副标题带种子"), View.Subtitle.ToString().Contains(TEXT("Seed 42")));
		TestTrue(TEXT("副标题带分辨率"), View.Subtitle.ToString().Contains(TEXT("1024²")));
	}

	// 运行：阶段 + 进度。
	{
		FDreamTextureJob Job = GenViewMakeJob(TEXT("j"), EDreamTextureJobStatus::Running, 0.45f);
		Job.Stage = TEXT("生成中");
		const FDreamTextureGenerateRowView View = Eval(1, &Job);
		TestTrue(TEXT("运行"), View.Kind == EKind::Running);
		TestEqual(TEXT("后端的阶段名换成本地化文字"), View.Stage.ToString(), FString(TEXT("Generating")));
		TestTrue(TEXT("画进度条"), View.bShowProgress);
		if (TestTrue(TEXT("进度有值"), View.Percent.IsSet()))
		{
			TestTrue(TEXT("进度 0.45"), FMath::IsNearlyEqual(View.Percent.GetValue(), 0.45f));
		}
		TestTrue(TEXT("可取消"), View.bCanCancel);
	}

	// 等 ComfyUI / 提交中：没有采样进度可言，进度条走跑马灯（不给假进度）。
	for (const TCHAR* Stage : { TEXT("等待 ComfyUI"), TEXT("提交中") })
	{
		FDreamTextureJob Job = GenViewMakeJob(TEXT("j"), EDreamTextureJobStatus::Running, 0.0f);
		Job.Stage = Stage;
		const FDreamTextureGenerateRowView View = Eval(1, &Job);
		TestTrue(*FString::Printf(TEXT("%s：画进度条"), Stage), View.bShowProgress);
		TestFalse(*FString::Printf(TEXT("%s：没有进度值（跑马灯）"), Stage), View.Percent.IsSet());
		TestEqual(*FString::Printf(TEXT("%s：阶段换成本地化文字"), Stage), View.Stage.ToString(), FDreamTextureJob::LocalizeStage(Stage).ToString());
		TestNotEqual(*FString::Printf(TEXT("%s：不再原样显示后端的中文"), Stage), View.Stage.ToString(), FString(Stage));
	}

	// 没给阶段的运行中任务：有兜底说法。
	{
		FDreamTextureJob Job = GenViewMakeJob(TEXT("j"), EDreamTextureJobStatus::Running, 0.1f);
		TestFalse(TEXT("阶段兜底不为空"), Eval(1, &Job).Stage.IsEmpty());
	}

	// 已请求取消、还没结束：取消中，不能再点，跑马灯。
	{
		FDreamTextureJob Job = GenViewMakeJob(TEXT("j"), EDreamTextureJobStatus::Running, 0.45f);
		const FDreamTextureGenerateRowView View = Eval(1, &Job, /*bCancel*/ true);
		TestTrue(TEXT("取消中"), View.Kind == EKind::Canceling);
		TestFalse(TEXT("不能重复取消"), View.bCanCancel);
		TestTrue(TEXT("画进度条"), View.bShowProgress);
		TestFalse(TEXT("但没有进度值"), View.Percent.IsSet());
	}

	// 完成：可导入；没材质 id 的完成不可导入（没东西可导）。
	{
		FDreamTextureJob Job = GenViewMakeJob(TEXT("j"), EDreamTextureJobStatus::Succeeded, 1.0f);
		const FDreamTextureGenerateRowView View = Eval(1, &Job);
		TestTrue(TEXT("完成"), View.Kind == EKind::Done);
		TestEqual(TEXT("阶段"), View.Stage.ToString(), FString(TEXT("Done")));
		TestTrue(TEXT("可导入"), View.bCanImport);
		TestFalse(TEXT("不可取消"), View.bCanCancel);
		TestTrue(TEXT("没导入过"), View.Import == EImport::None);
		TestEqual(TEXT("按钮叫导入"), View.ImportButton.ToString(), FString(TEXT("Import")));

		Job.MaterialId.Reset();
		TestFalse(TEXT("没材质 id 不可导入"), Eval(1, &Job).bCanImport);
	}

	// 已请求取消但已经完成：终态说了算。
	{
		FDreamTextureJob Job = GenViewMakeJob(TEXT("j"), EDreamTextureJobStatus::Succeeded, 1.0f);
		TestTrue(TEXT("完成就是完成，不再是取消中"), Eval(1, &Job, true).Kind == EKind::Done);
	}

	// 失败：带原因，原因为空时有兜底。
	{
		FDreamTextureJob Job = GenViewMakeJob(TEXT("j"), EDreamTextureJobStatus::Failed, 0.2f);
		Job.Error = TEXT("工作流校验未通过:\n缺少节点 X");
		FDreamTextureGenerateRowView View = Eval(1, &Job);
		TestTrue(TEXT("失败"), View.Kind == EKind::Failed);
		TestEqual(TEXT("原因原样"), View.Detail.ToString(), Job.Error);
		TestFalse(TEXT("不可导入"), View.bCanImport);
		TestFalse(TEXT("不可取消"), View.bCanCancel);
		TestFalse(TEXT("不画进度条"), View.bShowProgress);

		Job.Error.Reset();
		TestFalse(TEXT("原因为空也有说法"), Eval(1, &Job).Detail.IsEmpty());
	}

	// 取消：显示「已取消」，与失败是两种说法。
	{
		FDreamTextureJob Job = GenViewMakeJob(TEXT("j"), EDreamTextureJobStatus::Canceled, 0.3f);
		Job.Error = TEXT("已取消");
		const FDreamTextureGenerateRowView View = Eval(1, &Job);
		TestTrue(TEXT("已取消"), View.Kind == EKind::Canceled);
		TestEqual(TEXT("阶段"), View.Stage.ToString(), FString(TEXT("Canceled")));
		TestTrue(TEXT("后端那句通用的「已取消」不再重复显示"), View.Detail.IsEmpty());
		TestFalse(TEXT("不可取消"), View.bCanCancel);
	}

	// 后端给了不认识的状态：按未知处理，不当完成。
	{
		FDreamTextureJob Job = GenViewMakeJob(TEXT("j"), EDreamTextureJobStatus::Unknown);
		const FDreamTextureGenerateRowView View = Eval(1, &Job);
		TestTrue(TEXT("未知"), View.Kind == EKind::Unknown);
		TestFalse(TEXT("不可导入"), View.bCanImport);
	}

	// 导入各阶段（只在完成的变体上谈）。
	{
		FDreamTextureJob Job = GenViewMakeJob(TEXT("j"), EDreamTextureJobStatus::Succeeded, 1.0f);
		FDreamTextureImportStatus Status;
		Status.MaterialId = Job.MaterialId;

		for (const EDreamTextureImportStage Stage : { EDreamTextureImportStage::Queued, EDreamTextureImportStage::FetchingManifest,
			EDreamTextureImportStage::Downloading, EDreamTextureImportStage::Importing })
		{
			Status.Stage = Stage;
			Status.FilesDone = 2;
			Status.FilesTotal = 4;
			const FDreamTextureGenerateRowView View = Eval(1, &Job, false, &Status);
			TestTrue(TEXT("导入进行中"), View.Import == EImport::Active);
			TestFalse(TEXT("导入进行中不让再点"), View.bCanImport);
			TestFalse(TEXT("有说法"), View.ImportText.IsEmpty());
		}
		Status.Stage = EDreamTextureImportStage::Downloading;
		TestTrue(TEXT("下载中带计数"), Eval(1, &Job, false, &Status).ImportText.ToString().Contains(TEXT("2/4")));

		Status.Stage = EDreamTextureImportStage::Succeeded;
		Status.Result.bOK = true;
		Status.Result.AssetFolder = TEXT("/Game/DreamTexture/X_1");
		{
			const FDreamTextureGenerateRowView View = Eval(1, &Job, false, &Status);
			TestTrue(TEXT("已导入"), View.Import == EImport::Succeeded);
			TestEqual(TEXT("按钮变成重新导入"), View.ImportButton.ToString(), FString(TEXT("Re-import")));
			TestTrue(TEXT("可以再导"), View.bCanImport);
			TestEqual(TEXT("提示里是资产目录"), View.ImportTooltip.ToString(), FString(TEXT("/Game/DreamTexture/X_1")));
		}

		// bOK 为假的「完成」（全部跳过之类）按失败画。
		Status.Result.bOK = false;
		Status.Result.Error = TEXT("全部通道都被跳过");
		{
			const FDreamTextureGenerateRowView View = Eval(1, &Job, false, &Status);
			TestTrue(TEXT("按失败画"), View.Import == EImport::Failed);
			TestEqual(TEXT("原因在提示里"), View.ImportTooltip.ToString(), FString(TEXT("全部通道都被跳过")));
		}

		Status.Stage = EDreamTextureImportStage::Failed;
		Status.Message = TEXT("下载失败");
		{
			const FDreamTextureGenerateRowView View = Eval(1, &Job, false, &Status);
			TestTrue(TEXT("导入失败"), View.Import == EImport::Failed);
			TestEqual(TEXT("按钮变成重试"), View.ImportButton.ToString(), FString(TEXT("Retry")));
			TestEqual(TEXT("原因"), View.ImportTooltip.ToString(), FString(TEXT("下载失败")));
			TestTrue(TEXT("可以重试"), View.bCanImport);
		}

		// 任务没完成时，导入状态无关。
		FDreamTextureJob Running = GenViewMakeJob(TEXT("j"), EDreamTextureJobStatus::Running, 0.5f);
		TestTrue(TEXT("没完成不谈导入"), Eval(1, &Running, false, &Status).Import == EImport::None);
	}

	return true;
}

// ---------------------------------------------------------------------------
// 模型：进度推进、取消、重连补状态、恢复上一批
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateModelProgressTest, "DreamTexture.Editor.Generate.Model.Progress",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateModelProgressTest::RunTest(const FString& Parameters)
{
	using EKind = FDreamTextureGenerateRowView::EKind;

	FFakeBackend Fake;
	FDreamTextureWorkflowCatalog Catalog;
	TSharedRef<FDreamTextureGenerateModel> Model = MakeReadyModel(*this, Fake, Catalog);
	const TArray<FString> Ids = SubmitBatch(Fake, *Model, 2);
	if (!TestEqual(TEXT("两个变体"), Ids.Num(), 2))
	{
		return false;
	}

	auto ViewOf = [&Model](int32 Index) { return Model->GetRowView(Model->GetRows()[Index]); };

	TestTrue(TEXT("一开始排队"), ViewOf(0).Kind == EKind::Waiting);
	TestTrue(TEXT("本批有任务没结束"), Model->HasActiveJobs());

	// 事件推进：变体 1 开始跑。
	FDreamTextureJob Running = GenViewMakeJob(Ids[0], EDreamTextureJobStatus::Running, 0.3f);
	Running.Stage = TEXT("生成中");
	Fake.Tracker->Track(Running);
	{
		const FDreamTextureGenerateRowView View = ViewOf(0);
		TestTrue(TEXT("变体 1 运行中"), View.Kind == EKind::Running);
		TestTrue(TEXT("进度 0.3"), View.Percent.IsSet() && FMath::IsNearlyEqual(View.Percent.GetValue(), 0.3f));
	}
	TestTrue(TEXT("变体 2 仍在排队"), ViewOf(1).Kind == EKind::Waiting);

	// 进度往前走；迟到的旧进度不倒退。
	Running.Progress = 0.7f;
	Fake.Tracker->Track(Running);
	TestTrue(TEXT("进度推进到 0.7"), FMath::IsNearlyEqual(ViewOf(0).Percent.GetValue(), 0.7f));
	Running.Progress = 0.4f;
	Fake.Tracker->Track(Running);
	TestTrue(TEXT("旧进度不倒退"), FMath::IsNearlyEqual(ViewOf(0).Percent.GetValue(), 0.7f));

	// 完成。
	Fake.Tracker->Track(GenViewMakeJob(Ids[0], EDreamTextureJobStatus::Succeeded, 1.0f));
	TestTrue(TEXT("变体 1 完成"), ViewOf(0).Kind == EKind::Done);
	TestTrue(TEXT("可导入"), ViewOf(0).bCanImport);
	TestEqual(TEXT("一个已结束"), Model->GetNumFinished(), 1);
	TestTrue(TEXT("还有没结束的"), Model->HasActiveJobs());

	// 变体 2 失败。
	FDreamTextureJob Failed = GenViewMakeJob(Ids[1], EDreamTextureJobStatus::Failed, 0.2f);
	Failed.Error = TEXT("ComfyUI 报错");
	Fake.Tracker->Track(Failed);
	TestTrue(TEXT("变体 2 失败"), ViewOf(1).Kind == EKind::Failed);
	TestEqual(TEXT("原因"), ViewOf(1).Detail.ToString(), FString(TEXT("ComfyUI 报错")));
	TestEqual(TEXT("两个都结束了"), Model->GetNumFinished(), 2);
	TestFalse(TEXT("没有没结束的"), Model->HasActiveJobs());

	// 导入状态在完成的卡片上显示。
	{
		FDreamTextureImportStatus Status;
		Status.MaterialId = TEXT("mat_") + Ids[0];
		Status.Stage = EDreamTextureImportStage::Downloading;
		Status.FilesDone = 1;
		Status.FilesTotal = 4;
		Fake.ImportStatuses.Add(Status.MaterialId, Status);
		TestTrue(TEXT("导入进行中"), ViewOf(0).Import == FDreamTextureGenerateRowView::EImport::Active);

		Status.Stage = EDreamTextureImportStage::Succeeded;
		Status.Result.bOK = true;
		Fake.ImportStatuses.Add(Status.MaterialId, Status);
		TestTrue(TEXT("已导入"), ViewOf(0).Import == FDreamTextureGenerateRowView::EImport::Succeeded);
		TestTrue(TEXT("失败的变体不显示导入"), ViewOf(1).Import == FDreamTextureGenerateRowView::EImport::None);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateModelCancelTest, "DreamTexture.Editor.Generate.Model.Cancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateModelCancelTest::RunTest(const FString& Parameters)
{
	using EKind = FDreamTextureGenerateRowView::EKind;

	FFakeBackend Fake;
	FDreamTextureWorkflowCatalog Catalog;
	TSharedRef<FDreamTextureGenerateModel> Model = MakeReadyModel(*this, Fake, Catalog);
	const TArray<FString> Ids = SubmitBatch(Fake, *Model, 3);
	if (!TestEqual(TEXT("三个变体"), Ids.Num(), 3))
	{
		return false;
	}

	auto ViewOf = [&Model](int32 Index) { return Model->GetRowView(Model->GetRows()[Index]); };

	// 变体 1 在跑、2 在排队、3 已完成。
	Fake.Tracker->Track(GenViewMakeJob(Ids[0], EDreamTextureJobStatus::Running, 0.4f));
	Fake.Tracker->Track(GenViewMakeJob(Ids[2], EDreamTextureJobStatus::Succeeded, 1.0f));

	TArray<FString> ErrorNotices;
	Model->OnNotice.AddLambda([&ErrorNotices](const FText& Message, bool bIsError)
	{
		if (bIsError)
		{
			ErrorNotices.Add(Message.ToString());
		}
	});

	// 取消变体 1：发请求，立刻显示取消中。
	Model->CancelJob(Ids[0]);
	if (TestEqual(TEXT("发出取消请求"), Fake.CancelIds.Num(), 1))
	{
		TestEqual(TEXT("取消的是变体 1"), Fake.CancelIds[0], Ids[0]);
	}
	TestTrue(TEXT("显示取消中"), ViewOf(0).Kind == EKind::Canceling);

	// 重复取消、取消已完成的：不发请求。
	Model->CancelJob(Ids[0]);
	Model->CancelJob(Ids[2]);
	TestEqual(TEXT("重复取消与取消终态不发请求"), Fake.CancelIds.Num(), 1);

	// 全部取消：只取消还没结束、且没请求过的（变体 2）。
	Model->CancelBatch();
	if (TestEqual(TEXT("又发了变体 2 的取消"), Fake.CancelIds.Num(), 2))
	{
		TestEqual(TEXT("取消的是变体 2"), Fake.CancelIds[1], Ids[1]);
	}

	// 变体 2（排队中）取消成功：补一次状态，立刻拿到 canceled。
	Fake.CancelDones[1](true, FString());
	if (TestEqual(TEXT("取消成功后补状态"), Fake.Gets.Num(), 1))
	{
		FDreamTextureJob Canceled = GenViewMakeJob(Ids[1], EDreamTextureJobStatus::Canceled);
		Canceled.Error = TEXT("已取消");
		Fake.CompleteGet(0, Canceled);
		TestTrue(TEXT("变体 2 已取消"), ViewOf(1).Kind == EKind::Canceled);
	}

	// 变体 1（正在跑）：请求成功但要等 ComfyUI 被打断，仍是取消中；随后事件到达变成已取消。
	Fake.CancelDones[0](true, FString());
	TestTrue(TEXT("仍是取消中"), ViewOf(0).Kind == EKind::Canceling);
	FDreamTextureJob Canceled1 = GenViewMakeJob(Ids[0], EDreamTextureJobStatus::Canceled, 0.4f);
	Canceled1.Error = TEXT("已取消");
	Fake.Tracker->Track(Canceled1);
	TestTrue(TEXT("变体 1 已取消"), ViewOf(0).Kind == EKind::Canceled);
	TestFalse(TEXT("没有没结束的"), Model->HasActiveJobs());

	// 取消失败：提示原因，回到原状（可以再点）。
	{
		FFakeBackend Fake2;
		FDreamTextureWorkflowCatalog Catalog2;
		TSharedRef<FDreamTextureGenerateModel> Model2 = MakeReadyModel(*this, Fake2, Catalog2);
		const TArray<FString> Ids2 = SubmitBatch(Fake2, *Model2, 1);
		Fake2.Tracker->Track(GenViewMakeJob(Ids2[0], EDreamTextureJobStatus::Running, 0.5f));

		TArray<FString> Errors2;
		Model2->OnNotice.AddLambda([&Errors2](const FText& Message, bool bIsError)
		{
			if (bIsError)
			{
				Errors2.Add(Message.ToString());
			}
		});
		Model2->CancelJob(Ids2[0]);
		TestTrue(TEXT("取消中"), Model2->GetRowView(Model2->GetRows()[0]).Kind == EKind::Canceling);
		Fake2.CancelDones[0](false, TEXT("取消失败：任务已结束"));
		if (TestEqual(TEXT("提示了原因"), Errors2.Num(), 1))
		{
			TestEqual(TEXT("原因原样"), Errors2[0], FString(TEXT("取消失败：任务已结束")));
		}
		TestTrue(TEXT("回到运行中"), Model2->GetRowView(Model2->GetRows()[0]).Kind == EKind::Running);
		TestTrue(TEXT("可以再取消"), Model2->GetRowView(Model2->GetRows()[0]).bCanCancel);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateModelReconnectTest, "DreamTexture.Editor.Generate.Model.Reconnect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateModelReconnectTest::RunTest(const FString& Parameters)
{
	using EKind = FDreamTextureGenerateRowView::EKind;
	using ECatalog = FDreamTextureGenerateModel::ECatalogState;

	FFakeBackend Fake;
	FDreamTextureWorkflowCatalog Catalog;
	TSharedRef<FDreamTextureGenerateModel> Model = MakeReadyModel(*this, Fake, Catalog);
	const TArray<FString> Ids = SubmitBatch(Fake, *Model, 2);
	if (!TestEqual(TEXT("两个变体"), Ids.Num(), 2))
	{
		return false;
	}
	Fake.Tracker->Track(GenViewMakeJob(Ids[0], EDreamTextureJobStatus::Running, 0.5f));

	auto ViewOf = [&Model](int32 Index) { return Model->GetRowView(Model->GetRows()[Index]); };

	// 断线期间变体 1 已经跑完、变体 2 也失败了：事件收不到。重新连上，追踪器把没结束的各问一遍，补上。
	Fake.Tracker->HandleLinkChanged(EDreamTextureLink::Stale);
	TestEqual(TEXT("断线期间不问"), Fake.Gets.Num(), 0);
	Fake.Tracker->HandleLinkChanged(EDreamTextureLink::Live);
	if (!TestEqual(TEXT("重连后问了两个没结束的"), Fake.Gets.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("补状态前仍是旧的"), ViewOf(0).Kind == EKind::Running);

	Fake.CompleteGet(0, GenViewMakeJob(Ids[0], EDreamTextureJobStatus::Succeeded, 1.0f));
	FDreamTextureJob Failed = GenViewMakeJob(Ids[1], EDreamTextureJobStatus::Failed, 0.1f);
	Failed.Error = TEXT("ComfyUI 重启过，任务丢了");
	Fake.CompleteGet(1, Failed);
	TestTrue(TEXT("变体 1 补成完成"), ViewOf(0).Kind == EKind::Done);
	TestTrue(TEXT("变体 2 补成失败"), ViewOf(1).Kind == EKind::Failed);
	TestEqual(TEXT("失败原因显示得出来"), ViewOf(1).Detail.ToString(), FString(TEXT("ComfyUI 重启过，任务丢了")));

	// 用户点「同步」：对没结束的任务再问一遍（这里都结束了，什么都不问）。
	const int32 GetsBefore = Fake.Gets.Num();
	Model->SyncJobs();
	TestEqual(TEXT("都结束了，同步什么都不问"), Fake.Gets.Num(), GetsBefore);

	// 面板关掉再打开：新模型从追踪器里找回上一批，进度条接着走。
	{
		TSharedRef<FDreamTextureGenerateModel> Reopened = MakeShared<FDreamTextureGenerateModel>(Fake.Make());
		int32 BatchChanges = 0;
		Reopened->OnBatchChanged.AddLambda([&BatchChanges]() { ++BatchChanges; });
		TestEqual(TEXT("初始没有批"), Reopened->GetRows().Num(), 0);
		Reopened->Initialize(false);
		TestEqual(TEXT("找回了批次 id"), Reopened->GetBatchId(), FString(TEXT("bA")));
		if (TestEqual(TEXT("找回了两行"), Reopened->GetRows().Num(), 2))
		{
			TestTrue(TEXT("第一行是完成"), Reopened->GetRowView(Reopened->GetRows()[0]).Kind == EKind::Done);
			TestTrue(TEXT("第二行是失败"), Reopened->GetRowView(Reopened->GetRows()[1]).Kind == EKind::Failed);
		}
		TestTrue(TEXT("广播了批次变化"), BatchChanges >= 1);
	}

	// 面板重新打开时批里还有没结束的任务：对它们各问一次，补上关着期间可能丢掉的事件。
	{
		FFakeBackend Fake3;
		FDreamTextureWorkflowCatalog Catalog3;
		TSharedRef<FDreamTextureGenerateModel> First = MakeReadyModel(*this, Fake3, Catalog3);
		const TArray<FString> Ids3 = SubmitBatch(Fake3, *First, 2);
		Fake3.Tracker->Track(GenViewMakeJob(Ids3[0], EDreamTextureJobStatus::Succeeded, 1.0f));
		const int32 GetsBeforeReopen = Fake3.Gets.Num();

		TSharedRef<FDreamTextureGenerateModel> Reopened = MakeShared<FDreamTextureGenerateModel>(Fake3.Make());
		Reopened->Initialize(false);
		if (TestEqual(TEXT("重新打开时对没结束的那个问了一次"), Fake3.Gets.Num(), GetsBeforeReopen + 1))
		{
			TestEqual(TEXT("问的是没结束的"), Fake3.Gets.Last().Id, Ids3[1]);
		}
	}

	// 目录没拉到时，链路变 Live 再拉；拉到了就不重拉。
	{
		FFakeBackend Fake2;
		TSharedRef<FDreamTextureGenerateModel> Model2 = MakeShared<FDreamTextureGenerateModel>(Fake2.Make());
		Model2->Initialize(false);

		Model2->HandleLinkChanged(EDreamTextureLink::Connecting);
		TestEqual(TEXT("非 Live 不拉"), Fake2.CatalogCalls.Num(), 0);

		Model2->HandleLinkChanged(EDreamTextureLink::Live);
		TestEqual(TEXT("Live 时拉目录"), Fake2.CatalogCalls.Num(), 1);
		TestEqual(TEXT("同时拉扩写模型列表"), Fake2.ModelCalls.Num(), 1);

		Fake2.FailCatalog(0, TEXT("连不上"));
		TestTrue(TEXT("失败"), Model2->GetCatalogState() == ECatalog::Failed);
		Model2->HandleLinkChanged(EDreamTextureLink::Live);
		TestEqual(TEXT("失败后下一次 Live 再拉"), Fake2.CatalogCalls.Num(), 2);

		FDreamTextureWorkflowCatalog Fixture;
		LoadCatalogFixture(*this, Fixture);
		Fake2.CompleteCatalog(1, Fixture);
		Model2->HandleLinkChanged(EDreamTextureLink::Live);
		TestEqual(TEXT("拉到了就不重拉"), Fake2.CatalogCalls.Num(), 2);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateModelImportTest, "DreamTexture.Editor.Generate.Model.Import",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateModelImportTest::RunTest(const FString& Parameters)
{
	FFakeBackend Fake;
	FDreamTextureWorkflowCatalog Catalog;
	TSharedRef<FDreamTextureGenerateModel> Model = MakeReadyModel(*this, Fake, Catalog);
	const TArray<FString> Ids = SubmitBatch(Fake, *Model, 2);
	if (!TestEqual(TEXT("两个变体"), Ids.Num(), 2))
	{
		return false;
	}

	int32 Notices = 0;
	Model->OnNotice.AddLambda([&Notices](const FText&, bool) { ++Notices; });
	const int32 NoticesBefore = Notices;

	// 没完成的不能导。
	Model->RequestImport(Ids[0]);
	TestEqual(TEXT("排队中的不排导入"), Fake.Enqueued.Num(), 0);

	// 完成的：排它的材质 id（不是任务 id）。
	Fake.Tracker->Track(GenViewMakeJob(Ids[0], EDreamTextureJobStatus::Succeeded, 1.0f));
	Model->RequestImport(Ids[0]);
	if (TestEqual(TEXT("排了一个导入"), Fake.Enqueued.Num(), 1))
	{
		TestEqual(TEXT("排的是材质 id"), Fake.Enqueued[0], FString(TEXT("mat_")) + Ids[0]);
	}
	TestEqual(TEXT("成功排队不提示"), Notices, NoticesBefore);

	// 已在队列里：说一声。
	Fake.bEnqueueResult = false;
	Model->RequestImport(Ids[0]);
	TestEqual(TEXT("已在队列里提示一声"), Notices, NoticesBefore + 1);

	// 失败的、没有这个任务的：不排。
	FDreamTextureJob Failed = GenViewMakeJob(Ids[1], EDreamTextureJobStatus::Failed);
	Fake.Tracker->Track(Failed);
	Model->RequestImport(Ids[1]);
	Model->RequestImport(TEXT("nonexistent"));
	TestEqual(TEXT("失败的与不存在的都不排"), Fake.Enqueued.Num(), 2);

	// 自动导入开关：读写都走后端函数（测试里不碰项目设置）。
	TestFalse(TEXT("默认关"), Model->GetAutoImport());
	Model->SetAutoImport(true);
	TestTrue(TEXT("设上了"), Model->GetAutoImport());
	TestTrue(TEXT("写进了后端函数"), Fake.bAutoImport);

	return true;
}

// ---------------------------------------------------------------------------
// 模型：扩写
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateModelRefineTest, "DreamTexture.Editor.Generate.Model.Refine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateModelRefineTest::RunTest(const FString& Parameters)
{
	using ERefine = FDreamTextureGenerateModel::ERefineState;

	FFakeBackend Fake;
	FDreamTextureWorkflowCatalog Catalog;
	TSharedRef<FDreamTextureGenerateModel> Model = MakeReadyModel(*this, Fake, Catalog);

	int32 PromptReplaced = 0;
	TArray<FString> ErrorNotices;
	int32 InfoNotices = 0;
	Model->OnPromptReplaced.AddLambda([&PromptReplaced]() { ++PromptReplaced; });
	Model->OnNotice.AddLambda([&](const FText& Message, bool bIsError)
	{
		if (bIsError)
		{
			ErrorNotices.Add(Message.ToString());
		}
		else
		{
			++InfoNotices;
		}
	});

	// 扩写模型列表：列出来的和列不出来的两种。
	Model->LoadRefineModels();
	TestEqual(TEXT("请求了模型列表"), Fake.ModelCalls.Num(), 1);
	Model->LoadRefineModels();
	TestEqual(TEXT("在途时不重复请求"), Fake.ModelCalls.Num(), 1);
	{
		FDreamTextureRefineModels Models;
		Models.Models = { TEXT("gpt-5-mini"), TEXT("gpt-5") };
		Models.Default = TEXT("gpt-5-mini");
		Fake.ModelCalls[0](true, MoveTemp(Models), FString());
	}
	TestEqual(TEXT("两个模型"), Model->GetRefineModels().Num(), 2);
	TestEqual(TEXT("默认模型"), Model->GetRefineDefaultModel(), FString(TEXT("gpt-5-mini")));
	TestTrue(TEXT("没有列举错误"), Model->GetRefineModelsError().IsEmpty());

	{
		// 列不出来不算错：没有下拉可选，默认模型照样能扩写。
		FFakeBackend Fake2;
		TSharedRef<FDreamTextureGenerateModel> Model2 = MakeShared<FDreamTextureGenerateModel>(Fake2.Make());
		Model2->LoadRefineModels();
		FDreamTextureRefineModels Models;
		Models.Default = TEXT("gpt-5-mini");
		Models.Error = TEXT("网关不提供 /models");
		Fake2.ModelCalls[0](true, MoveTemp(Models), FString());
		TestEqual(TEXT("没有模型可选"), Model2->GetRefineModels().Num(), 0);
		TestEqual(TEXT("原因记下"), Model2->GetRefineModelsError(), FString(TEXT("网关不提供 /models")));
		TestEqual(TEXT("默认仍在"), Model2->GetRefineDefaultModel(), FString(TEXT("gpt-5-mini")));
	}

	// 提示词是空的：不发请求，提示一声。
	Model->SetPrompt(TEXT("  "));
	Model->Refine();
	TestEqual(TEXT("空提示词不发请求"), Fake.RefineCalls.Num(), 0);
	TestEqual(TEXT("提示了一声（不是错误）"), InfoNotices, 1);
	TestTrue(TEXT("状态没变"), Model->GetRefineState() == ERefine::Idle);

	// 扩写：材质的提示词带硬约束，所以 purpose 是 texture；选了模型就传下去。
	Model->SetPrompt(TEXT("  苔藓岩石 "));
	Model->SetRefineModel(TEXT("gpt-5"));
	Model->Refine();
	if (!TestEqual(TEXT("发了一个扩写请求"), Fake.RefineCalls.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("传的是去掉空白的提示词"), Fake.RefineCalls[0].Prompt, FString(TEXT("苔藓岩石")));
	TestTrue(TEXT("材质用 texture 的 purpose"), Fake.RefineCalls[0].Purpose == EDreamTextureRefinePurpose::Texture);
	TestEqual(TEXT("传了选的模型"), Fake.RefineCalls[0].Model, FString(TEXT("gpt-5")));
	TestTrue(TEXT("扩写中"), Model->GetRefineState() == ERefine::Working);

	// 在途时不重复发。
	Model->Refine();
	TestEqual(TEXT("在途时不重复发"), Fake.RefineCalls.Num(), 1);

	// 结果回来：摆出来，不直接盖掉输入框。
	{
		FDreamTextureRefineResult Result;
		Result.Prompt = TEXT("mossy fractured ancient rock, top-down, flat lighting");
		Result.Model = TEXT("gpt-5");
		Fake.RefineCalls[0].Done(true, MoveTemp(Result), FString());
	}
	TestTrue(TEXT("有结果"), Model->GetRefineState() == ERefine::HasResult);
	TestEqual(TEXT("结果"), Model->GetRefinedPrompt(), FString(TEXT("mossy fractured ancient rock, top-down, flat lighting")));
	TestEqual(TEXT("结果的模型"), Model->GetRefinedModel(), FString(TEXT("gpt-5")));
	TestEqual(TEXT("输入框没被盖掉"), Model->GetPrompt(), FString(TEXT("  苔藓岩石 ")));
	TestEqual(TEXT("没有触发替换"), PromptReplaced, 0);

	// 用它：替换提示词，通知面板更新输入框。
	Model->AcceptRefine();
	TestEqual(TEXT("提示词被替换"), Model->GetPrompt(), FString(TEXT("mossy fractured ancient rock, top-down, flat lighting")));
	TestEqual(TEXT("通知了面板"), PromptReplaced, 1);
	TestTrue(TEXT("回到空闲"), Model->GetRefineState() == ERefine::Idle);
	Model->AcceptRefine();
	TestEqual(TEXT("没有结果时接受无效"), PromptReplaced, 1);

	// 不用：丢弃，提示词不变；丢弃也作废在飞的那次。
	Model->Refine();
	{
		FDreamTextureRefineResult Result;
		Result.Prompt = TEXT("another");
		Result.Model = TEXT("m");
		Fake.RefineCalls[1].Done(true, MoveTemp(Result), FString());
	}
	Model->DiscardRefine();
	TestTrue(TEXT("丢弃后回到空闲"), Model->GetRefineState() == ERefine::Idle);
	TestEqual(TEXT("提示词没变"), Model->GetPrompt(), FString(TEXT("mossy fractured ancient rock, top-down, flat lighting")));

	Model->Refine();
	Model->DiscardRefine();
	{
		FDreamTextureRefineResult Late;
		Late.Prompt = TEXT("迟到的结果");
		Late.Model = TEXT("m");
		Fake.RefineCalls[2].Done(true, MoveTemp(Late), FString());
	}
	TestTrue(TEXT("丢弃后迟到的结果不再冒出来"), Model->GetRefineState() == ERefine::Idle);
	TestTrue(TEXT("也没有结果文本"), Model->GetRefinedPrompt().IsEmpty());

	// 失败：回到空闲，记下原因，提示错误。
	Model->Refine();
	Fake.RefineCalls[3].Done(false, FDreamTextureRefineResult(), TEXT("扩写失败：网关不提供对话接口"));
	TestTrue(TEXT("失败回到空闲"), Model->GetRefineState() == ERefine::Idle);
	TestEqual(TEXT("记下原因"), Model->GetRefineError(), FString(TEXT("扩写失败：网关不提供对话接口")));
	if (TestEqual(TEXT("错误提示一次"), ErrorNotices.Num(), 1))
	{
		TestEqual(TEXT("提示内容"), ErrorNotices[0], FString(TEXT("扩写失败：网关不提供对话接口")));
	}

	// 再扩写会清掉上一次的错误。
	Model->Refine();
	TestTrue(TEXT("重新扩写清掉旧错误"), Model->GetRefineError().IsEmpty());

	// 模型先销毁、结果后到：不能崩。
	{
		FFakeBackend Fake3;
		FDreamTextureWorkflowCatalog Catalog3;
		TSharedPtr<FDreamTextureGenerateModel> Short = MakeReadyModel(*this, Fake3, Catalog3);
		Short->Refine();
		Short.Reset();
		FDreamTextureRefineResult Result;
		Result.Prompt = TEXT("x");
		Result.Model = TEXT("m");
		Fake3.RefineCalls[0].Done(true, MoveTemp(Result), FString());
		TestTrue(TEXT("模型销毁后回调到达没有崩溃"), true);
	}

	return true;
}

// ---------------------------------------------------------------------------
// 自动导入
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAutoImporterTest, "DreamTexture.Editor.Generate.AutoImport.Trigger",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAutoImporterTest::RunTest(const FString& Parameters)
{
	FFakeBackend Fake;

	bool bEnabled = true;
	TArray<FString> Enqueued;
	bool bEnqueueResult = true;
	FDreamTextureAutoImporter::FHooks Hooks;
	Hooks.IsEnabled = [&bEnabled]() { return bEnabled; };
	Hooks.Enqueue = [&](const FString& MaterialId)
	{
		Enqueued.Add(MaterialId);
		return bEnqueueResult;
	};

	TSharedRef<FDreamTextureAutoImporter> Importer = MakeShared<FDreamTextureAutoImporter>(MoveTemp(Hooks));
	Importer->Start(Fake.Tracker);

	// 排队、运行：不导。
	Fake.Tracker->Track(GenViewMakeJob(TEXT("a"), EDreamTextureJobStatus::Queued));
	Fake.Tracker->Track(GenViewMakeJob(TEXT("a"), EDreamTextureJobStatus::Running, 0.5f));
	TestEqual(TEXT("没完成不导"), Enqueued.Num(), 0);

	// 完成：导它的材质 id。
	Fake.Tracker->Track(GenViewMakeJob(TEXT("a"), EDreamTextureJobStatus::Succeeded, 1.0f));
	if (TestEqual(TEXT("完成后导了一个"), Enqueued.Num(), 1))
	{
		TestEqual(TEXT("导的是材质 id"), Enqueued[0], FString(TEXT("mat_a")));
	}

	// 同一个终态再广播（登记时并入暂存事件、补状态…）：只导一次。
	Importer->HandleJobChanged(GenViewMakeJob(TEXT("a"), EDreamTextureJobStatus::Succeeded, 1.0f));
	Importer->HandleJobChanged(GenViewMakeJob(TEXT("a"), EDreamTextureJobStatus::Succeeded, 1.0f));
	TestEqual(TEXT("每个任务最多触发一次"), Enqueued.Num(), 1);
	TestEqual(TEXT("计数"), Importer->GetNumTriggered(), 1);

	// 失败、取消：不导。
	Fake.Tracker->Track(GenViewMakeJob(TEXT("f"), EDreamTextureJobStatus::Failed));
	Fake.Tracker->Track(GenViewMakeJob(TEXT("c"), EDreamTextureJobStatus::Canceled));
	TestEqual(TEXT("失败与取消不导"), Enqueued.Num(), 1);

	// 别的提交方（回流）的任务：不归这里管。
	Fake.Tracker->Track(GenViewMakeJob(TEXT("r"), EDreamTextureJobStatus::Succeeded, 1.0f), TEXT("reflow"));
	TestEqual(TEXT("别的来源的任务不导"), Enqueued.Num(), 1);

	// 开关关着时完成：完成的那一刻看开关，不追认——事后打开开关不会补导。
	bEnabled = false;
	Fake.Tracker->Track(GenViewMakeJob(TEXT("b"), EDreamTextureJobStatus::Running, 0.9f));
	Fake.Tracker->Track(GenViewMakeJob(TEXT("b"), EDreamTextureJobStatus::Succeeded, 1.0f));
	TestEqual(TEXT("开关关着完成的不导"), Enqueued.Num(), 1);
	bEnabled = true;
	Importer->HandleJobChanged(GenViewMakeJob(TEXT("b"), EDreamTextureJobStatus::Succeeded, 1.0f));
	TestEqual(TEXT("事后打开开关不追认"), Enqueued.Num(), 1);

	// 开着时完成的新任务照常导。
	Fake.Tracker->Track(GenViewMakeJob(TEXT("d"), EDreamTextureJobStatus::Succeeded, 1.0f));
	TestEqual(TEXT("开着时完成的照导"), Enqueued.Num(), 2);

	// 已经在导入队列里（用户抢先点了「导入」）：不是错误，不崩。
	bEnqueueResult = false;
	Fake.Tracker->Track(GenViewMakeJob(TEXT("e"), EDreamTextureJobStatus::Succeeded, 1.0f));
	TestEqual(TEXT("队列说已在里面也尝试过一次"), Enqueued.Num(), 3);

	// 没有材质 id 的完成：没东西可导。
	{
		FDreamTextureJob NoMaterial = GenViewMakeJob(TEXT("n"), EDreamTextureJobStatus::Succeeded, 1.0f);
		NoMaterial.MaterialId.Reset();
		Importer->HandleJobChanged(NoMaterial);
		TestEqual(TEXT("没材质 id 不导"), Enqueued.Num(), 3);
	}

	// 停掉之后不再收；重复 Start 不会双份订阅。
	Importer->Stop();
	Fake.Tracker->Track(GenViewMakeJob(TEXT("z"), EDreamTextureJobStatus::Succeeded, 1.0f));
	TestEqual(TEXT("停掉后不导"), Enqueued.Num(), 3);

	Importer->Start(Fake.Tracker);
	Importer->Start(Fake.Tracker);
	Fake.Tracker->Track(GenViewMakeJob(TEXT("y"), EDreamTextureJobStatus::Succeeded, 1.0f));
	TestEqual(TEXT("重复 Start 只导一次"), Enqueued.Num(), 4);

	// 追踪器先于导入器销毁：不能崩。
	{
		FDreamTextureAutoImporter::FHooks Hooks2;
		Hooks2.IsEnabled = []() { return true; };
		Hooks2.Enqueue = [](const FString&) { return true; };
		TSharedRef<FDreamTextureAutoImporter> Importer2 = MakeShared<FDreamTextureAutoImporter>(MoveTemp(Hooks2));
		TSharedPtr<FDreamTextureJobTracker> Short = MakeShared<FDreamTextureJobTracker>();
		Importer2->Start(Short);
		Short.Reset();
		Importer2->Stop();
		TestTrue(TEXT("追踪器先销毁没有崩溃"), true);
	}

	return true;
}

// ---------------------------------------------------------------------------
// 面板：各状态下构造不崩
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGeneratePanelTest, "DreamTexture.Editor.Generate.Panel.Build",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGeneratePanelTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddWarning(TEXT("Slate 没有初始化（命令行环境），跳过面板构造部分；模型部分见 .Model.* 子测试。"));
		return true;
	}

	using EForm = SDreamTextureGeneratePanel::EFormState;
	using ERefine = FDreamTextureGenerateModel::ERefineState;

	FDreamTextureWorkflowCatalog Fixture;
	if (!LoadCatalogFixture(*this, Fixture))
	{
		return false;
	}

	FFakeBackend Fake;
	TSharedRef<FDreamTextureGenerateModel> Model = MakeShared<FDreamTextureGenerateModel>(Fake.Make());
	Model->Initialize(false);

	TSharedPtr<SDreamTextureGeneratePanel> Panel = SNew(SDreamTextureGeneratePanel)
		.Model(Model)
		.bBindExternal(false)
		.bAutoLoad(false);

	// 1) 还没拉目录：读取中，不是空表单。
	TestTrue(TEXT("初始是读取中"), Panel->GetFormState() == EForm::Loading);
	Panel->BuildWidgetsForTest();

	// 2) 目录请求在途：仍是读取中。
	Model->LoadCatalog();
	TestTrue(TEXT("请求在途是读取中"), Panel->GetFormState() == EForm::Loading);

	// 3) 后端连不上：失败态，带原因与重试，不是空下拉。
	Fake.FailCatalog(0, TEXT("读取工作流列表失败：连不上，后端进程可能没在跑"));
	TestTrue(TEXT("失败态"), Panel->GetFormState() == EForm::Failed);
	Panel->BuildWidgetsForTest();

	// 3.5) 后端进程没应答：配了可执行文件 → 左栏留在表单上（写提示词、点「启动后端并生成」）；
	//      没配 → 仍是失败态，另给跳到设置的入口。
	Fake.Conn->Set(EDreamTextureLink::Offline, false, TEXT("连接被拒绝"));
	TestTrue(TEXT("后端没应答 + 配了可执行文件：表单"), Panel->GetFormState() == EForm::Form);
	TestTrue(TEXT("主按钮是启动后端并生成"), Model->GetPrimaryAction() == FDreamTextureGenerateModel::EPrimaryAction::StartAndSubmit);
	Model->SetPrompt(TEXT("苔藓岩石"));
	Panel->BuildWidgetsForTest();
	Model->ActivatePrimary();
	TestTrue(TEXT("启动中仍是表单"), Panel->GetFormState() == EForm::Form);
	Panel->BuildWidgetsForTest();
	Model->CancelAutoStart();
	Fake.FailEnsure(0, TEXT("启动后端失败：找不到后端可执行文件"));
	Panel->BuildWidgetsForTest();
	Fake.bHasExecutable = false;
	TestTrue(TEXT("没配可执行文件：仍是失败态"), Panel->GetFormState() == EForm::Failed);
	TestTrue(TEXT("并且该给设置入口"), Model->NeedsExecutableSetting());
	Panel->BuildWidgetsForTest();
	Fake.bHasExecutable = true;
	Fake.Conn->SetAllReady();
	Model->SetPrompt(FString());

	// 4) 目录到了：表单。
	Model->LoadCatalog();
	Fake.CompleteCatalog(1, Fixture);
	TestTrue(TEXT("表单态"), Panel->GetFormState() == EForm::Form);
	Panel->BuildWidgetsForTest();
	TestTrue(TEXT("控件树真的算出了尺寸（左栏定宽 344）"), Panel->GetDesiredSize().X > 344.0f);

	// 5) 各种选择：不可商用的组合、云端底图 + 搭不上的提醒、没有这个组合、传统派生。
	Model->SetPrompt(TEXT("苔藓岩石"));
	Panel->BuildWidgetsForTest();
	Model->SetSourceId(TEXT("src-api-stylized-v1"));
	Model->SetDecomposeId(TEXT("dec-chord-v1"));
	Panel->BuildWidgetsForTest();
	Model->SetSourceId(TEXT("src-api-realistic-v1"));
	Panel->BuildWidgetsForTest();

	// 云端底图：清单在途 / 没填令牌 / 连不上 / 有模型 / 失败，各档下构造不崩。
	Model->SetSourceId(TEXT("src-api-stylized-v1"));
	Panel->BuildWidgetsForTest();
	{
		FDreamTextureImagenProvider Provider;
		Provider.Id = TEXT("openai");
		Provider.Label = TEXT("OpenAI");
		FDreamTextureImagenProviders Providers;
		Providers.Providers.Add(Provider);
		Fake.CompleteCloud(Fake.CloudCalls.Num() - 1, Providers);
		Panel->BuildWidgetsForTest();

		Model->LoadCloudModels();
		Providers.Providers[0].bConfigured = true;
		Providers.Providers[0].Error = TEXT("组织未完成身份验证");
		Fake.CompleteCloud(Fake.CloudCalls.Num() - 1, Providers);
		Panel->BuildWidgetsForTest();

		Model->LoadCloudModels();
		Providers.Providers[0].Error.Reset();
		FDreamTextureImagenModel Known;
		Known.Id = TEXT("gpt-image-2");
		Known.Label = TEXT("GPT Image 2");
		Known.Note = TEXT("最新，较慢");
		Known.bKnown = true;
		FDreamTextureImagenModel Unknown;
		Unknown.Id = TEXT("gpt-image-9");
		Providers.Providers[0].Models = { Known, Unknown };
		Fake.CompleteCloud(Fake.CloudCalls.Num() - 1, Providers);
		Panel->BuildWidgetsForTest();

		Model->LoadCloudModels();
		Fake.FailCloud(Fake.CloudCalls.Num() - 1, TEXT("读取云端模型列表失败：连不上"));
		Panel->BuildWidgetsForTest();
	}

	Model->SetSourceId(TEXT("src-local-realistic-v1"));
	Model->SetDecomposeId(TEXT("dec-derive-v1"));
	Panel->BuildWidgetsForTest();

	// 6) 三级状态各个档位下的提交提示。
	Fake.Conn->Set(EDreamTextureLink::Offline, false, TEXT("连接被拒绝"));
	Panel->BuildWidgetsForTest();
	Fake.Conn->Set(EDreamTextureLink::Connecting, true);
	Panel->BuildWidgetsForTest();
	{
		FDreamTextureHealth Health;
		Health.Mode = TEXT("attach");
		Health.bStarting = true;
		Fake.Conn->Set(EDreamTextureLink::Live, true);
		Fake.Conn->SetHealth(Health);
		Panel->BuildWidgetsForTest();
		Health.bStarting = false;
		Health.bAlive = false;
		Fake.Conn->SetHealth(Health);
		Panel->BuildWidgetsForTest();
		Health.bAlive = true;
		Health.Reason = TEXT("队列积压");
		Fake.Conn->SetHealth(Health);
		Panel->BuildWidgetsForTest();
	}
	Fake.Conn->SetAllReady();

	// 7) 提交一批，卡片在各状态下构造不崩。
	const TArray<FString> Ids = SubmitBatch(Fake, *Model, 5);
	Panel->BuildWidgetsForTest();
	TestEqual(TEXT("面板里有五张卡片的行"), Model->GetRows().Num(), 5);

	Fake.Tracker->Track(GenViewMakeJob(Ids[0], EDreamTextureJobStatus::Running, 0.4f));
	FDreamTextureJob Waiting = GenViewMakeJob(Ids[1], EDreamTextureJobStatus::Running, 0.0f);
	Waiting.Stage = TEXT("等待 ComfyUI");
	Fake.Tracker->Track(Waiting);
	Fake.Tracker->Track(GenViewMakeJob(Ids[2], EDreamTextureJobStatus::Succeeded, 1.0f));
	FDreamTextureJob Failed = GenViewMakeJob(Ids[3], EDreamTextureJobStatus::Failed, 0.1f);
	Failed.Error = TEXT("工作流校验未通过");
	Fake.Tracker->Track(Failed);
	Fake.Tracker->Track(GenViewMakeJob(Ids[4], EDreamTextureJobStatus::Canceled, 0.1f));
	Panel->BuildWidgetsForTest();

	Model->CancelJob(Ids[0]);
	Panel->BuildWidgetsForTest();

	FDreamTextureImportStatus Import;
	Import.MaterialId = TEXT("mat_") + Ids[2];
	for (const EDreamTextureImportStage Stage : { EDreamTextureImportStage::Queued, EDreamTextureImportStage::Downloading,
		EDreamTextureImportStage::Importing, EDreamTextureImportStage::Failed })
	{
		Import.Stage = Stage;
		Import.FilesTotal = 4;
		Import.FilesDone = 1;
		Import.Message = TEXT("原因");
		Fake.ImportStatuses.Add(Import.MaterialId, Import);
		Panel->BuildWidgetsForTest();
	}
	Import.Stage = EDreamTextureImportStage::Succeeded;
	Import.Result.bOK = true;
	Import.Result.AssetFolder = TEXT("/Game/DreamTexture/X_1");
	Fake.ImportStatuses.Add(Import.MaterialId, Import);
	Panel->BuildWidgetsForTest();

	// 8) 扩写各状态；接受扩写结果时面板更新输入框。
	Model->Refine();
	TestTrue(TEXT("扩写中"), Model->GetRefineState() == ERefine::Working);
	Panel->BuildWidgetsForTest();
	{
		FDreamTextureRefineResult Result;
		Result.Prompt = TEXT("扩写后的提示词");
		Result.Model = TEXT("gpt-5-mini");
		Fake.RefineCalls.Last().Done(true, MoveTemp(Result), FString());
	}
	TestTrue(TEXT("有结果"), Model->GetRefineState() == ERefine::HasResult);
	Panel->BuildWidgetsForTest();
	Model->AcceptRefine();
	TestEqual(TEXT("提示词被替换"), Model->GetPrompt(), FString(TEXT("扩写后的提示词")));
	Panel->BuildWidgetsForTest();

	Model->Refine();
	Fake.RefineCalls.Last().Done(false, FDreamTextureRefineResult(), TEXT("扩写失败：令牌没配"));
	Panel->BuildWidgetsForTest();

	// 9) 面板先关、回调后到：不能崩。
	Model->Refine();
	Model->LoadCatalog();
	Panel.Reset();
	Fake.RefineCalls.Last().Done(true, FDreamTextureRefineResult(), FString());
	Fake.CompleteCatalog(Fake.CatalogCalls.Num() - 1, Fixture);
	TestTrue(TEXT("面板销毁后回调到达没有崩溃"), true);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
