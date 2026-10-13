// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureMaterialBuilder.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "DreamTextureAssetUtil.h"
#include "DreamTextureLog.h"
#include "Engine/Texture2D.h"
#include "FileHelpers.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionUtils.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"

/*
 * 材质实例 MI_<Slug>。
 *
 * 实例上有两类东西，归属不同，更新时一定要分清：
 *   - 插件管的：四个贴图参数（BaseColor / Normal / ORM / Height）和静态开关 UseNormal / UseORM（以及「没高度图就别开 UsePOM」）。
 *     它们由「这套材质里有哪些贴图」决定，每次导入都按现状校准。
 *   - 用户管的：UVTiling、BaseColorTint、NormalStrength、粗糙度范围、AO 强度……用户在实例上调过的值。
 *     更新时**一个都不碰**——重新导入不该把人家调好的平铺倍数冲掉。
 * 所以更新是「逐个参数比对，不一致才写」，而不是清空重来。
 */

namespace
{
	/** manifest 通道 → 母材质上的贴图参数。其余通道（roughness / metallic / ao）母材质不采样，不进实例。 */
	FName ParameterForChannel(const FString& Channel)
	{
		if (Channel.Equals(DreamTextureChannel::BaseColor, ESearchCase::CaseSensitive))
		{
			return DreamTextureMaterialParam::BaseColor;
		}
		if (Channel.Equals(DreamTextureChannel::Normal, ESearchCase::CaseSensitive))
		{
			return DreamTextureMaterialParam::Normal;
		}
		if (Channel.Equals(DreamTextureChannel::ORM, ESearchCase::CaseSensitive))
		{
			return DreamTextureMaterialParam::ORM;
		}
		if (Channel.Equals(DreamTextureChannel::Height, ESearchCase::CaseSensitive))
		{
			return DreamTextureMaterialParam::Height;
		}
		return NAME_None;
	}

	bool IsPresentAction(EDreamTextureImportAction Action)
	{
		return Action == EDreamTextureImportAction::Created || Action == EDreamTextureImportAction::Updated || Action == EDreamTextureImportAction::Unchanged;
	}

	UTexture2D* BuilderFindTexture(const FString& ObjectPath)
	{
		if (UTexture2D* InMemory = FindObject<UTexture2D>(nullptr, *ObjectPath))
		{
			return InMemory;
		}
		return LoadObject<UTexture2D>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	/** 母材质上这个贴图参数的采样器类型；找不到返回 false。 */
	bool FindSamplerType(const UMaterial* Master, const FName& Parameter, EMaterialSamplerType& OutType)
	{
		for (const UMaterialExpression* Expression : Master->GetExpressions())
		{
			if (const UMaterialExpressionTextureSampleParameter* Sample = Cast<UMaterialExpressionTextureSampleParameter>(Expression))
			{
				if (Sample->ParameterName == Parameter)
				{
					OutType = Sample->SamplerType;
					return true;
				}
			}
		}
		return false;
	}

	/** 实例归属记录的键：为哪套材质建的。记在包的元数据里，与贴图的来源记录同一套做法（理由见 DreamTextureImporter.cpp 的 SourceKey）。 */
	const TCHAR* const InstanceOwnerKey = TEXT("DreamTexture.InstanceMaterialId");

	void WriteInstanceOwner(UObject* Instance, const FString& MaterialId)
	{
#if WITH_METADATA
		Instance->GetOutermost()->GetMetaData().SetValue(Instance, InstanceOwnerKey, *MaterialId);
#endif
	}

	FDreamTextureImportedTexture Fail(FDreamTextureImportedTexture Out, const FString& Note)
	{
		Out.Action = EDreamTextureImportAction::Failed;
		Out.Note = Note;
		return Out;
	}
}

FString FDreamTextureMaterialBuilder::GetInstanceAssetName(const FString& Slug)
{
	return FString::Printf(TEXT("MI_%s"), *Slug);
}

FString FDreamTextureMaterialBuilder::ReadInstanceMaterialId(const UObject* Instance)
{
#if WITH_METADATA
	if (Instance != nullptr)
	{
		if (UPackage* Package = Instance->GetOutermost())
		{
			if (const FString* Value = Package->GetMetaData().FindValue(Instance, InstanceOwnerKey))
			{
				return *Value;
			}
		}
	}
#endif
	return FString();
}

FDreamTextureImportedTexture FDreamTextureMaterialBuilder::CreateOrUpdateInstance(const FDreamTextureManifest& Manifest, const FDreamTextureImportResult& Textures, const FString& ContentRoot, bool bSave)
{
	check(IsInGameThread());

	FDreamTextureImportedTexture Out;
	Out.Action = EDreamTextureImportAction::Skipped;

	if (Manifest.Id.IsEmpty())
	{
		return Fail(Out, NSLOCTEXT("DreamTextureMaterialBuilder", "NoMaterialId", "The manifest has no material id, so the material instance path cannot be determined.").ToString());
	}

	const FString Slug = FDreamTextureImporter::MakeSlug(Manifest);
	const FString Folder = FDreamTextureImporter::GetAssetFolder(Manifest, ContentRoot);
	const FString AssetName = GetInstanceAssetName(Slug);
	const FString PackageName = Folder / AssetName;
	Out.ObjectPath = PackageName + TEXT(".") + AssetName;

	// ---- 1. 这一套里有哪些贴图可用 ----
	// Created / Updated / Unchanged 都算：Unchanged 的贴图这次没动，但它还在工程里，实例照样要引用它。
	TMap<FName, UTexture2D*> Wanted;
	TArray<FString> Warnings;
	for (const FDreamTextureImportedTexture& Entry : Textures.Textures)
	{
		const FName Parameter = ParameterForChannel(Entry.Channel);
		if (Parameter.IsNone() || !IsPresentAction(Entry.Action) || Entry.ObjectPath.IsEmpty())
		{
			continue;
		}
		UTexture2D* Texture = BuilderFindTexture(Entry.ObjectPath);
		if (Texture == nullptr)
		{
			Warnings.Add(FText::Format(NSLOCTEXT("DreamTextureMaterialBuilder", "TextureNotFound", "The {0} texture was not found in the project ({1}), so the instance does not use it."), FText::FromString(Entry.Channel), FText::FromString(Entry.ObjectPath)).ToString());
			continue;
		}
		Wanted.Add(Parameter, Texture);
	}

	if (Wanted.Num() == 0)
	{
		Out.Note = NSLOCTEXT("DreamTextureMaterialBuilder", "NoUsableTextures", "This material set has no textures usable by a material instance (none of BaseColor, Normal, ORM or Height), so no material instance was created.").ToString();
		if (Warnings.Num() > 0)
		{
			Out.Note += TEXT(" ") + FString::Join(Warnings, TEXT(" "));
		}
		return Out;
	}

	// ---- 2. 母材质 ----
	FString MasterError;
	UMaterial* Master = FDreamTextureMasterMaterial::FindOrCreate(ContentRoot, bSave, MasterError);
	if (Master == nullptr)
	{
		return Fail(Out, FText::Format(NSLOCTEXT("DreamTextureMaterialBuilder", "MasterUnavailable", "The master material is unavailable: {0}"), FText::FromString(MasterError)).ToString());
	}

	// 贴图的采样器类型必须与母材质里那个参数的完全一致，否则材质编译报错（运行时退回默认贴图）。
	// 最常见的成因是用户导入后改了贴图的压缩设置或 sRGB。这种贴图不挂上去，并把原因告诉用户——
	// 对应的静态开关随之关掉，材质仍然能用，只是少这一路。
	for (auto It = Wanted.CreateIterator(); It; ++It)
	{
		EMaterialSamplerType SamplerType = SAMPLERTYPE_Color;
		if (!FindSamplerType(Master, It.Key(), SamplerType))
		{
			Warnings.Add(FText::Format(NSLOCTEXT("DreamTextureMaterialBuilder", "NoTextureParameter", "The master material has no texture parameter {0}, so the instance does not use it."), FText::FromString(It.Key().ToString())).ToString());
			It.RemoveCurrent();
			continue;
		}
		const UTexture2D* Texture = It.Value();
		FString Mismatch;
		if (!MaterialExpressionUtils::VerifySamplerType(Texture->GetPathName(), MaterialExpressionUtils::GetSamplerTypeForTexture(Texture), Texture->SRGB, SamplerType, Mismatch))
		{
			Warnings.Add(FText::Format(NSLOCTEXT("DreamTextureMaterialBuilder", "CompressionMismatch", "The compression settings of the {0} texture do not match the master material ({1}), so the instance does not use it. Importing this material set again restores the settings."), FText::FromString(It.Key().ToString()), FText::FromString(Mismatch)).ToString());
			It.RemoveCurrent();
		}
	}

	// ---- 3. 实例本体：有就用，没有就建 ----
	UObject* Existing = DreamTextureAssetUtil::FindOrLoad(PackageName, AssetName);
	UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Existing);
	if (Existing != nullptr && Instance == nullptr)
	{
		return Fail(Out, FText::Format(NSLOCTEXT("DreamTextureMaterialBuilder", "PathTaken", "The path {0} is already taken by a {1} asset, so the material instance cannot be placed there."), FText::FromString(Out.ObjectPath), FText::FromString(Existing->GetClass()->GetName())).ToString());
	}

	// 已有的实例得是为这一套材质建的才能动。同名的有两种情形：
	//  - 另一套材质（slug 撞车：名字的 PascalCase 与 id 尾段都一样）——贴图那边撞车时会 Failed，实例这边也不能抢；
	//  - 用户自己建的、碰巧同名的实例（没有记录）——改它等于改用户的东西。
	// 两种都 Failed 并说明，不修改。
	if (Instance != nullptr)
	{
		const FString OwnerId = ReadInstanceMaterialId(Instance);
		if (OwnerId.IsEmpty())
		{
			return Fail(Out, FText::Format(NSLOCTEXT("DreamTextureMaterialBuilder", "NotOurs", "There is already a material instance at {0}, but this plugin did not create it for this material set (it has no provenance record), so it will not be modified. Rename or move it, then import again."), FText::FromString(Out.ObjectPath)).ToString());
		}
		if (!OwnerId.Equals(Manifest.Id, ESearchCase::CaseSensitive))
		{
			return Fail(Out, FText::Format(NSLOCTEXT("DreamTextureMaterialBuilder", "OwnedByOtherSet", "The material instance {0} belongs to another material set ({1}) and cannot be overwritten."), FText::FromString(Out.ObjectPath), FText::FromString(OwnerId)).ToString());
		}
	}

	const bool bCreate = (Instance == nullptr);
	bool bChanged = bCreate;
	if (bCreate)
	{
		UPackage* Package = CreatePackage(*PackageName);
		if (Package == nullptr)
		{
			return Fail(Out, FText::Format(NSLOCTEXT("DreamTextureMaterialBuilder", "CreatePackageFailed", "Could not create the package: {0}"), FText::FromString(PackageName)).ToString());
		}
		Instance = NewObject<UMaterialInstanceConstant>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
		if (Instance == nullptr)
		{
			return Fail(Out, FText::Format(NSLOCTEXT("DreamTextureMaterialBuilder", "CreateInstanceFailed", "Could not create the material instance object: {0}"), FText::FromString(Out.ObjectPath)).ToString());
		}
		WriteInstanceOwner(Instance, Manifest.Id);
		UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Master);
	}
	else if (Instance->Parent == nullptr)
	{
		// 父材质丢了（母材质被删过之类）才补回来。用户特意改挂到别的父材质的，不动——那是人家的定制。
		UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Master);
		bChanged = true;
	}

	// ---- 4. 逐个参数比对，不一致才写 ----
	using namespace DreamTextureMaterialParam;

	for (const FName& Parameter : { BaseColor, Normal, ORM, Height })
	{
		UTexture2D* const* Desired = Wanted.Find(Parameter);
		if (Desired == nullptr)
		{
			// 这一轮没给这张贴图：实例上原有的赋值不撤（它可能是以前导入的，或用户自己换的）。
			continue;
		}
		const bool bSame = UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, Parameter) == *Desired &&
			UMaterialEditingLibrary::IsMaterialInstanceParameterOverridden(Instance, Parameter);
		if (!bSame)
		{
			Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Parameter), *Desired);
			bChanged = true;
		}
	}

	auto SetSwitch = [&Instance, &bChanged](const FName& Parameter, bool bValue)
	{
		const bool bSame = UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Instance, Parameter) == bValue &&
			UMaterialEditingLibrary::IsMaterialInstanceParameterOverridden(Instance, Parameter);
		if (!bSame)
		{
			// 不在这里逐个更新：一次导入最多改三个开关，最后统一 UpdateMaterialInstance 一次，省掉重复的静态排列重编。
			UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(Instance, Parameter, bValue, EMaterialParameterAssociation::GlobalParameter, /*bUpdateMaterialInstance*/ false);
			bChanged = true;
		}
	};

	// 缺哪张就关掉对应的开关，走标量兜底，而不是留着开关去采母材质的默认贴图冒充。
	SetSwitch(UseNormal, Wanted.Contains(Normal));
	SetSwitch(UseORM, Wanted.Contains(ORM));

	// UsePOM 是用户的选择（贵，默认关）。只有一种情形替他关：实例上根本没有高度图——开着只会采到默认贴图。
	{
		const bool bHasHeight = Wanted.Contains(Height) || UMaterialEditingLibrary::IsMaterialInstanceParameterOverridden(Instance, Height);
		if (!bHasHeight && UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Instance, UsePOM))
		{
			SetSwitch(UsePOM, false);
		}
	}

	// ---- 5. 落盘 ----
	if (bCreate)
	{
		FAssetRegistryModule::AssetCreated(Instance);
	}
	// 要不要存盘不只看这一轮改没改：上一轮保存失败的话，实例还躺在内存里、包是脏的，
	// 这一轮「没有变化」，但它没落盘——这时只看 bChanged 会把没存盘的当成功（重开工程实例就没了）。
	// 没有改动时（bChanged 为假）这里读到的就是包原本的状态；有改动时无论如何都要存，所以读到什么都无所谓。
	const bool bWasDirty = Instance->GetOutermost()->IsDirty();
	if (bChanged)
	{
		UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
		Instance->MarkPackageDirty();
	}
	if (bSave && (bChanged || bWasDirty))
	{
		TArray<UPackage*> ToSave;
		ToSave.Add(Instance->GetOutermost());
		if (!UEditorLoadingAndSavingUtils::SavePackages(ToSave, /*bOnlyDirty*/ false))
		{
			return Fail(Out, FText::Format(NSLOCTEXT("DreamTextureMaterialBuilder", "SaveFailed", "The material instance was created, but saving it to disk failed: {0} (see the log)"), FText::FromString(PackageName)).ToString());
		}
	}

	Out.Action = bCreate ? EDreamTextureImportAction::Created : (bChanged ? EDreamTextureImportAction::Updated : EDreamTextureImportAction::Unchanged);
	if (Warnings.Num() > 0)
	{
		Out.Note = FString::Join(Warnings, TEXT(" "));
	}
	return Out;
}
