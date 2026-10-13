// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureMaterialBuilder.h"
#include "DreamTextureMaterialTestUtils.h"
#include "FileHelpers.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionUtils.h"
#include "Materials/MaterialFunction.h"

using namespace DreamTextureTestUtils;

/*
 * 母材质的无头测试（-nullrhi 能跑）。
 *
 * 这里管的是「图对不对」：参数齐不齐、类型和采样器类型对不对、连线通不通、版本与就地重建。
 * 「着色器编不编得过」需要真实 RHI，归 DreamTexture.Render.MasterMaterial。
 */

namespace
{
	/** 母材质上要有的参数，按类型。与 DreamTextureMaterialBuilder.h 里 DreamTextureMaterialParam 逐个对应——测试里再写一遍是故意的。 */
	const TCHAR* const ExpectedScalars[] = {
		TEXT("UVTiling"), TEXT("NormalStrength"), TEXT("RoughnessMin"), TEXT("RoughnessMax"),
		TEXT("AOStrength"), TEXT("HeightRatio"), TEXT("Roughness"), TEXT("Metallic"),
	};
	const TCHAR* const ExpectedVectors[] = { TEXT("BaseColorTint") };
	const TCHAR* const ExpectedTextures[] = { TEXT("BaseColor"), TEXT("Normal"), TEXT("ORM"), TEXT("Height") };
	const TCHAR* const ExpectedSwitches[] = { TEXT("UseNormal"), TEXT("UseORM"), TEXT("UsePOM") };

	template <class TParameterMap, size_t N>
	void CheckNames(FAutomationTestBase& Test, const TCHAR* Kind, const TParameterMap& Map, const TCHAR* const (&Expected)[N])
	{
		Test.TestEqual(FString::Printf(TEXT("%s 参数个数"), Kind), Map.Num(), static_cast<int32>(N));
		for (const TCHAR* Name : Expected)
		{
			Test.TestTrue(FString::Printf(TEXT("有 %s 参数 %s"), Kind, Name), Map.Contains(FName(Name)));
		}
	}
}

// ---------------------------------------------------------------------------
// 图：参数、采样器类型、连线
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureMasterGraphTest, "DreamTexture.Assets.Material.MasterGraph", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureMasterGraphTest::RunTest(const FString& Parameters)
{
	FScopedTestContent Content;
	FString Error;
	UMaterial* Master = FDreamTextureMasterMaterial::FindOrCreate(Content.Root, /*bSave*/ true, Error);
	if (!TestNotNull(FString::Printf(TEXT("生成母材质（%s）"), *Error), Master))
	{
		return false;
	}

	TestEqual(TEXT("资产名"), FString(FDreamTextureMasterMaterial::GetAssetName()), FString(TEXT("M_DreamTexture_Master")));
	TestEqual(TEXT("对象路径"), Master->GetPathName(), FDreamTextureMasterMaterial::GetObjectPath(Content.Root));
	TestEqual(TEXT("落在内容根下"), FDreamTextureMasterMaterial::GetObjectPath(Content.Root), Content.Root / TEXT("M_DreamTexture_Master.M_DreamTexture_Master"));
	TestTrue(TEXT("已存盘"), FPackageName::DoesPackageExist(Master->GetOutermost()->GetName()));

	// 母材质用的是传统材质：不写 Substrate 版本，宿主开着 Substrate 时由引擎转换。
	TestEqual(TEXT("材质域是 Surface"), static_cast<int32>(Master->MaterialDomain), static_cast<int32>(MD_Surface));
	TestEqual(TEXT("不透明"), static_cast<int32>(Master->BlendMode.GetValue()), static_cast<int32>(BLEND_Opaque));
	TestFalse(TEXT("不用 MaterialAttributes"), Master->bUseMaterialAttributes != 0);

	// ---- 参数齐全、类型对 ----
	const FMasterParameters Params = CollectParameters(Master);
	CheckNames(*this, TEXT("标量"), Params.Scalars, ExpectedScalars);
	CheckNames(*this, TEXT("向量"), Params.Vectors, ExpectedVectors);
	CheckNames(*this, TEXT("贴图"), Params.Textures, ExpectedTextures);
	CheckNames(*this, TEXT("静态开关"), Params.Switches, ExpectedSwitches);

	// 头文件里的常量必须和这里写的名字一致：改名等于让所有已有的材质实例丢参数。
	TestEqual(TEXT("常量 BaseColor"), DreamTextureMaterialParam::BaseColor, FName(TEXT("BaseColor")));
	TestEqual(TEXT("常量 ORM"), DreamTextureMaterialParam::ORM, FName(TEXT("ORM")));
	TestEqual(TEXT("常量 UsePOM"), DreamTextureMaterialParam::UsePOM, FName(TEXT("UsePOM")));
	TestEqual(TEXT("常量 UVTiling"), DreamTextureMaterialParam::UVTiling, FName(TEXT("UVTiling")));

	// ---- 默认值 ----
	auto Scalar = [&Params](const TCHAR* Name) { const auto* Found = Params.Scalars.Find(FName(Name)); return Found != nullptr ? (*Found)->DefaultValue : -999.f; };
	TestEqual(TEXT("UVTiling 默认 1"), Scalar(TEXT("UVTiling")), 1.f);
	TestEqual(TEXT("NormalStrength 默认 1"), Scalar(TEXT("NormalStrength")), 1.f);
	TestEqual(TEXT("RoughnessMin 默认 0"), Scalar(TEXT("RoughnessMin")), 0.f);
	TestEqual(TEXT("RoughnessMax 默认 1"), Scalar(TEXT("RoughnessMax")), 1.f);
	TestEqual(TEXT("AOStrength 默认 1"), Scalar(TEXT("AOStrength")), 1.f);
	TestEqual(TEXT("Roughness 兜底默认 0.5"), Scalar(TEXT("Roughness")), 0.5f);
	TestEqual(TEXT("Metallic 兜底默认 0"), Scalar(TEXT("Metallic")), 0.f);
	TestTrue(TEXT("HeightRatio 默认是个正数"), Scalar(TEXT("HeightRatio")) > 0.f);
	if (const auto* Tint = Params.Vectors.Find(FName(TEXT("BaseColorTint"))))
	{
		TestTrue(TEXT("BaseColorTint 默认白色"), (*Tint)->DefaultValue.Equals(FLinearColor::White));
	}
	auto Switch = [&Params](const TCHAR* Name) { const auto* Found = Params.Switches.Find(FName(Name)); return Found != nullptr && (*Found)->DefaultValue != 0; };
	TestTrue(TEXT("UseNormal 默认开"), Switch(TEXT("UseNormal")));
	TestTrue(TEXT("UseORM 默认开"), Switch(TEXT("UseORM")));
	TestFalse(TEXT("UsePOM 默认关（贵）"), Switch(TEXT("UsePOM")));

	// ---- 采样器类型：与贴图的压缩设置对上，否则材质编译报错 ----
	for (const TCHAR* Name : ExpectedTextures)
	{
		const UMaterialExpressionTextureSampleParameter* const* Found = Params.Textures.Find(FName(Name));
		if (Found == nullptr)
		{
			continue;
		}
		const UMaterialExpressionTextureSampleParameter* Expression = *Found;
		TestEqual(FString::Printf(TEXT("%s 的采样器类型"), Name), static_cast<int32>(Expression->SamplerType.GetValue()), static_cast<int32>(ExpectedSamplerOf(FName(Name))));

		const UTexture* Default = Expression->Texture;
		if (TestNotNull(FString::Printf(TEXT("%s 有默认贴图"), Name), Default))
		{
			TestTrue(FString::Printf(TEXT("%s 的默认贴图在 /Engine 下（%s）"), Name, *Default->GetPathName()), Default->GetPathName().StartsWith(TEXT("/Engine/")));

			FString Mismatch;
			const bool bMatches = MaterialExpressionUtils::VerifySamplerType(Default->GetPathName(), MaterialExpressionUtils::GetSamplerTypeForTexture(Default), Default->SRGB,
				Expression->SamplerType, Mismatch);
			TestTrue(FString::Printf(TEXT("%s 的默认贴图满足采样器类型检查（%s）"), Name, *Mismatch), bMatches);
		}
	}

	// ---- 主要属性都连上了，并且连到该连的参数 ----
	{
		const TSet<UMaterialExpression*> BaseColorUp = UpstreamOfProperty(Master, MP_BaseColor);
		const TSet<UMaterialExpression*> NormalUp = UpstreamOfProperty(Master, MP_Normal);
		const TSet<UMaterialExpression*> RoughnessUp = UpstreamOfProperty(Master, MP_Roughness);
		const TSet<UMaterialExpression*> MetallicUp = UpstreamOfProperty(Master, MP_Metallic);
		const TSet<UMaterialExpression*> AOUp = UpstreamOfProperty(Master, MP_AmbientOcclusion);

		TestTrue(TEXT("BaseColor 连上了"), BaseColorUp.Num() > 0);
		TestTrue(TEXT("Normal 连上了"), NormalUp.Num() > 0);
		TestTrue(TEXT("Roughness 连上了"), RoughnessUp.Num() > 0);
		TestTrue(TEXT("Metallic 连上了"), MetallicUp.Num() > 0);
		TestTrue(TEXT("AmbientOcclusion 连上了"), AOUp.Num() > 0);

		auto Reaches = [this](const TCHAR* What, const TSet<UMaterialExpression*>& Up, std::initializer_list<const TCHAR*> Names)
		{
			for (const TCHAR* Name : Names)
			{
				TestTrue(FString::Printf(TEXT("%s 的上游有 %s"), What, Name), UpstreamHasParameter(Up, FName(Name)));
			}
		};
		Reaches(TEXT("BaseColor"), BaseColorUp, { TEXT("BaseColor"), TEXT("BaseColorTint"), TEXT("UVTiling"), TEXT("UsePOM") });
		Reaches(TEXT("Normal"), NormalUp, { TEXT("Normal"), TEXT("NormalStrength"), TEXT("UseNormal"), TEXT("UVTiling") });
		Reaches(TEXT("Roughness"), RoughnessUp, { TEXT("ORM"), TEXT("RoughnessMin"), TEXT("RoughnessMax"), TEXT("Roughness"), TEXT("UseORM") });
		Reaches(TEXT("Metallic"), MetallicUp, { TEXT("ORM"), TEXT("Metallic"), TEXT("UseORM") });
		Reaches(TEXT("AO"), AOUp, { TEXT("ORM"), TEXT("AOStrength"), TEXT("UseORM") });

		// 开关只管各自该管的：UseNormal 不该出现在粗糙度的上游，反过来也一样。
		TestFalse(TEXT("Roughness 的上游没有 UseNormal"), UpstreamHasParameter(RoughnessUp, TEXT("UseNormal")));
		TestFalse(TEXT("Normal 的上游没有 UseORM"), UpstreamHasParameter(NormalUp, TEXT("UseORM")));
		TestFalse(TEXT("BaseColor 的上游没有 ORM"), UpstreamHasParameter(BaseColorUp, TEXT("ORM")));
	}

	// ---- UV 链与视差遮蔽 ----
	{
		const TSet<UMaterialExpression*> BaseColorUp = UpstreamOfProperty(Master, MP_BaseColor);

		bool bPomCall = false;
		for (const UMaterialExpression* Expression : BaseColorUp)
		{
			const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
			if (Call == nullptr || Call->MaterialFunction == nullptr || !Call->MaterialFunction->GetPathName().Contains(TEXT("ParallaxOcclusionMapping")))
			{
				continue;
			}
			bPomCall = true;

			// 函数的几个输入都得接着，尤其是高度图：连的得是 Height 这个参数。
			TSet<UMaterialExpression*> HeightUp;
			for (const FFunctionExpressionInput& Input : Call->FunctionInputs)
			{
				if (Input.ExpressionInput != nullptr && Input.ExpressionInput->InputName == FName(TEXT("Heightmap Texture")))
				{
					CollectUpstream(Input.Input.Expression, HeightUp);
				}
			}
			TestTrue(TEXT("视差函数的高度图输入连着 Height 参数"), UpstreamHasParameter(HeightUp, TEXT("Height")));
			bool bObjectParameter = false;
			for (const UMaterialExpression* Upstream : HeightUp)
			{
				bObjectParameter |= Upstream->IsA<UMaterialExpressionTextureObjectParameter>();
			}
			TestTrue(TEXT("Height 是贴图对象参数（输出贴图本身，不先采样）"), bObjectParameter);

			TSet<UMaterialExpression*> UVUp;
			for (const FFunctionExpressionInput& Input : Call->FunctionInputs)
			{
				if (Input.ExpressionInput != nullptr && Input.ExpressionInput->InputName == FName(TEXT("UVs")))
				{
					CollectUpstream(Input.Input.Expression, UVUp);
				}
			}
			TestTrue(TEXT("视差函数的 UV 输入来自 UVTiling"), UpstreamHasParameter(UVUp, TEXT("UVTiling")));
			TestTrue(TEXT("视差函数的 HeightRatio 输入存在"), UpstreamHasParameter(BaseColorUp, TEXT("HeightRatio")));
		}
		TestTrue(TEXT("UsePOM 真的接着引擎的 ParallaxOcclusionMapping 函数（不是空开关）"), bPomCall);
	}

	// ---- 版本 ----
	TestEqual(TEXT("记录的图版本等于当前版本"), FDreamTextureMasterMaterial::ReadRecordedVersion(Master), FDreamTextureMasterMaterial::GraphVersion);
	TestFalse(TEXT("存盘之后不脏"), Master->GetOutermost()->IsDirty());

	// 卸载再从磁盘读：版本记录是存在包里的，不是只活在内存里。
	const FString ObjectPath = Master->GetPathName();
	if (TestTrue(TEXT("卸载母材质所在的包"), UnloadAssetPackages({ Master })))
	{
		TestNull(TEXT("卸载后内存里没有"), FindObject<UMaterial>(nullptr, *ObjectPath));

		FString ReloadError;
		UMaterial* Reloaded = FDreamTextureMasterMaterial::FindOrCreate(Content.Root, true, ReloadError);
		if (TestNotNull(FString::Printf(TEXT("从磁盘读回（%s）"), *ReloadError), Reloaded))
		{
			TestEqual(TEXT("读盘后版本还在"), FDreamTextureMasterMaterial::ReadRecordedVersion(Reloaded), FDreamTextureMasterMaterial::GraphVersion);
			TestFalse(TEXT("读盘后再调一次没有标脏"), Reloaded->GetOutermost()->IsDirty());
			TestEqual(TEXT("读盘后参数数量不变"), CollectParameters(Reloaded).Scalars.Num(), static_cast<int32>(UE_ARRAY_COUNT(ExpectedScalars)));
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// 版本：同版本什么都不做；低版本就地重建，实例不断
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureMasterVersionTest, "DreamTexture.Assets.Material.MasterVersion", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureMasterVersionTest::RunTest(const FString& Parameters)
{
	FScopedTestContent Content;
	FString Error;
	UMaterial* Master = FDreamTextureMasterMaterial::FindOrCreate(Content.Root, true, Error);
	if (!TestNotNull(FString::Printf(TEXT("生成母材质（%s）"), *Error), Master))
	{
		return false;
	}
	const int32 ExpressionCount = UMaterialEditingLibrary::GetNumMaterialExpressions(Master);
	TestTrue(TEXT("有一批表达式"), ExpressionCount > 20);

	// ---- 同版本再调：同一个对象、不标脏、什么都没动 ----
	{
		TestFalse(TEXT("首次生成并存盘后不脏"), Master->GetOutermost()->IsDirty());
		TArray<UMaterialExpression*> Before(Master->GetExpressions());

		FString SecondError;
		UMaterial* Again = FDreamTextureMasterMaterial::FindOrCreate(Content.Root, true, SecondError);
		TestTrue(FString::Printf(TEXT("同版本再调返回同一个对象（%s）"), *SecondError), Again == Master);
		TestFalse(TEXT("同版本再调不标脏"), Master->GetOutermost()->IsDirty());
		TestEqual(TEXT("同版本再调表达式数不变"), UMaterialEditingLibrary::GetNumMaterialExpressions(Master), ExpressionCount);

		const TArray<UMaterialExpression*> After(Master->GetExpressions());
		TestTrue(TEXT("同版本再调表达式对象一个都没换"), Before == After);

		// bSave 关着也一样：判断「要不要重建」跟存不存盘无关。
		UMaterial* NoSave = FDreamTextureMasterMaterial::FindOrCreate(Content.Root, false, SecondError);
		TestTrue(TEXT("bSave 关着同版本也是同一个对象"), NoSave == Master);
		TestFalse(TEXT("bSave 关着同版本也不标脏"), Master->GetOutermost()->IsDirty());
	}

	// ---- 一个材质实例，带着用户的覆盖值，看重建后断不断 ----
	UMaterialInstanceConstant* Instance = nullptr;
	UTexture* NormalOverride = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineMaterials/DefaultNormal.DefaultNormal"));
	{
		const FString Name = TEXT("MI_Probe");
		UPackage* Package = CreatePackage(*(Content.Root / Name));
		Instance = NewObject<UMaterialInstanceConstant>(Package, FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
		if (!TestNotNull(TEXT("建出探针实例"), Instance) || !TestNotNull(TEXT("引擎的 DefaultNormal 在"), NormalOverride))
		{
			return false;
		}
		UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Master);
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(Instance, DreamTextureMaterialParam::UseNormal, false);
		UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, DreamTextureMaterialParam::UVTiling, 3.f);
		UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, DreamTextureMaterialParam::BaseColorTint, FLinearColor(1.f, 0.5f, 0.25f, 1.f));
		UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, DreamTextureMaterialParam::Normal, NormalOverride);
		TestEqual(TEXT("探针：UVTiling 是 3"), UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(Instance, DreamTextureMaterialParam::UVTiling), 3.f);
		TestFalse(TEXT("探针：UseNormal 关"), UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Instance, DreamTextureMaterialParam::UseNormal));
	}

	// ---- 更新的版本（别的、更新的插件建的）：不动 ----
	{
		FDreamTextureMasterMaterial::WriteRecordedVersion(Master, FDreamTextureMasterMaterial::GraphVersion + 1);
		UMaterialExpression* Marker = UMaterialEditingLibrary::CreateMaterialExpression(Master, UMaterialExpressionConstant::StaticClass(), 0, 4000);
		FString NewerError;
		UMaterial* Same = FDreamTextureMasterMaterial::FindOrCreate(Content.Root, false, NewerError);
		TestTrue(TEXT("工程里的版本更新时返回原对象"), Same == Master);
		TestEqual(TEXT("更新的版本不回退"), FDreamTextureMasterMaterial::ReadRecordedVersion(Master), FDreamTextureMasterMaterial::GraphVersion + 1);
		TestTrue(TEXT("更新的版本下图原样保留（标记节点还在）"), Master->GetExpressions().Contains(Marker));
		UMaterialEditingLibrary::DeleteMaterialExpression(Master, Marker);
	}

	// ---- 低版本：就地重建 ----
	{
		TArray<TWeakObjectPtr<UMaterialExpression>> OldExpressions;
		for (UMaterialExpression* Expression : Master->GetExpressions())
		{
			OldExpressions.Add(Expression);
		}

		// 模拟「工程里是旧插件生成的」：版本记成当前的上一版，图里混进一个旧图没有的节点。
		// （GraphVersion 现在是 1，上一版是 0——那是一条**写着 0 的记录**，不是「没有记录」；
		// 没有记录的材质是用户自己的，见 DreamTexture.Assets.Material.MasterForeign。）
		FDreamTextureMasterMaterial::WriteRecordedVersion(Master, FDreamTextureMasterMaterial::GraphVersion - 1);
		TestTrue(TEXT("旧版本也是一条记录"), FDreamTextureMasterMaterial::HasRecordedVersion(Master));
		UMaterialExpression* Junk = UMaterialEditingLibrary::CreateMaterialExpression(Master, UMaterialExpressionConstant::StaticClass(), 0, 4000);
		const TWeakObjectPtr<UMaterialExpression> WeakJunk = Junk;
		TestEqual(TEXT("混进了一个多余节点"), UMaterialEditingLibrary::GetNumMaterialExpressions(Master), ExpressionCount + 1);

		FString RebuildError;
		UMaterial* Rebuilt = FDreamTextureMasterMaterial::FindOrCreate(Content.Root, true, RebuildError);
		TestTrue(FString::Printf(TEXT("低版本重建返回同一个 UMaterial 对象（%s）"), *RebuildError), Rebuilt == Master);
		TestEqual(TEXT("版本更新到当前"), FDreamTextureMasterMaterial::ReadRecordedVersion(Master), FDreamTextureMasterMaterial::GraphVersion);
		TestEqual(TEXT("重建后表达式数回到原样（多余节点没了）"), UMaterialEditingLibrary::GetNumMaterialExpressions(Master), ExpressionCount);
		TestFalse(TEXT("多余节点真的被丢掉了"), WeakJunk.IsValid() && Master->GetExpressions().Contains(Junk));
		TestFalse(TEXT("存盘之后不脏"), Master->GetOutermost()->IsDirty());

		// 旧表达式一个都不该留在新图里：是重建，不是原样放过。
		int32 Kept = 0;
		for (const TWeakObjectPtr<UMaterialExpression>& Old : OldExpressions)
		{
			Kept += (Old.IsValid() && Master->GetExpressions().Contains(Old.Get())) ? 1 : 0;
		}
		TestEqual(TEXT("旧图里的表达式没有一个留在新图里"), Kept, 0);

		// 图的内容还是对的。
		const FMasterParameters Params = CollectParameters(Master);
		TestEqual(TEXT("重建后标量参数仍是 8 个"), Params.Scalars.Num(), static_cast<int32>(UE_ARRAY_COUNT(ExpectedScalars)));
		TestEqual(TEXT("重建后贴图参数仍是 4 个"), Params.Textures.Num(), static_cast<int32>(UE_ARRAY_COUNT(ExpectedTextures)));
		TestEqual(TEXT("重建后静态开关仍是 3 个"), Params.Switches.Num(), static_cast<int32>(UE_ARRAY_COUNT(ExpectedSwitches)));
		TestTrue(TEXT("重建后 BaseColor 仍连着"), UpstreamOfProperty(Master, MP_BaseColor).Num() > 0);
		TestTrue(TEXT("重建后 Roughness 仍连着 UseORM"), UpstreamHasParameter(UpstreamOfProperty(Master, MP_Roughness), TEXT("UseORM")));
	}

	// ---- 实例没断：父材质还是它，用户的覆盖值都在 ----
	{
		TestTrue(TEXT("实例的父材质还是同一个母材质"), Instance->Parent == Master);
		TestEqual(TEXT("用户调的 UVTiling 还是 3"), UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(Instance, DreamTextureMaterialParam::UVTiling), 3.f);
		TestTrue(TEXT("UVTiling 仍是覆盖状态"), UMaterialEditingLibrary::IsMaterialInstanceParameterOverridden(Instance, DreamTextureMaterialParam::UVTiling));

		const FLinearColor Tint = UMaterialEditingLibrary::GetMaterialInstanceVectorParameterValue(Instance, DreamTextureMaterialParam::BaseColorTint);
		TestTrue(TEXT("用户调的 Tint 还在"), Tint.Equals(FLinearColor(1.f, 0.5f, 0.25f, 1.f)));
		TestTrue(TEXT("实例上的贴图覆盖还在"), UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, DreamTextureMaterialParam::Normal) == NormalOverride);

		// 静态开关的覆盖是最容易在重建时丢的：它按参数名 + 表达式 GUID 记。
		TestFalse(TEXT("UseNormal 的关闭还在"), UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Instance, DreamTextureMaterialParam::UseNormal));
		TestTrue(TEXT("UseNormal 仍是覆盖状态"), UMaterialEditingLibrary::IsMaterialInstanceParameterOverridden(Instance, DreamTextureMaterialParam::UseNormal));
	}

	return true;
}

// ---------------------------------------------------------------------------
// 路径被别的资产占着
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureMasterConflictTest, "DreamTexture.Assets.Material.MasterConflict", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureMasterConflictTest::RunTest(const FString& Parameters)
{
	FScopedTestContent Content;

	// 不合法的根：空格进了路径。
	{
		FString Error;
		UMaterial* Material = FDreamTextureMasterMaterial::FindOrCreate(TEXT("/Game/Bad Root"), false, Error);
		TestNull(TEXT("路径不合法时失败"), Material);
		TestFalse(TEXT("失败原因不为空"), Error.IsEmpty());
	}

	// 母材质的路径上已经有别的类型的资产：不动它，报错。
	{
		const FString Name = FDreamTextureMasterMaterial::GetAssetName();
		UPackage* Package = CreatePackage(*(Content.Root / Name));
		UTexture2D* Squatter = NewObject<UTexture2D>(Package, FName(Name), RF_Public | RF_Standalone | RF_Transactional);
		if (!TestNotNull(TEXT("占位的贴图"), Squatter))
		{
			return false;
		}

		FString Error;
		UMaterial* Material = FDreamTextureMasterMaterial::FindOrCreate(Content.Root, false, Error);
		TestNull(TEXT("路径被别的资产占着时失败"), Material);
		TestFalse(TEXT("失败原因不为空"), Error.IsEmpty());
		TestTrue(TEXT("占着的资产没被动"), FindObject<UTexture2D>(nullptr, *(Content.Root / Name + TEXT(".") + Name)) == Squatter);
	}

	return true;
}

// ---------------------------------------------------------------------------
// 路径上是用户自己的材质（没有版本记录）：原样不动，说清原因
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureMasterForeignTest, "DreamTexture.Assets.Material.MasterForeign", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureMasterForeignTest::RunTest(const FString& Parameters)
{
	FScopedTestContent Content;
	const FString Name = FDreamTextureMasterMaterial::GetAssetName();
	const FString PackageName = Content.Root / Name;

	// 用户自己建的、碰巧叫这个名字的材质：一个常量接到底色上，没有任何 DreamTexture 的版本记录。
	UPackage* Package = CreatePackage(*PackageName);
	UMaterial* Mine = NewObject<UMaterial>(Package, FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("用户的材质建得出来"), Mine))
	{
		return false;
	}
	UMaterialExpressionConstant* Marker = Cast<UMaterialExpressionConstant>(UMaterialEditingLibrary::CreateMaterialExpression(Mine, UMaterialExpressionConstant::StaticClass(), -300, 0));
	if (!TestNotNull(TEXT("用户的节点建得出来"), Marker))
	{
		return false;
	}
	Marker->R = 0.123f;
	UMaterialEditingLibrary::ConnectMaterialProperty(Marker, TEXT(""), MP_BaseColor);
	TestFalse(TEXT("准备工作：用户的材质没有版本记录"), FDreamTextureMasterMaterial::HasRecordedVersion(Mine));
	TestEqual(TEXT("准备工作：没有记录时读出来是 0"), FDreamTextureMasterMaterial::ReadRecordedVersion(Mine), 0);
	const int32 ExpressionCountBefore = UMaterialEditingLibrary::GetNumMaterialExpressions(Mine);

	// ---- FindOrCreate：报错，不碰 ----
	FString Error;
	UMaterial* Result = FDreamTextureMasterMaterial::FindOrCreate(Content.Root, /*bSave*/ false, Error);
	TestNull(TEXT("路径上有用户的材质时不返回母材质"), Result);
	TestTrue(FString::Printf(TEXT("原因是中文，并点明不是本插件生成的、没有版本记录（%s）"), *Error), Error.Contains(TEXT("did not generate")) && Error.Contains(TEXT("version record")));
	TestTrue(TEXT("原因里带路径"), Error.Contains(PackageName));

	TestTrue(TEXT("用户的材质对象还在"), FindObject<UMaterial>(nullptr, *(PackageName + TEXT(".") + Name)) == Mine);
	TestEqual(TEXT("用户的表达式一个没少"), UMaterialEditingLibrary::GetNumMaterialExpressions(Mine), ExpressionCountBefore);
	TestTrue(TEXT("用户的常量节点还在"), Mine->GetExpressions().Contains(Marker));
	TestEqual(TEXT("用户的常量值没变"), Marker->R, 0.123f);
	TestTrue(TEXT("底色仍连着用户的常量"), UpstreamOfProperty(Mine, MP_BaseColor).Contains(Marker));
	TestFalse(TEXT("没有被写上版本记录"), FDreamTextureMasterMaterial::HasRecordedVersion(Mine));
	TestEqual(TEXT("没有被加参数节点"), CollectParameters(Mine).Scalars.Num(), 0);

	// ---- 实例这一层也一样：母材质不可用，实例不建，用户的材质照旧 ----
	FDreamTextureManifest Manifest;
	TMap<FString, FString> Files;
	FString LoadError;
	if (TestTrue(FString::Printf(TEXT("读固定素材：%s"), *LoadError), LoadFixture(Manifest, Files, LoadError)))
	{
		FDreamTextureImportRequest Request;
		Request.Manifest = Manifest;
		Request.LocalFiles = Files;
		Request.ContentRoot = Content.Root;
		Request.bSave = false;
		const FDreamTextureImportResult Imported = FDreamTextureImporter::Import(Request);
		if (TestTrue(FString::Printf(TEXT("导入成功（%s）"), *Imported.Error), Imported.bOK))
		{
			const FDreamTextureImportedTexture Instance = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Imported, Content.Root, false);
			TestEqual(TEXT("母材质被占时实例是 Failed"), static_cast<int32>(Instance.Action), static_cast<int32>(EDreamTextureImportAction::Failed));
			TestTrue(FString::Printf(TEXT("Note 点出母材质不可用（%s）"), *Instance.Note), Instance.Note.Contains(TEXT("master material")) && Instance.Note.Contains(TEXT("did not generate")));
			TestNull(TEXT("没有建出实例"), FindObject<UMaterialInstanceConstant>(nullptr, *Instance.ObjectPath));
			TestEqual(TEXT("用户的表达式仍然一个没少"), UMaterialEditingLibrary::GetNumMaterialExpressions(Mine), ExpressionCountBefore);
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// 就地重建中途失败：回到重建之前，不留被掏空的脏包
// ---------------------------------------------------------------------------

namespace
{
	/** 作用域内打开「建到第 N 个节点就失败」的测试注入，离开作用域一定关掉（别让它漏到别的测试里）。 */
	struct FScopedBuildFailure
	{
		explicit FScopedBuildFailure(int32 AfterNodes) { FDreamTextureMasterMaterial::DebugFailAfterNodeCount = AfterNodes; }
		~FScopedBuildFailure() { FDreamTextureMasterMaterial::DebugFailAfterNodeCount = -1; }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureMasterRollbackTest, "DreamTexture.Assets.Material.MasterRollback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureMasterRollbackTest::RunTest(const FString& Parameters)
{
	FScopedTestContent Content;
	FString Error;
	UMaterial* Master = FDreamTextureMasterMaterial::FindOrCreate(Content.Root, /*bSave*/ true, Error);
	if (!TestNotNull(FString::Printf(TEXT("生成母材质（%s）"), *Error), Master))
	{
		return false;
	}
	UPackage* Package = Master->GetOutermost();
	const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	const FDateTime FileStamp = IFileManager::Get().GetTimeStamp(*File);

	// ---- 重建前的样子 ----
	const TArray<UMaterialExpression*> Before(Master->GetExpressions());
	const int32 CountBefore = Before.Num();
	TestTrue(TEXT("有一批表达式"), CountBefore > 20);
	const TSet<UMaterialExpression*> BeforeSet(Before);
	const TSet<UMaterialExpression*> BaseColorBefore = UpstreamOfProperty(Master, MP_BaseColor);
	const TSet<UMaterialExpression*> RoughnessBefore = UpstreamOfProperty(Master, MP_Roughness);

	// 一个带着用户覆盖值的实例，重建失败后它必须原样。
	UPackage* InstancePackage = CreatePackage(*(Content.Root / TEXT("MI_Probe")));
	UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(InstancePackage, TEXT("MI_Probe"), RF_Public | RF_Standalone | RF_Transactional);
	UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Master);
	UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(Instance, DreamTextureMaterialParam::UseORM, false);
	UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, DreamTextureMaterialParam::UVTiling, 5.f);

	// 模拟旧版本插件生成的：版本记成上一版。这一步不该把包弄脏；弄脏了的话先还原，
	// 保证「重建前干净」这个前提是真的，下面才能断言失败后仍然干净。
	FDreamTextureMasterMaterial::WriteRecordedVersion(Master, FDreamTextureMasterMaterial::GraphVersion - 1);
	if (Package->IsDirty())
	{
		Package->ClearDirtyFlag();
	}
	TestFalse(TEXT("准备工作：重建前母材质包是干净的"), Package->IsDirty());

	// ---- 重建建到第 12 个节点失败 ----
	{
		const FScopedBuildFailure Failure(12);
		FString FailError;
		UMaterial* Result = FDreamTextureMasterMaterial::FindOrCreate(Content.Root, /*bSave*/ true, FailError);
		TestNull(TEXT("重建中途失败时返回空"), Result);
		TestTrue(FString::Printf(TEXT("原因是中文并带上失败的说明（%s）"), *FailError), FailError.Contains(TEXT("building the master material graph")) && FailError.Contains(TEXT("injected by a test")));
	}

	TestTrue(TEXT("母材质对象还是原来那个"), FindObject<UMaterial>(nullptr, *FDreamTextureMasterMaterial::GetObjectPath(Content.Root)) == Master);
	TestEqual(TEXT("表达式数量回到重建前"), UMaterialEditingLibrary::GetNumMaterialExpressions(Master), CountBefore);
	const TArray<UMaterialExpression*> After(Master->GetExpressions());
	bool bSameNodes = (After.Num() == Before.Num());
	for (UMaterialExpression* Expression : After)
	{
		bSameNodes &= BeforeSet.Contains(Expression);
	}
	TestTrue(TEXT("留下的全是旧节点——这一轮新建的半成品一个不剩"), bSameNodes);
	TestTrue(TEXT("旧节点没有被标成垃圾（对象仍可用）"), After.Num() > 0 && IsValid(After[0]));

	TestTrue(TEXT("底色仍连在旧图上"), UpstreamOfProperty(Master, MP_BaseColor).Num() == BaseColorBefore.Num() && UpstreamOfProperty(Master, MP_BaseColor).Includes(BaseColorBefore));
	TestTrue(TEXT("粗糙度仍连在旧图上"), UpstreamOfProperty(Master, MP_Roughness).Includes(RoughnessBefore));
	TestTrue(TEXT("粗糙度上游仍有 UseORM"), UpstreamHasParameter(UpstreamOfProperty(Master, MP_Roughness), TEXT("UseORM")));
	TestEqual(TEXT("版本记录没动（下次还会重试）"), FDreamTextureMasterMaterial::ReadRecordedVersion(Master), FDreamTextureMasterMaterial::GraphVersion - 1);
	TestFalse(TEXT("包没有被标脏——不留被改过的脏包"), Package->IsDirty());
	TestTrue(TEXT("磁盘上的文件没被动过"), IFileManager::Get().GetTimeStamp(*File) == FileStamp);

	// 实例没断。
	TestTrue(TEXT("实例的父材质还是它"), Instance->Parent == Master);
	TestEqual(TEXT("用户的 UVTiling 还在"), UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(Instance, DreamTextureMaterialParam::UVTiling), 5.f);
	TestFalse(TEXT("UseORM 的关闭还在"), UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Instance, DreamTextureMaterialParam::UseORM));

	// ---- 不再注入失败，重试：这次真的重建成功 ----
	{
		FString RetryError;
		UMaterial* Rebuilt = FDreamTextureMasterMaterial::FindOrCreate(Content.Root, true, RetryError);
		TestTrue(FString::Printf(TEXT("重试成功，仍是同一个对象（%s）"), *RetryError), Rebuilt == Master);
		TestEqual(TEXT("版本更新到当前"), FDreamTextureMasterMaterial::ReadRecordedVersion(Master), FDreamTextureMasterMaterial::GraphVersion);
		TestEqual(TEXT("重建后表达式数量与原来一致"), UMaterialEditingLibrary::GetNumMaterialExpressions(Master), CountBefore);
		int32 Kept = 0;
		for (UMaterialExpression* Expression : Master->GetExpressions())
		{
			Kept += BeforeSet.Contains(Expression) ? 1 : 0;
		}
		TestEqual(TEXT("重建后旧节点一个都不在了（是真的重建）"), Kept, 0);
		TestFalse(TEXT("存盘之后不脏"), Package->IsDirty());
		TestTrue(TEXT("实例仍指着它，用户的覆盖还在"), Instance->Parent == Master && UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(Instance, DreamTextureMaterialParam::UVTiling) == 5.f);
	}

	// ---- 首次创建时失败：不留资产、不留脏包，之后能正常建 ----
	{
		FScopedTestContent FreshContent;
		const FString FreshPackageName = FreshContent.Root / FDreamTextureMasterMaterial::GetAssetName();
		{
			const FScopedBuildFailure Failure(5);
			FString FailError;
			UMaterial* Result = FDreamTextureMasterMaterial::FindOrCreate(FreshContent.Root, true, FailError);
			TestNull(TEXT("首次创建中途失败时返回空"), Result);
			TestTrue(TEXT("原因说明了是生成图时出的错"), FailError.Contains(TEXT("building the master material graph")));
		}
		TestNull(TEXT("失败后内存里没有残留的母材质"), FindObject<UMaterial>(nullptr, *FDreamTextureMasterMaterial::GetObjectPath(FreshContent.Root)));
		TestFalse(TEXT("磁盘上没有"), FPackageName::DoesPackageExist(FreshPackageName));
		if (UPackage* Leftover = FindPackage(nullptr, *FreshPackageName))
		{
			TestFalse(TEXT("留下的空包没有脏标记（「全部保存」不该列出它）"), Leftover->IsDirty());
		}

		FString OkError;
		UMaterial* Created = FDreamTextureMasterMaterial::FindOrCreate(FreshContent.Root, true, OkError);
		TestNotNull(FString::Printf(TEXT("之后能正常创建（%s）"), *OkError), Created);
	}

	return true;
}

// ---------------------------------------------------------------------------
// 常见的 bUsedWith 标志预设好：赋给骨骼网格等不会让实例（和母材质）被标脏
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureMasterUsageTest, "DreamTexture.Assets.Material.MasterUsage", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureMasterUsageTest::RunTest(const FString& Parameters)
{
	FScopedTestContent Content;
	FString Error;
	UMaterial* Master = FDreamTextureMasterMaterial::FindOrCreate(Content.Root, /*bSave*/ true, Error);
	if (!TestNotNull(FString::Printf(TEXT("生成母材质（%s）"), *Error), Master))
	{
		return false;
	}
	UPackage* MasterPackage = Master->GetOutermost();

	// 一个实例，走真正的路径：这套材质导入后建的实例，存盘，让它的包是干净的。
	UPackage* InstancePackage = CreatePackage(*(Content.Root / TEXT("MI_Usage")));
	UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(InstancePackage, TEXT("MI_Usage"), RF_Public | RF_Standalone | RF_Transactional);
	UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Master);
	{
		TArray<UPackage*> ToSave;
		ToSave.Add(InstancePackage);
		TestTrue(TEXT("准备工作：实例存盘"), UEditorLoadingAndSavingUtils::SavePackages(ToSave, /*bOnlyDirty*/ false));
	}
	TestFalse(TEXT("准备工作：母材质包是干净的"), MasterPackage->IsDirty());
	TestFalse(TEXT("准备工作：实例包是干净的"), InstancePackage->IsDirty());

	const EMaterialUsage Preset[] = { MATUSAGE_StaticMesh, MATUSAGE_SkeletalMesh, MATUSAGE_MorphTargets, MATUSAGE_InstancedStaticMeshes, MATUSAGE_SplineMesh, MATUSAGE_Nanite };
	for (const EMaterialUsage Usage : Preset)
	{
		TestTrue(FString::Printf(TEXT("母材质预设了使用标志 %d"), static_cast<int32>(Usage)), Master->GetUsageByFlag(Usage));
		TestTrue(FString::Printf(TEXT("实例继承了使用标志 %d"), static_cast<int32>(Usage)), Instance->GetUsageByFlag(Usage));
		TestTrue(FString::Printf(TEXT("实例检查使用标志 %d 通过"), static_cast<int32>(Usage)), Instance->CheckMaterialUsage(Usage));
	}
	TestFalse(TEXT("把实例赋给骨骼网格、实例化网格、Nanite 网格之后母材质包仍是干净的"), MasterPackage->IsDirty());
	TestFalse(TEXT("……实例包也仍是干净的（没有被引擎补标志标脏）"), InstancePackage->IsDirty());

	// 对照：没预设的标志，引擎会在编辑器里当场补上并把包标脏——这就是预设要避免的现象，也证明上面的「仍然干净」有意义。
	// 5.8 里补标志的是**实例**自己（UMaterialInstance::SetMaterialUsage 写实例的 BasePropertyOverrides.UsageFlags，
	// 标脏的是实例的包、并重编这个实例的静态排列），不是母材质；母材质这边预设了，实例才能继承到。
	TestFalse(TEXT("准备工作：布料标志没预设"), Master->GetUsageByFlag(MATUSAGE_Clothing));
	TestTrue(TEXT("对照：没预设的布料标志，实例检查时引擎当场补上"), Instance->CheckMaterialUsage(MATUSAGE_Clothing));
	TestTrue(TEXT("对照：补完之后实例有这个标志"), Instance->GetUsageByFlag(MATUSAGE_Clothing));
	TestTrue(TEXT("对照：而且实例包被标脏了"), InstancePackage->IsDirty());
	TestFalse(TEXT("对照：母材质不受影响"), MasterPackage->IsDirty());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
