// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

class AActor;
class UMaterialInterface;
class UMeshComponent;
class UStaticMesh;

/** 要被赋值的东西是什么。 */
enum class EDreamTextureAssignKind : uint8
{
	/** 关卡里某个 Actor 的网格组件：改的是这个组件实例的材质覆盖（OverrideMaterials），不动资产。 */
	Component,

	/** 一个 StaticMesh 资产：改的是资产自己的默认材质槽，所有用到它的地方（没被覆盖的）都会变。 */
	StaticMeshAsset,
};

/** 一个可被赋值的目标，以及它此刻的材质槽。纯数据快照，收集之后物体可能被删，用之前会重新判活。 */
struct DREAMTEXTUREASSETS_API FDreamTextureAssignTarget
{
	EDreamTextureAssignKind Kind = EDreamTextureAssignKind::Component;

	/** UMeshComponent 或 UStaticMesh。 */
	TWeakObjectPtr<UObject> Object;

	/** 给人看的名字：组件是「Actor 标签」（Actor 上不止一个网格组件时再加「 · 组件名」），资产是资产名。 */
	FString Label;

	/** 每个槽的名字，Num() 就是槽数。槽没有名字时是 NAME_None。 */
	TArray<FName> SlotNames;

	/**
	 * 不会被改。目前只有一种原因：引擎自带的内容（包文件在引擎目录下的资产，含引擎插件挂载点如 /Water/ 下的）——改了它会弄脏引擎的包，
	 * 而用户多半只是顺手选中了个引擎自带的网格。收集时仍然列出来，让界面能如实告诉用户「跳过了它」。
	 */
	bool bLocked = false;
	FString LockedReason;

	int32 NumSlots() const { return SlotNames.Num(); }
	bool IsAsset() const { return Kind == EDreamTextureAssignKind::StaticMeshAsset; }
};

/** 选哪些槽。多个目标时按槽号对应：槽 2 就是每个目标各自的第 2 个槽，没有第 2 个槽的目标不受影响。 */
struct DREAMTEXTUREASSETS_API FDreamTextureSlotFilter
{
	/** true = 每个槽都赋。Indices 此时被忽略。 */
	bool bAll = true;

	TArray<int32> Indices;

	static FDreamTextureSlotFilter All()
	{
		return FDreamTextureSlotFilter();
	}

	static FDreamTextureSlotFilter Only(TArray<int32> InIndices)
	{
		FDreamTextureSlotFilter Out;
		Out.bAll = false;
		Out.Indices = MoveTemp(InIndices);
		return Out;
	}

	bool Includes(int32 SlotIndex) const
	{
		return bAll || Indices.Contains(SlotIndex);
	}
};

/** 一个槽被改了：改前是什么。 */
struct DREAMTEXTUREASSETS_API FDreamTextureSlotChange
{
	int32 SlotIndex = INDEX_NONE;
	FName SlotName;

	/** 改前这个槽实际生效的材质（组件上是覆盖或网格默认；可能为空）。 */
	TWeakObjectPtr<UMaterialInterface> Previous;
};

/** 一个目标上改了哪些槽。 */
struct DREAMTEXTUREASSETS_API FDreamTextureAssignChange
{
	EDreamTextureAssignKind Kind = EDreamTextureAssignKind::Component;
	TWeakObjectPtr<UObject> Object;
	FString Label;
	TArray<FDreamTextureSlotChange> Slots;
};

struct DREAMTEXTUREASSETS_API FDreamTextureAssignResult
{
	/** 没有硬错误。一个槽都没改（全都已经是这个材质）时也是 true，TotalChanged() 为 0。 */
	bool bOK = false;

	/** bOK 为 false 时的中文原因。 */
	FString Error;

	/** 实际改了槽的目标，每个带上改了哪几个槽。本来就是这个材质的槽不算。 */
	TArray<FDreamTextureAssignChange> Changes;

	/** 因为 bLocked 或物体已被销毁而整个跳过的目标，给界面说「跳过了 X」。 */
	TArray<FString> SkippedLabels;

	/** 本次赋值是否写进了撤销栈。没有编辑器（命令行）时是 false。 */
	bool bTransacted = false;

	int32 TotalChanged() const
	{
		int32 Total = 0;
		for (const FDreamTextureAssignChange& Change : Changes)
		{
			Total += Change.Slots.Num();
		}
		return Total;
	}
};

/**
 * M3：把材质实例赋给选中的物体。
 *
 * 全程包在**一个** FScopedTransaction 里：多个 Actor、多个组件、多个槽、再加上 StaticMesh 资产，
 * Ctrl+Z 一下全部回滚，不会留下改了一半的状态。
 *
 * 本类只管「改」，不管「选」——界面模块决定目标从哪来（关卡选中 / 内容浏览器选中），再交给 Assign()。
 * 这样本模块不碰 Slate、不碰内容浏览器，自动化测试也能直接喂目标。
 */
class DREAMTEXTUREASSETS_API FDreamTextureMaterialAssign
{
public:
	/**
	 * 关卡（编辑器世界）里选中的 Actor 上的所有网格组件，一个组件一个目标。
	 * 只收有材质槽的、非编辑器专用的组件。**游戏线程。**
	 */
	static void CollectFromSelectedActors(TArray<FDreamTextureAssignTarget>& OutTargets);

	/**
	 * 选中的 Actor 里是否至少有一个带可赋值网格组件的。比 CollectFromSelectedActors 便宜（找到一个就返回），
	 * 给界面每隔一会儿探一次「按钮该不该亮」用。
	 */
	static bool AnySelectedActorAssignable();

	/**
	 * 从任意对象收目标：AActor（它所有的网格组件）、UMeshComponent、UStaticMesh 都认，别的忽略。
	 * 内容浏览器里选中的资产由界面模块加载出来传进来。
	 */
	static void CollectFromObjects(const TArray<UObject*>& Objects, TArray<FDreamTextureAssignTarget>& OutTargets);

	/** 某个组件 / 静态网格资产自己的目标。不是这两种、或没有槽，返回 false。 */
	static bool MakeTarget(UObject* Object, FDreamTextureAssignTarget& OutTarget);

	/** 这组目标在这个过滤下**将被替换**的槽数（跳过 bLocked 的；不看材质是否已经相同）。 */
	static int32 CountSlots(const TArray<FDreamTextureAssignTarget>& Targets, const FDreamTextureSlotFilter& Filter);

	/** 目标里最多的槽数，界面据此决定要不要给槽位选择。 */
	static int32 MaxSlotCount(const TArray<FDreamTextureAssignTarget>& Targets);

	/** 第 SlotIndex 个槽的名字，取第一个有这个槽的目标的；没有返回 NAME_None。 */
	static FName SlotNameAt(const TArray<FDreamTextureAssignTarget>& Targets, int32 SlotIndex);

	/**
	 * 赋值。**游戏线程。** 整个过程一个事务，名字是 TransactionName（空则用默认的）。
	 * 什么都没改（没有目标、过滤选不中任何槽、槽里本来就是它）时事务被取消，不在撤销栈里留空条目。
	 */
	static FDreamTextureAssignResult Assign(UMaterialInterface* Material, const TArray<FDreamTextureAssignTarget>& Targets,
		const FDreamTextureSlotFilter& Filter, const FText& TransactionName = FText());
};
