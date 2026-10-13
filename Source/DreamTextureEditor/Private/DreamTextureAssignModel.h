// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DreamTextureMaterialAssign.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UMaterialInterface;

/** 这次要赋给谁，是从哪儿选出来的。 */
enum class EDreamTextureAssignSource : uint8
{
	/** 什么都没选，或选的东西没有材质槽。 */
	None,

	/** 关卡里选中的 Actor。 */
	Actors,

	/** 内容浏览器里选中的静态网格资产。 */
	Assets,
};

/**
 * 「赋给选中物体」此刻的选择快照。
 *
 * ## 关卡选中的 Actor 优先于内容浏览器
 *
 * 两边的选中是各管各的：用户在关卡里点了个 Actor，内容浏览器里很可能还留着上一次选的网格。
 * 两边都算进去，等于在用户不知情时顺手改了资产的默认材质——所有用到它的地方都跟着变，
 * 影响面比用户以为的大得多。所以有可赋值的 Actor 就只算 Actor，内容浏览器里残留的选中只提示一句、不动。
 */
struct FDreamTextureAssignSelection
{
	EDreamTextureAssignSource Source = EDreamTextureAssignSource::None;
	TArray<FDreamTextureAssignTarget> Targets;

	/** 关卡选中的 Actor 数（Source 为 Actors 时有意义）。 */
	int32 NumActors = 0;

	/** Source 为 Actors 时，内容浏览器里还选着、这次不会去改的静态网格数。 */
	int32 NumIgnoredAssets = 0;

	/** 有槽可替换（不算锁住的）。 */
	bool HasUsableSlots() const
	{
		return FDreamTextureMaterialAssign::CountSlots(Targets, FDreamTextureSlotFilter::All()) > 0;
	}

	/** 读当前编辑器里的选中。会加载内容浏览器里选中的静态网格资产——只在用户真点了按钮时调。 */
	static FDreamTextureAssignSelection Query();

	/** 自动化测试用：直接给目标，来源按目标种类推。 */
	static FDreamTextureAssignSelection FromTargets(TArray<FDreamTextureAssignTarget> InTargets);
};

/** 按钮此刻该不该亮，以及不亮的原因。很便宜，界面每帧都可以问。 */
struct FDreamTextureAssignProbe
{
	bool bCanAssign = false;

	/** 一句话。能赋时说会改哪儿；不能赋时说为什么、该怎么办。 */
	FText Hint;

	/**
	 * 探一次：关卡选中里有没有可赋值的 Actor，没有再看内容浏览器里有没有选中静态网格（不加载资产）。
	 * 结果缓存 0.25 秒——选中几千个 Actor 时逐帧遍历不起。
	 */
	static const FDreamTextureAssignProbe& Get(bool bHasMaterial, const FText& NoMaterialHint);

	/** 丢掉缓存。自动化测试在同一帧里连着改选中、连着探时用。 */
	static void Invalidate();
};

/**
 * 赋值确认框背后的数据：材质、选择、选了哪些槽，以及「将替换 N 个材质槽」这句话。
 * 不碰 Slate，自动化测试直接断言文案和赋值结果。
 */
class FDreamTextureAssignModel : public TSharedFromThis<FDreamTextureAssignModel>
{
public:
	FDreamTextureAssignModel(UMaterialInterface* InMaterial, FDreamTextureAssignSelection InSelection);

	UMaterialInterface* GetMaterial() const { return Material.Get(); }
	FText GetMaterialName() const;

	const FDreamTextureAssignSelection& GetSelection() const { return Selection; }

	//~ 槽位选择
	/** 目标里最大槽数。> 1 时界面才给槽位选择。 */
	int32 GetMaxSlots() const { return MaxSlots; }
	/** 第 SlotIndex 个槽有几个目标有。 */
	int32 GetNumTargetsWithSlot(int32 SlotIndex) const;
	/** 「Slot1」之类，槽没名字时是空。 */
	FText GetSlotName(int32 SlotIndex) const;

	bool IsAllSlots() const { return bAllSlots; }
	void SetAllSlots(bool bInAll);
	bool IsSlotChosen(int32 SlotIndex) const { return ChosenSlots.Contains(SlotIndex); }
	void SetSlotChosen(int32 SlotIndex, bool bChosen);
	FDreamTextureSlotFilter GetFilter() const;

	//~ 文案
	/** 「将替换 N 个材质槽」。 */
	int32 GetReplaceCount() const;
	FText GetHeadline() const;
	/** 来源与影响面的说明（关卡 Actor / 资产默认材质），含内容浏览器残留选中的提醒。 */
	FText GetSourceText() const;
	/** 逐个目标一行：「名字 — N 个槽」，太多就折叠。 */
	FText GetTargetListText() const;
	/** 被跳过的引擎自带网格。没有则为空。 */
	FText GetLockedText() const;
	/** 资产默认槽的风险提示。只有 Source 为 Assets 时非空。 */
	FText GetAssetWarningText() const;

	bool CanApply(FText* OutWhy = nullptr) const;

	/** 赋值。整个赋值一个事务。 */
	FDreamTextureAssignResult Apply();

	/** 把结果翻成给用户看的话：改了几个槽、哪些目标哪些槽，或「本来就是它」。 */
	static FText DescribeResult(const FDreamTextureAssignResult& Result, const FText& MaterialName);

private:
	TWeakObjectPtr<UMaterialInterface> Material;
	FDreamTextureAssignSelection Selection;
	int32 MaxSlots = 0;
	bool bAllSlots = true;
	TSet<int32> ChosenSlots;
};

/** 从材质实例（或任何材质）的对象路径到「赋给选中物体」的整条流程。 */
namespace DreamTextureAssignFlow
{
	/** 按对象路径加载材质；找不到返回 nullptr。 */
	UMaterialInterface* LoadMaterial(const FString& ObjectPath);

	/**
	 * 弹出确认框（先说「将替换 N 个材质槽」，多槽时让选槽位），确认后赋值，再用一条通知说结果。
	 * 没有选中可赋值的东西时不弹框，直接通知原因。没有 Slate（命令行/测试）时什么都不做。
	 */
	void Run(UMaterialInterface* Material);
	void Run(const FString& MaterialObjectPath);
}
