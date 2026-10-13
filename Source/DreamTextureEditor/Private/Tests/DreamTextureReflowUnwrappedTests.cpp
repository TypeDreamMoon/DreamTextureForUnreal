// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureReflowTestUtils.h"
#include "SDreamTextureReflowPanel.h"

using namespace DreamTextureReflowTest;

/*
 * 回流的 UV 展开路径（M9）：角色、道具、trim sheet 这类一张图对应一块网格展开的贴图。
 *
 * 和平铺的区别，每一条都在这里有断言：
 *   - 平铺 / UV 展开的选择按材质记住（Saved/DreamTexture 下，不写进用户的包）；
 *   - UV 展开时请求带 tileable=false；
 *   - 分解模型默认选传统派生，不是 CHORD；CHORD 仍可手动选，旁边标「分布外，质量不保证」；
 *   - 改写外观的重绘幅度有 0.35 的硬上限，超了拦住并用人话说明；
 *   - 结果预览不出 3×3 平铺图，改成「原图 | 结果」并排。
 *
 * 外部能力全是假的，见 DreamTextureReflowTestUtils.h。
 */

namespace
{
	using EState = FDreamTextureReflowRun::EState;

	FText WhyNotRun(const FDreamTextureReflowActions& Actions)
	{
		FText Why;
		Actions.CanRun(&Why);
		return Why;
	}

	/** 把一次运行推到成功：任务完成、导入成功。 */
	void CompleteRun(FPageFixture& Fixture, const FString& JobId)
	{
		Fixture.World.PushJob(MakeJob(JobId, EDreamTextureJobStatus::Succeeded, 1.0f));
		FDreamTextureImportStatus Done = MakeImportStatus(TEXT("mat_") + JobId, EDreamTextureImportStage::Succeeded);
		Done.Result = MakeImportedResult();
		Fixture.World.PushImport(Done);
	}
}

// ---------------------------------------------------------------------------
// 按材质记住
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowUnwrappedRememberTest, "DreamTexture.Editor.Reflow.Unwrapped.Remember", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowUnwrappedRememberTest::RunTest(const FString& Parameters)
{
	FPageFixture Fixture;
	Fixture.SelectTypical(*this);
	FDreamTextureReflowActions& Actions = *Fixture.Actions;

	TArray<FString> Notices;
	Actions.OnNotice.AddLambda([&Notices](const FText& Message, bool) { Notices.Add(Message.ToString()); });

	// ---- 默认是平铺，记进库 ----
	TestTrue(TEXT("默认平铺"), Actions.GetUVKind() == EDreamTextureUVKind::Tiled);
	Actions.SetUVKind(EDreamTextureUVKind::Unwrapped);
	TestTrue(TEXT("选了 UV 展开"), Actions.GetUVKind() == EDreamTextureUVKind::Unwrapped);
	EDreamTextureUVKind Stored = EDreamTextureUVKind::Tiled;
	TestTrue(TEXT("记在库里（按材质资产路径）"), Fixture.Store.Store.FindUVKind(TEXT("/Game/Test/MI_Rock.MI_Rock"), Stored) && Stored == EDreamTextureUVKind::Unwrapped);
	TestEqual(TEXT("没有多余的通知"), Notices.Num(), 0);

	// ---- 换到另一个材质：读的是它自己的记录，没记过就是平铺 ----
	UMaterialInstanceConstant* Other = NewObject<UMaterialInstanceConstant>(GetTransientPackage(), NAME_None, RF_Transient);
	Fixture.Select(Other, TEXT("/Game/Test/MI_Crate.MI_Crate"), TypicalRows());
	TestTrue(TEXT("另一个材质没记过：平铺（上一个材质的选择不串过来）"), Fixture.Actions->GetUVKind() == EDreamTextureUVKind::Tiled);
	Fixture.Actions->SetUVKind(EDreamTextureUVKind::Unwrapped);

	Fixture.Select(Fixture.Instance, TEXT("/Game/Test/MI_Rock.MI_Rock"), TypicalRows());
	TestTrue(TEXT("换回第一个材质：记着的 UV 展开恢复"), Fixture.Actions->GetUVKind() == EDreamTextureUVKind::Unwrapped);

	// ---- 重开（新的页面模型、新的库实例读同一个文件）----
	{
		FDreamTextureOverrideStore Reopened(Fixture.Store.Path);
		TestTrue(TEXT("重开编辑器后还在：Rock"), Reopened.FindUVKind(TEXT("/Game/Test/MI_Rock.MI_Rock"), Stored) && Stored == EDreamTextureUVKind::Unwrapped);
		TestTrue(TEXT("重开编辑器后还在：Crate"), Reopened.FindUVKind(TEXT("/Game/Test/MI_Crate.MI_Crate"), Stored) && Stored == EDreamTextureUVKind::Unwrapped);
	}

	// ---- 同一个文件里还有用户指认：互不冲掉 ----
	Fixture.Model->SetRole(TEXT("/Game/Test/T_N.T_N"), EDreamTextureRole::Roughness);
	TestTrue(TEXT("写了一条指认之后 UV 展开的记忆还在"), Fixture.Store.Store.FindUVKind(TEXT("/Game/Test/MI_Rock.MI_Rock"), Stored) && Stored == EDreamTextureUVKind::Unwrapped);
	TestTrue(TEXT("指认之后重新解析，页面上仍是 UV 展开"), Fixture.Actions->GetUVKind() == EDreamTextureUVKind::Unwrapped);

	// ---- 选回平铺：不留条目 ----
	Fixture.Actions->SetUVKind(EDreamTextureUVKind::Tiled);
	TestFalse(TEXT("选回平铺：库里不留条目（没有记录就是平铺）"), Fixture.Store.Store.FindUVKind(TEXT("/Game/Test/MI_Rock.MI_Rock"), Stored));

	// ---- 没选东西时开关不起作用 ----
	{
		FPageFixture None;
		None.Build(*this);
		None.Actions->SetUVKind(EDreamTextureUVKind::Unwrapped);
		TestTrue(TEXT("没选材质：设不上，也没东西可记"), None.Actions->GetUVKind() == EDreamTextureUVKind::Tiled);
	}

	// ---- 记不下来：说一声，这次仍然有效 ----
	{
		// 库文件的「父目录」其实是个文件：MakeDirectory 建不出来，写盘必然失败。
		const FString Blocker = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamTextureTest") / TEXT("ReflowActions") / (FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".blocker")));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Blocker), true);
		FFileHelper::SaveStringToFile(TEXT("x"), *Blocker);
		FDreamTextureOverrideStore Broken(Blocker / TEXT("inner.json"));

		FPageFixture Fix2;
		Fix2.Selection.Store = &Broken;
		Fix2.Model = MakeShared<FDreamTextureReflowModel>(Fix2.Selection.Make());
		Fix2.Select(Fix2.Instance, TEXT("/Game/Test/MI_Rock.MI_Rock"), TypicalRows());
		Fix2.Build(*this);
		TArray<FString> FailNotices;
		TArray<bool> FailErrors;
		Fix2.Actions->OnNotice.AddLambda([&FailNotices, &FailErrors](const FText& Message, bool bIsError)
		{
			FailNotices.Add(Message.ToString());
			FailErrors.Add(bIsError);
		});
		Fix2.Actions->SetUVKind(EDreamTextureUVKind::Unwrapped);
		TestTrue(TEXT("写盘失败：这次编辑器会话里仍然有效"), Fix2.Actions->GetUVKind() == EDreamTextureUVKind::Unwrapped);
		if (TestEqual(TEXT("写盘失败：通知一次"), FailNotices.Num(), 1))
		{
			TestTrue(TEXT("是错误通知"), FailErrors[0]);
			TestTrue(TEXT("说明：没能记住、重开会丢"), FailNotices[0].Contains(TEXT("Could not remember")) && FailNotices[0].Contains(TEXT("restart")));
		}
		IFileManager::Get().Delete(*Blocker, false, true, true);
	}

	// ---- 材质没有资产路径（运行时临时生成的）：这次有效，说一声记不住 ----
	{
		FPageFixture Fix3;
		Fix3.Select(Fix3.Instance, TEXT("/Game/Test/MI_Dyn.MI_Dyn"), TypicalRows());
		Fix3.Selection.Results[Fix3.Instance->GetPathName()].AssetPath.Reset();
		Fix3.Model->Reanalyze();
		Fix3.Build(*this);
		TArray<FString> DynNotices;
		Fix3.Actions->OnNotice.AddLambda([&DynNotices](const FText& Message, bool) { DynNotices.Add(Message.ToString()); });
		Fix3.Actions->SetUVKind(EDreamTextureUVKind::Unwrapped);
		TestTrue(TEXT("这次有效"), Fix3.Actions->GetUVKind() == EDreamTextureUVKind::Unwrapped);
		TestTrue(TEXT("说明点出不是工程里的资产、记不住"), DynNotices.Num() == 1 && DynNotices[0].Contains(TEXT("not an asset in the project")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// 分解模型：UV 展开默认传统派生，CHORD 标分布外
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowUnwrappedDecomposeTest, "DreamTexture.Editor.Reflow.Unwrapped.Decompose", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowUnwrappedDecomposeTest::RunTest(const FString& Parameters)
{
	FPageFixture Fixture;
	Fixture.SelectTypical(*this);
	FDreamTextureReflowActions& Actions = *Fixture.Actions;

	const FDreamTextureWorkflowMeta* Chord = Actions.GetCatalog().FindSegment(TEXT("dec-chord-v1"));
	const FDreamTextureWorkflowMeta* Derive = Actions.GetCatalog().FindSegment(TEXT("dec-derive-v1"));
	if (!TestTrue(TEXT("目录里有两个分解段"), Chord != nullptr && Derive != nullptr))
	{
		return false;
	}

	// ---- 依赖平铺假设的判据来自声明，不认名字 ----
	TestTrue(TEXT("CHORD 声明了 tileable：依赖平铺假设"), FDreamTextureReflowActions::IsTileDependent(*Chord));
	TestFalse(TEXT("传统派生没有任何依赖无缝的步骤，不声明 tileable"), FDreamTextureReflowActions::IsTileDependent(*Derive));
	{
		FDreamTextureWorkflowMeta Hypothetical = *Derive;
		FDreamTextureParamSpec Param;
		Param.Key = TEXT("tileable");
		Hypothetical.Advanced.Add(Param);
		TestTrue(TEXT("以后加的分解模型声明了 tileable 就同样适用（不看名字）"), FDreamTextureReflowActions::IsTileDependent(Hypothetical));
	}

	// ---- 平铺：默认 CHORD（与生成页一致），不标分布外 ----
	TestEqual(TEXT("平铺默认：按 id 排序的第一个（CHORD）"), Actions.GetDecomposeId(), FString(TEXT("dec-chord-v1")));
	TestFalse(TEXT("平铺：CHORD 不标分布外"), FDreamTextureReflowActions::DescribeSegment(*Chord, EDreamTextureUVKind::Tiled).ToString().Contains(TEXT("分布外")));
	TestFalse(TEXT("平铺：没有分布外提醒"), Actions.GetDecomposeHint().bOutOfDistribution);

	// ---- UV 展开：默认传统派生 ----
	Actions.SetUVKind(EDreamTextureUVKind::Unwrapped);
	TestEqual(TEXT("UV 展开默认传统派生，不是 CHORD"), Actions.GetDecomposeId(), FString(TEXT("dec-derive-v1")));
	TestEqual(TEXT("补通道拼出的组合"), Actions.GetCurrentWorkflow()->Id, FString(TEXT("src-upload-v1.dec-derive-v1")));
	TestFalse(TEXT("默认选的传统派生没有分布外提醒"), Actions.GetDecomposeHint().bOutOfDistribution);
	TestFalse(TEXT("也没有许可提醒（可商用）"), Actions.GetDecomposeHint().bNonCommercial);

	Actions.SetMode(EDreamTextureReflowMode::Restyle);
	TestEqual(TEXT("改写外观的组合：本地写实 × 传统派生"), Actions.GetCurrentWorkflow()->Id, FString(TEXT("src-local-realistic-v1.dec-derive-v1")));
	Actions.SetMode(EDreamTextureReflowMode::FillChannels);

	// ---- CHORD 仍可手动选，旁边标分布外 ----
	TestTrue(TEXT("下拉里 CHORD 一项标「分布外，质量不保证」"), FDreamTextureReflowActions::DescribeSegment(*Chord, EDreamTextureUVKind::Unwrapped).ToString().Contains(TEXT("Out of distribution, quality not guaranteed")));
	TestTrue(TEXT("同时仍标「仅限研究用途」"), FDreamTextureReflowActions::DescribeSegment(*Chord, EDreamTextureUVKind::Unwrapped).ToString().Contains(TEXT("Research use only")));
	TestFalse(TEXT("传统派生一项不标"), FDreamTextureReflowActions::DescribeSegment(*Derive, EDreamTextureUVKind::Unwrapped).ToString().Contains(TEXT("分布外")));

	Actions.SetDecomposeId(TEXT("dec-chord-v1"));
	TestEqual(TEXT("手动选 CHORD：照选"), Actions.GetDecomposeId(), FString(TEXT("dec-chord-v1")));
	{
		const FDreamTextureReflowDecomposeHint Hint = Actions.GetDecomposeHint();
		TestTrue(TEXT("UV + CHORD：分布外提醒"), Hint.bOutOfDistribution);
		TestTrue(TEXT("提醒说明质量不保证、原因是 UV 展开图对它是分布外"), Hint.OutOfDistributionText.Contains(TEXT("quality not guaranteed")) && Hint.OutOfDistributionText.Contains(TEXT("out-of-distribution")) && Hint.OutOfDistributionText.Contains(TEXT("tileable material")));
		TestTrue(TEXT("许可提醒仍在"), Hint.bNonCommercial);
	}
	TestTrue(TEXT("只提醒不拦：照样能开始"), Actions.CanRun());

	// ---- 切回平铺：用户明确选的不变（选过的就是选过的）----
	Actions.SetUVKind(EDreamTextureUVKind::Tiled);
	TestEqual(TEXT("明确选过 CHORD：切回平铺仍是它"), Actions.GetDecomposeId(), FString(TEXT("dec-chord-v1")));
	TestFalse(TEXT("平铺下 CHORD 不再标分布外"), Actions.GetDecomposeHint().bOutOfDistribution);

	// ---- 明确选了传统派生：切到 UV 展开也不变；没选过的才跟着默认走 ----
	FPageFixture Fix2;
	Fix2.SelectTypical(*this);
	Fix2.Actions->SetUVKind(EDreamTextureUVKind::Unwrapped);
	TestEqual(TEXT("没选过：UV 展开默认传统派生"), Fix2.Actions->GetDecomposeId(), FString(TEXT("dec-derive-v1")));
	Fix2.Actions->SetUVKind(EDreamTextureUVKind::Tiled);
	TestEqual(TEXT("没选过：切回平铺默认回到 CHORD（默认跟着平铺方式走）"), Fix2.Actions->GetDecomposeId(), FString(TEXT("dec-chord-v1")));

	// ---- 重开面板：记着 UV 展开的材质一取选择就是传统派生 ----
	FPageFixture Fix3;
	Fix3.Store.Store.SetUVKind(TEXT("/Game/Test/MI_Rock.MI_Rock"), EDreamTextureUVKind::Unwrapped);
	Fix3.SelectTypical(*this);
	TestTrue(TEXT("记着 UV 展开"), Fix3.Actions->GetUVKind() == EDreamTextureUVKind::Unwrapped);
	TestEqual(TEXT("所以默认就是传统派生"), Fix3.Actions->GetCurrentWorkflow()->Id, FString(TEXT("src-upload-v1.dec-derive-v1")));
	return true;
}

// ---------------------------------------------------------------------------
// 请求：tileable
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowUnwrappedRequestTest, "DreamTexture.Editor.Reflow.Unwrapped.Request", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowUnwrappedRequestTest::RunTest(const FString& Parameters)
{
	FPageFixture Fixture;
	Fixture.SelectTypical(*this);
	FDreamTextureReflowActions& Actions = *Fixture.Actions;
	FDreamTextureReflowRunSpec Spec;

	// ---- 平铺：tileable=true ----
	Actions.BuildSpec(Spec);
	TestTrue(TEXT("平铺：请求里带 tileable"), Spec.Request.BoolParams.Contains(TEXT("tileable")));
	TestTrue(TEXT("平铺：tileable=true"), Spec.Request.BoolParams.FindRef(TEXT("tileable")));
	TestTrue(TEXT("平铺：运行输入记着平铺"), Spec.UVKind == EDreamTextureUVKind::Tiled);
	TestFalse(TEXT("平铺：标题不带 UV 展开"), Spec.Title.Contains(TEXT("UV")));

	// ---- UV 展开：tileable=false，补通道 ----
	Actions.SetUVKind(EDreamTextureUVKind::Unwrapped);
	if (TestTrue(TEXT("UV 展开：补通道拼得出"), Actions.BuildSpec(Spec)))
	{
		TestTrue(TEXT("UV 展开：请求里带 tileable"), Spec.Request.BoolParams.Contains(TEXT("tileable")));
		TestFalse(TEXT("UV 展开：tileable=false"), Spec.Request.BoolParams.FindRef(TEXT("tileable")));
		TestEqual(TEXT("UV 展开：分解模型默认传统派生"), Spec.Request.WorkflowId, FString(TEXT("src-upload-v1.dec-derive-v1")));
		TestTrue(TEXT("UV 展开：运行输入记着 UV 展开"), Spec.UVKind == EDreamTextureUVKind::Unwrapped);
		TestTrue(TEXT("标题标明 UV 展开"), Spec.Title.Contains(TEXT("UV-unwrapped")));
		FString Error;
		TestTrue(FString::Printf(TEXT("请求通过 Client 本地校验（%s）"), *Error), Spec.Request.Validate(Error));
	}

	// ---- UV 展开：改写外观，tileable=false，重绘幅度默认 0.30 ----
	Actions.SetMode(EDreamTextureReflowMode::Restyle);
	Actions.SetPrompt(TEXT("worn leather"));
	if (TestTrue(TEXT("UV 展开：改写外观拼得出"), Actions.BuildSpec(Spec)))
	{
		TestFalse(TEXT("UV 展开：tileable=false"), Spec.Request.BoolParams.FindRef(TEXT("tileable")));
		TestEqual(TEXT("UV 展开：重绘幅度默认 0.30（在上限以内）"), Spec.Request.FloatParams.FindRef(TEXT("denoise")), DreamTextureReflow::DefaultUnwrappedDenoise);
		TestEqual(TEXT("工作流：本地写实 × 传统派生"), Spec.Request.WorkflowId, FString(TEXT("src-local-realistic-v1.dec-derive-v1")));
	}
	Actions.SetUVKind(EDreamTextureUVKind::Tiled);
	Actions.BuildSpec(Spec);
	TestTrue(TEXT("切回平铺：tileable=true"), Spec.Request.BoolParams.FindRef(TEXT("tileable")));
	TestEqual(TEXT("切回平铺：重绘幅度默认回到 0.55（没动过的默认跟着平铺方式走）"), Spec.Request.FloatParams.FindRef(TEXT("denoise")), DreamTextureReflow::DefaultTiledDenoise);

	// ---- 用户动过的重绘幅度不被默认冲掉 ----
	Actions.SetDenoise(0.4);
	Actions.SetUVKind(EDreamTextureUVKind::Unwrapped);
	TestTrue(TEXT("用户给的 0.4 不因为切换平铺方式被改写"), FMath::IsNearlyEqual(Actions.GetDenoise(), 0.4));

	// ---- 旧后端：出图段 / 分解段没声明 tileable，就不发（后端只认声明过的键）----
	{
		FPageFixture Old;
		Old.Select(Old.Instance, TEXT("/Game/Test/MI_Rock.MI_Rock"), TypicalRows());
		Old.Build(*this, false);
		FDreamTextureWorkflowCatalog Stripped = Old.Catalog;
		auto StripTileable = [](FDreamTextureWorkflowMeta& Meta)
		{
			Meta.Params.RemoveAll([](const FDreamTextureParamSpec& P) { return P.Key == TEXT("tileable"); });
			Meta.Advanced.RemoveAll([](const FDreamTextureParamSpec& P) { return P.Key == TEXT("tileable"); });
		};
		for (FDreamTextureWorkflowMeta& Meta : Stripped.Workflows) { StripTileable(Meta); }
		for (FDreamTextureWorkflowMeta& Meta : Stripped.Segments) { StripTileable(Meta); }
		Old.CompleteCatalog(Stripped);
		Old.Actions->SetUVKind(EDreamTextureUVKind::Unwrapped);
		FDreamTextureReflowRunSpec OldSpec;
		if (TestTrue(TEXT("旧后端：UV 展开也拼得出"), Old.Actions->BuildSpec(OldSpec)))
		{
			TestFalse(TEXT("旧后端没有 tileable 参数：不发（发了也被忽略，白发）"), OldSpec.Request.BoolParams.Contains(TEXT("tileable")));
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// 重绘幅度的硬上限
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowUnwrappedDenoiseTest, "DreamTexture.Editor.Reflow.Unwrapped.Denoise", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowUnwrappedDenoiseTest::RunTest(const FString& Parameters)
{
	FPageFixture Fixture;
	Fixture.SelectTypical(*this);
	FDreamTextureReflowActions& Actions = *Fixture.Actions;
	Actions.SetMode(EDreamTextureReflowMode::Restyle);
	Actions.SetPrompt(TEXT("worn leather"));
	Actions.SetUVKind(EDreamTextureUVKind::Unwrapped);

	// ---- 默认值在上限以内，能开始 ----
	TestFalse(TEXT("默认 0.30：不拦"), Actions.IsDenoiseBlocked());
	TestTrue(TEXT("能开始"), Actions.CanRun());

	// ---- 超过上限：拦住，并用人话说明 ----
	Actions.SetDenoise(0.6);
	FString Why;
	TestTrue(TEXT("0.6：被拦"), Actions.IsDenoiseBlocked(&Why));
	TestTrue(TEXT("说明点出上限 0.35 和现在的 0.60"), Why.Contains(TEXT("0.35")) && Why.Contains(TEXT("0.60")));
	TestTrue(TEXT("说明讲后果：重绘会破坏 UV 对应关系，贴回模型就全乱了"), Why.Contains(TEXT("Restyling")) && Why.Contains(TEXT("UV")) && Why.Contains(TEXT("no longer fits the model")));
	TestTrue(TEXT("说明给出出路：调低或切回平铺"), Why.Contains(TEXT("switch back to Tileable")));
	TestTrue(TEXT("没有被悄悄改小：页面上仍是用户给的 0.6"), FMath::IsNearlyEqual(Actions.GetDenoise(), 0.6));

	FText RunWhy;
	TestFalse(TEXT("CanRun 拦住"), Actions.CanRun(&RunWhy));
	TestEqual(TEXT("CanRun 给的原因就是这句人话"), RunWhy.ToString(), Why);
	FDreamTextureReflowRunSpec Spec;
	FText SpecError;
	TestFalse(TEXT("BuildSpec 也拦（请求根本不会拼出来）"), Actions.BuildSpec(Spec, &SpecError));
	TestEqual(TEXT("BuildSpec 的原因同上"), SpecError.ToString(), Why);

	Actions.Run();
	TestEqual(TEXT("点开始：不造运行"), Fixture.Runs.Num(), 0);
	TestEqual(TEXT("也没有向后端提交"), Fixture.World.SubmitRequests.Num(), 0);

	// ---- 边界 ----
	Actions.SetDenoise(0.36);
	TestTrue(TEXT("0.36：被拦"), Actions.IsDenoiseBlocked());
	Actions.SetDenoise(0.35);
	TestFalse(TEXT("0.35：恰好允许"), Actions.IsDenoiseBlocked());
	TestTrue(TEXT("0.35：能开始"), Actions.CanRun());
	Actions.SetDenoise(static_cast<double>(0.35f));
	TestFalse(TEXT("滑块存成 float 的 0.35 也不算超限"), Actions.IsDenoiseBlocked());

	// ---- 平铺不设上限 ----
	Actions.SetDenoise(0.9);
	TestTrue(TEXT("UV 展开：0.9 被拦"), Actions.IsDenoiseBlocked());
	Actions.SetUVKind(EDreamTextureUVKind::Tiled);
	TestFalse(TEXT("平铺：同样的 0.9 不拦"), Actions.IsDenoiseBlocked());
	TestTrue(TEXT("平铺：能开始"), Actions.CanRun());

	// ---- 补通道没有重绘幅度，不受影响 ----
	Actions.SetUVKind(EDreamTextureUVKind::Unwrapped);
	Actions.SetMode(EDreamTextureReflowMode::FillChannels);
	TestFalse(TEXT("补通道：没有重绘幅度可言，不拦"), Actions.IsDenoiseBlocked());
	TestTrue(TEXT("补通道：能开始"), Actions.CanRun());

	// ---- 兜底：运行在最低一层再拦一次（别的入口直接构造运行也绕不过去）----
	{
		FRunWorld World;
		FDreamTextureReflowRunSpec Bad = MakeFillSpec(TEXT("src-local-realistic-v1.dec-derive-v1"));
		Bad.Mode = EDreamTextureReflowMode::Restyle;
		Bad.UVKind = EDreamTextureUVKind::Unwrapped;
		Bad.UploadParamKey = TEXT("reference");
		Bad.Request.FloatParams.Add(TEXT("denoise"), 0.6);
		Bad.Request.DerivedFrom.Mode = FDreamTextureDerivedFrom::ModeRestyle();
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(Bad, World.Make());
		Run->Start();
		TestTrue(TEXT("运行直接失败"), Run->GetState() == EState::Failed);
		TestTrue(TEXT("说的是重绘幅度的上限"), Run->GetMessage().ToString().Contains(TEXT("0.35")) && Run->GetMessage().ToString().Contains(TEXT("no longer fits the model")));
		TestEqual(TEXT("什么都没碰：没确认后端"), World.BackendCalls, 0);
		TestEqual(TEXT("没导出"), World.ExportCalls, 0);
		TestEqual(TEXT("没上传"), World.UploadedFiles.Num(), 0);
		TestEqual(TEXT("没提交"), World.SubmitRequests.Num(), 0);

		// 平铺的同样请求照常开始。
		FRunWorld World2;
		FDreamTextureReflowRunSpec Fine = Bad;
		Fine.UVKind = EDreamTextureUVKind::Tiled;
		const TSharedRef<FDreamTextureReflowRun> Run2 = MakeShared<FDreamTextureReflowRun>(Fine, World2.Make());
		Run2->Start();
		TestTrue(TEXT("平铺 0.6：照常开始"), Run2->IsRunning());
	}
	return true;
}

// ---------------------------------------------------------------------------
// 结果预览：平铺 3×3，UV 展开并排
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowUnwrappedPreviewTest, "DreamTexture.Editor.Reflow.Unwrapped.Preview", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowUnwrappedPreviewTest::RunTest(const FString& Parameters)
{
	using EKind = FDreamTextureReflowPreviewPlan::EKind;

	// ---- 还没有运行 / 没成功：没有预览 ----
	{
		FPageFixture Fixture;
		Fixture.SelectTypical(*this);
		TestTrue(TEXT("没运行过：没有预览"), Fixture.Actions->GetPreviewPlan().Kind == EKind::None);
		Fixture.Actions->Run();
		TestTrue(TEXT("运行中：没有预览"), Fixture.Actions->GetPreviewPlan().Kind == EKind::None);
		Fixture.World.PushJob(MakeJob(Fixture.World.JobId, EDreamTextureJobStatus::Failed, 0.0f, TEXT(""), TEXT("ComfyUI 报错")));
		TestTrue(TEXT("失败了：没有预览"), Fixture.Actions->GetPreviewPlan().Kind == EKind::None);
	}

	// ---- 平铺：后端的 3×3 平铺预览 ----
	{
		FPageFixture Fixture;
		Fixture.SelectTypical(*this);
		Fixture.Actions->Run();
		CompleteRun(Fixture, Fixture.World.JobId);
		const FDreamTextureReflowPreviewPlan Plan = Fixture.Actions->GetPreviewPlan();
		TestTrue(TEXT("平铺：一张"), Plan.Kind == EKind::Tiled);
		TestEqual(TEXT("取的是后端材质里的 preview.png"), Plan.LeftKey, FDreamTextureReflowActions::MakeMaterialFileKey(TEXT("mat_job_1"), TEXT("preview.png")));
		TestTrue(TEXT("没有右边那张"), Plan.RightKey.IsEmpty());
		TestTrue(TEXT("标签说是 3×3 平铺预览"), Plan.LeftLabel.ToString().Contains(TEXT("3×3")));
	}
	{
		// manifest 里写了别的预览文件名：按 manifest 的。
		FPageFixture Fixture;
		Fixture.World.ManifestToReturn.Preview = TEXT("tiles.png");
		Fixture.SelectTypical(*this);
		Fixture.Actions->Run();
		CompleteRun(Fixture, Fixture.World.JobId);
		TestEqual(TEXT("按 manifest 里的预览文件名"), Fixture.Actions->GetPreviewPlan().LeftKey, FDreamTextureReflowActions::MakeMaterialFileKey(TEXT("mat_job_1"), TEXT("tiles.png")));
	}

	// ---- UV 展开：补通道 → 原图 | 补出的法线（底色是原样进分解的，并排看不出东西）----
	{
		FPageFixture Fixture;
		Fixture.SelectTypical(*this);
		Fixture.Actions->SetUVKind(EDreamTextureUVKind::Unwrapped);
		Fixture.Actions->Run();
		CompleteRun(Fixture, Fixture.World.JobId);
		const TSharedPtr<FDreamTextureReflowRun> Run = Fixture.Actions->GetRun();
		const FDreamTextureReflowPreviewPlan Plan = Fixture.Actions->GetPreviewPlan();
		TestTrue(TEXT("UV 展开：并排两张，不出 3×3"), Plan.Kind == EKind::SideBySide);
		TestEqual(TEXT("左边是送上去的原图（刚导出的 PNG）"), Plan.LeftKey, FDreamTextureReflowActions::MakeFileKey(Run->GetExportedFile()));
		TestEqual(TEXT("右边是补出的法线"), Plan.RightKey, FDreamTextureReflowActions::MakeMaterialFileKey(TEXT("mat_job_1"), TEXT("normal.png")));
		TestEqual(TEXT("左标签"), Plan.LeftLabel.ToString(), FString(TEXT("Original")));
		TestEqual(TEXT("右标签"), Plan.RightLabel.ToString(), FString(TEXT("Generated ")) + DreamTextureReflow::GetChannelLabel(TEXT("normal")));
		TestFalse(TEXT("不是 preview.png（3×3 平铺图在 UV 展开时没有意义）"), Plan.RightKey.Contains(TEXT("preview.png")) || Plan.LeftKey.Contains(TEXT("preview.png")));
	}

	// ---- UV 展开：改写外观 → 原图 | 改写后的底色 ----
	{
		FPageFixture Fixture;
		Fixture.SelectTypical(*this);
		Fixture.Actions->SetMode(EDreamTextureReflowMode::Restyle);
		Fixture.Actions->SetPrompt(TEXT("worn leather"));
		Fixture.Actions->SetUVKind(EDreamTextureUVKind::Unwrapped);
		Fixture.Actions->Run();
		CompleteRun(Fixture, Fixture.World.JobId);
		const FDreamTextureReflowPreviewPlan Plan = Fixture.Actions->GetPreviewPlan();
		TestTrue(TEXT("并排"), Plan.Kind == EKind::SideBySide);
		TestEqual(TEXT("右边是改写后的底色（和原图并排最直观）"), Plan.RightKey, FDreamTextureReflowActions::MakeMaterialFileKey(TEXT("mat_job_1"), TEXT("basecolor.png")));
		TestEqual(TEXT("右标签"), Plan.RightLabel.ToString(), FString(TEXT("Restyled ")) + DreamTextureReflow::GetChannelLabel(TEXT("basecolor")));
	}

	// ---- manifest 里缺法线：退到别的新补出来的通道；一个都没有就不画 ----
	{
		FPageFixture Fixture;
		Fixture.World.ManifestToReturn.Maps.Remove(TEXT("normal"));
		FDreamTextureMapInfo Rough;
		Rough.Channel = TEXT("roughness");
		Rough.File = TEXT("roughness.png");
		Fixture.World.ManifestToReturn.Maps.Add(TEXT("roughness"), Rough);
		Fixture.SelectTypical(*this);
		Fixture.Actions->SetUVKind(EDreamTextureUVKind::Unwrapped);
		Fixture.Actions->Run();
		CompleteRun(Fixture, Fixture.World.JobId);
		TestEqual(TEXT("缺法线：退到粗糙度"), Fixture.Actions->GetPreviewPlan().RightKey, FDreamTextureReflowActions::MakeMaterialFileKey(TEXT("mat_job_1"), TEXT("roughness.png")));
	}
	{
		FPageFixture Fixture;
		Fixture.World.ManifestToReturn.Maps.Reset();
		Fixture.SelectTypical(*this);
		Fixture.Actions->SetUVKind(EDreamTextureUVKind::Unwrapped);
		Fixture.Actions->Run();
		CompleteRun(Fixture, Fixture.World.JobId);
		TestTrue(TEXT("manifest 里没有可看的通道：不画（说明文字仍在）"), Fixture.Actions->GetPreviewPlan().Kind == EKind::None);
		TestTrue(TEXT("运行本身仍是成功的"), Fixture.Actions->GetRun()->GetState() == EState::Succeeded);
	}

	// ---- manifest 没拉到：UV 展开先不画并排（平铺不受影响，preview.png 是约定名）----
	{
		FPageFixture Fixture;
		Fixture.World.bManifestOK = false;
		Fixture.SelectTypical(*this);
		Fixture.Actions->SetUVKind(EDreamTextureUVKind::Unwrapped);
		Fixture.Actions->Run();
		CompleteRun(Fixture, Fixture.World.JobId);
		TestTrue(TEXT("UV 展开：没有 manifest 就不知道结果有哪些通道：不画"), Fixture.Actions->GetPreviewPlan().Kind == EKind::None);
		TestTrue(TEXT("运行仍是成功的（预览只是锦上添花）"), Fixture.Actions->GetRun()->GetState() == EState::Succeeded);
	}
	{
		FPageFixture Fixture;
		Fixture.World.bManifestOK = false;
		Fixture.SelectTypical(*this);
		Fixture.Actions->Run();
		CompleteRun(Fixture, Fixture.World.JobId);
		TestEqual(TEXT("平铺：没有 manifest 也用约定的 preview.png"), Fixture.Actions->GetPreviewPlan().LeftKey, FDreamTextureReflowActions::MakeMaterialFileKey(TEXT("mat_job_1"), TEXT("preview.png")));
	}

	// ---- 键的格式 ----
	TestEqual(TEXT("本地文件键"), FDreamTextureReflowActions::MakeFileKey(TEXT("C:/x/y.png")), FString(TEXT("file:C:/x/y.png")));
	TestEqual(TEXT("后端文件键"), FDreamTextureReflowActions::MakeMaterialFileKey(TEXT("mat_1"), TEXT("normal.png")), FString(TEXT("mat:mat_1/normal.png")));
	return true;
}

// ---------------------------------------------------------------------------
// 面板：UV 展开下各块都能构造
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowUnwrappedPanelTest, "DreamTexture.Editor.Reflow.Unwrapped.Panel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowUnwrappedPanelTest::RunTest(const FString& Parameters)
{
	FPageFixture Fixture;
	Fixture.SelectTypical(*this);

	TSharedRef<SDreamTextureReflowPanel> Panel = SNew(SDreamTextureReflowPanel).Model(Fixture.Model).Actions(Fixture.Actions).bThumbnails(false).bAutoRefresh(false);
	Panel->BuildWidgetsForTest();  // 平铺

	Fixture.Actions->SetUVKind(EDreamTextureUVKind::Unwrapped);
	Panel->BuildWidgetsForTest();  // UV 展开，补通道

	Fixture.Actions->SetDecomposeId(TEXT("dec-chord-v1"));
	Panel->BuildWidgetsForTest();  // UV 展开 + CHORD：分布外提醒

	Fixture.Actions->SetMode(EDreamTextureReflowMode::Restyle);
	Fixture.Actions->SetPrompt(TEXT("worn leather"));
	Fixture.Actions->SetDenoise(0.7);
	Panel->BuildWidgetsForTest();  // 改写外观：重绘幅度超限的警告与按钮

	Fixture.Actions->SetDenoise(0.3);
	Fixture.Actions->Run();
	CompleteRun(Fixture, Fixture.World.JobId);
	Panel->BuildWidgetsForTest();  // 成功：并排预览

	Fixture.Select(Fixture.Master, TEXT("/Game/Test/M_Rock.M_Rock"), TypicalRows(), false);
	Panel->BuildWidgetsForTest();  // 换了材质
	TestTrue(TEXT("构造与布局都没崩"), true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
