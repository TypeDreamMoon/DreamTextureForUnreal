// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureMaterialBuilder.h"
#include "DreamTextureMaterialTestUtils.h"
#include "HAL/PlatformFileManager.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialExpressionUtils.h"

using namespace DreamTextureTestUtils;

/*
 * 材质实例 MI_<Slug> 的无头测试（-nullrhi 能跑）。
 *
 * 母材质自己的图、版本在 DreamTextureMasterMaterialTests.cpp；这里管实例：
 * 建出来对不对、缺贴图时开关怎么走、重新导入时哪些参数被校准、哪些是用户的、不许碰。
 */

namespace
{
	bool InstanceLoadFixtureOrFail(FAutomationTestBase& Test, FDreamTextureManifest& Manifest, TMap<FString, FString>& Files)
	{
		FString Error;
		const bool bLoaded = LoadFixture(Manifest, Files, Error);
		return Test.TestTrue(FString::Printf(TEXT("读固定素材：%s"), *Error), bLoaded);
	}

	/** 导入固定素材，可以丢掉几个通道（连 manifest 里的项一起丢，模拟「这套材质本来就没有这张图」）。 */
	FDreamTextureImportResult InstanceImportFixture(const FDreamTextureManifest& Full, const TMap<FString, FString>& AllFiles, const FString& Root, bool bSave, std::initializer_list<const TCHAR*> Drop = {})
	{
		FDreamTextureImportRequest Request;
		Request.Manifest = Full;
		Request.LocalFiles = AllFiles;
		for (const TCHAR* Channel : Drop)
		{
			Request.Manifest.Maps.Remove(Channel);
			Request.LocalFiles.Remove(Channel);
		}
		Request.ContentRoot = Root;
		Request.bSave = bSave;
		return FDreamTextureImporter::Import(Request);
	}

	UMaterialInstanceConstant* FindInstance(const FString& ObjectPath)
	{
		return FindObject<UMaterialInstanceConstant>(nullptr, *ObjectPath);
	}

	UTexture* TextureOf(UMaterialInstanceConstant* Instance, const FName& Parameter)
	{
		return UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, Parameter);
	}

	bool SwitchOf(UMaterialInstanceConstant* Instance, const FName& Parameter)
	{
		return UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Instance, Parameter);
	}

	bool Overridden(UMaterialInstanceConstant* Instance, const FName& Parameter)
	{
		return UMaterialEditingLibrary::IsMaterialInstanceParameterOverridden(Instance, Parameter);
	}

	/** 实例上被覆盖的贴图参数个数。 */
	int32 CountTextureOverrides(const UMaterialInstanceConstant* Instance)
	{
		return Instance->TextureParameterValues.Num();
	}
}

// ---------------------------------------------------------------------------
// 新建：路径、父材质、贴图参数、静态开关，存盘读盘后依然成立
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureInstanceCreateTest, "DreamTexture.Assets.Material.InstanceCreate", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureInstanceCreateTest::RunTest(const FString& Parameters)
{
	FDreamTextureManifest Manifest;
	TMap<FString, FString> Files;
	if (!InstanceLoadFixtureOrFail(*this, Manifest, Files))
	{
		return false;
	}

	FScopedTestContent Content;
	const FDreamTextureImportResult Imported = InstanceImportFixture(Manifest, Files, Content.Root, /*bSave*/ true);
	if (!TestTrue(FString::Printf(TEXT("导入成功（%s）"), *Imported.Error), Imported.bOK))
	{
		return false;
	}

	TestEqual(TEXT("实例名"), FDreamTextureMaterialBuilder::GetInstanceAssetName(TEXT("MossyRiverRocks_ref")), FString(TEXT("MI_MossyRiverRocks_ref")));

	const FDreamTextureImportedTexture Created = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Imported, Content.Root, /*bSave*/ true);
	TestEqual(FString::Printf(TEXT("新建（%s）"), *Created.Note), static_cast<int32>(Created.Action), static_cast<int32>(EDreamTextureImportAction::Created));
	TestTrue(TEXT("Channel 为空"), Created.Channel.IsEmpty());

	const FString ExpectedPath = Imported.AssetFolder / TEXT("MI_MossyRiverRocks_ref.MI_MossyRiverRocks_ref");
	TestEqual(TEXT("MI 与贴图同目录，名字是 MI_<Slug>"), Created.ObjectPath, ExpectedPath);

	UMaterialInstanceConstant* Instance = FindInstance(Created.ObjectPath);
	if (!TestNotNull(TEXT("实例对象在"), Instance))
	{
		return false;
	}

	FString MasterError;
	UMaterial* Master = FDreamTextureMasterMaterial::FindOrCreate(Content.Root, true, MasterError);
	TestTrue(TEXT("父材质是工程里的 M_DreamTexture_Master"), Master != nullptr && Instance->Parent == Master);
	TestEqual(TEXT("母材质在内容根下"), Instance->Parent != nullptr ? Instance->Parent->GetPathName() : FString(), Content.Root / TEXT("M_DreamTexture_Master.M_DreamTexture_Master"));
	TestTrue(TEXT("实例已存盘"), FPackageName::DoesPackageExist(Instance->GetOutermost()->GetName()));

	// ---- 贴图参数指向对的贴图 ----
	const TMap<FName, FString> ParameterToChannel = {
		{ DreamTextureMaterialParam::BaseColor, TEXT("basecolor") },
		{ DreamTextureMaterialParam::Normal, TEXT("normal") },
		{ DreamTextureMaterialParam::ORM, TEXT("orm") },
		{ DreamTextureMaterialParam::Height, TEXT("height") },
	};
	for (const TPair<FName, FString>& Pair : ParameterToChannel)
	{
		const FDreamTextureImportedTexture* Entry = Imported.Textures.FindByPredicate([&Pair](const FDreamTextureImportedTexture& E) { return E.Channel == Pair.Value; });
		if (!TestNotNull(FString::Printf(TEXT("导入结果里有 %s"), *Pair.Value), Entry))
		{
			continue;
		}
		UTexture* Expected = FindTexture(Entry->ObjectPath);
		UTexture* Actual = TextureOf(Instance, Pair.Key);
		TestTrue(FString::Printf(TEXT("%s 参数指向 %s 的贴图"), *Pair.Key.ToString(), *Pair.Value), Expected != nullptr && Actual == Expected);
		TestTrue(FString::Printf(TEXT("%s 是覆盖状态"), *Pair.Key.ToString()), Overridden(Instance, Pair.Key));

		// 挂上去的贴图必须通过母材质里这个参数的采样器类型检查，否则运行时会退回默认贴图。
		if (Actual != nullptr)
		{
			FString Mismatch;
			TestTrue(FString::Printf(TEXT("%s 的贴图满足采样器类型检查（%s）"), *Pair.Key.ToString(), *Mismatch),
				MaterialExpressionUtils::VerifySamplerType(Actual->GetPathName(), MaterialExpressionUtils::GetSamplerTypeForTexture(Actual), Actual->SRGB, ExpectedSamplerOf(Pair.Key), Mismatch));
		}
	}
	TestEqual(TEXT("只覆盖了四张贴图参数（分通道的 roughness/metallic/ao 母材质不采样，不进实例）"), CountTextureOverrides(Instance), 4);

	// ---- 静态开关 ----
	TestTrue(TEXT("UseNormal 开"), SwitchOf(Instance, DreamTextureMaterialParam::UseNormal));
	TestTrue(TEXT("UseORM 开"), SwitchOf(Instance, DreamTextureMaterialParam::UseORM));
	TestTrue(TEXT("UseNormal 是显式覆盖"), Overridden(Instance, DreamTextureMaterialParam::UseNormal));
	TestTrue(TEXT("UseORM 是显式覆盖"), Overridden(Instance, DreamTextureMaterialParam::UseORM));
	TestFalse(TEXT("UsePOM 保持默认关（贵，用户自己开）"), SwitchOf(Instance, DreamTextureMaterialParam::UsePOM));
	TestFalse(TEXT("UsePOM 没被覆盖"), Overridden(Instance, DreamTextureMaterialParam::UsePOM));

	// ---- 用户参数没被碰：新建的实例上，这些仍是继承母材质的默认值 ----
	for (const FName& Parameter : { DreamTextureMaterialParam::UVTiling, DreamTextureMaterialParam::NormalStrength, DreamTextureMaterialParam::AOStrength,
		DreamTextureMaterialParam::RoughnessMin, DreamTextureMaterialParam::RoughnessMax, DreamTextureMaterialParam::Roughness, DreamTextureMaterialParam::Metallic,
		DreamTextureMaterialParam::HeightRatio })
	{
		TestFalse(FString::Printf(TEXT("%s 没有被覆盖"), *Parameter.ToString()), Overridden(Instance, Parameter));
	}
	TestFalse(TEXT("BaseColorTint 没有被覆盖"), Overridden(Instance, DreamTextureMaterialParam::BaseColorTint));

	// ---- 再来一次：什么都没变 → Unchanged，同一个对象，不标脏 ----
	{
		TestFalse(TEXT("建好并存盘之后不脏"), Instance->GetOutermost()->IsDirty());
		const FDreamTextureImportedTexture Again = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Imported, Content.Root, true);
		TestEqual(FString::Printf(TEXT("再来一次是 Unchanged（%s）"), *Again.Note), static_cast<int32>(Again.Action), static_cast<int32>(EDreamTextureImportAction::Unchanged));
		TestEqual(TEXT("对象路径不变"), Again.ObjectPath, Created.ObjectPath);
		TestTrue(TEXT("还是同一个对象"), FindInstance(Created.ObjectPath) == Instance);
		TestFalse(TEXT("Unchanged 不标脏"), Instance->GetOutermost()->IsDirty());
		TestFalse(TEXT("Unchanged 时母材质也不脏"), Master != nullptr && Master->GetOutermost()->IsDirty());
	}

	// ---- 存盘读盘：卸掉实例和母材质，从磁盘读回来仍然 Unchanged ----
	if (TestTrue(TEXT("卸载实例与母材质所在的包"), UnloadAssetPackages({ Instance, Master })))
	{
		TestNull(TEXT("实例已不在内存里"), FindInstance(Created.ObjectPath));

		const FDreamTextureImportedTexture Reloaded = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Imported, Content.Root, true);
		TestEqual(FString::Printf(TEXT("读盘后是 Unchanged（%s）"), *Reloaded.Note), static_cast<int32>(Reloaded.Action), static_cast<int32>(EDreamTextureImportAction::Unchanged));

		UMaterialInstanceConstant* FromDisk = FindInstance(Created.ObjectPath);
		if (TestNotNull(TEXT("实例从磁盘读回"), FromDisk))
		{
			TestTrue(TEXT("读盘后 UseORM 仍开且是覆盖"), SwitchOf(FromDisk, DreamTextureMaterialParam::UseORM) && Overridden(FromDisk, DreamTextureMaterialParam::UseORM));
			TestEqual(TEXT("读盘后四张贴图参数仍在"), CountTextureOverrides(FromDisk), 4);
			TestFalse(TEXT("读盘后不脏"), FromDisk->GetOutermost()->IsDirty());
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// 缺贴图：对应的静态开关关掉
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureInstanceMissingTest, "DreamTexture.Assets.Material.InstanceMissing", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureInstanceMissingTest::RunTest(const FString& Parameters)
{
	FDreamTextureManifest Manifest;
	TMap<FString, FString> Files;
	if (!InstanceLoadFixtureOrFail(*this, Manifest, Files))
	{
		return false;
	}

	struct FCase
	{
		const TCHAR* Name;
		TArray<const TCHAR*> Drop;
		bool bUseNormal;
		bool bUseORM;
	};
	const FCase Cases[] = {
		{ TEXT("缺 ORM"), { TEXT("orm") }, true, false },
		{ TEXT("缺法线"), { TEXT("normal") }, false, true },
		{ TEXT("法线和 ORM 都缺"), { TEXT("normal"), TEXT("orm") }, false, false },
		{ TEXT("缺高度图"), { TEXT("height") }, true, true },
		{ TEXT("只有底色"), { TEXT("normal"), TEXT("orm"), TEXT("height"), TEXT("roughness"), TEXT("metallic"), TEXT("ao") }, false, false },
	};

	for (const FCase& Case : Cases)
	{
		FScopedTestContent Content;
		FDreamTextureImportRequest Request;
		Request.Manifest = Manifest;
		Request.LocalFiles = Files;
		for (const TCHAR* Channel : Case.Drop)
		{
			Request.Manifest.Maps.Remove(Channel);
			Request.LocalFiles.Remove(Channel);
		}
		Request.ContentRoot = Content.Root;
		Request.bSave = false;
		const FDreamTextureImportResult Imported = FDreamTextureImporter::Import(Request);
		if (!TestTrue(FString::Printf(TEXT("%s：导入成功（%s）"), Case.Name, *Imported.Error), Imported.bOK))
		{
			continue;
		}

		const FDreamTextureImportedTexture Result = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Request.Manifest, Imported, Content.Root, false);
		TestEqual(FString::Printf(TEXT("%s：新建（%s）"), Case.Name, *Result.Note), static_cast<int32>(Result.Action), static_cast<int32>(EDreamTextureImportAction::Created));

		UMaterialInstanceConstant* Instance = FindInstance(Result.ObjectPath);
		if (!TestNotNull(FString::Printf(TEXT("%s：实例在"), Case.Name), Instance))
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("%s：UseNormal"), Case.Name), SwitchOf(Instance, DreamTextureMaterialParam::UseNormal), Case.bUseNormal);
		TestEqual(FString::Printf(TEXT("%s：UseORM"), Case.Name), SwitchOf(Instance, DreamTextureMaterialParam::UseORM), Case.bUseORM);
		TestFalse(FString::Printf(TEXT("%s：UsePOM 仍是默认关"), Case.Name), SwitchOf(Instance, DreamTextureMaterialParam::UsePOM));

		// 缺的那张贴图参数不该被赋值（不拿别的图冒充）。
		for (const TCHAR* Channel : Case.Drop)
		{
			const FName Parameter = FString(Channel) == TEXT("normal") ? DreamTextureMaterialParam::Normal
				: FString(Channel) == TEXT("orm") ? DreamTextureMaterialParam::ORM
				: FString(Channel) == TEXT("height") ? DreamTextureMaterialParam::Height : NAME_None;
			if (!Parameter.IsNone())
			{
				TestFalse(FString::Printf(TEXT("%s：%s 没有被赋值"), Case.Name, *Parameter.ToString()), Overridden(Instance, Parameter));
			}
		}
		TestTrue(FString::Printf(TEXT("%s：底色总是有的"), Case.Name), TextureOf(Instance, DreamTextureMaterialParam::BaseColor) != nullptr && Overridden(Instance, DreamTextureMaterialParam::BaseColor));
	}

	// ---- 一张可用的贴图都没有：不建实例，说明原因 ----
	{
		FScopedTestContent Content;
		FDreamTextureImportResult Empty;
		Empty.bOK = false;
		FDreamTextureImportedTexture Skipped;
		Skipped.Channel = TEXT("orm");
		Skipped.Action = EDreamTextureImportAction::Skipped;
		Skipped.Note = TEXT("测试：没文件");
		Empty.Textures.Add(Skipped);
		FDreamTextureImportedTexture Failed;
		Failed.Channel = TEXT("basecolor");
		Failed.Action = EDreamTextureImportAction::Failed;
		Failed.Note = TEXT("测试：解码失败");
		Empty.Textures.Add(Failed);

		const FDreamTextureImportedTexture Result = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Empty, Content.Root, false);
		TestEqual(TEXT("没有可用贴图时是 Skipped"), static_cast<int32>(Result.Action), static_cast<int32>(EDreamTextureImportAction::Skipped));
		TestFalse(TEXT("Skipped 带中文说明"), Result.Note.IsEmpty());
		TestNull(TEXT("没有建出实例"), FindInstance(Result.ObjectPath));
		TestNull(TEXT("母材质也没有白建"), FindObject<UMaterial>(nullptr, *FDreamTextureMasterMaterial::GetObjectPath(Content.Root)));
	}

	// ---- 贴图在结果里写着 Created，但工程里已经不在了：当作缺，不崩 ----
	{
		FScopedTestContent Content;
		FDreamTextureImportResult Ghost;
		Ghost.bOK = true;
		FDreamTextureImportedTexture Entry;
		Entry.Channel = TEXT("basecolor");
		Entry.Action = EDreamTextureImportAction::Created;
		Entry.ObjectPath = Content.Root / TEXT("Nope/T_Nope_BC.T_Nope_BC");
		Ghost.Textures.Add(Entry);

		const FDreamTextureImportedTexture Result = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Ghost, Content.Root, false);
		TestEqual(TEXT("结果里的贴图在工程里找不到时是 Skipped"), static_cast<int32>(Result.Action), static_cast<int32>(EDreamTextureImportAction::Skipped));
		TestTrue(TEXT("说明里点出了找不到"), Result.Note.Contains(TEXT("was not found")));
	}

	return true;
}

// ---------------------------------------------------------------------------
// 就地更新：只校准插件管的参数，用户调过的不动
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureInstanceUpdateTest, "DreamTexture.Assets.Material.InstanceUpdate", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureInstanceUpdateTest::RunTest(const FString& Parameters)
{
	FDreamTextureManifest Manifest;
	TMap<FString, FString> Files;
	if (!InstanceLoadFixtureOrFail(*this, Manifest, Files))
	{
		return false;
	}

	FScopedTestContent Content;
	const FDreamTextureImportResult Imported = InstanceImportFixture(Manifest, Files, Content.Root, false);
	if (!TestTrue(FString::Printf(TEXT("导入成功（%s）"), *Imported.Error), Imported.bOK))
	{
		return false;
	}

	const FDreamTextureImportedTexture First = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Imported, Content.Root, false);
	UMaterialInstanceConstant* Instance = FindInstance(First.ObjectPath);
	if (!TestNotNull(FString::Printf(TEXT("建出实例（%s）"), *First.Note), Instance))
	{
		return false;
	}

	UTexture* BaseColorTexture = TextureOf(Instance, DreamTextureMaterialParam::BaseColor);
	UTexture* NormalTexture = TextureOf(Instance, DreamTextureMaterialParam::Normal);
	UTexture* WhiteGrid = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineMaterials/DefaultWhiteGrid.DefaultWhiteGrid"));
	if (!TestTrue(TEXT("准备工作：底色/法线贴图在，引擎的 DefaultWhiteGrid 在"), BaseColorTexture != nullptr && NormalTexture != nullptr && WhiteGrid != nullptr))
	{
		return false;
	}

	// ---- 用户在实例上的调整 ----
	const FLinearColor UserTint(0.8f, 0.6f, 0.4f, 1.f);
	UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, DreamTextureMaterialParam::UVTiling, 4.f);
	UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, DreamTextureMaterialParam::NormalStrength, 0.5f);
	UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, DreamTextureMaterialParam::RoughnessMax, 0.8f);
	UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, DreamTextureMaterialParam::AOStrength, 0.25f);
	UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, DreamTextureMaterialParam::BaseColorTint, UserTint);
	UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(Instance, DreamTextureMaterialParam::UsePOM, true);

	// ...以及一处插件管的参数被人换掉了：底色贴图改成了别的图。
	Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(DreamTextureMaterialParam::BaseColor), WhiteGrid);
	UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
	TestTrue(TEXT("准备工作：底色贴图被换成了别的"), TextureOf(Instance, DreamTextureMaterialParam::BaseColor) == WhiteGrid);

	// ---- 再导入一次（贴图没变）----
	const FDreamTextureImportResult ImportedAgain = InstanceImportFixture(Manifest, Files, Content.Root, false);
	TestEqual(TEXT("贴图本身全部 Unchanged"), ImportedAgain.Textures.FilterByPredicate([](const FDreamTextureImportedTexture& E) { return E.Action == EDreamTextureImportAction::Unchanged; }).Num(), 7);

	const FDreamTextureImportedTexture Second = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, ImportedAgain, Content.Root, false);
	TestEqual(FString::Printf(TEXT("有参数被校准，所以是 Updated（%s）"), *Second.Note), static_cast<int32>(Second.Action), static_cast<int32>(EDreamTextureImportAction::Updated));
	TestTrue(TEXT("还是同一个 MI 对象"), FindInstance(Second.ObjectPath) == Instance);
	TestEqual(TEXT("对象路径不变"), Second.ObjectPath, First.ObjectPath);

	// 插件管的：贴图参数按现状校准。
	TestTrue(TEXT("底色贴图参数被校准回导入的那张"), TextureOf(Instance, DreamTextureMaterialParam::BaseColor) == BaseColorTexture);
	TestTrue(TEXT("法线贴图参数没变"), TextureOf(Instance, DreamTextureMaterialParam::Normal) == NormalTexture);
	TestTrue(TEXT("UseORM 仍开"), SwitchOf(Instance, DreamTextureMaterialParam::UseORM));
	TestTrue(TEXT("UseNormal 仍开"), SwitchOf(Instance, DreamTextureMaterialParam::UseNormal));

	// 用户的：一个都不许动。
	TestEqual(TEXT("UVTiling 保留"), UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(Instance, DreamTextureMaterialParam::UVTiling), 4.f);
	TestEqual(TEXT("NormalStrength 保留"), UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(Instance, DreamTextureMaterialParam::NormalStrength), 0.5f);
	TestEqual(TEXT("RoughnessMax 保留"), UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(Instance, DreamTextureMaterialParam::RoughnessMax), 0.8f);
	TestEqual(TEXT("AOStrength 保留"), UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(Instance, DreamTextureMaterialParam::AOStrength), 0.25f);
	TestTrue(TEXT("BaseColorTint 保留"), UMaterialEditingLibrary::GetMaterialInstanceVectorParameterValue(Instance, DreamTextureMaterialParam::BaseColorTint).Equals(UserTint));
	TestTrue(TEXT("用户打开的 UsePOM 保留（Height 仍在）"), SwitchOf(Instance, DreamTextureMaterialParam::UsePOM));
	TestFalse(TEXT("RoughnessMin 本来没调，仍是继承状态"), Overridden(Instance, DreamTextureMaterialParam::RoughnessMin));

	// ---- 三次：什么都没变了 → Unchanged（用户的覆盖不会让它误判成有变化）----
	{
		const FDreamTextureImportedTexture Third = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, ImportedAgain, Content.Root, false);
		TestEqual(FString::Printf(TEXT("校准过后再来是 Unchanged（%s）"), *Third.Note), static_cast<int32>(Third.Action), static_cast<int32>(EDreamTextureImportAction::Unchanged));
		TestEqual(TEXT("UVTiling 仍保留"), UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(Instance, DreamTextureMaterialParam::UVTiling), 4.f);
	}

	// ---- 这一轮 ORM 没了（比如换了不产 ORM 的管线）：UseORM 关，用户参数仍不动 ----
	{
		FDreamTextureImportResult NoOrm = ImportedAgain;
		NoOrm.Textures.RemoveAll([](const FDreamTextureImportedTexture& E) { return E.Channel == TEXT("orm"); });
		const FDreamTextureImportedTexture Fourth = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, NoOrm, Content.Root, false);
		TestEqual(FString::Printf(TEXT("ORM 没了是 Updated（%s）"), *Fourth.Note), static_cast<int32>(Fourth.Action), static_cast<int32>(EDreamTextureImportAction::Updated));
		TestTrue(TEXT("还是同一个对象"), FindInstance(Fourth.ObjectPath) == Instance);
		TestFalse(TEXT("UseORM 关了"), SwitchOf(Instance, DreamTextureMaterialParam::UseORM));
		TestTrue(TEXT("UseNormal 仍开"), SwitchOf(Instance, DreamTextureMaterialParam::UseNormal));
		TestEqual(TEXT("UVTiling 仍是 4"), UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(Instance, DreamTextureMaterialParam::UVTiling), 4.f);
		TestEqual(TEXT("AOStrength 仍是 0.25"), UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(Instance, DreamTextureMaterialParam::AOStrength), 0.25f);

		// 又有了 ORM：UseORM 回到开。
		const FDreamTextureImportedTexture Fifth = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, ImportedAgain, Content.Root, false);
		TestEqual(FString::Printf(TEXT("ORM 回来是 Updated（%s）"), *Fifth.Note), static_cast<int32>(Fifth.Action), static_cast<int32>(EDreamTextureImportAction::Updated));
		TestTrue(TEXT("UseORM 又开了"), SwitchOf(Instance, DreamTextureMaterialParam::UseORM));
	}

	return true;
}

// ---------------------------------------------------------------------------
// 没有高度图时 UsePOM 不能开着；贴图被改过压缩设置时不挂
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureInstanceGuardTest, "DreamTexture.Assets.Material.InstanceGuard", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureInstanceGuardTest::RunTest(const FString& Parameters)
{
	FDreamTextureManifest Manifest;
	TMap<FString, FString> Files;
	if (!InstanceLoadFixtureOrFail(*this, Manifest, Files))
	{
		return false;
	}

	// ---- 没有高度图：用户（或旧状态）把 UsePOM 开着，更新时替他关掉 ----
	{
		FScopedTestContent Content;
		const FDreamTextureImportResult Imported = InstanceImportFixture(Manifest, Files, Content.Root, false, { TEXT("height") });
		if (!TestTrue(FString::Printf(TEXT("导入成功（%s）"), *Imported.Error), Imported.bOK))
		{
			return false;
		}
		FDreamTextureManifest NoHeight = Manifest;
		NoHeight.Maps.Remove(TEXT("height"));

		const FDreamTextureImportedTexture Created = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(NoHeight, Imported, Content.Root, false);
		UMaterialInstanceConstant* Instance = FindInstance(Created.ObjectPath);
		if (TestNotNull(TEXT("建出实例"), Instance))
		{
			TestFalse(TEXT("没有高度图：UsePOM 关"), SwitchOf(Instance, DreamTextureMaterialParam::UsePOM));
			TestFalse(TEXT("没有高度图：Height 没被赋值"), Overridden(Instance, DreamTextureMaterialParam::Height));

			UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(Instance, DreamTextureMaterialParam::UsePOM, true);
			TestTrue(TEXT("准备工作：UsePOM 被打开了"), SwitchOf(Instance, DreamTextureMaterialParam::UsePOM));

			const FDreamTextureImportedTexture Updated = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(NoHeight, Imported, Content.Root, false);
			TestEqual(FString::Printf(TEXT("UsePOM 被校准关掉所以是 Updated（%s）"), *Updated.Note), static_cast<int32>(Updated.Action), static_cast<int32>(EDreamTextureImportAction::Updated));
			TestFalse(TEXT("没有高度图时 UsePOM 被关掉"), SwitchOf(Instance, DreamTextureMaterialParam::UsePOM));
		}
	}

	// ---- 贴图的压缩设置被改过：采样器类型对不上，不挂上去，开关关掉，说明里点出来 ----
	{
		FScopedTestContent Content;
		const FDreamTextureImportResult Imported = InstanceImportFixture(Manifest, Files, Content.Root, false);
		if (!TestTrue(FString::Printf(TEXT("导入成功（%s）"), *Imported.Error), Imported.bOK))
		{
			return false;
		}

		const FDreamTextureImportedTexture* OrmEntry = Imported.Textures.FindByPredicate([](const FDreamTextureImportedTexture& E) { return E.Channel == TEXT("orm"); });
		UTexture2D* Orm = OrmEntry != nullptr ? FindTexture(OrmEntry->ObjectPath) : nullptr;
		if (!TestNotNull(TEXT("ORM 贴图在"), Orm))
		{
			return false;
		}
		// 模拟用户在内容浏览器里把 ORM 的压缩设置从 Masks 改成 Default（彩色、sRGB 开）：采样器类型随之变成 Color，
		// 与母材质 ORM 参数要求的 Masks 对不上，挂上去母材质会编译失败——实例要拒绝挂它。
		//
		// 以前这里只把 SRGB 设成 true，但那是编辑器里造不出来的状态：UTexture::ValidateSettingsAfterImportOrEdit
		// 对 TC_Masks / TC_Normalmap 等会强制关掉 sRGB。只在内存里改又不 PostEditChange，真实 RHI 下贴图异步编译完成时
		// 引擎发现属性变了、又把它登记回编译管理器，报 LogTexture 错误（-nullrhi 下不异步编译，所以只有渲染运行才暴露）。
		// 所以改属性要夹在 PreEditChange（会先等异步编译结束）/ PostEditChange 之间，并且改成一个真能出现的错配。
		Orm->PreEditChange(nullptr);
		Orm->CompressionSettings = TC_Default;
		Orm->SRGB = true;
		Orm->PostEditChange();

		const FDreamTextureImportedTexture Result = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Imported, Content.Root, false);
		TestEqual(FString::Printf(TEXT("仍然建出了实例（%s）"), *Result.Note), static_cast<int32>(Result.Action), static_cast<int32>(EDreamTextureImportAction::Created));
		TestTrue(TEXT("说明里点出了是哪张贴图、为什么"), Result.Note.Contains(TEXT("ORM")) && Result.Note.Contains(TEXT("compression settings")));

		UMaterialInstanceConstant* Instance = FindInstance(Result.ObjectPath);
		if (TestNotNull(TEXT("实例在"), Instance))
		{
			TestFalse(TEXT("ORM 没被挂上"), Overridden(Instance, DreamTextureMaterialParam::ORM));
			TestFalse(TEXT("UseORM 关了，走标量兜底"), SwitchOf(Instance, DreamTextureMaterialParam::UseORM));
			TestTrue(TEXT("别的贴图照常挂上"), TextureOf(Instance, DreamTextureMaterialParam::BaseColor) != nullptr && Overridden(Instance, DreamTextureMaterialParam::BaseColor));
			TestTrue(TEXT("UseNormal 开"), SwitchOf(Instance, DreamTextureMaterialParam::UseNormal));
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// 保存失败之后重试：没存盘的不能当成功
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureInstanceSaveRetryTest, "DreamTexture.Assets.Material.SaveRetry", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureInstanceSaveRetryTest::RunTest(const FString& Parameters)
{
	FDreamTextureManifest Manifest;
	TMap<FString, FString> Files;
	if (!InstanceLoadFixtureOrFail(*this, Manifest, Files))
	{
		return false;
	}

	FScopedTestContent Content;
	const FDreamTextureImportResult Imported = InstanceImportFixture(Manifest, Files, Content.Root, /*bSave*/ true);
	if (!TestTrue(FString::Printf(TEXT("导入成功（%s）"), *Imported.Error), Imported.bOK))
	{
		return false;
	}

	// ---- 第一轮：bSave 关着建出来，包是脏的、没落盘。 ----
	const FDreamTextureImportedTexture First = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Imported, Content.Root, /*bSave*/ false);
	TestEqual(FString::Printf(TEXT("新建（%s）"), *First.Note), static_cast<int32>(First.Action), static_cast<int32>(EDreamTextureImportAction::Created));
	UMaterialInstanceConstant* Instance = FindInstance(First.ObjectPath);
	FString MasterError;
	UMaterial* Master = FDreamTextureMasterMaterial::FindOrCreate(Content.Root, /*bSave*/ false, MasterError);
	if (!TestNotNull(TEXT("实例在"), Instance) || !TestNotNull(FString::Printf(TEXT("母材质在（%s）"), *MasterError), Master))
	{
		return false;
	}
	const FString InstanceFile = FPackageName::LongPackageNameToFilename(Instance->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
	const FString MasterFile = FPackageName::LongPackageNameToFilename(Master->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
	TestTrue(TEXT("准备工作：实例包是脏的"), Instance->GetOutermost()->IsDirty());
	TestTrue(TEXT("准备工作：母材质包是脏的"), Master->GetOutermost()->IsDirty());
	TestFalse(TEXT("准备工作：实例没落盘"), IFileManager::Get().FileExists(*InstanceFile));
	TestFalse(TEXT("准备工作：母材质没落盘"), IFileManager::Get().FileExists(*MasterFile));

	// ---- 重试，这一轮要存盘：什么参数都没变（Unchanged），但脏的必须补存。 ----
	{
		const FDreamTextureImportedTexture Retry = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Imported, Content.Root, /*bSave*/ true);
		TestEqual(FString::Printf(TEXT("参数没变所以是 Unchanged（%s）"), *Retry.Note), static_cast<int32>(Retry.Action), static_cast<int32>(EDreamTextureImportAction::Unchanged));
		TestTrue(TEXT("实例补存到了磁盘"), IFileManager::Get().FileExists(*InstanceFile));
		TestTrue(TEXT("母材质补存到了磁盘（同版本早返回前也要补存）"), IFileManager::Get().FileExists(*MasterFile));
		TestFalse(TEXT("实例包不再是脏的"), Instance->GetOutermost()->IsDirty());
		TestFalse(TEXT("母材质包不再是脏的"), Master->GetOutermost()->IsDirty());
	}

	// ---- 再来一次：已经存好了，不重复存（不脏就不动文件）。 ----
	{
		const FDateTime InstanceStamp = IFileManager::Get().GetTimeStamp(*InstanceFile);
		const FDateTime MasterStamp = IFileManager::Get().GetTimeStamp(*MasterFile);
		const FDreamTextureImportedTexture Again = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Imported, Content.Root, true);
		TestEqual(TEXT("再来一次还是 Unchanged"), static_cast<int32>(Again.Action), static_cast<int32>(EDreamTextureImportAction::Unchanged));
		TestTrue(TEXT("实例文件没被重写"), IFileManager::Get().GetTimeStamp(*InstanceFile) == InstanceStamp);
		TestTrue(TEXT("母材质文件没被重写"), IFileManager::Get().GetTimeStamp(*MasterFile) == MasterStamp);
	}

	// ---- 真的保存失败：文件改成只读，再把包弄脏。 ----
	// 这一步依赖「只读文件让 SavePackages 返回失败」；如果这台机器上它照样存成功了，就说明模拟不了，
	// 在 AddInfo 里写明，上面「脏包 + 同版本 → 会存」那一半已经覆盖了核心行为。
	// 引擎存盘失败时会往日志里写 Error，自动化框架把日志里的 Error 算成测试失败——这里的失败是我们故意造的，
	// 先声明。次数填 0 = 不限次数（母材质、实例各存一遍，引擎还会重试）。
	AddExpectedError(TEXT("as it is read only"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("Error saving"), EAutomationExpectedErrorFlags::Contains, 0);
	FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*MasterFile, true);
	FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*InstanceFile, true);
	Master->MarkPackageDirty();
	Instance->MarkPackageDirty();

	FString SaveError;
	UMaterial* Failed = FDreamTextureMasterMaterial::FindOrCreate(Content.Root, /*bSave*/ true, SaveError);
	if (Failed == nullptr)
	{
		TestTrue(FString::Printf(TEXT("母材质存盘失败时返回空并说明原因（%s）"), *SaveError), SaveError.Contains(TEXT("saving")));
		TestTrue(TEXT("失败后母材质包仍是脏的（下次还会补存）"), Master->GetOutermost()->IsDirty());

		const FDreamTextureImportedTexture InstanceFailed = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Imported, Content.Root, true);
		TestEqual(TEXT("母材质存不了，实例也报失败"), static_cast<int32>(InstanceFailed.Action), static_cast<int32>(EDreamTextureImportAction::Failed));
		TestTrue(TEXT("实例包仍是脏的"), Instance->GetOutermost()->IsDirty());

		// 恢复可写，重试：补存成功，且不是「当成功但没存」。
		FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*MasterFile, false);
		FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*InstanceFile, false);
		FString RetryError;
		UMaterial* Recovered = FDreamTextureMasterMaterial::FindOrCreate(Content.Root, true, RetryError);
		TestTrue(FString::Printf(TEXT("恢复可写后母材质补存成功（%s）"), *RetryError), Recovered == Master);
		TestFalse(TEXT("补存之后母材质包不脏"), Master->GetOutermost()->IsDirty());
		const FDreamTextureImportedTexture InstanceRecovered = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Imported, Content.Root, true);
		TestEqual(FString::Printf(TEXT("恢复可写后实例补存成功（%s）"), *InstanceRecovered.Note), static_cast<int32>(InstanceRecovered.Action), static_cast<int32>(EDreamTextureImportAction::Unchanged));
		TestFalse(TEXT("补存之后实例包不脏"), Instance->GetOutermost()->IsDirty());
	}
	else
	{
		FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*MasterFile, false);
		FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*InstanceFile, false);
		AddInfo(TEXT("没能模拟保存失败：把文件设成只读之后 SavePackages 仍然存成功了（编辑器自己清了只读位）。「脏包 + 同版本 → 会存」已由上面的步骤验证。"));
		TestFalse(TEXT("（模拟不了失败时）母材质包存完不脏"), Master->GetOutermost()->IsDirty());
	}

	// 清理前恢复可写，免得删不掉测试目录。
	FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*MasterFile, false);
	FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*InstanceFile, false);
	return true;
}

// ---------------------------------------------------------------------------
// 归属：只改自己为这套材质建的实例
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureInstanceOwnerTest, "DreamTexture.Assets.Material.InstanceOwner", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureInstanceOwnerTest::RunTest(const FString& Parameters)
{
	FDreamTextureManifest Manifest;
	TMap<FString, FString> Files;
	if (!InstanceLoadFixtureOrFail(*this, Manifest, Files))
	{
		return false;
	}

	// ---- 新建时写下归属记录，存盘读盘后还在 ----
	{
		FScopedTestContent Content;
		const FDreamTextureImportResult Imported = InstanceImportFixture(Manifest, Files, Content.Root, true);
		if (!TestTrue(FString::Printf(TEXT("导入成功（%s）"), *Imported.Error), Imported.bOK))
		{
			return false;
		}
		const FDreamTextureImportedTexture Created = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Imported, Content.Root, true);
		UMaterialInstanceConstant* Instance = FindInstance(Created.ObjectPath);
		if (!TestNotNull(FString::Printf(TEXT("建出实例（%s）"), *Created.Note), Instance))
		{
			return false;
		}
		TestEqual(TEXT("实例上记着它属于固定素材这套材质"), FDreamTextureMaterialBuilder::ReadInstanceMaterialId(Instance), Manifest.Id);

		FString MasterError;
		UMaterial* Master = FDreamTextureMasterMaterial::FindOrCreate(Content.Root, true, MasterError);
		if (TestTrue(TEXT("卸载实例与母材质所在的包"), UnloadAssetPackages({ Instance, Master })))
		{
			const FDreamTextureImportedTexture Again = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Imported, Content.Root, true);
			TestEqual(FString::Printf(TEXT("读盘后归属记录还在，仍是自己的实例 → Unchanged（%s）"), *Again.Note), static_cast<int32>(Again.Action), static_cast<int32>(EDreamTextureImportAction::Unchanged));
			TestEqual(TEXT("读盘后记录仍是这套材质"), FDreamTextureMaterialBuilder::ReadInstanceMaterialId(FindInstance(Created.ObjectPath)), Manifest.Id);
		}
	}

	// ---- slug 撞车：另一套材质（id 不同，名字与 id 尾段相同，于是同一个目录同一个实例名）----
	{
		FScopedTestContent Content;
		const FDreamTextureImportResult Imported = InstanceImportFixture(Manifest, Files, Content.Root, false);
		if (!TestTrue(FString::Printf(TEXT("导入成功（%s）"), *Imported.Error), Imported.bOK))
		{
			return false;
		}
		const FDreamTextureImportedTexture Created = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Imported, Content.Root, false);
		UMaterialInstanceConstant* Instance = FindInstance(Created.ObjectPath);
		if (!TestNotNull(FString::Printf(TEXT("第一套建出实例（%s）"), *Created.Note), Instance))
		{
			return false;
		}
		// 用户在第一套的实例上调过东西，撞车的那套不许碰。
		UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, DreamTextureMaterialParam::UVTiling, 7.f);
		UTexture* BaseColorBefore = TextureOf(Instance, DreamTextureMaterialParam::BaseColor);

		FDreamTextureManifest Other = Manifest;
		Other.Id = TEXT("mat_20260827_555555_ref");
		TestEqual(TEXT("准备工作：两套材质的 slug 相同"), FDreamTextureImporter::MakeSlug(Other), FDreamTextureImporter::MakeSlug(Manifest));

		const FDreamTextureImportedTexture Clash = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Other, Imported, Content.Root, false);
		TestEqual(TEXT("撞车时实例 Failed"), static_cast<int32>(Clash.Action), static_cast<int32>(EDreamTextureImportAction::Failed));
		TestTrue(FString::Printf(TEXT("原因是中文，点明属于另一套材质并带上它的 id（%s）"), *Clash.Note), Clash.Note.Contains(TEXT("another material set")) && Clash.Note.Contains(Manifest.Id));
		TestEqual(TEXT("实例的归属没被改"), FDreamTextureMaterialBuilder::ReadInstanceMaterialId(Instance), Manifest.Id);
		TestEqual(TEXT("用户调的 UVTiling 没被碰"), UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(Instance, DreamTextureMaterialParam::UVTiling), 7.f);
		TestTrue(TEXT("贴图参数没被碰"), TextureOf(Instance, DreamTextureMaterialParam::BaseColor) == BaseColorBefore);

		// 原来那套仍然能正常更新。
		const FDreamTextureImportedTexture Own = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Imported, Content.Root, false);
		TestEqual(FString::Printf(TEXT("自己那套照常 Unchanged（%s）"), *Own.Note), static_cast<int32>(Own.Action), static_cast<int32>(EDreamTextureImportAction::Unchanged));
	}

	// ---- 用户自己的同名实例：没有记录，不动 ----
	{
		FScopedTestContent Content;
		const FDreamTextureImportResult Imported = InstanceImportFixture(Manifest, Files, Content.Root, false);
		if (!TestTrue(FString::Printf(TEXT("导入成功（%s）"), *Imported.Error), Imported.bOK))
		{
			return false;
		}

		const FString Name = FDreamTextureMaterialBuilder::GetInstanceAssetName(FDreamTextureImporter::MakeSlug(Manifest));
		UPackage* Package = CreatePackage(*(Imported.AssetFolder / Name));
		UMaterialInstanceConstant* Mine = NewObject<UMaterialInstanceConstant>(Package, FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
		if (!TestNotNull(TEXT("用户的实例建得出来"), Mine))
		{
			return false;
		}
		UTexture* Marker = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineMaterials/DefaultWhiteGrid.DefaultWhiteGrid"));
		if (!TestNotNull(TEXT("引擎的 DefaultWhiteGrid 在"), Marker))
		{
			return false;
		}
		Mine->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(DreamTextureMaterialParam::BaseColor), Marker);
		TestTrue(TEXT("准备工作：用户的实例没有归属记录"), FDreamTextureMaterialBuilder::ReadInstanceMaterialId(Mine).IsEmpty());
		TestTrue(TEXT("准备工作：用户的实例没有父材质"), Mine->Parent == nullptr);

		const FDreamTextureImportedTexture Result = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Imported, Content.Root, false);
		TestEqual(TEXT("用户的同名实例：Failed"), static_cast<int32>(Result.Action), static_cast<int32>(EDreamTextureImportAction::Failed));
		TestTrue(FString::Printf(TEXT("原因是中文，点明不是本插件建的、没有来源记录（%s）"), *Result.Note), Result.Note.Contains(TEXT("this plugin did not create it")) && Result.Note.Contains(TEXT("provenance record")));
		TestTrue(TEXT("用户的实例原样还在"), FindInstance(Result.ObjectPath) == Mine);
		TestTrue(TEXT("用户挂的贴图没被换"), TextureOf(Mine, DreamTextureMaterialParam::BaseColor) == Marker);
		TestEqual(TEXT("没有被加参数覆盖（只有用户自己挂的那一个）"), CountTextureOverrides(Mine), 1);
		TestTrue(TEXT("没有被安上父材质"), Mine->Parent == nullptr);
		TestTrue(TEXT("没有被写上归属记录"), FDreamTextureMaterialBuilder::ReadInstanceMaterialId(Mine).IsEmpty());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
