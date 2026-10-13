// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureAnalyzeTestUtils.h"
#include "DreamTextureMaterialAnalysis.h"

using namespace DreamTextureTestUtils;

/*
 * 平铺 / UV 展开的记忆（M9）：按材质资产路径记在 Saved/DreamTexture/ReflowOverrides.json 里，
 * 与 M7 的用户指认同一个文件、同一套存取，不写进用户的材质包。
 */

// ---------------------------------------------------------------------------
// 平铺 / UV 展开的记忆
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowUVKindTest, "DreamTexture.Assets.Reflow.UVKind", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowUVKindTest::RunTest(const FString& Parameters)
{
	FScopedOverrideFile Temp;
	const FString Mat = TEXT("/Game/Hero/MI_Hero.MI_Hero");
	const FString Prop = TEXT("/Game/Props/MI_Crate.MI_Crate");

	EDreamTextureUVKind Kind = EDreamTextureUVKind::Tiled;
	{
		FDreamTextureOverrideStore Store(Temp.Path);
		TestFalse(TEXT("没记过：查不到（按平铺处理）"), Store.FindUVKind(Mat, Kind));

		TestTrue(TEXT("记下 UV 展开"), Store.SetUVKind(Mat, EDreamTextureUVKind::Unwrapped));
		TestTrue(TEXT("查得到"), Store.FindUVKind(Mat, Kind) && Kind == EDreamTextureUVKind::Unwrapped);
		TestFalse(TEXT("按材质记，别的材质不受影响"), Store.FindUVKind(Prop, Kind));

		// 与用户指认共存于同一个文件。
		FDreamTextureRoleOverride Override;
		Override.Role = EDreamTextureRole::Normal;
		TestTrue(TEXT("同时记一条指认"), Store.Set(Mat, TEXT("/Game/Hero/T_X.T_X"), Override));
		TestTrue(TEXT("再记一个"), Store.SetUVKind(Prop, EDreamTextureUVKind::Unwrapped));
	}
	{
		FDreamTextureOverrideStore Reopened(Temp.Path);
		TestTrue(TEXT("重开：Hero 还是 UV 展开"), Reopened.FindUVKind(Mat, Kind) && Kind == EDreamTextureUVKind::Unwrapped);
		TestTrue(TEXT("重开：Crate 还是 UV 展开"), Reopened.FindUVKind(Prop, Kind) && Kind == EDreamTextureUVKind::Unwrapped);
		FDreamTextureRoleOverride Found;
		TestTrue(TEXT("重开：指认没被冲掉"), Reopened.Find(Mat, TEXT("/Game/Hero/T_X.T_X"), Found) && Found.Role == EDreamTextureRole::Normal);

		// 写指认不能把 UV 展开的记忆弄丢（Save 要把两类一起写）。
		FDreamTextureRoleOverride Another;
		Another.Role = EDreamTextureRole::Roughness;
		Reopened.Set(Prop, TEXT("/Game/Props/T_Y.T_Y"), Another);
		Reopened.Remove(Mat, TEXT("/Game/Hero/T_X.T_X"));
	}
	{
		FDreamTextureOverrideStore Again(Temp.Path);
		TestTrue(TEXT("改指认之后 UV 展开的记忆还在"), Again.FindUVKind(Mat, Kind) && Kind == EDreamTextureUVKind::Unwrapped);

		// 选回平铺：删掉条目（没有记录就是平铺），文件里不堆默认值。
		TestTrue(TEXT("选回平铺"), Again.SetUVKind(Mat, EDreamTextureUVKind::Tiled));
		TestFalse(TEXT("查不到了"), Again.FindUVKind(Mat, Kind));
		TestTrue(TEXT("对本来就是平铺的再选一次平铺：不报错"), Again.SetUVKind(TEXT("/Game/Never/MI_Never.MI_Never"), EDreamTextureUVKind::Tiled));
		TestFalse(TEXT("没有为它留条目"), Again.FindUVKind(TEXT("/Game/Never/MI_Never.MI_Never"), Kind));

		TestFalse(TEXT("空路径记不了"), Again.SetUVKind(FString(), EDreamTextureUVKind::Unwrapped));
		TestFalse(TEXT("原因不为空"), Again.GetLastError().IsEmpty());
	}
	{
		FDreamTextureOverrideStore Final(Temp.Path);
		TestFalse(TEXT("重开：Hero 回到平铺"), Final.FindUVKind(Mat, Kind));
		TestTrue(TEXT("重开：Crate 仍是 UV 展开"), Final.FindUVKind(Prop, Kind));
		FString Text;
		FFileHelper::LoadFileToString(Text, *Temp.Path);
		TestFalse(TEXT("文件里不再有 Hero（平铺不留条目，它的指认也删光了）"), Text.Contains(TEXT("MI_Hero")));
		TestTrue(TEXT("文件里有 uv_kinds"), Text.Contains(TEXT("uv_kinds")));
	}

	// ---- 读不懂的条目丢掉，不拖垮整个文件 ----
	{
		FScopedOverrideFile Hand;
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Hand.Path), true);
		FFileHelper::SaveStringToFile(TEXT("{\"version\":1,\"uv_kinds\":{\"/Game/A.A\":\"unwrapped\",\"/Game/B.B\":\"sideways\",\"/Game/C.C\":7,\"/Game/D.D\":\"tiled\"},\"materials\":{}}"), *Hand.Path);
		FDreamTextureOverrideStore Store(Hand.Path);
		TestTrue(TEXT("认得的留下"), Store.FindUVKind(TEXT("/Game/A.A"), Kind) && Kind == EDreamTextureUVKind::Unwrapped);
		TestFalse(TEXT("不认识的值丢掉"), Store.FindUVKind(TEXT("/Game/B.B"), Kind));
		TestFalse(TEXT("类型不对的丢掉"), Store.FindUVKind(TEXT("/Game/C.C"), Kind));
		TestFalse(TEXT("写成默认值的不留"), Store.FindUVKind(TEXT("/Game/D.D"), Kind));
	}

	// ---- 标识 ----
	TestEqual(TEXT("tiled"), FString(LexToString(EDreamTextureUVKind::Tiled)), FString(TEXT("tiled")));
	TestEqual(TEXT("unwrapped"), FString(LexToString(EDreamTextureUVKind::Unwrapped)), FString(TEXT("unwrapped")));
	EDreamTextureUVKind Parsed = EDreamTextureUVKind::Tiled;
	TestTrue(TEXT("解析，大小写不敏感"), LexTryParseString(Parsed, TEXT("UNWRAPPED")) && Parsed == EDreamTextureUVKind::Unwrapped);
	TestFalse(TEXT("解析不认识的"), LexTryParseString(Parsed, TEXT("uv")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
