// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureReflowTestUtils.h"
#include "SDreamTextureReflowActions.h"
#include "SDreamTextureReflowPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SNullWidget.h"

using namespace DreamTextureReflowTest;

/*
 * 回流页动作区（M8 平铺路径）的页面模型：两个模式 × 分解模型 × 勾选通道 × 新建/原地覆盖 × 后端能力 × 连接三级，
 * 以及一次运行怎么挂在页面上。
 *
 * 选区、解析、工作流目录、连接、运行的外部能力全是假的；目录用 Tests/Data/Reflow/workflows.json
 * （新后端真实 /api/workflows 应答的截取，带「已有底图」出图段）。
 */

namespace
{
	using EState = FDreamTextureReflowRun::EState;

	using FFixture = FPageFixture;

	FString Join(const TSet<FString>& Set)
	{
		TArray<FString> Items = Set.Array();
		Items.Sort();
		return FString::Join(Items, TEXT(","));
	}

	const FDreamTextureReflowChannelView* FindView(const TArray<FDreamTextureReflowChannelView>& Views, const TCHAR* Channel)
	{
		return Views.FindByPredicate([Channel](const FDreamTextureReflowChannelView& View) { return View.Channel == Channel; });
	}

	FText WhyNot(const FDreamTextureReflowActions& Actions)
	{
		FText Why;
		Actions.CanRun(&Why);
		return Why;
	}
}

// ---------------------------------------------------------------------------
// 目录：出图段、分解段、组合、许可
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowActionsCatalogTest, "DreamTexture.Editor.Reflow.Actions.Catalog", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowActionsCatalogTest::RunTest(const FString& Parameters)
{
	FFixture Fixture;
	Fixture.Select(Fixture.Instance, TEXT("/Game/Test/MI_Rock.MI_Rock"), TypicalRows());
	Fixture.Build(*this, /*bCompleteCatalog*/ false);
	FDreamTextureReflowActions& Actions = *Fixture.Actions;

	// ---- 加载：读取中 → 到位；晚到的旧应答作废 ----
	TestTrue(TEXT("构造时就去拉目录"), Fixture.CatalogCalls.Num() == 1);
	TestTrue(TEXT("读取中"), Actions.GetCatalogState() == FDreamTextureReflowActions::ECatalogState::Loading);
	TestNull(TEXT("目录没到：拼不出工作流"), Actions.GetCurrentWorkflow());

	Actions.LoadCatalog();
	TestEqual(TEXT("再拉一次"), Fixture.CatalogCalls.Num(), 2);
	Fixture.CompleteCatalog(Fixture.Catalog, 1);
	TestTrue(TEXT("第二次的应答到位"), Actions.GetCatalogState() == FDreamTextureReflowActions::ECatalogState::Ready);
	FDreamTextureWorkflowCatalog Empty;
	Fixture.CompleteCatalog(Empty, 0);
	TestTrue(TEXT("晚到的第一次应答（代数不对）被丢掉，不覆盖新目录"), Actions.GetCatalogState() == FDreamTextureReflowActions::ECatalogState::Ready && Actions.GetCatalog().Workflows.Num() > 0);

	// ---- 出图段与分解段 ----
	const FDreamTextureWorkflowMeta* Upload = Actions.FindUploadSource();
	if (TestNotNull(TEXT("找得到「已有底图」出图段（按声明里的 source_image，不按 id）"), Upload))
	{
		TestEqual(TEXT("是 src-upload-v1"), Upload->Id, FString(TEXT("src-upload-v1")));
	}
	const TArray<const FDreamTextureWorkflowMeta*> Restyle = Actions.GetRestyleSources();
	if (TestEqual(TEXT("改写外观能用的出图段：两个本地的（云端的 reference 是发给云端的，没有重绘幅度）"), Restyle.Num(), 2))
	{
		TestEqual(TEXT("按 id 排序：写实在前"), Restyle[0]->Id, FString(TEXT("src-local-realistic-v1")));
		TestEqual(TEXT("手绘"), Restyle[1]->Id, FString(TEXT("src-local-stylized-v1")));
	}
	const TArray<const FDreamTextureWorkflowMeta*> Decompose = Actions.GetDecomposeOptions();
	if (TestEqual(TEXT("两个分解段"), Decompose.Num(), 2))
	{
		TestEqual(TEXT("CHORD 在前（按 id）"), Decompose[0]->Id, FString(TEXT("dec-chord-v1")));
		TestEqual(TEXT("传统派生"), Decompose[1]->Id, FString(TEXT("dec-derive-v1")));
	}

	// ---- 默认选择与组合 id ----
	TestEqual(TEXT("默认分解模型：按 id 排序的第一个（与生成页一致）"), Actions.GetDecomposeId(), FString(TEXT("dec-chord-v1")));
	TestEqual(TEXT("补通道的组合：已有底图 × CHORD"), Actions.GetCurrentWorkflow()->Id, FString(TEXT("src-upload-v1.dec-chord-v1")));
	Actions.SetDecomposeId(TEXT("dec-derive-v1"));
	TestEqual(TEXT("换成传统派生"), Actions.GetCurrentWorkflow()->Id, FString(TEXT("src-upload-v1.dec-derive-v1")));
	Actions.SetDecomposeId(TEXT("dec-nope"));
	TestEqual(TEXT("选了目录里没有的：落回默认，不提交一个不存在的组合"), Actions.GetDecomposeId(), FString(TEXT("dec-chord-v1")));

	Actions.SetMode(EDreamTextureReflowMode::Restyle);
	TestEqual(TEXT("改写外观的组合：本地写实 × CHORD"), Actions.GetCurrentWorkflow()->Id, FString(TEXT("src-local-realistic-v1.dec-chord-v1")));
	Actions.SetRestyleSourceId(TEXT("src-local-stylized-v1"));
	TestEqual(TEXT("换成手绘出图"), Actions.GetCurrentWorkflow()->Id, FString(TEXT("src-local-stylized-v1.dec-chord-v1")));

	// ---- 许可：CHORD 仅限研究用途 ----
	{
		const FDreamTextureReflowDecomposeHint Hint = Actions.GetDecomposeHint();
		TestTrue(TEXT("CHORD：不可商用"), Hint.bNonCommercial);
		TestTrue(TEXT("原因里点名 CHORD 与研究用途"), Hint.LicenseReason.Contains(TEXT("CHORD")) && Hint.LicenseReason.Contains(TEXT("Research")));
		TestFalse(TEXT("手绘 × CHORD：后端给了「搭不上」的提醒（只提醒不拦）"), Hint.Mismatch.IsEmpty());
	}
	Actions.SetRestyleSourceId(TEXT("src-local-realistic-v1"));
	TestTrue(TEXT("写实 × CHORD：没有搭不上的提醒"), Actions.GetDecomposeHint().Mismatch.IsEmpty());
	Actions.SetDecomposeId(TEXT("dec-derive-v1"));
	{
		const FDreamTextureReflowDecomposeHint Hint = Actions.GetDecomposeHint();
		TestFalse(TEXT("传统派生：可商用"), Hint.bNonCommercial);
		TestTrue(TEXT("没有许可原因"), Hint.LicenseReason.IsEmpty());
	}
	TestTrue(TEXT("下拉里 CHORD 一项标出「仅限研究用途」"), FDreamTextureReflowActions::DescribeSegment(*Actions.GetCatalog().FindSegment(TEXT("dec-chord-v1"))).ToString().Contains(TEXT("Research use only")));
	TestFalse(TEXT("传统派生一项不标"), FDreamTextureReflowActions::DescribeSegment(*Actions.GetCatalog().FindSegment(TEXT("dec-derive-v1"))).ToString().Contains(TEXT("Research")));

	// ---- 失败：保留旧目录；没旧目录时是 Failed 并带原因 ----
	Actions.LoadCatalog();
	Fixture.CatalogCalls.Last()(false, FDreamTextureWorkflowCatalog(), TEXT("读取工作流列表失败：连不上"));
	TestTrue(TEXT("已有目录时一次失败不清空（仍 Ready）"), Actions.GetCatalogState() == FDreamTextureReflowActions::ECatalogState::Ready);
	TestTrue(TEXT("错误原因留着"), Actions.GetCatalogError().Contains(TEXT("连不上")));

	FFixture Fresh;
	Fresh.Select(Fresh.Instance, TEXT("/Game/Test/MI_Rock.MI_Rock"), TypicalRows());
	Fresh.Build(*this, false);
	Fresh.CatalogCalls[0](false, FDreamTextureWorkflowCatalog(), TEXT("读取工作流列表失败：连不上"));
	TestTrue(TEXT("没有旧目录时失败就是 Failed"), Fresh.Actions->GetCatalogState() == FDreamTextureReflowActions::ECatalogState::Failed);

	// 链路变 Live 且目录没拉到：自动再拉（后端刚起来）。
	Fresh.Actions->HandleLinkChanged(EDreamTextureLink::Connecting);
	TestEqual(TEXT("链路没到 Live 不重拉"), Fresh.CatalogCalls.Num(), 1);
	Fresh.Actions->HandleLinkChanged(EDreamTextureLink::Live);
	TestEqual(TEXT("链路到 Live 且目录是 Failed：重拉"), Fresh.CatalogCalls.Num(), 2);
	Fresh.CompleteCatalog(Fresh.Catalog);
	Fresh.Actions->HandleLinkChanged(EDreamTextureLink::Live);
	TestEqual(TEXT("目录已经到位：不重复拉"), Fresh.CatalogCalls.Num(), 2);

	// ---- 旧后端：没有「已有底图」出图段 ----
	FFixture Old;
	Old.Select(Old.Instance, TEXT("/Game/Test/MI_Rock.MI_Rock"), TypicalRows());
	Old.Build(*this, false);
	Old.CompleteCatalog(WithoutUpload(Old.Catalog));
	TestNull(TEXT("旧后端：没有「已有底图」出图段"), Old.Actions->FindUploadSource());
	TestNull(TEXT("旧后端：补通道拼不出工作流"), Old.Actions->GetCurrentWorkflow());
	Old.Actions->SetMode(EDreamTextureReflowMode::Restyle);
	TestNotNull(TEXT("旧后端：改写外观不受影响"), Old.Actions->GetCurrentWorkflow());
	return true;
}

// ---------------------------------------------------------------------------
// 通道：现状、默认勾选、打包图成组、随材质变
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowActionsChannelsTest, "DreamTexture.Editor.Reflow.Actions.Channels", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowActionsChannelsTest::RunTest(const FString& Parameters)
{
	FFixture Fixture;
	Fixture.SelectTypical(*this);
	FDreamTextureReflowActions& Actions = *Fixture.Actions;

	// ---- 补通道的默认：缺的勾上，已有的不动 ----
	TestEqual(TEXT("默认只勾缺的：高度"), Join(Actions.GetSelection()), FString(TEXT("height")));
	TArray<FDreamTextureReflowChannelView> Views = Actions.GetChannelViews();
	TestEqual(TEXT("六行"), Views.Num(), 6);

	if (const FDreamTextureReflowChannelView* Base = FindView(Views, TEXT("basecolor")))
	{
		TestTrue(TEXT("底色：已有"), Base->bPresent);
		TestFalse(TEXT("底色：补通道时锁定"), Base->bEnabled);
		TestFalse(TEXT("底色：不勾"), Base->bChecked);
		TestTrue(TEXT("状态里有贴图名与参数"), Base->Status.ToString().Contains(TEXT("T_Base")) && Base->Status.ToString().Contains(TEXT("BaseColor")));
		TestTrue(TEXT("提示说明底色沿用原图的原因"), Base->Tooltip.ToString().Contains(TEXT("original texture")));
	}
	if (const FDreamTextureReflowChannelView* Height = FindView(Views, TEXT("height")))
	{
		TestFalse(TEXT("高度：缺失"), Height->bPresent);
		TestTrue(TEXT("高度：勾上"), Height->bChecked && Height->bEnabled);
		TestEqual(TEXT("状态：缺失"), Height->Status.ToString(), FString(TEXT("Missing")));
	}
	if (const FDreamTextureReflowChannelView* Roughness = FindView(Views, TEXT("roughness")))
	{
		TestTrue(TEXT("粗糙度：已有，在 ORM 里"), Roughness->bPresent && Roughness->Status.ToString().Contains(TEXT("T_ORM")));
		TestTrue(TEXT("状态里说明与 AO、金属度同一张"), Roughness->Status.ToString().Contains(DreamTextureReflow::GetChannelLabel(TEXT("ao"))) && Roughness->Status.ToString().Contains(DreamTextureReflow::GetChannelLabel(TEXT("metallic"))));
		TestFalse(TEXT("已有的默认不勾"), Roughness->bChecked);
		TestTrue(TEXT("提示说明成组"), Roughness->Tooltip.ToString().Contains(TEXT("together")));
	}

	// ---- 打包图成组：勾一个，同组的都勾 ----
	Actions.SetChannelChecked(TEXT("roughness"), true);
	TestEqual(TEXT("勾粗糙度：ORM 三路一起勾"), Join(Actions.GetSelection()), FString(TEXT("ao,height,metallic,roughness")));
	Actions.SetChannelChecked(TEXT("ao"), false);
	TestEqual(TEXT("取消 AO：三路一起取消"), Join(Actions.GetSelection()), FString(TEXT("height")));
	Actions.SetChannelChecked(TEXT("basecolor"), true);
	TestFalse(TEXT("补通道时底色勾不上"), Actions.IsChannelChecked(TEXT("basecolor")));

	// ---- 恢复默认 ----
	Actions.SetChannelChecked(TEXT("normal"), true);
	Actions.SetChannelChecked(TEXT("height"), false);
	TestEqual(TEXT("手动勾选后"), Join(Actions.GetSelection()), FString(TEXT("normal")));
	Actions.ResetSelection();
	TestEqual(TEXT("恢复默认"), Join(Actions.GetSelection()), FString(TEXT("height")));

	// ---- 换模式：默认随模式变 ----
	Actions.SetChannelChecked(TEXT("normal"), true);
	Actions.SetMode(EDreamTextureReflowMode::Restyle);
	TestEqual(TEXT("改写外观：默认底色与缺的高度（手动勾选是针对上一个模式的，不带过来）"), Join(Actions.GetSelection()), FString(TEXT("basecolor,height")));
	{
		// 视图数组要先存下来：FindView 返回的是数组里的指针，临时数组在这一句结束时就没了。
		const TArray<FDreamTextureReflowChannelView> RestyleViews = Actions.GetChannelViews();
		const FDreamTextureReflowChannelView* Base = FindView(RestyleViews, TEXT("basecolor"));
		TestTrue(TEXT("改写外观时底色不锁"), Base != nullptr && Base->bEnabled && Base->bChecked);
		TestTrue(TEXT("提示说明取消勾选就沿用原底色"), Base != nullptr && Base->Tooltip.ToString().Contains(TEXT("keeps the original Base Color")));
	}
	Actions.SetMode(EDreamTextureReflowMode::FillChannels);
	TestEqual(TEXT("换回补通道：回到默认"), Join(Actions.GetSelection()), FString(TEXT("height")));

	// ---- 同一个材质重新解析：手动勾选保留；没动过的跟着现状走 ----
	Fixture.Model->Reanalyze();
	TestEqual(TEXT("重新解析：没动过的默认保持"), Join(Actions.GetSelection()), FString(TEXT("height")));
	Actions.SetChannelChecked(TEXT("normal"), true);
	Fixture.Model->Reanalyze();
	TestEqual(TEXT("重新解析：动过的勾选保留（同一个材质）"), Join(Actions.GetSelection()), FString(TEXT("height,normal")));

	// ---- 用户指认之后通道现状变了：没动过的默认跟着变 ----
	{
		FFixture Fix2;
		Fix2.Select(Fix2.Instance, TEXT("/Game/Test/MI_Odd.MI_Odd"), { MakeRow(TEXT("T_Base"), EDreamTextureRole::BaseColor, { TEXT("BaseColor") }), MakeRow(TEXT("T_Mystery"), EDreamTextureRole::Unknown) });
		Fix2.Build(*this);
		TestTrue(TEXT("指认前：法线缺失，默认要补"), Fix2.Actions->GetSelection().Contains(TEXT("normal")));
		Fix2.Model->SetRole(TEXT("/Game/Test/T_Mystery.T_Mystery"), EDreamTextureRole::Normal);
		TestFalse(TEXT("指认成法线之后：法线已有，默认不再覆盖它"), Fix2.Actions->GetSelection().Contains(TEXT("normal")));
		const TArray<FDreamTextureReflowChannelView> Fix2Views = Fix2.Actions->GetChannelViews();
		if (const FDreamTextureReflowChannelView* Normal = FindView(Fix2Views, TEXT("normal")))
		{
			TestTrue(TEXT("法线现在是已有的"), Normal->bPresent && Normal->Status.ToString().Contains(TEXT("T_Mystery")));
		}
	}

	// ---- 换了材质：表单回到默认 ----
	{
		FFixture Fix3;
		Fix3.SelectTypical(*this);
		Fix3.Actions->SetChannelChecked(TEXT("normal"), true);
		Fix3.Actions->SetDenoise(0.4);
		const FString Chosen = Join(Fix3.Actions->GetSelection());
		TestEqual(TEXT("先手动勾了法线"), Chosen, FString(TEXT("height,normal")));

		UMaterialInstanceConstant* Other = NewObject<UMaterialInstanceConstant>(GetTransientPackage(), NAME_None, RF_Transient);
		Fix3.Select(Other, TEXT("/Game/Test/MI_Other.MI_Other"), BaseOnlyRows());
		TestEqual(TEXT("换了材质：勾选回到新材质的默认（缺法线、ORM、高度）"), Join(Fix3.Actions->GetSelection()), FString(TEXT("ao,height,metallic,normal,roughness")));
		TestEqual(TEXT("换了材质：重绘幅度的手动值不带过去"), Fix3.Actions->GetDenoise(), DreamTextureReflow::DefaultTiledDenoise);
	}

	// ---- 没有可用解析时 ----
	{
		FFixture Fix4;
		Fix4.Build(*this);
		TestEqual(TEXT("没选东西：没有通道行"), Fix4.Actions->GetChannelViews().Num(), 0);
		TestEqual(TEXT("没有勾选"), Fix4.Actions->GetSelection().Num(), 0);
	}
	return true;
}

// ---------------------------------------------------------------------------
// 一次运行的输入：补通道
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowActionsSpecFillTest, "DreamTexture.Editor.Reflow.Actions.Spec.Fill", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowActionsSpecFillTest::RunTest(const FString& Parameters)
{
	FFixture Fixture;
	Fixture.SelectTypical(*this);
	FDreamTextureReflowActions& Actions = *Fixture.Actions;

	FDreamTextureReflowRunSpec Spec;
	FText Error;
	if (!TestTrue(FString::Printf(TEXT("拼得出输入（%s）"), *Error.ToString()), Actions.BuildSpec(Spec, &Error)))
	{
		return false;
	}

	// ---- 请求 ----
	const FDreamTextureGenerateRequest& Request = Spec.Request;
	TestEqual(TEXT("工作流：已有底图 × 默认分解模型"), Request.WorkflowId, FString(TEXT("src-upload-v1.dec-chord-v1")));
	TestEqual(TEXT("一个变体"), Request.Variants, 1);
	TestEqual(TEXT("展示名：去掉 MI_ 前缀 + 补通道（新实例叫 MI_<名字>，名字里再带 MI_ 就成了 MI_MIRock）"), Request.Name, FString(TEXT("Rock fill")));
	TestFalse(TEXT("补通道不带提示词（出图段没有这个参数）"), Request.StringParams.Contains(TEXT("prompt")));
	TestFalse(TEXT("补通道不带重绘幅度"), Request.FloatParams.Contains(TEXT("denoise")));
	TestFalse(TEXT("上传文件名要运行时才知道，这里还没有"), Request.StringParams.Contains(TEXT("source_image")));
	TestEqual(TEXT("上传文件名填进 source_image"), Spec.UploadParamKey, FString(TEXT("source_image")));
	TestEqual(TEXT("分辨率取参数默认值"), Request.IntParams.FindRef(TEXT("resolution")), static_cast<int64>(1024));
	FString ValidateError;
	TestTrue(FString::Printf(TEXT("通过 Client 的本地校验（%s）"), *ValidateError), Request.Validate(ValidateError));

	// ---- derived_from ----
	TestEqual(TEXT("kind"), Request.DerivedFrom.Kind, FString(TEXT("ue_material")));
	TestEqual(TEXT("mode"), Request.DerivedFrom.Mode, FString(TEXT("fill_channels")));
	TestEqual(TEXT("asset_path 是选中材质的资产路径"), Request.DerivedFrom.AssetPath, FString(TEXT("/Game/Test/MI_Rock.MI_Rock")));
	TestEqual(TEXT("engine"), Request.DerivedFrom.Engine, FString(TEXT("5.8.2")));
	TestEqual(TEXT("maps_in 要等导出之后才知道指纹，由运行填"), Request.DerivedFrom.MapsIn.Num(), 0);

	// ---- 导出 ----
	if (TestEqual(TEXT("导出只导底图那一张"), Spec.ExportAnalysis.Textures.Num(), 1))
	{
		TestEqual(TEXT("导的是底色"), Spec.ExportAnalysis.Textures[0].AssetName, FString(TEXT("T_Base")));
	}
	TestEqual(TEXT("底图路径"), Spec.SourceTexturePath, FString(TEXT("/Game/Test/T_Base.T_Base")));
	TestEqual(TEXT("导出长边默认 2048（后端会缩到 768/1024，更大的图没好处，还会撞 32 MB 上传上限）"), Spec.ExportMaxEdge, 2048);
	Fixture.Model->SetExportMaxEdge(1024);
	Actions.BuildSpec(Spec);
	TestEqual(TEXT("页面上选了更小的上限：照它"), Spec.ExportMaxEdge, 1024);
	Fixture.Model->SetExportMaxEdge(4096);
	Actions.BuildSpec(Spec);
	TestEqual(TEXT("页面上选了更大的上限：夹到 2048"), Spec.ExportMaxEdge, 2048);
	Fixture.Model->SetExportMaxEdge(0);
	Actions.BuildSpec(Spec);
	TestEqual(TEXT("原尺寸（0）：用默认 2048，不是不缩"), Spec.ExportMaxEdge, 2048);

	// ---- 通道计划：底色指向原图，没勾的沿用，只导入勾的 ----
	TestFalse(TEXT("新建"), Spec.bInPlace);
	TestTrue(TEXT("显式给通道"), Spec.ImportOptions.bExplicitChannels);
	TestEqual(TEXT("只导入缺的高度"), FString::Join(Spec.ImportOptions.Channels, TEXT(",")), FString(TEXT("height")));
	TestEqual(TEXT("补通道回填的 BaseColor 是原图"), Spec.ImportOptions.ReuseTextures.FindRef(TEXT("basecolor")), FString(TEXT("/Game/Test/T_Base.T_Base")));
	TestEqual(TEXT("没勾的法线沿用"), Spec.ImportOptions.ReuseTextures.FindRef(TEXT("normal")), FString(TEXT("/Game/Test/T_N.T_N")));
	TestEqual(TEXT("没勾的 ORM 沿用"), Spec.ImportOptions.ReuseTextures.FindRef(TEXT("orm")), FString(TEXT("/Game/Test/T_ORM.T_ORM")));
	TestTrue(TEXT("要建材质实例"), Spec.ImportOptions.bCreateMaterialInstance);
	TestTrue(TEXT("标题"), Spec.Title.Contains(TEXT("Fill missing channels")) && Spec.Title.Contains(TEXT("MI_Rock")));

	// ---- 分解模型换了：组合 id 跟着换，其余不变 ----
	Actions.SetDecomposeId(TEXT("dec-derive-v1"));
	Actions.BuildSpec(Spec);
	TestEqual(TEXT("传统派生"), Spec.Request.WorkflowId, FString(TEXT("src-upload-v1.dec-derive-v1")));

	// ---- 分辨率 ----
	TestEqual(TEXT("分辨率候选（派生段有 512/768/1024，已有底图段 768/1024；合并后取先出现的声明）"), Actions.GetResolutionOptions().Num() > 0, true);
	Actions.SetResolution(768);
	Actions.BuildSpec(Spec);
	TestEqual(TEXT("选了 768"), Spec.Request.IntParams.FindRef(TEXT("resolution")), static_cast<int64>(768));
	Actions.SetResolution(999);
	Actions.BuildSpec(Spec);
	TestEqual(TEXT("选了候选里没有的：回到默认，不提交会被后端拒掉的值"), Spec.Request.IntParams.FindRef(TEXT("resolution")), static_cast<int64>(1024));

	// ---- 勾选透传到计划 ----
	Actions.SetChannelChecked(TEXT("roughness"), true);
	Actions.BuildSpec(Spec);
	TestEqual(TEXT("勾了 ORM 里的粗糙度：导入 ORM 与高度"), FString::Join(Spec.ImportOptions.Channels, TEXT(",")), FString(TEXT("orm,height")));
	TestFalse(TEXT("ORM 要重新生成，就不再沿用原来的"), Spec.ImportOptions.ReuseTextures.Contains(TEXT("orm")));

	// ---- 说明带上 ----
	Actions.ResetSelection();
	Actions.SetChannelChecked(TEXT("normal"), true);
	Actions.BuildSpec(Spec);
	TestEqual(TEXT("勾了法线与高度"), FString::Join(Spec.ImportOptions.Channels, TEXT(",")), FString(TEXT("normal,height")));
	return true;
}

// ---------------------------------------------------------------------------
// 一次运行的输入：改写外观
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowActionsSpecRestyleTest, "DreamTexture.Editor.Reflow.Actions.Spec.Restyle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowActionsSpecRestyleTest::RunTest(const FString& Parameters)
{
	FFixture Fixture;
	Fixture.SelectTypical(*this);
	FDreamTextureReflowActions& Actions = *Fixture.Actions;
	Actions.SetMode(EDreamTextureReflowMode::Restyle);

	// ---- 提示词必填 ----
	FDreamTextureReflowRunSpec Spec;
	FText Error;
	TestFalse(TEXT("没写提示词：拼不出"), Actions.BuildSpec(Spec, &Error));
	TestTrue(TEXT("原因点明提示词"), Error.ToString().Contains(TEXT("prompt")));
	Actions.SetPrompt(TEXT("   \n  "));
	TestFalse(TEXT("全是空白也不行"), Actions.BuildSpec(Spec, &Error));

	Actions.SetPrompt(TEXT("  mossy broken rock  "));
	if (!TestTrue(FString::Printf(TEXT("写了提示词：拼得出（%s）"), *Error.ToString()), Actions.BuildSpec(Spec, &Error)))
	{
		return false;
	}

	const FDreamTextureGenerateRequest& Request = Spec.Request;
	TestEqual(TEXT("工作流：本地写实 × 默认分解模型"), Request.WorkflowId, FString(TEXT("src-local-realistic-v1.dec-chord-v1")));
	TestEqual(TEXT("提示词去掉首尾空白"), Request.StringParams.FindRef(TEXT("prompt")), FString(TEXT("mossy broken rock")));
	TestEqual(TEXT("上传文件名填进 reference"), Spec.UploadParamKey, FString(TEXT("reference")));
	TestEqual(TEXT("展示名：改写"), Request.Name, FString(TEXT("Rock restyle")));
	TestTrue(TEXT("种子随机（改写要出新东西）"), Request.IsRandomSeed());
	TestEqual(TEXT("derived_from.mode"), Request.DerivedFrom.Mode, FString(TEXT("restyle")));
	TestEqual(TEXT("derived_from.asset_path"), Request.DerivedFrom.AssetPath, FString(TEXT("/Game/Test/MI_Rock.MI_Rock")));
	TestEqual(TEXT("分辨率"), Request.IntParams.FindRef(TEXT("resolution")), static_cast<int64>(1024));
	TestEqual(TEXT("参考图缩放与出图分辨率一致（后端文档：应与出图分辨率一致）"), Request.IntParams.FindRef(TEXT("reference_size")), static_cast<int64>(1024));

	// ---- 重绘幅度 ----
	TestTrue(TEXT("默认 0.55：保留原图结构，换材质表现"), FMath::IsNearlyEqual(Request.FloatParams.FindRef(TEXT("denoise")), 0.55));
	Actions.SetDenoise(0.3);
	Actions.BuildSpec(Spec);
	TestTrue(TEXT("用户给的 0.3"), FMath::IsNearlyEqual(Spec.Request.FloatParams.FindRef(TEXT("denoise")), 0.3));
	Actions.SetDenoise(5.0);
	TestTrue(TEXT("超出后端声明的上限：夹到 1.0"), FMath::IsNearlyEqual(Actions.GetDenoise(), 1.0));
	Actions.SetDenoise(0.0);
	TestTrue(TEXT("低于后端声明的下限：夹到 0.1"), FMath::IsNearlyEqual(Actions.GetDenoise(), 0.1));

	// ---- 分辨率换了，参考图缩放跟着 ----
	Actions.SetDenoise(0.5);
	Actions.SetResolution(768);
	Actions.BuildSpec(Spec);
	TestEqual(TEXT("出图 768"), Spec.Request.IntParams.FindRef(TEXT("resolution")), static_cast<int64>(768));
	TestEqual(TEXT("参考图缩放跟着 768"), Spec.Request.IntParams.FindRef(TEXT("reference_size")), static_cast<int64>(768));

	// ---- 通道：底色重新生成，所以导入；没勾的沿用 ----
	TestEqual(TEXT("导入底色与缺的高度"), FString::Join(Spec.ImportOptions.Channels, TEXT(",")), FString(TEXT("basecolor,height")));
	TestFalse(TEXT("底色重新生成，不沿用原图"), Spec.ImportOptions.ReuseTextures.Contains(TEXT("basecolor")));
	TestTrue(TEXT("没勾的法线仍沿用"), Spec.ImportOptions.ReuseTextures.Contains(TEXT("normal")));
	TestTrue(TEXT("标题"), Spec.Title.Contains(TEXT("Restyle")));

	// ---- 出图模型换成手绘 ----
	Actions.SetRestyleSourceId(TEXT("src-local-stylized-v1"));
	Actions.BuildSpec(Spec);
	TestEqual(TEXT("手绘出图 × CHORD"), Spec.Request.WorkflowId, FString(TEXT("src-local-stylized-v1.dec-chord-v1")));

	// ---- 补通道不受提示词影响 ----
	Actions.SetMode(EDreamTextureReflowMode::FillChannels);
	Actions.BuildSpec(Spec);
	TestFalse(TEXT("补通道不带提示词"), Spec.Request.StringParams.Contains(TEXT("prompt")));
	TestFalse(TEXT("补通道不带重绘幅度"), Spec.Request.FloatParams.Contains(TEXT("denoise")));

	// ---- 后端没有本地出图段 ----
	{
		FFixture NoLocal;
		NoLocal.Select(NoLocal.Instance, TEXT("/Game/Test/MI_Rock.MI_Rock"), TypicalRows());
		NoLocal.Build(*this, false);
		FDreamTextureWorkflowCatalog Cut = NoLocal.Catalog;
		Cut.Segments.RemoveAll([](const FDreamTextureWorkflowMeta& Meta) { return Meta.Id.StartsWith(TEXT("src-local")); });
		Cut.Workflows.RemoveAll([](const FDreamTextureWorkflowMeta& Meta) { return Meta.SourceSegment.StartsWith(TEXT("src-local")); });
		NoLocal.CompleteCatalog(Cut);
		NoLocal.Actions->SetMode(EDreamTextureReflowMode::Restyle);
		NoLocal.Actions->SetPrompt(TEXT("rock"));
		FText Why;
		TestFalse(TEXT("没有本地出图段：改写外观不成立"), NoLocal.Actions->BuildSpec(Spec, &Why));
		TestTrue(TEXT("原因点明改写外观与本地出图"), Why.ToString().Contains(TEXT("Restyle")) && Why.ToString().Contains(TEXT("local image")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// 落回方式：新建 / 原地覆盖
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowActionsInPlaceTest, "DreamTexture.Editor.Reflow.Actions.InPlace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowActionsInPlaceTest::RunTest(const FString& Parameters)
{
	FFixture Fixture;
	Fixture.SelectTypical(*this);
	FDreamTextureReflowActions& Actions = *Fixture.Actions;

	// ---- 选中的是材质实例：可以原地覆盖 ----
	FString Why;
	TestTrue(TEXT("材质实例：能原地覆盖"), Actions.CanOverwriteInPlace(&Why));
	TestFalse(TEXT("默认是新建"), Actions.IsInPlace());
	Actions.SetInPlace(true);
	TestTrue(TEXT("选上原地覆盖"), Actions.IsInPlace());

	// 此时只勾了缺的高度，而材质里没有高度参数：原地覆盖没处可放，开始不了，原因说清楚。
	FText Reason;
	TestFalse(TEXT("只勾了高度：原地覆盖没处可放，不能开始"), Actions.CanRun(&Reason));
	TestTrue(TEXT("原因点明高度"), Reason.ToString().Contains(DreamTextureReflow::GetChannelLabel(TEXT("height"))));
	const TArray<FDreamTextureReflowChannelView> InPlaceViews = Actions.GetChannelViews();
	if (const FDreamTextureReflowChannelView* Height = FindView(InPlaceViews, TEXT("height")))
	{
		TestTrue(TEXT("界面上高度那行标成原地改不了"), Height->bInPlaceBlocked);
		TestTrue(TEXT("提示里有原因"), Height->Tooltip.ToString().Contains(TEXT("no parameter")));
	}

	// 勾上法线（材质里已有、绑着 Normal 参数）：能改。
	Actions.SetChannelChecked(TEXT("normal"), true);
	FDreamTextureReflowRunSpec Spec;
	FText Error;
	if (TestTrue(FString::Printf(TEXT("勾了法线：拼得出（%s）"), *Error.ToString()), Actions.BuildSpec(Spec, &Error)))
	{
		TestTrue(TEXT("原地覆盖"), Spec.bInPlace);
		TestTrue(TEXT("目标是选中的那个实例"), Spec.InPlaceTarget.Get() == Fixture.Instance);
		if (TestEqual(TEXT("一处覆盖"), Spec.Overwrites.Num(), 1))
		{
			TestEqual(TEXT("按选中材质自己的参数名"), Spec.Overwrites[0].Parameters[0].ToString(), FString(TEXT("Normal")));
			TestEqual(TEXT("用后端的法线"), Spec.Overwrites[0].ImportChannel, FString(TEXT("normal")));
		}
		TestEqual(TEXT("只导入法线"), FString::Join(Spec.ImportOptions.Channels, TEXT(",")), FString(TEXT("normal")));
		TestFalse(TEXT("原地覆盖不另建实例"), Spec.ImportOptions.bCreateMaterialInstance);
		TestEqual(TEXT("原地覆盖不用沿用"), Spec.ImportOptions.ReuseTextures.Num(), 0);
		bool bMentionsHeight = false;
		for (const FString& Note : Spec.PlanNotes)
		{
			bMentionsHeight |= Note.Contains(DreamTextureReflow::GetChannelLabel(TEXT("height")));
		}
		TestTrue(TEXT("没处可放的高度记进说明（成功后告诉用户没改）"), bMentionsHeight);
		TestTrue(TEXT("能开始"), Actions.CanRun());
	}

	// ---- 取消勾选后回到新建：计划又是新建的 ----
	Actions.SetInPlace(false);
	Actions.BuildSpec(Spec);
	TestFalse(TEXT("新建"), Spec.bInPlace);
	TestEqual(TEXT("计划回到新建的：沿用没勾的"), Spec.ImportOptions.ReuseTextures.Num() > 0, true);

	// ---- 选中的是母材质：原地覆盖置灰，选不上 ----
	{
		FFixture OnMaster;
		OnMaster.Select(OnMaster.Master, TEXT("/Game/Test/M_Rock.M_Rock"), TypicalRows(), /*bInstance*/ false);
		OnMaster.Build(*this);
		FString MasterWhy;
		TestFalse(TEXT("母材质：不能原地覆盖"), OnMaster.Actions->CanOverwriteInPlace(&MasterWhy));
		TestTrue(TEXT("原因点明母材质、贴图硬连在图里、没有参数"), MasterWhy.Contains(TEXT("master material")) && MasterWhy.Contains(TEXT("wired directly")) && MasterWhy.Contains(TEXT("no parameters")));
		OnMaster.Actions->SetInPlace(true);
		TestFalse(TEXT("置灰的选项选不上（兜底）"), OnMaster.Actions->IsInPlace());
		TestTrue(TEXT("母材质上仍能新建"), OnMaster.Actions->CanRun());
	}

	// ---- 换到母材质：已经选上的原地覆盖被收回 ----
	{
		FFixture Switch;
		Switch.SelectTypical(*this);
		Switch.Actions->SetInPlace(true);
		TestTrue(TEXT("先选上原地覆盖"), Switch.Actions->IsInPlace());
		Switch.Select(Switch.Master, TEXT("/Game/Test/M_Rock.M_Rock"), TypicalRows(), false);
		TestFalse(TEXT("换了材质，原地覆盖回到新建"), Switch.Actions->IsInPlace());
	}

	// ---- 没有解析时 ----
	{
		FFixture None;
		None.Build(*this);
		FString NoneWhy;
		TestFalse(TEXT("没有选择：不能原地覆盖"), None.Actions->CanOverwriteInPlace(&NoneWhy));
		TestFalse(TEXT("有原因"), NoneWhy.IsEmpty());
	}
	return true;
}

// ---------------------------------------------------------------------------
// 能不能开始
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowActionsCanRunTest, "DreamTexture.Editor.Reflow.Actions.CanRun", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowActionsCanRunTest::RunTest(const FString& Parameters)
{
	// ---- 没选东西：用页面自己的说明 ----
	{
		FFixture Fixture;
		Fixture.Build(*this);
		TestFalse(TEXT("没选东西：不能开始"), Fixture.Actions->CanRun());
		TestTrue(TEXT("原因是页面的说明"), WhyNot(*Fixture.Actions).ToString().Contains(TEXT("Get Selection")));
	}

	// ---- 没认出底色 / 底色导不出 ----
	{
		FFixture Fixture;
		Fixture.Select(Fixture.Instance, TEXT("/Game/Test/MI_X.MI_X"), { MakeRow(TEXT("T_N"), EDreamTextureRole::Normal) });
		Fixture.Build(*this);
		TestFalse(TEXT("没有底色"), Fixture.Actions->CanRun());
		TestTrue(TEXT("原因点明没认出底色、并指路"), WhyNot(*Fixture.Actions).ToString().Contains(TEXT("No Base Color texture was recognized")) && WhyNot(*Fixture.Actions).ToString().Contains(TEXT("assign")));
	}
	{
		FFixture Fixture;
		Fixture.Select(Fixture.Instance, TEXT("/Game/Test/MI_X.MI_X"), { MakeRow(TEXT("T_Base"), EDreamTextureRole::BaseColor, {}, /*bExportable*/ false) });
		Fixture.Build(*this);
		TestFalse(TEXT("底色导不出"), Fixture.Actions->CanRun());
		TestTrue(TEXT("原因里有贴图名和导不出的原因"), WhyNot(*Fixture.Actions).ToString().Contains(TEXT("T_Base")) && WhyNot(*Fixture.Actions).ToString().Contains(TEXT("没有源数据")));
	}

	// ---- 目录 ----
	{
		FFixture Fixture;
		Fixture.Select(Fixture.Instance, TEXT("/Game/Test/MI_Rock.MI_Rock"), TypicalRows());
		Fixture.Build(*this, /*bCompleteCatalog*/ false);
		TestFalse(TEXT("目录读取中"), Fixture.Actions->CanRun());
		TestTrue(TEXT("原因：读取中"), WhyNot(*Fixture.Actions).ToString().Contains(TEXT("Reading the workflow list")));
		Fixture.CatalogCalls[0](false, FDreamTextureWorkflowCatalog(), TEXT("读取工作流列表失败：连不上"));
		TestTrue(TEXT("原因：读不到，带后端的话"), WhyNot(*Fixture.Actions).ToString().Contains(TEXT("Could not read the workflow list")) && WhyNot(*Fixture.Actions).ToString().Contains(TEXT("连不上")));
	}
	{
		FFixture Fixture;
		Fixture.Select(Fixture.Instance, TEXT("/Game/Test/MI_Rock.MI_Rock"), TypicalRows());
		Fixture.Build(*this, false);
		Fixture.CompleteCatalog(WithoutUpload(Fixture.Catalog));
		TestFalse(TEXT("旧后端：补通道不能开始"), Fixture.Actions->CanRun());
		TestTrue(TEXT("原因点明不支持补齐通道、要更新后端"), WhyNot(*Fixture.Actions).ToString().Contains(TEXT("Fill missing channels")) && WhyNot(*Fixture.Actions).ToString().Contains(TEXT("Update the backend")));
		Fixture.Actions->SetMode(EDreamTextureReflowMode::Restyle);
		Fixture.Actions->SetPrompt(TEXT("rock"));
		TestTrue(TEXT("旧后端：改写外观照常"), Fixture.Actions->CanRun());
	}

	// ---- 提示词 / 勾选 ----
	{
		FFixture Fixture;
		Fixture.SelectTypical(*this);
		TestTrue(TEXT("补通道：默认就能开始"), Fixture.Actions->CanRun());

		Fixture.Actions->SetChannelChecked(TEXT("height"), false);
		TestFalse(TEXT("什么都没勾：不能开始"), Fixture.Actions->CanRun());
		TestTrue(TEXT("原因：至少勾一个"), WhyNot(*Fixture.Actions).ToString().Contains(TEXT("Check at least one channel")));

		Fixture.Actions->SetMode(EDreamTextureReflowMode::Restyle);
		TestFalse(TEXT("改写外观没写提示词：不能开始"), Fixture.Actions->CanRun());
		TestTrue(TEXT("原因：提示词"), WhyNot(*Fixture.Actions).ToString().Contains(TEXT("prompt")));
		Fixture.Actions->SetPrompt(TEXT("moss"));
		TestTrue(TEXT("写了提示词（底色默认勾着）：能开始"), Fixture.Actions->CanRun());
	}

	// ---- 连接三级：后端没起不拦（会自动启动），ComfyUI 明确没在跑才拦 ----
	{
		FFixture Fixture;
		Fixture.SelectTypical(*this);

		Fixture.Conn->Set(EDreamTextureLink::Offline, false);
		TestTrue(TEXT("后端没应答：不拦（点开始会先启动它）"), Fixture.Actions->CanRun());
		TestTrue(TEXT("但提示一句会先启动"), Fixture.Actions->GetRunNote().ToString().Contains(TEXT("launch")));

		Fixture.Conn->Set(EDreamTextureLink::Stale, true);
		TestTrue(TEXT("长连接断了：可以提交"), Fixture.Actions->CanRun());
		TestTrue(TEXT("提示进度要等重连"), Fixture.Actions->GetRunNote().ToString().Contains(TEXT("event stream")));

		Fixture.Conn->SetAllReady();
		TestTrue(TEXT("三级都好"), Fixture.Actions->CanRun());
		TestTrue(TEXT("没有提示"), Fixture.Actions->GetRunNote().IsEmpty());

		FDreamTextureHealth Health;
		Health.Mode = TEXT("attach");
		Health.bAlive = false;
		Health.Reason = TEXT("无法连接 ComfyUI");
		Fixture.Conn->SetHealth(Health);
		TestFalse(TEXT("ComfyUI 没在运行：拦下（提交了也只会停在「等待 ComfyUI」）"), Fixture.Actions->CanRun());
		TestTrue(TEXT("原因点明 ComfyUI、别去重启后端"), WhyNot(*Fixture.Actions).ToString().Contains(TEXT("ComfyUI")) && WhyNot(*Fixture.Actions).ToString().Contains(TEXT("do not restart the backend")));

		Health.bAlive = true;
		Health.bReady = false;
		Health.Reason = TEXT("队列积压");
		Fixture.Conn->SetHealth(Health);
		TestTrue(TEXT("ComfyUI 忙但在：不拦（会排队）"), Fixture.Actions->CanRun());
	}

	// ---- 底色不是正方形：提醒，不拦 ----
	{
		FFixture Fixture;
		Fixture.Select(Fixture.Instance, TEXT("/Game/Test/MI_Wide.MI_Wide"), { MakeRow(TEXT("T_Wide"), EDreamTextureRole::BaseColor, { TEXT("BaseColor") }, true, 2048, 1024) });
		Fixture.Build(*this);
		TestTrue(TEXT("不是正方形也能开始"), Fixture.Actions->CanRun());
		TestTrue(TEXT("但提醒会被居中裁切"), Fixture.Actions->GetRunNote().ToString().Contains(TEXT("2048×1024")) && Fixture.Actions->GetRunNote().ToString().Contains(TEXT("center-crops")));
	}

	// ---- 多张底色：默认第一张，可以挑 ----
	{
		FFixture Fixture;
		Fixture.Select(Fixture.Instance, TEXT("/Game/Test/MI_Two.MI_Two"), { MakeRow(TEXT("T_BaseA"), EDreamTextureRole::BaseColor, { TEXT("A") }), MakeRow(TEXT("T_BaseB"), EDreamTextureRole::BaseColor, { TEXT("B") }) });
		Fixture.Build(*this);
		TestEqual(TEXT("两张候选"), Fixture.Actions->GetSourceCandidates().Num(), 2);
		TestEqual(TEXT("默认第一张"), Fixture.Actions->GetSourceTexturePath(), FString(TEXT("/Game/Test/T_BaseA.T_BaseA")));
		Fixture.Actions->SetSourceTexture(TEXT("/Game/Test/T_BaseB.T_BaseB"));
		FDreamTextureReflowRunSpec Spec;
		Fixture.Actions->BuildSpec(Spec);
		TestEqual(TEXT("选了第二张：导出的是它"), Spec.SourceTexturePath, FString(TEXT("/Game/Test/T_BaseB.T_BaseB")));
		TestEqual(TEXT("导出分析里只有它"), Spec.ExportAnalysis.Textures[0].AssetName, FString(TEXT("T_BaseB")));
		Fixture.Actions->SetSourceTexture(TEXT("/Game/Test/Nope.Nope"));
		TestEqual(TEXT("选了不在候选里的：回到第一张"), Fixture.Actions->GetSourceTexturePath(), FString(TEXT("/Game/Test/T_BaseA.T_BaseA")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// 运行挂在页面上
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowActionsRunTest, "DreamTexture.Editor.Reflow.Actions.Run", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowActionsRunTest::RunTest(const FString& Parameters)
{
	FFixture Fixture;
	Fixture.SelectTypical(*this);
	FDreamTextureReflowActions& Actions = *Fixture.Actions;

	TArray<FString> Notices;
	TArray<bool> NoticeErrors;
	Actions.OnNotice.AddLambda([&Notices, &NoticeErrors](const FText& Message, bool bIsError)
	{
		Notices.Add(Message.ToString());
		NoticeErrors.Add(bIsError);
	});
	int32 Changes = 0;
	Actions.OnChanged.AddLambda([&Changes]() { ++Changes; });

	// ---- 开始：造一次运行，带着当前表单 ----
	TestFalse(TEXT("还没有运行"), Actions.GetRun().IsValid());
	Actions.Run();
	if (!TestEqual(TEXT("造了一次运行"), Fixture.Runs.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("页面上挂着这次运行"), Actions.GetRun() == Fixture.Runs[0]);
	TestTrue(TEXT("运行已开始（假世界当场回，停在生成）"), Actions.GetRun()->GetStep() == EDreamTextureReflowStep::Generate);
	TestTrue(TEXT("页面知道运行在进行"), Actions.IsRunActive());
	TestEqual(TEXT("运行拿到的是页面当前的表单"), Fixture.RunSpecs[0].Request.WorkflowId, FString(TEXT("src-upload-v1.dec-chord-v1")));
	TestTrue(TEXT("广播了变化"), Changes > 0);

	// 进行中：再点开始什么都不做，也不能开始。
	Actions.Run();
	TestEqual(TEXT("进行中再点开始：不造第二次"), Fixture.Runs.Num(), 1);
	TestFalse(TEXT("进行中不能开始"), Actions.CanRun());
	TestTrue(TEXT("原因：还在进行中"), WhyNot(Actions).ToString().Contains(TEXT("still in progress")));
	TestEqual(TEXT("进行中没有通知"), Notices.Num(), 0);

	// ---- 失败：通知一次，带哪一步 ----
	Fixture.World.PushJob(MakeJob(Fixture.World.JobId, EDreamTextureJobStatus::Failed, 0.0f, TEXT(""), TEXT("ComfyUI 报错")));
	TestFalse(TEXT("失败后不再进行"), Actions.IsRunActive());
	if (TestEqual(TEXT("失败通知一次"), Notices.Num(), 1))
	{
		TestTrue(TEXT("是错误通知"), NoticeErrors[0]);
		TestTrue(TEXT("说明点名「生成」这一步"), Notices[0].Contains(TEXT("Failed at the \"Generate\" step")));
	}
	TestTrue(TEXT("失败后可以重新开始"), Actions.CanRun());

	// ---- 再来一次：新的运行；成功通知一次，不重复 ----
	Fixture.World.JobId = TEXT("job_2");
	Actions.Run();
	TestEqual(TEXT("造了第二次运行"), Fixture.Runs.Num(), 2);
	Fixture.World.PushJob(MakeJob(TEXT("job_2"), EDreamTextureJobStatus::Succeeded, 1.0f));
	FDreamTextureImportStatus Done = MakeImportStatus(TEXT("mat_job_2"), EDreamTextureImportStage::Succeeded);
	Done.Result = MakeImportedResult();
	Fixture.World.PushImport(Done);
	TestTrue(TEXT("第二次成功"), Actions.GetRun()->GetState() == EState::Succeeded);
	if (TestEqual(TEXT("成功再通知一次（拉到 manifest 之后的再次广播不重复通知）"), Notices.Num(), 2))
	{
		TestFalse(TEXT("成功不是错误"), NoticeErrors[1]);
		TestTrue(TEXT("说明里有新实例"), Notices[1].Contains(TEXT("MI_Rock_ab12")));
	}
	TestEqual(TEXT("新建成功不重新解析原材质（它没被动）"), Fixture.Selection.AnalyzeCalls, 1);
	TestTrue(TEXT("预览用的 manifest 拉到了"), Actions.GetRun()->HasManifest());

	// ---- 取消 ----
	Fixture.World.JobId = TEXT("job_3");
	Actions.Run();
	Actions.CancelRun();
	TestEqual(TEXT("取消透传给运行：向后端取消了任务"), Fixture.World.CancelIds.Num(), 1);

	// ---- 换了材质：上一次运行不再挂在这个材质的页面上 ----
	UMaterialInstanceConstant* Other = NewObject<UMaterialInstanceConstant>(GetTransientPackage(), NAME_None, RF_Transient);
	Fixture.Select(Other, TEXT("/Game/Test/MI_Other.MI_Other"), BaseOnlyRows());
	TestFalse(TEXT("换了材质：这次运行不显示在新材质的页面上"), Actions.GetRun().IsValid());
	TestTrue(TEXT("但它自己还在跑（面板关了任务照常推进）"), Fixture.Runs.Last()->IsRunning());

	// ---- 开始不了时点开始：通知原因，不造运行 ----
	const int32 RunsBefore = Fixture.Runs.Num();
	Actions.SetMode(EDreamTextureReflowMode::Restyle);  // 没写提示词
	Actions.Run();
	TestEqual(TEXT("开始不了：不造运行"), Fixture.Runs.Num(), RunsBefore);
	if (TestTrue(TEXT("通知了原因"), Notices.Num() > 2))
	{
		TestTrue(TEXT("是错误通知，原因是提示词"), NoticeErrors.Last() && Notices.Last().Contains(TEXT("prompt")));
	}

	// ---- 面板先销毁、运行后结束：不崩 ----
	Fixture.Actions.Reset();
	Fixture.World.PushJob(MakeJob(TEXT("job_3"), EDreamTextureJobStatus::Canceled, 0.0f, TEXT(""), TEXT("已取消")));
	TestTrue(TEXT("页面没了，运行照常收尾"), Fixture.Runs[2]->GetState() == EState::Failed);
	return true;
}

// ---------------------------------------------------------------------------
// 原地覆盖成功后，表里的现状跟着刷新
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowActionsRefreshTest, "DreamTexture.Editor.Reflow.Actions.Refresh", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowActionsRefreshTest::RunTest(const FString& Parameters)
{
	FFixture Fixture;
	Fixture.SelectTypical(*this);
	Fixture.Actions->SetChannelChecked(TEXT("normal"), true);
	Fixture.Actions->SetInPlace(true);

	const int32 Before = Fixture.Selection.AnalyzeCalls;
	Fixture.Actions->Run();
	Fixture.World.PushJob(MakeJob(Fixture.World.JobId, EDreamTextureJobStatus::Succeeded, 1.0f));

	FDreamTextureImportStatus Done = MakeImportStatus(TEXT("mat_") + Fixture.World.JobId, EDreamTextureImportStage::Succeeded);
	Done.Result = MakeImportedResult();
	Done.Result.MaterialInstance = FDreamTextureImportedTexture();
	Done.Result.MaterialInstance.Action = EDreamTextureImportAction::Skipped;
	Fixture.World.PushImport(Done);

	TestTrue(FString::Printf(TEXT("原地覆盖成功（%s）"), *Fixture.Actions->GetRun()->GetMessage().ToString()), Fixture.Actions->GetRun()->GetState() == EState::Succeeded);
	TestEqual(TEXT("成功后重新解析了一次（实例被改了，表里的现状该刷新）"), Fixture.Selection.AnalyzeCalls, Before + 1);
	TestEqual(TEXT("同一个材质：运行仍挂在页面上"), Fixture.Actions->GetRun().IsValid(), true);
	return true;
}

// ---------------------------------------------------------------------------
// 面板：动作区挂进给它留的容器里
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowActionsPanelTest, "DreamTexture.Editor.Reflow.Actions.Panel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowActionsPanelTest::RunTest(const FString& Parameters)
{
	FFixture Fixture;
	Fixture.Select(Fixture.Instance, TEXT("/Game/Test/MI_Rock.MI_Rock"), TypicalRows());
	Fixture.Build(*this, /*bCompleteCatalog*/ false);

	{
		TSharedRef<SDreamTextureReflowPanel> Panel = SNew(SDreamTextureReflowPanel).Model(Fixture.Model).Actions(Fixture.Actions).bThumbnails(false).bAutoRefresh(false);
		TestTrue(TEXT("面板用的是注入的动作区模型"), Panel->GetActions() == Fixture.Actions);
		TestTrue(TEXT("给回流留的挂载点里放了动作区"), Panel->GetActionsHost()->GetChildren()->GetChildAt(0) != SNullWidget::NullWidget);

		Panel->BuildWidgetsForTest();  // 目录读取中

		Fixture.CompleteCatalog(Fixture.Catalog);
		Panel->BuildWidgetsForTest();  // 补通道、能开始
		{
			// 动作区自己限高并带滚动：控件再多也不把上面的结果表挤没。
			const FVector2f Desired = Panel->GetActionsHost()->GetDesiredSize();
			TestTrue(FString::Printf(TEXT("动作区有内容（期望高度 %.0f）"), Desired.Y), Desired.Y > 50.0f);
			TestTrue(FString::Printf(TEXT("动作区限高 380（期望高度 %.0f）"), Desired.Y), Desired.Y <= 380.0f + 1.0f);
		}

		Fixture.Actions->SetMode(EDreamTextureReflowMode::Restyle);
		Fixture.Actions->SetPrompt(TEXT("moss"));
		Fixture.Actions->SetInPlace(true);
		Fixture.Actions->SetChannelChecked(TEXT("height"), true);
		Panel->BuildWidgetsForTest();  // 改写外观、原地覆盖、勾了改不了的通道

		Fixture.Actions->SetInPlace(false);
		Fixture.Actions->Run();
		Panel->BuildWidgetsForTest();  // 运行中
		Fixture.World.PushJob(MakeJob(Fixture.World.JobId, EDreamTextureJobStatus::Running, 0.4f, TEXT("生成中")));
		Panel->BuildWidgetsForTest();  // 有进度

		Fixture.World.PushJob(MakeJob(Fixture.World.JobId, EDreamTextureJobStatus::Failed, 0.0f, TEXT(""), TEXT("ComfyUI 报错")));
		Panel->BuildWidgetsForTest();  // 失败

		Fixture.World.JobId = TEXT("job_2");
		Fixture.Actions->Run();
		Fixture.World.PushJob(MakeJob(TEXT("job_2"), EDreamTextureJobStatus::Succeeded, 1.0f));
		FDreamTextureImportStatus Done = MakeImportStatus(TEXT("mat_job_2"), EDreamTextureImportStage::Succeeded);
		Done.Result = MakeImportedResult();
		Fixture.World.PushImport(Done);
		Panel->BuildWidgetsForTest();  // 成功

		Fixture.Select(Fixture.Master, TEXT("/Game/Test/M_Rock.M_Rock"), TypicalRows(), false);
		Panel->BuildWidgetsForTest();  // 母材质：原地覆盖置灰
	}

	// 面板先销毁、模型后销毁：之后再广播不能崩。
	Fixture.Model->RefreshSelection();
	Fixture.Actions->SetMode(EDreamTextureReflowMode::FillChannels);
	TestTrue(TEXT("面板销毁之后广播没有崩溃"), true);

	// 默认不关动作区时面板自己建一个（连真实后端的默认实现）：构造不崩。
	{
		FFakeSelection DefaultSelection;
		TSharedRef<SDreamTextureReflowPanel> Default = SNew(SDreamTextureReflowPanel).Model(MakeShared<FDreamTextureReflowModel>(DefaultSelection.Make())).bThumbnails(false).bAutoRefresh(false);
		TestTrue(TEXT("默认建了动作区"), Default->GetActions().IsValid());
		Default->BuildWidgetsForTest();
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
