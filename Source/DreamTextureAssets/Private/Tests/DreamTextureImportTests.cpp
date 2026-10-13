// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureImporter.h"
#include "DreamTextureTestUtils.h"
#include "HAL/PlatformFileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "PackageTools.h"
#include "UObject/UObjectGlobals.h"

using namespace DreamTextureTestUtils;

namespace
{
	/** 读固定素材；失败时把原因记成测试错误。（别把 LoadFixture 直接写在 TestTrue 的参数里：Error 会在它跑之前先被求值。） */
	bool ImportLoadFixtureOrFail(FAutomationTestBase& Test, FDreamTextureManifest& Manifest, TMap<FString, FString>& Files)
	{
		FString Error;
		const bool bLoaded = LoadFixture(Manifest, Files, Error);
		return Test.TestTrue(FString::Printf(TEXT("读固定素材：%s"), *Error), bLoaded);
	}

	/** 一次导入固定素材。bSave 默认关：绝大多数断言只关心内存里的资产。 */
	FDreamTextureImportResult ImportOneFixture(const FDreamTextureManifest& Manifest, const TMap<FString, FString>& Files, const FString& Root, bool bSave = false)
	{
		FDreamTextureImportRequest Request;
		Request.Manifest = Manifest;
		Request.LocalFiles = Files;
		Request.ContentRoot = Root;
		Request.bSave = bSave;
		return FDreamTextureImporter::Import(Request);
	}

	const FDreamTextureImportedTexture* FindEntry(const FDreamTextureImportResult& Result, const FString& Channel)
	{
		return Result.Textures.FindByPredicate([&Channel](const FDreamTextureImportedTexture& Entry)
		{
			return Entry.Channel.Equals(Channel, ESearchCase::CaseSensitive);
		});
	}

	int32 ImportCountAction(const FDreamTextureImportResult& Result, EDreamTextureImportAction Action)
	{
		int32 Count = 0;
		for (const FDreamTextureImportedTexture& Entry : Result.Textures)
		{
			Count += (Entry.Action == Action) ? 1 : 0;
		}
		return Count;
	}

	/** 断言一张贴图的设置与规则表、manifest 一致。 */
	void CheckTextureSettings(FAutomationTestBase& Test, const FDreamTextureManifest& Manifest, const FString& Channel, const UTexture2D* Texture)
	{
		const FDreamTextureChannelRule* Rule = FDreamTextureChannelRule::Find(Channel);
		const FDreamTextureMapInfo* Map = Manifest.FindMap(Channel);
		if (!Test.TestNotNull(FString::Printf(TEXT("%s 有规则和 manifest 项"), *Channel), Rule) || !Test.TestNotNull(TEXT("manifest 项"), Map) ||
			!Test.TestNotNull(FString::Printf(TEXT("%s 贴图存在"), *Channel), Texture))
		{
			return;
		}

		Test.TestEqual(FString::Printf(TEXT("%s 压缩设置"), *Channel), (int32)Texture->CompressionSettings.GetValue(), (int32)Rule->Compression.GetValue());
		Test.TestEqual(FString::Printf(TEXT("%s LODGroup"), *Channel), (int32)Texture->LODGroup.GetValue(), (int32)Rule->LODGroup.GetValue());

		// sRGB 以 manifest 的 colorspace 为准：只有 basecolor 是 srgb。
		const bool bExpectedSRGB = (Map->ColorSpace == EDreamTextureColorSpace::SRGB);
		Test.TestEqual(FString::Printf(TEXT("%s sRGB"), *Channel), Texture->SRGB != 0, bExpectedSRGB);

		// 法线不翻 G：固定素材的 y 是 directx。守的就是这一条，别「顺手」改成真。
		Test.TestFalse(FString::Printf(TEXT("%s 不翻绿通道"), *Channel), Texture->bFlipGreenChannel != 0);

		Test.TestEqual(FString::Printf(TEXT("%s 宽"), *Channel), (int32)Texture->Source.GetSizeX(), Map->Width);
		Test.TestEqual(FString::Printf(TEXT("%s 高"), *Channel), (int32)Texture->Source.GetSizeY(), Map->Height);

		FDreamTextureSourceRecord Record;
		if (Test.TestTrue(FString::Printf(TEXT("%s 有来源记录"), *Channel), FDreamTextureImporter::ReadSourceRecord(Texture, Record)))
		{
			Test.TestEqual(FString::Printf(TEXT("%s 记录的材质 id"), *Channel), Record.MaterialId, Manifest.Id);
			Test.TestEqual(FString::Printf(TEXT("%s 记录的通道"), *Channel), Record.Channel, Channel);
			Test.TestEqual(FString::Printf(TEXT("%s 记录的 sha256"), *Channel), Record.Sha256, Map->Sha256);
		}
	}

	/** 把一张图写成 PNG 到磁盘。 */
	bool WritePng(const FString& Path, const FImageView& Image)
	{
		TArray64<uint8> Bytes;
		return FImageUtils::CompressImage(Bytes, TEXT("png"), Image) && FFileHelper::SaveArrayToFile(Bytes, *Path);
	}

	/** 把已加载的资产连包一起卸掉，下次访问只能从磁盘读。 */
	bool UnloadPackagesOf(const TArray<UPackage*>& Packages)
	{
		FTextureCompilingManager::Get().FinishAllCompilation();
		FText Error;
		return UPackageTools::UnloadPackages(Packages, Error, /*bUnloadDirtyPackages*/ true);
	}
}

// ---------------------------------------------------------------------------
// 新建
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAssetsImportCreateTest, "DreamTexture.Assets.Import.Create", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAssetsImportCreateTest::RunTest(const FString& Parameters)
{
	FDreamTextureManifest Manifest;
	TMap<FString, FString> Files;
	if (!ImportLoadFixtureOrFail(*this, Manifest, Files))
	{
		return false;
	}

	FScopedTestContent Content;
	const FDreamTextureImportResult Result = ImportOneFixture(Manifest, Files, Content.Root);

	TestTrue(FString::Printf(TEXT("导入成功（%s）"), *Result.Error), Result.bOK);
	TestEqual(TEXT("落在 <根>/<Slug>"), Result.AssetFolder, Content.Root / TEXT("MossyRiverRocks_ref"));
	TestEqual(TEXT("七个通道各一张"), Result.Textures.Num(), 7);
	TestEqual(TEXT("全部是新建"), ImportCountAction(Result, EDreamTextureImportAction::Created), 7);

	for (const TPair<FString, FDreamTextureMapInfo>& Pair : Manifest.Maps)
	{
		const FDreamTextureImportedTexture* Entry = FindEntry(Result, Pair.Key);
		if (!TestNotNull(FString::Printf(TEXT("结果里有 %s"), *Pair.Key), Entry))
		{
			continue;
		}

		const FString ExpectedName = FDreamTextureImporter::GetTextureAssetName(TEXT("MossyRiverRocks_ref"), Pair.Key);
		const FString ExpectedPath = Result.AssetFolder / ExpectedName + TEXT(".") + ExpectedName;
		TestEqual(FString::Printf(TEXT("%s 对象路径"), *Pair.Key), Entry->ObjectPath, ExpectedPath);

		CheckTextureSettings(*this, Manifest, Pair.Key, FindTexture(Entry->ObjectPath));
	}

	// 固定素材是 8 位 RGB(A) PNG，源格式应该是 BGRA8。
	if (const UTexture2D* BaseColor = FindTexture(Result.AssetFolder / TEXT("T_MossyRiverRocks_ref_BC.T_MossyRiverRocks_ref_BC")))
	{
		TestEqual(TEXT("basecolor 源格式"), (int32)BaseColor->Source.GetFormat(), (int32)TSF_BGRA8);
	}

	// 来源记录读回来的和写进去的一致。
	const TMap<FString, FString> Hashes = FDreamTextureImporter::FindImportedHashes(Manifest, Content.Root);
	TestEqual(TEXT("FindImportedHashes 数量"), Hashes.Num(), 7);
	for (const TPair<FString, FDreamTextureMapInfo>& Pair : Manifest.Maps)
	{
		const FString* Found = Hashes.Find(Pair.Key);
		if (TestNotNull(FString::Printf(TEXT("%s 的哈希被找到"), *Pair.Key), Found))
		{
			TestEqual(FString::Printf(TEXT("%s 的哈希与 manifest 一致"), *Pair.Key), *Found, Pair.Value.Sha256);
		}
	}

	// slug 撞车：另一套材质（id 不同）但名字和 id 尾段都一样，于是得到同一个目录。
	// 它既看不到这批资产的哈希，也不能覆盖它们——否则会弄坏已经连好的第一套材质。
	{
		FDreamTextureManifest Other = Manifest;
		Other.Id = TEXT("mat_20260827_555555_ref");
		TestEqual(TEXT("撞车的两套材质 slug 相同"), FDreamTextureImporter::MakeSlug(Other), FDreamTextureImporter::MakeSlug(Manifest));
		TestEqual(TEXT("别的 id 读不到这批资产的哈希"), FDreamTextureImporter::FindImportedHashes(Other, Content.Root).Num(), 0);

		const FDreamTextureImportResult Clash = ImportOneFixture(Other, Files, Content.Root);
		TestFalse(TEXT("撞车时不覆盖"), Clash.bOK);
		TestEqual(TEXT("七个通道全部失败"), ImportCountAction(Clash, EDreamTextureImportAction::Failed), 7);
		TestFalse(TEXT("失败原因是中文"), Clash.Error.IsEmpty());

		FDreamTextureSourceRecord Record;
		const UTexture2D* BaseColor = FindTexture(Result.AssetFolder / TEXT("T_MossyRiverRocks_ref_BC.T_MossyRiverRocks_ref_BC"));
		if (TestTrue(TEXT("原资产的来源记录还在"), FDreamTextureImporter::ReadSourceRecord(BaseColor, Record)))
		{
			TestEqual(TEXT("记录仍是第一套材质的 id"), Record.MaterialId, Manifest.Id);
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// 再导入：去重与就地更新
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAssetsImportReimportTest, "DreamTexture.Assets.Import.Reimport", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAssetsImportReimportTest::RunTest(const FString& Parameters)
{
	FDreamTextureManifest Manifest;
	TMap<FString, FString> Files;
	if (!ImportLoadFixtureOrFail(*this, Manifest, Files))
	{
		return false;
	}

	FScopedTestContent Content;
	const FDreamTextureImportResult First = ImportOneFixture(Manifest, Files, Content.Root);
	if (!TestTrue(TEXT("首次导入成功"), First.bOK))
	{
		return false;
	}

	TMap<FString, UTexture2D*> Before;
	for (const FDreamTextureImportedTexture& Entry : First.Textures)
	{
		Before.Add(Entry.Channel, FindTexture(Entry.ObjectPath));
	}

	// 原样再导一次：同一批 UObject，什么都不做。
	{
		const FDreamTextureImportResult Again = ImportOneFixture(Manifest, Files, Content.Root);
		TestTrue(TEXT("原样再导成功"), Again.bOK);
		TestEqual(TEXT("全部 Unchanged"), ImportCountAction(Again, EDreamTextureImportAction::Unchanged), 7);
		for (const FDreamTextureImportedTexture& Entry : Again.Textures)
		{
			TestTrue(FString::Printf(TEXT("%s 仍是同一个 UObject"), *Entry.Channel), FindTexture(Entry.ObjectPath) == Before.FindRef(Entry.Channel));
		}
	}

	// 只给 manifest、不给文件（Fetcher 报 UpToDate 的情形）：同样是 Unchanged，而不是 Skipped。
	{
		const TMap<FString, FString> NoFiles;
		const FDreamTextureImportResult UpToDate = ImportOneFixture(Manifest, NoFiles, Content.Root);
		TestTrue(TEXT("没给文件但哈希都对得上"), UpToDate.bOK);
		TestEqual(TEXT("没给文件也是 Unchanged"), ImportCountAction(UpToDate, EDreamTextureImportAction::Unchanged), 7);
	}

	// 改一张图的字节：这里用「改 manifest 里的 sha256，并换一张不同的 PNG」来模拟。
	// 同一个 UObject，动作是 Updated，源数据真的换掉了，其余通道不动。
	{
		UTexture2D* Height = Before.FindRef(DreamTextureChannel::Height);
		if (!TestNotNull(TEXT("height 贴图在"), Height))
		{
			return false;
		}
		FImage OldPixels;
		TestTrue(TEXT("读出旧的 height 源数据"), Height->Source.GetMipImage(OldPixels, 0));

		FDreamTextureManifest Changed = Manifest;
		Changed.Maps.FindChecked(DreamTextureChannel::Height).Sha256 = TEXT("ffffffffffffffff");
		TMap<FString, FString> ChangedFiles = Files;
		ChangedFiles.Add(DreamTextureChannel::Height, Files.FindChecked(DreamTextureChannel::Roughness));

		const FDreamTextureImportResult Updated = ImportOneFixture(Changed, ChangedFiles, Content.Root);
		TestTrue(FString::Printf(TEXT("更新成功（%s）"), *Updated.Error), Updated.bOK);
		TestEqual(TEXT("只有一张 Updated"), ImportCountAction(Updated, EDreamTextureImportAction::Updated), 1);
		TestEqual(TEXT("其余六张 Unchanged"), ImportCountAction(Updated, EDreamTextureImportAction::Unchanged), 6);
		TestEqual(TEXT("没有新建任何资产"), ImportCountAction(Updated, EDreamTextureImportAction::Created), 0);

		const FDreamTextureImportedTexture* Entry = FindEntry(Updated, DreamTextureChannel::Height);
		if (TestNotNull(TEXT("结果里有 height"), Entry))
		{
			TestTrue(TEXT("height 动作是 Updated"), Entry->Action == EDreamTextureImportAction::Updated);
			TestTrue(TEXT("height 仍是同一个 UObject（引用它的材质不受影响）"), FindTexture(Entry->ObjectPath) == Height);
		}
		for (const TPair<FString, UTexture2D*>& Pair : Before)
		{
			TestTrue(FString::Printf(TEXT("%s 仍是同一个 UObject"), *Pair.Key), FindTexture(FindEntry(Updated, Pair.Key)->ObjectPath) == Pair.Value);
		}

		// 不能出现 _1 后缀的新资产。
		TestNull(TEXT("没有 _1 后缀的资产"), FindTexture(First.AssetFolder + TEXT("/T_MossyRiverRocks_ref_H_1.T_MossyRiverRocks_ref_H_1")));

		FImage NewPixels;
		TestTrue(TEXT("读出新的 height 源数据"), Height->Source.GetMipImage(NewPixels, 0));
		TestTrue(TEXT("源数据确实被换掉了"), OldPixels.RawData.Num() == NewPixels.RawData.Num() && FMemory::Memcmp(OldPixels.RawData.GetData(), NewPixels.RawData.GetData(), OldPixels.RawData.Num()) != 0);

		FDreamTextureSourceRecord Record;
		if (TestTrue(TEXT("更新后仍有来源记录"), FDreamTextureImporter::ReadSourceRecord(Height, Record)))
		{
			TestEqual(TEXT("记录里的哈希更新了"), Record.Sha256, FString(TEXT("ffffffffffffffff")));
		}
		TestEqual(TEXT("FindImportedHashes 读到新哈希"), FDreamTextureImporter::FindImportedHashes(Changed, Content.Root).FindRef(DreamTextureChannel::Height), FString(TEXT("ffffffffffffffff")));

		// 更新过的设置还是规则表规定的。
		CheckTextureSettings(*this, Changed, DreamTextureChannel::Height, Height);
	}

	return true;
}

// ---------------------------------------------------------------------------
// 降级：少通道、未知通道、坏文件
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAssetsImportDegradeTest, "DreamTexture.Assets.Import.Degrade", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAssetsImportDegradeTest::RunTest(const FString& Parameters)
{
	FDreamTextureManifest Manifest;
	TMap<FString, FString> Files;
	if (!ImportLoadFixtureOrFail(*this, Manifest, Files))
	{
		return false;
	}

	// manifest 少一个通道：降级，不失败。
	{
		FScopedTestContent Content;
		FDreamTextureManifest Less = Manifest;
		TMap<FString, FString> LessFiles = Files;
		Less.Maps.Remove(DreamTextureChannel::Height);
		LessFiles.Remove(DreamTextureChannel::Height);

		const FDreamTextureImportResult Result = ImportOneFixture(Less, LessFiles, Content.Root);
		TestTrue(FString::Printf(TEXT("少 height 仍成功（%s）"), *Result.Error), Result.bOK);
		TestEqual(TEXT("导入了其余六张"), ImportCountAction(Result, EDreamTextureImportAction::Created), 6);
		TestNull(TEXT("结果里没有 height"), FindEntry(Result, DreamTextureChannel::Height));
		TestNull(TEXT("工程里没有 height 贴图"), FindTexture(Result.AssetFolder / TEXT("T_MossyRiverRocks_ref_H.T_MossyRiverRocks_ref_H")));
	}

	// 未知通道：Skipped，带中文说明，其余照常。
	{
		FScopedTestContent Content;
		FDreamTextureManifest Extra = Manifest;
		TMap<FString, FString> ExtraFiles = Files;
		FDreamTextureMapInfo Emissive = Manifest.Maps.FindChecked(DreamTextureChannel::BaseColor);
		Emissive.Channel = TEXT("emissive");
		Extra.Maps.Add(TEXT("emissive"), Emissive);
		ExtraFiles.Add(TEXT("emissive"), Files.FindChecked(DreamTextureChannel::BaseColor));

		const FDreamTextureImportResult Result = ImportOneFixture(Extra, ExtraFiles, Content.Root);
		TestTrue(FString::Printf(TEXT("有未知通道仍成功（%s）"), *Result.Error), Result.bOK);
		TestEqual(TEXT("七张照常新建"), ImportCountAction(Result, EDreamTextureImportAction::Created), 7);
		const FDreamTextureImportedTexture* Entry = FindEntry(Result, TEXT("emissive"));
		if (TestNotNull(TEXT("结果里记着 emissive"), Entry))
		{
			TestTrue(TEXT("未知通道是 Skipped"), Entry->Action == EDreamTextureImportAction::Skipped);
			TestTrue(TEXT("Skipped 有中文说明"), Entry->Note.Contains(TEXT("Unknown channel")));
			TestTrue(TEXT("未知通道没有对象路径"), Entry->ObjectPath.IsEmpty());
		}
	}

	// 只给 manifest、没给某个通道的文件、工程里也没有：Skipped，不是失败。
	{
		FScopedTestContent Content;
		TMap<FString, FString> Partial = Files;
		Partial.Remove(DreamTextureChannel::Roughness);

		const FDreamTextureImportResult Result = ImportOneFixture(Manifest, Partial, Content.Root);
		TestTrue(FString::Printf(TEXT("缺一个文件仍成功（%s）"), *Result.Error), Result.bOK);
		TestEqual(TEXT("六张新建"), ImportCountAction(Result, EDreamTextureImportAction::Created), 6);
		const FDreamTextureImportedTexture* Entry = FindEntry(Result, DreamTextureChannel::Roughness);
		if (TestNotNull(TEXT("结果里记着 roughness"), Entry))
		{
			TestTrue(TEXT("缺文件的通道是 Skipped"), Entry->Action == EDreamTextureImportAction::Skipped);
			TestFalse(TEXT("Skipped 有说明"), Entry->Note.IsEmpty());
		}
	}

	// 文件在 LocalFiles 里但读不到 / 不是图片：那个通道 Failed，bOK 为假，别的通道不受牵连。
	{
		FScopedTestContent Content;
		TMap<FString, FString> Broken = Files;
		Broken.Add(DreamTextureChannel::AO, Content.GetDiskDir() / TEXT("不存在.png"));

		const FString GarbagePath = FPaths::ProjectSavedDir() / TEXT("DreamTextureTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits) / TEXT("garbage.png");
		FFileHelper::SaveStringToFile(TEXT("这不是 PNG"), *GarbagePath);
		Broken.Add(DreamTextureChannel::Metallic, GarbagePath);

		const FDreamTextureImportResult Result = ImportOneFixture(Manifest, Broken, Content.Root);
		TestFalse(TEXT("有通道失败则 bOK 为假"), Result.bOK);
		TestFalse(TEXT("给出中文失败原因"), Result.Error.IsEmpty());
		TestEqual(TEXT("两个通道失败"), ImportCountAction(Result, EDreamTextureImportAction::Failed), 2);
		TestEqual(TEXT("其余五张不受牵连"), ImportCountAction(Result, EDreamTextureImportAction::Created), 5);
		for (const TCHAR* Channel : { TEXT("ao"), TEXT("metallic") })
		{
			const FDreamTextureImportedTexture* Entry = FindEntry(Result, Channel);
			if (TestNotNull(FString::Printf(TEXT("结果里记着 %s"), Channel), Entry))
			{
				TestTrue(FString::Printf(TEXT("%s 是 Failed"), Channel), Entry->Action == EDreamTextureImportAction::Failed);
				TestFalse(FString::Printf(TEXT("%s 的 Failed 有说明"), Channel), Entry->Note.IsEmpty());
			}
		}
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(GarbagePath), false, true);
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(FPaths::GetPath(GarbagePath)), false, false);
	}

	// 什么都没有：bOK 为假，有说明。
	{
		FScopedTestContent Content;
		FDreamTextureManifest Empty;
		Empty.Id = TEXT("mat_1_2_zz");
		Empty.Name = TEXT("empty");
		const FDreamTextureImportResult Result = ImportOneFixture(Empty, TMap<FString, FString>(), Content.Root);
		TestFalse(TEXT("没有任何通道时不算成功"), Result.bOK);
		TestFalse(TEXT("有说明"), Result.Error.IsEmpty());
	}

	// 没有 id：拒绝。
	{
		FDreamTextureManifest NoId = Manifest;
		NoId.Id.Empty();
		const FDreamTextureImportResult Result = ImportOneFixture(NoId, Files, TEXT("/Game/DreamTextureTests/ShouldNotExist"));
		TestFalse(TEXT("没有 id 不导入"), Result.bOK);
		TestEqual(TEXT("没有建任何东西"), Result.Textures.Num(), 0);
	}

	return true;
}

// ---------------------------------------------------------------------------
// 位深：灰度 8 位 → G8，灰度 16 位 → G16，不压缩位深
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAssetsImportBitDepthTest, "DreamTexture.Assets.Import.BitDepth", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAssetsImportBitDepthTest::RunTest(const FString& Parameters)
{
	const FString TempDir = FPaths::ProjectSavedDir() / TEXT("DreamTextureTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*TempDir, false, true);
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(TempDir), false, false);
	};

	constexpr int32 Size = 8;

	// 16 位灰度：故意用 8 位表达不了的值（不是 257 的倍数），压成 8 位再还原就会对不上。
	FImage Gray16(Size, Size, ERawImageFormat::G16, EGammaSpace::Linear);
	{
		uint16* Pixels = reinterpret_cast<uint16*>(Gray16.RawData.GetData());
		for (int32 Index = 0; Index < Size * Size; ++Index)
		{
			Pixels[Index] = static_cast<uint16>(Index * 1000 + 7);
		}
	}
	FImage Gray8(Size, Size, ERawImageFormat::G8, EGammaSpace::Linear);
	{
		uint8* Pixels = Gray8.RawData.GetData();
		for (int32 Index = 0; Index < Size * Size; ++Index)
		{
			Pixels[Index] = static_cast<uint8>(Index * 3 + 1);
		}
	}

	const FString Gray16Path = TempDir / TEXT("height16.png");
	const FString Gray8Path = TempDir / TEXT("roughness8.png");
	if (!TestTrue(TEXT("写出 16 位灰度 PNG"), WritePng(Gray16Path, Gray16)) || !TestTrue(TEXT("写出 8 位灰度 PNG"), WritePng(Gray8Path, Gray8)))
	{
		return false;
	}

	FDreamTextureManifest Manifest;
	Manifest.Id = TEXT("mat_20260101_000001_bd");
	Manifest.Name = TEXT("bit depth");
	for (const TCHAR* Channel : { TEXT("height"), TEXT("roughness") })
	{
		FDreamTextureMapInfo Info;
		Info.Channel = Channel;
		Info.ColorSpace = EDreamTextureColorSpace::Linear;
		Info.Width = Size;
		Info.Height = Size;
		Info.Sha256 = FString(Channel).Equals(TEXT("height")) ? TEXT("1111111111111111") : TEXT("2222222222222222");
		Manifest.Maps.Add(Channel, Info);
	}
	TMap<FString, FString> Files;
	Files.Add(TEXT("height"), Gray16Path);
	Files.Add(TEXT("roughness"), Gray8Path);

	FScopedTestContent Content;
	const FDreamTextureImportResult Result = ImportOneFixture(Manifest, Files, Content.Root);
	TestTrue(FString::Printf(TEXT("导入成功（%s）"), *Result.Error), Result.bOK);

	const FDreamTextureImportedTexture* HeightEntry = FindEntry(Result, TEXT("height"));
	const FDreamTextureImportedTexture* RoughEntry = FindEntry(Result, TEXT("roughness"));
	if (!TestNotNull(TEXT("height 结果"), HeightEntry) || !TestNotNull(TEXT("roughness 结果"), RoughEntry))
	{
		return false;
	}

	UTexture2D* Height = FindTexture(HeightEntry->ObjectPath);
	UTexture2D* Roughness = FindTexture(RoughEntry->ObjectPath);
	if (!TestNotNull(TEXT("height 贴图"), Height) || !TestNotNull(TEXT("roughness 贴图"), Roughness))
	{
		return false;
	}

	TestEqual(TEXT("16 位灰度的源格式是 G16"), (int32)Height->Source.GetFormat(), (int32)TSF_G16);
	TestEqual(TEXT("8 位灰度的源格式是 G8"), (int32)Roughness->Source.GetFormat(), (int32)TSF_G8);
	TestEqual(TEXT("灰度走 TC_Grayscale"), (int32)Height->CompressionSettings.GetValue(), (int32)TC_Grayscale);

	FImage Got16;
	if (TestTrue(TEXT("读回 G16 源数据"), Height->Source.GetMipImage(Got16, 0)))
	{
		TestEqual(TEXT("G16 数据长度"), Got16.RawData.Num(), Gray16.RawData.Num());
		TestTrue(TEXT("G16 像素逐位一致（没有被压成 8 位）"), Got16.RawData.Num() == Gray16.RawData.Num() && FMemory::Memcmp(Got16.RawData.GetData(), Gray16.RawData.GetData(), Gray16.RawData.Num()) == 0);
	}
	FImage Got8;
	if (TestTrue(TEXT("读回 G8 源数据"), Roughness->Source.GetMipImage(Got8, 0)))
	{
		TestTrue(TEXT("G8 像素逐位一致"), Got8.RawData.Num() == Gray8.RawData.Num() && FMemory::Memcmp(Got8.RawData.GetData(), Gray8.RawData.GetData(), Gray8.RawData.Num()) == 0);
	}

	return true;
}

// ---------------------------------------------------------------------------
// 法线 y：只有 manifest 明说 opengl 才翻 G
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAssetsImportNormalYTest, "DreamTexture.Assets.Import.NormalY", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAssetsImportNormalYTest::RunTest(const FString& Parameters)
{
	FDreamTextureManifest Manifest;
	TMap<FString, FString> Files;
	if (!ImportLoadFixtureOrFail(*this, Manifest, Files))
	{
		return false;
	}

	// 只留法线，换不同的 y 反复导入同一个资产：每次都是就地更新，翻 G 开关跟着 manifest 走。
	FDreamTextureManifest Normal = Manifest;
	Normal.Maps.Reset();
	FDreamTextureMapInfo Info = Manifest.Maps.FindChecked(DreamTextureChannel::Normal);
	Normal.Maps.Add(DreamTextureChannel::Normal, Info);
	TMap<FString, FString> NormalFiles;
	NormalFiles.Add(DreamTextureChannel::Normal, Files.FindChecked(DreamTextureChannel::Normal));

	FScopedTestContent Content;

	auto ImportWith = [&](const TCHAR* NormalY, const TCHAR* Sha) -> UTexture2D*
	{
		Normal.Maps.FindChecked(DreamTextureChannel::Normal).NormalY = NormalY;
		Normal.Maps.FindChecked(DreamTextureChannel::Normal).Sha256 = Sha;
		const FDreamTextureImportResult Result = ImportOneFixture(Normal, NormalFiles, Content.Root);
		TestTrue(FString::Printf(TEXT("导入 y=%s 成功（%s）"), NormalY, *Result.Error), Result.bOK);
		const FDreamTextureImportedTexture* Entry = FindEntry(Result, DreamTextureChannel::Normal);
		return Entry != nullptr ? FindTexture(Entry->ObjectPath) : nullptr;
	};

	UTexture2D* Texture = ImportWith(TEXT("directx"), TEXT("aaaaaaaaaaaaaaaa"));
	if (!TestNotNull(TEXT("法线贴图"), Texture))
	{
		return false;
	}
	TestFalse(TEXT("y=directx 不翻 G"), Texture->bFlipGreenChannel != 0);
	TestEqual(TEXT("法线是 TC_Normalmap"), (int32)Texture->CompressionSettings.GetValue(), (int32)TC_Normalmap);
	TestEqual(TEXT("法线是 WorldNormalMap 组"), (int32)Texture->LODGroup.GetValue(), (int32)TEXTUREGROUP_WorldNormalMap);
	TestFalse(TEXT("法线 sRGB 关"), Texture->SRGB != 0);

	TestTrue(TEXT("y=opengl 时同一个对象"), ImportWith(TEXT("opengl"), TEXT("bbbbbbbbbbbbbbbb")) == Texture);
	TestTrue(TEXT("y=opengl 翻 G"), Texture->bFlipGreenChannel != 0);

	TestTrue(TEXT("y=directx 时同一个对象"), ImportWith(TEXT("directx"), TEXT("cccccccccccccccc")) == Texture);
	TestFalse(TEXT("改回 directx 后又不翻"), Texture->bFlipGreenChannel != 0);

	// y 缺失按 directx 处理（v1 的约定），不翻。
	TestTrue(TEXT("y 缺失时同一个对象"), ImportWith(TEXT(""), TEXT("dddddddddddddddd")) == Texture);
	TestFalse(TEXT("y 缺失不翻 G"), Texture->bFlipGreenChannel != 0);

	return true;
}

// ---------------------------------------------------------------------------
// 存盘后来源记录仍在：卸载再从磁盘读
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAssetsImportPersistTest, "DreamTexture.Assets.Import.Persist", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAssetsImportPersistTest::RunTest(const FString& Parameters)
{
	FDreamTextureManifest Manifest;
	TMap<FString, FString> Files;
	if (!ImportLoadFixtureOrFail(*this, Manifest, Files))
	{
		return false;
	}

	FScopedTestContent Content;
	const FDreamTextureImportResult Result = ImportOneFixture(Manifest, Files, Content.Root, /*bSave*/ true);
	if (!TestTrue(FString::Printf(TEXT("导入并存盘成功（%s）"), *Result.Error), Result.bOK))
	{
		return false;
	}

	TArray<UPackage*> Packages;
	for (const FDreamTextureImportedTexture& Entry : Result.Textures)
	{
		const FString PackageName = FPackageName::ObjectPathToPackageName(Entry.ObjectPath);
		TestTrue(FString::Printf(TEXT("%s 已写到磁盘"), *Entry.Channel), FPackageName::DoesPackageExist(PackageName));
		if (const UTexture2D* Texture = FindTexture(Entry.ObjectPath))
		{
			Packages.Add(Texture->GetOutermost());
		}
	}

	// 把内存里的包卸掉，之后读到的只能是磁盘上的那份——来源记录要是没存进包里，这里就找不到。
	if (!TestTrue(TEXT("卸载已存盘的包"), UnloadPackagesOf(Packages)))
	{
		return false;
	}
	for (const FDreamTextureImportedTexture& Entry : Result.Textures)
	{
		TestNull(FString::Printf(TEXT("%s 已不在内存里"), *Entry.Channel), FindTexture(Entry.ObjectPath));
	}

	const TMap<FString, FString> Hashes = FDreamTextureImporter::FindImportedHashes(Manifest, Content.Root);
	TestEqual(TEXT("从磁盘读回的来源记录数量"), Hashes.Num(), 7);
	for (const TPair<FString, FDreamTextureMapInfo>& Pair : Manifest.Maps)
	{
		TestEqual(FString::Printf(TEXT("%s 的哈希经存盘读盘后一致"), *Pair.Key), Hashes.FindRef(Pair.Key), Pair.Value.Sha256);
	}

	// 磁盘上的资产参与去重：再导一次全是 Unchanged，不会因为没加载就重复新建。
	{
		const FDreamTextureImportResult Again = ImportOneFixture(Manifest, Files, Content.Root, /*bSave*/ true);
		TestTrue(FString::Printf(TEXT("读盘后再导成功（%s）"), *Again.Error), Again.bOK);
		TestEqual(TEXT("读盘后再导全部 Unchanged"), ImportCountAction(Again, EDreamTextureImportAction::Unchanged), 7);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
