// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureMaterialAnalysis.h"
#include "DreamTextureTestUtils.h"
#include "Engine/Texture2D.h"
#include "ImageCore.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMakeMaterialAttributes.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionStaticBoolParameter.h"
#include "Materials/MaterialExpressionStaticSwitch.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceConstant.h"

/** 材质解析（M7）自动化测试共用的小工具：用代码搭材质、造带固定像素的贴图。 */
namespace DreamTextureTestUtils
{
	/** TextureSample 的输出下标：0=RGB 1=R 2=G 3=B 4=A 5=RGBA。 */
	namespace SampleOut
	{
		constexpr int32 RGB = 0;
		constexpr int32 R = 1;
		constexpr int32 G = 2;
		constexpr int32 B = 3;
		constexpr int32 A = 4;
		constexpr int32 RGBA = 5;
	}

	/**
	 * 造一张带固定像素的贴图，放在测试内容目录下（不存盘，随 FScopedTestContent 一起清掉）。
	 * Data 的字节数要与格式、尺寸对得上；每个像素的字节由调用方给，导出测试靠它逐字节比对。
	 */
	inline UTexture2D* MakeTestTexture(const FString& Root, const TCHAR* Name, int32 Width, int32 Height, ERawImageFormat::Type Format,
		const TArray<uint8>& Data, bool bSRGB, TextureCompressionSettings Compression)
	{
		UPackage* Package = CreatePackage(*(Root / Name));
		UTexture2D* Texture = NewObject<UTexture2D>(Package, FName(Name), RF_Public | RF_Standalone | RF_Transactional);
		if (Texture == nullptr)
		{
			return nullptr;
		}

		FImage Image(Width, Height, Format, bSRGB ? EGammaSpace::sRGB : EGammaSpace::Linear);
		check(Image.RawData.Num() == Data.Num());
		FMemory::Memcpy(Image.RawData.GetData(), Data.GetData(), Data.Num());

		Texture->PreEditChange(nullptr);
		Texture->Source.Init(Image);
		Texture->SRGB = bSRGB;
		Texture->CompressionSettings = Compression;
		Texture->MipGenSettings = TMGS_NoMipmaps;
		Texture->PostEditChange();
		return Texture;
	}

	/** 确定性的伪随机字节，同一个 Seed 永远是同一串。 */
	inline TArray<uint8> MakeBytes(int32 Count, uint32 Seed)
	{
		TArray<uint8> Out;
		Out.SetNumUninitialized(Count);
		uint32 State = Seed * 2654435761u + 12345u;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			State = State * 1664525u + 1013904223u;
			Out[Index] = static_cast<uint8>(State >> 24);
		}
		return Out;
	}

	/** 4×4 的小贴图，按用途设好压缩与 sRGB，使它与对应的采样器类型匹配。 */
	inline UTexture2D* MakeColorTexture(const FString& Root, const TCHAR* Name, uint32 Seed = 1)
	{
		return MakeTestTexture(Root, Name, 4, 4, ERawImageFormat::BGRA8, MakeBytes(4 * 4 * 4, Seed), true, TC_Default);
	}

	inline UTexture2D* MakeNormalTexture(const FString& Root, const TCHAR* Name, uint32 Seed = 2)
	{
		return MakeTestTexture(Root, Name, 4, 4, ERawImageFormat::BGRA8, MakeBytes(4 * 4 * 4, Seed), false, TC_Normalmap);
	}

	inline UTexture2D* MakeMasksTexture(const FString& Root, const TCHAR* Name, uint32 Seed = 3)
	{
		return MakeTestTexture(Root, Name, 4, 4, ERawImageFormat::BGRA8, MakeBytes(4 * 4 * 4, Seed), false, TC_Masks);
	}

	inline UTexture2D* MakeGrayTexture(const FString& Root, const TCHAR* Name, uint32 Seed = 4)
	{
		return MakeTestTexture(Root, Name, 4, 4, ERawImageFormat::G8, MakeBytes(4 * 4, Seed), false, TC_Grayscale);
	}

	/** 在测试内容目录下建一个空的 UMaterial，搭图用。 */
	inline UMaterial* MakeTestMaterial(const FString& Root, const TCHAR* Name)
	{
		UPackage* Package = CreatePackage(*(Root / Name));
		return NewObject<UMaterial>(Package, FName(Name), RF_Public | RF_Standalone | RF_Transactional);
	}

	inline UMaterialInstanceConstant* MakeTestInstance(const FString& Root, const TCHAR* Name, UMaterialInterface* Parent)
	{
		UPackage* Package = CreatePackage(*(Root / Name));
		UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(Package, FName(Name), RF_Public | RF_Standalone | RF_Transactional);
		if (Instance != nullptr)
		{
			UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Parent);
		}
		return Instance;
	}

	/** 搭材质图用的薄封装。 */
	struct FGraph
	{
		UMaterial* Material = nullptr;
		int32 NextX = -600;

		explicit FGraph(UMaterial* InMaterial) : Material(InMaterial) {}

		template <class T>
		T* Add()
		{
			NextX += 20;
			return Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(Material, T::StaticClass(), NextX, 0));
		}

		/** 直接引用贴图的采样节点。 */
		UMaterialExpressionTextureSample* Sample(UTexture* Texture, EMaterialSamplerType SamplerType)
		{
			UMaterialExpressionTextureSample* Node = Add<UMaterialExpressionTextureSample>();
			Node->Texture = Texture;
			Node->SamplerType = SamplerType;
			return Node;
		}

		/** 贴图参数。 */
		UMaterialExpressionTextureSampleParameter2D* Param(const TCHAR* Name, UTexture* Default, EMaterialSamplerType SamplerType)
		{
			UMaterialExpressionTextureSampleParameter2D* Node = Add<UMaterialExpressionTextureSampleParameter2D>();
			Node->ParameterName = Name;
			Node->Texture = Default;
			Node->SamplerType = SamplerType;
			return Node;
		}

		UMaterialExpressionScalarParameter* Scalar(const TCHAR* Name, float Value)
		{
			UMaterialExpressionScalarParameter* Node = Add<UMaterialExpressionScalarParameter>();
			Node->ParameterName = Name;
			Node->DefaultValue = Value;
			return Node;
		}

		UMaterialExpressionConstant* Const(float Value)
		{
			UMaterialExpressionConstant* Node = Add<UMaterialExpressionConstant>();
			Node->R = Value;
			return Node;
		}

		UMaterialExpressionMultiply* Multiply(UMaterialExpression* A, int32 AOut, UMaterialExpression* B, int32 BOut = 0)
		{
			UMaterialExpressionMultiply* Node = Add<UMaterialExpressionMultiply>();
			Node->A.Connect(AOut, A);
			Node->B.Connect(BOut, B);
			return Node;
		}

		UMaterialExpressionLinearInterpolate* Lerp(UMaterialExpression* A, int32 AOut, UMaterialExpression* B, int32 BOut, UMaterialExpression* Alpha, int32 AlphaOut = 0)
		{
			UMaterialExpressionLinearInterpolate* Node = Add<UMaterialExpressionLinearInterpolate>();
			Node->A.Connect(AOut, A);
			Node->B.Connect(BOut, B);
			Node->Alpha.Connect(AlphaOut, Alpha);
			return Node;
		}

		/** 取输入的某几个分量。 */
		UMaterialExpressionComponentMask* Mask(UMaterialExpression* Input, int32 InputOut, bool bR, bool bG, bool bB, bool bA)
		{
			UMaterialExpressionComponentMask* Node = Add<UMaterialExpressionComponentMask>();
			Node->Input.Connect(InputOut, Input);
			Node->R = bR;
			Node->G = bG;
			Node->B = bB;
			Node->A = bA;
			return Node;
		}

		void Connect(EMaterialProperty Property, UMaterialExpression* From, int32 OutputIndex = 0)
		{
			if (FExpressionInput* Input = Material->GetExpressionInputForProperty(Property))
			{
				Input->Connect(OutputIndex, From);
			}
		}

		/** 刷新材质的参数缓存。nullrhi 下不编着色器，只更新表达式数据。 */
		void Finish()
		{
			UMaterialEditingLibrary::RecompileMaterial(Material);
		}
	};

	/** 解析结果里按参数名找行。 */
	inline const FDreamTextureMaterialTexture* FindRowByParameter(const FDreamTextureMaterialAnalysis& Analysis, const TCHAR* Parameter)
	{
		for (const FDreamTextureMaterialTexture& Row : Analysis.Textures)
		{
			if (Row.HasParameter(FName(Parameter)))
			{
				return &Row;
			}
		}
		return nullptr;
	}

	/** 解析结果里按贴图资产名找行。 */
	inline const FDreamTextureMaterialTexture* FindRowByName(const FDreamTextureMaterialAnalysis& Analysis, const TCHAR* AssetName)
	{
		for (const FDreamTextureMaterialTexture& Row : Analysis.Textures)
		{
			if (Row.AssetName == AssetName)
			{
				return &Row;
			}
		}
		return nullptr;
	}

	/** 临时的指认库文件：放在这一次测试的内容目录对应的 Saved 下，用完删。 */
	struct FScopedOverrideFile
	{
		FString Path;

		FScopedOverrideFile()
			: Path(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamTextureTest") / TEXT("Overrides") / (FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".json"))))
		{
		}

		~FScopedOverrideFile()
		{
			IFileManager::Get().Delete(*Path, /*RequireExists*/ false, /*EvenReadOnly*/ true);
			IFileManager::Get().Delete(*(Path + TEXT(".tmp")), false, true);
		}
	};
}

#endif // WITH_DEV_AUTOMATION_TESTS
