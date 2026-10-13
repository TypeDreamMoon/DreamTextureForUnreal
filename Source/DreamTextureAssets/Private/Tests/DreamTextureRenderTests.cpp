// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureMaterialBuilder.h"
#include "DreamTextureMaterialTestUtils.h"
#include "DreamTextureRenderTestUtils.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"

using namespace DreamTextureTestUtils;

/*
 * 需要真实 RHI 的测试（DreamTexture.Render.*）。
 *
 * -nullrhi 下材质根本不会走到着色器编译，母材质的图哪怕有「采样器类型对不上」「Substrate 转换出错」
 * 这类错也一声不吭——无头测试（DreamTexture.Assets.Material.*）只能管图的结构。这一组才是真正把
 * 材质交给翻译器和着色器编译器的，`Tools/Test.ps1 -Render` 跑它。
 * 不能渲染的环境（含默认的 -nullrhi）会检测到，跳过并写明原因。
 */

namespace
{
	struct FStaticCombo
	{
		bool bUseNormal;
		bool bUseORM;
		bool bUsePOM;

		FString ToString() const
		{
			return FString::Printf(TEXT("UseNormal=%d UseORM=%d UsePOM=%d"), bUseNormal ? 1 : 0, bUseORM ? 1 : 0, bUsePOM ? 1 : 0);
		}
	};

	void ApplyCombo(UMaterialInstanceConstant* Instance, const FStaticCombo& Combo)
	{
		using namespace DreamTextureMaterialParam;
		// 逐个调用库函数；每次都会重新排静态排列的编译。最后一次之后统一等。
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(Instance, UseNormal, Combo.bUseNormal, EMaterialParameterAssociation::GlobalParameter, false);
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(Instance, UseORM, Combo.bUseORM, EMaterialParameterAssociation::GlobalParameter, false);
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(Instance, UsePOM, Combo.bUsePOM, EMaterialParameterAssociation::GlobalParameter, false);
		UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
	}
}

// ---------------------------------------------------------------------------
// 母材质与各种静态开关组合都能编过
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureRenderMasterMaterialTest, "DreamTexture.Render.MasterMaterial", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureRenderMasterMaterialTest::RunTest(const FString& Parameters)
{
	FString WhyNot;
	if (!DreamTextureRenderTestUtils::CanRender(WhyNot))
	{
		AddInfo(FString::Printf(TEXT("已跳过：%s。用 Tools/Test.ps1 -Render 跑（不带 -nullrhi 启动编辑器）。"), *WhyNot));
		return true;
	}
	AddInfo(DreamTextureRenderTestUtils::DescribeRenderer());

	FDreamTextureManifest Manifest;
	TMap<FString, FString> Files;
	FString LoadError;
	if (!TestTrue(FString::Printf(TEXT("读固定素材：%s"), *LoadError), LoadFixture(Manifest, Files, LoadError)))
	{
		return false;
	}

	FScopedTestContent Content;

	// ---- 对照：故意做错的材质必须被这套检查抓到 ----
	// 没有这一步，下面「没有编译错误」可能只是因为根本没编译。拿一张法线贴图配 Color 采样器——
	// 正是「采样器类型不匹配」这类错的原型——看翻译器是否报错、报的是不是采样器类型。
	{
		FString Error;
		UPackage* Package = CreatePackage(*(Content.Root / TEXT("M_Broken")));
		UMaterial* Broken = NewObject<UMaterial>(Package, TEXT("M_Broken"), RF_Public | RF_Standalone | RF_Transactional);
		UTexture* Normal = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineMaterials/FlatNormal.FlatNormal"));
		if (TestNotNull(TEXT("对照用的材质与贴图建得出来"), Broken) && TestNotNull(TEXT("引擎的 FlatNormal 在"), Normal))
		{
			UMaterialExpressionTextureSampleParameter2D* Sample = Cast<UMaterialExpressionTextureSampleParameter2D>(
				UMaterialEditingLibrary::CreateMaterialExpression(Broken, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -300, 0));
			if (TestNotNull(TEXT("对照用的采样节点"), Sample))
			{
				Sample->ParameterName = TEXT("Mismatched");
				Sample->Texture = Normal;
				Sample->SamplerType = SAMPLERTYPE_Color;
				UMaterialEditingLibrary::ConnectMaterialProperty(Sample, TEXT("RGB"), MP_BaseColor);
				UMaterialEditingLibrary::RecompileMaterial(Broken);

				const DreamTextureRenderTestUtils::FCompileOutcome Outcome = DreamTextureRenderTestUtils::WaitForCompile(Broken);
				TestTrue(TEXT("对照：故意配错采样器类型的材质被报了错"), Outcome.Errors.Num() > 0);
				TestTrue(FString::Printf(TEXT("对照：报的是采样器类型的错（%s）"), *FString::Join(Outcome.Errors, TEXT(" | "))),
					Outcome.Errors.ContainsByPredicate([](const FString& E) { return E.Contains(TEXT("Sampler type")); }));
				TestFalse(TEXT("对照：没有产出可用的着色器"), Outcome.bHasShaderMap);
			}
		}
	}

	// ---- 母材质本身（所有贴图参数都是引擎的默认贴图）----
	FString MasterError;
	UMaterial* Master = FDreamTextureMasterMaterial::FindOrCreate(Content.Root, /*bSave*/ false, MasterError);
	if (!TestNotNull(FString::Printf(TEXT("生成母材质（%s）"), *MasterError), Master))
	{
		return false;
	}
	{
		const DreamTextureRenderTestUtils::FCompileOutcome Outcome = DreamTextureRenderTestUtils::WaitForCompile(Master);
		TestTrue(FString::Printf(TEXT("母材质编译没有错误：%s"), *FString::Join(Outcome.Errors, TEXT("\n"))), Outcome.Errors.Num() == 0);
		TestTrue(TEXT("母材质产出了着色器"), Outcome.bHasShaderMap && Outcome.bComplete && Outcome.NumShaders > 0);
		AddInfo(FString::Printf(TEXT("母材质着色器表：%d 个着色器"), Outcome.NumShaders));
		for (const FString& Error : Outcome.Errors)
		{
			AddError(FString::Printf(TEXT("母材质编译错误：%s"), *Error));
		}
	}

	// ---- 实例：固定素材导入后建出来，再逐个开关组合编译 ----
	FDreamTextureImportRequest Request;
	Request.Manifest = Manifest;
	Request.LocalFiles = Files;
	Request.ContentRoot = Content.Root;
	Request.bSave = false;
	const FDreamTextureImportResult Imported = FDreamTextureImporter::Import(Request);
	if (!TestTrue(FString::Printf(TEXT("导入成功（%s）"), *Imported.Error), Imported.bOK))
	{
		return false;
	}
	const FDreamTextureImportedTexture InstanceResult = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Imported, Content.Root, false);
	UMaterialInstanceConstant* Instance = FindObject<UMaterialInstanceConstant>(nullptr, *InstanceResult.ObjectPath);
	if (!TestNotNull(FString::Printf(TEXT("建出材质实例（%s）"), *InstanceResult.Note), Instance))
	{
		return false;
	}

	// 八种组合全跑一遍：每个静态开关都会让着色器多出一种排列，错常常只出在某个组合里。
	const FStaticCombo Combos[] = {
		{ true, true, false },	// 导入后的默认状态
		{ true, true, true },	// 视差遮蔽打开
		{ false, true, false },	// 缺法线
		{ true, false, false },	// 缺 ORM
		{ false, false, false },	// 只有底色
		{ false, false, true },
		{ false, true, true },
		{ true, false, true },
	};
	for (const FStaticCombo& Combo : Combos)
	{
		ApplyCombo(Instance, Combo);
		const DreamTextureRenderTestUtils::FCompileOutcome Outcome = DreamTextureRenderTestUtils::WaitForCompile(Instance);
		TestTrue(FString::Printf(TEXT("实例 [%s] 编译没有错误"), *Combo.ToString()), Outcome.Errors.Num() == 0);
		TestTrue(FString::Printf(TEXT("实例 [%s] 产出了着色器"), *Combo.ToString()), Outcome.bHasShaderMap && Outcome.bComplete && Outcome.NumShaders > 0);

		// 编译统计：像素着色器指令数 > 0 说明基础通道的像素着色器真的编出来了（不是只有一张空表）。
		// 顺带把采样器数打出来——关掉 ORM / 法线 / 视差，数字应当跟着变，是开关真的在裁剪着色器的旁证。
		const FMaterialStatistics Stats = UMaterialEditingLibrary::GetStatistics(Instance);
		TestTrue(FString::Printf(TEXT("实例 [%s] 的像素着色器指令数大于 0"), *Combo.ToString()), Stats.NumPixelShaderInstructions > 0);
		AddInfo(FString::Printf(TEXT("实例 [%s]：%d 个着色器，像素着色器 %d 条指令，%d 个采样器，%d 次像素纹理采样"),
			*Combo.ToString(), Outcome.NumShaders, Stats.NumPixelShaderInstructions, Stats.NumSamplers, Stats.NumPixelTextureSamples));
		for (const FString& Error : Outcome.Errors)
		{
			AddError(FString::Printf(TEXT("实例 [%s] 编译错误：%s"), *Combo.ToString(), *Error));
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
