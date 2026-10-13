// Copyright TypeDreamMoon. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureAssignTestUtils.h"
#include "DreamTextureMaterialAssign.h"

#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/PackageName.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"

/*
 * DreamTexture.Assets.Assign.* —— M3：把材质赋给选中的物体。
 *
 * 在编辑器世界里生成几个带多槽 StaticMeshComponent 的 Actor（网格是引擎自带 Cube 的临时副本，
 * 外加几个空槽凑成 3 槽），赋值之后逐槽检查，并用 GEditor 的撤销验证一次 Ctrl+Z 能完整回滚。
 * 材质用临时包里新建的 UMaterial，彼此可区分、不依赖任何磁盘资产。
 * 每个测试结束都销毁生成的 Actor、清空撤销栈。
 */

using namespace DreamTextureAssignTest;

//////////////////////////////////////////////////////////////////////////
// 收集目标

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAssignCollectTest, "DreamTexture.Assets.Assign.Collect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAssignCollectTest::RunTest(const FString& Parameters)
{
	FAssignFixture Fixture;
	if (!Fixture.Setup(*this, /*NumActors*/ 2))
	{
		return true;
	}

	Fixture.SelectActors({ 0, 1 });

	TArray<FDreamTextureAssignTarget> Targets;
	FDreamTextureMaterialAssign::CollectFromSelectedActors(Targets);
	TestEqual(TEXT("两个选中的 Actor 各一个网格组件"), Targets.Num(), 2);
	for (const FDreamTextureAssignTarget& Target : Targets)
	{
		TestTrue(TEXT("目标是组件"), Target.Kind == EDreamTextureAssignKind::Component);
		TestEqual(TEXT("每个组件 3 个槽"), Target.NumSlots(), 3);
		TestFalse(TEXT("组件目标不锁"), Target.bLocked);
		TestFalse(TEXT("有名字"), Target.Label.IsEmpty());
	}
	TestEqual(TEXT("全部槽数 = 6"), FDreamTextureMaterialAssign::CountSlots(Targets, FDreamTextureSlotFilter::All()), 6);
	TestEqual(TEXT("只槽 1 = 2"), FDreamTextureMaterialAssign::CountSlots(Targets, FDreamTextureSlotFilter::Only({ 1 })), 2);
	TestEqual(TEXT("槽 0 和 2 = 4"), FDreamTextureMaterialAssign::CountSlots(Targets, FDreamTextureSlotFilter::Only({ 0, 2 })), 4);
	TestEqual(TEXT("越界的槽号不算"), FDreamTextureMaterialAssign::CountSlots(Targets, FDreamTextureSlotFilter::Only({ 7 })), 0);
	TestEqual(TEXT("最大槽数"), FDreamTextureMaterialAssign::MaxSlotCount(Targets), 3);
	TestTrue(TEXT("槽名按槽号取"), FDreamTextureMaterialAssign::SlotNameAt(Targets, 2) == FName(TEXT("Slot2")));

	// 没选中就没有目标。
	Fixture.SelectActors({});
	Targets.Reset();
	FDreamTextureMaterialAssign::CollectFromSelectedActors(Targets);
	TestEqual(TEXT("没选中 = 没目标"), Targets.Num(), 0);

	// CollectFromObjects：Actor / 组件 / 资产都认，别的东西忽略。
	TArray<UObject*> Objects;
	Objects.Add(Fixture.Actors[0]);
	Objects.Add(Fixture.Actors[1]->GetStaticMeshComponent());
	Objects.Add(Fixture.Mesh);
	Objects.Add(Fixture.MatA);
	Objects.Add(nullptr);
	Targets.Reset();
	FDreamTextureMaterialAssign::CollectFromObjects(Objects, Targets);
	TestEqual(TEXT("Actor + 组件 + 资产 = 3 个目标，材质和空指针被忽略"), Targets.Num(), 3);
	int32 NumAssets = 0;
	for (const FDreamTextureAssignTarget& Target : Targets)
	{
		NumAssets += Target.IsAsset() ? 1 : 0;
	}
	TestEqual(TEXT("其中一个是资产"), NumAssets, 1);

	// 引擎自带的网格：列出来，但锁住。
	Targets.Reset();
	FDreamTextureAssignTarget EngineTarget;
	UObject* EngineCube = Fixture.LoadEngineCube();
	if (TestNotNull(TEXT("引擎自带 Cube"), EngineCube) && TestTrue(TEXT("为引擎网格生成目标"), FDreamTextureMaterialAssign::MakeTarget(EngineCube, EngineTarget)))
	{
		TestTrue(TEXT("引擎自带的资产被锁"), EngineTarget.bLocked);
		TestFalse(TEXT("锁住的有原因"), EngineTarget.LockedReason.IsEmpty());
		Targets.Add(EngineTarget);
		TestEqual(TEXT("锁住的不计入将替换的槽数"), FDreamTextureMaterialAssign::CountSlots(Targets, FDreamTextureSlotFilter::All()), 0);
		TestEqual(TEXT("锁住的不计入最大槽数"), FDreamTextureMaterialAssign::MaxSlotCount(Targets), 0);
	}
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 组件：全部槽，多 Actor

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAssignComponentTest, "DreamTexture.Assets.Assign.Component",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAssignComponentTest::RunTest(const FString& Parameters)
{
	FAssignFixture Fixture;
	if (!Fixture.Setup(*this, /*NumActors*/ 2))
	{
		return true;
	}

	// 先给两个 Actor 的某些槽设上覆盖，赋值之后「改前」要如实记下来。
	Fixture.Comp(0)->SetMaterial(1, Fixture.MatA);
	Fixture.Comp(1)->SetMaterial(2, Fixture.MatB);

	Fixture.SelectActors({ 0, 1 });
	TArray<FDreamTextureAssignTarget> Targets;
	FDreamTextureMaterialAssign::CollectFromSelectedActors(Targets);

	const FDreamTextureAssignResult Result = FDreamTextureMaterialAssign::Assign(Fixture.MatNew, Targets, FDreamTextureSlotFilter::All());
	TestTrue(*FString::Printf(TEXT("赋值成功（%s）"), *Result.Error), Result.bOK);
	TestTrue(TEXT("写进了撤销栈"), Result.bTransacted);
	TestEqual(TEXT("改了两个组件"), Result.Changes.Num(), 2);
	TestEqual(TEXT("共 6 个槽"), Result.TotalChanged(), 6);

	for (int32 ActorIndex = 0; ActorIndex < 2; ++ActorIndex)
	{
		for (int32 Slot = 0; Slot < 3; ++Slot)
		{
			TestTrue(*FString::Printf(TEXT("Actor%d 槽%d 是新材质"), ActorIndex, Slot), Fixture.Comp(ActorIndex)->GetMaterial(Slot) == Fixture.MatNew);
		}
	}

	// 「改前」记录：Actor0 的槽 1 原来是 MatA，Actor1 的槽 2 原来是 MatB，其余原来是空。
	auto FindPrevious = [&Result](UObject* Component, int32 Slot) -> UMaterialInterface*
	{
		for (const FDreamTextureAssignChange& Change : Result.Changes)
		{
			if (Change.Object.Get() == Component)
			{
				for (const FDreamTextureSlotChange& SlotChange : Change.Slots)
				{
					if (SlotChange.SlotIndex == Slot)
					{
						return SlotChange.Previous.Get();
					}
				}
			}
		}
		return reinterpret_cast<UMaterialInterface*>(static_cast<uintptr_t>(1));
	};
	TestTrue(TEXT("记着 Actor0 槽 1 改前是 MatA"), FindPrevious(Fixture.Comp(0), 1) == Fixture.MatA);
	TestTrue(TEXT("记着 Actor1 槽 2 改前是 MatB"), FindPrevious(Fixture.Comp(1), 2) == Fixture.MatB);
	TestTrue(TEXT("记着 Actor0 槽 0 改前是空"), FindPrevious(Fixture.Comp(0), 0) == nullptr);

	// 槽名也记下来了，界面据此说「改了 Slot1」。
	bool bNamed = false;
	for (const FDreamTextureAssignChange& Change : Result.Changes)
	{
		for (const FDreamTextureSlotChange& SlotChange : Change.Slots)
		{
			bNamed |= SlotChange.SlotIndex == 1 && SlotChange.SlotName == FName(TEXT("Slot1"));
		}
	}
	TestTrue(TEXT("槽名随结果带回"), bNamed);
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 撤销：多槽、多 Actor 一次 Ctrl+Z 全部回滚

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAssignUndoTest, "DreamTexture.Assets.Assign.Undo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAssignUndoTest::RunTest(const FString& Parameters)
{
	FAssignFixture Fixture;
	if (!Fixture.Setup(*this, /*NumActors*/ 3))
	{
		return true;
	}
	if (!TestTrue(TEXT("编辑器的撤销栈可用"), GEditor != nullptr && GEditor->Trans != nullptr))
	{
		return true;
	}

	Fixture.Comp(0)->SetMaterial(0, Fixture.MatA);
	Fixture.Comp(2)->SetMaterial(1, Fixture.MatB);
	const TArray<UMaterialInterface*> Before = Fixture.SnapshotMaterials();

	Fixture.SelectActors({ 0, 1, 2 });
	TArray<FDreamTextureAssignTarget> Targets;
	FDreamTextureMaterialAssign::CollectFromSelectedActors(Targets);

	const int32 QueueBefore = GEditor->Trans->GetQueueLength();
	const FDreamTextureAssignResult Result = FDreamTextureMaterialAssign::Assign(Fixture.MatNew, Targets, FDreamTextureSlotFilter::All(),
		FText::FromString(TEXT("测试赋值")));
	TestEqual(TEXT("3 个 Actor 共 9 个槽"), Result.TotalChanged(), 9);
	TestEqual(TEXT("撤销栈只多了一条（整个赋值是一个事务）"), GEditor->Trans->GetQueueLength(), QueueBefore + 1);

	const TArray<UMaterialInterface*> After = Fixture.SnapshotMaterials();
	for (UMaterialInterface* Material : After)
	{
		TestTrue(TEXT("赋值后每个槽都是新材质"), Material == Fixture.MatNew);
	}

	// 一次撤销，所有 Actor 所有槽都回到赋值前。
	TestTrue(TEXT("UndoTransaction 成功"), GEditor->UndoTransaction());
	const TArray<UMaterialInterface*> Undone = Fixture.SnapshotMaterials();
	TestTrue(TEXT("撤销后与赋值前逐槽一致"), Undone == Before);
	TestEqual(TEXT("撤销栈回到赋值前的长度（那条在重做栈里）"), GEditor->Trans->GetQueueLength(), QueueBefore + 1);

	// 重做：全部回来。
	TestTrue(TEXT("RedoTransaction 成功"), GEditor->RedoTransaction());
	TestTrue(TEXT("重做后与赋值后逐槽一致"), Fixture.SnapshotMaterials() == After);

	// 再撤一次，确认可以反复。
	TestTrue(TEXT("再次 Undo"), GEditor->UndoTransaction());
	TestTrue(TEXT("再次撤销后仍与赋值前一致"), Fixture.SnapshotMaterials() == Before);
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 只赋指定槽，其他槽不动

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAssignSlotsTest, "DreamTexture.Assets.Assign.Slots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAssignSlotsTest::RunTest(const FString& Parameters)
{
	FAssignFixture Fixture;
	if (!Fixture.Setup(*this, /*NumActors*/ 2))
	{
		return true;
	}

	Fixture.Comp(0)->SetMaterial(0, Fixture.MatA);
	Fixture.Comp(0)->SetMaterial(2, Fixture.MatB);
	Fixture.Comp(1)->SetMaterial(1, Fixture.MatA);

	Fixture.SelectActors({ 0, 1 });
	TArray<FDreamTextureAssignTarget> Targets;
	FDreamTextureMaterialAssign::CollectFromSelectedActors(Targets);

	// 只赋槽 1：两个 Actor 的槽 0 / 2 都不动。
	FDreamTextureAssignResult Result = FDreamTextureMaterialAssign::Assign(Fixture.MatNew, Targets, FDreamTextureSlotFilter::Only({ 1 }));
	TestTrue(TEXT("只槽 1：成功"), Result.bOK);
	TestEqual(TEXT("只槽 1：改了 2 个槽"), Result.TotalChanged(), 2);
	TestTrue(TEXT("Actor0 槽 0 不动"), Fixture.Comp(0)->GetMaterial(0) == Fixture.MatA);
	TestTrue(TEXT("Actor0 槽 1 变了"), Fixture.Comp(0)->GetMaterial(1) == Fixture.MatNew);
	TestTrue(TEXT("Actor0 槽 2 不动"), Fixture.Comp(0)->GetMaterial(2) == Fixture.MatB);
	TestTrue(TEXT("Actor1 槽 0 不动（空）"), Fixture.Comp(1)->GetMaterial(0) == nullptr);
	TestTrue(TEXT("Actor1 槽 1 变了"), Fixture.Comp(1)->GetMaterial(1) == Fixture.MatNew);
	TestTrue(TEXT("Actor1 槽 2 不动（空）"), Fixture.Comp(1)->GetMaterial(2) == nullptr);

	// 再赋槽 0 和 2：槽 1 保持上一步的结果。
	Result = FDreamTextureMaterialAssign::Assign(Fixture.MatB, Targets, FDreamTextureSlotFilter::Only({ 0, 2 }));
	TestEqual(TEXT("槽 0 和 2：4 个槽里 Actor0 的槽 2 本来就是 MatB 所以不算，实际改 3 个"), Result.TotalChanged(), 3);
	TestTrue(TEXT("Actor0 槽 0 变成 MatB"), Fixture.Comp(0)->GetMaterial(0) == Fixture.MatB);
	TestTrue(TEXT("Actor0 槽 1 保持"), Fixture.Comp(0)->GetMaterial(1) == Fixture.MatNew);
	TestTrue(TEXT("Actor1 槽 1 保持"), Fixture.Comp(1)->GetMaterial(1) == Fixture.MatNew);
	TestTrue(TEXT("Actor1 槽 2 变成 MatB"), Fixture.Comp(1)->GetMaterial(2) == Fixture.MatB);

	// 槽数不同的两个目标：槽号对不上的那个不受影响。
	AStaticMeshActor* Single = Fixture.SpawnWithMesh(Fixture.OneSlotMesh, TEXT("OneSlot"));
	if (TestNotNull(TEXT("单槽 Actor"), Single))
	{
		Fixture.SelectActors({ 0 });
		GEditor->SelectActor(Single, true, false);
		Targets.Reset();
		FDreamTextureMaterialAssign::CollectFromSelectedActors(Targets);
		TestEqual(TEXT("两个目标"), Targets.Num(), 2);
		TestEqual(TEXT("槽 2 只对 3 槽的那个有效"), FDreamTextureMaterialAssign::CountSlots(Targets, FDreamTextureSlotFilter::Only({ 2 })), 1);

		const TObjectPtr<UMaterialInterface> SingleBefore = Single->GetStaticMeshComponent()->GetMaterial(0);
		Result = FDreamTextureMaterialAssign::Assign(Fixture.MatA, Targets, FDreamTextureSlotFilter::Only({ 2 }));
		TestEqual(TEXT("槽 2：只改了 1 个槽"), Result.TotalChanged(), 1);
		TestTrue(TEXT("单槽 Actor 没被碰"), Single->GetStaticMeshComponent()->GetMaterial(0) == SingleBefore);
		TestTrue(TEXT("3 槽 Actor 的槽 2 变了"), Fixture.Comp(0)->GetMaterial(2) == Fixture.MatA);
	}

	// 参数错误：不是静默成功。
	Result = FDreamTextureMaterialAssign::Assign(nullptr, Targets, FDreamTextureSlotFilter::All());
	TestFalse(TEXT("空材质：失败"), Result.bOK);
	TestFalse(TEXT("空材质：有中文原因"), Result.Error.IsEmpty());
	Result = FDreamTextureMaterialAssign::Assign(Fixture.MatA, Targets, FDreamTextureSlotFilter::Only({}));
	TestFalse(TEXT("没选任何槽：失败"), Result.bOK);
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 什么都没改：不留空撤销记录

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAssignNoChangeTest, "DreamTexture.Assets.Assign.NoChange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAssignNoChangeTest::RunTest(const FString& Parameters)
{
	FAssignFixture Fixture;
	if (!Fixture.Setup(*this, /*NumActors*/ 1))
	{
		return true;
	}
	if (!TestTrue(TEXT("编辑器的撤销栈可用"), GEditor != nullptr && GEditor->Trans != nullptr))
	{
		return true;
	}

	Fixture.SelectActors({ 0 });
	TArray<FDreamTextureAssignTarget> Targets;
	FDreamTextureMaterialAssign::CollectFromSelectedActors(Targets);

	FDreamTextureMaterialAssign::Assign(Fixture.MatNew, Targets, FDreamTextureSlotFilter::All());

	const int32 QueueBefore = GEditor->Trans->GetQueueLength();
	const FDreamTextureAssignResult Again = FDreamTextureMaterialAssign::Assign(Fixture.MatNew, Targets, FDreamTextureSlotFilter::All());
	TestTrue(TEXT("重复赋值：成功"), Again.bOK);
	TestEqual(TEXT("重复赋值：一个槽都没改"), Again.TotalChanged(), 0);
	TestFalse(TEXT("重复赋值：没有撤销记录"), Again.bTransacted);
	TestEqual(TEXT("撤销栈长度不变（事务被取消）"), GEditor->Trans->GetQueueLength(), QueueBefore);

	// 没有目标同理。
	const FDreamTextureAssignResult Empty = FDreamTextureMaterialAssign::Assign(Fixture.MatA, TArray<FDreamTextureAssignTarget>(), FDreamTextureSlotFilter::All());
	TestTrue(TEXT("没有目标：成功但什么都没改"), Empty.bOK && Empty.TotalChanged() == 0);
	TestEqual(TEXT("没有目标：撤销栈长度不变"), GEditor->Trans->GetQueueLength(), QueueBefore);
	return true;
}

//////////////////////////////////////////////////////////////////////////
// StaticMesh 资产的默认材质槽

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAssignAssetTest, "DreamTexture.Assets.Assign.Asset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAssignAssetTest::RunTest(const FString& Parameters)
{
	FAssignFixture Fixture;
	if (!Fixture.Setup(*this, /*NumActors*/ 1))
	{
		return true;
	}
	if (!TestTrue(TEXT("编辑器的撤销栈可用"), GEditor != nullptr && GEditor->Trans != nullptr))
	{
		return true;
	}

	UStaticMesh* Mesh = Fixture.Mesh;
	// 初始状态直接写数组：避免在准备阶段就触发一次网格重建，被测的重建只发生在 Assign 里。
	Mesh->GetStaticMaterials()[1].MaterialInterface = Fixture.MatA;
	const TArray<UMaterialInterface*> Before = Fixture.SnapshotMeshMaterials();

	TArray<FDreamTextureAssignTarget> Targets;
	FDreamTextureAssignTarget Target;
	if (!TestTrue(TEXT("为资产生成目标"), FDreamTextureMaterialAssign::MakeTarget(Mesh, Target)))
	{
		return true;
	}
	TestTrue(TEXT("目标是资产"), Target.IsAsset());
	TestFalse(TEXT("临时包里的副本不算引擎内容"), Target.bLocked);
	TestEqual(TEXT("3 个槽"), Target.NumSlots(), 3);
	Targets.Add(Target);

	// 只改槽 2：槽 0 / 1 不动。
	const int32 QueueBefore = GEditor->Trans->GetQueueLength();
	FDreamTextureAssignResult Result = FDreamTextureMaterialAssign::Assign(Fixture.MatNew, Targets, FDreamTextureSlotFilter::Only({ 2 }));
	TestTrue(*FString::Printf(TEXT("只槽 2：成功（%s）"), *Result.Error), Result.bOK);
	TestEqual(TEXT("只槽 2：改了 1 个槽"), Result.TotalChanged(), 1);
	TestTrue(TEXT("槽 0 不动"), Mesh->GetStaticMaterials()[0].MaterialInterface == Before[0]);
	TestTrue(TEXT("槽 1 不动"), Mesh->GetStaticMaterials()[1].MaterialInterface == Fixture.MatA);
	TestTrue(TEXT("槽 2 变了"), Mesh->GetStaticMaterials()[2].MaterialInterface == Fixture.MatNew);
	TestEqual(TEXT("撤销栈只多一条（UStaticMesh::SetMaterial 自带的事务并进了我们的）"), GEditor->Trans->GetQueueLength(), QueueBefore + 1);

	TestTrue(TEXT("撤销"), GEditor->UndoTransaction());
	TestTrue(TEXT("撤销后资产默认槽回到赋值前"), Fixture.SnapshotMeshMaterials() == Before);
	TestTrue(TEXT("重做"), GEditor->RedoTransaction());
	TestTrue(TEXT("重做后槽 2 又是新材质"), Mesh->GetStaticMaterials()[2].MaterialInterface == Fixture.MatNew);
	TestTrue(TEXT("再撤销"), GEditor->UndoTransaction());

	// 全部槽。
	Result = FDreamTextureMaterialAssign::Assign(Fixture.MatNew, Targets, FDreamTextureSlotFilter::All());
	TestEqual(TEXT("全部槽：改了 3 个"), Result.TotalChanged(), 3);
	for (UMaterialInterface* Material : Fixture.SnapshotMeshMaterials())
	{
		TestTrue(TEXT("资产的每个槽都是新材质"), Material == Fixture.MatNew);
	}
	TestTrue(TEXT("撤销全部"), GEditor->UndoTransaction());
	TestTrue(TEXT("撤销后与赋值前一致"), Fixture.SnapshotMeshMaterials() == Before);

	// 引擎自带的网格：被跳过，一个字都不改。
	UStaticMesh* EngineCube = Fixture.LoadEngineCube();
	if (TestNotNull(TEXT("引擎自带 Cube"), EngineCube))
	{
		const UMaterialInterface* CubeSlot0 = EngineCube->GetStaticMaterials()[0].MaterialInterface;
		FDreamTextureAssignTarget EngineTarget;
		TestTrue(TEXT("引擎网格生成目标"), FDreamTextureMaterialAssign::MakeTarget(EngineCube, EngineTarget));
		TArray<FDreamTextureAssignTarget> EngineTargets;
		EngineTargets.Add(EngineTarget);
		Result = FDreamTextureMaterialAssign::Assign(Fixture.MatNew, EngineTargets, FDreamTextureSlotFilter::All());
		TestEqual(TEXT("引擎网格：一个槽都没改"), Result.TotalChanged(), 0);
		TestEqual(TEXT("引擎网格：列入跳过"), Result.SkippedLabels.Num(), 1);
		TestTrue(TEXT("引擎网格的槽原样"), EngineCube->GetStaticMaterials()[0].MaterialInterface == CubeSlot0);
	}
	return true;
}

//////////////////////////////////////////////////////////////////////////
// Actor 和资产一起：一个事务

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAssignMixedTest, "DreamTexture.Assets.Assign.Mixed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAssignMixedTest::RunTest(const FString& Parameters)
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

	// 一个独立的网格副本给资产目标用，免得和 Actor 用的那个互相影响结果。
	UStaticMesh* AssetMesh = Fixture.MakeThreeSlotMesh();
	if (!TestNotNull(TEXT("资产网格"), AssetMesh))
	{
		return true;
	}

	Fixture.SelectActors({ 0, 1 });
	TArray<FDreamTextureAssignTarget> Targets;
	FDreamTextureMaterialAssign::CollectFromSelectedActors(Targets);
	FDreamTextureAssignTarget AssetTarget;
	FDreamTextureMaterialAssign::MakeTarget(AssetMesh, AssetTarget);
	Targets.Add(AssetTarget);

	const TArray<UMaterialInterface*> ActorsBefore = Fixture.SnapshotMaterials();
	TArray<UMaterialInterface*> AssetBefore;
	for (const FStaticMaterial& Material : AssetMesh->GetStaticMaterials())
	{
		AssetBefore.Add(Material.MaterialInterface);
	}

	const int32 QueueBefore = GEditor->Trans->GetQueueLength();
	const FDreamTextureAssignResult Result = FDreamTextureMaterialAssign::Assign(Fixture.MatNew, Targets, FDreamTextureSlotFilter::All());
	TestEqual(TEXT("2 个 Actor + 1 个资产，共 9 个槽"), Result.TotalChanged(), 9);
	TestEqual(TEXT("撤销栈只多一条"), GEditor->Trans->GetQueueLength(), QueueBefore + 1);

	TestTrue(TEXT("一次撤销"), GEditor->UndoTransaction());
	TestTrue(TEXT("Actor 们回到赋值前"), Fixture.SnapshotMaterials() == ActorsBefore);
	TArray<UMaterialInterface*> AssetAfterUndo;
	for (const FStaticMaterial& Material : AssetMesh->GetStaticMaterials())
	{
		AssetAfterUndo.Add(Material.MaterialInterface);
	}
	TestTrue(TEXT("资产也回到赋值前"), AssetAfterUndo == AssetBefore);
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 引擎自带内容：按磁盘路径判断，引擎插件的挂载点（/Water/ 之类）也锁

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAssignEngineContentTest, "DreamTexture.Assets.Assign.EngineContent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAssignEngineContentTest::RunTest(const FString& Parameters)
{
	FAssignFixture Fixture;
	if (!Fixture.Setup(*this, /*NumActors*/ 1))
	{
		return true;
	}

	// ---- 找一个随引擎发布、挂载点不是 /Engine/ 的静态网格：遍历已启用的引擎插件 ----
	IAssetRegistry& Registry = IAssetRegistry::GetChecked();
	UStaticMesh* PluginMesh = nullptr;
	FString PluginMount;
	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPluginsWithContent())
	{
		if (Plugin->GetLoadedFrom() != EPluginLoadedFrom::Engine)
		{
			continue;
		}
		const FString Mount = Plugin->GetMountedAssetPath();
		if (Mount.Equals(TEXT("/Engine/"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(*Mount.LeftChop(1)), Assets, /*bRecursive*/ true);
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetClassPath == UStaticMesh::StaticClass()->GetClassPathName())
			{
				PluginMesh = Cast<UStaticMesh>(Asset.GetAsset());
				if (PluginMesh != nullptr && PluginMesh->GetStaticMaterials().Num() > 0)
				{
					PluginMount = Mount;
					break;
				}
				PluginMesh = nullptr;
			}
		}
		if (PluginMesh != nullptr)
		{
			break;
		}
	}

	if (PluginMesh == nullptr)
	{
		AddInfo(TEXT("已跳过引擎插件那一半：已启用的引擎插件里找不到带内容的静态网格。"));
	}
	else
	{
		AddInfo(FString::Printf(TEXT("用引擎插件里的网格 %s（挂载点 %s）验证"), *PluginMesh->GetPathName(), *PluginMount));
		TestFalse(TEXT("准备工作：包名不以 /Engine/ 打头——旧写法认不出它"), PluginMesh->GetOutermost()->GetName().StartsWith(TEXT("/Engine/")));

		FDreamTextureAssignTarget Target;
		if (TestTrue(TEXT("为引擎插件的网格生成目标"), FDreamTextureMaterialAssign::MakeTarget(PluginMesh, Target)))
		{
			TestTrue(TEXT("引擎插件的网格被锁定"), Target.bLocked);
			TestFalse(TEXT("锁定时写了原因"), Target.LockedReason.IsEmpty());

			// 真赋一次：一个槽都不改，列入跳过，网格原样。
			const UMaterialInterface* SlotBefore = PluginMesh->GetStaticMaterials()[0].MaterialInterface;
			TArray<FDreamTextureAssignTarget> Targets;
			Targets.Add(Target);
			const FDreamTextureAssignResult Result = FDreamTextureMaterialAssign::Assign(Fixture.MatNew, Targets, FDreamTextureSlotFilter::All());
			TestEqual(TEXT("引擎插件的网格：一个槽都没改"), Result.TotalChanged(), 0);
			TestEqual(TEXT("引擎插件的网格：列入跳过"), Result.SkippedLabels.Num(), 1);
			TestTrue(TEXT("引擎插件的网格槽位原样"), PluginMesh->GetStaticMaterials()[0].MaterialInterface == SlotBefore);
			TestFalse(TEXT("引擎插件的包没有被弄脏"), PluginMesh->GetOutermost()->IsDirty());
		}
	}

	// ---- 对照：/Engine/ 下的照旧锁；工程里的（临时包）照旧不锁 ----
	if (UStaticMesh* EngineCube = Fixture.LoadEngineCube())
	{
		FDreamTextureAssignTarget Target;
		TestTrue(TEXT("引擎 Cube 生成目标"), FDreamTextureMaterialAssign::MakeTarget(EngineCube, Target));
		TestTrue(TEXT("/Engine/ 下的网格仍然锁定"), Target.bLocked);
	}
	{
		FDreamTextureAssignTarget Target;
		TestTrue(TEXT("工程里的网格生成目标"), FDreamTextureMaterialAssign::MakeTarget(Fixture.Mesh, Target));
		TestFalse(TEXT("临时包里的网格不锁"), Target.bLocked);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
