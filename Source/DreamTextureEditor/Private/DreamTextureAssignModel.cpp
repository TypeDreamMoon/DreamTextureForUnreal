// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureAssignModel.h"

#include "AssetRegistry/AssetData.h"
#include "Components/MeshComponent.h"
#include "ContentBrowserModule.h"
#include "DreamTextureLog.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "IContentBrowserSingleton.h"
#include "Materials/MaterialInterface.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "DreamTextureAssignModel"

namespace
{
	/** 内容浏览器里选中的静态网格资产（只看 AssetData，不加载）。内容浏览器没起来时为空。 */
	void GetSelectedStaticMeshAssetData(TArray<FAssetData>& OutAssets)
	{
		if (!FModuleManager::Get().IsModuleLoaded(TEXT("ContentBrowser")))
		{
			return;
		}

		TArray<FAssetData> Selected;
		FModuleManager::GetModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().GetSelectedAssets(Selected);

		const FTopLevelAssetPath MeshClass = UStaticMesh::StaticClass()->GetClassPathName();
		for (const FAssetData& Asset : Selected)
		{
			if (Asset.AssetClassPath == MeshClass)
			{
				OutAssets.Add(Asset);
			}
		}
	}

	// 列表项之间的分隔符，随语言变（中文是「、」）。
	FText ListSeparator()
	{
		return LOCTEXT("ListSeparator", ", ");
	}

	FText SlotLabel(int32 SlotIndex, FName SlotName)
	{
		return SlotName.IsNone()
			? FText::Format(LOCTEXT("SlotPlain", "Slot {0}"), FText::AsNumber(SlotIndex))
			: FText::Format(LOCTEXT("SlotNamed", "Slot {0} ({1})"), FText::AsNumber(SlotIndex), FText::FromName(SlotName));
	}
}

//////////////////////////////////////////////////////////////////////////
// 选择

FDreamTextureAssignSelection FDreamTextureAssignSelection::Query()
{
	FDreamTextureAssignSelection Out;

	FDreamTextureMaterialAssign::CollectFromSelectedActors(Out.Targets);

	TArray<FAssetData> SelectedMeshes;
	GetSelectedStaticMeshAssetData(SelectedMeshes);

	if (Out.Targets.Num() > 0)
	{
		Out.Source = EDreamTextureAssignSource::Actors;
		TSet<const AActor*> Owners;
		for (const FDreamTextureAssignTarget& Target : Out.Targets)
		{
			if (const UMeshComponent* Component = Cast<UMeshComponent>(Target.Object.Get()))
			{
				Owners.Add(Component->GetOwner());
			}
		}
		Out.NumActors = Owners.Num();
		Out.NumIgnoredAssets = SelectedMeshes.Num();
		return Out;
	}

	// 关卡里没有可赋值的 Actor：看内容浏览器。到这一步才真加载资产。
	TArray<UObject*> Meshes;
	for (const FAssetData& Asset : SelectedMeshes)
	{
		if (UObject* Mesh = Asset.GetAsset())
		{
			Meshes.Add(Mesh);
		}
	}
	FDreamTextureMaterialAssign::CollectFromObjects(Meshes, Out.Targets);
	Out.Source = Out.Targets.Num() > 0 ? EDreamTextureAssignSource::Assets : EDreamTextureAssignSource::None;
	return Out;
}

FDreamTextureAssignSelection FDreamTextureAssignSelection::FromTargets(TArray<FDreamTextureAssignTarget> InTargets)
{
	FDreamTextureAssignSelection Out;
	Out.Targets = MoveTemp(InTargets);
	if (Out.Targets.Num() > 0)
	{
		Out.Source = Out.Targets[0].IsAsset() ? EDreamTextureAssignSource::Assets : EDreamTextureAssignSource::Actors;
	}
	if (Out.Source == EDreamTextureAssignSource::Actors)
	{
		TSet<const AActor*> Owners;
		for (const FDreamTextureAssignTarget& Target : Out.Targets)
		{
			if (const UMeshComponent* Component = Cast<UMeshComponent>(Target.Object.Get()))
			{
				Owners.Add(Component->GetOwner());
			}
		}
		Out.NumActors = Owners.Num();
	}
	return Out;
}

//////////////////////////////////////////////////////////////////////////
// 探针

namespace
{
	FDreamTextureAssignProbe GCachedProbe;
	double GLastProbeTime = -1000.0;
	bool GLastHadMaterial = false;
}

void FDreamTextureAssignProbe::Invalidate()
{
	GLastProbeTime = -1000.0;
}

const FDreamTextureAssignProbe& FDreamTextureAssignProbe::Get(bool bHasMaterial, const FText& NoMaterialHint)
{
	FDreamTextureAssignProbe& Cached = GCachedProbe;
	double& LastProbeTime = GLastProbeTime;
	bool& bLastHadMaterial = GLastHadMaterial;

	// 材质有无变了就立刻重算：它决定的是 Hint 说哪句话，不是选中。
	const double Now = FPlatformTime::Seconds();
	if (Now - LastProbeTime < 0.25 && bLastHadMaterial == bHasMaterial)
	{
		return Cached;
	}
	LastProbeTime = Now;
	bLastHadMaterial = bHasMaterial;

	if (!bHasMaterial)
	{
		Cached.bCanAssign = false;
		Cached.Hint = NoMaterialHint;
		return Cached;
	}

	if (GEditor == nullptr)
	{
		Cached.bCanAssign = false;
		Cached.Hint = LOCTEXT("NoEditor", "The editor is not ready yet.");
		return Cached;
	}

	if (FDreamTextureMaterialAssign::AnySelectedActorAssignable())
	{
		Cached.bCanAssign = true;
		Cached.Hint = LOCTEXT("HintActors", "Assigns the material to the Actors selected in the level. Clicking first tells you how many material slots will be replaced, and Ctrl+Z undoes the whole change.");
		return Cached;
	}

	TArray<FAssetData> Meshes;
	GetSelectedStaticMeshAssetData(Meshes);
	if (Meshes.Num() > 0)
	{
		Cached.bCanAssign = true;
		Cached.Hint = LOCTEXT("HintAssets", "No Actor is selected in the level, so this changes the default material of the static mesh assets selected in the Content Browser. Clicking first tells you how many material slots will be replaced, and Ctrl+Z undoes the whole change.");
		return Cached;
	}

	Cached.bCanAssign = false;
	Cached.Hint = LOCTEXT("HintNothing", "Nothing is selected. Select an Actor with a mesh in the level, or a static mesh asset in the Content Browser, first.");
	return Cached;
}

//////////////////////////////////////////////////////////////////////////
// 模型

FDreamTextureAssignModel::FDreamTextureAssignModel(UMaterialInterface* InMaterial, FDreamTextureAssignSelection InSelection)
	: Material(InMaterial)
	, Selection(MoveTemp(InSelection))
{
	MaxSlots = FDreamTextureMaterialAssign::MaxSlotCount(Selection.Targets);
}

FText FDreamTextureAssignModel::GetMaterialName() const
{
	const UMaterialInterface* Mat = Material.Get();
	return Mat ? FText::FromString(Mat->GetName()) : LOCTEXT("NoMaterialName", "(material no longer exists)");
}

int32 FDreamTextureAssignModel::GetNumTargetsWithSlot(int32 SlotIndex) const
{
	int32 Count = 0;
	for (const FDreamTextureAssignTarget& Target : Selection.Targets)
	{
		if (!Target.bLocked && SlotIndex < Target.NumSlots())
		{
			++Count;
		}
	}
	return Count;
}

FText FDreamTextureAssignModel::GetSlotName(int32 SlotIndex) const
{
	const FName Name = FDreamTextureMaterialAssign::SlotNameAt(Selection.Targets, SlotIndex);
	return Name.IsNone() ? FText::GetEmpty() : FText::FromName(Name);
}

void FDreamTextureAssignModel::SetAllSlots(bool bInAll)
{
	bAllSlots = bInAll;
}

void FDreamTextureAssignModel::SetSlotChosen(int32 SlotIndex, bool bChosen)
{
	if (bChosen)
	{
		ChosenSlots.Add(SlotIndex);
	}
	else
	{
		ChosenSlots.Remove(SlotIndex);
	}
}

FDreamTextureSlotFilter FDreamTextureAssignModel::GetFilter() const
{
	// 只有一个槽的时候没有槽位选择，「全部」就是唯一的选择。
	if (bAllSlots || MaxSlots <= 1)
	{
		return FDreamTextureSlotFilter::All();
	}
	TArray<int32> Indices = ChosenSlots.Array();
	Indices.Sort();
	return FDreamTextureSlotFilter::Only(MoveTemp(Indices));
}

int32 FDreamTextureAssignModel::GetReplaceCount() const
{
	return FDreamTextureMaterialAssign::CountSlots(Selection.Targets, GetFilter());
}

FText FDreamTextureAssignModel::GetHeadline() const
{
	const int32 Count = GetReplaceCount();
	return Count > 0
		? FText::Format(LOCTEXT("Headline", "Will replace {0} {0}|plural(one=material slot,other=material slots)"), Count)
		: LOCTEXT("HeadlineNone", "No material slots to replace");
}

FText FDreamTextureAssignModel::GetSourceText() const
{
	int32 Usable = 0;
	for (const FDreamTextureAssignTarget& Target : Selection.Targets)
	{
		Usable += Target.bLocked ? 0 : 1;
	}

	switch (Selection.Source)
	{
	case EDreamTextureAssignSource::Actors:
	{
		FText Text = FText::Format(LOCTEXT("SourceActors", "{0} {0}|plural(one=Actor,other=Actors) selected in the level, with {1} mesh {1}|plural(one=component,other=components) in total. This changes the material overrides on those components and leaves the mesh assets alone."),
			Selection.NumActors, Usable);
		if (Selection.NumIgnoredAssets > 0)
		{
			Text = FText::Format(LOCTEXT("SourceIgnoredAssets", "{0}\nThe Content Browser also has {1} static {1}|plural(one=mesh,other=meshes) selected; they are left alone this time. To change the assets instead, deselect the Actors first."),
				Text, Selection.NumIgnoredAssets);
		}
		return Text;
	}
	case EDreamTextureAssignSource::Assets:
		return FText::Format(LOCTEXT("SourceAssets", "{0} static mesh {0}|plural(one=asset,other=assets) selected in the Content Browser."), Selection.Targets.Num());
	default:
		return LOCTEXT("SourceNone", "Nothing assignable is selected.");
	}
}

FText FDreamTextureAssignModel::GetTargetListText() const
{
	constexpr int32 MaxLines = 6;

	TArray<FText> Lines;
	int32 Shown = 0;
	int32 Hidden = 0;
	for (const FDreamTextureAssignTarget& Target : Selection.Targets)
	{
		if (Target.bLocked)
		{
			continue;
		}
		if (Shown >= MaxLines)
		{
			++Hidden;
			continue;
		}
		++Shown;
		Lines.Add(FText::Format(LOCTEXT("TargetLine", "· {0} — {1} {1}|plural(one=slot,other=slots)"), FText::FromString(Target.Label), Target.NumSlots()));
	}
	if (Hidden > 0)
	{
		Lines.Add(FText::Format(LOCTEXT("TargetMore", "· and {0} more"), FText::AsNumber(Hidden)));
	}
	return FText::Join(FText::FromString(TEXT("\n")), Lines);
}

FText FDreamTextureAssignModel::GetLockedText() const
{
	TArray<FString> Names;
	for (const FDreamTextureAssignTarget& Target : Selection.Targets)
	{
		if (Target.bLocked)
		{
			Names.Add(Target.Label);
		}
	}
	if (Names.Num() == 0)
	{
		return FText::GetEmpty();
	}
	return FText::Format(LOCTEXT("Locked", "Skipped {0} engine {0}|plural(one=mesh,other=meshes) because changing engine assets would dirty engine packages: {1}"),
		Names.Num(), FText::FromString(FString::Join(Names, *ListSeparator().ToString())));
}

FText FDreamTextureAssignModel::GetAssetWarningText() const
{
	if (Selection.Source != EDreamTextureAssignSource::Assets)
	{
		return FText::GetEmpty();
	}
	return LOCTEXT("AssetWarning", "This changes the asset's own default material: everything that uses the asset without a material override of its own will change too, and the asset will be marked as modified (it needs to be saved).");
}

bool FDreamTextureAssignModel::CanApply(FText* OutWhy) const
{
	auto Why = [OutWhy](const FText& Text) { if (OutWhy) { *OutWhy = Text; } return false; };

	if (!Material.IsValid())
	{
		return Why(LOCTEXT("WhyNoMaterial", "The material to assign no longer exists."));
	}
	if (Selection.Source == EDreamTextureAssignSource::None || !Selection.HasUsableSlots())
	{
		return Why(LOCTEXT("WhyNoTarget", "Nothing with material slots is selected."));
	}
	if (GetReplaceCount() <= 0)
	{
		return Why(LOCTEXT("WhyNoSlot", "Select at least one material slot."));
	}
	return true;
}

FDreamTextureAssignResult FDreamTextureAssignModel::Apply()
{
	return FDreamTextureMaterialAssign::Assign(Material.Get(), Selection.Targets, GetFilter(),
		FText::Format(LOCTEXT("TransactionName", "DreamTexture: Assign {0} to Selection"), GetMaterialName()));
}

FText FDreamTextureAssignModel::DescribeResult(const FDreamTextureAssignResult& Result, const FText& MaterialName)
{
	if (!Result.bOK)
	{
		return FText::Format(LOCTEXT("ResultFailed", "The material could not be assigned: {0}"), FText::FromString(Result.Error));
	}

	if (Result.TotalChanged() == 0)
	{
		return FText::Format(LOCTEXT("ResultUnchanged", "The selected material slots already use '{0}', so nothing was changed."), MaterialName);
	}

	TArray<FText> Lines;
	Lines.Add(FText::Format(LOCTEXT("ResultOK", "Assigned '{0}' to {1} {1}|plural(one=material slot,other=material slots) ({2} {2}|plural(one=target,other=targets)). Press Ctrl+Z to undo."),
		MaterialName, Result.TotalChanged(), Result.Changes.Num()));

	constexpr int32 MaxLines = 4;
	int32 Shown = 0;
	for (const FDreamTextureAssignChange& Change : Result.Changes)
	{
		if (Shown >= MaxLines)
		{
			Lines.Add(FText::Format(LOCTEXT("ResultMore", "· and {0} more"), FText::AsNumber(Result.Changes.Num() - Shown)));
			break;
		}
		++Shown;

		TArray<FText> Slots;
		for (const FDreamTextureSlotChange& Slot : Change.Slots)
		{
			Slots.Add(SlotLabel(Slot.SlotIndex, Slot.SlotName));
		}
		Lines.Add(FText::Format(LOCTEXT("ResultLine", "· {0}: {1}"), FText::FromString(Change.Label),
			FText::Join(ListSeparator(), Slots)));
	}

	if (Result.SkippedLabels.Num() > 0)
	{
		Lines.Add(FText::Format(LOCTEXT("ResultSkipped", "Skipped: {0}"), FText::FromString(FString::Join(Result.SkippedLabels, *ListSeparator().ToString()))));
	}
	return FText::Join(FText::FromString(TEXT("\n")), Lines);
}

UMaterialInterface* DreamTextureAssignFlow::LoadMaterial(const FString& ObjectPath)
{
	if (ObjectPath.IsEmpty())
	{
		return nullptr;
	}
	return LoadObject<UMaterialInterface>(nullptr, *ObjectPath);
}

#undef LOCTEXT_NAMESPACE
