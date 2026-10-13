// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureMaterialAssign.h"

#include "Components/MeshComponent.h"
#include "DreamTextureLog.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "DreamTextureMaterialAssign"

namespace
{
	/**
	 * 引擎自带内容：包文件在引擎目录（FPaths::EngineDir()）下。
	 *
	 * 不能只认 /Engine/ 前缀：引擎插件的内容挂载点是插件自己的名字（/Water/、/LiveLink/、/Niagara/ 这类），
	 * 文件却在 Engine/Plugins/.../Content 下，同样是随引擎发布的、改了会弄脏引擎的包。所以按磁盘路径判断。
	 * 临时包不算（那不是磁盘上的引擎资产）；没有挂载点的包名（/Memory/ 之类）转不出磁盘路径，也不算。
	 */
	bool IsEngineContent(const UObject* Object)
	{
		const UPackage* Package = Object ? Object->GetOutermost() : nullptr;
		if (Package == nullptr || Package == GetTransientPackage())
		{
			return false;
		}

		FString Filename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), Filename, FPackageName::GetAssetPackageExtension()))
		{
			return false;
		}
		static const FString EngineDir = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
		return FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(Filename), EngineDir);
	}

	FString ActorDisplayName(const AActor* Actor)
	{
		const FString ActorLabel = Actor->GetActorLabel();
		return ActorLabel.IsEmpty() ? Actor->GetName() : ActorLabel;
	}

	/** 只收真正有槽、给人看的网格组件：编辑器专用的和可视化辅助组件不算。 */
	bool IsAssignableComponent(const UMeshComponent* Component)
	{
		return Component != nullptr
			&& !Component->IsEditorOnly()
			&& !Component->IsVisualizationComponent()
			&& !Component->IsTemplate()
			&& Component->GetNumMaterials() > 0;
	}

	void FillComponentSlots(const UMeshComponent* Component, TArray<FName>& OutNames)
	{
		const int32 Num = Component->GetNumMaterials();
		OutNames = Component->GetMaterialSlotNames();
		// 有的组件的槽名数组和槽数对不上（没有名字的槽）：补齐成 NAME_None，槽数以 GetNumMaterials 为准。
		OutNames.SetNum(Num);
	}

	void AppendActorTargets(AActor* Actor, TArray<FDreamTextureAssignTarget>& OutTargets)
	{
		if (Actor == nullptr || Actor->IsTemplate())
		{
			return;
		}

		TArray<UMeshComponent*> Components;
		Actor->GetComponents<UMeshComponent>(Components);
		Components.RemoveAll([](const UMeshComponent* Component) { return !IsAssignableComponent(Component); });

		for (UMeshComponent* Component : Components)
		{
			FDreamTextureAssignTarget Target;
			Target.Kind = EDreamTextureAssignKind::Component;
			Target.Object = Component;
			FillComponentSlots(Component, Target.SlotNames);
			Target.Label = Components.Num() > 1
				? FString::Printf(TEXT("%s · %s"), *ActorDisplayName(Actor), *Component->GetName())
				: ActorDisplayName(Actor);
			OutTargets.Add(MoveTemp(Target));
		}
	}
}

bool FDreamTextureMaterialAssign::MakeTarget(UObject* Object, FDreamTextureAssignTarget& OutTarget)
{
	if (UMeshComponent* Component = Cast<UMeshComponent>(Object))
	{
		if (!IsAssignableComponent(Component))
		{
			return false;
		}
		OutTarget = FDreamTextureAssignTarget();
		OutTarget.Kind = EDreamTextureAssignKind::Component;
		OutTarget.Object = Component;
		FillComponentSlots(Component, OutTarget.SlotNames);
		const AActor* Owner = Component->GetOwner();
		OutTarget.Label = Owner ? FString::Printf(TEXT("%s · %s"), *ActorDisplayName(Owner), *Component->GetName()) : Component->GetName();
		return true;
	}

	if (UStaticMesh* Mesh = Cast<UStaticMesh>(Object))
	{
		const TArray<FStaticMaterial>& Materials = Mesh->GetStaticMaterials();
		if (Materials.Num() == 0)
		{
			return false;
		}
		OutTarget = FDreamTextureAssignTarget();
		OutTarget.Kind = EDreamTextureAssignKind::StaticMeshAsset;
		OutTarget.Object = Mesh;
		OutTarget.Label = FText::Format(NSLOCTEXT("DreamTextureMaterialAssign", "AssetLabel", "{0} (asset)"), FText::FromString(Mesh->GetName())).ToString();
		for (const FStaticMaterial& Material : Materials)
		{
			OutTarget.SlotNames.Add(Material.MaterialSlotName);
		}
		if (IsEngineContent(Mesh))
		{
			OutTarget.bLocked = true;
			OutTarget.LockedReason = NSLOCTEXT("DreamTextureMaterialAssign", "EngineContentLocked", "Engine content; it is not modified.").ToString();
		}
		return true;
	}

	return false;
}

void FDreamTextureMaterialAssign::CollectFromSelectedActors(TArray<FDreamTextureAssignTarget>& OutTargets)
{
	if (GEditor == nullptr)
	{
		return;
	}

	USelection* Selection = GEditor->GetSelectedActors();
	if (Selection == nullptr)
	{
		return;
	}

	for (FSelectionIterator It(*Selection); It; ++It)
	{
		AppendActorTargets(Cast<AActor>(*It), OutTargets);
	}
}

bool FDreamTextureMaterialAssign::AnySelectedActorAssignable()
{
	USelection* Selection = GEditor ? GEditor->GetSelectedActors() : nullptr;
	if (Selection == nullptr)
	{
		return false;
	}

	for (FSelectionIterator It(*Selection); It; ++It)
	{
		const AActor* Actor = Cast<AActor>(*It);
		if (Actor == nullptr || Actor->IsTemplate())
		{
			continue;
		}
		TArray<UMeshComponent*> Components;
		Actor->GetComponents<UMeshComponent>(Components);
		for (const UMeshComponent* Component : Components)
		{
			if (IsAssignableComponent(Component))
			{
				return true;
			}
		}
	}
	return false;
}

void FDreamTextureMaterialAssign::CollectFromObjects(const TArray<UObject*>& Objects, TArray<FDreamTextureAssignTarget>& OutTargets)
{
	for (UObject* Object : Objects)
	{
		if (AActor* Actor = Cast<AActor>(Object))
		{
			AppendActorTargets(Actor, OutTargets);
			continue;
		}

		FDreamTextureAssignTarget Target;
		if (MakeTarget(Object, Target))
		{
			OutTargets.Add(MoveTemp(Target));
		}
	}
}

int32 FDreamTextureMaterialAssign::CountSlots(const TArray<FDreamTextureAssignTarget>& Targets, const FDreamTextureSlotFilter& Filter)
{
	int32 Count = 0;
	for (const FDreamTextureAssignTarget& Target : Targets)
	{
		if (Target.bLocked)
		{
			continue;
		}
		for (int32 SlotIndex = 0; SlotIndex < Target.NumSlots(); ++SlotIndex)
		{
			if (Filter.Includes(SlotIndex))
			{
				++Count;
			}
		}
	}
	return Count;
}

int32 FDreamTextureMaterialAssign::MaxSlotCount(const TArray<FDreamTextureAssignTarget>& Targets)
{
	int32 Max = 0;
	for (const FDreamTextureAssignTarget& Target : Targets)
	{
		if (!Target.bLocked)
		{
			Max = FMath::Max(Max, Target.NumSlots());
		}
	}
	return Max;
}

FName FDreamTextureMaterialAssign::SlotNameAt(const TArray<FDreamTextureAssignTarget>& Targets, int32 SlotIndex)
{
	for (const FDreamTextureAssignTarget& Target : Targets)
	{
		if (!Target.bLocked && Target.SlotNames.IsValidIndex(SlotIndex))
		{
			return Target.SlotNames[SlotIndex];
		}
	}
	return NAME_None;
}

FDreamTextureAssignResult FDreamTextureMaterialAssign::Assign(UMaterialInterface* Material, const TArray<FDreamTextureAssignTarget>& Targets,
	const FDreamTextureSlotFilter& Filter, const FText& TransactionName)
{
	FDreamTextureAssignResult Result;

	if (Material == nullptr)
	{
		Result.Error = NSLOCTEXT("DreamTextureMaterialAssign", "NoMaterial", "There is no material to assign.").ToString();
		return Result;
	}
	if (!Filter.bAll && Filter.Indices.Num() == 0)
	{
		Result.Error = NSLOCTEXT("DreamTextureMaterialAssign", "NoSlotSelected", "No material slot is selected.").ToString();
		return Result;
	}

	// 事务必须在碰任何东西之前开：Modify() 只在事务里才会把改前的样子存进撤销栈。
	// 整个赋值共用这一个，所以多 Actor / 多槽 / 加资产，Ctrl+Z 一次全部回滚。
	// 没有编辑器（命令行）时不开事务，赋值照样进行，只是没法撤销（bTransacted = false）。
	const bool bCanTransact = GEditor != nullptr && GEditor->Trans != nullptr;
	TUniquePtr<FScopedTransaction> Transaction;
	if (bCanTransact)
	{
		Transaction = MakeUnique<FScopedTransaction>(TransactionName.IsEmpty()
			? LOCTEXT("DefaultTransaction", "DreamTexture: Assign to Selection")
			: TransactionName);
	}

	for (const FDreamTextureAssignTarget& Target : Targets)
	{
		UObject* Object = Target.Object.Get();
		if (Object == nullptr || Target.bLocked)
		{
			Result.SkippedLabels.Add(Target.Label);
			continue;
		}

		FDreamTextureAssignChange Change;
		Change.Kind = Target.Kind;
		Change.Object = Object;
		Change.Label = Target.Label;

		if (UMeshComponent* Component = Cast<UMeshComponent>(Object))
		{
			// 先收集要改的槽，一个都没有就不碰这个组件（也就不会把它标脏）。
			TArray<int32> ToChange;
			const int32 NumMaterials = Component->GetNumMaterials();
			for (int32 SlotIndex = 0; SlotIndex < NumMaterials; ++SlotIndex)
			{
				if (Filter.Includes(SlotIndex) && Component->GetMaterial(SlotIndex) != Material)
				{
					ToChange.Add(SlotIndex);
				}
			}
			if (ToChange.Num() == 0)
			{
				continue;
			}

			// 组件默认不是 RF_Transactional 的话 Modify() 存不进撤销栈——打上标记。这个标记本身不影响任何行为。
			Component->SetFlags(RF_Transactional);
			Component->Modify();

			const TArray<FName> SlotNames = Component->GetMaterialSlotNames();
			for (const int32 SlotIndex : ToChange)
			{
				FDreamTextureSlotChange Slot;
				Slot.SlotIndex = SlotIndex;
				Slot.SlotName = SlotNames.IsValidIndex(SlotIndex) ? SlotNames[SlotIndex] : NAME_None;
				Slot.Previous = Component->GetMaterial(SlotIndex);
				Change.Slots.Add(Slot);

				Component->SetMaterial(SlotIndex, Material);
			}
		}
		else if (UStaticMesh* Mesh = Cast<UStaticMesh>(Object))
		{
			TArray<int32> ToChange;
			const TArray<FStaticMaterial>& Materials = Mesh->GetStaticMaterials();
			for (int32 SlotIndex = 0; SlotIndex < Materials.Num(); ++SlotIndex)
			{
				if (Filter.Includes(SlotIndex) && Materials[SlotIndex].MaterialInterface != Material)
				{
					ToChange.Add(SlotIndex);
				}
			}
			if (ToChange.Num() == 0)
			{
				continue;
			}

			Mesh->SetFlags(RF_Transactional);
			Mesh->Modify();

			for (const int32 SlotIndex : ToChange)
			{
				FDreamTextureSlotChange Slot;
				Slot.SlotIndex = SlotIndex;
				Slot.SlotName = Mesh->GetStaticMaterials()[SlotIndex].MaterialSlotName;
				Slot.Previous = Mesh->GetStaticMaterials()[SlotIndex].MaterialInterface;
				Change.Slots.Add(Slot);

				// UStaticMesh::SetMaterial 自带一个事务（"StaticMesh: Material changed"）、
				// PreEditChange / PostEditChangeProperty、并给还没名字的槽补名字。
				// 嵌在我们的事务里，并入同一条撤销记录，不会多出一条。
				Mesh->SetMaterial(SlotIndex, Material);
			}
		}
		else
		{
			Result.SkippedLabels.Add(Target.Label);
			continue;
		}

		Result.Changes.Add(MoveTemp(Change));
	}

	Result.bOK = true;
	Result.bTransacted = bCanTransact && Result.Changes.Num() > 0;

	if (Transaction.IsValid() && Result.Changes.Num() == 0)
	{
		// 一个槽都没改：别在撤销栈里留一条空记录，用户按 Ctrl+Z 却什么都没发生很让人困惑。
		Transaction->Cancel();
	}

	UE_LOG(LogDreamTexture, Log, TEXT("Assigned material %s: changed %d target(s) and %d slot(s), skipped %d target(s)"),
		*Material->GetPathName(), Result.Changes.Num(), Result.TotalChanged(), Result.SkippedLabels.Num());
	return Result;
}

#undef LOCTEXT_NAMESPACE
