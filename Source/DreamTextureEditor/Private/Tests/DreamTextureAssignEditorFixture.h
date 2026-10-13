// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Editor/Transactor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

/**
 * DreamTexture.Editor.Assign.* 的夹具（Assets 模块 Private/Tests 下同名夹具的副本——Private 头文件跨模块用不了）：
 * 在编辑器世界里生成几个带 3 槽 StaticMeshComponent 的 Actor，并备好三个可区分的临时材质。
 *
 * 网格是引擎自带 /Engine/BasicShapes/Cube 的**临时副本**，外加空槽凑成 3 槽——
 * 这样测试能随便改它，而引擎自带的那个原样不动。析构时销毁 Actor、清掉撤销栈、解除 GC 保护。
 */
namespace DreamTextureAssignEditorTest
{
	struct FAssignFixture
	{
		UWorld* World = nullptr;

		/** 3 槽网格：Slot0 / Slot1 / Slot2，初始材质全空。 */
		UStaticMesh* Mesh = nullptr;
		/** 单槽网格。 */
		UStaticMesh* OneSlotMesh = nullptr;

		UMaterial* MatA = nullptr;
		UMaterial* MatB = nullptr;
		UMaterial* MatNew = nullptr;

		/** Setup 时生成、用 Mesh 的 Actor。 */
		TArray<AStaticMeshActor*> Actors;
		/** 之后用 SpawnWithMesh 额外生成的。 */
		TArray<AStaticMeshActor*> Extra;
		/** 为 GC 保护登记过的临时对象。 */
		TArray<UObject*> Rooted;

		~FAssignFixture()
		{
			if (GEditor)
			{
				GEditor->SelectNone(false, true, false);
			}
			for (AStaticMeshActor* Actor : Actors)
			{
				Destroy(Actor);
			}
			for (AStaticMeshActor* Actor : Extra)
			{
				Destroy(Actor);
			}
			// 撤销栈里还存着刚才这些对象的快照，清掉，免得后面的测试撤销到我们的残留。
			if (GEditor && GEditor->Trans)
			{
				GEditor->ResetTransaction(FText::FromString(TEXT("DreamTexture 测试收尾")));
			}
			for (UObject* Object : Rooted)
			{
				if (Object && Object->IsRooted())
				{
					Object->RemoveFromRoot();
				}
			}
		}

		static void Destroy(AStaticMeshActor* Actor)
		{
			if (IsValid(Actor))
			{
				Actor->Destroy();
			}
		}

		template <typename T>
		T* Root(T* Object)
		{
			if (Object)
			{
				Object->AddToRoot();
				Rooted.Add(Object);
			}
			return Object;
		}

		UStaticMesh* LoadEngineCube() const
		{
			return LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		}

		UMaterial* MakeMaterial(const TCHAR* Name)
		{
			return Root(NewObject<UMaterial>(GetTransientPackage(), FName(Name), RF_Transient));
		}

		/** 引擎 Cube 的临时副本，凑成 3 个槽（Slot0..Slot2），材质全空。 */
		UStaticMesh* MakeThreeSlotMesh()
		{
			UStaticMesh* Cube = LoadEngineCube();
			if (Cube == nullptr)
			{
				return nullptr;
			}
			UStaticMesh* Copy = DuplicateObject<UStaticMesh>(Cube, GetTransientPackage());
			if (Copy == nullptr)
			{
				return nullptr;
			}
			TArray<FStaticMaterial>& Materials = Copy->GetStaticMaterials();
			while (Materials.Num() < 3)
			{
				Materials.Add(FStaticMaterial(nullptr));
			}
			Materials.SetNum(3);
			for (int32 Index = 0; Index < 3; ++Index)
			{
				Materials[Index].MaterialInterface = nullptr;
				Materials[Index].MaterialSlotName = FName(*FString::Printf(TEXT("Slot%d"), Index));
				Materials[Index].ImportedMaterialSlotName = Materials[Index].MaterialSlotName;
			}
			return Root(Copy);
		}

		UStaticMesh* MakeOneSlotMesh()
		{
			UStaticMesh* Copy = MakeThreeSlotMesh();
			if (Copy)
			{
				Copy->GetStaticMaterials().SetNum(1);
			}
			return Copy;
		}

		AStaticMeshActor* SpawnWithMesh(UStaticMesh* UseMesh, const TCHAR* Label)
		{
			if (World == nullptr || UseMesh == nullptr)
			{
				return nullptr;
			}
			AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(FVector(100.0 * (Actors.Num() + Extra.Num()), 0.0, 0.0), FRotator::ZeroRotator);
			if (Actor == nullptr)
			{
				return nullptr;
			}
			UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
			// 静态的组件在世界已注册时不让换网格（编辑器世界里放宽，但先改成可动最稳）。
			Component->SetMobility(EComponentMobility::Movable);
			Component->SetStaticMesh(UseMesh);
			Actor->SetActorLabel(Label);
			Extra.Add(Actor);
			return Actor;
		}

		bool Setup(FAutomationTestBase& Test, int32 NumActors)
		{
			World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (World == nullptr)
			{
				Test.AddInfo(TEXT("已跳过：没有编辑器世界（这个测试要在编辑器里跑）。"));
				return false;
			}

			Mesh = MakeThreeSlotMesh();
			OneSlotMesh = MakeOneSlotMesh();
			if (Mesh == nullptr || OneSlotMesh == nullptr)
			{
				Test.AddError(TEXT("拿不到引擎自带的 /Engine/BasicShapes/Cube，没法造多槽网格。"));
				return false;
			}

			MatA = MakeMaterial(TEXT("DreamTextureTest_MatA"));
			MatB = MakeMaterial(TEXT("DreamTextureTest_MatB"));
			MatNew = MakeMaterial(TEXT("DreamTextureTest_MatNew"));

			for (int32 Index = 0; Index < NumActors; ++Index)
			{
				AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(FVector(100.0 * Index, 0.0, 0.0), FRotator::ZeroRotator);
				if (Actor == nullptr)
				{
					Test.AddError(TEXT("生成测试 Actor 失败。"));
					return false;
				}
				Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
				Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
				Actor->SetActorLabel(*FString::Printf(TEXT("DT_Assign_%d"), Index));
				Actors.Add(Actor);
			}

			// 清空撤销栈：之后测的是「赋值」这一条，不是生成 Actor 留下的。
			if (GEditor->Trans)
			{
				GEditor->ResetTransaction(FText::FromString(TEXT("DreamTexture 测试开始")));
			}
			return true;
		}

		UStaticMeshComponent* Comp(int32 Index) const
		{
			return Actors[Index]->GetStaticMeshComponent();
		}

		/** 选中第几个 Actor（只选这些；空 = 全不选）。 */
		void SelectActors(std::initializer_list<int32> Indices)
		{
			GEditor->SelectNone(false, true, false);
			for (const int32 Index : Indices)
			{
				GEditor->SelectActor(Actors[Index], true, false);
			}
		}

		/** Setup 时生成的所有 Actor 的所有槽，按 Actor、槽的顺序排平。 */
		TArray<UMaterialInterface*> SnapshotMaterials() const
		{
			TArray<UMaterialInterface*> Out;
			for (AStaticMeshActor* Actor : Actors)
			{
				UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
				for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
				{
					Out.Add(Component->GetMaterial(Slot));
				}
			}
			return Out;
		}

		/** 3 槽网格资产自己的默认材质。 */
		TArray<UMaterialInterface*> SnapshotMeshMaterials() const
		{
			TArray<UMaterialInterface*> Out;
			for (const FStaticMaterial& Material : Mesh->GetStaticMaterials())
			{
				Out.Add(Material.MaterialInterface);
			}
			return Out;
		}
	};
}

#endif // WITH_DEV_AUTOMATION_TESTS
