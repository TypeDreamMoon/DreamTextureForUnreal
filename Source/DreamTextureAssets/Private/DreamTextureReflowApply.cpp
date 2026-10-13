// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureReflow.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "DreamTextureLog.h"
#include "DreamTextureMaterialBuilder.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Texture2D.h"
#include "FileHelpers.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionUtils.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "DreamTextureReflowApply"

/*
 * 原地覆盖：把用户选中的那个材质实例上的贴图参数换成刚导进来的新贴图。
 *
 * 动的是「贴图参数的覆盖值」。唯一的例外是**插件自己建的实例**上的 UseNormal / UseORM 两个静态开关：
 * 实例建出来时没有法线 / ORM，插件把对应开关关掉了（走标量兜底），这时只换参数里的贴图，材质根本不会去采它，
 * 用户会看到「覆盖成功了、材质没变化」。所以换 Normal / ORM 时顺手把对应开关打开。
 * UsePOM 不碰：它是用户的选择（贵，默认关）。用户自己的母材质上哪怕有同名的开关也不碰——不认识的材质不替它做主。
 *
 * 实例是用户的东西，所以整个改动包进一个事务，Ctrl+Z 能原样退回；
 * 新导入的贴图留在 ImportRoot 下（它们是新资产，撤销不会删它们，也不需要删）。
 */

namespace
{
	/** 根材质里这个贴图参数的采样器类型。参数在材质函数 / 层里时找不到，返回 false（此时不做核对）。 */
	bool ApplyFindSamplerType(const UMaterial* Root, const FName& Parameter, EMaterialSamplerType& OutType)
	{
		if (Root == nullptr)
		{
			return false;
		}
		for (const UMaterialExpression* Expression : Root->GetExpressions())
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

	bool ApplyIsPresentAction(EDreamTextureImportAction Action)
	{
		return Action == EDreamTextureImportAction::Created || Action == EDreamTextureImportAction::Updated || Action == EDreamTextureImportAction::Unchanged;
	}

	/** 插件自己建的实例：根材质是插件母材质，或实例的包元数据里记着它是为哪套材质建的。判法与 Describe 一致。 */
	bool IsPluginInstance(const UMaterialInstanceConstant* Instance)
	{
		if (!FDreamTextureMaterialBuilder::ReadInstanceMaterialId(Instance).IsEmpty())
		{
			return true;
		}
		const UMaterial* Root = Instance->GetMaterial();
		return Root != nullptr && Root->GetName().Equals(FDreamTextureMasterMaterial::GetAssetName(), ESearchCase::CaseSensitive);
	}

	/** 这个实例的母材质链上有没有这个静态开关。 */
	bool HasStaticSwitch(const UMaterialInstanceConstant* Instance, const FName& Switch)
	{
		TMap<FMaterialParameterInfo, FMaterialParameterMetadata> Switches;
		Instance->GetAllParametersOfType(EMaterialParameterType::StaticSwitch, Switches);
		return Switches.Contains(FMaterialParameterInfo(Switch));
	}

	UTexture2D* LoadImported(const FString& ObjectPath)
	{
		if (UTexture2D* InMemory = FindObject<UTexture2D>(nullptr, *ObjectPath))
		{
			return InMemory;
		}
		return LoadObject<UTexture2D>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
}

FDreamTextureReflowApplyResult FDreamTextureReflowPlanner::ApplyInPlace(UMaterialInstanceConstant* Instance, const TArray<FDreamTextureReflowOverwrite>& Items,
	const FDreamTextureImportResult& Imported, bool bSave)
{
	check(IsInGameThread());

	FDreamTextureReflowApplyResult Result;
	if (Instance == nullptr)
	{
		Result.Error = NSLOCTEXT("DreamTextureReflowApply", "InstanceGone", "The material instance to modify no longer exists (it was unloaded or deleted).").ToString();
		return Result;
	}

	const bool bCanTransact = GEditor != nullptr && GEditor->Trans != nullptr;
	TUniquePtr<FScopedTransaction> Transaction;
	if (bCanTransact)
	{
		Transaction = MakeUnique<FScopedTransaction>(LOCTEXT("InPlaceTransaction", "DreamTexture: Reflow - overwrite material instance textures"));
	}
	Instance->SetFlags(RF_Transactional);
	Instance->Modify();

	const UMaterial* Root = Instance->GetMaterial();
	const bool bPluginInstance = IsPluginInstance(Instance);
	int32 NumChanged = 0;
	TSet<FName> SwitchesToEnable;

	for (const FDreamTextureReflowOverwrite& Item : Items)
	{
		const FDreamTextureImportedTexture* Entry = Imported.Textures.FindByPredicate([&Item](const FDreamTextureImportedTexture& Texture)
		{
			return Texture.Channel.Equals(Item.ImportChannel, ESearchCase::CaseSensitive) && ApplyIsPresentAction(Texture.Action) && !Texture.ObjectPath.IsEmpty();
		});
		UTexture2D* Texture = Entry != nullptr ? LoadImported(Entry->ObjectPath) : nullptr;
		if (Texture == nullptr)
		{
			Result.Skipped.Add(FText::Format(NSLOCTEXT("DreamTextureReflowApply", "NoImportedTexture", "{0}: the backend's result has no successfully imported {1} texture."), FText::FromString(DreamTextureReflow::GetChannelLabel(Item.Covers.Num() > 0 ? Item.Covers[0] : Item.ImportChannel)), FText::FromString(Item.ImportChannel)).ToString());
			continue;
		}

		for (const FName& Parameter : Item.Parameters)
		{
			// 贴图的采样器类型必须与参数的一致，否则材质编译报错。后端给的贴图设置是对的，
			// 但用户的参数可能是别的采样器（把粗糙度当 Color 读之类）；不一致就不改，而不是弄坏他的材质。
			EMaterialSamplerType SamplerType = SAMPLERTYPE_Color;
			if (ApplyFindSamplerType(Root, Parameter, SamplerType))
			{
				FString Mismatch;
				if (!MaterialExpressionUtils::VerifySamplerType(Texture->GetPathName(), MaterialExpressionUtils::GetSamplerTypeForTexture(Texture), Texture->SRGB, SamplerType, Mismatch))
				{
					Result.Skipped.Add(FText::Format(NSLOCTEXT("DreamTextureReflowApply", "ParameterSkipped", "Parameter {0} was not changed: the new texture's compression settings do not match its sampler type ({1})."), FText::FromString(Parameter.ToString()), FText::FromString(Mismatch)).ToString());
					continue;
				}
			}

			Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Parameter), Texture);
			Result.Changed.Add(FString::Printf(TEXT("%s → %s"), *Parameter.ToString(), *Texture->GetName()));
			++NumChanged;

			if (bPluginInstance)
			{
				if (Parameter == DreamTextureMaterialParam::Normal)
				{
					SwitchesToEnable.Add(DreamTextureMaterialParam::UseNormal);
				}
				else if (Parameter == DreamTextureMaterialParam::ORM)
				{
					SwitchesToEnable.Add(DreamTextureMaterialParam::UseORM);
				}
			}
		}
	}

	Result.bOK = true;
	Result.bTransacted = bCanTransact && NumChanged > 0;

	if (NumChanged == 0)
	{
		// 一个参数都没改：别在撤销栈里留空记录。
		if (Transaction.IsValid())
		{
			Transaction->Cancel();
		}
		return Result;
	}

	// 换了 Normal / ORM 的插件实例：对应的开关关着就打开，否则材质不会采这张新贴图。
	for (const FName& Switch : SwitchesToEnable)
	{
		if (HasStaticSwitch(Instance, Switch) && !UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Instance, Switch))
		{
			// 不在这里逐个更新：最后统一 UpdateMaterialInstance 一次，省掉重复的静态排列重编。
			UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(Instance, Switch, true, EMaterialParameterAssociation::GlobalParameter, /*bUpdateMaterialInstance*/ false);
			Result.Changed.Add(FText::Format(NSLOCTEXT("DreamTextureReflowApply", "SwitchTurnedOn", "{0} -> on (otherwise the material would not sample the new texture)"), FText::FromString(Switch.ToString())).ToString());
		}
	}

	UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
	Instance->MarkPackageDirty();

	if (bSave)
	{
		TArray<UPackage*> ToSave;
		ToSave.Add(Instance->GetOutermost());
		if (!UEditorLoadingAndSavingUtils::SavePackages(ToSave, /*bOnlyDirty*/ false))
		{
			// 改动已经生效且在撤销栈里；只是没存到磁盘。不回滚，让用户自己存。
			Result.Skipped.Add(FText::Format(NSLOCTEXT("DreamTextureReflowApply", "SaveFailed", "The material instance was modified, but saving it to disk failed: {0}. Save it manually."), FText::FromString(Instance->GetOutermost()->GetName())).ToString());
		}
	}

	UE_LOG(LogDreamTexture, Log, TEXT("Reflow overwrite in place %s: changed %d parameter(s), skipped %d"), *Instance->GetPathName(), NumChanged, Result.Skipped.Num());
	return Result;
}

#undef LOCTEXT_NAMESPACE
