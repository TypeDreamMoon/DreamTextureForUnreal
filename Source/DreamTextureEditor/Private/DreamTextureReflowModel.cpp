// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureReflowModel.h"

#include "AssetRegistry/AssetData.h"
#include "ContentBrowserModule.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "IContentBrowserSingleton.h"
#include "Components/PrimitiveComponent.h"
#include "Materials/MaterialInterface.h"
#include "Modules/ModuleManager.h"
#include "Selection.h"

#define LOCTEXT_NAMESPACE "DreamTextureReflowModel"

//////////////////////////////////////////////////////////////////////////
// 默认后端

FDreamTextureReflowBackend FDreamTextureReflowBackend::MakeDefault()
{
	FDreamTextureReflowBackend Out;

	Out.GetSelectedMaterials = []()
	{
		TArray<UMaterialInterface*> Result;
		if (!FModuleManager::Get().IsModuleLoaded(TEXT("ContentBrowser")))
		{
			return Result;
		}
		FContentBrowserModule& Module = FModuleManager::GetModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
		TArray<FAssetData> Selected;
		Module.Get().GetSelectedAssets(Selected);
		for (const FAssetData& Data : Selected)
		{
			// 先按类过滤再加载：选了几百张贴图时不该把它们全加载进来。
			if (Data.IsInstanceOf(UMaterialInterface::StaticClass()))
			{
				if (UMaterialInterface* Material = Cast<UMaterialInterface>(Data.GetAsset()))
				{
					Result.Add(Material);
				}
			}
		}
		return Result;
	};

	Out.GetSelectedActorSlots = []()
	{
		TArray<FDreamTextureMaterialSlot> Result;
		if (GEditor == nullptr)
		{
			return Result;
		}
		TArray<AActor*> Actors;
		GEditor->GetSelectedActors()->GetSelectedObjects<AActor>(Actors);
		for (AActor* Actor : Actors)
		{
			for (FDreamTextureMaterialSlot& Slot : FDreamTextureMaterialAnalyzer::GetActorSlots(Actor))
			{
				Slot.Label = Actor->GetActorLabel() + TEXT(" / ") + Slot.Label;
				Result.Add(MoveTemp(Slot));
			}
		}
		return Result;
	};

	Out.Analyze = [](UMaterialInterface* Material, const FDreamTextureAnalyzeOptions& Options)
	{
		return FDreamTextureMaterialAnalyzer::Analyze(Material, Options);
	};

	Out.Export = [](const FDreamTextureMaterialAnalysis& Analysis, const FDreamTextureExportOptions& Options)
	{
		return FDreamTextureTextureExporter::ExportAnalysis(Analysis, Options);
	};

	Out.MakeTempDir = []()
	{
		return FDreamTextureTextureExporter::MakeTempDir();
	};

	return Out;
}

//////////////////////////////////////////////////////////////////////////
// 文案

FText FDreamTextureReflowModel::RoleDisplayName(EDreamTextureRole Role)
{
	switch (Role)
	{
	case EDreamTextureRole::BaseColor: return LOCTEXT("RoleBaseColor", "Base Color");
	case EDreamTextureRole::Normal: return LOCTEXT("RoleNormal", "Normal");
	case EDreamTextureRole::Roughness: return LOCTEXT("RoleRoughness", "Roughness");
	case EDreamTextureRole::Metallic: return LOCTEXT("RoleMetallic", "Metallic");
	case EDreamTextureRole::AO: return LOCTEXT("RoleAO", "Ambient Occlusion");
	case EDreamTextureRole::Height: return LOCTEXT("RoleHeight", "Height");
	case EDreamTextureRole::Packed: return LOCTEXT("RolePacked", "Packed texture (ORM)");
	case EDreamTextureRole::Emissive: return LOCTEXT("RoleEmissive", "Emissive");
	case EDreamTextureRole::Opacity: return LOCTEXT("RoleOpacity", "Opacity");
	case EDreamTextureRole::Specular: return LOCTEXT("RoleSpecular", "Specular");
	case EDreamTextureRole::Other: return LOCTEXT("RoleOther", "Other (ignored)");
	default: return LOCTEXT("RoleUnknown", "Unrecognized");
	}
}

FText FDreamTextureReflowModel::ChannelContentDisplayName(EDreamTextureRole Role)
{
	switch (Role)
	{
	case EDreamTextureRole::Unknown: return LOCTEXT("ChannelUnknown", "?");
	case EDreamTextureRole::Other: return LOCTEXT("ChannelUnused", "Unused");
	case EDreamTextureRole::AO: return LOCTEXT("ChannelAO", "AO");
	default: return RoleDisplayName(Role);
	}
}

FText FDreamTextureReflowModel::EvidenceDisplayName(EDreamTextureEvidence Evidence)
{
	switch (Evidence)
	{
	case EDreamTextureEvidence::UserOverride: return LOCTEXT("EvidenceUser", "Your manual assignment");
	case EDreamTextureEvidence::SourceRecord: return LOCTEXT("EvidenceRecord", "Provenance record");
	case EDreamTextureEvidence::MaterialGraph: return LOCTEXT("EvidenceGraph", "Material graph");
	case EDreamTextureEvidence::ParameterName: return LOCTEXT("EvidenceParameter", "Parameter name");
	case EDreamTextureEvidence::AssetName: return LOCTEXT("EvidenceAssetName", "Asset name");
	case EDreamTextureEvidence::SamplerType: return LOCTEXT("EvidenceSampler", "Sampler type (guess)");
	default: return LOCTEXT("EvidenceNone", "No evidence");
	}
}

FText FDreamTextureReflowModel::SinkDisplayName(EDreamTextureSink Sink)
{
	// 材质属性名在材质编辑器里就是英文，照原样给：用户对着材质编辑器核对时认得出来。
	switch (Sink)
	{
	case EDreamTextureSink::UVOffset: return LOCTEXT("SinkUV", "UV offset");
	case EDreamTextureSink::BlendMask: return LOCTEXT("SinkBlend", "Blend weight");
	default: return FText::FromString(LexToString(Sink));
	}
}

FText FDreamTextureReflowModel::DescribeUses(const FDreamTextureMaterialTexture& Row)
{
	if (Row.Uses.Num() == 0)
	{
		return LOCTEXT("NoUses", "(no usage recorded in the material graph)");
	}

	auto ChannelsText = [](uint8 Channels)
	{
		FString Out;
		if (Channels & DreamTextureChannelBit::R) { Out += TEXT("R"); }
		if (Channels & DreamTextureChannelBit::G) { Out += TEXT("G"); }
		if (Channels & DreamTextureChannelBit::B) { Out += TEXT("B"); }
		if (Channels & DreamTextureChannelBit::A) { Out += TEXT("A"); }
		return Out;
	};

	TArray<FString> Parts;
	for (const FDreamTextureTextureUse& Use : Row.Uses)
	{
		const FText Channels = FText::FromString(ChannelsText(Use.Channels));
		const FText Part = Use.bApproximate
			? FText::Format(LOCTEXT("UseApproximate", "{0} ← {1} (approx.)"), SinkDisplayName(Use.Sink), Channels)
			: FText::Format(LOCTEXT("UseExact", "{0} ← {1}"), SinkDisplayName(Use.Sink), Channels);
		Parts.AddUnique(Part.ToString());
	}
	return FText::FromString(FString::Join(Parts, *LOCTEXT("UseSeparator", "; ").ToString()));
}

//////////////////////////////////////////////////////////////////////////
// 构造与通知

FDreamTextureReflowModel::FDreamTextureReflowModel(FDreamTextureReflowBackend InBackend)
	: Backend(MoveTemp(InBackend))
{
}

void FDreamTextureReflowModel::Notify(const FText& Message, bool bIsError)
{
	OnNotice.Broadcast(Message, bIsError);
}

//////////////////////////////////////////////////////////////////////////
// 选区

const FDreamTextureReflowCandidate* FDreamTextureReflowModel::GetSelectedCandidate() const
{
	return Candidates.IsValidIndex(SelectedIndex) ? &Candidates[SelectedIndex] : nullptr;
}

void FDreamTextureReflowModel::RefreshSelection()
{
	const FString PreviousKey = Candidates.IsValidIndex(SelectedIndex) ? Candidates[SelectedIndex].Key : FString();

	Candidates.Reset();

	if (Backend.GetSelectedMaterials)
	{
		for (UMaterialInterface* Material : Backend.GetSelectedMaterials())
		{
			if (Material == nullptr)
			{
				continue;
			}
			FDreamTextureReflowCandidate Candidate;
			Candidate.Kind = FDreamTextureReflowCandidate::EKind::Asset;
			Candidate.Material = Material;
			Candidate.MaterialPath = Material->GetPathName();
			Candidate.Label = Material->GetName();
			Candidate.Key = TEXT("asset:") + Candidate.MaterialPath;
			Candidates.Add(MoveTemp(Candidate));
		}
	}

	if (Backend.GetSelectedActorSlots)
	{
		for (const FDreamTextureMaterialSlot& Slot : Backend.GetSelectedActorSlots())
		{
			FDreamTextureReflowCandidate Candidate;
			Candidate.Kind = FDreamTextureReflowCandidate::EKind::ActorSlot;
			Candidate.Material = Slot.Material;
			if (const UMaterialInterface* Material = Slot.Material.Get())
			{
				Candidate.MaterialPath = Material->GetPathName();
			}
			Candidate.Label = Slot.Label;
			// 键不含材质：用户在槽里换了材质，仍然是「同一个槽」，刷新后应该保持选中。
			// 标签里带着材质名，所以不能拿标签当键；有组件就用「组件路径 + 槽号」，没有（测试里的假槽）才退回标签。
			Candidate.Key = Slot.Component.IsValid()
				? FString::Printf(TEXT("slot:%s:%d"), *Slot.Component->GetPathName(), Slot.SlotIndex)
				: FString::Printf(TEXT("slot:%s"), *Slot.Label);
			Candidates.Add(MoveTemp(Candidate));
		}
	}

	// 之前选的还在就保持；否则只有一个候选时自动选它。
	int32 NewIndex = INDEX_NONE;
	if (!PreviousKey.IsEmpty())
	{
		NewIndex = Candidates.IndexOfByPredicate([&PreviousKey](const FDreamTextureReflowCandidate& Candidate) { return Candidate.Key == PreviousKey; });
	}
	if (NewIndex == INDEX_NONE && Candidates.Num() == 1)
	{
		NewIndex = 0;
	}

	const bool bSameSelection = (NewIndex != INDEX_NONE) && !PreviousKey.IsEmpty() && Candidates[NewIndex].Key == PreviousKey;
	SelectedIndex = NewIndex;
	if (!bSameSelection)
	{
		// 换了对象：上一个对象的导出结果不属于它。
		Exports.Reset();
		ExportDir.Reset();
	}

	// 无论选中的是不是同一个，都重新解析一遍：用户可能刚在材质编辑器里改过图。
	Reanalyze();
}

void FDreamTextureReflowModel::SelectCandidate(int32 Index)
{
	const int32 NewIndex = Candidates.IsValidIndex(Index) ? Index : INDEX_NONE;
	if (NewIndex != SelectedIndex)
	{
		Exports.Reset();
		ExportDir.Reset();
	}
	SelectedIndex = NewIndex;
	Reanalyze();
}

//////////////////////////////////////////////////////////////////////////
// 解析

void FDreamTextureReflowModel::RebuildRows()
{
	// 同一路径的行复用原来的指针：STileView/SListView 靠指针认选中态和滚动位置。
	TMap<FString, FDreamTextureReflowRowItemPtr> Old;
	for (const FDreamTextureReflowRowItemPtr& Item : Rows)
	{
		Old.Add(Item->TexturePath, Item);
	}
	Rows.Reset();
	for (const FDreamTextureMaterialTexture& Texture : Analysis.Textures)
	{
		if (const FDreamTextureReflowRowItemPtr* Found = Old.Find(Texture.TexturePath))
		{
			Rows.Add(*Found);
		}
		else
		{
			FDreamTextureReflowRowItemPtr Item = MakeShared<FDreamTextureReflowRowItem>();
			Item->TexturePath = Texture.TexturePath;
			Rows.Add(Item);
		}
	}
}

void FDreamTextureReflowModel::Reanalyze()
{
	Analysis = FDreamTextureMaterialAnalysis();
	bHasAnalysis = false;

	if (const FDreamTextureReflowCandidate* Candidate = GetSelectedCandidate())
	{
		if (UMaterialInterface* Material = Candidate->Material.Get())
		{
			FDreamTextureAnalyzeOptions Options;
			Options.Overrides = Backend.Overrides;
			if (Backend.Analyze)
			{
				Analysis = Backend.Analyze(Material, Options);
				bHasAnalysis = true;
			}
		}
	}

	RebuildRows();
	OnChanged.Broadcast();
}

const FDreamTextureMaterialTexture* FDreamTextureReflowModel::FindTexture(const FString& TexturePath) const
{
	return Analysis.FindTexture(TexturePath);
}

FDreamTextureReflowModel::EViewState FDreamTextureReflowModel::GetViewState() const
{
	if (Candidates.Num() == 0)
	{
		return EViewState::NoSelection;
	}
	const FDreamTextureReflowCandidate* Candidate = GetSelectedCandidate();
	if (Candidate == nullptr)
	{
		return EViewState::PickOne;
	}
	if (Candidate->IsEmptySlot())
	{
		return EViewState::EmptySlot;
	}
	if (!bHasAnalysis || !Analysis.bOK)
	{
		return EViewState::AnalysisFailed;
	}
	if (Analysis.Textures.Num() == 0)
	{
		return EViewState::NoTextures;
	}
	return EViewState::Ready;
}

FText FDreamTextureReflowModel::GetStatusText() const
{
	switch (GetViewState())
	{
	case EViewState::NoSelection:
		return LOCTEXT("StatusNoSelection", "Nothing is selected yet. Select a material or material instance in the Content Browser, or an Actor in the level, then click Get Selection above.");
	case EViewState::PickOne:
		return FText::Format(LOCTEXT("StatusPickOne", "The selection contains {0} object(s) that can be reflowed. Pick one above."), FText::AsNumber(Candidates.Num()));
	case EViewState::EmptySlot:
		return LOCTEXT("StatusEmptySlot", "This material slot is empty, so there is no material to analyze.");
	case EViewState::AnalysisFailed:
		if (bHasAnalysis && !Analysis.Error.IsEmpty())
		{
			return FText::FromString(Analysis.Error);
		}
		return LOCTEXT("StatusFailed", "The material is no longer in memory (it was unloaded or deleted). Get the selection again.");
	case EViewState::NoTextures:
		return LOCTEXT("StatusNoTextures", "This material uses no textures (only constants and scalar parameters), so there is nothing to reflow.");
	default:
		return FText::GetEmpty();
	}
}

//////////////////////////////////////////////////////////////////////////
// 用户指认

FDreamTextureOverrideStore& FDreamTextureReflowModel::GetOverrideStore() const
{
	return Backend.Overrides != nullptr ? *Backend.Overrides : FDreamTextureOverrideStore::Get();
}

void FDreamTextureReflowModel::SetOverride(const FString& TexturePath, const FDreamTextureRoleOverride& Override)
{
	if (!bHasAnalysis || !Analysis.bOK)
	{
		return;
	}
	if (Analysis.AssetPath.IsEmpty())
	{
		Notify(LOCTEXT("OverrideNoAsset", "This material is not an asset in the project (it was created at runtime), so your manual assignment cannot be remembered."), true);
		return;
	}

	FDreamTextureOverrideStore& Store = GetOverrideStore();
	if (!Store.Set(Analysis.AssetPath, TexturePath, Override))
	{
		Notify(FText::Format(LOCTEXT("OverrideSaveFailed", "Could not write the manual assignment to the record file: {0}. The assignment still applies for this editor session but will be lost after a restart."), FText::FromString(Store.GetLastError())), true);
	}
	Reanalyze();
}

void FDreamTextureReflowModel::SetRole(const FString& TexturePath, EDreamTextureRole Role)
{
	if (Role == EDreamTextureRole::Unknown)
	{
		ClearOverride(TexturePath);
		return;
	}
	const FDreamTextureMaterialTexture* Row = FindTexture(TexturePath);
	if (Row == nullptr)
	{
		return;
	}
	FDreamTextureRoleOverride Override;
	Override.Role = Role;
	if (Role == EDreamTextureRole::Packed)
	{
		Override.Layout = Row->Layout;
	}
	SetOverride(TexturePath, Override);
}

void FDreamTextureReflowModel::SetLayoutChannel(const FString& TexturePath, int32 ChannelIndex, EDreamTextureRole Role)
{
	const FDreamTextureMaterialTexture* Row = FindTexture(TexturePath);
	if (Row == nullptr || ChannelIndex < 0 || ChannelIndex > 3)
	{
		return;
	}
	FDreamTextureRoleOverride Override;
	Override.Role = EDreamTextureRole::Packed;
	if (Row->Role == EDreamTextureRole::Packed)
	{
		Override.Layout = Row->Layout;
	}
	Override.Layout.Channels[ChannelIndex] = Role;
	SetOverride(TexturePath, Override);
}

void FDreamTextureReflowModel::ClearOverride(const FString& TexturePath)
{
	if (!bHasAnalysis || !Analysis.bOK || Analysis.AssetPath.IsEmpty())
	{
		return;
	}
	GetOverrideStore().Remove(Analysis.AssetPath, TexturePath);
	Reanalyze();
}

//////////////////////////////////////////////////////////////////////////
// 导出

void FDreamTextureReflowModel::SetExportMaxEdge(int32 InMaxEdge)
{
	ExportMaxEdge = FMath::Max(0, InMaxEdge);
	OnChanged.Broadcast();
}

int32 FDreamTextureReflowModel::GetExportableCount() const
{
	int32 Count = 0;
	for (const FDreamTextureMaterialTexture& Texture : Analysis.Textures)
	{
		Count += Texture.bExportable ? 1 : 0;
	}
	return Count;
}

bool FDreamTextureReflowModel::CanExport() const
{
	return GetViewState() == EViewState::Ready && GetExportableCount() > 0;
}

void FDreamTextureReflowModel::Export()
{
	if (GetViewState() != EViewState::Ready)
	{
		Notify(LOCTEXT("ExportNothing", "There is no analysis result to export. Get the selection first."), true);
		return;
	}
	if (GetExportableCount() == 0)
	{
		Notify(LOCTEXT("ExportNoneExportable", "None of the textures in this material can be exported. See the Export column in the table for the reasons."), true);
		return;
	}
	if (!Backend.Export || !Backend.MakeTempDir)
	{
		return;
	}

	FDreamTextureExportOptions Options;
	Options.OutputDir = Backend.MakeTempDir();
	Options.MaxEdge = ExportMaxEdge;

	const TArray<FDreamTextureExportedImage> Results = Backend.Export(Analysis, Options);

	Exports.Reset();
	int32 NumExported = 0;
	int32 NumBlocked = 0;
	for (int32 Index = 0; Index < Analysis.Textures.Num() && Index < Results.Num(); ++Index)
	{
		Exports.Add(Analysis.Textures[Index].TexturePath, Results[Index]);
		(Results[Index].bExported ? NumExported : NumBlocked) += 1;
	}
	ExportDir = Options.OutputDir;

	if (NumBlocked > 0)
	{
		Notify(FText::Format(LOCTEXT("ExportPartial", "Exported {0} texture(s) to {2}; {1} could not be exported, see the table for the reasons."), FText::AsNumber(NumExported), FText::AsNumber(NumBlocked), FText::FromString(ExportDir)), NumExported == 0);
	}
	else
	{
		Notify(FText::Format(LOCTEXT("ExportDone", "Exported {0} texture(s) to {1}."), FText::AsNumber(NumExported), FText::FromString(ExportDir)), false);
	}
	OnChanged.Broadcast();
}

const FDreamTextureExportedImage* FDreamTextureReflowModel::FindExport(const FString& TexturePath) const
{
	return Exports.Find(TexturePath);
}

#undef LOCTEXT_NAMESPACE
