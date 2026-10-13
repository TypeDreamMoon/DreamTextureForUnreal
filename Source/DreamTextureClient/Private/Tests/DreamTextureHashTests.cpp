// Copyright TypeDreamMoon. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureApi.h"
#include "DreamTextureManifest.h"
#include "DreamTextureSha256.h"
#include "DreamTextureTestUtil.h"

#include "Misc/AutomationTest.h"

/*
 * SHA-256 的正确性。DreamTextureSha256.cpp 是自己写的（理由见那个头文件），
 * 所以要从三个互相独立的方向证明它没错：
 *   1. NIST 公布的标准向量；
 *   2. 填充边界上的各个长度（期望值由 .NET 的 SHA256 算出，而不是这份实现）；
 *   3. 固定素材里每个文件的哈希，与后端（Go 的 crypto/sha256）写进 manifest 的值逐个对照。
 */

namespace
{
	FString FullHex(TConstArrayView<uint8> Data)
	{
		uint8 Digest[32];
		DreamTexture::Sha256(Data, Digest);

		FString Hex;
		for (const uint8 Byte : Digest)
		{
			Hex.Appendf(TEXT("%02x"), Byte);
		}
		return Hex;
	}

	FString FullHexOfString(const ANSICHAR* Text)
	{
		const int32 Length = FCStringAnsi::Strlen(Text);
		return FullHex(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Text), Length));
	}

	FString FullHexOfRepeatedA(int32 Count)
	{
		TArray<uint8> Data;
		Data.Init('a', Count);
		return FullHex(Data);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureSha256VectorsTest, "DreamTexture.Client.Hash.Sha256Vectors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureSha256VectorsTest::RunTest(const FString& Parameters)
{
	// FIPS 180-4 / NIST 示例向量。
	TestEqual(TEXT("空输入"), FullHexOfString(""),
		FString(TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));
	TestEqual(TEXT("\"abc\""), FullHexOfString("abc"),
		FString(TEXT("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
	TestEqual(TEXT("448 位的两块消息"), FullHexOfString("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
		FString(TEXT("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")));

	// 一百万个 'a'：跨很多块，验证多块链式处理（含 uint64 的比特长度字段）。
	TestEqual(TEXT("一百万个 a"), FullHexOfRepeatedA(1000000),
		FString(TEXT("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")));

	// 填充边界：剩余 55 字节时长度字段刚好塞进同一块，56 字节起必须多补一块；
	// 63/64/65 是块边界；119/120 和 127/128/129 是第二块上的同样边界。写错一般就错在这几个长度上。
	struct FBoundary
	{
		int32 Length;
		const TCHAR* Expected;
	};
	const FBoundary Boundaries[] = {
		{ 1, TEXT("ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb") },
		{ 55, TEXT("9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318") },
		{ 56, TEXT("b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a") },
		{ 57, TEXT("f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6") },
		{ 63, TEXT("7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34") },
		{ 64, TEXT("ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb") },
		{ 65, TEXT("635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0") },
		{ 119, TEXT("31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb") },
		{ 120, TEXT("2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c") },
		{ 127, TEXT("c57e9278af78fa3cab38667bef4ce29d783787a2f731d4e12200270f0c32320a") },
		{ 128, TEXT("6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e") },
		{ 129, TEXT("c12cb024a2e5551cca0e08fce8f1c5e314555cc3fef6329ee994a3db752166ae") },
	};
	for (const FBoundary& Case : Boundaries)
	{
		TestEqual(FString::Printf(TEXT("%d 个 a"), Case.Length), FullHexOfRepeatedA(Case.Length), FString(Case.Expected));
	}

	// HashBytes16 就是上面那个摘要的前 16 个十六进制字符。
	{
		const ANSICHAR* Text = "abc";
		const TConstArrayView<uint8> Abc(reinterpret_cast<const uint8*>(Text), 3);
		TestEqual(TEXT("HashBytes16(\"abc\")"), FDreamTextureFetcher::HashBytes16(Abc), FString(TEXT("ba7816bf8f01cfea")));
		TestEqual(TEXT("HashBytes16(空)"), FDreamTextureFetcher::HashBytes16(TConstArrayView<uint8>()), FString(TEXT("e3b0c44298fc1c14")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureHashFixtureTest, "DreamTexture.Client.Hash.Fixture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureHashFixtureTest::RunTest(const FString& Parameters)
{
	FString ManifestText;
	if (!TestTrue(TEXT("读得到固定素材的 manifest.json"), DreamTextureTest::LoadFixtureText(TEXT("manifest.json"), ManifestText)))
	{
		return false;
	}

	FDreamTextureManifest Manifest;
	FString Error;
	if (!TestTrue(TEXT("manifest 解析成功"), FDreamTextureManifest::FromJsonString(ManifestText, Manifest, Error)))
	{
		return false;
	}

	// 这里证明的是「我们算出来的 == 后端算出来写进 manifest 的」，对每个通道文件逐一对照。
	// bytes 一并核对，防止固定素材本身被谁换过却没重算 manifest。
	TestEqual(TEXT("固定素材共 7 个通道文件"), Manifest.Maps.Num(), 7);
	for (const TPair<FString, FDreamTextureMapInfo>& Pair : Manifest.Maps)
	{
		const FDreamTextureMapInfo& Info = Pair.Value;

		TArray<uint8> Bytes;
		if (!TestTrue(FString::Printf(TEXT("读得到 %s"), *Info.File), DreamTextureTest::LoadFixtureBytes(Info.File, Bytes)))
		{
			continue;
		}

		TestEqual(FString::Printf(TEXT("%s 的字节数与 manifest 一致"), *Info.File), static_cast<int64>(Bytes.Num()), Info.Bytes);
		TestEqual(FString::Printf(TEXT("%s 的 HashBytes16 与后端写的 sha256 一致"), *Info.File),
			FDreamTextureFetcher::HashBytes16(Bytes), Info.Sha256);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
