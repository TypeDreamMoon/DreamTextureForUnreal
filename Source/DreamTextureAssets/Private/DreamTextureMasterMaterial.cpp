// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureMaterialBuilder.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "DreamTextureAssetUtil.h"
#include "DreamTextureLog.h"
#include "Engine/Texture2D.h"
#include "FileHelpers.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionStaticBoolParameter.h"
#include "Materials/MaterialExpressionStaticSwitch.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionUtils.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialFunction.h"
#include "Misc/PackageName.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"

/*
 * 母材质 M_DreamTexture_Master 的图。完整的连线见 BuildGraph 里的注释；这里先说几个做法上的取舍。
 *
 * 1. 三个静态开关各只有一个节点（UMaterialExpressionStaticBoolParameter），分支用
 *    UMaterialExpressionStaticSwitch 的 Value 引脚去接它。UseORM 要同时切 AO / 粗糙度 / 金属度三路，
 *    如果每路都放一个同名的 StaticSwitchParameter，就是三个同名参数——编辑器会警告，
 *    且只认第一个的默认值。
 *
 * 2. 关掉的分支不会被编译：UseORM 关时 ORM 贴图根本没被采样，UsePOM 关时视差函数也不会进着色器。
 *    所以开关既是「走哪条路」，也是「省不省采样」。
 *
 * 3. 所有贴图参数都带一张能通过采样器类型检查的引擎默认贴图（见 GetDefaultTexturePath）。
 *    默认贴图一旦不符，母材质自己就编不过；实例里换上去的贴图不符，则会在运行时退回默认图。
 */

namespace
{
	/** 版本号记在包元数据里用的键。与贴图的来源记录同一套做法（见 DreamTextureImporter.cpp 里 SourceKey 的注释）。 */
	const TCHAR* const VersionKey = TEXT("DreamTexture.MasterGraphVersion");

	// ---- 引擎自带内容的路径。5.8 的 Engine/Content 里实测核对过压缩设置与 sRGB（见 GetDefaultTexturePath 的注释）。 ----

	const TCHAR* const PomFunctionPath = TEXT("/Engine/Functions/Engine_MaterialFunctions01/Texturing/ParallaxOcclusionMapping.ParallaxOcclusionMapping");

	/**
	 * 每个贴图参数的默认贴图，以及它应当配的采样器类型。
	 *
	 * 采样器类型由贴图自己的压缩设置与 sRGB 决定（MaterialExpressionUtils::GetSamplerTypeForTexture），
	 * 参数节点上填的类型必须与它**完全相等**，否则材质编译报 “Sampler type is X, should be Y for …”。
	 *
	 *   BaseColor  TC_Default + sRGB 开    → SAMPLERTYPE_Color
	 *   Normal     TC_Normalmap            → SAMPLERTYPE_Normal
	 *   ORM        TC_Masks（sRGB 必须关）  → SAMPLERTYPE_Masks
	 *   Height     TC_Grayscale + sRGB 关   → SAMPLERTYPE_LinearGrayscale
	 *
	 * 注意最后一行：manifest 里高度图是 linear，导入器因此把 sRGB 关掉，
	 * 采样器类型就是 **LinearGrayscale**，不是直觉上的 Grayscale（那个对应 sRGB 开）。
	 *
	 * 默认贴图（逐个在 5.8 里加载验证过）：
	 *   BaseColor  /Engine/EngineResources/WhiteSquareTexture   纯白，乘上 Tint 后就是 Tint 本身
	 *   Normal     /Engine/EngineMaterials/FlatNormal           16×16 的平坦法线，TC_Normalmap
	 *   ORM        /Engine/EngineMaterials/DefaultDiffuse_TC_Masks   引擎里唯一一张 TC_Masks 的通用贴图
	 *   Height     /Engine/EngineMaterials/BaseFlattenGrayscaleMap   32×32，TC_Grayscale + sRGB 关
	 * 这些路径都在 /Engine 下：引擎内容打包时跟着走，不引用插件，所以不破坏「产物与插件无关」。
	 */
	struct FTextureSlot
	{
		FName ParameterName;
		const TCHAR* DefaultTexturePath;
		EMaterialSamplerType SamplerType;
	};

	const FTextureSlot& GetSlot(const FName& ParameterName)
	{
		static const FTextureSlot Slots[] = {
			{ DreamTextureMaterialParam::BaseColor, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"), SAMPLERTYPE_Color },
			{ DreamTextureMaterialParam::Normal, TEXT("/Engine/EngineMaterials/FlatNormal.FlatNormal"), SAMPLERTYPE_Normal },
			{ DreamTextureMaterialParam::ORM, TEXT("/Engine/EngineMaterials/DefaultDiffuse_TC_Masks.DefaultDiffuse_TC_Masks"), SAMPLERTYPE_Masks },
			{ DreamTextureMaterialParam::Height, TEXT("/Engine/EngineMaterials/BaseFlattenGrayscaleMap.BaseFlattenGrayscaleMap"), SAMPLERTYPE_LinearGrayscale },
		};
		for (const FTextureSlot& Slot : Slots)
		{
			if (Slot.ParameterName == ParameterName)
			{
				return Slot;
			}
		}
		checkNoEntry();
		return Slots[0];
	}

	// 参数分组。FName，只用 ASCII——材质实例编辑器按这个名字分节。
	const FName GroupTextures(TEXT("Textures"));
	const FName GroupUV(TEXT("UV"));
	const FName GroupColor(TEXT("Color"));
	const FName GroupNormal(TEXT("Normal"));
	const FName GroupSurface(TEXT("Roughness and AO"));
	const FName GroupHeight(TEXT("Height and POM"));
	const FName GroupFallback(TEXT("Fallback (when ORM is off)"));

	/** 节点摆放的栅格：列越靠右越接近材质输出。纯排版，不影响行为，只为了在材质编辑器里读得顺。 */
	constexpr int32 ColumnWidth = 300;
	constexpr int32 RowHeight = 140;
	constexpr int32 OriginX = -2400;

	/** 一次建图的上下文。出错时 Error 非空，后续的创建一律短路，最后由 Build 统一返回。 */
	struct FGraphBuilder
	{
		UMaterial* Material = nullptr;
		FString Error;

		/** 已经建出的节点数，给测试用的失败注入计数。 */
		int32 NodesBuilt = 0;

		/** POM 函数是否真的接上了。接不上（引擎里找不到函数、或引脚名对不上）时 UsePOM 退化成空开关。 */
		bool bPomWired = false;

		bool Failed() const { return !Error.IsEmpty(); }

		template <class T>
		T* Add(int32 Column, int32 Row)
		{
			if (Failed())
			{
				return nullptr;
			}
			if (FDreamTextureMasterMaterial::DebugFailAfterNodeCount >= 0 && NodesBuilt >= FDreamTextureMasterMaterial::DebugFailAfterNodeCount)
			{
				Error = TEXT("(failure injected by a test)");
				return nullptr;
			}
			++NodesBuilt;
			T* Expression = Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(Material, T::StaticClass(), OriginX + Column * ColumnWidth, Row * RowHeight));
			if (Expression == nullptr)
			{
				Error = FText::Format(NSLOCTEXT("DreamTextureMasterMaterial", "CreateExpressionFailed", "Could not create the material expression {0}"), FText::FromString(T::StaticClass()->GetName())).ToString();
			}
			return Expression;
		}

		static void Describe(UMaterialExpression* Expression, const TCHAR* Description)
		{
			// Desc 既是节点上的注释，也是材质实例编辑器里参数的提示文字。
			Expression->Desc = Description;
		}

		UMaterialExpressionScalarParameter* Scalar(const FName& Name, float Default, const FName& Group, int32 Sort, const TCHAR* Description, int32 Column, int32 Row)
		{
			UMaterialExpressionScalarParameter* Expression = Add<UMaterialExpressionScalarParameter>(Column, Row);
			if (Expression != nullptr)
			{
				Expression->ParameterName = Name;
				Expression->DefaultValue = Default;
				Expression->Group = Group;
				Expression->SortPriority = Sort;
				Describe(Expression, Description);
			}
			return Expression;
		}

		UMaterialExpressionStaticBoolParameter* StaticBool(const FName& Name, bool bDefault, int32 Sort, const TCHAR* Description, int32 Column, int32 Row)
		{
			UMaterialExpressionStaticBoolParameter* Expression = Add<UMaterialExpressionStaticBoolParameter>(Column, Row);
			if (Expression != nullptr)
			{
				Expression->ParameterName = Name;
				Expression->DefaultValue = bDefault;
				Expression->Group = GroupTextures;
				Expression->SortPriority = Sort;
				Describe(Expression, Description);
			}
			return Expression;
		}

		/** 取默认贴图并核对它与采样器类型对得上。对不上直接记成错误：这是代码写错了，不该留到编译材质时才发现。 */
		UTexture* LoadDefaultTexture(const FTextureSlot& Slot)
		{
			if (Failed())
			{
				return nullptr;
			}
			UTexture* Texture = LoadObject<UTexture>(nullptr, Slot.DefaultTexturePath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (Texture == nullptr)
			{
				Error = FText::Format(NSLOCTEXT("DreamTextureMasterMaterial", "DefaultTextureMissing", "Could not find the engine's default texture {0} (parameter {1})"), FText::FromString(Slot.DefaultTexturePath), FText::FromString(Slot.ParameterName.ToString())).ToString();
				return nullptr;
			}
			FString Mismatch;
			if (!MaterialExpressionUtils::VerifySamplerType(Texture->GetPathName(), MaterialExpressionUtils::GetSamplerTypeForTexture(Texture), Texture->SRGB, Slot.SamplerType, Mismatch))
			{
				Error = FText::Format(NSLOCTEXT("DreamTextureMasterMaterial", "DefaultTextureSamplerMismatch", "The default texture does not match the sampler type (parameter {0}): {1}"), FText::FromString(Slot.ParameterName.ToString()), FText::FromString(Mismatch)).ToString();
				return nullptr;
			}
			return Texture;
		}

		UMaterialExpressionTextureSampleParameter2D* TextureSample(const FName& Name, int32 Sort, const TCHAR* Description, int32 Column, int32 Row)
		{
			const FTextureSlot& Slot = GetSlot(Name);
			UTexture* Default = LoadDefaultTexture(Slot);
			UMaterialExpressionTextureSampleParameter2D* Expression = Default != nullptr ? Add<UMaterialExpressionTextureSampleParameter2D>(Column, Row) : nullptr;
			if (Expression != nullptr)
			{
				Expression->ParameterName = Name;
				Expression->Texture = Default;
				Expression->SamplerType = Slot.SamplerType;
				Expression->Group = GroupTextures;
				Expression->SortPriority = Sort;
				Describe(Expression, Description);
			}
			return Expression;
		}

		/** 输出贴图对象本身、不采样的参数——给视差函数用，它要自己用不同的 UV 去采高度。 */
		UMaterialExpressionTextureObjectParameter* TextureObject(const FName& Name, int32 Sort, const TCHAR* Description, int32 Column, int32 Row)
		{
			const FTextureSlot& Slot = GetSlot(Name);
			UTexture* Default = LoadDefaultTexture(Slot);
			UMaterialExpressionTextureObjectParameter* Expression = Default != nullptr ? Add<UMaterialExpressionTextureObjectParameter>(Column, Row) : nullptr;
			if (Expression != nullptr)
			{
				Expression->ParameterName = Name;
				Expression->Texture = Default;
				Expression->SamplerType = Slot.SamplerType;
				Expression->Group = GroupTextures;
				Expression->SortPriority = Sort;
				Describe(Expression, Description);
			}
			return Expression;
		}

		UMaterialExpressionMultiply* Multiply(UMaterialExpression* A, int32 AOut, UMaterialExpression* B, int32 BOut, int32 Column, int32 Row)
		{
			UMaterialExpressionMultiply* Expression = Add<UMaterialExpressionMultiply>(Column, Row);
			if (Expression != nullptr)
			{
				Expression->A.Connect(AOut, A);
				Expression->B.Connect(BOut, B);
			}
			return Expression;
		}

		UMaterialExpressionLinearInterpolate* Lerp(UMaterialExpression* A, int32 AOut, UMaterialExpression* B, int32 BOut, UMaterialExpression* Alpha, int32 AlphaOut, int32 Column, int32 Row)
		{
			UMaterialExpressionLinearInterpolate* Expression = Add<UMaterialExpressionLinearInterpolate>(Column, Row);
			if (Expression != nullptr)
			{
				Expression->A.Connect(AOut, A);
				Expression->B.Connect(BOut, B);
				Expression->Alpha.Connect(AlphaOut, Alpha);
			}
			return Expression;
		}

		/** Value 为真走 WhenTrue（StaticSwitch 的 A 引脚），否则走 WhenFalse（B 引脚）。 */
		UMaterialExpressionStaticSwitch* Switch(UMaterialExpression* Value, UMaterialExpression* WhenTrue, int32 TrueOut, UMaterialExpression* WhenFalse, int32 FalseOut, int32 Column, int32 Row)
		{
			UMaterialExpressionStaticSwitch* Expression = Add<UMaterialExpressionStaticSwitch>(Column, Row);
			if (Expression != nullptr)
			{
				Expression->Value.Connect(0, Value);
				Expression->A.Connect(TrueOut, WhenTrue);
				Expression->B.Connect(FalseOut, WhenFalse);
			}
			return Expression;
		}

		UMaterialExpressionConstant* Constant(float Value, int32 Column, int32 Row)
		{
			UMaterialExpressionConstant* Expression = Add<UMaterialExpressionConstant>(Column, Row);
			if (Expression != nullptr)
			{
				Expression->R = Value;
			}
			return Expression;
		}

		/** 接到材质的一个输出属性上。 */
		void ConnectProperty(EMaterialProperty Property, UMaterialExpression* From, int32 OutputIndex)
		{
			if (Failed())
			{
				return;
			}
			FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
			if (Input == nullptr || From == nullptr)
			{
				Error = FText::Format(NSLOCTEXT("DreamTextureMasterMaterial", "ConnectPropertyFailed", "Could not connect material property {0}"), static_cast<int32>(Property)).ToString();
				return;
			}
			Input->Connect(OutputIndex, From);
		}

		/**
		 * 视差遮蔽。返回「视差后的 UV」所在的表达式与输出下标；做不出来返回 nullptr，调用方退回普通 UV。
		 *
		 * 用引擎自带的 ParallaxOcclusionMapping 材质函数，不自己写 raymarch。引脚是按名字找的
		 * （5.8 里实测：输入 Heightmap Texture / Height Ratio / UVs / Heightmap Channel，输出 Parallax UVs），
		 * 以后引擎改了名字就会找不到——此时不让整张母材质建不出来，只是 POM 不工作，并留一条警告。
		 */
		UMaterialExpression* BuildPom(UMaterialExpression* UV, UMaterialExpression* HeightObject, UMaterialExpression* HeightRatio, int32 Column, int32 Row, int32& OutOutputIndex)
		{
			OutOutputIndex = 0;
			if (Failed())
			{
				return nullptr;
			}

			UMaterialFunction* Function = LoadObject<UMaterialFunction>(nullptr, PomFunctionPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (Function == nullptr)
			{
				UE_LOG(LogDreamTexture, Warning, TEXT("Could not find the engine's parallax occlusion mapping function %s; UsePOM will have no effect."), PomFunctionPath);
				return nullptr;
			}

			UMaterialExpressionMaterialFunctionCall* Call = Add<UMaterialExpressionMaterialFunctionCall>(Column, Row);
			if (Call == nullptr || !Call->SetMaterialFunction(Function))
			{
				UE_LOG(LogDreamTexture, Warning, TEXT("Could not attach the parallax occlusion mapping function; UsePOM will have no effect."));
				return nullptr;
			}

			// 高度图取 R 通道：导入的高度图是 TC_Grayscale，单通道，采样出来 RGB 同值，取 R 最稳。
			UMaterialExpressionConstant4Vector* Channel = Add<UMaterialExpressionConstant4Vector>(Column - 1, Row + 1);
			if (Channel == nullptr)
			{
				return nullptr;
			}
			Channel->Constant = FLinearColor(1.f, 0.f, 0.f, 0.f);

			bool bAllWired = true;
			auto Wire = [Call, &bAllWired](const TCHAR* InputName, UMaterialExpression* From)
			{
				for (FFunctionExpressionInput& Input : Call->FunctionInputs)
				{
					if (Input.ExpressionInput != nullptr && Input.ExpressionInput->InputName == FName(InputName))
					{
						Input.Input.Connect(0, From);
						return;
					}
				}
				UE_LOG(LogDreamTexture, Warning, TEXT("The parallax occlusion mapping function has no input named '%s'; the engine function may have changed."), InputName);
				bAllWired = false;
			};
			Wire(TEXT("Heightmap Texture"), HeightObject);
			Wire(TEXT("Height Ratio"), HeightRatio);
			Wire(TEXT("UVs"), UV);
			Wire(TEXT("Heightmap Channel"), Channel);

			int32 UVOutput = INDEX_NONE;
			for (int32 Index = 0; Index < Call->Outputs.Num(); ++Index)
			{
				if (Call->Outputs[Index].OutputName == FName(TEXT("Parallax UVs")))
				{
					UVOutput = Index;
					break;
				}
			}
			if (!bAllWired || UVOutput == INDEX_NONE)
			{
				UE_LOG(LogDreamTexture, Warning, TEXT("The parallax occlusion mapping function is not fully wired; UsePOM will have no effect."));
				return nullptr;
			}

			OutOutputIndex = UVOutput;
			return Call;
		}

		/**
		 * 图的全貌。左边是输入，右边是材质的五个输出属性。
		 *
		 *   TexCoord[0] ─┐
		 *   UVTiling ────┴─ × ─────────────┐(false)
		 *                  │                │
		 *   Height(贴图对象) ┐ (UVs)         │
		 *   HeightRatio ────┼─► POM ─UVs──► Switch[UsePOM] ──► UV ──┬─► BaseColor(采样) ─┐
		 *   (1,0,0,0) ──────┘                                       │                    × BaseColorTint ─► BaseColor
		 *                                                           ├─► Normal(采样) ──► Lerp((0,0,1), n, NormalStrength)
		 *                                                           │        └─► Switch[UseNormal](false: (0,0,1)) ─► Normal
		 *                                                           └─► ORM(采样) ─┬ R ─► Lerp(1, ao, AOStrength) ─┐
		 *                                                                          ├ G ─► Lerp(RoughnessMin, RoughnessMax, g) ┤ Switch[UseORM] ─► AO / Roughness / Metallic
		 *                                                                          └ B ─────────────────────────────────────┘ (false: 1 / Roughness / Metallic 标量)
		 */
		bool Build()
		{
			using namespace DreamTextureMaterialParam;

			// ---- UV ----
			UMaterialExpressionTextureCoordinate* TexCoord = Add<UMaterialExpressionTextureCoordinate>(0, 0);
			UMaterialExpressionScalarParameter* UVTilingParam = Scalar(UVTiling, 1.f, GroupUV, 0,
				TEXT("UV tiling multiplier. 1 = the texture repeats once over UV 0-1; 2 = it repeats 2x2."), 0, 1);
			UMaterialExpressionMultiply* UVScaled = TexCoord != nullptr && UVTilingParam != nullptr ? Multiply(TexCoord, 0, UVTilingParam, 0, 1, 0) : nullptr;

			// ---- 视差遮蔽（UsePOM）----
			UMaterialExpressionStaticBoolParameter* UsePomParam = StaticBool(UsePOM, false, 12,
				TEXT("Parallax occlusion mapping. It adds noticeable shading cost and is off by default. Assign a Height texture first."), 0, 7);
			UMaterialExpressionTextureObjectParameter* HeightObject = TextureObject(Height, 3,
				TEXT("Height map (white = high). Only used when UsePOM is on."), 0, 3);
			UMaterialExpressionScalarParameter* HeightRatioParam = Scalar(HeightRatio, 0.05f, GroupHeight, 1,
				TEXT("Parallax depth, as a fraction of the texture's tiling size. Larger values look deeper but stretch if too large."), 0, 5);

			int32 PomOutput = 0;
			UMaterialExpression* Pom = UVScaled != nullptr ? BuildPom(UVScaled, HeightObject, HeightRatioParam, 1, 3, PomOutput) : nullptr;
			bPomWired = Pom != nullptr;

			// POM 接不上时，开关两边都是普通 UV：UsePOM 这个参数仍在（实例里不会丢），只是不起作用。
			UMaterialExpressionStaticSwitch* UVSwitch = UVScaled != nullptr && UsePomParam != nullptr
				? Switch(UsePomParam, bPomWired ? Pom : UVScaled, bPomWired ? PomOutput : 0, UVScaled, 0, 2, 1)
				: nullptr;

			if (Failed() || UVSwitch == nullptr)
			{
				return false;
			}

			// ---- BaseColor ----
			UMaterialExpressionTextureSampleParameter2D* BaseColorSample = TextureSample(BaseColor, 0, TEXT("Base Color texture (sRGB)."), 3, 0);
			UMaterialExpressionVectorParameter* TintParam = Add<UMaterialExpressionVectorParameter>(3, 1);
			if (BaseColorSample == nullptr || TintParam == nullptr)
			{
				return false;
			}
			BaseColorSample->Coordinates.Connect(0, UVSwitch);
			TintParam->ParameterName = BaseColorTint;
			TintParam->DefaultValue = FLinearColor::White;
			TintParam->Group = GroupColor;
			TintParam->SortPriority = 0;
			Describe(TintParam, TEXT("Base Color tint, multiplied with the Base Color texture. White = unchanged."));
			UMaterialExpressionMultiply* TintedBase = Multiply(BaseColorSample, 0, TintParam, 0, 4, 0);

			// ---- Normal ----
			UMaterialExpressionTextureSampleParameter2D* NormalSample = TextureSample(Normal, 1, TEXT("Normal texture (DirectX convention; the green channel is not flipped)."), 3, 3);
			UMaterialExpressionConstant3Vector* FlatNormal = Add<UMaterialExpressionConstant3Vector>(3, 4);
			UMaterialExpressionScalarParameter* NormalStrengthParam = Scalar(NormalStrength, 1.f, GroupNormal, 0,
				TEXT("Normal strength. 0 = flat, 1 = as authored, above 1 = exaggerated."), 3, 5);
			UMaterialExpressionStaticBoolParameter* UseNormalParam = StaticBool(UseNormal, true, 10,
				TEXT("Use the Normal texture. Import turns this off when there is no Normal texture, and the normal falls back to flat (0,0,1)."), 4, 6);
			if (NormalSample == nullptr || FlatNormal == nullptr || NormalStrengthParam == nullptr || UseNormalParam == nullptr)
			{
				return false;
			}
			NormalSample->Coordinates.Connect(0, UVSwitch);
			FlatNormal->Constant = FLinearColor(0.f, 0.f, 1.f);
			// 在平坦法线与贴图法线之间插值，是引擎里调法线强度最常见的做法；
			// 材质输出阶段引擎还会再归一化一次，所以不必在这里自己 normalize。
			UMaterialExpressionLinearInterpolate* NormalBlend = Lerp(FlatNormal, 0, NormalSample, 0, NormalStrengthParam, 0, 4, 3);
			UMaterialExpressionStaticSwitch* NormalSwitch = NormalBlend != nullptr ? Switch(UseNormalParam, NormalBlend, 0, FlatNormal, 0, 5, 3) : nullptr;

			// ---- ORM ----
			UMaterialExpressionTextureSampleParameter2D* ORMSample = TextureSample(ORM, 2, TEXT("R = Ambient Occlusion, G = Roughness, B = Metallic (linear)."), 3, 8);
			UMaterialExpressionStaticBoolParameter* UseORMParam = StaticBool(UseORM, true, 11,
				TEXT("Use the packed texture (ORM). Import turns this off when there is no ORM texture; Roughness and Metallic then come from the scalars below, and Ambient Occlusion is 1."), 4, 12);
			UMaterialExpressionConstant* One = Constant(1.f, 3, 9);
			UMaterialExpressionScalarParameter* AOStrengthParam = Scalar(AOStrength, 1.f, GroupSurface, 2,
				TEXT("Ambient Occlusion strength. 0 = no Ambient Occlusion, 1 = as authored."), 3, 10);
			UMaterialExpressionScalarParameter* RoughnessMinParam = Scalar(RoughnessMin, 0.f, GroupSurface, 0,
				TEXT("Lower bound of the Roughness remap: a 0 in the texture maps to this value."), 3, 11);
			UMaterialExpressionScalarParameter* RoughnessMaxParam = Scalar(RoughnessMax, 1.f, GroupSurface, 1,
				TEXT("Upper bound of the Roughness remap: a 1 in the texture maps to this value."), 3, 12);
			UMaterialExpressionScalarParameter* RoughnessFallback = Scalar(Roughness, 0.5f, GroupFallback, 0,
				TEXT("Roughness used when UseORM is off."), 4, 10);
			UMaterialExpressionScalarParameter* MetallicFallback = Scalar(Metallic, 0.f, GroupFallback, 1,
				TEXT("Metallic used when UseORM is off."), 4, 11);
			if (ORMSample == nullptr || UseORMParam == nullptr || One == nullptr || AOStrengthParam == nullptr || RoughnessMinParam == nullptr ||
				RoughnessMaxParam == nullptr || RoughnessFallback == nullptr || MetallicFallback == nullptr)
			{
				return false;
			}
			ORMSample->Coordinates.Connect(0, UVSwitch);

			// TextureSample 的输出：0=RGB 1=R 2=G 3=B 4=A 5=RGBA。
			constexpr int32 OutR = 1;
			constexpr int32 OutG = 2;
			constexpr int32 OutB = 3;
			UMaterialExpressionLinearInterpolate* AOBlend = Lerp(One, 0, ORMSample, OutR, AOStrengthParam, 0, 5, 8);
			UMaterialExpressionLinearInterpolate* RoughnessRemap = Lerp(RoughnessMinParam, 0, RoughnessMaxParam, 0, ORMSample, OutG, 5, 9);
			if (AOBlend == nullptr || RoughnessRemap == nullptr)
			{
				return false;
			}
			UMaterialExpressionStaticSwitch* AOSwitch = Switch(UseORMParam, AOBlend, 0, One, 0, 6, 8);
			UMaterialExpressionStaticSwitch* RoughnessSwitch = Switch(UseORMParam, RoughnessRemap, 0, RoughnessFallback, 0, 6, 9);
			UMaterialExpressionStaticSwitch* MetallicSwitch = Switch(UseORMParam, ORMSample, OutB, MetallicFallback, 0, 6, 10);

			if (Failed() || TintedBase == nullptr || NormalSwitch == nullptr || AOSwitch == nullptr || RoughnessSwitch == nullptr || MetallicSwitch == nullptr)
			{
				return false;
			}

			ConnectProperty(MP_BaseColor, TintedBase, 0);
			ConnectProperty(MP_Normal, NormalSwitch, 0);
			ConnectProperty(MP_Roughness, RoughnessSwitch, 0);
			ConnectProperty(MP_Metallic, MetallicSwitch, 0);
			ConnectProperty(MP_AmbientOcclusion, AOSwitch, 0);

			return !Failed();
		}
	};

	/**
	 * 把这几个表达式从母材质里删掉。
	 *
	 * 不用 UMaterialEditingLibrary::DeleteAllMaterialExpressions：它一边遍历 Material->GetExpressions() 的视图
	 * 一边往同一个数组里删，每删一个后面的元素就前移一位，于是隔一个漏一个（5.8 实测会剩下一半）。
	 * 这里拿的是调用方事先拷好的列表，逐个删。
	 */
	void DeleteExpressions(UMaterial* Material, const TArray<UMaterialExpression*>& Expressions)
	{
		for (UMaterialExpression* Expression : Expressions)
		{
			UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
		}
	}

	/** 存盘。失败时 OutError 给中文原因，包仍是脏的（下一次 FindOrCreate 会补存）。 */
	bool SavePackageOf(UObject* Material, const FString& PackageName, FString& OutError)
	{
		TArray<UPackage*> ToSave;
		ToSave.Add(Material->GetOutermost());
		if (!UEditorLoadingAndSavingUtils::SavePackages(ToSave, /*bOnlyDirty*/ false))
		{
			OutError = FText::Format(NSLOCTEXT("DreamTextureMasterMaterial", "SaveFailed", "The master material was created, but saving it to disk failed: {0} (see the log)"), FText::FromString(PackageName)).ToString();
			return false;
		}
		return true;
	}
}

int32 FDreamTextureMasterMaterial::DebugFailAfterNodeCount = -1;

const TCHAR* FDreamTextureMasterMaterial::GetAssetName()
{
	return TEXT("M_DreamTexture_Master");
}

FString FDreamTextureMasterMaterial::GetObjectPath(const FString& ContentRoot)
{
	return FDreamTextureImporter::ResolveContentRoot(ContentRoot) / GetAssetName() + TEXT(".") + GetAssetName();
}

int32 FDreamTextureMasterMaterial::ReadRecordedVersion(const UObject* Material)
{
#if WITH_METADATA
	if (Material != nullptr)
	{
		if (UPackage* Package = Material->GetOutermost())
		{
			if (const FString* Value = Package->GetMetaData().FindValue(Material, VersionKey))
			{
				return FCString::Atoi(**Value);
			}
		}
	}
#endif
	return 0;
}

bool FDreamTextureMasterMaterial::HasRecordedVersion(const UObject* Material)
{
#if WITH_METADATA
	if (Material != nullptr)
	{
		if (UPackage* Package = Material->GetOutermost())
		{
			return Package->GetMetaData().FindValue(Material, VersionKey) != nullptr;
		}
	}
#endif
	return false;
}

void FDreamTextureMasterMaterial::WriteRecordedVersion(UObject* Material, int32 Version)
{
#if WITH_METADATA
	if (Material != nullptr)
	{
		Material->GetOutermost()->GetMetaData().SetValue(Material, VersionKey, *FString::FromInt(Version));
	}
#endif
}

UMaterial* FDreamTextureMasterMaterial::FindOrCreate(const FString& ContentRoot, bool bSave, FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();

	const FString AssetName = GetAssetName();
	const FString PackageName = FDreamTextureImporter::ResolveContentRoot(ContentRoot) / AssetName;

	FText Reason;
	if (!FPackageName::IsValidLongPackageName(PackageName, /*bIncludeReadOnlyRoots*/ false, &Reason))
	{
		OutError = FText::Format(NSLOCTEXT("DreamTextureMasterMaterial", "InvalidPath", "The master material path is not valid: {0} ({1})"), FText::FromString(PackageName), Reason).ToString();
		return nullptr;
	}

	UObject* Existing = DreamTextureAssetUtil::FindOrLoad(PackageName, AssetName);
	UMaterial* Material = Cast<UMaterial>(Existing);
	if (Existing != nullptr && Material == nullptr)
	{
		OutError = FText::Format(NSLOCTEXT("DreamTextureMasterMaterial", "PathTaken", "The path {0} is already taken by a {1} asset, so the master material cannot be placed there."), FText::FromString(PackageName), FText::FromString(Existing->GetClass()->GetName())).ToString();
		return nullptr;
	}

	const bool bCreate = (Material == nullptr);
	if (!bCreate)
	{
		// 路径上已经有一个 UMaterial，但没有版本记录：不是我们建的，是用户自己的材质。
		// 不能当成「版本 0」去重建——重建是先清空再生成，等于删掉用户的材质。
		// 插件从来没发布过不带版本记录的版本，所以「没有记录」只可能是别人的东西。
		if (!HasRecordedVersion(Material))
		{
			OutError = FText::Format(NSLOCTEXT("DreamTextureMasterMaterial", "NotOurs", "There is already a material at {0}, but this plugin did not generate it (it has no version record), so it will not be overwritten. Rename or move it, then import again."), FText::FromString(PackageName)).ToString();
			return nullptr;
		}

		const int32 Recorded = ReadRecordedVersion(Material);
		if (Recorded >= GraphVersion)
		{
			// 版本一致（或更新的插件建的）：不重建，尤其不标脏——否则每次导入都会让用户的工程多出一个待保存的资产。
			//
			// 但「不重建」不等于「什么都不做」：上一次生成之后保存失败（磁盘满了、文件只读、被版本控制锁着……）的话，
			// 母材质还躺在内存里，包是脏的，版本记录却已经写好了。这里要是直接返回，重试就会把没存盘的当成功，
			// 实例存了盘，它的父材质却不在磁盘上——重开工程实例就断了。所以要存盘且包是脏的，就补存。
			if (bSave && Material->GetOutermost()->IsDirty() && !SavePackageOf(Material, PackageName, OutError))
			{
				return nullptr;
			}
			return Material;
		}
		UE_LOG(LogDreamTexture, Display, TEXT("Master material %s has graph version %d, lower than the current %d; rebuilding in place (same asset, same parameter names; existing material instances are unaffected)."),
			*PackageName, Recorded, GraphVersion);
	}

	if (bCreate)
	{
		UPackage* Package = CreatePackage(*PackageName);
		if (Package == nullptr)
		{
			OutError = FText::Format(NSLOCTEXT("DreamTextureMasterMaterial", "CreatePackageFailed", "Could not create the package: {0}"), FText::FromString(PackageName)).ToString();
			return nullptr;
		}
		Material = NewObject<UMaterial>(Package, FName(AssetName), RF_Public | RF_Standalone | RF_Transactional);
		if (Material == nullptr)
		{
			OutError = FText::Format(NSLOCTEXT("DreamTextureMasterMaterial", "CreateMaterialFailed", "Could not create the master material object: {0}"), FText::FromString(PackageName)).ToString();
			return nullptr;
		}
	}

	// 就地重建的做法：**先在旧图旁边把新图建好，建成了才切过去、再删旧图**，而不是先清空再建。
	// 先清空的话，建到一半失败（引擎内容找不到、节点建不出来……）就留下一个被掏空、又被标脏的母材质，
	// 用户一存盘，所有实例的父材质就成了空壳。并排建的话，失败时旧图完好无损，只要把新建的几个节点删掉、
	// 把脏标记还原就行。切换（把五个输出属性接到新图上）放在最后一步、一次做完，中途失败碰不到它。
	// 并排期间新旧参数同名并存没有问题：参数名是建完节点后才设的，且旧图在重新编译之前已经删光。
	TArray<UMaterialExpression*> OldExpressions;
	const bool bWasDirty = Material->GetOutermost()->IsDirty();
	if (!bCreate)
	{
		OldExpressions = TArray<UMaterialExpression*>(Material->GetExpressions());
	}

	FGraphBuilder Builder;
	Builder.Material = Material;
	const bool bBuilt = Builder.Build();
	if (!bBuilt)
	{
		OutError = FText::Format(NSLOCTEXT("DreamTextureMasterMaterial", "BuildGraphFailed", "An error occurred while building the master material graph: {0}"), Builder.Error.IsEmpty() ? NSLOCTEXT("DreamTextureMasterMaterial", "BuildGraphNodeMissing", "a node could not be created") : FText::FromString(Builder.Error)).ToString();
		if (bCreate)
		{
			// 半成品不留在包里，下次从头来。包也别留着脏标记——里面已经没有资产了，「全部保存」不该列出它。
			//
			// 只 MarkAsGarbage 不够：垃圾对象在下一次 GC 之前仍然挂在原来的路径上，FindObject 照样找得到它——
			// 下一次 FindOrCreate 会把这个半成品当成「已有的母材质」捡起来（没有版本记录，又会被当成用户的材质拒绝）。
			// 所以先把它挪进临时包，路径就腾出来了。
			UPackage* LeftoverPackage = Material->GetOutermost();
			Material->ClearFlags(RF_Public | RF_Standalone);
			Material->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
			Material->MarkAsGarbage();
			LeftoverPackage->ClearDirtyFlag();
		}
		else
		{
			// 回到重建之前：删掉这一轮新建的节点（旧图没动过，输出属性还连在旧图上），把脏标记还原。
			// 版本记录也没更新，下次还会重试。
			TArray<UMaterialExpression*> Added;
			for (UMaterialExpression* Expression : Material->GetExpressions())
			{
				if (!OldExpressions.Contains(Expression))
				{
					Added.Add(Expression);
				}
			}
			DeleteExpressions(Material, Added);
			if (!bWasDirty)
			{
				Material->GetOutermost()->ClearDirtyFlag();
			}
		}
		return nullptr;
	}

	if (!bCreate)
	{
		// 新图已经建成并接到了输出属性上，旧图现在没人引用了，删掉。
		DeleteExpressions(Material, OldExpressions);
		if (UMaterialEditingLibrary::GetNumMaterialExpressions(Material) != Builder.NodesBuilt)
		{
			OutError = NSLOCTEXT("DreamTextureMasterMaterial", "RebuildCountMismatch", "After the rebuild the master material has the wrong number of expressions; the old graph was not fully removed.").ToString();
			return nullptr;
		}
	}

	if (bCreate)
	{
		FAssetRegistryModule::AssetCreated(Material);
	}

	// 预设常见的 bUsedWith 标志。M3 能把实例赋给任何带 MeshComponent 的 Actor；母材质缺某个标志时，
	// 引擎在编辑器里的做法是：第一次有网格用到它（UMaterial::SetMaterialUsage），就地把标志补上、重编着色器、
	// **把母材质包标脏**。用户什么都没改，却看到母材质莫名其妙变成「未保存」，还白等一轮着色器编译，
	// 存盘时才发现它被动过。所以生成的时候就把真会碰到的几个标志开好：
	//   骨骼网格 / 变形目标      —— 角色、带表情的脸
	//   实例化静态网格 / 样条网格 —— 植被、PCG、HISM、路面这类
	//   Nanite                   —— 项目里开了 Nanite 的静态网格
	// 没开的（水、毛发、粒子、Niagara、几何缓存、布料……）要么极少碰到，要么每开一个就多编一整套着色器排列，
	// 碰到时仍由引擎按需补上。静态网格这一项引擎默认就是开的。
	// 5.8 里直接读写 bUsedWith* 成员已弃用，要走 SetUsageByFlag。
	for (const EMaterialUsage Usage : { MATUSAGE_SkeletalMesh, MATUSAGE_MorphTargets, MATUSAGE_InstancedStaticMeshes, MATUSAGE_SplineMesh, MATUSAGE_Nanite })
	{
		Material->SetUsageByFlag(Usage, true);
	}

	WriteRecordedVersion(Material, GraphVersion);

	// 触发材质重编译（PreEditChange/PostEditChange），顺带标脏、刷新引用它的实例与视口。
	// 无渲染环境（-nullrhi）下不会真的编着色器；有渲染环境时着色器在后台编，这里不等。
	UMaterialEditingLibrary::RecompileMaterial(Material);
	Material->MarkPackageDirty();

	if (bSave && !SavePackageOf(Material, PackageName, OutError))
	{
		return nullptr;
	}

	return Material;
}
