// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureImportNotifier.h"

/*
 * DreamTexture.Editor.ImportLicense.Notice：导入完成的通知要说出「不可商用」。
 *
 * 不可商用的产出（CHORD 分解）在导入完成的那一刻就要让用户看到，别等东西用进项目里才发现。
 * 只在 manifest 明说 commercial_use=false 时才加这一行；可商用、或 manifest 根本没写的，都不加。
 * 通知内容拆成了纯数据（FNoticeContent），所以这里不依赖 Slate 就能断言文案。
 */
namespace
{
	FDreamTextureImportStatus MakeSucceeded(const FDreamTextureLicense& License)
	{
		FDreamTextureImportStatus Status;
		Status.MaterialId = TEXT("mat_20260827_777001_ref");
		Status.Stage = EDreamTextureImportStage::Succeeded;
		Status.Result.bOK = true;
		Status.Result.AssetFolder = TEXT("/Game/DreamTexture/MossyRiverRocks_1f53");
		Status.Result.License = License;

		FDreamTextureImportedTexture Tex;
		Tex.Action = EDreamTextureImportAction::Created;
		Status.Result.Textures.Add(Tex);

		Status.Result.MaterialInstance.Action = EDreamTextureImportAction::Created;
		Status.Result.MaterialInstance.ObjectPath = TEXT("/Game/DreamTexture/MossyRiverRocks_1f53/MI_MossyRiverRocks_1f53.MI_MossyRiverRocks_1f53");
		return Status;
	}

	FDreamTextureLicense MakeLicense(bool bKnown, bool bCommercial, const TCHAR* Reason)
	{
		FDreamTextureLicense License;
		License.bKnown = bKnown;
		License.bCommercialUse = bCommercial;
		License.Reason = Reason;
		return License;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureImportLicenseNoticeTest, "DreamTexture.Editor.ImportLicense.Notice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureImportLicenseNoticeTest::RunTest(const FString& Parameters)
{
	const TCHAR* ChordReason = TEXT("CHORD: Ubisoft ML License (research-only)");

	// 不可商用：通知里有一行授权说明，带原因，并且标出 bNonCommercial。
	{
		const FDreamTextureImportNotifier::FNoticeContent Content =
			FDreamTextureImportNotifier::Describe(MakeSucceeded(MakeLicense(true, false, ChordReason)), /*bCreateMaterialInstance*/ true);
		const FString Text = Content.Message.ToString();
		TestFalse(TEXT("导入成功不是错误"), Content.bIsError);
		TestTrue(TEXT("标出不可商用"), Content.bNonCommercial);
		TestTrue(TEXT("说了不可商用"), Text.Contains(TEXT("Not for commercial use")));
		TestTrue(TEXT("带着 manifest 里的原因"), Text.Contains(ChordReason));
		TestTrue(TEXT("授权单独占一行"), Text.Contains(TEXT("\nLicense: Not for commercial use")));
		TestTrue(TEXT("原来的内容还在：贴图数"), Text.Contains(TEXT("1 texture created")));
		TestTrue(TEXT("原来的内容还在：材质实例"), Text.Contains(TEXT("MI_MossyRiverRocks_1f53")));
		TestFalse(TEXT("授权是最后一行"), Text.Contains(FString(TEXT("commercial use (")) + ChordReason + TEXT(")\n")));
		TestFalse(TEXT("不影响选中材质实例"), Content.AssetToSelect.IsEmpty());
	}

	// 可商用：一个字都不加。
	{
		const FDreamTextureImportNotifier::FNoticeContent Content =
			FDreamTextureImportNotifier::Describe(MakeSucceeded(MakeLicense(true, true, TEXT("传统派生，无第三方限制"))), true);
		TestFalse(TEXT("可商用不标"), Content.bNonCommercial);
		TestFalse(TEXT("可商用不提授权"), Content.Message.ToString().Contains(TEXT("License")));
		TestFalse(TEXT("也不提商用"), Content.Message.ToString().Contains(TEXT("commercial")));
	}

	// manifest 没写：不替它下结论（没写不等于不能商用），也不加。
	{
		const FDreamTextureImportNotifier::FNoticeContent Content =
			FDreamTextureImportNotifier::Describe(MakeSucceeded(MakeLicense(false, false, TEXT(""))), true);
		TestFalse(TEXT("没写不标"), Content.bNonCommercial);
		TestFalse(TEXT("没写不提商用"), Content.Message.ToString().Contains(TEXT("commercial")));
	}

	// 不可商用但没有原因：照样说，不留一对空括号。
	{
		const FDreamTextureImportNotifier::FNoticeContent Content =
			FDreamTextureImportNotifier::Describe(MakeSucceeded(MakeLicense(true, false, TEXT("  "))), true);
		const FString Text = Content.Message.ToString();
		TestTrue(TEXT("标出不可商用"), Content.bNonCommercial);
		TestTrue(TEXT("说了不可商用"), Text.Contains(TEXT("License: Not for commercial use")));
		TestFalse(TEXT("没有空括号"), Text.Contains(TEXT("()")));
	}

	// 失败：只说失败原因，授权那行不是这时候的事。
	{
		FDreamTextureImportStatus Status = MakeSucceeded(MakeLicense(true, false, ChordReason));
		Status.Stage = EDreamTextureImportStage::Failed;
		Status.Message = TEXT("下载 basecolor 失败：校验不符");
		Status.Result.bOK = false;
		const FDreamTextureImportNotifier::FNoticeContent Content = FDreamTextureImportNotifier::Describe(Status, true);
		TestTrue(TEXT("失败是错误"), Content.bIsError);
		TestFalse(TEXT("失败不标授权"), Content.bNonCommercial);
		TestFalse(TEXT("失败不提授权"), Content.Message.ToString().Contains(TEXT("License")));
	}

	// DescribeLicense 本身的三种情形。
	TestTrue(TEXT("可商用返回空"), FDreamTextureImportNotifier::DescribeLicense(MakeLicense(true, true, TEXT("x"))).IsEmpty());
	TestTrue(TEXT("没写返回空"), FDreamTextureImportNotifier::DescribeLicense(MakeLicense(false, false, TEXT("x"))).IsEmpty());
	TestEqual(TEXT("不可商用的完整说法"), FDreamTextureImportNotifier::DescribeLicense(MakeLicense(true, false, ChordReason)).ToString(),
		FString(TEXT("License: Not for commercial use (CHORD: Ubisoft ML License (research-only))")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
