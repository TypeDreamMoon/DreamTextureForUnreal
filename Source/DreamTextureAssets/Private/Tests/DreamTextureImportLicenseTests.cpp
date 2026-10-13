// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureImporter.h"
#include "DreamTextureTestUtils.h"

using namespace DreamTextureTestUtils;

/*
 * DreamTexture.Assets.Import.License：导入结果带着 manifest 的许可（license_flags）。
 *
 * CHORD 分解的产出不可商用（Ubisoft ML License，research-only）。导入完成的通知要把这一点说出来，
 * 通知的素材是 FDreamTextureImportResult，所以导入器必须把 manifest 里的许可原样带到结果里——
 * 否则界面这一头什么都说不了。固定素材 mat_20260827_777001_ref 本身就是 CHORD 产出，manifest 里写着不可商用。
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureImportLicenseTest, "DreamTexture.Assets.Import.License",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureImportLicenseTest::RunTest(const FString& Parameters)
{
	FDreamTextureManifest Manifest;
	TMap<FString, FString> Files;
	FString Error;
	const bool bLoaded = LoadFixture(Manifest, Files, Error);
	if (!TestTrue(FString::Printf(TEXT("读固定素材：%s"), *Error), bLoaded))
	{
		return false;
	}

	FScopedTestContent Content;

	auto Import = [&Content](const FDreamTextureManifest& InManifest, const TMap<FString, FString>& InFiles)
	{
		FDreamTextureImportRequest Request;
		Request.Manifest = InManifest;
		Request.LocalFiles = InFiles;
		Request.ContentRoot = Content.Root;
		Request.bSave = false;
		return FDreamTextureImporter::Import(Request);
	};

	// 固定素材：manifest 明写不可商用，原因照搬。
	TestTrue(TEXT("前提：固定素材的 manifest 写了许可"), Manifest.License.bKnown);
	TestFalse(TEXT("前提：固定素材不可商用"), Manifest.License.bCommercialUse);
	{
		const FDreamTextureImportResult Result = Import(Manifest, Files);
		TestTrue(TEXT("导入成功"), Result.bOK);
		TestTrue(TEXT("结果里知道许可"), Result.License.bKnown);
		TestFalse(TEXT("结果里是不可商用"), Result.License.bCommercialUse);
		TestEqual(TEXT("原因原样带过来"), Result.License.Reason, Manifest.License.Reason);
		TestTrue(TEXT("原因里点名了 CHORD"), Result.License.Reason.Contains(TEXT("CHORD")));
	}

	// 可商用：也原样带，别被「默认不可商用」覆盖。
	{
		FDreamTextureManifest Commercial = Manifest;
		Commercial.License.bKnown = true;
		Commercial.License.bCommercialUse = true;
		Commercial.License.Reason = TEXT("传统派生，无第三方限制");
		const FDreamTextureImportResult Result = Import(Commercial, Files);
		TestTrue(TEXT("导入成功"), Result.bOK);
		TestTrue(TEXT("许可已知"), Result.License.bKnown);
		TestTrue(TEXT("可商用"), Result.License.bCommercialUse);
		TestEqual(TEXT("原因"), Result.License.Reason, Commercial.License.Reason);
	}

	// manifest 没写许可：结果里也是「未知」，不能凭空变成可商用或不可商用。
	{
		FDreamTextureManifest Unknown = Manifest;
		Unknown.License = FDreamTextureLicense();
		const FDreamTextureImportResult Result = Import(Unknown, Files);
		TestTrue(TEXT("导入成功"), Result.bOK);
		TestFalse(TEXT("许可未知"), Result.License.bKnown);
	}

	// 导入失败（缺材质 id）时也带着：界面按需取用，不必猜。
	{
		FDreamTextureManifest NoId = Manifest;
		NoId.Id.Reset();
		const FDreamTextureImportResult Result = Import(NoId, Files);
		TestFalse(TEXT("缺 id 导入失败"), Result.bOK);
		TestTrue(TEXT("失败的结果里许可仍在"), Result.License.bKnown && !Result.License.bCommercialUse);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
