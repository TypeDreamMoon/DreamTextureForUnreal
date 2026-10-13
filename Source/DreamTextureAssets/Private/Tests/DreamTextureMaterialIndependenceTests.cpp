// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/IAssetRegistry.h"
#include "DreamTextureMaterialBuilder.h"
#include "DreamTextureMaterialTestUtils.h"
#include "Misc/FileHelper.h"

using namespace DreamTextureTestUtils;

/*
 * M2 的核心验收：产物与插件无关。
 *
 * 插件是 editor-only 的。母材质或材质实例只要引用了插件里的任何东西（插件 Content 下的资产，
 * 或插件模块里定义的类），打包、或者禁用插件之后，它们就断链。所以这里把两个包存盘，
 * 用资产注册表查它们落在磁盘上的依赖——**只看磁盘上存下来的**，内存里的引用不算数。
 */

namespace
{
	/** 任何指向插件的依赖。插件的内容挂载点是 /DreamTextureForUnreal/，三个模块的类在 /Script/DreamTexture* 下。 */
	bool IsPluginDependency(const FName& PackageName)
	{
		const FString Name = PackageName.ToString();
		return Name.StartsWith(TEXT("/DreamTextureForUnreal/"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("/DreamTextureForUnreal"), ESearchCase::IgnoreCase) ||
			Name.StartsWith(TEXT("/Script/DreamTexture"), ESearchCase::IgnoreCase);
	}

	bool HasDependency(const TArray<FName>& Dependencies, const FString& Prefix)
	{
		return Dependencies.ContainsByPredicate([&Prefix](const FName& Name) { return Name.ToString().StartsWith(Prefix); });
	}

	/** 文件字节里有没有这段文本（按 ASCII 找，也按 UTF-16LE 找一遍）。 */
	bool FileContains(const TArray<uint8>& Bytes, const FString& Needle)
	{
		auto Find = [&Bytes](const TArray<uint8>& Pattern)
		{
			if (Pattern.Num() == 0 || Bytes.Num() < Pattern.Num())
			{
				return false;
			}
			for (int32 Start = 0; Start + Pattern.Num() <= Bytes.Num(); ++Start)
			{
				if (FMemory::Memcmp(Bytes.GetData() + Start, Pattern.GetData(), Pattern.Num()) == 0)
				{
					return true;
				}
			}
			return false;
		};

		TArray<uint8> Ascii;
		TArray<uint8> Wide;
		for (const TCHAR C : Needle)
		{
			Ascii.Add(static_cast<uint8>(C));
			Wide.Add(static_cast<uint8>(C & 0xFF));
			Wide.Add(static_cast<uint8>((C >> 8) & 0xFF));
		}
		return Find(Ascii) || Find(Wide);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureMaterialIndependenceTest, "DreamTexture.Assets.Material.Independence", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureMaterialIndependenceTest::RunTest(const FString& Parameters)
{
	FDreamTextureManifest Manifest;
	TMap<FString, FString> Files;
	FString LoadError;
	if (!TestTrue(FString::Printf(TEXT("读固定素材：%s"), *LoadError), LoadFixture(Manifest, Files, LoadError)))
	{
		return false;
	}

	FScopedTestContent Content;

	FDreamTextureImportRequest Request;
	Request.Manifest = Manifest;
	Request.LocalFiles = Files;
	Request.ContentRoot = Content.Root;
	Request.bSave = true;
	const FDreamTextureImportResult Imported = FDreamTextureImporter::Import(Request);
	if (!TestTrue(FString::Printf(TEXT("导入并存盘（%s）"), *Imported.Error), Imported.bOK))
	{
		return false;
	}

	// bSave 开着：母材质和实例都真的写到磁盘，依赖才查得到。
	const FDreamTextureImportedTexture Instance = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Imported, Content.Root, /*bSave*/ true);
	if (!TestEqual(FString::Printf(TEXT("建出材质实例（%s）"), *Instance.Note), static_cast<int32>(Instance.Action), static_cast<int32>(EDreamTextureImportAction::Created)))
	{
		return false;
	}

	const FString MasterPackage = Content.Root / FDreamTextureMasterMaterial::GetAssetName();
	const FString InstancePackage = FPackageName::ObjectPathToPackageName(Instance.ObjectPath);
	TestTrue(TEXT("母材质的包在磁盘上"), FPackageName::DoesPackageExist(MasterPackage));
	TestTrue(TEXT("材质实例的包在磁盘上"), FPackageName::DoesPackageExist(InstancePackage));

	// 让资产注册表从磁盘读这几个包（它的依赖表是读盘建出来的，不是读内存里的对象）。
	IAssetRegistry& Registry = IAssetRegistry::GetChecked();
	Registry.ScanPathsSynchronous({ Content.Root }, /*bForceRescan*/ true);

	auto CheckPackage = [this, &Registry](const TCHAR* What, const FString& PackageName) -> TArray<FName>
	{
		TArray<FName> Dependencies;
		const bool bQueried = Registry.GetDependencies(FName(*PackageName), Dependencies, UE::AssetRegistry::EDependencyCategory::Package);
		TestTrue(FString::Printf(TEXT("%s：资产注册表查得到依赖"), What), bQueried);

		// 先证明这次查询不是空查，下面「没有指向插件的依赖」才有意义。
		TestTrue(FString::Printf(TEXT("%s：依赖表不是空的（%d 项）"), What, Dependencies.Num()), Dependencies.Num() > 0);

		for (const FName& Dependency : Dependencies)
		{
			TestFalse(FString::Printf(TEXT("%s 不依赖插件：%s"), What, *Dependency.ToString()), IsPluginDependency(Dependency));
		}
		return Dependencies;
	};

	const TArray<FName> MasterDependencies = CheckPackage(TEXT("母材质"), MasterPackage);
	const TArray<FName> InstanceDependencies = CheckPackage(TEXT("材质实例"), InstancePackage);

	// 母材质的依赖里是引擎自带的默认贴图和视差函数，都在 /Engine 下。
	TestTrue(TEXT("母材质引用引擎的默认贴图（FlatNormal）"), HasDependency(MasterDependencies, TEXT("/Engine/EngineMaterials/FlatNormal")));
	TestTrue(TEXT("母材质引用引擎的视差遮蔽函数"), HasDependency(MasterDependencies, TEXT("/Engine/Functions/")));
	TestFalse(TEXT("母材质不引用这批导入的贴图"), HasDependency(MasterDependencies, Content.Root + TEXT("/MossyRiverRocks_ref/")));

	// 实例引用母材质和这一套的四张贴图。
	TestTrue(TEXT("实例引用母材质"), InstanceDependencies.Contains(FName(*MasterPackage)));
	for (const TCHAR* Channel : { TEXT("basecolor"), TEXT("normal"), TEXT("orm"), TEXT("height") })
	{
		const FString TexturePackage = Imported.AssetFolder / FDreamTextureImporter::GetTextureAssetName(TEXT("MossyRiverRocks_ref"), Channel);
		TestTrue(FString::Printf(TEXT("实例引用 %s 贴图"), Channel), InstanceDependencies.Contains(FName(*TexturePackage)));
	}

	// 资产注册表的依赖表只记「包」之间的引用，**不记对原生类的引用**（它把 /Script/ 下的包滤掉了）——
	// 而「材质表达式是不是插件里定义的类」恰恰是最容易漏的一种依赖。所以再直接读存盘后的 .uasset 字节：
	// 引用到插件的类或插件内容，名字表里一定会出现 "/Script/DreamTexture…" 或 "/DreamTextureForUnreal/…"。
	// 先在同一份字节里找 /Script/Engine 当对照：能找到它，说明这种读法确实看得见类所在的包。
	for (const FString& PackageName : { MasterPackage, InstancePackage })
	{
		TArray<uint8> Bytes;
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		if (!TestTrue(FString::Printf(TEXT("读得到 %s"), *Filename), FFileHelper::LoadFileToArray(Bytes, *Filename)))
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("%s：字节里能看到 /Script/Engine（对照）"), *PackageName), FileContains(Bytes, TEXT("/Script/Engine")));
		TestFalse(FString::Printf(TEXT("%s：字节里没有 /Script/DreamTexture…"), *PackageName), FileContains(Bytes, TEXT("/Script/DreamTexture")));
		TestFalse(FString::Printf(TEXT("%s：字节里没有 /DreamTextureForUnreal/…"), *PackageName), FileContains(Bytes, TEXT("/DreamTextureForUnreal")));
	}

	// 这一整套（贴图 + 母材质 + 实例）里没有一个包依赖插件。
	{
		TArray<FAssetData> All;
		Registry.GetAssetsByPath(FName(*Content.Root), All, /*bRecursive*/ true);
		TestTrue(TEXT("测试目录下至少有贴图、母材质和实例"), All.Num() >= 9);
		for (const FAssetData& Asset : All)
		{
			TArray<FName> Dependencies;
			Registry.GetDependencies(Asset.PackageName, Dependencies, UE::AssetRegistry::EDependencyCategory::Package);
			for (const FName& Dependency : Dependencies)
			{
				TestFalse(FString::Printf(TEXT("%s 不依赖插件：%s"), *Asset.PackageName.ToString(), *Dependency.ToString()), IsPluginDependency(Dependency));
			}
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
