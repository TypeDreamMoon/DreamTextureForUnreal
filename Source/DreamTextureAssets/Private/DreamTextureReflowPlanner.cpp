// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureReflow.h"

#include "DreamTextureApi.h"
#include "DreamTextureMaterialBuilder.h"
#include "Engine/Texture.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

/*
 * 回流的「材质这一头」。设计上的几条取舍，写在这里而不是散在各函数里：
 *
 * ## 「已有」的认定
 *
 * 一个通道算已有，要么解析结果里有一张扮演它的贴图，要么（插件自己建的实例上）它挂在插件管的贴图参数上。
 * 后者是为了 UsePOM 关着时的 Height：解析器只走静态开关生效的那一支，看不到它，可它是用户的东西。
 * 默认勾选只勾缺的，所以「已有」认漏一个，就等于用户一勾就把他原来的高度图盖掉。
 *
 * ## 打包图成组
 *
 * ORM 里的 AO / 粗糙度 / 金属度是一张贴图的三个通道。回流不做像素级的重新打包，所以只能整张换或整张不换：
 * 勾其中一个就是勾三个。排布不是标准 ORM 的打包图（RMA、带高度的 ORMH）插件不碰——
 * 拿后端的标准 ORM 去顶它会把排布错位的通道全写坏。
 *
 * ## 底色
 *
 * 补通道时底色不重新生成：后端的底色是缩放、重新编码过的，换上去等于悄悄改了用户的底色。
 * 新实例的 BaseColor 直接指向用户原来那张。
 */

namespace
{
	using FState = FDreamTextureReflowChannelState;
	using EPresence = FDreamTextureReflowChannelState::EPresence;

	/** 一张打包图的通道角色 → 回流用的通道名。不是这六个之一的返回空串。 */
	FString RoleToChannel(EDreamTextureRole Role)
	{
		switch (Role)
		{
		case EDreamTextureRole::AO: return DreamTextureChannel::AO;
		case EDreamTextureRole::Roughness: return DreamTextureChannel::Roughness;
		case EDreamTextureRole::Metallic: return DreamTextureChannel::Metallic;
		case EDreamTextureRole::Height: return DreamTextureChannel::Height;
		default: return FString();
		}
	}

	FState* FindState(TArray<FState>& States, const FString& Channel)
	{
		return States.FindByPredicate([&Channel](const FState& State) { return State.Channel == Channel; });
	}

	const FState* FindState(const TArray<FState>& States, const FString& Channel)
	{
		return States.FindByPredicate([&Channel](const FState& State) { return State.Channel == Channel; });
	}

	/** 这个通道要一起处理的那一组（含自己）。 */
	TArray<FString> GroupOf(const TArray<FState>& States, const FString& Channel)
	{
		const FState* State = FindState(States, Channel);
		if (State != nullptr && State->TiedChannels.Num() > 0)
		{
			return State->TiedChannels;
		}
		return { Channel };
	}

	void Claim(FState& State, EPresence Presence, const FDreamTextureMaterialTexture& Row, const TArray<FString>& Tied)
	{
		if (State.Presence != EPresence::Missing)
		{
			++State.NumCandidates;
			return;
		}
		State.Presence = Presence;
		State.TexturePath = Row.TexturePath;
		State.TextureName = Row.AssetName;
		State.Parameters = Row.Parameters;
		State.TiedChannels = Tied;
		State.NumCandidates = 1;
	}

	/**
	 * 引擎自带内容：包文件的磁盘路径在引擎目录下（含 Engine/Plugins/.../Content）。
	 * 不能只认 /Engine/ 前缀——临时包叫 /Engine/Transient，却不是磁盘上的引擎资产。
	 * 与 DreamTextureMaterialAssign.cpp 里同名的判断同一口径（那份在它自己的匿名命名空间里）。
	 */
	bool PlannerIsEngineContent(const UObject* Object)
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

	/** 引擎自带的占位贴图（母材质参数的默认值）不算用户的。 */
	bool IsUserTexture(const UTexture* Texture)
	{
		return Texture != nullptr && !PlannerIsEngineContent(Texture);
	}

	/** 实例上这个参数此刻生效的贴图；是引擎默认占位或没有就返回空。 */
	UTexture* GetUserTextureOnParameter(const UMaterialInterface* Material, const FName& Parameter)
	{
		UTexture* Value = nullptr;
		if (Material->GetTextureParameterValue(FHashedMaterialParameterInfo(Parameter), Value, /*bOveriddenOnly*/ false) && IsUserTexture(Value))
		{
			return Value;
		}
		return nullptr;
	}

	bool IsPluginMaster(const FDreamTextureMaterialAnalysis& Analysis)
	{
		if (Analysis.BaseMaterialPath.IsEmpty())
		{
			return false;
		}
		return FPackageName::ObjectPathToObjectName(Analysis.BaseMaterialPath).Equals(FDreamTextureMasterMaterial::GetAssetName(), ESearchCase::CaseSensitive);
	}
}

//////////////////////////////////////////////////////////////////////////
// 基础

const TCHAR* LexToString(EDreamTextureReflowMode Mode)
{
	return Mode == EDreamTextureReflowMode::Restyle ? TEXT("restyle") : TEXT("fill_channels");
}

const TArray<FString>& DreamTextureReflow::GetChannels()
{
	static const TArray<FString> Channels = {
		DreamTextureChannel::BaseColor,
		DreamTextureChannel::Normal,
		DreamTextureChannel::Roughness,
		DreamTextureChannel::Metallic,
		DreamTextureChannel::AO,
		DreamTextureChannel::Height,
	};
	return Channels;
}

FString DreamTextureReflow::GetChannelLabel(const FString& Channel)
{
	if (Channel == DreamTextureChannel::BaseColor) { return NSLOCTEXT("DreamTextureReflowPlanner", "LabelBaseColor", "Base Color").ToString(); }
	if (Channel == DreamTextureChannel::Normal) { return NSLOCTEXT("DreamTextureReflowPlanner", "LabelNormal", "Normal").ToString(); }
	if (Channel == DreamTextureChannel::Roughness) { return NSLOCTEXT("DreamTextureReflowPlanner", "LabelRoughness", "Roughness").ToString(); }
	if (Channel == DreamTextureChannel::Metallic) { return NSLOCTEXT("DreamTextureReflowPlanner", "LabelMetallic", "Metallic").ToString(); }
	if (Channel == DreamTextureChannel::AO) { return NSLOCTEXT("DreamTextureReflowPlanner", "LabelAO", "Ambient Occlusion").ToString(); }
	if (Channel == DreamTextureChannel::Height) { return NSLOCTEXT("DreamTextureReflowPlanner", "LabelHeight", "Height").ToString(); }
	if (Channel == DreamTextureChannel::ORM) { return TEXT("ORM"); }
	return Channel;
}

bool DreamTextureReflow::IsDenoiseAllowed(EDreamTextureUVKind Kind, double Denoise)
{
	if (Kind == EDreamTextureUVKind::Unwrapped)
	{
		// 比较时留一点浮点余量：滑块把 0.35 存成 float 再读回来不该被判超限。
		return Denoise <= MaxUnwrappedDenoise + 1.0e-6;
	}
	return true;
}

FString DreamTextureReflow::DescribeDenoiseLimit(double Denoise)
{
	const FString Limit = FString::Printf(TEXT("%.2f"), MaxUnwrappedDenoise);
	return FText::Format(NSLOCTEXT("DreamTextureReflowPlanner", "DenoiseLimit", "For a UV-unwrapped texture, the denoise strength of a Restyle cannot exceed {0} (it is {1} now). Restyling changes the layout of the content, and every piece of content on a UV-unwrapped texture maps to a specific face of the mesh, so once the layout changes the texture no longer fits the model (seams do not line up and details land in the wrong place). Lower the denoise strength to {0} or less, or switch back to Tileable first."),
		FText::FromString(Limit), FText::FromString(FString::Printf(TEXT("%.2f"), Denoise))).ToString();
}

//////////////////////////////////////////////////////////////////////////
// 现状

TArray<FDreamTextureReflowChannelState> FDreamTextureReflowPlanner::Describe(const FDreamTextureMaterialAnalysis& Analysis, const UMaterialInterface* Material)
{
	TArray<FState> States;
	for (const FString& Channel : DreamTextureReflow::GetChannels())
	{
		FState State;
		State.Channel = Channel;
		States.Add(MoveTemp(State));
	}

	const TArray<FString> OrmGroup = { DreamTextureChannel::AO, DreamTextureChannel::Roughness, DreamTextureChannel::Metallic };

	for (const FDreamTextureMaterialTexture& Row : Analysis.Textures)
	{
		if (Row.Role == EDreamTextureRole::Packed)
		{
			// A 通道里存着别的东西（高度、不透明度…）的「ORM」：整张换会毁掉它，按非标准排布处理。
			const EDreamTextureRole A = Row.Layout.Channels[3];
			const bool bAlphaCarriesData = A != EDreamTextureRole::Unknown && A != EDreamTextureRole::Other;
			const bool bStandard = Row.GetManifestChannel() == DreamTextureChannel::ORM && !bAlphaCarriesData;

			if (bStandard)
			{
				for (const FString& Channel : OrmGroup)
				{
					Claim(*FindState(States, Channel), EPresence::InStandardORM, Row, OrmGroup);
				}
			}
			else
			{
				TArray<FString> Members;
				for (int32 Index = 0; Index < 4; ++Index)
				{
					const FString Channel = RoleToChannel(Row.Layout.Channels[Index]);
					if (!Channel.IsEmpty())
					{
						Members.AddUnique(Channel);
					}
				}
				for (const FString& Channel : Members)
				{
					FState& State = *FindState(States, Channel);
					const bool bWasMissing = State.Presence == EPresence::Missing;
					Claim(State, EPresence::InOtherPacked, Row, Members);
					if (bWasMissing)
					{
						State.Note = FText::Format(NSLOCTEXT("DreamTextureReflowPlanner", "NoteOtherPacked", "It is in the packed texture {0} ({1}), whose layout is not the standard ORM; the plugin will not change it."), FText::FromString(Row.AssetName), FText::FromString(Row.Layout.ToString())).ToString();
					}
				}
			}
			continue;
		}

		const FString Channel = Row.GetManifestChannel();
		if (FState* State = Channel.IsEmpty() ? nullptr : FindState(States, Channel))
		{
			Claim(*State, EPresence::Dedicated, Row, { Channel });
		}
	}

	// ---- 插件自己建的实例：看参数上实际挂着什么 ----
	// 「插件建的」有两个认法，任一成立即可：根材质是插件母材质，或实例的包元数据里记着它是为哪套材质建的（M2 的归属记录）。
	// 后者在用户把母材质挪走 / 改名之后仍然成立。
	const bool bPluginInstance = Material != nullptr && Cast<UMaterialInstanceConstant>(Material) != nullptr
		&& (IsPluginMaster(Analysis) || !FDreamTextureMaterialBuilder::ReadInstanceMaterialId(Material).IsEmpty());
	if (bPluginInstance)
	{
		const TArray<TPair<FName, FString>> Singles = {
			{ DreamTextureMaterialParam::BaseColor, DreamTextureChannel::BaseColor },
			{ DreamTextureMaterialParam::Normal, DreamTextureChannel::Normal },
			{ DreamTextureMaterialParam::Height, DreamTextureChannel::Height },
		};
		for (const TPair<FName, FString>& Pair : Singles)
		{
			FState& State = *FindState(States, Pair.Value);
			if (State.Presence != EPresence::Missing)
			{
				continue;
			}
			if (const UTexture* Texture = GetUserTextureOnParameter(Material, Pair.Key))
			{
				State.Presence = EPresence::Dedicated;
				State.TexturePath = Texture->GetPathName();
				State.TextureName = Texture->GetName();
				State.Parameters = { Pair.Key };
				State.TiedChannels = { Pair.Value };
				State.NumCandidates = 1;
				State.bFromPluginParameter = true;
			}
		}

		const bool bOrmUnclaimed = FindState(States, DreamTextureChannel::AO)->Presence == EPresence::Missing
			&& FindState(States, DreamTextureChannel::Roughness)->Presence == EPresence::Missing
			&& FindState(States, DreamTextureChannel::Metallic)->Presence == EPresence::Missing;
		if (bOrmUnclaimed)
		{
			if (const UTexture* Texture = GetUserTextureOnParameter(Material, DreamTextureMaterialParam::ORM))
			{
				for (const FString& Channel : OrmGroup)
				{
					FState& State = *FindState(States, Channel);
					State.Presence = EPresence::InStandardORM;
					State.TexturePath = Texture->GetPathName();
					State.TextureName = Texture->GetName();
					State.Parameters = { DreamTextureMaterialParam::ORM };
					State.TiedChannels = OrmGroup;
					State.NumCandidates = 1;
					State.bFromPluginParameter = true;
				}
			}
		}
	}

	for (FState& State : States)
	{
		if (State.NumCandidates > 1 && State.Note.IsEmpty())
		{
			State.Note = FText::Format(NSLOCTEXT("DreamTextureReflowPlanner", "NoteMultipleCandidates", "The material has {0} textures playing the {1} role; only the first one ({2}) is handled."), State.NumCandidates, FText::FromString(DreamTextureReflow::GetChannelLabel(State.Channel)), FText::FromString(State.TextureName)).ToString();
		}
	}
	return States;
}

TArray<const FDreamTextureMaterialTexture*> FDreamTextureReflowPlanner::FindBaseColorSources(const FDreamTextureMaterialAnalysis& Analysis)
{
	TArray<const FDreamTextureMaterialTexture*> Out;
	for (const FDreamTextureMaterialTexture& Row : Analysis.Textures)
	{
		if (Row.Role == EDreamTextureRole::BaseColor && Row.bExportable)
		{
			Out.Add(&Row);
		}
	}
	return Out;
}

//////////////////////////////////////////////////////////////////////////
// 勾选

bool FDreamTextureReflowPlanner::IsLocked(const FString& Channel, EDreamTextureReflowMode Mode)
{
	return Mode == EDreamTextureReflowMode::FillChannels && Channel == DreamTextureChannel::BaseColor;
}

TSet<FString> FDreamTextureReflowPlanner::Normalize(const TArray<FState>& States, const TSet<FString>& Selection, EDreamTextureReflowMode Mode)
{
	TSet<FString> Out;
	for (const FString& Channel : Selection)
	{
		if (DreamTextureReflow::GetChannels().Contains(Channel) && !IsLocked(Channel, Mode))
		{
			Out.Add(Channel);
		}
	}

	// 成组：组里有一个勾了，整组都勾。
	for (const FState& State : States)
	{
		if (State.TiedChannels.Num() > 1)
		{
			bool bAny = false;
			for (const FString& Tied : State.TiedChannels)
			{
				bAny |= Out.Contains(Tied);
			}
			if (bAny)
			{
				for (const FString& Tied : State.TiedChannels)
				{
					Out.Add(Tied);
				}
			}
		}
	}
	return Out;
}

TSet<FString> FDreamTextureReflowPlanner::DefaultSelection(const TArray<FState>& States, EDreamTextureReflowMode Mode)
{
	TSet<FString> Selection;
	for (const FState& State : States)
	{
		if (IsLocked(State.Channel, Mode))
		{
			continue;
		}
		const bool bRestyleBase = Mode == EDreamTextureReflowMode::Restyle && State.Channel == DreamTextureChannel::BaseColor;
		if (bRestyleBase || !State.IsPresent())
		{
			Selection.Add(State.Channel);
		}
	}
	return Normalize(States, Selection, Mode);
}

TSet<FString> FDreamTextureReflowPlanner::Toggle(const TArray<FState>& States, const TSet<FString>& Selection, EDreamTextureReflowMode Mode, const FString& Channel, bool bOn)
{
	TSet<FString> Next = Selection;
	for (const FString& Member : GroupOf(States, Channel))
	{
		if (bOn)
		{
			Next.Add(Member);
		}
		else
		{
			Next.Remove(Member);
		}
	}
	return Normalize(States, Next, Mode);
}

//////////////////////////////////////////////////////////////////////////
// 新建

FDreamTextureReflowCreatePlan FDreamTextureReflowPlanner::PlanCreate(const TArray<FState>& States, const TSet<FString>& InSelection,
	EDreamTextureReflowMode Mode, const FString& SourceBaseColorPath)
{
	FDreamTextureReflowCreatePlan Plan;
	const TSet<FString> Selection = Normalize(States, InSelection, Mode);
	auto Checked = [&Selection](const FString& Channel) { return Selection.Contains(Channel); };

	// ---- 要从后端结果里导入哪些 ----
	TSet<FString> Wanted;
	if (Mode == EDreamTextureReflowMode::Restyle && Checked(DreamTextureChannel::BaseColor))
	{
		Wanted.Add(DreamTextureChannel::BaseColor);
	}
	if (Checked(DreamTextureChannel::Normal))
	{
		Wanted.Add(DreamTextureChannel::Normal);
	}
	if (Checked(DreamTextureChannel::Height))
	{
		Wanted.Add(DreamTextureChannel::Height);
	}
	const bool bAnyPacked = Checked(DreamTextureChannel::AO) || Checked(DreamTextureChannel::Roughness) || Checked(DreamTextureChannel::Metallic);
	if (bAnyPacked)
	{
		// 母材质只采样 ORM：粗糙度/金属度/AO 任何一个被勾，就要这张打包图。
		Wanted.Add(DreamTextureChannel::ORM);
	}
	for (const FString& Separate : { DreamTextureChannel::Roughness, DreamTextureChannel::Metallic, DreamTextureChannel::AO })
	{
		if (Selection.Contains(Separate))
		{
			Wanted.Add(Separate);
		}
	}

	// 设置里没开「分通道导入」时，分通道图本来就不导（ORM 已经包含了）：与默认通道取交集。
	Plan.Options.bExplicitChannels = true;
	for (const FString& Channel : FDreamTextureFetcher::DefaultChannels())
	{
		if (Wanted.Contains(Channel))
		{
			Plan.Options.Channels.Add(Channel);
		}
	}

	if (Plan.Options.Channels.Num() == 0)
	{
		Plan.Error = Mode == EDreamTextureReflowMode::FillChannels
			? NSLOCTEXT("DreamTextureReflowPlanner", "ErrorNoFillChannel", "Check at least one channel to fill.").ToString()
			: NSLOCTEXT("DreamTextureReflowPlanner", "ErrorNoRegenerateChannel", "Check at least one channel to regenerate.").ToString();
		return Plan;
	}

	// ---- 没勾的，沿用用户原来的 ----
	const bool bBaseGenerated = Wanted.Contains(DreamTextureChannel::BaseColor);
	if (!bBaseGenerated && !SourceBaseColorPath.IsEmpty())
	{
		Plan.Options.ReuseTextures.Add(DreamTextureChannel::BaseColor, SourceBaseColorPath);
	}

	for (const FString& Single : { DreamTextureChannel::Normal, DreamTextureChannel::Height })
	{
		const FState* State = FindState(States, Single);
		if (!Wanted.Contains(Single) && State != nullptr && State->Presence == EPresence::Dedicated && !State->TexturePath.IsEmpty())
		{
			Plan.Options.ReuseTextures.Add(Single, State->TexturePath);
		}
	}

	const FState* Ao = FindState(States, DreamTextureChannel::AO);
	if (!bAnyPacked && Ao != nullptr && Ao->Presence == EPresence::InStandardORM && !Ao->TexturePath.IsEmpty())
	{
		Plan.Options.ReuseTextures.Add(DreamTextureChannel::ORM, Ao->TexturePath);
	}

	// ---- 带不过去的，说一声 ----
	for (const FString& Channel : { DreamTextureChannel::Roughness, DreamTextureChannel::Metallic, DreamTextureChannel::AO })
	{
		const FState* State = FindState(States, Channel);
		if (State == nullptr || Selection.Contains(Channel))
		{
			continue;
		}
		const bool bCarried = State->Presence == EPresence::InStandardORM && !bAnyPacked;
		if (State->IsPresent() && !bCarried)
		{
			Plan.Notes.Add(FText::Format(NSLOCTEXT("DreamTextureReflowPlanner", "NoteNotCarriedORM", "The original material's {0} ({1}) was not carried over to the new instance: the new instance's master material only accepts one standard ORM texture."),
				FText::FromString(DreamTextureReflow::GetChannelLabel(Channel)), FText::FromString(State->TextureName)).ToString());
		}
	}
	for (const FString& Single : { DreamTextureChannel::Normal, DreamTextureChannel::Height })
	{
		const FState* State = FindState(States, Single);
		if (State != nullptr && !Selection.Contains(Single) && State->IsPresent() && !Plan.Options.ReuseTextures.Contains(Single))
		{
			Plan.Notes.Add(FText::Format(NSLOCTEXT("DreamTextureReflowPlanner", "NoteNotCarried", "The original material's {0} ({1}) was not carried over to the new instance: {2}"), FText::FromString(DreamTextureReflow::GetChannelLabel(Single)), FText::FromString(State->TextureName),
				State->Note.IsEmpty() ? NSLOCTEXT("DreamTextureReflowPlanner", "NoteInUnsplitPacked", "it is in a packed texture the plugin does not split.") : FText::FromString(State->Note)).ToString());
		}
	}

	Plan.bOK = true;
	return Plan;
}

//////////////////////////////////////////////////////////////////////////
// 原地覆盖

bool FDreamTextureReflowPlanner::CanOverwriteInPlace(const FDreamTextureMaterialAnalysis& Analysis, const UMaterialInterface* Material, FString& OutReason)
{
	if (!Analysis.bOK || Material == nullptr)
	{
		OutReason = NSLOCTEXT("DreamTextureReflowPlanner", "ReasonNotAnalyzed", "The material has not been analyzed yet, so there is nothing to change.").ToString();
		return false;
	}
	if (Cast<UMaterialInstanceConstant>(Material) == nullptr)
	{
		if (!Analysis.bIsInstance)
		{
			OutReason = NSLOCTEXT("DreamTextureReflowPlanner", "ReasonMasterMaterial", "A master material is selected; its textures are wired directly into the graph, so there are no parameters to change. Overwrite in place only works on material instances: select its material instance, or use Create new.").ToString();
		}
		else
		{
			OutReason = NSLOCTEXT("DreamTextureReflowPlanner", "ReasonTransientInstance", "This material instance was generated temporarily at runtime and is not an asset in the project, so changes would not persist. Select a material instance asset in the project, or use Create new.").ToString();
		}
		return false;
	}
	if (PlannerIsEngineContent(Material))
	{
		OutReason = NSLOCTEXT("DreamTextureReflowPlanner", "ReasonEngineInstance", "This is an engine material instance; changing it would dirty an engine package. Use Create new.").ToString();
		return false;
	}
	return true;
}

bool FDreamTextureReflowPlanner::CanOverwriteChannel(const FState& State, FString& OutReason)
{
	const FString Label = DreamTextureReflow::GetChannelLabel(State.Channel);
	switch (State.Presence)
	{
	case EPresence::Missing:
		OutReason = FText::Format(NSLOCTEXT("DreamTextureReflowPlanner", "ReasonMissingChannel", "The material has no {0} texture, so there is no parameter to change and nowhere for Overwrite in place to put one. Use Create new to add it."), FText::FromString(Label)).ToString();
		return false;
	case EPresence::InOtherPacked:
		OutReason = FText::Format(NSLOCTEXT("DreamTextureReflowPlanner", "ReasonOtherPacked", "{0} is in a packed texture whose layout is not the standard ORM; the plugin will not change it."), FText::FromString(Label)).ToString();
		return false;
	default:
		break;
	}
	if (State.Parameters.Num() == 0)
	{
		OutReason = FText::Format(NSLOCTEXT("DreamTextureReflowPlanner", "ReasonNotParameter", "The {0} texture ({1}) is written directly into the master material's graph and is not a parameter, so the instance cannot change it."), FText::FromString(Label), FText::FromString(State.TextureName)).ToString();
		return false;
	}
	return true;
}

FDreamTextureReflowInPlacePlan FDreamTextureReflowPlanner::PlanInPlace(const TArray<FState>& States, const TSet<FString>& InSelection, EDreamTextureReflowMode Mode)
{
	FDreamTextureReflowInPlacePlan Plan;
	const TSet<FString> Selection = Normalize(States, InSelection, Mode);

	TSet<FString> HandledGroups;
	for (const FString& Channel : DreamTextureReflow::GetChannels())
	{
		if (!Selection.Contains(Channel))
		{
			continue;
		}
		const FState* State = FindState(States, Channel);
		if (State == nullptr)
		{
			continue;
		}

		FString Why;
		if (!CanOverwriteChannel(*State, Why))
		{
			Plan.Skipped.Add(Why);
			continue;
		}

		FDreamTextureReflowOverwrite Item;
		Item.Parameters = State->Parameters;
		Item.OldTexturePath = State->TexturePath;
		if (State->Presence == EPresence::InStandardORM)
		{
			// 整张换：组里每个通道都勾了才行。Normalize 保证了这一点，这里再确认一遍。
			bool bAllChecked = true;
			for (const FString& Tied : State->TiedChannels)
			{
				bAllChecked &= Selection.Contains(Tied);
			}
			if (!bAllChecked)
			{
				Plan.Skipped.Add(FText::Format(NSLOCTEXT("DreamTextureReflowPlanner", "SkippedTiedToORM", "{0} is in the ORM texture; replacing the whole texture would also change channels that are not checked, so it was left alone."), FText::FromString(DreamTextureReflow::GetChannelLabel(Channel))).ToString());
				continue;
			}
			if (HandledGroups.Contains(State->TexturePath))
			{
				continue;
			}
			HandledGroups.Add(State->TexturePath);
			Item.ImportChannel = DreamTextureChannel::ORM;
			Item.Covers = State->TiedChannels;
		}
		else
		{
			Item.ImportChannel = Channel;
			Item.Covers = { Channel };
		}
		Plan.Items.Add(MoveTemp(Item));
	}

	if (Plan.Items.Num() == 0)
	{
		Plan.Error = Plan.Skipped.Num() > 0
			? NSLOCTEXT("DreamTextureReflowPlanner", "ErrorNoneOverwritable", "None of the checked channels can be overwritten in place (see the reasons below).").ToString()
			: NSLOCTEXT("DreamTextureReflowPlanner", "ErrorNoneChecked", "No channel to overwrite is checked.").ToString();
		return Plan;
	}

	Plan.Options.bExplicitChannels = true;
	for (const FDreamTextureReflowOverwrite& Item : Plan.Items)
	{
		Plan.Options.Channels.AddUnique(Item.ImportChannel);
	}
	// 贴图导进来之后由回流去改用户的实例；不再另建一个。
	Plan.Options.bCreateMaterialInstance = false;
	Plan.bOK = true;
	return Plan;
}
