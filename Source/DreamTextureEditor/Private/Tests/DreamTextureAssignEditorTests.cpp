// Copyright TypeDreamMoon. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureAssignEditorFixture.h"
#include "DreamTextureAssignModel.h"
#include "SDreamTextureAssignDialog.h"

#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/AutomationTest.h"

/*
 * DreamTexture.Editor.Assign.* —— M3 的界面一侧：选中的读取、「将替换 N 个材质槽」的说法、
 * 槽位选择、赋值与撤销（经过模型）、按钮的可用探针、确认框控件能构造。
 *
 * 模态确认框本身不在自动化里弹（没人点它），所以流程拆成：模型（可直接断言）+ 控件（只验证能构造和取值不炸）。
 */

using namespace DreamTextureAssignEditorTest;

namespace
{
	bool Contains(const FText& Text, const TCHAR* Needle)
	{
		return Text.ToString().Contains(Needle);
	}
}

//////////////////////////////////////////////////////////////////////////
// 选中 → 模型

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAssignEditorModelTest, "DreamTexture.Editor.Assign.Model",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAssignEditorModelTest::RunTest(const FString& Parameters)
{
	FAssignFixture Fixture;
	if (!Fixture.Setup(*this, /*NumActors*/ 2))
	{
		return true;
	}

	// 什么都没选。
	Fixture.SelectActors({});
	{
		const FDreamTextureAssignSelection Empty = FDreamTextureAssignSelection::Query();
		TestTrue(TEXT("没选中：来源 None"), Empty.Source == EDreamTextureAssignSource::None);
		FDreamTextureAssignModel Model(Fixture.MatNew, Empty);
		FText Why;
		TestFalse(TEXT("没选中：不能赋"), Model.CanApply(&Why));
		TestFalse(TEXT("没选中：有原因"), Why.IsEmpty());
		TestTrue(TEXT("没选中：标题说没有可替换的槽"), Contains(Model.GetHeadline(), TEXT("No material slots")));
	}

	// 选中两个 3 槽 Actor。
	Fixture.SelectActors({ 0, 1 });
	const FDreamTextureAssignSelection Selection = FDreamTextureAssignSelection::Query();
	TestTrue(TEXT("选中 Actor：来源 Actors"), Selection.Source == EDreamTextureAssignSource::Actors);
	TestEqual(TEXT("2 个 Actor"), Selection.NumActors, 2);
	TestEqual(TEXT("2 个目标"), Selection.Targets.Num(), 2);

	const TSharedRef<FDreamTextureAssignModel> Model = MakeShared<FDreamTextureAssignModel>(Fixture.MatNew, Selection);
	TestEqual(TEXT("最大槽数 3（界面据此给槽位选择）"), Model->GetMaxSlots(), 3);
	TestTrue(TEXT("默认全部槽"), Model->IsAllSlots());
	TestEqual(TEXT("全部：将替换 6 个"), Model->GetReplaceCount(), 6);
	TestTrue(*Model->GetHeadline().ToString(), Contains(Model->GetHeadline(), TEXT("Will replace 6 material slots")));
	TestTrue(TEXT("来源说明提到 2 个 Actor"), Contains(Model->GetSourceText(), TEXT("2 Actors")));
	TestTrue(TEXT("Actor 来源没有资产警告"), Model->GetAssetWarningText().IsEmpty());
	TestTrue(TEXT("没有被锁的"), Model->GetLockedText().IsEmpty());
	TestTrue(TEXT("目标清单列了 Actor 名"), Contains(Model->GetTargetListText(), TEXT("DT_Assign_0")));
	TestEqual(TEXT("槽 1 两个目标都有"), Model->GetNumTargetsWithSlot(1), 2);
	TestEqual(TEXT("槽 5 没人有"), Model->GetNumTargetsWithSlot(5), 0);
	TestTrue(TEXT("槽名"), Model->GetSlotName(1).ToString() == TEXT("Slot1"));
	TestTrue(TEXT("能赋"), Model->CanApply());

	// 指定槽位：数量实时变；一个都不选就不能赋，并说明原因。
	Model->SetAllSlots(false);
	TestEqual(TEXT("指定槽位但没勾：0 个"), Model->GetReplaceCount(), 0);
	{
		FText Why;
		TestFalse(TEXT("指定槽位但没勾：不能赋"), Model->CanApply(&Why));
		TestTrue(TEXT("原因说要勾槽"), Contains(Why, TEXT("Select at least one")));
	}
	Model->SetSlotChosen(1, true);
	TestEqual(TEXT("勾槽 1：2 个"), Model->GetReplaceCount(), 2);
	TestTrue(TEXT("标题跟着变"), Contains(Model->GetHeadline(), TEXT("Will replace 2 material slots")));
	Model->SetSlotChosen(2, true);
	TestEqual(TEXT("再勾槽 2：4 个"), Model->GetReplaceCount(), 4);
	Model->SetSlotChosen(1, false);
	TestEqual(TEXT("取消槽 1：2 个"), Model->GetReplaceCount(), 2);
	TestTrue(TEXT("过滤器是 Only{2}"), !Model->GetFilter().bAll && Model->GetFilter().Indices == TArray<int32>({ 2 }));
	Model->SetAllSlots(true);
	TestEqual(TEXT("回到全部：6 个"), Model->GetReplaceCount(), 6);

	// 单槽目标：没有槽位选择，永远是「全部」。
	{
		AStaticMeshActor* Single = Fixture.SpawnWithMesh(Fixture.OneSlotMesh, TEXT("DT_Single"));
		if (TestNotNull(TEXT("单槽 Actor"), Single))
		{
			Fixture.SelectActors({});
			GEditor->SelectActor(Single, true, false);
			const FDreamTextureAssignSelection OneSel = FDreamTextureAssignSelection::Query();
			FDreamTextureAssignModel OneModel(Fixture.MatNew, OneSel);
			TestEqual(TEXT("单槽：最大槽数 1，不给槽位选择"), OneModel.GetMaxSlots(), 1);
			OneModel.SetAllSlots(false); // 即使被改成指定，也不该变成 0
			TestEqual(TEXT("单槽：仍是 1 个"), OneModel.GetReplaceCount(), 1);
		}
	}

	// 资产来源：说明与警告。
	{
		FDreamTextureAssignTarget AssetTarget;
		FDreamTextureMaterialAssign::MakeTarget(Fixture.Mesh, AssetTarget);
		FDreamTextureAssignModel AssetModel(Fixture.MatNew, FDreamTextureAssignSelection::FromTargets({ AssetTarget }));
		TestTrue(TEXT("资产来源"), AssetModel.GetSelection().Source == EDreamTextureAssignSource::Assets);
		TestFalse(TEXT("资产来源有警告：改的是默认材质"), AssetModel.GetAssetWarningText().IsEmpty());
		TestEqual(TEXT("资产 3 个槽"), AssetModel.GetReplaceCount(), 3);
	}

	// 引擎自带网格：列为被跳过，不计数。
	{
		FDreamTextureAssignTarget EngineTarget;
		FDreamTextureMaterialAssign::MakeTarget(Fixture.LoadEngineCube(), EngineTarget);
		FDreamTextureAssignModel EngineModel(Fixture.MatNew, FDreamTextureAssignSelection::FromTargets({ EngineTarget }));
		TestFalse(TEXT("只有引擎网格：不能赋"), EngineModel.CanApply());
		TestFalse(TEXT("说明了跳过的引擎网格"), EngineModel.GetLockedText().IsEmpty());
		TestEqual(TEXT("引擎网格不计数"), EngineModel.GetReplaceCount(), 0);
	}

	// 内容浏览器里残留了选中的网格：Actor 优先，并提示这次不动它们。
	{
		FDreamTextureAssignSelection WithIgnored = Selection;
		WithIgnored.NumIgnoredAssets = 2;
		FDreamTextureAssignModel IgnoredModel(Fixture.MatNew, WithIgnored);
		TestTrue(TEXT("提示内容浏览器里还有 2 个网格没动"), Contains(IgnoredModel.GetSourceText(), TEXT("2 static meshes selected")));
	}
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 经过模型赋值与撤销，及结果文案

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAssignEditorApplyTest, "DreamTexture.Editor.Assign.Apply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAssignEditorApplyTest::RunTest(const FString& Parameters)
{
	FAssignFixture Fixture;
	if (!Fixture.Setup(*this, /*NumActors*/ 2))
	{
		return true;
	}
	if (!TestTrue(TEXT("编辑器的撤销栈可用"), GEditor != nullptr && GEditor->Trans != nullptr))
	{
		return true;
	}

	Fixture.Comp(0)->SetMaterial(1, Fixture.MatA);
	const TArray<UMaterialInterface*> Before = Fixture.SnapshotMaterials();

	Fixture.SelectActors({ 0, 1 });
	const TSharedRef<FDreamTextureAssignModel> Model = MakeShared<FDreamTextureAssignModel>(Fixture.MatNew, FDreamTextureAssignSelection::Query());

	// 勾槽 0 和 2，赋值：槽 1 不动。
	Model->SetAllSlots(false);
	Model->SetSlotChosen(0, true);
	Model->SetSlotChosen(2, true);
	const int32 QueueBefore = GEditor->Trans->GetQueueLength();
	const FDreamTextureAssignResult Result = Model->Apply();
	TestTrue(TEXT("赋值成功"), Result.bOK);
	TestEqual(TEXT("与标题说的一致：改了 4 个槽"), Result.TotalChanged(), Model->GetReplaceCount());
	TestEqual(TEXT("撤销栈只多一条"), GEditor->Trans->GetQueueLength(), QueueBefore + 1);
	TestTrue(TEXT("Actor0 槽 0 变了"), Fixture.Comp(0)->GetMaterial(0) == Fixture.MatNew);
	TestTrue(TEXT("Actor0 槽 1 没动（仍是 MatA）"), Fixture.Comp(0)->GetMaterial(1) == Fixture.MatA);
	TestTrue(TEXT("Actor1 槽 1 没动（空）"), Fixture.Comp(1)->GetMaterial(1) == nullptr);
	TestTrue(TEXT("Actor1 槽 2 变了"), Fixture.Comp(1)->GetMaterial(2) == Fixture.MatNew);

	const FText Text = FDreamTextureAssignModel::DescribeResult(Result, Model->GetMaterialName());
	TestTrue(*Text.ToString(), Contains(Text, TEXT("4 material slots")) && Contains(Text, TEXT("Ctrl+Z")));
	TestTrue(TEXT("结果里逐个说了目标和槽"), Contains(Text, TEXT("DT_Assign_0")) && Contains(Text, TEXT("Slot 0")));

	// 一次撤销，全部回来。
	TestTrue(TEXT("Undo"), GEditor->UndoTransaction());
	TestTrue(TEXT("撤销后与赋值前逐槽一致"), Fixture.SnapshotMaterials() == Before);

	// 再赋一次全部，然后同样的选择重复赋 → 「本来就是它」。
	Model->SetAllSlots(true);
	Model->Apply();
	const FDreamTextureAssignResult Again = Model->Apply();
	const FText AgainText = FDreamTextureAssignModel::DescribeResult(Again, Model->GetMaterialName());
	TestTrue(*AgainText.ToString(), Contains(AgainText, TEXT("already use")));

	// 失败说法。
	FDreamTextureAssignResult Failed;
	Failed.Error = TEXT("test error");
	TestTrue(TEXT("失败文案带原因"), Contains(FDreamTextureAssignModel::DescribeResult(Failed, FText::FromString(TEXT("X"))), TEXT("test error")));

	// 材质没了：不能赋，不崩。
	FDreamTextureAssignModel Orphan(nullptr, FDreamTextureAssignSelection::Query());
	FText Why;
	TestFalse(TEXT("没有材质：不能赋"), Orphan.CanApply(&Why));
	TestFalse(TEXT("没有材质：有原因"), Why.IsEmpty());
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 按钮的可用探针

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAssignEditorProbeTest, "DreamTexture.Editor.Assign.Probe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAssignEditorProbeTest::RunTest(const FString& Parameters)
{
	FAssignFixture Fixture;
	if (!Fixture.Setup(*this, /*NumActors*/ 1))
	{
		return true;
	}

	const FText NoMaterial = FText::FromString(TEXT("No material instance"));

	// 没有材质：不亮，说的是材质的事（而不是「没选中」）。
	Fixture.SelectActors({ 0 });
	FDreamTextureAssignProbe::Invalidate();
	{
		const FDreamTextureAssignProbe& Probe = FDreamTextureAssignProbe::Get(false, NoMaterial);
		TestFalse(TEXT("没有材质：不亮"), Probe.bCanAssign);
		TestTrue(TEXT("没有材质：提示是材质的原因"), Probe.Hint.ToString() == NoMaterial.ToString());
	}

	// 有材质、没选中：不亮，并说明去选什么。
	Fixture.SelectActors({});
	FDreamTextureAssignProbe::Invalidate();
	{
		const FDreamTextureAssignProbe& Probe = FDreamTextureAssignProbe::Get(true, NoMaterial);
		TestFalse(TEXT("没选中：不亮"), Probe.bCanAssign);
		TestTrue(*Probe.Hint.ToString(), Contains(Probe.Hint, TEXT("Nothing is selected")));
	}

	// 选中带网格的 Actor：亮。
	Fixture.SelectActors({ 0 });
	FDreamTextureAssignProbe::Invalidate();
	{
		const FDreamTextureAssignProbe& Probe = FDreamTextureAssignProbe::Get(true, NoMaterial);
		TestTrue(TEXT("选中 Actor：亮"), Probe.bCanAssign);
		TestTrue(TEXT("提示说了可以撤销"), Contains(Probe.Hint, TEXT("Ctrl+Z")));
	}

	// 缓存：同一个 0.25 秒窗口里改了选中，结果不变（这正是缓存要的）；Invalidate 后才跟上。
	Fixture.SelectActors({});
	TestTrue(TEXT("缓存窗口内仍是上一次的结论"), FDreamTextureAssignProbe::Get(true, NoMaterial).bCanAssign);
	FDreamTextureAssignProbe::Invalidate();
	TestFalse(TEXT("丢缓存后跟上"), FDreamTextureAssignProbe::Get(true, NoMaterial).bCanAssign);

	FDreamTextureAssignProbe::Invalidate();
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 确认框控件

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAssignEditorDialogTest, "DreamTexture.Editor.Assign.Dialog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAssignEditorDialogTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("已跳过：Slate 没初始化，没法构造控件。"));
		return true;
	}

	FAssignFixture Fixture;
	if (!Fixture.Setup(*this, /*NumActors*/ 2))
	{
		return true;
	}

	Fixture.SelectActors({ 0, 1 });
	const TSharedRef<FDreamTextureAssignModel> Model = MakeShared<FDreamTextureAssignModel>(Fixture.MatNew, FDreamTextureAssignSelection::Query());

	int32 Confirmed = 0;
	int32 Cancelled = 0;
	TSharedPtr<SDreamTextureAssignDialog> Dialog = SNew(SDreamTextureAssignDialog)
		.Model(Model)
		.OnConfirm(FSimpleDelegate::CreateLambda([&Confirmed]() { ++Confirmed; }))
		.OnCancel(FSimpleDelegate::CreateLambda([&Cancelled]() { ++Cancelled; }));
	TestTrue(TEXT("3 槽 Actor：确认框构造成功"), Dialog.IsValid());

	// 让 Slate 把属性取一遍（可见性、文案、可用性都是 lambda，构造时不求值）。
	Dialog->SlatePrepass(1.0f);
	Model->SetAllSlots(false);
	Dialog->SlatePrepass(1.0f);
	Model->SetSlotChosen(0, true);
	Dialog->SlatePrepass(1.0f);
	Dialog.Reset();

	// 材质在对话框开着时被删：控件不能崩。
	Model->SetAllSlots(true);
	TSharedPtr<SDreamTextureAssignDialog> Dialog2 = SNew(SDreamTextureAssignDialog).Model(Model);
	Dialog2->SlatePrepass(1.0f);
	Dialog2.Reset();

	TestEqual(TEXT("没人点按钮：回调没触发"), Confirmed + Cancelled, 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
