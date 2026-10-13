// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureImporter.h"
#include "DreamTextureTestUtils.h"

using namespace DreamTextureTestUtils;

namespace
{
	FDreamTextureManifest MakeNamed(const TCHAR* Name, const TCHAR* Id)
	{
		FDreamTextureManifest Manifest;
		Manifest.Name = Name;
		Manifest.Id = Id;
		return Manifest;
	}
}

// ---------------------------------------------------------------------------
// 规则表
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAssetsRulesTest, "DreamTexture.Assets.Rules", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAssetsRulesTest::RunTest(const FString& Parameters)
{
	struct FExpected
	{
		const TCHAR* Channel;
		const TCHAR* Suffix;
		TextureCompressionSettings Compression;
		TextureGroup LODGroup;
		bool bSRGB;
	};

	// 这张表就是 manifest-v1.md「UE 导入映射」的抄本。改规则时两边一起改。
	const FExpected Table[] = {
		{ TEXT("basecolor"), TEXT("BC"), TC_Default, TEXTUREGROUP_World, true },
		{ TEXT("normal"), TEXT("N"), TC_Normalmap, TEXTUREGROUP_WorldNormalMap, false },
		{ TEXT("orm"), TEXT("ORM"), TC_Masks, TEXTUREGROUP_World, false },
		{ TEXT("height"), TEXT("H"), TC_Grayscale, TEXTUREGROUP_World, false },
		{ TEXT("roughness"), TEXT("R"), TC_Grayscale, TEXTUREGROUP_World, false },
		{ TEXT("metallic"), TEXT("M"), TC_Grayscale, TEXTUREGROUP_World, false },
		{ TEXT("ao"), TEXT("AO"), TC_Grayscale, TEXTUREGROUP_World, false },
	};

	for (const FExpected& Row : Table)
	{
		const FDreamTextureChannelRule* Rule = FDreamTextureChannelRule::Find(Row.Channel);
		if (!TestNotNull(FString::Printf(TEXT("通道 %s 应该有规则"), Row.Channel), Rule))
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("%s 后缀"), Row.Channel), Rule->Suffix, FString(Row.Suffix));
		TestEqual(FString::Printf(TEXT("%s 压缩设置"), Row.Channel), (int32)Rule->Compression.GetValue(), (int32)Row.Compression);
		TestEqual(FString::Printf(TEXT("%s LODGroup"), Row.Channel), (int32)Rule->LODGroup.GetValue(), (int32)Row.LODGroup);
		TestEqual(FString::Printf(TEXT("%s 默认 sRGB"), Row.Channel), Rule->bDefaultSRGB, Row.bSRGB);
		TestEqual(FString::Printf(TEXT("%s 资产名"), Row.Channel), FDreamTextureImporter::GetTextureAssetName(TEXT("Rocks_ab12"), Row.Channel), FString::Printf(TEXT("T_Rocks_ab12_%s"), Row.Suffix));
	}

	// 未知通道没有规则，也没有资产名——导入器据此把它记成 Skipped。
	TestNull(TEXT("未知通道没有规则"), FDreamTextureChannelRule::Find(TEXT("emissive")));
	TestNull(TEXT("大小写不同视为未知通道"), FDreamTextureChannelRule::Find(TEXT("BaseColor")));
	TestNull(TEXT("空通道名"), FDreamTextureChannelRule::Find(FString()));
	TestTrue(TEXT("未知通道资产名为空"), FDreamTextureImporter::GetTextureAssetName(TEXT("Rocks_ab12"), TEXT("emissive")).IsEmpty());

	return true;
}

// ---------------------------------------------------------------------------
// Slug 与路径
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAssetsSlugTest, "DreamTexture.Assets.Slug", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAssetsSlugTest::RunTest(const FString& Parameters)
{
	// 头文件注释里的两个例子。
	TestEqual(TEXT("普通名字"), FDreamTextureImporter::MakeSlug(MakeNamed(TEXT("mossy river rocks"), TEXT("mat_20260827_092650_1f53"))), FString(TEXT("MossyRiverRocks_1f53")));
	TestEqual(TEXT("固定素材"), FDreamTextureImporter::MakeSlug(MakeNamed(TEXT("mossy river rocks"), TEXT("mat_20260827_777001_ref"))), FString(TEXT("MossyRiverRocks_ref")));

	// 标点、连续分隔符、首尾空白：全部当分隔符。
	TestEqual(TEXT("标点与多余空白"), FDreamTextureImporter::MakeSlug(MakeNamed(TEXT("  wet-stone, (cobble)!!  wall "), TEXT("mat_1_2_aa"))), FString(TEXT("WetStoneCobbleWall_aa")));

	// 已经是驼峰或大写的词只动首字母，不压小写。
	TestEqual(TEXT("驼峰保持原样"), FDreamTextureImporter::MakeSlug(MakeNamed(TEXT("MossyRock PBR"), TEXT("mat_x_9"))), FString(TEXT("MossyRockPBR_9")));

	// 中文与英文混着写：中文是分隔符，英文留下。
	TestEqual(TEXT("中英混合"), FDreamTextureImporter::MakeSlug(MakeNamed(TEXT("苔藓 rocks 石头"), TEXT("mat_1_2_cd"))), FString(TEXT("Rocks_cd")));

	// 全中文：一个 ASCII 字母数字都不剩，改用 id 去掉 mat_ 前缀后的整段（保证唯一）。
	TestEqual(TEXT("全中文"), FDreamTextureImporter::MakeSlug(MakeNamed(TEXT("长满青苔的河石"), TEXT("mat_20260827_092650_1f53"))), FString(TEXT("Mat_20260827_092650_1f53")));
	TestEqual(TEXT("空名字"), FDreamTextureImporter::MakeSlug(MakeNamed(TEXT(""), TEXT("mat_1_2_ef"))), FString(TEXT("Mat_1_2_ef")));
	TestEqual(TEXT("全标点"), FDreamTextureImporter::MakeSlug(MakeNamed(TEXT("---!!!"), TEXT("mat_1_2_ef"))), FString(TEXT("Mat_1_2_ef")));

	// 没有 mat_ 前缀的 id：整段当 slug。非 ASCII 字母数字和重复/首尾的下划线被清掉。
	TestEqual(TEXT("无前缀 id"), FDreamTextureImporter::MakeSlug(MakeNamed(TEXT("石头"), TEXT("abc123"))), FString(TEXT("Mat_abc123")));
	TestEqual(TEXT("id 里混着中文与连字符"), FDreamTextureImporter::MakeSlug(MakeNamed(TEXT("石头"), TEXT("mat_测试_ab-cd"))), FString(TEXT("Mat_abcd")));
	TestEqual(TEXT("id 里有连续下划线与首尾下划线"), FDreamTextureImporter::MakeSlug(MakeNamed(TEXT("石头"), TEXT("mat___1__2_"))), FString(TEXT("Mat_1_2")));
	TestEqual(TEXT("id 为空"), FDreamTextureImporter::MakeSlug(MakeNamed(TEXT("石头"), TEXT(""))), FString(TEXT("Mat_unknown")));
	TestEqual(TEXT("id 只有 mat_"), FDreamTextureImporter::MakeSlug(MakeNamed(TEXT("石头"), TEXT("mat_"))), FString(TEXT("Mat_unknown")));
	TestEqual(TEXT("前缀不分大小写"), FDreamTextureImporter::MakeSlug(MakeNamed(TEXT("石头"), TEXT("MAT_20260827_1f53"))), FString(TEXT("Mat_20260827_1f53")));
	{
		const FString LongId(TEXT("mat_0123456789012345678901234567890123456789012345678901234567890123456789"));
		const FString Slug = FDreamTextureImporter::MakeSlug(MakeNamed(TEXT("石头"), *LongId));
		TestEqual(TEXT("病态的超长 id 截断到 48 个字符再加前缀"), Slug, FString(TEXT("Mat_012345678901234567890123456789012345678901234567")));
	}

	// 修的就是这个：同一批里 id 的最后一段相同、前面的时间不同的中文名变体，slug 必须不同。
	// 有英文名时规则不变（名字 + 尾段），所以同样的 id 在英文名下仍然只靠尾段区分——那是名字在帮忙。
	{
		const FString SlugA = FDreamTextureImporter::MakeSlug(MakeNamed(TEXT("长满青苔的河石"), TEXT("mat_20260827_092650_1f53")));
		const FString SlugB = FDreamTextureImporter::MakeSlug(MakeNamed(TEXT("长满青苔的河石"), TEXT("mat_20260827_092651_1f53")));
		TestNotEqual(TEXT("同名同尾段、时间差 1 秒的两个中文名变体 slug 不同"), SlugA, SlugB);

		TSet<FString> Slugs;
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Slugs.Add(FDreamTextureImporter::MakeSlug(MakeNamed(TEXT("一批中文提示词的变体"), *FString::Printf(TEXT("mat_20260827_0926%02d_1f53"), Index))));
		}
		TestEqual(TEXT("一批 8 个中文提示词的变体 slug 互不相同"), Slugs.Num(), 8);

		// 有英文名的规则不变。
		TestEqual(TEXT("有英文名：规则不变"), FDreamTextureImporter::MakeSlug(MakeNamed(TEXT("mossy river rocks"), TEXT("mat_20260827_092650_1f53"))), FString(TEXT("MossyRiverRocks_1f53")));
		// 中文名里夹着英文：英文留下，也走原规则。
		TestEqual(TEXT("中英混合仍走原规则"), FDreamTextureImporter::MakeSlug(MakeNamed(TEXT("苔藓 rocks 石头"), TEXT("mat_20260827_092650_1f53"))), FString(TEXT("Rocks_1f53")));
	}

	// 超长：名字部分截到 32 个字符。
	{
		const FString Long(TEXT("abcdefghij abcdefghij abcdefghij abcdefghij abcdefghij"));
		const FString Slug = FDreamTextureImporter::MakeSlug(MakeNamed(*Long, TEXT("mat_1_2_ab12")));
		FString NamePart, TailPart;
		TestTrue(TEXT("超长名字的 slug 能按下划线拆开"), Slug.Split(TEXT("_"), &NamePart, &TailPart));
		TestEqual(TEXT("名字部分正好 32 个字符"), NamePart.Len(), 32);
		TestTrue(TEXT("截断后仍然从第一个词开始"), NamePart.StartsWith(TEXT("AbcdefghijAbcdefghij")));
		TestEqual(TEXT("尾段不受影响"), TailPart, FString(TEXT("ab12")));
	}

	// id 没有下划线：整段 id 当尾段，同样只留 ASCII 字母数字。
	TestEqual(TEXT("id 没有下划线"), FDreamTextureImporter::MakeSlug(MakeNamed(TEXT("rocks"), TEXT("abc123"))), FString(TEXT("Rocks_abc123")));
	TestEqual(TEXT("id 尾部是下划线时取前一段"), FDreamTextureImporter::MakeSlug(MakeNamed(TEXT("rocks"), TEXT("mat_1_2_ab_"))), FString(TEXT("Rocks_ab")));
	TestEqual(TEXT("id 为空"), FDreamTextureImporter::MakeSlug(MakeNamed(TEXT("rocks"), TEXT(""))), FString(TEXT("Rocks_unknown")));
	{
		const FString LongId(TEXT("0123456789012345678901234567890123456789"));
		const FString Slug = FDreamTextureImporter::MakeSlug(MakeNamed(TEXT("rocks"), *LongId));
		TestEqual(TEXT("没有下划线的超长 id 也被截断"), Slug, FString(TEXT("Rocks_01234567890123456789012345678901")));
	}

	// 同一份 manifest 每次都落在同一个地方。
	{
		const FDreamTextureManifest Manifest = MakeNamed(TEXT("mossy river rocks"), TEXT("mat_20260827_777001_ref"));
		TestEqual(TEXT("slug 稳定"), FDreamTextureImporter::MakeSlug(Manifest), FDreamTextureImporter::MakeSlug(Manifest));
		TestEqual(TEXT("指定内容根"), FDreamTextureImporter::GetAssetFolder(Manifest, TEXT("/Game/Foo")), FString(TEXT("/Game/Foo/MossyRiverRocks_ref")));
		TestEqual(TEXT("内容根带尾斜杠"), FDreamTextureImporter::GetAssetFolder(Manifest, TEXT("/Game/Foo//")), FString(TEXT("/Game/Foo/MossyRiverRocks_ref")));
		TestEqual(TEXT("内容根漏了前导斜杠"), FDreamTextureImporter::GetAssetFolder(Manifest, TEXT("Game/Foo")), FString(TEXT("/Game/Foo/MossyRiverRocks_ref")));
		TestEqual(TEXT("内容根用了反斜杠"), FDreamTextureImporter::GetAssetFolder(Manifest, TEXT("\\Game\\Foo")), FString(TEXT("/Game/Foo/MossyRiverRocks_ref")));
		// 内容根留空 = 设置里的 ImportRoot，默认 /Game/DreamTexture。
		TestTrue(TEXT("空内容根取设置里的 ImportRoot"), FDreamTextureImporter::GetAssetFolder(Manifest).EndsWith(TEXT("/MossyRiverRocks_ref")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
