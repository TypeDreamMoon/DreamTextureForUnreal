// Copyright TypeDreamMoon. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureApi.h"
#include "DreamTextureHttpUtil.h"
#include "DreamTextureSettings.h"
#include "DreamTextureTestUtil.h"

#include "Misc/AutomationTest.h"

/*
 * REST 封装里不需要网络就能验证的部分：地址怎么拼、字符怎么编码。
 * 请求真的发出去、应答真的解析出来的部分由 DreamTexture.Live.Fetch 覆盖。
 */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTexturePercentEncodeTest, "DreamTexture.Client.Http.PercentEncode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTexturePercentEncodeTest::RunTest(const FString& Parameters)
{
	// RFC 3986 的 unreserved 原样保留。
	TestEqual(TEXT("unreserved 不编码"), DreamTexture::PercentEncode(TEXT("AZaz09-._~")), FString(TEXT("AZaz09-._~")));
	TestEqual(TEXT("素材 ID 原样"), DreamTexture::PercentEncode(DreamTextureTest::FixtureId()), DreamTextureTest::FixtureId());
	TestEqual(TEXT("空串"), DreamTexture::PercentEncode(FString()), FString());

	// 会破坏 URL 结构的字符必须被编码：路径分隔、查询分隔、空格、百分号本身。
	TestEqual(TEXT("斜杠"), DreamTexture::PercentEncode(TEXT("a/b")), FString(TEXT("a%2Fb")));
	TestEqual(TEXT("反斜杠"), DreamTexture::PercentEncode(TEXT("a\\b")), FString(TEXT("a%5Cb")));
	TestEqual(TEXT("问号与与号"), DreamTexture::PercentEncode(TEXT("a?b&c=d")), FString(TEXT("a%3Fb%26c%3Dd")));
	TestEqual(TEXT("空格"), DreamTexture::PercentEncode(TEXT("a b")), FString(TEXT("a%20b")));
	TestEqual(TEXT("百分号"), DreamTexture::PercentEncode(TEXT("100%")), FString(TEXT("100%25")));
	TestEqual(TEXT("加号不能被当成空格"), DreamTexture::PercentEncode(TEXT("a+b")), FString(TEXT("a%2Bb")));

	// 中文按 UTF-8 字节编码：苔 = E8 8B 94，藓 = E8 97 93。
	TestEqual(TEXT("中文"), DreamTexture::PercentEncode(TEXT("苔藓")), FString(TEXT("%E8%8B%94%E8%97%93")));

	// 增补平面字符在 UTF-16 里是代理对。逐个 TCHAR 转换（FGenericPlatformHttp::UrlEncode 的做法）会把它拆坏；
	// 整串转换要得到一个完整的 4 字节序列。U+1FAA8（岩石 emoji）= F0 9F AA A8。
	{
		FString Emoji;
		Emoji.AppendChar(static_cast<TCHAR>(0xD83E));
		Emoji.AppendChar(static_cast<TCHAR>(0xDEA8));
		TestEqual(TEXT("代理对合成一个 4 字节序列"), DreamTexture::PercentEncode(Emoji), FString(TEXT("%F0%9F%AA%A8")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureMakeFileUrlTest, "DreamTexture.Client.Http.MakeFileURL",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureMakeFileUrlTest::RunTest(const FString& Parameters)
{
	{
		// 基地址每次现取项目设置：改了立刻生效，不需要重启或重新创建任何对象。
		DreamTextureTest::FBackendUrlOverride Override(TEXT("http://127.0.0.1:18777/"));
		TestEqual(TEXT("正常地址（尾斜杠被去掉）"), FDreamTextureApi::MakeFileURL(DreamTextureTest::FixtureId(), TEXT("basecolor.png")),
			FString(TEXT("http://127.0.0.1:18777/api/materials/mat_20260827_777001_ref/files/basecolor.png")));

		// id 和文件名都做编码，里面的斜杠不能变成多出来的路径段。
		TestEqual(TEXT("斜杠被编码，不会变成额外的路径段"), FDreamTextureApi::MakeFileURL(TEXT("a/b"), TEXT("../c.png")),
			FString(TEXT("http://127.0.0.1:18777/api/materials/a%2Fb/files/..%2Fc.png")));
	}

	{
		DreamTextureTest::FBackendUrlOverride Override(TEXT("https://example.test:9000"));
		TestEqual(TEXT("换了地址立刻生效"), FDreamTextureApi::MakeFileURL(TEXT("m"), TEXT("f.png")),
			FString(TEXT("https://example.test:9000/api/materials/m/files/f.png")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
