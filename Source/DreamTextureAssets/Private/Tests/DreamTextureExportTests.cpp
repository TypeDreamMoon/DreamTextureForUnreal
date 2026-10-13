// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureAnalyzeTestUtils.h"
#include "DreamTextureApi.h"
#include "DreamTextureImporter.h"
#include "DreamTextureTextureExporter.h"
#include "Engine/TextureCube.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ImageUtils.h"

using namespace DreamTextureTestUtils;

/*
 * 贴图导出（M7）。核心要求：导出的 PNG 与源数据**逐字节一致**，位深/灰度不变，sRGB 不被转换；
 * 取不到源数据的一律明说导不出，绝不退化成平台数据里的 mip0。
 */

namespace
{
	/** 导出目录，用完整个删掉。 */
	struct FScopedExportDir
	{
		FString Dir;

		FScopedExportDir()
			: Dir(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamTextureTest") / TEXT("Export") / FGuid::NewGuid().ToString(EGuidFormats::Digits)))
		{
		}

		~FScopedExportDir()
		{
			IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);
		}
	};

	bool LoadPng(const FString& Path, FImage& OutImage)
	{
		TArray64<uint8> Bytes;
		return FFileHelper::LoadFileToArray(Bytes, *Path) && FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), OutImage);
	}

	/** 解码结果与期望的字节是否逐字节一致；不一致时给出第一个不同的下标。 */
	bool BytesEqual(const FImage& Image, const TArray<uint8>& Expected, int64& OutFirstDiff)
	{
		OutFirstDiff = INDEX_NONE;
		if (Image.RawData.Num() != Expected.Num())
		{
			OutFirstDiff = FMath::Min<int64>(Image.RawData.Num(), Expected.Num());
			return false;
		}
		for (int64 Index = 0; Index < Expected.Num(); ++Index)
		{
			if (Image.RawData[Index] != Expected[Index])
			{
				OutFirstDiff = Index;
				return false;
			}
		}
		return true;
	}

	FString HashFile(const FString& Path)
	{
		TArray<uint8> Bytes;
		FFileHelper::LoadFileToArray(Bytes, *Path);
		return FDreamTextureFetcher::HashBytes16(Bytes);
	}
}

// ---------------------------------------------------------------------------
// 逐像素一致：8 位彩色、8/16 位灰度、16 位彩色
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureExportPixelsTest, "DreamTexture.Assets.Export.Pixels", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureExportPixelsTest::RunTest(const FString& Parameters)
{
	FScopedTestContent Content;
	FScopedExportDir Out;

	struct FCase
	{
		const TCHAR* Name;
		ERawImageFormat::Type Format;
		int32 BytesPerPixel;
		int32 Bits;
		int32 Channels;
		const TCHAR* SourceFormat;
		bool bSRGB;
		TextureCompressionSettings Compression;
		int32 Width;
		int32 Height;
	};
	const FCase Cases[] = {
		// 彩色 8 位，sRGB 开（底色）与关（线性彩色），尺寸故意不是 2 的幂也不是正方形。
		{ TEXT("T_Color8Srgb"), ERawImageFormat::BGRA8, 4, 8, 4, TEXT("BGRA8"), true, TC_Default, 13, 7 },
		{ TEXT("T_Color8Linear"), ERawImageFormat::BGRA8, 4, 8, 4, TEXT("BGRA8"), false, TC_Normalmap, 8, 8 },
		// 灰度 8 位 / 16 位。
		{ TEXT("T_Gray8"), ERawImageFormat::G8, 1, 8, 1, TEXT("G8"), false, TC_Grayscale, 16, 9 },
		{ TEXT("T_Gray16"), ERawImageFormat::G16, 2, 16, 1, TEXT("G16"), false, TC_Grayscale, 11, 11 },
		// 16 位彩色。
		{ TEXT("T_Color16"), ERawImageFormat::RGBA16, 8, 16, 4, TEXT("RGBA16"), false, TC_Default, 6, 10 },
	};

	uint32 Seed = 1000;
	for (const FCase& Case : Cases)
	{
		const FString Label = Case.Name;
		const int32 Count = Case.Width * Case.Height * Case.BytesPerPixel;
		const TArray<uint8> Bytes = MakeBytes(Count, Seed++);
		UTexture2D* Texture = MakeTestTexture(Content.Root, Case.Name, Case.Width, Case.Height, Case.Format, Bytes, Case.bSRGB, Case.Compression);
		if (!TestNotNull(FString::Printf(TEXT("%s：造好贴图"), *Label), Texture))
		{
			continue;
		}

		FString Reason;
		TestTrue(FString::Printf(TEXT("%s：能导出（%s）"), *Label, *Reason), FDreamTextureTextureExporter::CanExport(Texture, Reason));

		const FString Path = Out.Dir / (Label + TEXT(".png"));
		const FDreamTextureExportedImage Result = FDreamTextureTextureExporter::Export(Texture, Path);
		if (!TestTrue(FString::Printf(TEXT("%s：导出成功（%s）"), *Label, *Result.Reason), Result.bExported))
		{
			continue;
		}

		TestTrue(FString::Printf(TEXT("%s：文件在"), *Label), IFileManager::Get().FileExists(*Path));
		TestEqual(FString::Printf(TEXT("%s：宽"), *Label), Result.Width, Case.Width);
		TestEqual(FString::Printf(TEXT("%s：高"), *Label), Result.Height, Case.Height);
		TestFalse(FString::Printf(TEXT("%s：没缩放"), *Label), Result.bResized);
		TestEqual(FString::Printf(TEXT("%s：源格式"), *Label), Result.SourceFormat, FString(Case.SourceFormat));
		TestEqual(FString::Printf(TEXT("%s：位深"), *Label), Result.BitsPerChannel, Case.Bits);
		TestEqual(FString::Printf(TEXT("%s：通道数"), *Label), Result.NumChannels, Case.Channels);
		TestEqual(FString::Printf(TEXT("%s：记下贴图的 sRGB 开关"), *Label), Result.bSRGB, Case.bSRGB);
		TestEqual(FString::Printf(TEXT("%s：贴图路径"), *Label), Result.TexturePath, Texture->GetPathName());

		// 哈希：对写到磁盘上的整个文件算，与后端同一算法。
		TestEqual(FString::Printf(TEXT("%s：sha256 是文件字节的 HashBytes16"), *Label), Result.Sha256, HashFile(Path));
		TestEqual(FString::Printf(TEXT("%s：sha256 是 16 位十六进制"), *Label), Result.Sha256.Len(), 16);

		// 逐字节：解码 PNG，格式、尺寸、每一个字节都要与源数据一致。
		FImage Decoded;
		if (TestTrue(FString::Printf(TEXT("%s：PNG 能解码"), *Label), LoadPng(Path, Decoded)))
		{
			TestEqual(FString::Printf(TEXT("%s：解码后像素格式不变"), *Label), static_cast<int32>(Decoded.Format), static_cast<int32>(Case.Format));
			TestEqual(FString::Printf(TEXT("%s：解码后宽"), *Label), Decoded.SizeX, Case.Width);
			TestEqual(FString::Printf(TEXT("%s：解码后高"), *Label), Decoded.SizeY, Case.Height);
			int64 FirstDiff = INDEX_NONE;
			TestTrue(FString::Printf(TEXT("%s：逐字节一致（第一个不同的下标 %lld）"), *Label, FirstDiff), BytesEqual(Decoded, Bytes, FirstDiff));
			if (FirstDiff != INDEX_NONE)
			{
				AddError(FString::Printf(TEXT("%s：第一个不同的字节下标是 %lld"), *Label, FirstDiff));
			}
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// sRGB 不被转换：写到 PNG 里的就是源数据的字节
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureExportSrgbTest, "DreamTexture.Assets.Export.Srgb", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureExportSrgbTest::RunTest(const FString& Parameters)
{
	FScopedTestContent Content;
	FScopedExportDir Out;

	// 像素 (R,G,B,A) = (128, 64, 200, 255) 与 (10, 128, 250, 255) ……：
	// 如果导出时做了 sRGB↔线性转换，128 会变成 ~55 或 ~188，一眼就能看出来。
	TArray<uint8> Bytes;
	const uint8 Samples[][4] = { { 128, 64, 200, 255 }, { 10, 128, 250, 255 }, { 0, 255, 128, 255 }, { 192, 32, 128, 255 } };
	for (const auto& Pixel : Samples)
	{
		// BGRA 顺序。
		Bytes.Add(Pixel[2]);
		Bytes.Add(Pixel[1]);
		Bytes.Add(Pixel[0]);
		Bytes.Add(Pixel[3]);
	}

	for (const bool bSRGB : { true, false })
	{
		const FString Name = bSRGB ? TEXT("T_Srgb") : TEXT("T_Linear");
		UTexture2D* Texture = MakeTestTexture(Content.Root, *Name, 2, 2, ERawImageFormat::BGRA8, Bytes, bSRGB, bSRGB ? TC_Default : TC_Masks);
		if (!TestNotNull(FString::Printf(TEXT("%s：造好贴图"), *Name), Texture))
		{
			continue;
		}
		const FString Path = Out.Dir / (Name + TEXT(".png"));
		const FDreamTextureExportedImage Result = FDreamTextureTextureExporter::Export(Texture, Path);
		if (!TestTrue(FString::Printf(TEXT("%s：导出成功（%s）"), *Name, *Result.Reason), Result.bExported))
		{
			continue;
		}
		FImage Decoded;
		if (TestTrue(FString::Printf(TEXT("%s：PNG 能解码"), *Name), LoadPng(Path, Decoded)) && TestEqual(FString::Printf(TEXT("%s：像素数据长度"), *Name), Decoded.RawData.Num(), Bytes.Num()))
		{
			// 取 (R=128) 那个像素：BGRA 字节下标 2。
			TestEqual(FString::Printf(TEXT("%s：R=128 仍是 128"), *Name), static_cast<int32>(Decoded.RawData[2]), 128);
			int64 FirstDiff = INDEX_NONE;
			TestTrue(FString::Printf(TEXT("%s：整张图逐字节一致"), *Name), BytesEqual(Decoded, Bytes, FirstDiff));
		}
		TestEqual(FString::Printf(TEXT("%s：记下 sRGB 开关"), *Name), Result.bSRGB, bSRGB);
	}
	return true;
}

// ---------------------------------------------------------------------------
// 缩放：只缩小、保格式
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureExportResizeTest, "DreamTexture.Assets.Export.Resize", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureExportResizeTest::RunTest(const FString& Parameters)
{
	FScopedTestContent Content;
	FScopedExportDir Out;

	// 纯色图：缩放前后每个像素都该是同一个值（容许 1 的舍入误差），格式不变。
	{
		TArray<uint8> Gray;
		Gray.Init(200, 64 * 32);
		UTexture2D* Texture = MakeTestTexture(Content.Root, TEXT("T_ResizeGray"), 64, 32, ERawImageFormat::G8, Gray, false, TC_Grayscale);
		if (TestNotNull(TEXT("造好灰度贴图"), Texture))
		{
			const FString Path = Out.Dir / TEXT("gray_small.png");
			const FDreamTextureExportedImage Small = FDreamTextureTextureExporter::Export(Texture, Path, 16);
			if (TestTrue(FString::Printf(TEXT("缩小导出成功（%s）"), *Small.Reason), Small.bExported))
			{
				TestTrue(TEXT("标记了缩放"), Small.bResized);
				TestEqual(TEXT("长边缩到 16"), Small.Width, 16);
				TestEqual(TEXT("短边按比例"), Small.Height, 8);
				TestEqual(TEXT("源尺寸照实记"), Small.SourceWidth, 64);
				TestEqual(TEXT("源高照实记"), Small.SourceHeight, 32);
				FImage Decoded;
				if (TestTrue(TEXT("缩小后的 PNG 能解码"), LoadPng(Path, Decoded)))
				{
					TestEqual(TEXT("缩小后仍是 G8"), static_cast<int32>(Decoded.Format), static_cast<int32>(ERawImageFormat::G8));
					TestEqual(TEXT("缩小后的宽"), Decoded.SizeX, 16);
					TestEqual(TEXT("缩小后的高"), Decoded.SizeY, 8);
					int32 MaxDeviation = 0;
					for (const uint8 Value : Decoded.RawData)
					{
						MaxDeviation = FMath::Max(MaxDeviation, FMath::Abs(static_cast<int32>(Value) - 200));
					}
					TestTrue(FString::Printf(TEXT("纯色图缩小后值不变（最大偏差 %d）"), MaxDeviation), MaxDeviation <= 1);
				}
			}

			// 目标比源图还大：不放大。
			const FDreamTextureExportedImage Big = FDreamTextureTextureExporter::Export(Texture, Out.Dir / TEXT("gray_big.png"), 4096);
			TestTrue(TEXT("目标更大时导出成功"), Big.bExported);
			TestFalse(TEXT("目标更大时不放大"), Big.bResized);
			TestEqual(TEXT("目标更大时保持原宽"), Big.Width, 64);

			// 0 = 不缩。
			const FDreamTextureExportedImage Same = FDreamTextureTextureExporter::Export(Texture, Out.Dir / TEXT("gray_same.png"), 0);
			TestTrue(TEXT("MaxEdge=0 不缩"), Same.bExported && !Same.bResized && Same.Width == 64 && Same.Height == 32);
		}
	}

	// 16 位灰度缩小后仍是 16 位。
	{
		TArray<uint8> Gray16;
		Gray16.SetNumUninitialized(32 * 32 * 2);
		for (int32 Index = 0; Index < 32 * 32; ++Index)
		{
			// 值 0x4321：高低字节不同，缩小后若被压成 8 位或字节序乱了都看得出来。
			Gray16[Index * 2] = 0x21;
			Gray16[Index * 2 + 1] = 0x43;
		}
		UTexture2D* Texture = MakeTestTexture(Content.Root, TEXT("T_ResizeGray16"), 32, 32, ERawImageFormat::G16, Gray16, false, TC_Grayscale);
		if (TestNotNull(TEXT("造好 16 位灰度贴图"), Texture))
		{
			const FString Path = Out.Dir / TEXT("gray16_small.png");
			const FDreamTextureExportedImage Result = FDreamTextureTextureExporter::Export(Texture, Path, 8);
			if (TestTrue(FString::Printf(TEXT("16 位缩小导出成功（%s）"), *Result.Reason), Result.bExported))
			{
				TestEqual(TEXT("记下 16 位"), Result.BitsPerChannel, 16);
				FImage Decoded;
				if (TestTrue(TEXT("16 位缩小后的 PNG 能解码"), LoadPng(Path, Decoded)))
				{
					TestEqual(TEXT("缩小后仍是 G16"), static_cast<int32>(Decoded.Format), static_cast<int32>(ERawImageFormat::G16));
					TestEqual(TEXT("缩小后宽 8"), Decoded.SizeX, 8);
					const uint16 First = reinterpret_cast<const uint16*>(Decoded.RawData.GetData())[0];
					TestTrue(FString::Printf(TEXT("16 位值保持（0x%04X，期望 0x4321 ± 1）"), First), FMath::Abs(static_cast<int32>(First) - 0x4321) <= 1);
				}
			}
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// 导不出：明说原因，不写文件，绝不退化成 mip0
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureExportRefuseTest, "DreamTexture.Assets.Export.Refuse", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureExportRefuseTest::RunTest(const FString& Parameters)
{
	FScopedTestContent Content;
	FScopedExportDir Out;

	auto ExpectRefused = [this, &Out](const FString& What, UTexture* Texture, const TCHAR* ReasonContains)
	{
		FString Reason;
		const bool bCan = FDreamTextureTextureExporter::CanExport(Texture, Reason);
		TestFalse(FString::Printf(TEXT("%s：CanExport 为假"), *What), bCan);
		TestTrue(FString::Printf(TEXT("%s：原因里有“%s”（实际：%s）"), *What, ReasonContains, *Reason), Reason.Contains(ReasonContains));

		const FString Path = Out.Dir / (FPaths::MakeValidFileName(What) + TEXT(".png"));
		const FDreamTextureExportedImage Result = FDreamTextureTextureExporter::Export(Texture, Path);
		TestFalse(FString::Printf(TEXT("%s：Export 没导出"), *What), Result.bExported);
		TestEqual(FString::Printf(TEXT("%s：Export 的原因与 CanExport 一致"), *What), Result.Reason, Reason);
		TestTrue(FString::Printf(TEXT("%s：没有哈希"), *What), Result.Sha256.IsEmpty());
		TestTrue(FString::Printf(TEXT("%s：没有文件路径"), *What), Result.FilePath.IsEmpty());
		TestFalse(FString::Printf(TEXT("%s：磁盘上没有写出文件"), *What), IFileManager::Get().FileExists(*Path));
	};

	// ---- 空指针 ----
	ExpectRefused(TEXT("空指针"), nullptr, TEXT("does not exist"));

	// ---- 没有源数据，但有平台数据（mip0 其实在内存里）：这正是最容易被偷偷「退化」的情形 ----
	{
		UTexture2D* Transient = UTexture2D::CreateTransient(8, 8, PF_B8G8R8A8);
		if (TestNotNull(TEXT("造好程序生成的贴图"), Transient))
		{
			TestFalse(TEXT("前提：它没有源数据"), Transient->Source.IsValid());
			TestTrue(TEXT("前提：它有平台数据，mip0 就在内存里（所以「能读到」不等于「能导」）"), Transient->GetPlatformData() != nullptr && Transient->GetPlatformData()->Mips.Num() > 0);
			ExpectRefused(TEXT("程序生成"), Transient, TEXT("no source data"));
			FString Reason;
			FDreamTextureTextureExporter::CanExport(Transient, Reason);
			TestTrue(TEXT("原因里明说不会改用 mip0"), Reason.Contains(TEXT("mip 0")));
		}
	}

	// ---- 虚拟纹理且没有源数据 ----
	{
		UPackage* Package = CreatePackage(*(Content.Root / TEXT("T_VtNoSource")));
		UTexture2D* Virtual = NewObject<UTexture2D>(Package, TEXT("T_VtNoSource"), RF_Public | RF_Standalone | RF_Transactional);
		if (TestNotNull(TEXT("造好虚拟纹理（无源数据）"), Virtual))
		{
			// 这张没有源数据，也从没 PostEditChange 过，不会有异步编译在途，直接赋值即可；
			// 反过来对它调 PostEditChange 会因为没有源数据去构建而报错。
			Virtual->VirtualTextureStreaming = true;
			ExpectRefused(TEXT("虚拟纹理无源"), Virtual, TEXT("virtual texture"));
		}
	}

	// ---- 虚拟纹理但有源数据：编辑器里 VT 资产保留着源数据，从源导就是了 ----
	{
		TArray<uint8> Bytes = MakeBytes(4 * 4 * 4, 777);
		UTexture2D* Virtual = MakeTestTexture(Content.Root, TEXT("T_VtWithSource"), 4, 4, ERawImageFormat::BGRA8, Bytes, true, TC_Default);
		if (TestNotNull(TEXT("造好虚拟纹理（有源数据）"), Virtual))
		{
			// 改贴图属性必须夹在 PreEditChange / PostEditChange 之间：真实 RHI 下贴图是异步编译的，PreEditChange 会先等它编完。
			// 直接赋值的话，编译完成的回调里引擎会发现属性变了、又把它登记回编译管理器，报 LogTexture 错误
			// （-nullrhi 下不异步编译，所以只有不带 -nullrhi 的一次运行才暴露）。本文件其余几处同理。
			Virtual->PreEditChange(nullptr);
			Virtual->VirtualTextureStreaming = true;
			Virtual->PostEditChange();
			FString Reason;
			TestTrue(FString::Printf(TEXT("有源数据的 VT 能导出（%s）"), *Reason), FDreamTextureTextureExporter::CanExport(Virtual, Reason));
			const FString Path = Out.Dir / TEXT("vt_with_source.png");
			const FDreamTextureExportedImage Result = FDreamTextureTextureExporter::Export(Virtual, Path);
			TestTrue(FString::Printf(TEXT("有源数据的 VT 导出成功（%s）"), *Result.Reason), Result.bExported);
			FImage Decoded;
			int64 FirstDiff = INDEX_NONE;
			TestTrue(TEXT("有源数据的 VT：逐字节一致"), LoadPng(Path, Decoded) && BytesEqual(Decoded, Bytes, FirstDiff));
		}
	}

	// ---- 浮点 / HDR 源 ----
	{
		const int32 Count = 4 * 4 * 8;
		UTexture2D* Hdr = MakeTestTexture(Content.Root, TEXT("T_Hdr"), 4, 4, ERawImageFormat::RGBA16F, MakeBytes(Count, 888), false, TC_HDR);
		if (TestNotNull(TEXT("造好浮点贴图"), Hdr))
		{
			ExpectRefused(TEXT("浮点源"), Hdr, TEXT("RGBA16F"));
		}
	}

	// ---- 不是 2D 贴图 ----
	{
		UPackage* CubePackage = CreatePackage(*(Content.Root / TEXT("T_Cube")));
		UTextureCube* Cube = NewObject<UTextureCube>(CubePackage, TEXT("T_Cube"), RF_Public | RF_Standalone);
		if (TestNotNull(TEXT("造好立方体贴图"), Cube))
		{
			ExpectRefused(TEXT("立方体贴图"), Cube, TEXT("not a regular 2D"));
		}
		UPackage* RtPackage = CreatePackage(*(Content.Root / TEXT("RT_Target")));
		UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(RtPackage, TEXT("RT_Target"), RF_Public | RF_Standalone);
		if (TestNotNull(TEXT("造好渲染目标"), Target))
		{
			ExpectRefused(TEXT("渲染目标"), Target, TEXT("not a regular 2D"));
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// 固定素材走一遍：导入 → 导出 → 与原 PNG 解码后逐字节一致
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureExportRoundTripTest, "DreamTexture.Assets.Export.RoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureExportRoundTripTest::RunTest(const FString& Parameters)
{
	FDreamTextureManifest Manifest;
	TMap<FString, FString> Files;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("读固定素材：%s"), *Error), LoadFixture(Manifest, Files, Error)))
	{
		return false;
	}

	FScopedTestContent Content;
	FScopedExportDir Out;

	FDreamTextureImportRequest Request;
	Request.Manifest = Manifest;
	Request.LocalFiles = Files;
	Request.ContentRoot = Content.Root;
	Request.bSave = false;
	const FDreamTextureImportResult Imported = FDreamTextureImporter::Import(Request);
	if (!TestTrue(FString::Printf(TEXT("导入成功（%s）"), *Imported.Error), Imported.bOK))
	{
		return false;
	}

	int32 NumChecked = 0;
	for (const FDreamTextureImportedTexture& Entry : Imported.Textures)
	{
		UTexture2D* Texture = FindTexture(Entry.ObjectPath);
		const FString* OriginalPath = Files.Find(Entry.Channel);
		if (Texture == nullptr || OriginalPath == nullptr)
		{
			continue;
		}

		const FString Path = Out.Dir / (Entry.Channel + TEXT(".png"));
		const FDreamTextureExportedImage Result = FDreamTextureTextureExporter::Export(Texture, Path);
		if (!TestTrue(FString::Printf(TEXT("%s：导出成功（%s）"), *Entry.Channel, *Result.Reason), Result.bExported))
		{
			continue;
		}

		FImage Original;
		FImage Exported;
		if (TestTrue(FString::Printf(TEXT("%s：原 PNG 与导出的 PNG 都能解码"), *Entry.Channel), LoadPng(*OriginalPath, Original) && LoadPng(Path, Exported)))
		{
			TestEqual(FString::Printf(TEXT("%s：像素格式一致"), *Entry.Channel), static_cast<int32>(Exported.Format), static_cast<int32>(Original.Format));
			TestEqual(FString::Printf(TEXT("%s：尺寸一致"), *Entry.Channel), Exported.SizeX * 100000 + Exported.SizeY, Original.SizeX * 100000 + Original.SizeY);
			TestTrue(FString::Printf(TEXT("%s：逐字节与原图一致（导入→导出没有损失）"), *Entry.Channel), Exported.RawData.Num() == Original.RawData.Num() && FMemory::Memcmp(Exported.RawData.GetData(), Original.RawData.GetData(), Original.RawData.Num()) == 0);
			++NumChecked;
		}
	}
	TestTrue(TEXT("固定素材的各通道都核对过"), NumChecked >= 4);
	return true;
}

// ---------------------------------------------------------------------------
// 一次导出一整个解析结果：同名不覆盖、与解析行一一对应
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureExportBatchTest, "DreamTexture.Assets.Export.Batch", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureExportBatchTest::RunTest(const FString& Parameters)
{
	FScopedTestContent Content;
	FScopedExportDir Out;

	// 两张不同目录下的同名贴图，外加一张没有源数据的程序生成贴图。
	UTexture2D* First = MakeTestTexture(Content.Root + TEXT("/A"), TEXT("T_Same"), 4, 4, ERawImageFormat::BGRA8, MakeBytes(64, 1), true, TC_Default);
	UTexture2D* Second = MakeTestTexture(Content.Root + TEXT("/B"), TEXT("T_Same"), 4, 4, ERawImageFormat::BGRA8, MakeBytes(64, 2), true, TC_Default);
	UTexture2D* Transient = UTexture2D::CreateTransient(4, 4, PF_B8G8R8A8);
	if (!TestTrue(TEXT("造好贴图"), First && Second && Transient))
	{
		return false;
	}

	UMaterial* Material = MakeTestMaterial(Content.Root, TEXT("M_Batch"));
	FGraph G(Material);
	G.Connect(MP_BaseColor, G.Sample(First, SAMPLERTYPE_Color), SampleOut::RGB);
	G.Connect(MP_EmissiveColor, G.Sample(Second, SAMPLERTYPE_Color), SampleOut::RGB);
	G.Connect(MP_Specular, G.Sample(Transient, SAMPLERTYPE_Color), SampleOut::R);
	G.Finish();

	const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(Material);
	if (!TestTrue(FString::Printf(TEXT("解析成功（%s）"), *Analysis.Error), Analysis.bOK) || !TestEqual(TEXT("三行"), Analysis.Textures.Num(), 3))
	{
		return false;
	}

	// 解析阶段就已经知道哪一行导不出，以及为什么。
	const FDreamTextureMaterialTexture* TransientRow = Analysis.FindTexture(Transient->GetPathName());
	if (TestNotNull(TEXT("程序生成贴图那一行在"), TransientRow))
	{
		TestFalse(TEXT("解析结果里标了导不出"), TransientRow->bExportable);
		TestTrue(TEXT("解析结果里写了原因"), TransientRow->ExportBlocker.Contains(TEXT("no source data")));
	}

	FDreamTextureExportOptions Options;
	Options.OutputDir = Out.Dir;
	const TArray<FDreamTextureExportedImage> Results = FDreamTextureTextureExporter::ExportAnalysis(Analysis, Options);
	if (!TestEqual(TEXT("结果与解析行一一对应"), Results.Num(), Analysis.Textures.Num()))
	{
		return false;
	}

	TSet<FString> Files;
	int32 NumExported = 0;
	for (int32 Index = 0; Index < Results.Num(); ++Index)
	{
		const FDreamTextureMaterialTexture& Row = Analysis.Textures[Index];
		const FDreamTextureExportedImage& Result = Results[Index];
		TestEqual(FString::Printf(TEXT("第 %d 行：贴图路径对得上"), Index), Result.TexturePath, Row.TexturePath);
		TestEqual(FString::Printf(TEXT("第 %d 行：导出结果与解析时的判断一致"), Index), Result.bExported, Row.bExportable);
		if (Result.bExported)
		{
			++NumExported;
			Files.Add(Result.FilePath.ToLower());
			TestTrue(FString::Printf(TEXT("第 %d 行：文件在输出目录里"), Index), Result.FilePath.StartsWith(Out.Dir) || FPaths::IsUnderDirectory(Result.FilePath, Out.Dir));
			TestTrue(FString::Printf(TEXT("第 %d 行：文件存在"), Index), IFileManager::Get().FileExists(*Result.FilePath));
		}
		else
		{
			TestFalse(FString::Printf(TEXT("第 %d 行：导不出时写了原因"), Index), Result.Reason.IsEmpty());
		}
	}
	TestEqual(TEXT("两张能导的都导了"), NumExported, 2);
	TestEqual(TEXT("同名贴图的文件名不撞（不会互相覆盖）"), Files.Num(), 2);

	// ---- 临时目录：每次新的，在 Saved/DreamTexture/Reflow 下 ----
	const FString DirA = FDreamTextureTextureExporter::MakeTempDir();
	const FString DirB = FDreamTextureTextureExporter::MakeTempDir();
	TestNotEqual(TEXT("每次的临时目录不同"), DirA, DirB);
	TestTrue(TEXT("临时目录已建好"), IFileManager::Get().DirectoryExists(*DirA) && IFileManager::Get().DirectoryExists(*DirB));
	TestTrue(TEXT("临时目录在 Saved/DreamTexture/Reflow 下"), DirA.Contains(TEXT("DreamTexture")) && DirA.Contains(TEXT("Reflow")));
	IFileManager::Get().DeleteDirectory(*DirA, false, true);
	IFileManager::Get().DeleteDirectory(*DirB, false, true);

	// ---- 不给输出目录：用新的临时目录 ----
	const TArray<FDreamTextureExportedImage> Auto = FDreamTextureTextureExporter::ExportAnalysis(Analysis);
	FString AutoDir;
	for (const FDreamTextureExportedImage& Result : Auto)
	{
		if (Result.bExported)
		{
			AutoDir = FPaths::GetPath(Result.FilePath);
			break;
		}
	}
	TestFalse(TEXT("不给输出目录时也导出了"), AutoDir.IsEmpty());
	if (!AutoDir.IsEmpty())
	{
		IFileManager::Get().DeleteDirectory(*AutoDir, false, true);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Flip Green Channel：引擎只在构建派生数据时翻，导出时要替它翻，导出的图才与 UE 里实际用的一致
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureExportFlipGreenTest, "DreamTexture.Assets.Export.FlipGreen", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureExportFlipGreenTest::RunTest(const FString& Parameters)
{
	FScopedTestContent Content;
	FScopedExportDir Out;

	// ---- 8 位 BGRA：G 在第 1 个字节 ----
	{
		const int32 Width = 9;
		const int32 Height = 5;
		const TArray<uint8> Source = MakeBytes(Width * Height * 4, 77);
		UTexture2D* Texture = MakeTestTexture(Content.Root, TEXT("T_Flip8"), Width, Height, ERawImageFormat::BGRA8, Source, false, TC_Normalmap);
		if (!TestNotNull(TEXT("BGRA8：造好贴图"), Texture))
		{
			return false;
		}

		// 先不勾：逐字节与源一致，没翻。
		FDreamTextureExportedImage Plain = FDreamTextureTextureExporter::Export(Texture, Out.Dir / TEXT("flip8_plain.png"));
		if (TestTrue(FString::Printf(TEXT("BGRA8 不勾：导出成功（%s）"), *Plain.Reason), Plain.bExported))
		{
			TestFalse(TEXT("BGRA8 不勾：没翻"), Plain.bFlippedGreen);
			FImage Image;
			int64 FirstDiff = INDEX_NONE;
			TestTrue(TEXT("BGRA8 不勾：解码得到图"), LoadPng(Plain.FilePath, Image));
			TestTrue(FString::Printf(TEXT("BGRA8 不勾：与源逐字节一致（首个不同下标 %lld）"), FirstDiff), BytesEqual(Image, Source, FirstDiff));
		}

		// 勾上：G 取反，B / R / A 不动。
		Texture->PreEditChange(nullptr);
		Texture->bFlipGreenChannel = true;
		Texture->PostEditChange();
		TArray<uint8> Expected = Source;
		for (int32 Index = 0; Index < Width * Height; ++Index)
		{
			Expected[Index * 4 + 1] = static_cast<uint8>(255 - Expected[Index * 4 + 1]);
		}
		FDreamTextureExportedImage Flipped = FDreamTextureTextureExporter::Export(Texture, Out.Dir / TEXT("flip8_flipped.png"));
		if (TestTrue(FString::Printf(TEXT("BGRA8 勾上：导出成功（%s）"), *Flipped.Reason), Flipped.bExported))
		{
			TestTrue(TEXT("BGRA8 勾上：标明翻过"), Flipped.bFlippedGreen);
			FImage Image;
			int64 FirstDiff = INDEX_NONE;
			TestTrue(TEXT("BGRA8 勾上：解码得到图"), LoadPng(Flipped.FilePath, Image));
			TestTrue(FString::Printf(TEXT("BGRA8 勾上：G 取反、其余通道不变（首个不同下标 %lld）"), FirstDiff), BytesEqual(Image, Expected, FirstDiff));
			TestTrue(TEXT("翻过与没翻的两个文件哈希不同"), Flipped.Sha256 != Plain.Sha256);
		}

		// 翻的是读出来的副本：导出之后 Source 里的像素没变，再导一次得到同样的结果（不会越翻越回去）。
		{
			FImage Again;
			TestTrue(TEXT("导出后读 Source"), Texture->Source.GetMipImage(Again, 0, 0, 0));
			int64 FirstDiff = INDEX_NONE;
			TestTrue(FString::Printf(TEXT("导出之后 Source 原样不动（首个不同下标 %lld）"), FirstDiff), BytesEqual(Again, Source, FirstDiff));

			FDreamTextureExportedImage Second = FDreamTextureTextureExporter::Export(Texture, Out.Dir / TEXT("flip8_flipped_again.png"));
			TestTrue(TEXT("再导一次，字节与第一次完全一样"), Second.bExported && Second.Sha256 == Flipped.Sha256);
		}

		// 带缩放：先翻再缩，仍然标明翻过。
		{
			FDreamTextureExportedImage Small = FDreamTextureTextureExporter::Export(Texture, Out.Dir / TEXT("flip8_small.png"), /*MaxEdge*/ 4);
			TestTrue(FString::Printf(TEXT("BGRA8 勾上 + 缩放：导出成功（%s）"), *Small.Reason), Small.bExported);
			TestTrue(TEXT("BGRA8 勾上 + 缩放：确实缩了"), Small.bResized);
			TestTrue(TEXT("BGRA8 勾上 + 缩放：标明翻过"), Small.bFlippedGreen);
		}
	}

	// ---- 16 位 RGBA：G 是第 1 个 16 位通道 ----
	{
		const int32 Width = 6;
		const int32 Height = 4;
		const TArray<uint8> Source = MakeBytes(Width * Height * 8, 91);
		UTexture2D* Texture = MakeTestTexture(Content.Root, TEXT("T_Flip16"), Width, Height, ERawImageFormat::RGBA16, Source, false, TC_Normalmap);
		if (TestNotNull(TEXT("RGBA16：造好贴图"), Texture))
		{
			Texture->PreEditChange(nullptr);
			Texture->bFlipGreenChannel = true;
			Texture->PostEditChange();
			TArray<uint8> Expected = Source;
			uint16* Words = reinterpret_cast<uint16*>(Expected.GetData());
			for (int32 Index = 0; Index < Width * Height; ++Index)
			{
				Words[Index * 4 + 1] = static_cast<uint16>(65535 - Words[Index * 4 + 1]);
			}
			FDreamTextureExportedImage Result = FDreamTextureTextureExporter::Export(Texture, Out.Dir / TEXT("flip16.png"));
			if (TestTrue(FString::Printf(TEXT("RGBA16：导出成功（%s）"), *Result.Reason), Result.bExported))
			{
				TestTrue(TEXT("RGBA16：标明翻过"), Result.bFlippedGreen);
				TestEqual(TEXT("RGBA16：位深仍是 16"), Result.BitsPerChannel, 16);
				FImage Image;
				int64 FirstDiff = INDEX_NONE;
				TestTrue(TEXT("RGBA16：解码得到图"), LoadPng(Result.FilePath, Image));
				TestTrue(FString::Printf(TEXT("RGBA16：G 取反、其余通道不变（首个不同下标 %lld）"), FirstDiff), BytesEqual(Image, Expected, FirstDiff));
			}
		}
	}

	// ---- 灰度：没有绿通道，勾了也原样导出 ----
	{
		const TArray<uint8> Source = MakeBytes(8 * 8, 5);
		UTexture2D* Texture = MakeTestTexture(Content.Root, TEXT("T_FlipGray"), 8, 8, ERawImageFormat::G8, Source, false, TC_Grayscale);
		if (TestNotNull(TEXT("灰度：造好贴图"), Texture))
		{
			Texture->PreEditChange(nullptr);
			Texture->bFlipGreenChannel = true;
			Texture->PostEditChange();
			FDreamTextureExportedImage Result = FDreamTextureTextureExporter::Export(Texture, Out.Dir / TEXT("flipgray.png"));
			if (TestTrue(FString::Printf(TEXT("灰度：导出成功（%s）"), *Result.Reason), Result.bExported))
			{
				TestFalse(TEXT("灰度：没有绿通道，不翻"), Result.bFlippedGreen);
				FImage Image;
				int64 FirstDiff = INDEX_NONE;
				TestTrue(TEXT("灰度：解码得到图"), LoadPng(Result.FilePath, Image));
				TestTrue(FString::Printf(TEXT("灰度：与源逐字节一致（首个不同下标 %lld）"), FirstDiff), BytesEqual(Image, Source, FirstDiff));
			}
		}
	}

	// ---- 与导入器配合：OpenGL 法线导入后勾着 Flip Green，导出的就是翻过的（DirectX）----
	{
		FDreamTextureManifest Manifest;
		TMap<FString, FString> Files;
		FString LoadError;
		if (TestTrue(FString::Printf(TEXT("读固定素材：%s"), *LoadError), LoadFixture(Manifest, Files, LoadError)))
		{
			if (FDreamTextureMapInfo* Normal = Manifest.Maps.Find(TEXT("normal")))
			{
				Normal->NormalY = TEXT("opengl");
			}
			FDreamTextureImportRequest Request;
			Request.Manifest = Manifest;
			Request.LocalFiles = Files;
			Request.ContentRoot = Content.Root;
			Request.bSave = false;
			const FDreamTextureImportResult Imported = FDreamTextureImporter::Import(Request);
			const FDreamTextureImportedTexture* Entry = Imported.Textures.FindByPredicate([](const FDreamTextureImportedTexture& E) { return E.Channel == TEXT("normal"); });
			UTexture2D* Texture = Entry != nullptr ? FindObject<UTexture2D>(nullptr, *Entry->ObjectPath) : nullptr;
			if (TestNotNull(FString::Printf(TEXT("OpenGL 法线导入成功（%s）"), *Imported.Error), Texture))
			{
				TestTrue(TEXT("导入器给 OpenGL 法线勾了 Flip Green"), Texture->bFlipGreenChannel != 0);
				FDreamTextureExportedImage Result = FDreamTextureTextureExporter::Export(Texture, Out.Dir / TEXT("normal_opengl.png"));
				if (TestTrue(FString::Printf(TEXT("导出成功（%s）"), *Result.Reason), Result.bExported))
				{
					TestTrue(TEXT("导出时翻了 G，与 UE 里实际用的（DirectX）一致"), Result.bFlippedGreen);

					// 与固定素材里原始的 normal.png 对比：导出的 G 应当是原图 G 取反。
					FImage Original;
					FImage Exported;
					TestTrue(TEXT("读得到原始 normal.png"), LoadPng(Files[TEXT("normal")], Original));
					TestTrue(TEXT("读得到导出的 PNG"), LoadPng(Result.FilePath, Exported));
					// 只统一像素格式，伽马空间保持各自的：换伽马空间会改字节，「取反」就不成立了。
					Original.ChangeFormat(ERawImageFormat::BGRA8, Original.GammaSpace);
					Exported.ChangeFormat(ERawImageFormat::BGRA8, Exported.GammaSpace);
					bool bAllInverted = Original.RawData.Num() == Exported.RawData.Num() && Original.RawData.Num() > 0;
					for (int64 Pixel = 0; bAllInverted && Pixel < Original.RawData.Num() / 4; ++Pixel)
					{
						bAllInverted &= (Exported.RawData[Pixel * 4 + 1] == 255 - Original.RawData[Pixel * 4 + 1]) && (Exported.RawData[Pixel * 4 + 0] == Original.RawData[Pixel * 4 + 0]) &&
							(Exported.RawData[Pixel * 4 + 2] == Original.RawData[Pixel * 4 + 2]);
					}
					TestTrue(TEXT("导出的每个像素：G 是原图 G 取反，B、R 不变"), bAllInverted);
				}
			}
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// Windows 保留设备名：CON / PRN / AUX / NUL / COM1-9 / LPT1-9 不能直接当文件名
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureExportReservedNameTest, "DreamTexture.Assets.Export.ReservedName", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureExportReservedNameTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		const TCHAR* AssetName;
		const TCHAR* ExpectedStem;
	};
	const FCase Cases[] = {
		// 保留名：不分大小写、原来的大小写保留，加前缀。
		{ TEXT("CON"), TEXT("Texture_CON") },
		{ TEXT("prn"), TEXT("Texture_prn") },
		{ TEXT("Aux"), TEXT("Texture_Aux") },
		{ TEXT("NUL"), TEXT("Texture_NUL") },
		{ TEXT("COM1"), TEXT("Texture_COM1") },
		{ TEXT("com9"), TEXT("Texture_com9") },
		{ TEXT("LPT1"), TEXT("Texture_LPT1") },
		{ TEXT("lpt9"), TEXT("Texture_lpt9") },
		// 长得像、但不是保留名的：原样。
		{ TEXT("CONSOLE"), TEXT("CONSOLE") },
		{ TEXT("COM0"), TEXT("COM0") },
		{ TEXT("COM10"), TEXT("COM10") },
		{ TEXT("LPT0"), TEXT("LPT0") },
		{ TEXT("NULL"), TEXT("NULL") },
		{ TEXT("AUXILIARY"), TEXT("AUXILIARY") },
		{ TEXT("T_Rock_BC"), TEXT("T_Rock_BC") },
		{ TEXT("MyCON"), TEXT("MyCON") },
	};

	// ---- 直接测文件名规则 ----
	for (const FCase& Case : Cases)
	{
		TestEqual(FString::Printf(TEXT("资产名 %s → 文件名"), Case.AssetName), FDreamTextureTextureExporter::MakeSafeFileStem(Case.AssetName), FString(Case.ExpectedStem));
	}
	// Windows 对带尾随空格或点的保留名也当设备名。
	TestEqual(TEXT("尾随点的 CON."), FDreamTextureTextureExporter::MakeSafeFileStem(TEXT("CON.")), FString(TEXT("Texture_CON.")));
	TestEqual(TEXT("尾随空格的 NUL "), FDreamTextureTextureExporter::MakeSafeFileStem(TEXT("NUL ")).TrimEnd(), FString(TEXT("Texture_NUL")));
	TestFalse(TEXT("空名字有兜底"), FDreamTextureTextureExporter::MakeSafeFileStem(FString()).IsEmpty());
	TestFalse(TEXT("全是非法字符的名字也有兜底"), FDreamTextureTextureExporter::MakeSafeFileStem(TEXT("***")).IsEmpty());

	// ---- 走完整的批量导出：文件真的落在磁盘上，名字是改过的 ----
	FScopedTestContent Content;
	FScopedExportDir Out;

	FDreamTextureMaterialAnalysis Analysis;
	Analysis.bOK = true;
	uint32 Seed = 500;
	for (const FCase& Case : Cases)
	{
		UTexture2D* Texture = MakeTestTexture(Content.Root, Case.AssetName, 4, 4, ERawImageFormat::BGRA8, MakeBytes(64, Seed++), true, TC_Default);
		if (!TestNotNull(FString::Printf(TEXT("造好贴图 %s"), Case.AssetName), Texture))
		{
			return false;
		}
		FDreamTextureMaterialTexture Row;
		Row.Texture = Texture;
		Row.TexturePath = Texture->GetPathName();
		Row.AssetName = Case.AssetName;
		Analysis.Textures.Add(Row);
	}

	FDreamTextureExportOptions Options;
	Options.OutputDir = Out.Dir;
	const TArray<FDreamTextureExportedImage> Results = FDreamTextureTextureExporter::ExportAnalysis(Analysis, Options);
	if (!TestEqual(TEXT("结果与行一一对应"), Results.Num(), static_cast<int32>(UE_ARRAY_COUNT(Cases))))
	{
		return false;
	}
	TSet<FString> Names;
	for (int32 Index = 0; Index < Results.Num(); ++Index)
	{
		const FDreamTextureExportedImage& Result = Results[Index];
		if (!TestTrue(FString::Printf(TEXT("%s：导出成功（%s）"), Cases[Index].AssetName, *Result.Reason), Result.bExported))
		{
			continue;
		}
		const FString Clean = FPaths::GetCleanFilename(Result.FilePath);
		Names.Add(Clean.ToLower());
		TestEqual(FString::Printf(TEXT("%s：落盘的文件名"), Cases[Index].AssetName), Clean, FString(Cases[Index].ExpectedStem) + TEXT(".png"));
		TestTrue(FString::Printf(TEXT("%s：文件确实在磁盘上"), Cases[Index].AssetName), IFileManager::Get().FileExists(*Result.FilePath) && IFileManager::Get().FileSize(*Result.FilePath) > 0);
	}
	TestEqual(TEXT("所有文件名互不相同"), Names.Num(), static_cast<int32>(UE_ARRAY_COUNT(Cases)));

	// 加了前缀之后与一个真叫 Texture_CON 的资产撞名，仍按「同名加序号」处理，不互相覆盖。
	{
		FScopedExportDir Out2;
		FDreamTextureMaterialAnalysis Clash;
		Clash.bOK = true;
		for (const TCHAR* Name : { TEXT("CON"), TEXT("Texture_CON") })
		{
			UTexture2D* Texture = MakeTestTexture(Content.Root + TEXT("/Clash"), Name, 4, 4, ERawImageFormat::BGRA8, MakeBytes(64, Seed++), true, TC_Default);
			FDreamTextureMaterialTexture Row;
			Row.Texture = Texture;
			Row.TexturePath = Texture->GetPathName();
			Row.AssetName = Name;
			Clash.Textures.Add(Row);
		}
		FDreamTextureExportOptions ClashOptions;
		ClashOptions.OutputDir = Out2.Dir;
		const TArray<FDreamTextureExportedImage> ClashResults = FDreamTextureTextureExporter::ExportAnalysis(Clash, ClashOptions);
		TSet<FString> ClashNames;
		for (const FDreamTextureExportedImage& Result : ClashResults)
		{
			TestTrue(TEXT("撞名的也导出成功"), Result.bExported);
			ClashNames.Add(FPaths::GetCleanFilename(Result.FilePath).ToLower());
		}
		TestEqual(TEXT("加前缀后与真名撞车时文件名仍不重复"), ClashNames.Num(), 2);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
