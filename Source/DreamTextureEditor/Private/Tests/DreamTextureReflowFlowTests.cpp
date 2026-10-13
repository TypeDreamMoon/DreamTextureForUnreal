// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureReflowTestUtils.h"

using namespace DreamTextureReflowTest;

/*
 * 回流一次运行的流程：确保后端 → 导出 → 上传 → 提交 → 生成 → 下载 → 导入 →（原地覆盖时）改写实例。
 *
 * 外部能力全是假的（见 DreamTextureReflowTestUtils.h 的 FRunWorld）：回调当场回或攥在手里，
 * 追踪器是真的、网络层是假的。要验证的是——每一步成功时带着什么往下走，每一步失败时说的是哪一步、说了什么，
 * 取消在每个阶段怎么收尾，结束之后不再对迟到的事件起反应。
 */

namespace
{
	using EStep = EDreamTextureReflowStep;
	using EState = FDreamTextureReflowRun::EState;

	/** 任务完成、导入走到某个阶段。 */
	void FinishJob(FRunWorld& World)
	{
		World.PushJob(MakeJob(World.JobId, EDreamTextureJobStatus::Succeeded, 1.0f));
	}

	FString MaterialIdOf(const FRunWorld& World)
	{
		return TEXT("mat_") + World.JobId;
	}

	/** 失败说明的格式：「在「X」这一步失败：…。」，且结尾不叠标点。 */
	void ExpectFailure(FAutomationTestBase& Test, const TCHAR* What, const FDreamTextureReflowRun& Run, EStep Step, const TCHAR* ExpectedPart)
	{
		Test.TestTrue(FString::Printf(TEXT("%s：状态是失败"), What), Run.GetState() == EState::Failed);
		Test.TestTrue(FString::Printf(TEXT("%s：失败在 %s 这一步（实际 %s）"), What, *DescribeReflowStep(Step).ToString(), *DescribeReflowStep(Run.GetStep()).ToString()), Run.GetStep() == Step);
		const FString Message = Run.GetMessage().ToString();
		Test.TestTrue(FString::Printf(TEXT("%s：说明点名这一步「%s」（%s）"), What, *DescribeReflowStep(Step).ToString(), *Message),
			Message.Contains(FString::Printf(TEXT("Failed at the \"%s\" step"), *DescribeReflowStep(Step).ToString())));
		Test.TestTrue(FString::Printf(TEXT("%s：说明里有原因「%s」（%s）"), What, ExpectedPart, *Message), Message.Contains(ExpectedPart));
		Test.TestFalse(FString::Printf(TEXT("%s：结尾不叠标点（%s）"), What, *Message), Message.Contains(TEXT("..")));
		Test.TestFalse(FString::Printf(TEXT("%s：不把「上传失败」「提交失败」这类前缀叠在步骤名后面（%s）"), What, *Message),
			Message.Contains(TEXT("step: Upload failed")) || Message.Contains(TEXT("step: Could not submit the job")));
		Test.TestTrue(FString::Printf(TEXT("%s：以句号收尾"), What), Message.EndsWith(TEXT(".")));
		Test.TestFalse(FString::Printf(TEXT("%s：失败后没有进度"), What), Run.GetProgress().IsSet());
		Test.TestTrue(FString::Printf(TEXT("%s：已结束"), What), Run.IsFinished() && !Run.IsRunning());
	}
}

// ---------------------------------------------------------------------------
// 成功：新建
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowFlowCreateTest, "DreamTexture.Editor.Reflow.Flow.Create", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowFlowCreateTest::RunTest(const FString& Parameters)
{
	FRunWorld World;
	const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeFillSpec(), World.Make());
	FStepLog Log;
	Log.Bind(*Run);

	TestTrue(TEXT("开始之前是 Idle"), Run->GetState() == EState::Idle);
	TestEqual(TEXT("来源标签是 reflow（自动导入只认 generate，不会把回流的任务误导入成普通材质）"), FString(FDreamTextureReflowRun::GetOrigin()), FString(TEXT("reflow")));

	Run->Start();
	Run->Start();  // 只能开始一次

	// ---- 同步的假世界里，后端 → 导出 → 上传 → 提交一口气走完，停在「生成」 ----
	TestEqual(TEXT("确认后端在：一次"), World.BackendCalls, 1);
	TestEqual(TEXT("只导出一次"), World.ExportCalls, 1);
	if (TestEqual(TEXT("导出时只导底图那一张"), World.ExportAnalyses[0].Textures.Num(), 1))
	{
		TestEqual(TEXT("导的是底色"), World.ExportAnalyses[0].Textures[0].AssetName, FString(TEXT("T_Base")));
	}
	TestEqual(TEXT("导出时长边上限透传"), World.ExportOptions[0].MaxEdge, 2048);
	TestFalse(TEXT("导出到一个新的临时目录"), World.ExportOptions[0].OutputDir.IsEmpty());
	TestEqual(TEXT("记下了导出文件的指纹"), Run->GetExportedSha(), World.Sha);
	if (TestEqual(TEXT("上传了一个文件"), World.UploadedFiles.Num(), 1))
	{
		TestTrue(TEXT("上传的就是导出的那个 PNG"), World.UploadedFiles[0] == Run->GetExportedFile() && World.UploadedFiles[0].EndsWith(TEXT("T_Base.png")));
	}
	TestEqual(TEXT("记下了后端给的文件名"), Run->GetUploadedName(), World.StoredName);

	if (TestEqual(TEXT("提交了一次"), World.SubmitRequests.Num(), 1))
	{
		const FDreamTextureGenerateRequest& Request = World.SubmitRequests[0];
		TestEqual(TEXT("工作流"), Request.WorkflowId, FString(TEXT("src-upload-v1.dec-derive-v1")));
		TestEqual(TEXT("上传得到的文件名填进了 source_image"), Request.StringParams.FindRef(TEXT("source_image")), World.StoredName);
		TestEqual(TEXT("derived_from.maps_in.basecolor 是送上去那份文件的指纹"), Request.DerivedFrom.MapsIn.FindRef(TEXT("basecolor")), World.Sha);
		TestEqual(TEXT("derived_from.mode"), Request.DerivedFrom.Mode, FString(TEXT("fill_channels")));
		TestEqual(TEXT("derived_from.kind"), Request.DerivedFrom.Kind, FString(TEXT("ue_material")));
		TestEqual(TEXT("derived_from.asset_path 是源材质"), Request.DerivedFrom.AssetPath, FString(TEXT("/Game/Test/MI_Rock.MI_Rock")));
		TestEqual(TEXT("derived_from.engine"), Request.DerivedFrom.Engine, FString(TEXT("5.8.2")));
		FString Error;
		TestTrue(FString::Printf(TEXT("提交出去的请求通过 Client 的本地校验（%s）"), *Error), Request.Validate(Error));
		TestEqual(TEXT("一个变体"), Request.Variants, 1);
	}
	TestFalse(TEXT("运行自己的输入里没有被改写：上传文件名只进提交的那份"), Run->GetSpec().Request.StringParams.Contains(TEXT("source_image")));
	TestTrue(TEXT("提交出去的请求可以事后查"), Run->GetSubmittedRequest().StringParams.Contains(TEXT("source_image")));
	TestEqual(TEXT("来源标签"), World.Tracker->GetOrigin(World.JobId), FString(TEXT("reflow")));
	TestEqual(TEXT("任务 id"), Run->GetJobId(), World.JobId);
	TestEqual(TEXT("材质 id"), Run->GetMaterialId(), MaterialIdOf(World));

	TestTrue(TEXT("停在生成"), Run->GetStep() == EStep::Generate && Run->IsRunning());

	// ---- 生成：进度来自任务事件 ----
	World.PushJob(MakeJob(World.JobId, EDreamTextureJobStatus::Running, 0.0f, TEXT("等待 ComfyUI")));
	TestFalse(TEXT("等 ComfyUI 时没有真实进度，不画假进度条"), Run->GetProgress().IsSet());
	TestTrue(TEXT("说明里有阶段（本地化后的）"), Run->GetMessage().ToString().Contains(TEXT("Waiting for ComfyUI")));

	World.PushJob(MakeJob(World.JobId, EDreamTextureJobStatus::Running, 0.45f, TEXT("生成中")));
	if (TestTrue(TEXT("生成中有进度"), Run->GetProgress().IsSet()))
	{
		TestTrue(TEXT("进度 0.45"), FMath::IsNearlyEqual(Run->GetProgress().GetValue(), 0.45f));
	}
	TestTrue(TEXT("说明里带百分比"), Run->GetMessage().ToString().Contains(TEXT("45")));

	// 别的任务的事件不相干。
	World.Tracker->Track(MakeJob(TEXT("job_other"), EDreamTextureJobStatus::Failed, 0.0f, TEXT(""), TEXT("别人的任务")), TEXT("generate"));
	TestTrue(TEXT("别的任务失败不影响这次运行"), Run->IsRunning() && Run->GetStep() == EStep::Generate);

	// ---- 任务完成 → 排导入，带着计划里的选项 ----
	FinishJob(World);
	if (TestEqual(TEXT("排了一次导入"), World.EnqueuedIds.Num(), 1))
	{
		TestEqual(TEXT("导入的是这个任务的材质"), World.EnqueuedIds[0], MaterialIdOf(World));
		const FDreamTextureImportOptions& Options = World.EnqueuedOptions[0];
		TestTrue(TEXT("显式给通道"), Options.bExplicitChannels);
		TestEqual(TEXT("只导入法线、ORM、高度（没有底色）"), FString::Join(Options.Channels, TEXT(",")), FString(TEXT("normal,orm,height")));
		TestEqual(TEXT("底色沿用用户原来那张"), Options.ReuseTextures.FindRef(TEXT("basecolor")), FString(TEXT("/Game/Test/T_Base.T_Base")));
	}
	TestTrue(TEXT("进入下载"), Run->GetStep() == EStep::Download);
	TestEqual(TEXT("生成阶段的订阅放掉了：任务事件不再进来"), World.Tracker->OnJobChanged.IsBound(), false);

	// ---- 下载与导入：别的材质的导入状态不相干 ----
	World.PushImport(MakeImportStatus(TEXT("mat_unrelated"), EDreamTextureImportStage::Failed, TEXT("别人的导入失败了")));
	TestTrue(TEXT("别的材质导入失败不影响这次运行"), Run->IsRunning());

	World.PushImport(MakeImportStatus(MaterialIdOf(World), EDreamTextureImportStage::Queued, TEXT("准备导入…")));
	World.PushImport(MakeImportStatus(MaterialIdOf(World), EDreamTextureImportStage::FetchingManifest, TEXT("正在获取材质信息…")));
	World.PushImport(MakeImportStatus(MaterialIdOf(World), EDreamTextureImportStage::Downloading, TEXT("正在下载贴图 2/3"), 2, 3));
	TestTrue(TEXT("下载中"), Run->GetStep() == EStep::Download);
	if (TestTrue(TEXT("下载有进度"), Run->GetProgress().IsSet()))
	{
		TestTrue(TEXT("进度 2/3"), FMath::IsNearlyEqual(Run->GetProgress().GetValue(), 2.0f / 3.0f, 0.001f));
	}
	TestTrue(TEXT("说明是导入队列的原话"), Run->GetMessage().ToString().Contains(TEXT("2/3")));

	World.PushImport(MakeImportStatus(MaterialIdOf(World), EDreamTextureImportStage::Importing, TEXT("正在导入到工程…")));
	TestTrue(TEXT("导入中"), Run->GetStep() == EStep::Import);

	FDreamTextureImportStatus Done = MakeImportStatus(MaterialIdOf(World), EDreamTextureImportStage::Succeeded, TEXT("导入完成"));
	Done.Result = MakeImportedResult();
	World.PushImport(Done);

	TestTrue(TEXT("成功"), Run->GetState() == EState::Succeeded);
	TestTrue(TEXT("结束了"), Run->IsFinished());
	TestTrue(TEXT("进度满"), Run->GetProgress().IsSet() && FMath::IsNearlyEqual(Run->GetProgress().GetValue(), 1.0f));
	TestEqual(TEXT("结果实例路径是新建的那个"), Run->GetResultInstancePath(), FString(TEXT("/Game/DreamTexture/Rock_ab12/MI_Rock_ab12.MI_Rock_ab12")));
	const FString Message = Run->GetMessage().ToString();
	TestTrue(TEXT("说明里有新实例的名字"), Message.Contains(TEXT("MI_Rock_ab12")));
	TestTrue(TEXT("说明里数出新贴图（3 张，没把沿用的底色算进去）"), Message.Contains(TEXT("3 new texture(s)")));
	TestTrue(TEXT("说明原材质没动"), Message.Contains(TEXT("original material was not touched")));
	TestTrue(TEXT("补通道时说明底色指向原图"), Message.Contains(TEXT("Base Color points at your original texture")));
	TestEqual(TEXT("没有调原地覆盖"), World.ApplyCalls, 0);
	TestEqual(TEXT("导入队列的订阅放掉了"), World.ImportWatchers.Num(), 0);
	TestEqual(TEXT("拉了一次 manifest 给预览用"), World.ManifestCalls, 1);
	TestTrue(TEXT("拿到了 manifest"), Run->HasManifest() && Run->GetManifest().Maps.Contains(TEXT("normal")));

	// ---- 步骤的顺序 ----
	const TArray<EStep> Expected = { EStep::Backend, EStep::Export, EStep::Upload, EStep::Submit, EStep::Generate, EStep::Download, EStep::Import };
	TestTrue(FString::Printf(TEXT("步骤按序走过（实际 %d 个）"), Log.Steps.Num()), Log.Steps == Expected);

	// ---- 结束之后迟到的事件不再起反应 ----
	World.PushJob(MakeJob(World.JobId, EDreamTextureJobStatus::Failed, 0.0f, TEXT(""), TEXT("迟到的失败")));
	TestTrue(TEXT("结束后任务事件不改结论"), Run->GetState() == EState::Succeeded);
	Run->Cancel();
	TestTrue(TEXT("结束后取消什么都不做"), Run->GetState() == EState::Succeeded);
	return true;
}

// ---------------------------------------------------------------------------
// 成功：新建时的措辞随材质实例的结果走
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowFlowKeepAliveTest, "DreamTexture.Editor.Reflow.Flow.KeepAlive", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowFlowKeepAliveTest::RunTest(const FString& Parameters)
{
	// 一次生成动辄几分钟，用户常常点完就把面板关了：运行靠自己活着，跑完放手。
	FRunWorld World;
	TWeakPtr<FDreamTextureReflowRun> Weak;
	{
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeFillSpec(), World.Make());
		Run->KeepAliveWhileRunning();
		Weak = Run;
		TestEqual(TEXT("开始之前没有靠自己活着的运行"), FDreamTextureReflowRun::GetNumKeptAlive(), 0);
		Run->Start();
		TestEqual(TEXT("开始后靠自己活着"), FDreamTextureReflowRun::GetNumKeptAlive(), 1);
	}
	TestTrue(TEXT("外面的引用都放了，运行还活着（面板关了）"), Weak.IsValid());

	World.PushJob(MakeJob(World.JobId, EDreamTextureJobStatus::Running, 0.5f, TEXT("生成中")));
	FinishJob(World);
	FDreamTextureImportStatus Done = MakeImportStatus(MaterialIdOf(World), EDreamTextureImportStage::Succeeded);
	Done.Result = MakeImportedResult();
	World.PushImport(Done);

	TestEqual(TEXT("结束就放手"), FDreamTextureReflowRun::GetNumKeptAlive(), 0);
	TestFalse(TEXT("没人引用了，运行释放了"), Weak.IsValid());
	TestEqual(TEXT("订阅也清干净了"), World.ImportWatchers.Num(), 0);

	// 失败同样放手。
	FRunWorld Failing;
	Failing.bBackendOK = false;
	TWeakPtr<FDreamTextureReflowRun> WeakFailed;
	{
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeFillSpec(), Failing.Make());
		Run->KeepAliveWhileRunning();
		WeakFailed = Run;
		Run->Start();
	}
	TestFalse(TEXT("失败了也放手"), WeakFailed.IsValid());
	TestEqual(TEXT("计数回零"), FDreamTextureReflowRun::GetNumKeptAlive(), 0);
	return true;
}

// ---------------------------------------------------------------------------
// 每一步失败，说的是哪一步
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowFlowFailureTest, "DreamTexture.Editor.Reflow.Flow.Failure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowFlowFailureTest::RunTest(const FString& Parameters)
{
	// ---- 确保后端 ----
	{
		FRunWorld World;
		World.bBackendOK = false;
		World.BackendError = TEXT("dreamtexture.exe 没配路径，没法拉起后端");
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeFillSpec(), World.Make());
		Run->Start();
		ExpectFailure(*this, TEXT("后端"), *Run, EStep::Backend, TEXT("没配路径"));
		TestEqual(TEXT("后端不在就不导出"), World.ExportCalls, 0);
	}

	// ---- 导出 ----
	{
		FRunWorld World;
		World.bExportOK = false;
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeFillSpec(), World.Make());
		Run->Start();
		ExpectFailure(*this, TEXT("导出失败"), *Run, EStep::Export, TEXT("贴图没有源数据"));
		TestTrue(TEXT("说明点名了哪张贴图"), Run->GetMessage().ToString().Contains(TEXT("T_Base")));
		TestEqual(TEXT("导不出就不上传"), World.UploadedFiles.Num(), 0);
	}
	{
		FRunWorld World;
		World.bExportReturnsNothing = true;
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeFillSpec(), World.Make());
		Run->Start();
		ExpectFailure(*this, TEXT("导出没有结果"), *Run, EStep::Export, TEXT("returned no result"));
	}
	{
		FRunWorld World;
		World.Sha = TEXT("not-a-fingerprint");
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeFillSpec(), World.Make());
		Run->Start();
		ExpectFailure(*this, TEXT("导出的指纹不合法"), *Run, EStep::Export, TEXT("fingerprint"));
		TestEqual(TEXT("拿不到合法指纹就不上传（derived_from 会被后端拒掉）"), World.UploadedFiles.Num(), 0);
	}

	// ---- 上传 ----
	{
		FRunWorld World;
		World.bUploadOK = false;
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeFillSpec(), World.Make());
		Run->Start();
		ExpectFailure(*this, TEXT("上传失败"), *Run, EStep::Upload, TEXT("转存到 ComfyUI 失败"));
		TestEqual(TEXT("上传不成就不提交"), World.SubmitRequests.Num(), 0);
		TestEqual(TEXT("上传失败时导出的文件指纹仍记着（排障用）"), Run->GetExportedSha(), World.Sha);
	}

	// ---- 提交 ----
	{
		FRunWorld World;
		World.bSubmitOK = false;
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeFillSpec(), World.Make());
		Run->Start();
		ExpectFailure(*this, TEXT("提交失败"), *Run, EStep::Submit, TEXT("source_image 必填"));
		TestEqual(TEXT("提交失败时订阅也放掉了"), World.Tracker->OnJobChanged.IsBound(), false);
	}
	{
		FRunWorld World;
		FDreamTextureReflowRunHooks Hooks = World.Make();
		Hooks.GetTracker = []() -> TSharedPtr<FDreamTextureJobTracker> { return nullptr; };
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeFillSpec(), MoveTemp(Hooks));
		Run->Start();
		ExpectFailure(*this, TEXT("没有追踪器"), *Run, EStep::Submit, TEXT("tracker"));
	}

	// ---- 生成 ----
	{
		FRunWorld World;
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeFillSpec(), World.Make());
		Run->Start();
		World.PushJob(MakeJob(World.JobId, EDreamTextureJobStatus::Running, 0.3f, TEXT("生成中")));
		World.PushJob(MakeJob(World.JobId, EDreamTextureJobStatus::Failed, 0.3f, TEXT(""), TEXT("ComfyUI 报错：节点 dt.source_scale 缺少输入.")));
		ExpectFailure(*this, TEXT("生成失败"), *Run, EStep::Generate, TEXT("dt.source_scale"));
		TestEqual(TEXT("生成失败不会去导入"), World.EnqueuedIds.Num(), 0);

		// 失败是终态：之后再来「完成」的事件也不翻案。
		FinishJob(World);
		TestTrue(TEXT("失败之后迟到的完成不翻案"), Run->GetState() == EState::Failed);
		TestEqual(TEXT("也不会去导入"), World.EnqueuedIds.Num(), 0);
	}
	{
		FRunWorld World;
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeFillSpec(), World.Make());
		Run->Start();
		World.PushJob(MakeJob(World.JobId, EDreamTextureJobStatus::Failed, 0.0f, TEXT(""), TEXT("")));
		ExpectFailure(*this, TEXT("生成失败但后端没给原因"), *Run, EStep::Generate, TEXT("did not give a reason"));
	}
	{
		FRunWorld World;
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeFillSpec(), World.Make());
		Run->Start();
		World.PushJob(MakeJob(World.JobId, EDreamTextureJobStatus::Canceled, 0.0f, TEXT(""), TEXT("已取消")));
		ExpectFailure(*this, TEXT("任务被取消"), *Run, EStep::Generate, TEXT("Canceled"));
	}

	// ---- 下载 / 导入：导入队列失败时只留一句话，靠失败前最后看到的阶段判断是哪一步 ----
	auto RunToImport = [](FRunWorld& World) -> TSharedRef<FDreamTextureReflowRun>
	{
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeFillSpec(), World.Make());
		Run->Start();
		FinishJob(World);
		return Run;
	};
	{
		FRunWorld World;
		const TSharedRef<FDreamTextureReflowRun> Run = RunToImport(World);
		World.PushImport(MakeImportStatus(MaterialIdOf(World), EDreamTextureImportStage::FetchingManifest));
		World.PushImport(MakeImportStatus(MaterialIdOf(World), EDreamTextureImportStage::Failed, TEXT("拿不到材质信息（后端返回 HTTP 404）")));
		ExpectFailure(*this, TEXT("拿 manifest 失败"), *Run, EStep::Download, TEXT("拿不到材质信息"));
	}
	{
		FRunWorld World;
		const TSharedRef<FDreamTextureReflowRun> Run = RunToImport(World);
		World.PushImport(MakeImportStatus(MaterialIdOf(World), EDreamTextureImportStage::Downloading, TEXT("正在下载贴图 1/3"), 1, 3));
		World.PushImport(MakeImportStatus(MaterialIdOf(World), EDreamTextureImportStage::Failed, TEXT("下载没有完成：normal.png 校验失败")));
		ExpectFailure(*this, TEXT("下载失败"), *Run, EStep::Download, TEXT("校验失败"));
	}
	{
		FRunWorld World;
		const TSharedRef<FDreamTextureReflowRun> Run = RunToImport(World);
		World.PushImport(MakeImportStatus(MaterialIdOf(World), EDreamTextureImportStage::Downloading, TEXT("正在下载贴图 3/3"), 3, 3));
		World.PushImport(MakeImportStatus(MaterialIdOf(World), EDreamTextureImportStage::Importing, TEXT("正在导入到工程…")));
		World.PushImport(MakeImportStatus(MaterialIdOf(World), EDreamTextureImportStage::Failed, TEXT("落资产时出错：路径不合法")));
		ExpectFailure(*this, TEXT("导入失败"), *Run, EStep::Import, TEXT("路径不合法"));
	}
	{
		// 贴图落进来了，材质实例没建出来：用户要的是一个能用的材质，算「导入」这一步失败。
		FRunWorld World;
		const TSharedRef<FDreamTextureReflowRun> Run = RunToImport(World);
		FDreamTextureImportStatus Done = MakeImportStatus(MaterialIdOf(World), EDreamTextureImportStage::Succeeded);
		Done.Result = MakeImportedResult();
		Done.Result.MaterialInstance.Action = EDreamTextureImportAction::Failed;
		Done.Result.MaterialInstance.Note = TEXT("母材质不可用：被别的资产占用");
		World.PushImport(Done);
		ExpectFailure(*this, TEXT("材质实例没建出来"), *Run, EStep::Import, TEXT("母材质不可用"));
	}
	{
		FRunWorld World;
		const TSharedRef<FDreamTextureReflowRun> Run = RunToImport(World);
		FDreamTextureImportStatus Done = MakeImportStatus(MaterialIdOf(World), EDreamTextureImportStage::Succeeded);
		Done.Result = MakeImportedResult();
		Done.Result.MaterialInstance.Action = EDreamTextureImportAction::Skipped;
		Done.Result.MaterialInstance.Note = TEXT("设置里关闭了「自动创建材质实例」，只导入了贴图。");
		World.PushImport(Done);
		ExpectFailure(*this, TEXT("设置里关了建实例"), *Run, EStep::Import, TEXT("自动创建材质实例"));
	}
	{
		FRunWorld World;
		World.bEnqueueResult = false;
		const TSharedRef<FDreamTextureReflowRun> Run = RunToImport(World);
		ExpectFailure(*this, TEXT("排不进导入队列"), *Run, EStep::Download, TEXT("import queue"));
	}

	// ---- 失败后订阅放干净 ----
	{
		FRunWorld World;
		const TSharedRef<FDreamTextureReflowRun> Run = RunToImport(World);
		World.PushImport(MakeImportStatus(MaterialIdOf(World), EDreamTextureImportStage::Failed, TEXT("拿不到材质信息")));
		TestEqual(TEXT("失败后导入队列的订阅放掉了"), World.ImportWatchers.Num(), 0);
		World.PushImport(MakeImportStatus(MaterialIdOf(World), EDreamTextureImportStage::Succeeded));
		TestTrue(TEXT("失败后迟到的导入成功不翻案"), Run->GetState() == EState::Failed);
	}
	return true;
}

// ---------------------------------------------------------------------------
// 原地覆盖
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowFlowInPlaceTest, "DreamTexture.Editor.Reflow.Flow.InPlace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowFlowInPlaceTest::RunTest(const FString& Parameters)
{
	auto MakeInPlaceSpec = [](UMaterialInstanceConstant* Target)
	{
		FDreamTextureReflowRunSpec Spec = MakeFillSpec();
		Spec.bInPlace = true;
		Spec.InPlaceTarget = Target;
		Spec.ImportOptions.ReuseTextures.Empty();
		Spec.ImportOptions.Channels = { TEXT("normal") };
		Spec.ImportOptions.bCreateMaterialInstance = false;
		FDreamTextureReflowOverwrite Item;
		Item.ImportChannel = TEXT("normal");
		Item.Covers = { TEXT("normal") };
		Item.Parameters = { TEXT("Normal") };
		Spec.Overwrites = { Item };
		Spec.PlanNotes = { TEXT("没改：高度：材质里没有高度贴图。") };
		return Spec;
	};
	auto MakeNoInstanceResult = []()
	{
		FDreamTextureImportResult Result = MakeImportedResult();
		Result.MaterialInstance = FDreamTextureImportedTexture();
		Result.MaterialInstance.Action = EDreamTextureImportAction::Skipped;
		Result.MaterialInstance.Note = TEXT("这一次不新建材质实例。");
		return Result;
	};

	UMaterialInstanceConstant* Target = NewObject<UMaterialInstanceConstant>(GetTransientPackage(), TEXT("MI_InPlaceTarget"), RF_Transient);

	// ---- 成功 ----
	{
		FRunWorld World;
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeInPlaceSpec(Target), World.Make());
		FStepLog Log;
		Log.Bind(*Run);
		Run->Start();
		FinishJob(World);
		if (TestEqual(TEXT("排导入时不新建材质实例"), World.EnqueuedOptions.Num(), 1))
		{
			TestFalse(TEXT("bCreateMaterialInstance 为假"), World.EnqueuedOptions[0].bCreateMaterialInstance);
			TestEqual(TEXT("只导入要换的通道"), FString::Join(World.EnqueuedOptions[0].Channels, TEXT(",")), FString(TEXT("normal")));
			TestEqual(TEXT("不沿用底色（原地覆盖不新建实例，没有地方沿用）"), World.EnqueuedOptions[0].ReuseTextures.Num(), 0);
		}
		FDreamTextureImportStatus Done = MakeImportStatus(MaterialIdOf(World), EDreamTextureImportStage::Succeeded);
		Done.Result = MakeNoInstanceResult();
		World.PushImport(Done);

		TestEqual(TEXT("调了一次原地覆盖"), World.ApplyCalls, 1);
		if (TestEqual(TEXT("带着要改的一处"), World.AppliedItems.Num(), 1))
		{
			TestEqual(TEXT("按选中材质自己的参数名"), World.AppliedItems[0].Parameters[0].ToString(), FString(TEXT("Normal")));
		}
		TestEqual(TEXT("带着导入的结果（新贴图从这里找）"), World.AppliedImport.Textures.Num(), 3);
		TestTrue(TEXT("成功"), Run->GetState() == EState::Succeeded);
		TestEqual(TEXT("结果指向被改的那个实例"), Run->GetResultInstancePath(), Target->GetPathName());
		const FString Message = Run->GetMessage().ToString();
		TestTrue(TEXT("说明点名被改的实例与参数"), Message.Contains(TEXT("MI_InPlaceTarget")) && Message.Contains(TEXT("Normal")));
		TestTrue(TEXT("说明可以撤销"), Message.Contains(TEXT("Ctrl+Z")));
		TestTrue(TEXT("计划里没改的说明带上了"), Message.Contains(TEXT("没改：高度")));
		TestFalse(TEXT("不说「新建了材质实例」"), Message.Contains(TEXT("已新建")));
		TestTrue(TEXT("步骤里有改写实例这一步"), Log.Contains(EStep::Apply));
	}

	// ---- 参数改了一部分：被跳过的也说 ----
	{
		FRunWorld World;
		World.ApplyResult.Skipped = { TEXT("参数 ORM 没改：新贴图的压缩设置与它的采样器类型不匹配。") };
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeInPlaceSpec(Target), World.Make());
		Run->Start();
		FinishJob(World);
		FDreamTextureImportStatus Done = MakeImportStatus(MaterialIdOf(World), EDreamTextureImportStage::Succeeded);
		Done.Result = MakeNoInstanceResult();
		World.PushImport(Done);
		TestTrue(TEXT("成功"), Run->GetState() == EState::Succeeded);
		TestTrue(TEXT("跳过的原因出现在说明里"), Run->GetMessage().ToString().Contains(TEXT("采样器")));
	}

	// ---- 一个参数都没改：失败在「改写实例」这一步 ----
	{
		FRunWorld World;
		World.ApplyResult.Changed.Empty();
		World.ApplyResult.Skipped = { TEXT("参数 Normal 没改：采样器不匹配。") };
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeInPlaceSpec(Target), World.Make());
		Run->Start();
		FinishJob(World);
		FDreamTextureImportStatus Done = MakeImportStatus(MaterialIdOf(World), EDreamTextureImportStage::Succeeded);
		Done.Result = MakeNoInstanceResult();
		World.PushImport(Done);
		ExpectFailure(*this, TEXT("一个参数都没改"), *Run, EStep::Apply, TEXT("采样器不匹配"));
	}

	// ---- 覆盖本身出错 ----
	{
		FRunWorld World;
		World.ApplyResult.bOK = false;
		World.ApplyResult.Error = TEXT("要改的材质实例已经不在了");
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeInPlaceSpec(Target), World.Make());
		Run->Start();
		FinishJob(World);
		FDreamTextureImportStatus Done = MakeImportStatus(MaterialIdOf(World), EDreamTextureImportStage::Succeeded);
		Done.Result = MakeNoInstanceResult();
		World.PushImport(Done);
		ExpectFailure(*this, TEXT("覆盖出错"), *Run, EStep::Apply, TEXT("已经不在了"));
	}

	// ---- 要改的实例在等待期间没了（用户关了工程里的资产、删了它） ----
	{
		UMaterialInstanceConstant* Doomed = NewObject<UMaterialInstanceConstant>(GetTransientPackage(), TEXT("MI_Doomed"), RF_Transient);
		FRunWorld World;
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeInPlaceSpec(Doomed), World.Make());
		Run->Start();
		Doomed->MarkAsGarbage();
		FinishJob(World);
		FDreamTextureImportStatus Done = MakeImportStatus(MaterialIdOf(World), EDreamTextureImportStage::Succeeded);
		Done.Result = MakeNoInstanceResult();
		World.PushImport(Done);
		ExpectFailure(*this, TEXT("实例没了"), *Run, EStep::Apply, TEXT("no longer exists"));
		TestTrue(TEXT("说明告诉用户新贴图已经导进工程"), Run->GetMessage().ToString().Contains(TEXT("new textures were imported into the project")));
		TestEqual(TEXT("没有去调覆盖"), World.ApplyCalls, 0);
	}
	return true;
}

// ---------------------------------------------------------------------------
// 取消
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowFlowCancelTest, "DreamTexture.Editor.Reflow.Flow.Cancel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowFlowCancelTest::RunTest(const FString& Parameters)
{
	// ---- 没开始 / 已结束：取消什么都不做 ----
	{
		FRunWorld World;
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeFillSpec(), World.Make());
		Run->Cancel();
		TestTrue(TEXT("没开始时取消：仍是 Idle"), Run->GetState() == EState::Idle);
	}

	// ---- 上传途中取消：那一步结束时发现，停在上传，不再提交 ----
	{
		FRunWorld World;
		World.bHoldUpload = true;
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeFillSpec(), World.Make());
		Run->Start();
		TestTrue(TEXT("停在上传"), Run->GetStep() == EStep::Upload && Run->IsRunning());
		Run->Cancel();
		TestTrue(TEXT("取消中仍在运行（上传的请求收不回）"), Run->IsRunning());
		TestTrue(TEXT("说明是取消中"), Run->GetMessage().ToString().Contains(TEXT("Canceling")));
		FString Stored = World.StoredName;
		World.HeldUploads[0](true, MoveTemp(Stored), FString());
		ExpectFailure(*this, TEXT("上传途中取消"), *Run, EStep::Upload, TEXT("Canceled"));
		TestEqual(TEXT("取消后不提交"), World.SubmitRequests.Num(), 0);
	}

	// ---- 确认后端途中取消 ----
	{
		FRunWorld World;
		World.bHoldBackend = true;
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeFillSpec(), World.Make());
		Run->Start();
		Run->Cancel();
		World.HeldBackend[0](true, FString());
		ExpectFailure(*this, TEXT("起后端途中取消"), *Run, EStep::Backend, TEXT("Canceled"));
		TestEqual(TEXT("取消后不导出"), World.ExportCalls, 0);
	}

	// ---- 生成中取消：打断后端的任务，等 job.failed(canceled) 回来收尾 ----
	{
		FRunWorld World;
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeFillSpec(), World.Make());
		Run->Start();
		World.PushJob(MakeJob(World.JobId, EDreamTextureJobStatus::Running, 0.2f, TEXT("生成中")));
		Run->Cancel();
		if (TestEqual(TEXT("向后端请求取消这个任务"), World.CancelIds.Num(), 1))
		{
			TestEqual(TEXT("取消的是这个任务"), World.CancelIds[0], World.JobId);
		}
		TestTrue(TEXT("取消中，还没结束"), Run->IsRunning());
		TestTrue(TEXT("说明是取消中"), Run->GetMessage().ToString().Contains(TEXT("Canceling")));
		TestFalse(TEXT("取消中没有进度"), Run->GetProgress().IsSet());

		// 取消请求成功，但任务要等 ComfyUI 被打断才结束：期间任务的进度事件不把说明改回「生成中」。
		World.CancelDones[0](true, FString());
		World.PushJob(MakeJob(World.JobId, EDreamTextureJobStatus::Running, 0.25f, TEXT("生成中")));
		TestTrue(TEXT("取消中，进度事件不把说明改回去"), Run->GetMessage().ToString().Contains(TEXT("Canceling")));

		World.PushJob(MakeJob(World.JobId, EDreamTextureJobStatus::Canceled, 0.0f, TEXT(""), TEXT("已取消")));
		ExpectFailure(*this, TEXT("生成中取消"), *Run, EStep::Generate, TEXT("Canceled"));
		TestEqual(TEXT("取消了不导入"), World.EnqueuedIds.Num(), 0);
	}

	// ---- 取消请求没成（任务刚好结束了）：说一声，任务照常完成就照常导入 ----
	{
		FRunWorld World;
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeFillSpec(), World.Make());
		Run->Start();
		Run->Cancel();
		World.CancelDones[0](false, FString(TEXT("取消失败：后端返回 HTTP 400：任务已结束")));
		TestTrue(TEXT("取消失败：仍在运行"), Run->IsRunning());
		TestTrue(TEXT("说明点出取消没成功"), Run->GetMessage().ToString().Contains(TEXT("The cancel request did not succeed")));
		FinishJob(World);
		TestTrue(TEXT("任务完成了就继续导入"), Run->GetStep() == EStep::Download);
		TestEqual(TEXT("排了导入"), World.EnqueuedIds.Num(), 1);
	}

	// ---- 提交途中取消：应答回来、有任务 id 了，补一次真正的取消 ----
	{
		FRunWorld World;
		World.bHoldSubmit = true;
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MakeFillSpec(), World.Make());
		Run->Start();
		TestTrue(TEXT("停在提交"), Run->GetStep() == EStep::Submit);
		Run->Cancel();
		TestEqual(TEXT("还没有任务可取消"), World.CancelIds.Num(), 0);
		World.HeldSubmits[0](true, { MakeJob(World.JobId, EDreamTextureJobStatus::Queued) }, FString());
		TestEqual(TEXT("应答回来后补发了取消"), World.CancelIds.Num(), 1);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
