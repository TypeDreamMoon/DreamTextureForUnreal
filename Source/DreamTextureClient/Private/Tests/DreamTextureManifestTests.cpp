// Copyright TypeDreamMoon. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureManifest.h"
#include "DreamTextureTestUtil.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

/*
 * manifest 与素材列表行的解析。全部不连后端。
 *
 * 固定素材在 Tests/Fixtures/mat_20260827_777001_ref/，是后端 docs/example-material 缩成 64×64 的一套，
 * bytes / sha256 已按后端算法重算过——这里读到的就是后端真实产出的格式。
 */

namespace
{
	const TCHAR* const MinimalHeader = TEXT("\"schema\":\"dreamtexture/material-manifest\",\"schema_version\":1");

	FString MakeManifestJson(const FString& Body)
	{
		return FString::Printf(TEXT("{%s,%s}"), MinimalHeader, *Body);
	}

	/**
	 * 解析并把结论记进测试。不写成 TestTrue(Printf(..., *Error), FromJsonString(..., Error))：
	 * 两个实参的求值顺序没有保证，消息可能在 Error 被填上之前就拷走了一个空串。
	 */
	bool ParseExpectingSuccess(FAutomationTestBase& Test, const TCHAR* What, const FString& Json, FDreamTextureManifest& Out)
	{
		FString Error;
		const bool bOK = FDreamTextureManifest::FromJsonString(Json, Out, Error);
		Test.TestTrue(FString::Printf(TEXT("%s：%s"), What, *Error), bOK);
		return bOK;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureManifestFixtureTest, "DreamTexture.Client.Manifest.Fixture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureManifestFixtureTest::RunTest(const FString& Parameters)
{
	FString Text;
	if (!TestTrue(TEXT("读得到固定素材的 manifest.json"), DreamTextureTest::LoadFixtureText(TEXT("manifest.json"), Text)))
	{
		return false;
	}

	FDreamTextureManifest Manifest;
	if (!ParseExpectingSuccess(*this, TEXT("固定素材的 manifest 解析成功"), Text, Manifest))
	{
		return false;
	}

	TestEqual(TEXT("id"), Manifest.Id, DreamTextureTest::FixtureId());
	TestEqual(TEXT("name"), Manifest.Name, FString(TEXT("mossy river rocks")));
	TestEqual(TEXT("style"), Manifest.Style, FString(TEXT("realistic")));
	TestEqual(TEXT("workflow.id"), Manifest.WorkflowId, FString(TEXT("realistic-chord-v1")));
	TestEqual(TEXT("workflow.version"), Manifest.WorkflowVersion, 1);
	TestTrue(TEXT("prompt 带着原文"), Manifest.Prompt.StartsWith(TEXT("texture of large old broken rocks")));
	TestTrue(TEXT("negative 带着原文"), Manifest.Negative.StartsWith(TEXT("perspective, vignette")));
	TestEqual(TEXT("seed"), Manifest.Seed, static_cast<int64>(777001));
	TestEqual(TEXT("resolution"), Manifest.Resolution, 64);
	TestTrue(TEXT("tileable"), Manifest.bTileable);
	TestEqual(TEXT("preview"), Manifest.Preview, FString(TEXT("preview.png")));
	TestEqual(TEXT("created_at 原文"), Manifest.CreatedAt, FString(TEXT("2026-08-27T00:00:00+08:00")));
	TestFalse(TEXT("固定素材是本地生成的，没有 source"), Manifest.bHasCloudSource);
	TestTrue(TEXT("Raw 保留了整份 JSON"), Manifest.Raw.IsValid() && Manifest.Raw->HasField(TEXT("generator")));

	TestTrue(TEXT("license_flags 存在"), Manifest.License.bKnown);
	TestFalse(TEXT("CHORD 是 research-only，不可商用"), Manifest.License.bCommercialUse);
	TestTrue(TEXT("license 原因带着 CHORD"), Manifest.License.Reason.Contains(TEXT("CHORD")));

	// 7 个通道一个不少。期望值逐个写死，不从 manifest 里推——否则测试和被测代码会互相佐证。
	struct FExpectedMap
	{
		const TCHAR* Channel;
		const TCHAR* File;
		EDreamTextureColorSpace ColorSpace;
		int64 Bytes;
		const TCHAR* Sha256;
	};
	const FExpectedMap Expected[] = {
		{ TEXT("basecolor"), TEXT("basecolor.png"), EDreamTextureColorSpace::SRGB, 10601, TEXT("1c64396cbe363e5c") },
		{ TEXT("normal"), TEXT("normal.png"), EDreamTextureColorSpace::Linear, 9410, TEXT("b38d34f24d5ad0a9") },
		{ TEXT("roughness"), TEXT("roughness.png"), EDreamTextureColorSpace::Linear, 5313, TEXT("c72bde49c04d53fd") },
		{ TEXT("metallic"), TEXT("metallic.png"), EDreamTextureColorSpace::Linear, 858, TEXT("8b3088bc9abecd4e") },
		{ TEXT("ao"), TEXT("ao.png"), EDreamTextureColorSpace::Linear, 5009, TEXT("13aad6d8a5bcec1f") },
		{ TEXT("height"), TEXT("height.png"), EDreamTextureColorSpace::Linear, 6050, TEXT("8e5e141dde2cf577") },
		{ TEXT("orm"), TEXT("orm.png"), EDreamTextureColorSpace::Linear, 7524, TEXT("06bcaeef2f4dc56a") },
	};

	TestEqual(TEXT("maps 通道数"), Manifest.Maps.Num(), 7);
	for (const FExpectedMap& Want : Expected)
	{
		const FDreamTextureMapInfo* Info = Manifest.FindMap(Want.Channel);
		if (!TestNotNull(FString::Printf(TEXT("有 %s 通道"), Want.Channel), Info))
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("%s.channel"), Want.Channel), Info->Channel, FString(Want.Channel));
		TestEqual(FString::Printf(TEXT("%s.file"), Want.Channel), Info->File, FString(Want.File));
		TestTrue(FString::Printf(TEXT("%s.colorspace"), Want.Channel), Info->ColorSpace == Want.ColorSpace);
		TestEqual(FString::Printf(TEXT("%s.width"), Want.Channel), Info->Width, 64);
		TestEqual(FString::Printf(TEXT("%s.height"), Want.Channel), Info->Height, 64);
		TestEqual(FString::Printf(TEXT("%s.bytes"), Want.Channel), Info->Bytes, Want.Bytes);
		TestEqual(FString::Printf(TEXT("%s.sha256"), Want.Channel), Info->Sha256, FString(Want.Sha256));
	}

	// 只有法线有 y，只有 orm 有 packing；别的通道这两个字段必须是空的，
	// 不然导入端会对 basecolor 也去判断要不要翻 G。
	const FDreamTextureMapInfo* Normal = Manifest.FindMap(DreamTextureChannel::Normal);
	const FDreamTextureMapInfo* ORM = Manifest.FindMap(DreamTextureChannel::ORM);
	if (Normal != nullptr && ORM != nullptr)
	{
		TestEqual(TEXT("normal.y"), Normal->NormalY, FString(TEXT("directx")));
		TestEqual(TEXT("orm.packing"), ORM->Packing, FString(TEXT("R=ao,G=roughness,B=metallic")));
		for (const TPair<FString, FDreamTextureMapInfo>& Pair : Manifest.Maps)
		{
			if (Pair.Key != DreamTextureChannel::Normal)
			{
				TestTrue(FString::Printf(TEXT("%s 没有 y 字段"), *Pair.Key), Pair.Value.NormalY.IsEmpty());
			}
			if (Pair.Key != DreamTextureChannel::ORM)
			{
				TestTrue(FString::Printf(TEXT("%s 没有 packing 字段"), *Pair.Key), Pair.Value.Packing.IsEmpty());
			}
		}
	}

	TestNull(TEXT("没有的通道返回 nullptr"), Manifest.FindMap(TEXT("emissive")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureManifestToleranceTest, "DreamTexture.Client.Manifest.Tolerance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureManifestToleranceTest::RunTest(const FString& Parameters)
{
	// 可选字段全是 null：Go 的 nil 指针、nil map、nil 切片都会这样序列化，后端仓库里踩过两次。
	{
		const FString Json = MakeManifestJson(TEXT(
			"\"id\":\"mat_nulls\",\"name\":null,\"style\":null,\"workflow\":null,\"prompt\":null,\"negative\":null,"
			"\"seed\":null,\"resolution\":null,\"tileable\":null,\"reference\":null,\"maps\":null,\"preview\":null,"
			"\"created_at\":null,\"generator\":null,\"license_flags\":null,\"source\":null"));

		FDreamTextureManifest Manifest;
		if (ParseExpectingSuccess(*this, TEXT("全 null 也能解析"), Json, Manifest))
		{
			TestEqual(TEXT("id 仍然读到"), Manifest.Id, FString(TEXT("mat_nulls")));
			TestTrue(TEXT("name 为空"), Manifest.Name.IsEmpty());
			TestTrue(TEXT("workflow 为空"), Manifest.WorkflowId.IsEmpty() && Manifest.WorkflowVersion == 0);
			TestEqual(TEXT("seed 为 0"), Manifest.Seed, static_cast<int64>(0));
			TestEqual(TEXT("resolution 为 0"), Manifest.Resolution, 0);
			TestFalse(TEXT("tileable 为假"), Manifest.bTileable);
			TestEqual(TEXT("maps 为 null 时是空表"), Manifest.Maps.Num(), 0);
			TestTrue(TEXT("preview 为空"), Manifest.Preview.IsEmpty());
			TestFalse(TEXT("license_flags 缺失 = 不知道，而不是可商用"), Manifest.License.bKnown);
			TestFalse(TEXT("source 为 null 不算云端底图"), Manifest.bHasCloudSource);
		}
	}

	// 字段整个不存在，只剩 schema 两件套。
	{
		FDreamTextureManifest Manifest;
		ParseExpectingSuccess(*this, TEXT("只有 schema 两件套也能解析"), MakeManifestJson(TEXT("\"id\":\"mat_bare\"")), Manifest);
		TestEqual(TEXT("maps 缺失时是空表"), Manifest.Maps.Num(), 0);
		TestFalse(TEXT("没有 source"), Manifest.bHasCloudSource);
	}

	// 未知通道要保留，maps 里的脏条目要被丢掉而不是拖垮整个解析。
	{
		const FString Json = MakeManifestJson(TEXT(
			"\"id\":\"mat_unknown\",\"maps\":{"
			"\"emissive\":{\"file\":\"emissive.png\",\"colorspace\":\"srgb\",\"width\":8,\"height\":8,\"bytes\":10,\"sha256\":\"deadbeefdeadbeef\"},"
			"\"basecolor\":{\"file\":\"basecolor.png\",\"colorspace\":\"hdr-future\"},"
			"\"broken\":null,"
			"\"nofile\":{\"colorspace\":\"linear\"}}"));

		FDreamTextureManifest Manifest;
		if (ParseExpectingSuccess(*this, TEXT("带未知通道也能解析"), Json, Manifest))
		{
			const FDreamTextureMapInfo* Emissive = Manifest.FindMap(TEXT("emissive"));
			if (TestNotNull(TEXT("未知通道 emissive 被保留"), Emissive))
			{
				TestEqual(TEXT("emissive.file"), Emissive->File, FString(TEXT("emissive.png")));
				TestTrue(TEXT("emissive.colorspace"), Emissive->ColorSpace == EDreamTextureColorSpace::SRGB);
				TestEqual(TEXT("emissive.sha256"), Emissive->Sha256, FString(TEXT("deadbeefdeadbeef")));
			}

			const FDreamTextureMapInfo* Base = Manifest.FindMap(DreamTextureChannel::BaseColor);
			if (TestNotNull(TEXT("basecolor 在"), Base))
			{
				TestTrue(TEXT("不认识的 colorspace 值落到 Unknown，不猜"), Base->ColorSpace == EDreamTextureColorSpace::Unknown);
				TestTrue(TEXT("缺 sha256 时为空串"), Base->Sha256.IsEmpty());
				TestEqual(TEXT("缺 width 时为 0"), Base->Width, 0);
			}

			TestNull(TEXT("值为 null 的条目被丢弃"), Manifest.FindMap(TEXT("broken")));
			TestNull(TEXT("没有 file 的条目被丢弃"), Manifest.FindMap(TEXT("nofile")));
			TestEqual(TEXT("只剩两个有效通道"), Manifest.Maps.Num(), 2);
		}
	}

	// source 一出现就是云端底图，复现不了。
	{
		const FString Json = MakeManifestJson(TEXT(
			"\"id\":\"mat_cloud\",\"source\":{\"provider\":\"openai\",\"model\":\"gpt-image-2\",\"size\":\"1024x1024\","
			"\"cost_usd\":0.0473,\"flattened\":true}"));

		FDreamTextureManifest Manifest;
		if (ParseExpectingSuccess(*this, TEXT("带 source 的 manifest 能解析"), Json, Manifest))
		{
			TestTrue(TEXT("有 source -> bHasCloudSource"), Manifest.bHasCloudSource);
			TestTrue(TEXT("Raw 里还留着 source 的细节"), Manifest.Raw.IsValid() && Manifest.Raw->HasTypedField<EJson::Object>(TEXT("source")));
		}
	}

	// license_flags 存在且可商用，原因也要读到。
	{
		const FString Json = MakeManifestJson(TEXT(
			"\"id\":\"mat_lic\",\"license_flags\":{\"commercial_use\":true,\"reason\":\"自训模型\"}"));

		FDreamTextureManifest Manifest;
		if (ParseExpectingSuccess(*this, TEXT("带 license_flags 的能解析"), Json, Manifest))
		{
			TestTrue(TEXT("license 已知"), Manifest.License.bKnown);
			TestTrue(TEXT("commercial_use"), Manifest.License.bCommercialUse);
			TestEqual(TEXT("reason"), Manifest.License.Reason, FString(TEXT("自训模型")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureManifestSchemaGuardTest, "DreamTexture.Client.Manifest.SchemaGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureManifestSchemaGuardTest::RunTest(const FString& Parameters)
{
	// 每个失败用例都先把 Out 填成哨兵值，失败后它必须原封不动：调用方不该拿到半填的对象。
	struct FCase
	{
		const TCHAR* Name;
		FString Json;
		const TCHAR* ExpectedInError; // 中文原因里应当出现的片段
	};

	const TArray<FCase> Cases = {
		{ TEXT("schema 不对"), TEXT("{\"schema\":\"something/else\",\"schema_version\":1,\"id\":\"x\"}"), TEXT("something/else") },
		{ TEXT("缺 schema"), TEXT("{\"schema_version\":1,\"id\":\"x\"}"), TEXT("schema") },
		{ TEXT("schema 是 null"), TEXT("{\"schema\":null,\"schema_version\":1,\"id\":\"x\"}"), TEXT("schema") },
		{ TEXT("schema_version 为 2"), TEXT("{\"schema\":\"dreamtexture/material-manifest\",\"schema_version\":2,\"id\":\"x\"}"), TEXT("v2") },
		{ TEXT("schema_version 为 99"), TEXT("{\"schema\":\"dreamtexture/material-manifest\",\"schema_version\":99}"), TEXT("v99") },
		{ TEXT("缺 schema_version"), TEXT("{\"schema\":\"dreamtexture/material-manifest\",\"id\":\"x\"}"), TEXT("schema_version") },
		{ TEXT("schema_version 为 0"), TEXT("{\"schema\":\"dreamtexture/material-manifest\",\"schema_version\":0}"), TEXT("schema_version") },
		{ TEXT("不是合法 JSON"), TEXT("{\"schema\": "), TEXT("JSON") },
		{ TEXT("是数组不是对象"), TEXT("[1,2,3]"), TEXT("JSON") },
		{ TEXT("空串"), FString(), TEXT("JSON") },
	};

	for (const FCase& Case : Cases)
	{
		FDreamTextureManifest Manifest;
		Manifest.Id = TEXT("sentinel");
		FString Error;

		const bool bOK = FDreamTextureManifest::FromJsonString(Case.Json, Manifest, Error);
		TestFalse(FString::Printf(TEXT("%s -> 拒绝"), Case.Name), bOK);
		TestFalse(FString::Printf(TEXT("%s -> 给出原因"), Case.Name), Error.IsEmpty());
		TestTrue(FString::Printf(TEXT("%s -> 原因里提到「%s」（实际：%s）"), Case.Name, Case.ExpectedInError, *Error), Error.Contains(Case.ExpectedInError));
		TestEqual(FString::Printf(TEXT("%s -> 失败时不动 Out"), Case.Name), Manifest.Id, FString(TEXT("sentinel")));
	}

	// 原因会原样出现在面板上，所以要给出办法，不只是报个版本号。测试编辑器固定 -culture=en，这里比对英文源文。
	{
		FDreamTextureManifest Manifest;
		FString Error;
		FDreamTextureManifest::FromJsonString(TEXT("{\"schema\":\"dreamtexture/material-manifest\",\"schema_version\":2}"), Manifest, Error);
		TestTrue(FString::Printf(TEXT("版本过高的原因里说了怎么办（实际：%s）"), *Error), Error.Contains(TEXT("Upgrade the plugin")));
	}

	// 空指针不能崩。
	{
		FDreamTextureManifest Manifest;
		FString Error;
		TestFalse(TEXT("空对象 -> 拒绝"), FDreamTextureManifest::FromJson(nullptr, Manifest, Error));
		TestFalse(TEXT("空对象 -> 给出原因"), Error.IsEmpty());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureSummaryTest, "DreamTexture.Client.Manifest.Summary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureSummaryTest::RunTest(const FString& Parameters)
{
	auto Parse = [](const FString& Json, FDreamTextureMaterialSummary& Out) -> bool
	{
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Obj);
		return FDreamTextureMaterialSummary::FromJson(Obj, Out);
	};

	// 正常行，字段形状取自后端 store.Material 的 json 标签。
	{
		FDreamTextureMaterialSummary Row;
		const bool bOK = Parse(TEXT(
			"{\"id\":\"mat_20260827_092650_1f53\",\"name\":\"青苔岩石\",\"style\":\"realistic\",\"workflow_id\":\"realistic-chord-v1\","
			"\"prompt\":\"mossy rocks\",\"negative\":\"blurry\",\"seed\":2918374651,\"resolution\":1024,\"favorite\":true,"
			"\"tags\":[\"rock\",\"moss\"],\"created_at\":\"2026-08-27T09:26:50.123+08:00\"}"), Row);

		if (TestTrue(TEXT("正常行能解析"), bOK))
		{
			TestEqual(TEXT("id"), Row.Id, FString(TEXT("mat_20260827_092650_1f53")));
			TestEqual(TEXT("name 保留中文"), Row.Name, FString(TEXT("青苔岩石")));
			TestEqual(TEXT("style"), Row.Style, FString(TEXT("realistic")));
			TestEqual(TEXT("workflow_id"), Row.WorkflowId, FString(TEXT("realistic-chord-v1")));
			TestEqual(TEXT("prompt"), Row.Prompt, FString(TEXT("mossy rocks")));
			TestEqual(TEXT("seed 超过 int32 也不丢"), Row.Seed, static_cast<int64>(2918374651));
			TestEqual(TEXT("resolution"), Row.Resolution, 1024);
			TestTrue(TEXT("favorite"), Row.bFavorite);
			TestEqual(TEXT("tags 数量"), Row.Tags.Num(), 2);
			TestTrue(TEXT("tags 内容"), Row.Tags.Contains(TEXT("rock")) && Row.Tags.Contains(TEXT("moss")));

			// created_at 是带 +08:00 偏移的 RFC3339，解析后是 UTC：09:26:50.123+08:00 = 01:26:50.123Z。
			TestEqual(TEXT("created_at 换算成 UTC"), Row.CreatedAt, FDateTime(2026, 8, 27, 1, 26, 50, 123));
		}
	}

	// tags 为 null、缺失 created_at。
	{
		FDreamTextureMaterialSummary Row;
		const bool bOK = Parse(TEXT("{\"id\":\"mat_a\",\"name\":\"a\",\"tags\":null,\"favorite\":false}"), Row);
		if (TestTrue(TEXT("tags 为 null 也能解析"), bOK))
		{
			TestEqual(TEXT("tags 为空"), Row.Tags.Num(), 0);
			TestFalse(TEXT("favorite"), Row.bFavorite);
			TestEqual(TEXT("缺 created_at 时是默认值"), Row.CreatedAt.GetTicks(), static_cast<int64>(0));
		}
	}

	// tags 字段整个不存在（后端 omitempty 就是这样）。
	{
		FDreamTextureMaterialSummary Row;
		TestTrue(TEXT("没有 tags 字段也能解析"), Parse(TEXT("{\"id\":\"mat_b\"}"), Row));
		TestEqual(TEXT("tags 为空"), Row.Tags.Num(), 0);
	}

	// Go 的 time.Time 默认序列化成纳秒精度（RFC3339Nano），也要吃得下，并且 Z 后缀也认。
	{
		FDreamTextureMaterialSummary Row;
		TestTrue(TEXT("纳秒精度的 created_at"), Parse(TEXT("{\"id\":\"mat_c\",\"created_at\":\"2026-08-27T09:26:50.123456789+08:00\"}"), Row));
		TestEqual(TEXT("纳秒四舍五入到毫秒"), Row.CreatedAt, FDateTime(2026, 8, 27, 1, 26, 50, 123));

		FDreamTextureMaterialSummary Utc;
		TestTrue(TEXT("Z 结尾的 created_at"), Parse(TEXT("{\"id\":\"mat_d\",\"created_at\":\"2026-08-27T01:26:50Z\"}"), Utc));
		TestEqual(TEXT("Z 就是 UTC"), Utc.CreatedAt, FDateTime(2026, 8, 27, 1, 26, 50));
	}

	// 没有 id 的行要被拒绝，空对象也是。
	{
		FDreamTextureMaterialSummary Row;
		TestFalse(TEXT("缺 id -> 拒绝"), Parse(TEXT("{\"name\":\"no id\"}"), Row));
		TestFalse(TEXT("id 为空串 -> 拒绝"), Parse(TEXT("{\"id\":\"\"}"), Row));
		TestFalse(TEXT("id 为 null -> 拒绝"), Parse(TEXT("{\"id\":null}"), Row));
		TestFalse(TEXT("空对象指针 -> 拒绝"), FDreamTextureMaterialSummary::FromJson(nullptr, Row));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
