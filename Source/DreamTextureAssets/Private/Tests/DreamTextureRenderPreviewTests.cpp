// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/DirectionalLightComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkyLightComponent.h"
#include "DreamTextureMaterialBuilder.h"
#include "DreamTextureMaterialTestUtils.h"
#include "DreamTextureRenderTestUtils.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureCube.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/PlatformFileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "MaterialEditingLibrary.h"
#include "Misc/FileHelper.h"
#include "PreviewScene.h"
#include "TextureResource.h"
#include "HAL/IConsoleManager.h"
#include "UObject/StrongObjectPtr.h"
#include "Components/StaticMeshComponent.h"

using namespace DreamTextureTestUtils;

/*
 * DreamTexture.Render.Preview：用固定素材的材质实例，在一块 3×3 平铺的平面上、方向光加天光下渲一张图，存成 PNG。
 *
 * 这不是回归断言，是给人看的：光照下法线、粗糙度、AO 对不对，平铺有没有接缝。
 * 同时它是唯一真的把材质画在网格上的测试——网格通道（基础通道、深度、Nanite…）的着色器是第一次被画时才按需编译的，
 * 视差遮蔽那条路也要画出来才算数。所以断言只有几条保底的：没有编译错误、画面不是纯黑/纯色、PNG 存出来了。
 */

namespace
{
	constexpr int32 PreviewSize = 768;

	struct FPreviewVariant
	{
		FString Name;
		FString Description;
		TStrongObjectPtr<UMaterialInstanceConstant> Instance;
		/** 相机俯角（度）。-90 = 正上方往下看；更斜的角度用来看视差遮蔽——正上方看过去视差几乎为零。 */
		float Pitch = -90.f;
		TArray<FColor> Pixels;
		FString SavedPath;
	};

	struct FPreviewState
	{
		// 声明顺序即析构的反序：测试内容最后清，场景先拆。
		FScopedTestContent Content;
		TUniquePtr<FPreviewScene> Scene;
		TStrongObjectPtr<UTextureRenderTarget2D> Target;
		TStrongObjectPtr<USceneCaptureComponent2D> Capture;
		TStrongObjectPtr<UStaticMeshComponent> Plane;
		TArray<FPreviewVariant> Variants;

		int32 Current = -1;
		int32 FramesOnVariant = 0;
		int32 StableFrames = 0;
		TArray<FColor> Previous;
		float LastDiff = 0.f;
		double VariantStarted = 0.0;
		bool bFailed = false;

		/** 测试期间临时改的控制台变量，Teardown 时还原。 */
		TArray<TPair<FString, int32>> SavedCVars;

		void SetCVar(const TCHAR* Name, int32 Value)
		{
			if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name))
			{
				SavedCVars.Add({ Name, CVar->GetInt() });
				CVar->Set(Value, ECVF_SetByCode);
			}
		}

		~FPreviewState()
		{
			Teardown();
		}

		void Teardown()
		{
			if (Scene.IsValid())
			{
				if (Capture.IsValid())
				{
					Scene->RemoveComponent(Capture.Get());
				}
				if (Plane.IsValid())
				{
					Scene->RemoveComponent(Plane.Get());
				}
			}
			Capture.Reset();
			Plane.Reset();
			Target.Reset();
			for (FPreviewVariant& Variant : Variants)
			{
				Variant.Instance.Reset();
			}
			Scene.Reset();
			for (const TPair<FString, int32>& Saved : SavedCVars)
			{
				if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Saved.Key))
				{
					CVar->Set(Saved.Value, ECVF_SetByCode);
				}
			}
			SavedCVars.Reset();
			Content.Cleanup();
		}
	};

	uint32 HashPixels(const TArray<FColor>& Pixels)
	{
		return FCrc::MemCrc32(Pixels.GetData(), Pixels.Num() * sizeof(FColor));
	}

	/**
	 * 两帧之间每个通道的平均绝对差（0~255）。
	 * 不能比哈希：抗锯齿的抖动让每一帧的像素都差几个灰阶，哈希永远不一样；这里用「差得足够小」当作画面稳定。
	 */
	float MeanAbsDiff(const TArray<FColor>& A, const TArray<FColor>& B)
	{
		if (A.Num() != B.Num() || A.Num() == 0)
		{
			return 255.f;
		}
		double Sum = 0.0;
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			Sum += FMath::Abs(static_cast<int32>(A[Index].R) - static_cast<int32>(B[Index].R)) + FMath::Abs(static_cast<int32>(A[Index].G) - static_cast<int32>(B[Index].G)) +
				FMath::Abs(static_cast<int32>(A[Index].B) - static_cast<int32>(B[Index].B));
		}
		return static_cast<float>(Sum / (3.0 * A.Num()));
	}

	bool SavePng(const FString& Path, const TArray<FColor>& Pixels, int32 Width, int32 Height)
	{
		TArray<FColor> Opaque = Pixels;
		for (FColor& Pixel : Opaque)
		{
			Pixel.A = 255;
		}
		const FImageView View(Opaque.GetData(), Width, Height, ERawImageFormat::BGRA8);
		TArray64<uint8> Bytes;
		if (!FImageUtils::CompressImage(Bytes, TEXT("png"), View))
		{
			return false;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
		return FFileHelper::SaveArrayToFile(Bytes, *Path);
	}

	/** 画面是不是「有内容」：不是纯色。算一下通道的标准差（0~255）。 */
	float ContrastOf(const TArray<FColor>& Pixels)
	{
		if (Pixels.Num() == 0)
		{
			return 0.f;
		}
		double Sum = 0.0;
		double SumSq = 0.0;
		for (const FColor& Pixel : Pixels)
		{
			const double Luma = 0.2126 * Pixel.R + 0.7152 * Pixel.G + 0.0722 * Pixel.B;
			Sum += Luma;
			SumSq += Luma * Luma;
		}
		const double Mean = Sum / Pixels.Num();
		return static_cast<float>(FMath::Sqrt(FMath::Max(0.0, SumSq / Pixels.Num() - Mean * Mean)));
	}

	float MeanLuma(const TArray<FColor>& Pixels)
	{
		double Sum = 0.0;
		for (const FColor& Pixel : Pixels)
		{
			Sum += 0.2126 * Pixel.R + 0.7152 * Pixel.G + 0.0722 * Pixel.B;
		}
		return Pixels.Num() > 0 ? static_cast<float>(Sum / Pixels.Num()) : 0.f;
	}

	UMaterialInstanceConstant* MakeVariantInstance(const FString& Folder, const FString& Name, UMaterialInterface* Parent)
	{
		UPackage* Package = CreatePackage(*(Folder / Name));
		UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(Package, FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
		UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Parent);
		return Instance;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureRenderPreviewTest, "DreamTexture.Render.Preview", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureRenderPreviewTest::RunTest(const FString& Parameters)
{
	FString WhyNot;
	if (!DreamTextureRenderTestUtils::CanRender(WhyNot))
	{
		AddInfo(FString::Printf(TEXT("已跳过：%s。用 Tools/Test.ps1 -Render 跑（不带 -nullrhi 启动编辑器）。"), *WhyNot));
		return true;
	}

	FDreamTextureManifest Manifest;
	TMap<FString, FString> Files;
	FString LoadError;
	if (!TestTrue(FString::Printf(TEXT("读固定素材：%s"), *LoadError), LoadFixture(Manifest, Files, LoadError)))
	{
		return false;
	}

	const TSharedRef<FPreviewState> State = MakeShared<FPreviewState>();
	const TSharedRef<bool> bAborted = MakeShared<bool>(false);

	// ---- 素材：固定素材导入，建出材质实例，再各派生一份「预览用」实例（不动真正的那份）----
	FDreamTextureImportRequest Request;
	Request.Manifest = Manifest;
	Request.LocalFiles = Files;
	Request.ContentRoot = State->Content.Root;
	Request.bSave = false;
	const FDreamTextureImportResult Imported = FDreamTextureImporter::Import(Request);
	if (!TestTrue(FString::Printf(TEXT("导入成功（%s）"), *Imported.Error), Imported.bOK))
	{
		return false;
	}
	const FDreamTextureImportedTexture Created = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Imported, State->Content.Root, false);
	UMaterialInstanceConstant* Base = FindObject<UMaterialInstanceConstant>(nullptr, *Created.ObjectPath);
	if (!TestNotNull(FString::Printf(TEXT("建出材质实例（%s）"), *Created.Note), Base))
	{
		return false;
	}

	struct FVariantSpec
	{
		const TCHAR* Name;
		const TCHAR* Description;
		bool bUseNormal;
		bool bUseORM;
		bool bUsePOM;
		float Pitch;
	};
	const FVariantSpec Specs[] = {
		{ TEXT("default"), TEXT("导入后的默认状态：法线 + ORM 开，视差关；正上方俯视"), true, true, false, -90.f },
		{ TEXT("pom"), TEXT("同上，打开视差遮蔽；正上方俯视"), true, true, true, -90.f },
		{ TEXT("basecolor_only"), TEXT("对照：只有底色（法线、ORM 都关，粗糙度 0.5、金属度 0、AO 1）；正上方俯视"), false, false, false, -90.f },
		{ TEXT("default_oblique"), TEXT("默认状态，45° 斜看"), true, true, false, -45.f },
		{ TEXT("pom_oblique"), TEXT("打开视差遮蔽，45° 斜看（视差在斜看时才明显）"), true, true, true, -45.f },
	};
	for (const FVariantSpec& Spec : Specs)
	{
		FPreviewVariant& Variant = State->Variants.AddDefaulted_GetRef();
		Variant.Name = Spec.Name;
		Variant.Description = Spec.Description;
		Variant.Pitch = Spec.Pitch;
		UMaterialInstanceConstant* Instance = MakeVariantInstance(State->Content.Root, FString(TEXT("MI_Preview_")) + Spec.Name, Base->Parent);
		Variant.Instance.Reset(Instance);

		// 贴图参数照真正的那份抄过来，开关按变体，平铺倍数 3 → 平面上 3×3。
		using namespace DreamTextureMaterialParam;
		for (const FName& Parameter : { BaseColor, Normal, ORM, Height })
		{
			Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Parameter), UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Base, Parameter));
		}
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(Instance, UseNormal, Spec.bUseNormal, EMaterialParameterAssociation::GlobalParameter, false);
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(Instance, UseORM, Spec.bUseORM, EMaterialParameterAssociation::GlobalParameter, false);
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(Instance, UsePOM, Spec.bUsePOM, EMaterialParameterAssociation::GlobalParameter, false);
		UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, UVTiling, 3.f);
		UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
	}

	// ---- 场景：一块 300×300 的平面，方向光从斜上方打来，加一盏天光 ----
	FPreviewScene::ConstructionValues Values;
	Values.SetLightRotation(FRotator(-32.f, 35.f, 0.f)).SetSkyBrightness(1.f).SetLightBrightness(PI * 3.f).SetCreatePhysicsScene(false).SetTransactional(false);
	// 最终输出时的抖动（dither）每帧都不一样，连续两帧平均差 1~2 个灰阶；预览图要确定的一帧，关掉。
	State->SetCVar(TEXT("r.Tonemapper.GrainQuantization"), 0);
	State->Scene = MakeUnique<FPreviewScene>(Values);
	if (State->Scene->SkyLight != nullptr)
	{
		State->Scene->SetSkyCubemap(LoadObject<UTextureCube>(nullptr, TEXT("/Engine/EditorMaterials/AssetViewer/EpicQuadPanorama_CC+EV1.EpicQuadPanorama_CC+EV1")));
	}

	UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (!TestNotNull(TEXT("引擎的 Plane 网格在"), PlaneMesh))
	{
		return false;
	}
	UStaticMeshComponent* Plane = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
	Plane->SetStaticMesh(PlaneMesh);
	Plane->SetMaterial(0, State->Variants[0].Instance.Get());
	State->Plane.Reset(Plane);
	State->Scene->AddComponent(Plane, FTransform(FRotator::ZeroRotator, FVector::ZeroVector, FVector(3.f, 3.f, 1.f)));

	UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
	Target->RenderTargetFormat = RTF_RGBA8_SRGB;
	Target->bAutoGenerateMips = false;
	Target->InitAutoFormat(PreviewSize, PreviewSize);
	Target->UpdateResourceImmediate(true);
	State->Target.Reset(Target);

	USceneCaptureComponent2D* Capture = NewObject<USceneCaptureComponent2D>(GetTransientPackage(), NAME_None, RF_Transient);
	Capture->TextureTarget = Target;
	Capture->CaptureSource = SCS_FinalColorLDR;
	Capture->bCaptureEveryFrame = false;
	Capture->bCaptureOnMovement = false;
	Capture->bAlwaysPersistRenderingState = true;
	Capture->FOVAngle = 40.f;
	// 画面只关心材质本身：去掉会随机或随环境变的后处理。
	TArray<FEngineShowFlagsSetting> ShowFlagSettings;
	for (const TCHAR* Flag : { TEXT("EyeAdaptation"), TEXT("Bloom"), TEXT("MotionBlur"), TEXT("Fog"), TEXT("Atmosphere"), TEXT("VolumetricFog"), TEXT("AntiAliasing"), TEXT("TemporalAA"), TEXT("Vignette") })
	{
		FEngineShowFlagsSetting Setting;
		Setting.ShowFlagName = Flag;
		Setting.Enabled = false;
		ShowFlagSettings.Add(Setting);
	}
	Capture->SetShowFlagSettings(ShowFlagSettings);
	// 项目默认是 TSR，它的子像素抖动让每一帧的像素都差一点，画面永远「不稳定」（上面关掉了 TemporalAA 与 AntiAliasing 两个显示标志）。
	State->Capture.Reset(Capture);
	// 位置在每个变体开始时按俯角重新摆（绕平面中心，距离 345 厘米，实测这个距离平面刚好占满画面）。
	State->Scene->AddComponent(Capture, FTransform(FRotator(-90.f, 0.f, 0.f), FVector(0.f, 0.f, 345.f)));

	// 天光的立方体贴图要「捕获」一次才会参与光照；不更新的话背光面只有方向光，是死黑的。
	State->Scene->UpdateCaptureContents();

	AddInfo(DreamTextureRenderTestUtils::DescribeRenderer());

	// ---- 逐个变体：编译、画若干帧直到画面稳定，存 PNG ----
	AddLatent([this, State, bAborted]() -> bool
	{
		if (*bAborted)
		{
			return true;
		}
		const double Now = FPlatformTime::Seconds();

		if (State->Current >= State->Variants.Num())
		{
			return true;
		}

		if (State->Current < 0 || State->StableFrames >= 6 || Now - State->VariantStarted > 30.0)
		{
			// 收尾上一个变体。
			if (State->Current >= 0)
			{
				FPreviewVariant& Done = State->Variants[State->Current];
				const bool bTimedOut = State->StableFrames < 6;
				if (bTimedOut)
				{
					AddWarning(FString::Printf(TEXT("变体 %s：30 秒内画面没稳定下来，存的是最后一帧。"), *Done.Name));
				}
				Done.SavedPath = DreamTextureRenderTestUtils::GetOutputDir() / FString::Printf(TEXT("preview_%s.png"), *Done.Name);
				TestTrue(FString::Printf(TEXT("变体 %s：PNG 存出来了"), *Done.Name), SavePng(Done.SavedPath, Done.Pixels, PreviewSize, PreviewSize));
				TestTrue(FString::Printf(TEXT("变体 %s：画面不是纯色（对比度 %.1f，平均亮度 %.1f）"), *Done.Name, ContrastOf(Done.Pixels), MeanLuma(Done.Pixels)),
					ContrastOf(Done.Pixels) > 3.f);
				TestTrue(FString::Printf(TEXT("变体 %s：画面不是全黑（平均亮度 %.1f）"), *Done.Name, MeanLuma(Done.Pixels)), MeanLuma(Done.Pixels) > 8.f);
				AddInfo(FString::Printf(TEXT("预览图 %s（%s；画了 %d 帧，末两帧平均差 %.2f）：%s"), *Done.Name, *Done.Description, State->FramesOnVariant, State->LastDiff, *Done.SavedPath));
			}

			++State->Current;
			State->FramesOnVariant = 0;
			State->StableFrames = 0;
			State->Previous.Reset();
			State->LastDiff = 0.f;
			State->VariantStarted = Now;
			if (State->Current >= State->Variants.Num())
			{
				return true;
			}

			FPreviewVariant& Next = State->Variants[State->Current];
			State->Plane->SetMaterial(0, Next.Instance.Get());

			// 相机绕着平面中心转：俯角 -90 在正上方，-45 在 -X 侧斜上方。
			const float PitchRadians = FMath::DegreesToRadians(-Next.Pitch);
			const FVector Location(-345.f * FMath::Cos(PitchRadians), 0.f, 345.f * FMath::Sin(PitchRadians));
			State->Capture->SetWorldLocationAndRotation(Location, FRotator(Next.Pitch, 0.f, 0.f));
			State->Scene->UpdateCaptureContents();

			// 先把这个变体的着色器编出来并核对没有错误；网格通道的着色器在第一次画时按需编。
			const DreamTextureRenderTestUtils::FCompileOutcome Outcome = DreamTextureRenderTestUtils::WaitForCompile(Next.Instance.Get());
			TestTrue(FString::Printf(TEXT("变体 %s：编译没有错误（%s）"), *Next.Name, *FString::Join(Outcome.Errors, TEXT(" | "))), Outcome.Errors.Num() == 0);
			return false;
		}

		// 画一帧、读回、和上一帧比。连续 6 帧一模一样才算稳定（PSO 预缓存、纹理流送、着色器按需编译都会让头几帧不一样）。
		++State->FramesOnVariant;
		State->Capture->CaptureScene();
		FlushRenderingCommands();

		FPreviewVariant& Variant = State->Variants[State->Current];
		Variant.Pixels.Reset();
		if (FTextureRenderTargetResource* Resource = State->Target->GameThread_GetRenderTargetResource())
		{
			Resource->ReadPixels(Variant.Pixels);
		}
		State->LastDiff = MeanAbsDiff(Variant.Pixels, State->Previous);
		State->Previous = Variant.Pixels;
		// 稳定 = 已经画了一阵 + 没有着色器还在编（网格通道的着色器第一次画时才按需编，编完前画的是占位材质，
		// 占位材质本身也是「稳定」的，光看帧间差会被它骗过去）+ 连续几帧几乎不变。
		const bool bShadersBusy = GShaderCompilingManager != nullptr && GShaderCompilingManager->IsCompiling();
		if (State->FramesOnVariant > 20 && !bShadersBusy && State->LastDiff < 2.5f)
		{
			++State->StableFrames;
		}
		else
		{
			State->StableFrames = 0;
		}
		return false;
	});

	// 全部变体都画完之后，拼一张对比图（每行三张，一眼看差别），再核对几条保底的断言。
	AddLatentStep([this, State, bAborted]()
	{
		if (*bAborted)
		{
			return;
		}
		constexpr int32 Columns = 3;
		const int32 Count = State->Variants.Num();
		const int32 Rows = (Count + Columns - 1) / Columns;
		const int32 SheetWidth = PreviewSize * Columns;
		const int32 SheetHeight = PreviewSize * Rows;
		TArray<FColor> Sheet;
		Sheet.SetNumZeroed(SheetWidth * SheetHeight);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const TArray<FColor>& Pixels = State->Variants[Index].Pixels;
			if (Pixels.Num() != PreviewSize * PreviewSize)
			{
				continue;
			}
			const int32 CellX = (Index % Columns) * PreviewSize;
			const int32 CellY = (Index / Columns) * PreviewSize;
			for (int32 Y = 0; Y < PreviewSize; ++Y)
			{
				FMemory::Memcpy(&Sheet[(CellY + Y) * SheetWidth + CellX], &Pixels[Y * PreviewSize], PreviewSize * sizeof(FColor));
			}
		}
		const FString Path = DreamTextureRenderTestUtils::GetOutputDir() / TEXT("preview_sheet.png");
		TestTrue(TEXT("对比图存出来了"), SavePng(Path, Sheet, SheetWidth, SheetHeight));
		AddInfo(FString::Printf(TEXT("预览对比图（左上到右下：default / pom / basecolor_only / default_oblique / pom_oblique）：%s"), *Path));

		// 画面之间应当有实质差别——没有差别说明开关没起作用，或者画出来的全是同一个默认材质。
		// 这里把抗锯齿关了，连续两帧应当几乎一模一样，所以阈值取得很低。
		auto Diff = [State](int32 A, int32 B) { return MeanAbsDiff(State->Variants[A].Pixels, State->Variants[B].Pixels); };
		if (Count >= 5)
		{
			AddInfo(FString::Printf(TEXT("画面平均差（0~255）：default↔basecolor_only %.2f，default↔pom %.2f，default_oblique↔pom_oblique %.2f，default↔default_oblique %.2f"),
				Diff(0, 2), Diff(0, 1), Diff(3, 4), Diff(0, 3)));
			TestTrue(TEXT("default 与 basecolor_only 的画面不同（法线与 ORM 起了作用）"), Diff(0, 2) > 2.f);
			TestTrue(TEXT("斜看时 default 与 pom 的画面不同（视差起了作用）"), Diff(3, 4) > 1.f);
			TestTrue(TEXT("正上方看与斜看的画面不同（相机真的动了）"), Diff(0, 3) > 2.f);
		}
	});

	AddLatentStep([State]()
	{
		State->Teardown();
	});

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
