// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureMaterialBuilder.h"
#include "DreamTextureTestUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionStaticBoolParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "PackageTools.h"

/** 母材质 / 材质实例自动化测试共用的小工具（接在 DreamTextureTestUtils 之上）。 */
namespace DreamTextureTestUtils
{
	/** 母材质里按类型分好的参数表达式，键是参数名。 */
	struct FMasterParameters
	{
		TMap<FName, UMaterialExpressionScalarParameter*> Scalars;
		TMap<FName, UMaterialExpressionVectorParameter*> Vectors;
		TMap<FName, UMaterialExpressionTextureSampleParameter*> Textures;
		TMap<FName, UMaterialExpressionStaticBoolParameter*> Switches;
	};

	inline FMasterParameters CollectParameters(const UMaterial* Material)
	{
		FMasterParameters Out;
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			if (UMaterialExpressionTextureSampleParameter* Texture = Cast<UMaterialExpressionTextureSampleParameter>(Expression))
			{
				Out.Textures.Add(Texture->ParameterName, Texture);
			}
			else if (UMaterialExpressionStaticBoolParameter* Switch = Cast<UMaterialExpressionStaticBoolParameter>(Expression))
			{
				Out.Switches.Add(Switch->ParameterName, Switch);
			}
			else if (UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(Expression))
			{
				Out.Scalars.Add(Scalar->ParameterName, Scalar);
			}
			else if (UMaterialExpressionVectorParameter* Vector = Cast<UMaterialExpressionVectorParameter>(Expression))
			{
				Out.Vectors.Add(Vector->ParameterName, Vector);
			}
		}
		return Out;
	}

	/**
	 * 每个贴图参数应当配的采样器类型。依据：导入器给各通道设的压缩设置 + MaterialExpressionUtils::GetSamplerTypeForTexture。
	 * 测试里独立写一份，不去读生产代码里的表——表写错了，测试才抓得到。
	 */
	inline EMaterialSamplerType ExpectedSamplerOf(const FName& Parameter)
	{
		if (Parameter == TEXT("BaseColor")) return SAMPLERTYPE_Color;
		if (Parameter == TEXT("Normal")) return SAMPLERTYPE_Normal;
		if (Parameter == TEXT("ORM")) return SAMPLERTYPE_Masks;
		if (Parameter == TEXT("Height")) return SAMPLERTYPE_LinearGrayscale;
		return SAMPLERTYPE_MAX;
	}

	/** 从一个表达式往输入方向（上游）走，收集沿途所有表达式。 */
	inline void CollectUpstream(UMaterialExpression* Start, TSet<UMaterialExpression*>& Visited)
	{
		if (Start == nullptr || Visited.Contains(Start))
		{
			return;
		}
		Visited.Add(Start);
		for (FExpressionInputIterator It{ Start }; It; ++It)
		{
			CollectUpstream(It->Expression, Visited);
		}
	}

	/** 某个材质输出属性的上游里有没有叫这个名字的参数（任何类型）。 */
	inline bool UpstreamHasParameter(const TSet<UMaterialExpression*>& Upstream, FName ParameterName)
	{
		for (const UMaterialExpression* Expression : Upstream)
		{
			if (Expression->HasAParameterName() && Expression->GetParameterName() == ParameterName)
			{
				return true;
			}
		}
		return false;
	}

	/** 某个材质属性往上游走能到的全部表达式；属性没连则为空。 */
	inline TSet<UMaterialExpression*> UpstreamOfProperty(UMaterial* Material, EMaterialProperty Property)
	{
		TSet<UMaterialExpression*> Out;
		if (FExpressionInput* Input = Material->GetExpressionInputForProperty(Property))
		{
			CollectUpstream(Input->Expression, Out);
		}
		return Out;
	}

	/** 母材质所在的包卸掉（连带里面的实例等），之后只能从磁盘读。 */
	inline bool UnloadAssetPackages(const TArray<UObject*>& Assets)
	{
		TArray<UPackage*> Packages;
		for (const UObject* Asset : Assets)
		{
			if (Asset != nullptr)
			{
				Packages.AddUnique(Asset->GetOutermost());
			}
		}
		FText Error;
		return UPackageTools::UnloadPackages(Packages, Error, /*bUnloadDirtyPackages*/ true);
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
