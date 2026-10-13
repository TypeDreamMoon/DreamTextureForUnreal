// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureAnalyzeTestUtils.h"
#include "DreamTextureMaterialBuilder.h"
#include "DreamTextureMaterialTestUtils.h"
#include "Editor.h"
#include "Materials/MaterialExpressionBumpOffset.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Components/StaticMeshComponent.h"

using namespace DreamTextureTestUtils;

/*
 * 材质解析（M7）：用代码在测试内容目录里搭材质，断言每张贴图认成了什么、依据是什么。
 *
 * 全部不需要着色器（-nullrhi 能跑）：解析读的是表达式图，不是编译结果。
 */

namespace
{
	FString AnalyzeId(EDreamTextureRole Role)
	{
		return LexToString(Role);
	}

	FString AnalyzeId(EDreamTextureEvidence Evidence)
	{
		return LexToString(Evidence);
	}

	/** 一行的结论是否符合预期（角色与依据），失败时消息里写出实际值。 */
	bool ExpectRow(FAutomationTestBase& Test, const FString& What, const FDreamTextureMaterialTexture* Row, EDreamTextureRole Role, EDreamTextureEvidence Evidence)
	{
		if (!Test.TestNotNull(FString::Printf(TEXT("%s：解析结果里有这一行"), *What), Row))
		{
			return false;
		}
		bool bOK = true;
		bOK &= Test.TestEqual(FString::Printf(TEXT("%s：角色（依据：%s）"), *What, *Row->EvidenceDetail), AnalyzeId(Row->Role), AnalyzeId(Role));
		bOK &= Test.TestEqual(FString::Printf(TEXT("%s：依据"), *What), AnalyzeId(Row->Evidence), AnalyzeId(Evidence));
		return bOK;
	}

	bool HasUse(const FDreamTextureMaterialTexture& Row, EDreamTextureSink Sink, uint8 Channels)
	{
		return Row.Uses.ContainsByPredicate([Sink, Channels](const FDreamTextureTextureUse& Use) { return Use.Sink == Sink && Use.Channels == Channels; });
	}

	FDreamTextureImportResult ImportFixtureInto(const FDreamTextureManifest& Full, const TMap<FString, FString>& AllFiles, const FString& Root, std::initializer_list<const TCHAR*> Drop = {})
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
		Request.bSave = false;
		return FDreamTextureImporter::Import(Request);
	}
}

// ---------------------------------------------------------------------------
// 图遍历：直接连属性
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAnalyzeGraphDirectTest, "DreamTexture.Assets.Analyze.Graph.Direct", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAnalyzeGraphDirectTest::RunTest(const FString& Parameters)
{
	FScopedTestContent Content;
	// 贴图名故意取得毫无含义：这里只验证「连到哪个属性」这条依据，不能让名字抢了功劳。
	UTexture2D* A = MakeColorTexture(Content.Root, TEXT("TexA"), 11);
	UTexture2D* B = MakeNormalTexture(Content.Root, TEXT("TexB"), 12);
	UTexture2D* C = MakeGrayTexture(Content.Root, TEXT("TexC"), 13);
	UTexture2D* D = MakeGrayTexture(Content.Root, TEXT("TexD"), 14);
	UTexture2D* E = MakeGrayTexture(Content.Root, TEXT("TexE"), 15);
	UTexture2D* F = MakeColorTexture(Content.Root, TEXT("TexF"), 16);
	if (!TestTrue(TEXT("造好测试贴图"), A && B && C && D && E && F))
	{
		return false;
	}

	UMaterial* Material = MakeTestMaterial(Content.Root, TEXT("M_Direct"));
	FGraph G(Material);
	G.Connect(MP_BaseColor, G.Sample(A, SAMPLERTYPE_Color), SampleOut::RGB);
	G.Connect(MP_Normal, G.Sample(B, SAMPLERTYPE_Normal), SampleOut::RGB);
	G.Connect(MP_Roughness, G.Sample(C, SAMPLERTYPE_LinearGrayscale), SampleOut::RGB);
	G.Connect(MP_Metallic, G.Sample(D, SAMPLERTYPE_LinearGrayscale), SampleOut::R);
	G.Connect(MP_AmbientOcclusion, G.Sample(E, SAMPLERTYPE_LinearGrayscale), SampleOut::RGB);
	G.Connect(MP_EmissiveColor, G.Sample(F, SAMPLERTYPE_Color), SampleOut::RGB);
	G.Finish();

	const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(Material);
	if (!TestTrue(FString::Printf(TEXT("解析成功（%s）"), *Analysis.Error), Analysis.bOK))
	{
		return false;
	}
	TestFalse(TEXT("这是材质不是实例"), Analysis.bIsInstance);
	TestEqual(TEXT("六张贴图各一行"), Analysis.Textures.Num(), 6);

	ExpectRow(*this, TEXT("底色"), FindRowByName(Analysis, TEXT("TexA")), EDreamTextureRole::BaseColor, EDreamTextureEvidence::MaterialGraph);
	ExpectRow(*this, TEXT("法线"), FindRowByName(Analysis, TEXT("TexB")), EDreamTextureRole::Normal, EDreamTextureEvidence::MaterialGraph);
	ExpectRow(*this, TEXT("粗糙度（灰度图接 RGB 输出）"), FindRowByName(Analysis, TEXT("TexC")), EDreamTextureRole::Roughness, EDreamTextureEvidence::MaterialGraph);
	ExpectRow(*this, TEXT("金属度（接 R 输出）"), FindRowByName(Analysis, TEXT("TexD")), EDreamTextureRole::Metallic, EDreamTextureEvidence::MaterialGraph);
	ExpectRow(*this, TEXT("AO"), FindRowByName(Analysis, TEXT("TexE")), EDreamTextureRole::AO, EDreamTextureEvidence::MaterialGraph);
	ExpectRow(*this, TEXT("自发光"), FindRowByName(Analysis, TEXT("TexF")), EDreamTextureRole::Emissive, EDreamTextureEvidence::MaterialGraph);

	if (const FDreamTextureMaterialTexture* Base = FindRowByName(Analysis, TEXT("TexA")))
	{
		TestTrue(TEXT("底色的使用处：BaseColor ← RGB"), HasUse(*Base, EDreamTextureSink::BaseColor, DreamTextureChannelBit::RGB));
		TestTrue(TEXT("没有参数名（直接引用）"), Base->Parameters.Num() == 0);
		TestTrue(TEXT("贴图有源数据，能导出"), Base->bExportable && Base->ExportBlocker.IsEmpty());
		TestFalse(TEXT("不需要用户指认"), Base->bNeedsUserInput);
		TestEqual(TEXT("manifest 通道"), Base->GetManifestChannel(), FString(TEXT("basecolor")));
		TestEqual(TEXT("源数据宽"), Base->SourceWidth, 4);
	}
	if (const FDreamTextureMaterialTexture* Metal = FindRowByName(Analysis, TEXT("TexD")))
	{
		TestTrue(TEXT("金属度的使用处：Metallic ← R"), HasUse(*Metal, EDreamTextureSink::Metallic, DreamTextureChannelBit::R));
	}
	return true;
}

// ---------------------------------------------------------------------------
// 图遍历：穿过乘法 / 插值 / 通道遮罩，打包图
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAnalyzeGraphPassThroughTest, "DreamTexture.Assets.Analyze.Graph.PassThrough", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAnalyzeGraphPassThroughTest::RunTest(const FString& Parameters)
{
	FScopedTestContent Content;
	UTexture2D* Albedo = MakeColorTexture(Content.Root, TEXT("TexAlbedo"), 21);
	UTexture2D* Packed = MakeMasksTexture(Content.Root, TEXT("TexPacked"), 22);
	if (!TestTrue(TEXT("造好测试贴图"), Albedo && Packed))
	{
		return false;
	}

	UMaterial* Material = MakeTestMaterial(Content.Root, TEXT("M_PassThrough"));
	FGraph G(Material);

	// 底色：贴图 × 标量染色。
	UMaterialExpressionTextureSample* AlbedoSample = G.Sample(Albedo, SAMPLERTYPE_Color);
	G.Connect(MP_BaseColor, G.Multiply(AlbedoSample, SampleOut::RGB, G.Scalar(TEXT("Tint"), 1.f)), 0);

	// 同一张打包图的三条用法：
	//   粗糙度 = Lerp(0.2, 0.9, ORM.G)          —— 重映射，Alpha 才是数据
	//   金属度 = ComponentMask(B)(ORM.RGB) × 标量  —— 先 RGB 输出，再用通道遮罩取 B
	//   AO     = Lerp(1, ORM.R, AOStrength)     —— 标量 Alpha
	UMaterialExpressionTextureSample* PackedSample = G.Sample(Packed, SAMPLERTYPE_Masks);
	G.Connect(MP_Roughness, G.Lerp(G.Const(0.2f), 0, G.Const(0.9f), 0, PackedSample, SampleOut::G), 0);
	UMaterialExpressionComponentMask* BlueOnly = G.Mask(PackedSample, SampleOut::RGB, false, false, true, false);
	G.Connect(MP_Metallic, G.Multiply(BlueOnly, 0, G.Scalar(TEXT("MetalScale"), 1.f)), 0);
	G.Connect(MP_AmbientOcclusion, G.Lerp(G.Const(1.f), 0, PackedSample, SampleOut::R, G.Scalar(TEXT("AOStrength"), 1.f), 0), 0);
	G.Finish();

	const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(Material);
	if (!TestTrue(FString::Printf(TEXT("解析成功（%s）"), *Analysis.Error), Analysis.bOK))
	{
		return false;
	}
	TestEqual(TEXT("两张贴图两行（同一张图的三处使用合并成一行）"), Analysis.Textures.Num(), 2);

	ExpectRow(*this, TEXT("乘了染色的底色"), FindRowByName(Analysis, TEXT("TexAlbedo")), EDreamTextureRole::BaseColor, EDreamTextureEvidence::MaterialGraph);

	const FDreamTextureMaterialTexture* Row = FindRowByName(Analysis, TEXT("TexPacked"));
	if (ExpectRow(*this, TEXT("打包图"), Row, EDreamTextureRole::Packed, EDreamTextureEvidence::MaterialGraph))
	{
		TestEqual(TEXT("排布：R=AO，G=Roughness，B=Metallic"), Row->Layout.ToString(), FString(TEXT("R=ao,G=roughness,B=metallic,A=unknown")));
		TestFalse(TEXT("RGB 都认出来了，不需要用户指认"), Row->bNeedsUserInput);
		TestEqual(TEXT("恰好是标准 ORM"), Row->GetManifestChannel(), FString(TEXT("orm")));
		TestTrue(TEXT("粗糙度 ← G（穿过 Lerp 的 Alpha）"), HasUse(*Row, EDreamTextureSink::Roughness, DreamTextureChannelBit::G));
		TestTrue(TEXT("金属度 ← B（穿过通道遮罩与乘法）"), HasUse(*Row, EDreamTextureSink::Metallic, DreamTextureChannelBit::B));
		TestTrue(TEXT("AO ← R（穿过 Lerp 的 B 输入）"), HasUse(*Row, EDreamTextureSink::AmbientOcclusion, DreamTextureChannelBit::R));
	}
	return true;
}

// ---------------------------------------------------------------------------
// 图遍历：Lerp 的 Alpha 是混合权重时不是数据
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAnalyzeGraphBlendMaskTest, "DreamTexture.Assets.Analyze.Graph.BlendMask", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAnalyzeGraphBlendMaskTest::RunTest(const FString& Parameters)
{
	FScopedTestContent Content;
	UTexture2D* First = MakeColorTexture(Content.Root, TEXT("TexFirst"), 31);
	UTexture2D* Second = MakeColorTexture(Content.Root, TEXT("TexSecond"), 32);
	UTexture2D* Weight = MakeGrayTexture(Content.Root, TEXT("TexWeight"), 33);
	if (!TestTrue(TEXT("造好测试贴图"), First && Second && Weight))
	{
		return false;
	}

	UMaterial* Material = MakeTestMaterial(Content.Root, TEXT("M_Blend"));
	FGraph G(Material);
	UMaterialExpressionTextureSample* A = G.Sample(First, SAMPLERTYPE_Color);
	UMaterialExpressionTextureSample* B = G.Sample(Second, SAMPLERTYPE_Color);
	UMaterialExpressionTextureSample* W = G.Sample(Weight, SAMPLERTYPE_LinearGrayscale);
	G.Connect(MP_BaseColor, G.Lerp(A, SampleOut::RGB, B, SampleOut::RGB, W, SampleOut::R), 0);
	G.Finish();

	const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(Material);
	if (!TestTrue(FString::Printf(TEXT("解析成功（%s）"), *Analysis.Error), Analysis.bOK))
	{
		return false;
	}

	ExpectRow(*this, TEXT("混合的第一张"), FindRowByName(Analysis, TEXT("TexFirst")), EDreamTextureRole::BaseColor, EDreamTextureEvidence::MaterialGraph);
	ExpectRow(*this, TEXT("混合的第二张"), FindRowByName(Analysis, TEXT("TexSecond")), EDreamTextureRole::BaseColor, EDreamTextureEvidence::MaterialGraph);
	const FDreamTextureMaterialTexture* Row = FindRowByName(Analysis, TEXT("TexWeight"));
	if (ExpectRow(*this, TEXT("混合权重（两路都是贴图时，Alpha 不是底色数据）"), Row, EDreamTextureRole::Other, EDreamTextureEvidence::MaterialGraph))
	{
		TestTrue(TEXT("落点是 BlendMask"), HasUse(*Row, EDreamTextureSink::BlendMask, DreamTextureChannelBit::R));
	}
	return true;
}

// ---------------------------------------------------------------------------
// 图遍历：MaterialAttributes 模式
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAnalyzeGraphAttributesTest, "DreamTexture.Assets.Analyze.Graph.Attributes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAnalyzeGraphAttributesTest::RunTest(const FString& Parameters)
{
	FScopedTestContent Content;
	UTexture2D* Albedo = MakeColorTexture(Content.Root, TEXT("TexAlbedo"), 41);
	UTexture2D* Normal = MakeNormalTexture(Content.Root, TEXT("TexNormal"), 42);
	UTexture2D* Packed = MakeMasksTexture(Content.Root, TEXT("TexPacked"), 43);
	if (!TestTrue(TEXT("造好测试贴图"), Albedo && Normal && Packed))
	{
		return false;
	}

	UMaterial* Material = MakeTestMaterial(Content.Root, TEXT("M_Attributes"));
	Material->bUseMaterialAttributes = true;
	FGraph G(Material);

	UMaterialExpressionMakeMaterialAttributes* Make = G.Add<UMaterialExpressionMakeMaterialAttributes>();
	UMaterialExpressionTextureSample* PackedSample = G.Sample(Packed, SAMPLERTYPE_Masks);
	Make->BaseColor.Connect(SampleOut::RGB, G.Sample(Albedo, SAMPLERTYPE_Color));
	Make->Normal.Connect(SampleOut::RGB, G.Sample(Normal, SAMPLERTYPE_Normal));
	Make->Roughness.Connect(SampleOut::G, PackedSample);
	Make->Metallic.Connect(SampleOut::B, PackedSample);
	G.Connect(MP_MaterialAttributes, Make, 0);
	G.Finish();

	const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(Material);
	if (!TestTrue(FString::Printf(TEXT("解析成功（%s）"), *Analysis.Error), Analysis.bOK))
	{
		return false;
	}
	TestEqual(TEXT("三张贴图"), Analysis.Textures.Num(), 3);

	// MaterialAttributes 的每个属性只追它自己那一支：底色那支里的贴图不会被记到法线头上。
	const FDreamTextureMaterialTexture* AlbedoRow = FindRowByName(Analysis, TEXT("TexAlbedo"));
	if (ExpectRow(*this, TEXT("底色（经 Make）"), AlbedoRow, EDreamTextureRole::BaseColor, EDreamTextureEvidence::MaterialGraph))
	{
		TestEqual(TEXT("底色只落在 BaseColor 上"), AlbedoRow->Uses.Num(), 1);
	}
	ExpectRow(*this, TEXT("法线（经 Make）"), FindRowByName(Analysis, TEXT("TexNormal")), EDreamTextureRole::Normal, EDreamTextureEvidence::MaterialGraph);

	const FDreamTextureMaterialTexture* PackedRow = FindRowByName(Analysis, TEXT("TexPacked"));
	if (ExpectRow(*this, TEXT("打包图（经 Make）"), PackedRow, EDreamTextureRole::Packed, EDreamTextureEvidence::MaterialGraph))
	{
		TestEqual(TEXT("G=Roughness"), AnalyzeId(PackedRow->Layout.Channels[1]), AnalyzeId(EDreamTextureRole::Roughness));
		TestEqual(TEXT("B=Metallic"), AnalyzeId(PackedRow->Layout.Channels[2]), AnalyzeId(EDreamTextureRole::Metallic));
		TestEqual(TEXT("R 没人读，不知道是什么"), AnalyzeId(PackedRow->Layout.Channels[0]), AnalyzeId(EDreamTextureRole::Unknown));
		TestTrue(TEXT("于是需要用户指认"), PackedRow->bNeedsUserInput);
	}
	return true;
}

// ---------------------------------------------------------------------------
// UV 偏移：只在坐标里用到的图分不清是高度还是噪声——先猜，别的依据说得出就让位
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAnalyzeGraphUvOffsetTest, "DreamTexture.Assets.Analyze.Graph.UvOffset", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAnalyzeGraphUvOffsetTest::RunTest(const FString& Parameters)
{
	FScopedTestContent Content;
	UTexture2D* Surface = MakeColorTexture(Content.Root, TEXT("TexSurface"), 101);
	UTexture2D* Flow = MakeGrayTexture(Content.Root, TEXT("TexFlow"), 102);
	UTexture2D* Named = MakeGrayTexture(Content.Root, TEXT("TexNamed"), 103);
	UTexture2D* Bump = MakeGrayTexture(Content.Root, TEXT("TexBump"), 104);
	if (!TestTrue(TEXT("造好测试贴图"), Surface && Flow && Named && Bump))
	{
		return false;
	}

	UMaterial* Material = MakeTestMaterial(Content.Root, TEXT("M_UvOffset"));
	FGraph G(Material);
	UMaterialExpressionTextureSample* SurfaceSample = G.Sample(Surface, SAMPLERTYPE_Color);

	// 坐标 = 两张灰度图的和（一张参数名里没有任何线索，一张叫 HeightMap）再加 BumpOffset 的结果。
	UMaterialExpressionTextureSample* FlowSample = G.Sample(Flow, SAMPLERTYPE_LinearGrayscale);
	UMaterialExpressionTextureSampleParameter2D* NamedSample = G.Param(TEXT("HeightMap"), Named, SAMPLERTYPE_LinearGrayscale);
	UMaterialExpressionMultiply* Distort = G.Multiply(FlowSample, SampleOut::RGB, NamedSample, SampleOut::RGB);

	// BumpOffset 的 Height 引脚读的一定是高度图。
	UMaterialExpressionBumpOffset* BumpOffset = G.Add<UMaterialExpressionBumpOffset>();
	BumpOffset->Height.Connect(SampleOut::R, G.Sample(Bump, SAMPLERTYPE_LinearGrayscale));
	UMaterialExpressionMultiply* Coordinates = G.Multiply(Distort, 0, BumpOffset, 0);

	SurfaceSample->Coordinates.Connect(0, Coordinates);
	G.Connect(MP_BaseColor, SurfaceSample, SampleOut::RGB);
	G.Finish();

	const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(Material);
	if (!TestTrue(FString::Printf(TEXT("解析成功（%s）"), *Analysis.Error), Analysis.bOK))
	{
		return false;
	}

	ExpectRow(*this, TEXT("底色"), FindRowByName(Analysis, TEXT("TexSurface")), EDreamTextureRole::BaseColor, EDreamTextureEvidence::MaterialGraph);

	const FDreamTextureMaterialTexture* FlowRow = FindRowByName(Analysis, TEXT("TexFlow"));
	if (ExpectRow(*this, TEXT("只在 UV 里用、没有任何别的线索"), FlowRow, EDreamTextureRole::Height, EDreamTextureEvidence::MaterialGraph))
	{
		TestTrue(TEXT("落点是 UV 偏移"), FlowRow->Uses.ContainsByPredicate([](const FDreamTextureTextureUse& Use) { return Use.Sink == EDreamTextureSink::UVOffset; }));
		TestTrue(TEXT("Notes 里承认这是猜的"), FlowRow->Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("guessed")); }));
	}

	// 同样只在 UV 里用，但参数叫 HeightMap：名字比这个兜底的猜测更有说服力。
	ExpectRow(*this, TEXT("只在 UV 里用，参数名 HeightMap"), FindRowByName(Analysis, TEXT("TexNamed")), EDreamTextureRole::Height, EDreamTextureEvidence::ParameterName);

	const FDreamTextureMaterialTexture* BumpRow = FindRowByName(Analysis, TEXT("TexBump"));
	if (ExpectRow(*this, TEXT("BumpOffset 的 Height 引脚"), BumpRow, EDreamTextureRole::Height, EDreamTextureEvidence::MaterialGraph))
	{
		TestTrue(TEXT("落点是 Displacement，不是含糊的 UV"), HasUse(*BumpRow, EDreamTextureSink::Displacement, DreamTextureChannelBit::R));
		TestFalse(TEXT("确定的高度图，不带「猜」的提示"), BumpRow->Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("guessed")); }));
	}
	return true;
}

// ---------------------------------------------------------------------------
// 带 Alpha 的自发光图仍是自发光
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAnalyzeGraphEmissiveAlphaTest, "DreamTexture.Assets.Analyze.Graph.EmissiveAlpha", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAnalyzeGraphEmissiveAlphaTest::RunTest(const FString& Parameters)
{
	FScopedTestContent Content;
	UTexture2D* Glow = MakeColorTexture(Content.Root, TEXT("TexGlow"), 111);
	if (!TestNotNull(TEXT("造好测试贴图"), Glow))
	{
		return false;
	}

	UMaterial* Material = MakeTestMaterial(Content.Root, TEXT("M_EmissiveAlpha"));
	FGraph G(Material);
	UMaterialExpressionTextureSample* Sample = G.Sample(Glow, SAMPLERTYPE_Color);
	G.Connect(MP_EmissiveColor, Sample, SampleOut::RGB);
	G.Connect(MP_Opacity, Sample, SampleOut::A);
	G.Finish();

	const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(Material);
	if (!TestTrue(FString::Printf(TEXT("解析成功（%s）"), *Analysis.Error), Analysis.bOK))
	{
		return false;
	}
	const FDreamTextureMaterialTexture* Row = FindRowByName(Analysis, TEXT("TexGlow"));
	if (ExpectRow(*this, TEXT("RGB 当自发光、A 当不透明度"), Row, EDreamTextureRole::Emissive, EDreamTextureEvidence::MaterialGraph))
	{
		TestTrue(TEXT("Notes 里说了 A 通道的另一个用途"), Row->Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("opacity")); }));
	}
	return true;
}

// ---------------------------------------------------------------------------
// 打包图：只读了一个通道、用名字补全、名字与图矛盾
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAnalyzePackedPartialTest, "DreamTexture.Assets.Analyze.Packed.Partial", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAnalyzePackedPartialTest::RunTest(const FString& Parameters)
{
	FScopedTestContent Content;
	UTexture2D* Plain = MakeMasksTexture(Content.Root, TEXT("TexPlain"), 51);
	UTexture2D* ByName = MakeMasksTexture(Content.Root, TEXT("TexByName"), 52);
	UTexture2D* Clash = MakeMasksTexture(Content.Root, TEXT("TexClash"), 53);
	if (!TestTrue(TEXT("造好测试贴图"), Plain && ByName && Clash))
	{
		return false;
	}

	UMaterial* Material = MakeTestMaterial(Content.Root, TEXT("M_PackedPartial"));
	FGraph G(Material);

	// 三张都只有 G 通道被当粗糙度用，其余通道是什么材质没说。区别只在参数名：
	//   Plain  没有参数名可参考            → 排布推不出来，需要用户指认
	//   MRA    R=Metal G=Rough B=AO        → 与图里 G=Roughness 相容，补全
	//   RMA    R=Rough G=Metal B=AO        → 与图里 G=Roughness 矛盾，不采用
	UMaterialExpressionTextureSample* PlainSample = G.Sample(Plain, SAMPLERTYPE_Masks);
	UMaterialExpressionTextureSampleParameter2D* MraSample = G.Param(TEXT("MRA"), ByName, SAMPLERTYPE_Masks);
	UMaterialExpressionTextureSampleParameter2D* RmaSample = G.Param(TEXT("RMA"), Clash, SAMPLERTYPE_Masks);
	UMaterialExpressionMultiply* Chain = G.Multiply(G.Multiply(PlainSample, SampleOut::G, MraSample, SampleOut::G), 0, RmaSample, SampleOut::G);
	G.Connect(MP_Roughness, Chain, 0);
	G.Finish();

	const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(Material);
	if (!TestTrue(FString::Printf(TEXT("解析成功（%s）"), *Analysis.Error), Analysis.bOK))
	{
		return false;
	}

	const FDreamTextureMaterialTexture* PlainRow = FindRowByName(Analysis, TEXT("TexPlain"));
	if (ExpectRow(*this, TEXT("只读 G 的打包图"), PlainRow, EDreamTextureRole::Packed, EDreamTextureEvidence::MaterialGraph))
	{
		TestEqual(TEXT("只知道 G"), PlainRow->Layout.ToString(), FString(TEXT("R=unknown,G=roughness,B=unknown,A=unknown")));
		TestTrue(TEXT("需要用户指认"), PlainRow->bNeedsUserInput);
		TestTrue(TEXT("不是标准 ORM，也就没有对应的 manifest 通道"), PlainRow->GetManifestChannel().IsEmpty());
	}

	const FDreamTextureMaterialTexture* MraRow = FindRowByName(Analysis, TEXT("TexByName"));
	if (ExpectRow(*this, TEXT("参数叫 MRA"), MraRow, EDreamTextureRole::Packed, EDreamTextureEvidence::MaterialGraph))
	{
		TestEqual(TEXT("名字补全了其余通道"), MraRow->Layout.ToString(), FString(TEXT("R=metallic,G=roughness,B=ao,A=unknown")));
		TestFalse(TEXT("补全之后不需要用户指认"), MraRow->bNeedsUserInput);
		TestTrue(TEXT("详情里说明了补全的来源"), MraRow->EvidenceDetail.Contains(TEXT("MRA")));
	}

	const FDreamTextureMaterialTexture* RmaRow = FindRowByName(Analysis, TEXT("TexClash"));
	if (ExpectRow(*this, TEXT("参数叫 RMA，但图里 G 是粗糙度"), RmaRow, EDreamTextureRole::Packed, EDreamTextureEvidence::MaterialGraph))
	{
		TestEqual(TEXT("矛盾的排布不采用"), RmaRow->Layout.ToString(), FString(TEXT("R=unknown,G=roughness,B=unknown,A=unknown")));
		TestTrue(TEXT("仍需用户指认"), RmaRow->bNeedsUserInput);
		TestTrue(TEXT("Notes 里点出了矛盾"), RmaRow->Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("contradicts")); }));
	}
	return true;
}

// ---------------------------------------------------------------------------
// 名字词表
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAnalyzeNamesTableTest, "DreamTexture.Assets.Analyze.Names.Table", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAnalyzeNamesTableTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		const TCHAR* Name;
		EDreamTextureRole Role;
		const TCHAR* Layout; // 空 = 不检查
	};
	const FCase Cases[] = {
		// 底色
		{ TEXT("BaseColor"), EDreamTextureRole::BaseColor, TEXT("") }, { TEXT("base_color"), EDreamTextureRole::BaseColor, TEXT("") },
		{ TEXT("BASE_COLOR"), EDreamTextureRole::BaseColor, TEXT("") }, { TEXT("baseColorTexture"), EDreamTextureRole::BaseColor, TEXT("") },
		{ TEXT("Albedo"), EDreamTextureRole::BaseColor, TEXT("") }, { TEXT("Diffuse"), EDreamTextureRole::BaseColor, TEXT("") },
		{ TEXT("Color"), EDreamTextureRole::BaseColor, TEXT("") }, { TEXT("T_Rock_BC"), EDreamTextureRole::BaseColor, TEXT("") },
		// 法线（"normal" 里藏着 "orm"，不能被当成打包图）
		{ TEXT("Normal"), EDreamTextureRole::Normal, TEXT("") }, { TEXT("NormalMap"), EDreamTextureRole::Normal, TEXT("") },
		{ TEXT("Nrm"), EDreamTextureRole::Normal, TEXT("") }, { TEXT("normal_tex"), EDreamTextureRole::Normal, TEXT("") },
		{ TEXT("DetailNormal"), EDreamTextureRole::Normal, TEXT("") }, { TEXT("T_Rock_N"), EDreamTextureRole::Normal, TEXT("") },
		// 打包图
		{ TEXT("ORM"), EDreamTextureRole::Packed, TEXT("R=ao,G=roughness,B=metallic,A=unknown") },
		{ TEXT("ARM"), EDreamTextureRole::Packed, TEXT("R=ao,G=roughness,B=metallic,A=unknown") },
		{ TEXT("MRA"), EDreamTextureRole::Packed, TEXT("R=metallic,G=roughness,B=ao,A=unknown") },
		{ TEXT("RMA"), EDreamTextureRole::Packed, TEXT("R=roughness,G=metallic,B=ao,A=unknown") },
		{ TEXT("T_Rock_ORM"), EDreamTextureRole::Packed, TEXT("R=ao,G=roughness,B=metallic,A=unknown") },
		{ TEXT("ORMMap"), EDreamTextureRole::Packed, TEXT("R=ao,G=roughness,B=metallic,A=unknown") },
		{ TEXT("Packed"), EDreamTextureRole::Packed, TEXT("R=unknown,G=unknown,B=unknown,A=unknown") },
		{ TEXT("RoughnessMetallic"), EDreamTextureRole::Packed, TEXT("R=unknown,G=unknown,B=unknown,A=unknown") },
		// 粗糙度 / 金属度 / AO / 高度
		{ TEXT("Roughness"), EDreamTextureRole::Roughness, TEXT("") }, { TEXT("Rough"), EDreamTextureRole::Roughness, TEXT("") },
		{ TEXT("T_Rock_R"), EDreamTextureRole::Roughness, TEXT("") },
		{ TEXT("Metallic"), EDreamTextureRole::Metallic, TEXT("") }, { TEXT("Metalness"), EDreamTextureRole::Metallic, TEXT("") },
		{ TEXT("AO"), EDreamTextureRole::AO, TEXT("") }, { TEXT("Occlusion"), EDreamTextureRole::AO, TEXT("") },
		{ TEXT("AmbientOcclusion"), EDreamTextureRole::AO, TEXT("") }, { TEXT("ambient_occlusion"), EDreamTextureRole::AO, TEXT("") },
		{ TEXT("Height"), EDreamTextureRole::Height, TEXT("") }, { TEXT("Displacement"), EDreamTextureRole::Height, TEXT("") },
		{ TEXT("Bump"), EDreamTextureRole::Height, TEXT("") },
		// 其它
		{ TEXT("EmissiveColor"), EDreamTextureRole::Emissive, TEXT("") },
		{ TEXT("Opacity"), EDreamTextureRole::Opacity, TEXT("") },
		{ TEXT("Specular"), EDreamTextureRole::Specular, TEXT("") },
	};
	for (const FCase& Case : Cases)
	{
		FDreamTextureNameMatch Match;
		if (TestTrue(FString::Printf(TEXT("“%s”能认出来"), Case.Name), FDreamTextureMaterialAnalyzer::MatchName(Case.Name, Match)))
		{
			TestEqual(FString::Printf(TEXT("“%s”的角色"), Case.Name), AnalyzeId(Match.Role), AnalyzeId(Case.Role));
			if (FCString::Strlen(Case.Layout) > 0)
			{
				TestEqual(FString::Printf(TEXT("“%s”的排布"), Case.Name), Match.Layout.ToString(), FString(Case.Layout));
			}
		}
	}

	// 认不出来的：不能乱猜。
	// - Tiling / Tint 不是任何通道；
	// - "normalize" 以 normal 开头但不是那个词；
	// - T_Rock_1f5d 是导入器 slug 的写法（id 尾段），末尾的 d 不是「底色」后缀。
	for (const TCHAR* Name : { TEXT("Tiling"), TEXT("Tint"), TEXT("normalize"), TEXT("T_Rock_1f5d"), TEXT("N"), TEXT("Texture"), TEXT("") })
	{
		FDreamTextureNameMatch Match;
		TestFalse(FString::Printf(TEXT("“%s”认不出来"), Name), FDreamTextureMaterialAnalyzer::MatchName(Name, Match));
	}

	// 分词：驼峰、下划线、数字边界。
	TestEqual(TEXT("分词：BaseColor"), FString::Join(FDreamTextureMaterialAnalyzer::TokenizeName(TEXT("BaseColor")), TEXT("|")), FString(TEXT("base|color")));
	TestEqual(TEXT("分词：ORMMap"), FString::Join(FDreamTextureMaterialAnalyzer::TokenizeName(TEXT("ORMMap")), TEXT("|")), FString(TEXT("orm|map")));
	TestEqual(TEXT("分词：T_Rock_1f5d"), FString::Join(FDreamTextureMaterialAnalyzer::TokenizeName(TEXT("T_Rock_1f5d")), TEXT("|")), FString(TEXT("t|rock|1|f|5|d")));
	return true;
}

// ---------------------------------------------------------------------------
// 名字 / 采样器类型兜底，以及图压过名字
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAnalyzeNamesEvidenceTest, "DreamTexture.Assets.Analyze.Names.Evidence", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAnalyzeNamesEvidenceTest::RunTest(const FString& Parameters)
{
	FScopedTestContent Content;
	UTexture2D* ForParam = MakeColorTexture(Content.Root, TEXT("TexX1"), 61);
	UTexture2D* ForAsset = MakeGrayTexture(Content.Root, TEXT("T_Wall_N"), 62);
	UTexture2D* NoClue = MakeGrayTexture(Content.Root, TEXT("T_Wall_1f5d"), 63);
	UTexture2D* ForSampler = MakeNormalTexture(Content.Root, TEXT("TexX2"), 64);
	UTexture2D* Wrong = MakeColorTexture(Content.Root, TEXT("TexX3"), 65);
	if (!TestTrue(TEXT("造好测试贴图"), ForParam && ForAsset && NoClue && ForSampler && Wrong))
	{
		return false;
	}

	UMaterial* Material = MakeTestMaterial(Content.Root, TEXT("M_Names"));
	FGraph G(Material);

	// 把前四张接到 PixelDepthOffset：它不是图里能给出结论的落点（既不是颜色也不是任何 PBR 通道），
	// 于是图没有意见，轮到名字和采样器类型。串成一条乘法链让它们都在同一个输入上。
	UMaterialExpressionTextureSampleParameter2D* P1 = G.Param(TEXT("Albedo"), ForParam, SAMPLERTYPE_Color);
	UMaterialExpressionTextureSample* P2 = G.Sample(ForAsset, SAMPLERTYPE_LinearGrayscale);
	UMaterialExpressionTextureSample* P3 = G.Sample(NoClue, SAMPLERTYPE_LinearGrayscale);
	UMaterialExpressionTextureSample* P4 = G.Sample(ForSampler, SAMPLERTYPE_Normal);
	UMaterialExpressionMultiply* Chain = G.Multiply(G.Multiply(G.Multiply(P1, SampleOut::R, P2, SampleOut::R), 0, P3, SampleOut::R), 0, P4, SampleOut::R);
	G.Connect(MP_PixelDepthOffset, Chain, 0);

	// 参数叫 Roughness，但材质把它接到了底色：图比名字准。
	UMaterialExpressionTextureSampleParameter2D* P5 = G.Param(TEXT("Roughness"), Wrong, SAMPLERTYPE_Color);
	G.Connect(MP_BaseColor, P5, SampleOut::RGB);
	G.Finish();

	const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(Material);
	if (!TestTrue(FString::Printf(TEXT("解析成功（%s）"), *Analysis.Error), Analysis.bOK))
	{
		return false;
	}

	ExpectRow(*this, TEXT("参数名 Albedo"), FindRowByName(Analysis, TEXT("TexX1")), EDreamTextureRole::BaseColor, EDreamTextureEvidence::ParameterName);
	ExpectRow(*this, TEXT("资产名 T_Wall_N"), FindRowByName(Analysis, TEXT("T_Wall_N")), EDreamTextureRole::Normal, EDreamTextureEvidence::AssetName);
	ExpectRow(*this, TEXT("采样器类型 Normal"), FindRowByName(Analysis, TEXT("TexX2")), EDreamTextureRole::Normal, EDreamTextureEvidence::SamplerType);

	const FDreamTextureMaterialTexture* Clue = FindRowByName(Analysis, TEXT("T_Wall_1f5d"));
	if (TestNotNull(TEXT("T_Wall_1f5d 在结果里"), Clue))
	{
		TestEqual(TEXT("什么依据都没有：Unknown"), AnalyzeId(Clue->Role), AnalyzeId(EDreamTextureRole::Unknown));
		TestEqual(TEXT("依据为 None"), AnalyzeId(Clue->Evidence), AnalyzeId(EDreamTextureEvidence::None));
		TestTrue(TEXT("需要用户指认"), Clue->bNeedsUserInput);
	}

	const FDreamTextureMaterialTexture* Conflict = FindRowByName(Analysis, TEXT("TexX3"));
	if (ExpectRow(*this, TEXT("图说底色、参数名说粗糙度"), Conflict, EDreamTextureRole::BaseColor, EDreamTextureEvidence::MaterialGraph))
	{
		TestTrue(TEXT("Notes 里记下了参数名的不同意见"), Conflict->Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("Roughness")) && Note.Contains(TEXT("disagrees")); }));
	}
	return true;
}

// ---------------------------------------------------------------------------
// 材质实例：覆盖的贴图参数、两层实例
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAnalyzeInstanceTest, "DreamTexture.Assets.Analyze.Instance.Layers", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAnalyzeInstanceTest::RunTest(const FString& Parameters)
{
	FScopedTestContent Content;
	UTexture2D* DefaultAlbedo = MakeColorTexture(Content.Root, TEXT("TexDefaultAlbedo"), 71);
	UTexture2D* DefaultNormal = MakeNormalTexture(Content.Root, TEXT("TexDefaultNormal"), 72);
	UTexture2D* OverrideAlbedo = MakeColorTexture(Content.Root, TEXT("TexOverrideAlbedo"), 73);
	UTexture2D* OverrideNormal = MakeNormalTexture(Content.Root, TEXT("TexOverrideNormal"), 74);
	if (!TestTrue(TEXT("造好测试贴图"), DefaultAlbedo && DefaultNormal && OverrideAlbedo && OverrideNormal))
	{
		return false;
	}

	UMaterial* Master = MakeTestMaterial(Content.Root, TEXT("M_Master"));
	{
		FGraph G(Master);
		G.Connect(MP_BaseColor, G.Param(TEXT("Surface"), DefaultAlbedo, SAMPLERTYPE_Color), SampleOut::RGB);
		G.Connect(MP_Normal, G.Param(TEXT("Bumps"), DefaultNormal, SAMPLERTYPE_Normal), SampleOut::RGB);
		G.Finish();
	}

	UMaterialInstanceConstant* Inner = MakeTestInstance(Content.Root, TEXT("MI_Inner"), Master);
	UMaterialInstanceConstant* Outer = MakeTestInstance(Content.Root, TEXT("MI_Outer"), Inner);
	if (!TestTrue(TEXT("造好两层实例"), Inner && Outer))
	{
		return false;
	}
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Inner, TEXT("Surface"), OverrideAlbedo);
	UMaterialEditingLibrary::UpdateMaterialInstance(Inner);
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Outer, TEXT("Bumps"), OverrideNormal);
	UMaterialEditingLibrary::UpdateMaterialInstance(Outer);

	// ---- 母材质自己：用默认贴图 ----
	{
		const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(Master);
		if (TestTrue(TEXT("母材质解析成功"), Analysis.bOK))
		{
			TestEqual(TEXT("母材质：两行"), Analysis.Textures.Num(), 2);
			TestNotNull(TEXT("母材质：默认底色在"), FindRowByName(Analysis, TEXT("TexDefaultAlbedo")));
			TestNotNull(TEXT("母材质：默认法线在"), FindRowByName(Analysis, TEXT("TexDefaultNormal")));
			TestEqual(TEXT("母材质：base 路径是自己"), Analysis.BaseMaterialPath, Analysis.MaterialPath);
		}
	}

	// ---- 内层实例：底色被它覆盖，法线继承默认 ----
	{
		const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(Inner);
		if (TestTrue(TEXT("内层实例解析成功"), Analysis.bOK))
		{
			TestTrue(TEXT("是实例"), Analysis.bIsInstance);
			TestEqual(TEXT("两行"), Analysis.Textures.Num(), 2);
			TestEqual(TEXT("母材质路径沿父链找到"), Analysis.BaseMaterialPath, Master->GetPathName());
			const FDreamTextureMaterialTexture* Surface = FindRowByParameter(Analysis, TEXT("Surface"));
			if (ExpectRow(*this, TEXT("内层：Surface 参数"), Surface, EDreamTextureRole::BaseColor, EDreamTextureEvidence::MaterialGraph))
			{
				TestEqual(TEXT("用的是实例覆盖的贴图"), Surface->AssetName, FString(TEXT("TexOverrideAlbedo")));
				TestTrue(TEXT("标明这是所选实例自己覆盖的"), Surface->bOverriddenBySelectedInstance);
			}
			const FDreamTextureMaterialTexture* Bumps = FindRowByParameter(Analysis, TEXT("Bumps"));
			if (ExpectRow(*this, TEXT("内层：Bumps 参数"), Bumps, EDreamTextureRole::Normal, EDreamTextureEvidence::MaterialGraph))
			{
				TestEqual(TEXT("继承母材质的默认贴图"), Bumps->AssetName, FString(TEXT("TexDefaultNormal")));
				TestFalse(TEXT("不是它自己覆盖的"), Bumps->bOverriddenBySelectedInstance);
			}
			TestNull(TEXT("被覆盖掉的默认底色不再出现"), FindRowByName(Analysis, TEXT("TexDefaultAlbedo")));
		}
	}

	// ---- 外层实例（父是内层）：底色从内层继承，法线自己覆盖 ----
	{
		const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(Outer);
		if (TestTrue(TEXT("外层实例解析成功"), Analysis.bOK))
		{
			TestEqual(TEXT("两行"), Analysis.Textures.Num(), 2);
			TestEqual(TEXT("母材质路径穿过两层"), Analysis.BaseMaterialPath, Master->GetPathName());
			const FDreamTextureMaterialTexture* Surface = FindRowByParameter(Analysis, TEXT("Surface"));
			if (TestNotNull(TEXT("外层：Surface 在"), Surface))
			{
				TestEqual(TEXT("外层：底色继承内层覆盖的贴图"), Surface->AssetName, FString(TEXT("TexOverrideAlbedo")));
				TestFalse(TEXT("外层：它不是外层自己覆盖的"), Surface->bOverriddenBySelectedInstance);
			}
			const FDreamTextureMaterialTexture* Bumps = FindRowByParameter(Analysis, TEXT("Bumps"));
			if (TestNotNull(TEXT("外层：Bumps 在"), Bumps))
			{
				TestEqual(TEXT("外层：法线用自己覆盖的贴图"), Bumps->AssetName, FString(TEXT("TexOverrideNormal")));
				TestTrue(TEXT("外层：是它自己覆盖的"), Bumps->bOverriddenBySelectedInstance);
			}
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// 本插件导入的贴图：来源记录；静态开关；视差函数
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAnalyzeRecordTest, "DreamTexture.Assets.Analyze.Record.Imported", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAnalyzeRecordTest::RunTest(const FString& Parameters)
{
	FDreamTextureManifest Manifest;
	TMap<FString, FString> Files;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("读固定素材：%s"), *Error), LoadFixture(Manifest, Files, Error)))
	{
		return false;
	}

	FScopedTestContent Content;
	const FDreamTextureImportResult Imported = ImportFixtureInto(Manifest, Files, Content.Root);
	if (!TestTrue(FString::Printf(TEXT("导入成功（%s）"), *Imported.Error), Imported.bOK))
	{
		return false;
	}
	const FDreamTextureImportedTexture Created = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Imported, Content.Root, false);
	UMaterialInstanceConstant* Instance = FindObject<UMaterialInstanceConstant>(nullptr, *Created.ObjectPath);
	if (!TestNotNull(FString::Printf(TEXT("材质实例建出来了（%s）"), *Created.Note), Instance))
	{
		return false;
	}

	const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(Instance);
	if (!TestTrue(FString::Printf(TEXT("解析成功（%s）"), *Analysis.Error), Analysis.bOK))
	{
		return false;
	}

	// 默认 UsePOM 关：高度图不会被采样，也就不出现。UseNormal / UseORM 开：法线与 ORM 在。
	TestEqual(TEXT("三行：底色、法线、ORM（高度图没被用到）"), Analysis.Textures.Num(), 3);
	TestNull(TEXT("高度图不在"), FindRowByParameter(Analysis, TEXT("Height")));

	const FDreamTextureMaterialTexture* BaseColor = FindRowByParameter(Analysis, TEXT("BaseColor"));
	ExpectRow(*this, TEXT("底色"), BaseColor, EDreamTextureRole::BaseColor, EDreamTextureEvidence::SourceRecord);
	const FDreamTextureMaterialTexture* Normal = FindRowByParameter(Analysis, TEXT("Normal"));
	ExpectRow(*this, TEXT("法线"), Normal, EDreamTextureRole::Normal, EDreamTextureEvidence::SourceRecord);
	const FDreamTextureMaterialTexture* Orm = FindRowByParameter(Analysis, TEXT("ORM"));
	if (ExpectRow(*this, TEXT("ORM"), Orm, EDreamTextureRole::Packed, EDreamTextureEvidence::SourceRecord))
	{
		TestEqual(TEXT("ORM 的排布"), Orm->Layout.ToString(), FDreamTextureChannelLayout::MakeORM().ToString());
		TestEqual(TEXT("manifest 通道"), Orm->GetManifestChannel(), FString(TEXT("orm")));
		TestFalse(TEXT("不需要用户指认"), Orm->bNeedsUserInput);
		TestTrue(TEXT("详情里写了材质 id"), Orm->EvidenceDetail.Contains(Manifest.Id));

		// 来源记录与材质图互相印证：母材质确实把 ORM 的 R/G/B 分别接到了 AO / 粗糙度 / 金属度。
		TestTrue(TEXT("图里 AO ← R"), HasUse(*Orm, EDreamTextureSink::AmbientOcclusion, DreamTextureChannelBit::R));
		TestTrue(TEXT("图里 Roughness ← G"), HasUse(*Orm, EDreamTextureSink::Roughness, DreamTextureChannelBit::G));
		TestTrue(TEXT("图里 Metallic ← B"), HasUse(*Orm, EDreamTextureSink::Metallic, DreamTextureChannelBit::B));
		TestEqual(TEXT("两个依据一致时没有冲突提示"), Orm->Notes.Num(), 0);
	}
	if (BaseColor != nullptr)
	{
		TestTrue(TEXT("底色图的使用处：BaseColor ← RGB（穿过了染色乘法）"), HasUse(*BaseColor, EDreamTextureSink::BaseColor, DreamTextureChannelBit::RGB));
		TestEqual(TEXT("没有冲突提示"), BaseColor->Notes.Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAnalyzeRecordSwitchTest, "DreamTexture.Assets.Analyze.Record.Switches", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAnalyzeRecordSwitchTest::RunTest(const FString& Parameters)
{
	FDreamTextureManifest Manifest;
	TMap<FString, FString> Files;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("读固定素材：%s"), *Error), LoadFixture(Manifest, Files, Error)))
	{
		return false;
	}

	// ---- 只有底色：实例把 UseNormal / UseORM 关了，法线与 ORM 的默认贴图不该出现 ----
	{
		FScopedTestContent Content;
		const FDreamTextureImportResult Imported = ImportFixtureInto(Manifest, Files, Content.Root, { TEXT("normal"), TEXT("orm"), TEXT("height"), TEXT("roughness"), TEXT("metallic"), TEXT("ao") });
		FDreamTextureManifest Reduced = Manifest;
		for (const TCHAR* Channel : { TEXT("normal"), TEXT("orm"), TEXT("height"), TEXT("roughness"), TEXT("metallic"), TEXT("ao") })
		{
			Reduced.Maps.Remove(Channel);
		}
		const FDreamTextureImportedTexture Created = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Reduced, Imported, Content.Root, false);
		UMaterialInstanceConstant* Instance = FindObject<UMaterialInstanceConstant>(nullptr, *Created.ObjectPath);
		if (TestNotNull(TEXT("只有底色：实例建出来了"), Instance))
		{
			const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(Instance);
			TestTrue(TEXT("只有底色：解析成功"), Analysis.bOK);
			TestEqual(TEXT("只有底色：只有一行（关掉的分支不算用到）"), Analysis.Textures.Num(), 1);
			ExpectRow(*this, TEXT("只有底色"), FindRowByParameter(Analysis, TEXT("BaseColor")), EDreamTextureRole::BaseColor, EDreamTextureEvidence::SourceRecord);
		}
	}

	// ---- 打开 UsePOM：高度图经视差函数用到，落点是 UV ----
	{
		FScopedTestContent Content;
		const FDreamTextureImportResult Imported = ImportFixtureInto(Manifest, Files, Content.Root);
		const FDreamTextureImportedTexture Created = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Imported, Content.Root, false);
		UMaterialInstanceConstant* Instance = FindObject<UMaterialInstanceConstant>(nullptr, *Created.ObjectPath);
		if (TestNotNull(TEXT("POM：实例建出来了"), Instance))
		{
			UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(Instance, DreamTextureMaterialParam::UsePOM, true, EMaterialParameterAssociation::GlobalParameter, true);

			const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(Instance);
			TestTrue(TEXT("POM：解析成功"), Analysis.bOK);
			const FDreamTextureMaterialTexture* Height = FindRowByParameter(Analysis, TEXT("Height"));
			if (ExpectRow(*this, TEXT("POM 的高度图"), Height, EDreamTextureRole::Height, EDreamTextureEvidence::SourceRecord))
			{
				// 视差函数的结果喂给采样坐标，函数名带 Parallax：里面读的贴图确定是高度图，落点升级成 Displacement。
				TestTrue(TEXT("高度图的使用处落在 Displacement 上（穿过了视差材质函数）"), Height->Uses.ContainsByPredicate([](const FDreamTextureTextureUse& Use) { return Use.Sink == EDreamTextureSink::Displacement; }));
			}
			TestEqual(TEXT("POM：底色、法线、ORM、高度共四行"), Analysis.Textures.Num(), 4);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAnalyzeRecordConflictTest, "DreamTexture.Assets.Analyze.Record.Conflict", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAnalyzeRecordConflictTest::RunTest(const FString& Parameters)
{
	FDreamTextureManifest Manifest;
	TMap<FString, FString> Files;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("读固定素材：%s"), *Error), LoadFixture(Manifest, Files, Error)))
	{
		return false;
	}

	FScopedTestContent Content;
	const FDreamTextureImportResult Imported = ImportFixtureInto(Manifest, Files, Content.Root);
	const FDreamTextureImportedTexture Created = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Imported, Content.Root, false);
	UMaterialInstanceConstant* Instance = FindObject<UMaterialInstanceConstant>(nullptr, *Created.ObjectPath);
	const FDreamTextureImportedTexture* OrmEntry = Imported.Textures.FindByPredicate([](const FDreamTextureImportedTexture& Entry) { return Entry.Channel == TEXT("orm"); });
	if (!TestTrue(TEXT("实例与 ORM 贴图都在"), Instance != nullptr && OrmEntry != nullptr))
	{
		return false;
	}
	UTexture* OrmTexture = FindTexture(OrmEntry->ObjectPath);
	if (!TestNotNull(TEXT("ORM 贴图对象在"), OrmTexture))
	{
		return false;
	}

	// 用户手滑，把 ORM 贴图塞进了 BaseColor 参数：贴图本身是什么（来源记录）不会变，但材质的用法变了，值得提醒。
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, DreamTextureMaterialParam::BaseColor, OrmTexture);
	UMaterialEditingLibrary::UpdateMaterialInstance(Instance);

	const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(Instance);
	if (!TestTrue(FString::Printf(TEXT("解析成功（%s）"), *Analysis.Error), Analysis.bOK))
	{
		return false;
	}

	const FDreamTextureMaterialTexture* Row = FindRowByParameter(Analysis, TEXT("BaseColor"));
	if (ExpectRow(*this, TEXT("塞进 BaseColor 的 ORM 贴图"), Row, EDreamTextureRole::Packed, EDreamTextureEvidence::SourceRecord))
	{
		TestTrue(TEXT("同一张贴图合并成一行，两个参数都挂在上面"), Row->HasParameter(TEXT("BaseColor")) && Row->HasParameter(TEXT("ORM")));
		TestTrue(TEXT("Notes 里提醒了材质图的不同看法"), Row->Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("disagrees")) && Note.Contains(TEXT("basecolor")); }));
	}
	return true;
}

// ---------------------------------------------------------------------------
// 用户指认：压过推断、按材质路径记、重开后还在、不碰材质包
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAnalyzeOverridesTest, "DreamTexture.Assets.Analyze.Overrides.Memory", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAnalyzeOverridesTest::RunTest(const FString& Parameters)
{
	FScopedTestContent Content;
	UTexture2D* Mystery = MakeMasksTexture(Content.Root, TEXT("T_Mystery_1f5d"), 81);
	if (!TestNotNull(TEXT("造好测试贴图"), Mystery))
	{
		return false;
	}

	// 两个材质共用这一张贴图，后面要验证指认只属于其中一个。
	UMaterial* First = MakeTestMaterial(Content.Root, TEXT("M_First"));
	UMaterial* Second = MakeTestMaterial(Content.Root, TEXT("M_Second"));
	for (UMaterial* Material : { First, Second })
	{
		FGraph G(Material);
		G.Connect(MP_PixelDepthOffset, G.Sample(Mystery, SAMPLERTYPE_Masks), SampleOut::R);
		G.Finish();
	}

	FScopedOverrideFile File;
	FDreamTextureOverrideStore Store(File.Path);
	FDreamTextureAnalyzeOptions Options;
	Options.Overrides = &Store;

	const FString TexturePath = Mystery->GetPathName();

	// ---- 没指认过：只能按采样器类型猜成「打包图，排布不明」 ----
	{
		const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(First, Options);
		const FDreamTextureMaterialTexture* Row = Analysis.FindTexture(TexturePath);
		if (ExpectRow(*this, TEXT("指认前"), Row, EDreamTextureRole::Packed, EDreamTextureEvidence::SamplerType))
		{
			TestTrue(TEXT("指认前：需要用户指认"), Row->bNeedsUserInput);
		}
	}

	// 指认前后材质的包都不能被弄脏。
	First->GetOutermost()->ClearDirtyFlag();
	Mystery->GetOutermost()->ClearDirtyFlag();

	FDreamTextureRoleOverride Override;
	Override.Role = EDreamTextureRole::Packed;
	Override.Layout.Channels[0] = EDreamTextureRole::Metallic;
	Override.Layout.Channels[1] = EDreamTextureRole::Roughness;
	Override.Layout.Channels[2] = EDreamTextureRole::AO;
	const FString AssetPath = FDreamTextureMaterialAnalyzer::GetAssetPathForOverrides(First);
	TestEqual(TEXT("记忆的键是材质资产路径"), AssetPath, First->GetPathName());
	TestTrue(TEXT("记住并写盘"), Store.Set(AssetPath, TexturePath, Override));
	TestTrue(TEXT("文件落在了指定位置"), IFileManager::Get().FileExists(*File.Path));

	// ---- 指认后：用的是用户的 ----
	{
		const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(First, Options);
		const FDreamTextureMaterialTexture* Row = Analysis.FindTexture(TexturePath);
		if (ExpectRow(*this, TEXT("指认后"), Row, EDreamTextureRole::Packed, EDreamTextureEvidence::UserOverride))
		{
			TestEqual(TEXT("用户给的排布"), Row->Layout.ToString(), FString(TEXT("R=metallic,G=roughness,B=ao,A=unknown")));
			TestFalse(TEXT("指认完整了，不再需要用户指认"), Row->bNeedsUserInput);
			TestTrue(TEXT("Notes 里留着当初的自动推断"), Row->Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("Automatically inferred")); }));
		}
	}

	// ---- 不应用指认时看到的是纯推断 ----
	{
		FDreamTextureAnalyzeOptions Raw = Options;
		Raw.bApplyOverrides = false;
		const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(First, Raw);
		ExpectRow(*this, TEXT("不应用指认"), Analysis.FindTexture(TexturePath), EDreamTextureRole::Packed, EDreamTextureEvidence::SamplerType);
	}

	// ---- 另一个材质里的同一张贴图不受影响 ----
	{
		const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(Second, Options);
		ExpectRow(*this, TEXT("另一个材质"), Analysis.FindTexture(TexturePath), EDreamTextureRole::Packed, EDreamTextureEvidence::SamplerType);
	}

	// ---- 重开编辑器：内存清掉，从盘上读回来 ----
	{
		Store.Reload();
		FDreamTextureRoleOverride Found;
		if (TestTrue(TEXT("重载后还在"), Store.Find(AssetPath, TexturePath, Found)))
		{
			TestEqual(TEXT("重载后的排布"), Found.Layout.ToString(), Override.Layout.ToString());
		}

		FDreamTextureOverrideStore Other(File.Path);
		TestTrue(TEXT("另一个库实例读同一个文件也能找到"), Other.Find(AssetPath, TexturePath, Found));
	}

	// ---- 没有碰用户的资产 ----
	TestFalse(TEXT("材质包没被弄脏"), First->GetOutermost()->IsDirty());
	TestFalse(TEXT("贴图包没被弄脏"), Mystery->GetOutermost()->IsDirty());

	// ---- 撤销 ----
	TestTrue(TEXT("撤销一条"), Store.Remove(AssetPath, TexturePath));
	TestFalse(TEXT("再撤销就没有了"), Store.Remove(AssetPath, TexturePath));
	{
		const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(First, Options);
		ExpectRow(*this, TEXT("撤销后"), Analysis.FindTexture(TexturePath), EDreamTextureRole::Packed, EDreamTextureEvidence::SamplerType);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAnalyzeOverridesFileTest, "DreamTexture.Assets.Analyze.Overrides.File", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAnalyzeOverridesFileTest::RunTest(const FString& Parameters)
{
	FScopedOverrideFile File;

	// ---- 文件坏了：当空的用，不崩，说明原因，之后的保存能覆盖它 ----
	FFileHelper::SaveStringToFile(TEXT("{ this is not json"), *File.Path);
	{
		FDreamTextureOverrideStore Store(File.Path);
		FDreamTextureRoleOverride Found;
		TestFalse(TEXT("坏文件：找不到任何指认"), Store.Find(TEXT("/Game/A.A"), TEXT("/Game/T.T"), Found));
		TestFalse(TEXT("坏文件：说明了原因"), Store.GetLastError().IsEmpty());

		FDreamTextureRoleOverride Override;
		Override.Role = EDreamTextureRole::Normal;
		TestTrue(TEXT("坏文件：之后仍能保存"), Store.Set(TEXT("/Game/A.A"), TEXT("/Game/T.T"), Override));
	}
	{
		FDreamTextureOverrideStore Store(File.Path);
		FDreamTextureRoleOverride Found;
		if (TestTrue(TEXT("保存覆盖了坏文件"), Store.Find(TEXT("/Game/A.A"), TEXT("/Game/T.T"), Found)))
		{
			TestEqual(TEXT("角色"), AnalyzeId(Found.Role), AnalyzeId(EDreamTextureRole::Normal));
		}
	}

	// ---- 读得懂的留下，读不懂的条目丢掉，不让整个文件作废 ----
	FFileHelper::SaveStringToFile(TEXT("{\"version\":1,\"materials\":{\"/Game/A.A\":{\"/Game/Good.Good\":{\"role\":\"packed\",\"layout\":[\"ao\",\"roughness\",\"bogus\"]},\"/Game/Bad.Bad\":{\"role\":\"not_a_role\"},\"/Game/Weird.Weird\":42},\"/Game/B.B\":\"x\"}}"), *File.Path);
	{
		FDreamTextureOverrideStore Store(File.Path);
		FDreamTextureRoleOverride Found;
		if (TestTrue(TEXT("宽容读：好条目在"), Store.Find(TEXT("/Game/A.A"), TEXT("/Game/Good.Good"), Found)))
		{
			TestEqual(TEXT("宽容读：未知通道名当 unknown"), Found.Layout.ToString(), FString(TEXT("R=ao,G=roughness,B=unknown,A=unknown")));
		}
		TestFalse(TEXT("宽容读：未知角色名的条目丢掉"), Store.Find(TEXT("/Game/A.A"), TEXT("/Game/Bad.Bad"), Found));
		TestFalse(TEXT("宽容读：类型不对的条目丢掉"), Store.Find(TEXT("/Game/A.A"), TEXT("/Game/Weird.Weird"), Found));
		TestEqual(TEXT("宽容读：A 材质下只剩一条"), Store.NumFor(TEXT("/Game/A.A")), 1);
	}

	// ---- 角色名的往返 ----
	for (const EDreamTextureRole Role : { EDreamTextureRole::Unknown, EDreamTextureRole::BaseColor, EDreamTextureRole::Normal, EDreamTextureRole::Roughness, EDreamTextureRole::Metallic,
		EDreamTextureRole::AO, EDreamTextureRole::Height, EDreamTextureRole::Packed, EDreamTextureRole::Emissive, EDreamTextureRole::Opacity, EDreamTextureRole::Specular, EDreamTextureRole::Other })
	{
		EDreamTextureRole Parsed = EDreamTextureRole::Other;
		TestTrue(FString::Printf(TEXT("%s 能解析回来"), LexToString(Role)), LexTryParseString(Parsed, LexToString(Role)));
		TestEqual(FString::Printf(TEXT("%s 往返一致"), LexToString(Role)), AnalyzeId(Parsed), AnalyzeId(Role));
	}
	EDreamTextureRole Untouched = EDreamTextureRole::Normal;
	TestFalse(TEXT("不认识的角色名解析失败"), LexTryParseString(Untouched, TEXT("nope")));
	TestEqual(TEXT("解析失败时不动输出"), AnalyzeId(Untouched), AnalyzeId(EDreamTextureRole::Normal));
	return true;
}

// ---------------------------------------------------------------------------
// Actor 的材质槽
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAnalyzeSlotsTest, "DreamTexture.Assets.Analyze.Slots", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAnalyzeSlotsTest::RunTest(const FString& Parameters)
{
	UWorld* World = GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (World == nullptr)
	{
		AddInfo(TEXT("已跳过：这个进程里没有编辑器世界，没法生成测试 Actor。"));
		return true;
	}
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (Cube == nullptr)
	{
		AddInfo(TEXT("已跳过：找不到引擎自带的 Cube 网格。"));
		return true;
	}

	FScopedTestContent Content;
	UTexture2D* Albedo = MakeColorTexture(Content.Root, TEXT("TexSlotAlbedo"), 91);
	UMaterial* Material = MakeTestMaterial(Content.Root, TEXT("M_Slot"));
	{
		FGraph G(Material);
		G.Connect(MP_BaseColor, G.Param(TEXT("Surface"), Albedo, SAMPLERTYPE_Color), SampleOut::RGB);
		G.Finish();
	}
	UMaterialInstanceConstant* Instance = MakeTestInstance(Content.Root, TEXT("MI_Slot"), Material);
	if (!TestNotNull(TEXT("造好实例"), Instance))
	{
		return false;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags |= RF_Transient;
	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);
	if (!TestNotNull(TEXT("生成了测试 Actor"), Actor))
	{
		return false;
	}
	Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
	Actor->GetStaticMeshComponent()->SetMaterial(0, Instance);

	const TArray<FDreamTextureMaterialSlot> Slots = FDreamTextureMaterialAnalyzer::GetActorSlots(Actor);
	if (TestEqual(TEXT("Cube 只有一个槽"), Slots.Num(), 1))
	{
		TestEqual(TEXT("槽号"), Slots[0].SlotIndex, 0);
		TestTrue(TEXT("槽里的材质就是刚设的实例"), Slots[0].Material.Get() == Instance);
		TestTrue(TEXT("标签里有材质名"), Slots[0].Label.Contains(TEXT("MI_Slot")));

		const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(Slots[0].Material.Get());
		TestTrue(TEXT("取出槽里的材质再走同一条解析"), Analysis.bOK && Analysis.Textures.Num() == 1);
	}

	// 清空槽：列表仍然包含它，只是材质为空，界面才能说「这个槽是空的」。
	Actor->GetStaticMeshComponent()->SetMaterial(0, nullptr);
	const TArray<FDreamTextureMaterialSlot> After = FDreamTextureMaterialAnalyzer::GetActorSlots(Actor);
	TestEqual(TEXT("清空后仍列出这个槽"), After.Num(), 1);

	TestEqual(TEXT("没有 Actor 时是空列表"), FDreamTextureMaterialAnalyzer::GetActorSlots(nullptr).Num(), 0);

	Actor->Destroy();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
