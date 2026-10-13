// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureMaterialGraph.h"

#include "Engine/Texture.h"
#include "Materials/Material.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpressionBlendMaterialAttributes.h"
#include "Materials/MaterialExpressionBreakMaterialAttributes.h"
#include "Materials/MaterialExpressionBumpOffset.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionGetMaterialAttributes.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMakeMaterialAttributes.h"
#include "Materials/MaterialExpressionMaterialAttributeLayers.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionRuntimeVirtualTextureSample.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "Materials/MaterialExpressionStaticBool.h"
#include "Materials/MaterialExpressionStaticBoolParameter.h"
#include "Materials/MaterialExpressionStaticSwitch.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionSubstrate.h"
#include "Materials/MaterialExpressionTextureObject.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTextureProperty.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"

/*
 * 图遍历的做法（读之前先看这里）。
 *
 * ## 逆着连线走，带着「关心哪些分量」
 *
 * 起点是材质的每个输出属性，终点是贴图采样节点。路上带一个 Interest（4 位，bit0=R…bit3=A）：
 * 「下游到底读了当前这个输出的哪几个分量」。标量属性（粗糙度、金属度…）只读第 0 个分量，
 * 颜色/法线读前三个。穿过节点时按节点的语义改写它：
 *
 *   - ComponentMask：输出的第 k 个分量 = 输入里第 k 个被勾选的通道。所以下游关心 {R}、遮罩勾的是 G，
 *     往上游就变成关心 {G}。打包图的通道排布就是这样读出来的。
 *   - 逐分量运算（乘、加、插值的 A/B、Clamp…）：原样往上传。
 *   - 其余没专门处理的节点：当逐分量运算对待，并把 bApproximate 标上，只作旁证。
 *
 * 到了采样节点，输出引脚自带通道（RGB/R/G/B/A/RGBA），把 Interest 映射到贴图的真实通道即可。
 *
 * ## 为什么不直接用引擎的「已用贴图」
 *
 * GetUsedTextures 只告诉你用了哪些贴图，不告诉你连到哪个属性、读哪个通道；而且它依赖编译好的着色器地图，
 * -nullrhi 下是空的。这里完全读表达式图，不需要着色器。
 *
 * ## 几个特别处理
 *
 * - 静态开关（StaticSwitch / StaticSwitchParameter）按实例当前的值只走选中的那一支。母材质的 UseORM 关掉时，
 *   ORM 贴图就不会出现在结果里——它确实没被用到。
 * - 材质函数调用：进函数里走，遇到 FunctionInput 再跳回调用处的连线；调用栈是显式维护的。
 *   采样节点的 TextureObject 引脚同理（视差函数就是这么拿到高度图的）。
 * - Lerp 的 Alpha：A、B 上游有贴图时，Alpha 是混合权重（落点 BlendMask，不是数据）；A、B 都没贴图时
 *   Alpha 才是数据本身（粗糙度 = Lerp(Min, Max, ORM.G) 这种重映射）。
 * - 采样节点的 Coordinates 引脚单独走一遍，落点 UVOffset：视差、UV 扭曲用到的高度图才不会漏掉。
 *   UV 偏移本身分不清「视差高度图」和「流向图/噪声」，所以再认两个明确的信号，认到了就把落点升级成 Displacement（确定是高度）：
 *   BumpOffset 节点的 Height 引脚，以及名字带 Parallax / POM / BumpOffset 的材质函数（引擎自带的 ParallaxOcclusionMapping 就是）。
 * - MaterialAttributes：Make / Set / Break / Get / Blend 按「当前追的是哪个属性」选支。
 * - Substrate：BSDF 节点按引脚名（BaseColor / DiffuseAlbedo / Normal / Roughness…）改写落点。没有在 Substrate 工程里实测过。
 *
 * ## 穿不过去的
 *
 * 层堆栈（MaterialAttributeLayers / LayerStack）、运行时虚拟纹理采样、材质函数实例上覆盖的参数值：
 * 结果里记一条警告，不假装解析过。自定义 HLSL 节点能走到它的输入，但它里面怎么用的看不见。
 */

namespace DreamTextureGraph
{
	FString NormalizePinName(const FString& Name)
	{
		FString Out;
		for (const TCHAR C : Name)
		{
			if (C == TEXT(' ') || C == TEXT('_') || C == TEXT('-'))
			{
				continue;
			}
			Out.AppendChar(FChar::ToLower(C));
		}
		return Out;
	}
}

namespace
{
	using namespace DreamTextureGraph;
	namespace Bit = DreamTextureChannelBit;

	constexpr int32 MaxDepth = 400;
	constexpr int32 MaxVisits = 40000;

	EDreamTextureSink SinkForProperty(EMaterialProperty Property)
	{
		switch (Property)
		{
		case MP_BaseColor: return EDreamTextureSink::BaseColor;
		case MP_Metallic: return EDreamTextureSink::Metallic;
		case MP_Specular: return EDreamTextureSink::Specular;
		case MP_Roughness: return EDreamTextureSink::Roughness;
		case MP_Anisotropy: return EDreamTextureSink::Anisotropy;
		case MP_EmissiveColor: return EDreamTextureSink::Emissive;
		case MP_Opacity: return EDreamTextureSink::Opacity;
		case MP_OpacityMask: return EDreamTextureSink::OpacityMask;
		case MP_Normal: return EDreamTextureSink::Normal;
		case MP_Tangent: return EDreamTextureSink::Tangent;
		case MP_AmbientOcclusion: return EDreamTextureSink::AmbientOcclusion;
		case MP_SubsurfaceColor: return EDreamTextureSink::Subsurface;
		case MP_Refraction: return EDreamTextureSink::Refraction;
		case MP_Displacement: return EDreamTextureSink::Displacement;
		case MP_WorldPositionOffset: return EDreamTextureSink::WorldPositionOffset;
		default: return EDreamTextureSink::Other;
		}
	}

	/**
	 * 一个落点起步时下游读几个分量。标量属性读第 0 个（接了向量也只取 R），颜色/法线读前三个。
	 * 这个区别决定了「粗糙度 ← ORM 的 G 输出」读出来是 G 而不是 RGB。
	 */
	uint8 InterestForSink(EDreamTextureSink Sink)
	{
		switch (Sink)
		{
		case EDreamTextureSink::BaseColor:
		case EDreamTextureSink::Emissive:
		case EDreamTextureSink::Normal:
		case EDreamTextureSink::Tangent:
		case EDreamTextureSink::Subsurface:
		case EDreamTextureSink::WorldPositionOffset:
			return Bit::RGB;
		case EDreamTextureSink::UVOffset:
			return Bit::RGBA;
		default:
			return Bit::R;
		}
	}

	/** 引脚名 → 落点。只给 Substrate 的 BSDF 节点用；认不出返回 false。 */
	bool SinkFromPinName(const FString& NormalizedPin, EDreamTextureSink& OutSink)
	{
		struct FEntry { const TCHAR* Pin; EDreamTextureSink Sink; };
		static const FEntry Table[] = {
			{ TEXT("basecolor"), EDreamTextureSink::BaseColor },
			{ TEXT("diffusealbedo"), EDreamTextureSink::BaseColor },
			{ TEXT("diffusecolor"), EDreamTextureSink::BaseColor },
			{ TEXT("albedo"), EDreamTextureSink::BaseColor },
			{ TEXT("f0"), EDreamTextureSink::Specular },
			{ TEXT("specular"), EDreamTextureSink::Specular },
			{ TEXT("metallic"), EDreamTextureSink::Metallic },
			{ TEXT("roughness"), EDreamTextureSink::Roughness },
			{ TEXT("normal"), EDreamTextureSink::Normal },
			{ TEXT("tangent"), EDreamTextureSink::Tangent },
			{ TEXT("emissivecolor"), EDreamTextureSink::Emissive },
			{ TEXT("opacity"), EDreamTextureSink::Opacity },
			{ TEXT("anisotropy"), EDreamTextureSink::Anisotropy },
			{ TEXT("ambientocclusion"), EDreamTextureSink::AmbientOcclusion },
			{ TEXT("subsurfacecolor"), EDreamTextureSink::Subsurface },
		};
		for (const FEntry& Entry : Table)
		{
			if (NormalizedPin == Entry.Pin)
			{
				OutSink = Entry.Sink;
				return true;
			}
		}
		return false;
	}

	/** 连接实际选中的原始通道（RGBA 位）：输入自带的遮罩与所接输出的遮罩取交集，与引擎编译时的规则一致。 */
	uint8 ConnectionChannels(const FExpressionInput& Input)
	{
		auto ToBits = [](int32 R, int32 G, int32 B, int32 A)
		{
			return static_cast<uint8>((R ? Bit::R : 0) | (G ? Bit::G : 0) | (B ? Bit::B : 0) | (A ? Bit::A : 0));
		};

		uint8 Out = Bit::RGBA;
		bool bMasked = false;
		if (Input.Mask)
		{
			Out &= ToBits(Input.MaskR, Input.MaskG, Input.MaskB, Input.MaskA);
			bMasked = true;
		}
		if (Input.Expression != nullptr)
		{
			const TArray<FExpressionOutput>& Outputs = Input.Expression->GetOutputs();
			if (Outputs.IsValidIndex(Input.OutputIndex) && Outputs[Input.OutputIndex].Mask)
			{
				const FExpressionOutput& Output = Outputs[Input.OutputIndex];
				Out &= ToBits(Output.MaskR, Output.MaskG, Output.MaskB, Output.MaskA);
				bMasked = true;
			}
		}
		return bMasked ? Out : static_cast<uint8>(0);
	}

	/**
	 * 把「下游读第 k 个分量」映射成贴图的真实通道。
	 * Channels 是连接选中的原始通道（0 = 没有遮罩信息，分量就是原样的）。
	 */
	uint8 MapInterestToChannels(uint8 Channels, uint8 Interest)
	{
		if (Channels == 0)
		{
			return Interest;
		}
		uint8 Out = 0;
		int32 Component = 0;
		for (int32 Channel = 0; Channel < 4; ++Channel)
		{
			if (Channels & (1 << Channel))
			{
				if (Interest & (1 << Component))
				{
					Out |= static_cast<uint8>(1 << Channel);
				}
				++Component;
			}
		}
		if (Out == 0)
		{
			// 下游读的分量比这个输出有的还多/不在其中：取第一个，贴图总得有个通道被读。
			for (int32 Channel = 0; Channel < 4; ++Channel)
			{
				if (Channels & (1 << Channel))
				{
					return static_cast<uint8>(1 << Channel);
				}
			}
		}
		return Out;
	}

	/** BreakMaterialAttributes 的输出下标 → 材质属性。顺序抄自引擎的 PropertyToIOIndexMap（那张表没导出）。 */
	EMaterialProperty PropertyForBreakOutput(int32 OutputIndex)
	{
		static const EMaterialProperty Table[] = {
			MP_BaseColor, MP_Metallic, MP_Specular, MP_Roughness, MP_Anisotropy, MP_EmissiveColor, MP_Opacity, MP_OpacityMask,
			MP_Normal, MP_Tangent, MP_WorldPositionOffset, MP_SubsurfaceColor, MP_CustomData0, MP_CustomData1, MP_AmbientOcclusion,
			MP_Refraction, MP_CustomizedUVs0, MP_CustomizedUVs1, MP_CustomizedUVs2, MP_CustomizedUVs3, MP_CustomizedUVs4,
			MP_CustomizedUVs5, MP_CustomizedUVs6, MP_CustomizedUVs7, MP_PixelDepthOffset, MP_ShadingModel, MP_Displacement,
		};
		return (OutputIndex >= 0 && OutputIndex < static_cast<int32>(UE_ARRAY_COUNT(Table))) ? Table[OutputIndex] : MP_MAX;
	}

	/**
	 * MakeMaterialAttributes 上对应某个属性的输入。引擎自带的 GetExpressionInput 没导出（链接不过），
	 * 这里直接按公开成员对。
	 */
	const FExpressionInput* MakeInputForProperty(const UMaterialExpressionMakeMaterialAttributes& Make, EMaterialProperty Property)
	{
		switch (Property)
		{
		case MP_BaseColor: return &Make.BaseColor;
		case MP_Metallic: return &Make.Metallic;
		case MP_Specular: return &Make.Specular;
		case MP_Roughness: return &Make.Roughness;
		case MP_Anisotropy: return &Make.Anisotropy;
		case MP_EmissiveColor: return &Make.EmissiveColor;
		case MP_Opacity: return &Make.Opacity;
		case MP_OpacityMask: return &Make.OpacityMask;
		case MP_Normal: return &Make.Normal;
		case MP_Tangent: return &Make.Tangent;
		case MP_WorldPositionOffset: return &Make.WorldPositionOffset;
		case MP_SubsurfaceColor: return &Make.SubsurfaceColor;
		case MP_CustomData0: return &Make.ClearCoat;
		case MP_CustomData1: return &Make.ClearCoatRoughness;
		case MP_AmbientOcclusion: return &Make.AmbientOcclusion;
		case MP_Refraction: return &Make.Refraction;
		case MP_PixelDepthOffset: return &Make.PixelDepthOffset;
		case MP_ShadingModel: return &Make.ShadingModel;
		case MP_Displacement: return &Make.Displacement;
		default:
			if (Property >= MP_CustomizedUVs0 && Property <= MP_CustomizedUVs7)
			{
				return &Make.CustomizedUVs[Property - MP_CustomizedUVs0];
			}
			return nullptr;
		}
	}

	struct FVisit
	{
		const UMaterialExpression* Expression = nullptr;
		int32 OutputIndex = 0;
		uint8 Interest = 0;
		uint8 Channels = 0;
		uint8 Sink = 0;
		uint8 bApproximate = 0;
		int32 Attribute = 0;
		uint32 StackHash = 0;

		bool operator==(const FVisit& Other) const
		{
			return Expression == Other.Expression && OutputIndex == Other.OutputIndex && Interest == Other.Interest && Channels == Other.Channels
				&& Sink == Other.Sink && bApproximate == Other.bApproximate && Attribute == Other.Attribute && StackHash == Other.StackHash;
		}

		friend uint32 GetTypeHash(const FVisit& Key)
		{
			uint32 Hash = GetTypeHash(Key.Expression);
			Hash = HashCombine(Hash, GetTypeHash(Key.OutputIndex));
			Hash = HashCombine(Hash, static_cast<uint32>(Key.Interest) | (static_cast<uint32>(Key.Channels) << 8) | (static_cast<uint32>(Key.Sink) << 16) | (static_cast<uint32>(Key.bApproximate) << 24));
			Hash = HashCombine(Hash, GetTypeHash(Key.Attribute));
			return HashCombine(Hash, Key.StackHash);
		}
	};

	class FWalker
	{
	public:
		explicit FWalker(UMaterialInterface* InTop)
			: Top(InTop)
		{
		}

		FWalkResult Run();

	private:
		struct FState
		{
			uint8 Interest = Bit::R;
			EDreamTextureSink Sink = EDreamTextureSink::Other;

			/** 正沿着 MaterialAttributes 结构往上游走时，追的是哪个属性；MP_MAX = 追的是普通值。 */
			EMaterialProperty Attribute = MP_MAX;

			bool bApproximate = false;
		};

		/** 一个贴图节点代入参数之后的样子。 */
		struct FResolved
		{
			UTexture* Texture = nullptr;
			FName Parameter;
			bool bOverriddenBySelected = false;
			EMaterialSamplerType SamplerType = SAMPLERTYPE_Color;
			bool bValid = false;
		};

		void StartProperty(EMaterialProperty Property, bool bFromAttributes);

		void WalkInput(const FExpressionInput& Input, const FState& State);
		void WalkExpression(UMaterialExpression* Expression, int32 OutputIndex, uint8 Channels, const FState& State);
		void WalkAllInputs(UMaterialExpression* Expression, const FState& State);
		void WalkSample(UMaterialExpressionTextureSample* Sample, uint8 Channels, const FState& State);
		void WalkFunctionCall(UMaterialExpressionMaterialFunctionCall* Call, int32 OutputIndex, const FState& State);
		void WalkFunctionInput(UMaterialExpressionFunctionInput* FunctionInput, const FState& State);
		bool WalkMaterialAttributes(UMaterialExpression* Expression, int32 OutputIndex, const FState& State);
		bool WalkSubstrate(UMaterialExpression* Expression, const FState& State);

		FResolved ResolveTextureNode(const UMaterialExpressionTextureBase* Node) const;
		FResolved ResolveTextureObject(const FExpressionInput& Pin);
		TOptional<bool> ResolveStaticBool(const FExpressionInput& Input);
		bool StaticParameterValue(const FName& Name, bool bDefault) const;
		bool HasTextureUpstream(const FExpressionInput& Input, TSet<const UMaterialExpression*>& Seen, int32 Depth);

		void AddUse(const FResolved& Resolved, uint8 Channels, const FState& State);
		void Warn(const FString& Text);

		//~ 调用栈
		void PushCall(const UMaterialExpressionMaterialFunctionCall* Call);
		const UMaterialExpressionMaterialFunctionCall* PopCall();
		static UMaterialExpressionFunctionOutput* FindOutputNode(const UMaterialExpressionMaterialFunctionCall* Call, int32 OutputIndex);
		static int32 FindInputIndex(const UMaterialExpressionMaterialFunctionCall* Call, const UMaterialExpressionFunctionInput* Node);

		UMaterialInterface* Top = nullptr;
		UMaterial* Base = nullptr;

		TArray<const UMaterialExpressionMaterialFunctionCall*> Stack;
		TArray<uint32> StackHashes;
		uint32 StackHash = 0;

		TSet<FVisit> Visited;
		int32 Depth = 0;
		bool bTruncated = false;

		FWalkResult Result;
		TSet<FString> WarningSet;
	};

	void FWalker::Warn(const FString& Text)
	{
		if (!WarningSet.Contains(Text))
		{
			WarningSet.Add(Text);
			Result.Warnings.Add(Text);
		}
	}

	void FWalker::PushCall(const UMaterialExpressionMaterialFunctionCall* Call)
	{
		Stack.Add(Call);
		StackHashes.Add(StackHash);
		StackHash = HashCombine(StackHash, GetTypeHash(Call));
	}

	const UMaterialExpressionMaterialFunctionCall* FWalker::PopCall()
	{
		const UMaterialExpressionMaterialFunctionCall* Call = Stack.Pop();
		StackHash = StackHashes.Pop();
		return Call;
	}

	UMaterialExpressionFunctionOutput* FWalker::FindOutputNode(const UMaterialExpressionMaterialFunctionCall* Call, int32 OutputIndex)
	{
		if (!Call->FunctionOutputs.IsValidIndex(OutputIndex))
		{
			return nullptr;
		}
		const FFunctionExpressionOutput& Entry = Call->FunctionOutputs[OutputIndex];
		if (Entry.ExpressionOutput != nullptr)
		{
			return Entry.ExpressionOutput;
		}

		// 瞬态指针没填（节点刚加载、还没走过 UpdateFromFunctionResource）：按 Id 回函数里找。
		if (Call->MaterialFunction != nullptr)
		{
			for (UMaterialExpression* Expression : Call->MaterialFunction->GetExpressions())
			{
				if (UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(Expression))
				{
					if (Output->Id == Entry.ExpressionOutputId)
					{
						return Output;
					}
				}
			}
		}
		return nullptr;
	}

	int32 FWalker::FindInputIndex(const UMaterialExpressionMaterialFunctionCall* Call, const UMaterialExpressionFunctionInput* Node)
	{
		for (int32 Index = 0; Index < Call->FunctionInputs.Num(); ++Index)
		{
			const FFunctionExpressionInput& Entry = Call->FunctionInputs[Index];
			if (Entry.ExpressionInput == Node || (Entry.ExpressionInputId.IsValid() && Entry.ExpressionInputId == Node->Id))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	bool FWalker::StaticParameterValue(const FName& Name, bool bDefault) const
	{
		bool bValue = bDefault;
		FGuid Guid;
		if (Top->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(Name), bValue, Guid))
		{
			return bValue;
		}
		return bDefault;
	}

	TOptional<bool> FWalker::ResolveStaticBool(const FExpressionInput& Input)
	{
		const FExpressionInput Traced = Input.GetTracedInput();
		UMaterialExpression* Expression = Traced.Expression;
		if (Expression == nullptr)
		{
			return TOptional<bool>();
		}

		// StaticSwitchParameter 也是 StaticBoolParameter 的子类，但它输出的是分支里的值，不是布尔，先排除。
		if (!Expression->IsA<UMaterialExpressionStaticSwitchParameter>())
		{
			if (const UMaterialExpressionStaticBoolParameter* Parameter = Cast<UMaterialExpressionStaticBoolParameter>(Expression))
			{
				return StaticParameterValue(Parameter->ParameterName, Parameter->DefaultValue != 0);
			}
		}
		if (const UMaterialExpressionStaticBool* Constant = Cast<UMaterialExpressionStaticBool>(Expression))
		{
			return Constant->Value != 0;
		}
		if (UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(Expression))
		{
			// 函数里的静态布尔输入：到调用处去看它接的是什么。
			if (Stack.Num() > 0)
			{
				const UMaterialExpressionMaterialFunctionCall* Call = Stack.Last();
				const int32 Index = FindInputIndex(Call, FunctionInput);
				if (Index != INDEX_NONE && Call->FunctionInputs[Index].Input.GetTracedInput().Expression != nullptr)
				{
					PopCall();
					const TOptional<bool> Value = ResolveStaticBool(Call->FunctionInputs[Index].Input);
					PushCall(Call);
					return Value;
				}
			}
			if (FunctionInput->bUsePreviewValueAsDefault)
			{
				return ResolveStaticBool(FunctionInput->Preview);
			}
		}
		return TOptional<bool>();
	}

	FWalker::FResolved FWalker::ResolveTextureNode(const UMaterialExpressionTextureBase* Node) const
	{
		FResolved Out;
		Out.bValid = true;
		Out.SamplerType = Node->SamplerType;

		const UMaterialExpressionTextureSampleParameter* Parameter = Cast<UMaterialExpressionTextureSampleParameter>(Node);
		if (Parameter == nullptr)
		{
			Out.Texture = Node->Texture;
			return Out;
		}

		Out.Parameter = Parameter->ParameterName;

		UTexture* Value = nullptr;
		if (Top->GetTextureParameterValue(FHashedMaterialParameterInfo(Parameter->ParameterName), Value) && Value != nullptr)
		{
			Out.Texture = Value;
		}
		else
		{
			// 材质的参数缓存里没有这个名字（图刚改过还没重编译），退回节点上写的默认贴图。
			Out.Texture = Parameter->Texture;
		}

		if (const UMaterialInstance* Instance = Cast<UMaterialInstance>(Top))
		{
			for (const FTextureParameterValue& Entry : Instance->TextureParameterValues)
			{
				if (Entry.ParameterInfo.Name == Parameter->ParameterName && Entry.ParameterInfo.Association == GlobalParameter && Entry.ParameterValue != nullptr)
				{
					Out.bOverriddenBySelected = true;
					break;
				}
			}
		}
		return Out;
	}

	FWalker::FResolved FWalker::ResolveTextureObject(const FExpressionInput& Pin)
	{
		FResolved Out;
		const FExpressionInput Traced = Pin.GetTracedInput();
		UMaterialExpression* Expression = Traced.Expression;
		if (Expression == nullptr)
		{
			return Out;
		}

		if (const UMaterialExpressionTextureObjectParameter* ObjectParameter = Cast<UMaterialExpressionTextureObjectParameter>(Expression))
		{
			return ResolveTextureNode(ObjectParameter);
		}
		if (const UMaterialExpressionTextureObject* Object = Cast<UMaterialExpressionTextureObject>(Expression))
		{
			return ResolveTextureNode(Object);
		}
		if (UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(Expression))
		{
			if (Stack.Num() > 0)
			{
				const UMaterialExpressionMaterialFunctionCall* Call = Stack.Last();
				const int32 Index = FindInputIndex(Call, FunctionInput);
				if (Index != INDEX_NONE && Call->FunctionInputs[Index].Input.GetTracedInput().Expression != nullptr)
				{
					PopCall();
					const FResolved Resolved = ResolveTextureObject(Call->FunctionInputs[Index].Input);
					PushCall(Call);
					return Resolved;
				}
			}
			if (FunctionInput->bUsePreviewValueAsDefault)
			{
				return ResolveTextureObject(FunctionInput->Preview);
			}
		}
		return Out;
	}

	void FWalker::AddUse(const FResolved& Resolved, uint8 Channels, const FState& State)
	{
		if (!Resolved.bValid)
		{
			return;
		}
		if (Resolved.Texture == nullptr)
		{
			if (!Resolved.Parameter.IsNone())
			{
				Warn(FText::Format(NSLOCTEXT("DreamTextureMaterialGraph", "ParameterWithoutTexture", "The texture parameter '{0}' has no texture assigned, so its role in the material cannot be recorded."), FText::FromString(Resolved.Parameter.ToString())).ToString());
			}
			return;
		}

		FRawUse Use;
		Use.Texture = Resolved.Texture;
		Use.Parameter = Resolved.Parameter;
		Use.bOverriddenBySelected = Resolved.bOverriddenBySelected;
		Use.SamplerType = Resolved.SamplerType;
		Use.Use.Sink = State.Sink;
		Use.Use.Channels = Channels;
		Use.Use.Parameter = Resolved.Parameter;
		Use.Use.bApproximate = State.bApproximate;

		for (const FRawUse& Existing : Result.Uses)
		{
			if (Existing.Texture == Use.Texture && Existing.Use.Sink == Use.Use.Sink && Existing.Use.Channels == Use.Use.Channels
				&& Existing.Use.Parameter == Use.Use.Parameter && Existing.Use.bApproximate == Use.Use.bApproximate)
			{
				return;
			}
		}
		Result.Uses.Add(Use);
	}

	bool FWalker::HasTextureUpstream(const FExpressionInput& Input, TSet<const UMaterialExpression*>& Seen, int32 InDepth)
	{
		const FExpressionInput Traced = Input.GetTracedInput();
		UMaterialExpression* Expression = Traced.Expression;
		if (Expression == nullptr || InDepth > 64 || Seen.Contains(Expression))
		{
			return false;
		}
		Seen.Add(Expression);

		if (Expression->IsA<UMaterialExpressionTextureBase>())
		{
			return true;
		}
		for (FExpressionInputIterator It{ Expression }; It; ++It)
		{
			if (HasTextureUpstream(*It.Input, Seen, InDepth + 1))
			{
				return true;
			}
		}
		return false;
	}

	//////////////////////////////////////////////////////////////////////////
	// 入口

	void FWalker::StartProperty(EMaterialProperty Property, bool bFromAttributes)
	{
		FExpressionInput* Input = bFromAttributes ? Base->GetExpressionInputForProperty(MP_MaterialAttributes) : Base->GetExpressionInputForProperty(Property);
		if (Input == nullptr || !Input->GetTracedInput().Expression)
		{
			return;
		}

		FState State;
		State.Sink = SinkForProperty(Property);
		State.Interest = InterestForSink(State.Sink);
		State.Attribute = bFromAttributes ? Property : MP_MAX;
		WalkInput(*Input, State);
	}

	FWalkResult FWalker::Run()
	{
		Base = Top != nullptr ? Top->GetMaterial() : nullptr;
		if (Base == nullptr)
		{
			Warn(NSLOCTEXT("DreamTextureMaterialGraph", "NoRootMaterial", "Could not find the root material (is the parent chain broken?), so the material cannot be analyzed.").ToString());
			return MoveTemp(Result);
		}

		static const EMaterialProperty Properties[] = {
			MP_BaseColor, MP_Metallic, MP_Specular, MP_Roughness, MP_Anisotropy, MP_EmissiveColor, MP_Opacity, MP_OpacityMask,
			MP_Normal, MP_Tangent, MP_AmbientOcclusion, MP_SubsurfaceColor, MP_Refraction, MP_Displacement, MP_WorldPositionOffset,
			MP_CustomData0, MP_CustomData1, MP_PixelDepthOffset,
			MP_CustomizedUVs0, MP_CustomizedUVs1, MP_CustomizedUVs2, MP_CustomizedUVs3, MP_CustomizedUVs4, MP_CustomizedUVs5, MP_CustomizedUVs6, MP_CustomizedUVs7,
		};

		if (Base->bUseMaterialAttributes)
		{
			for (const EMaterialProperty Property : Properties)
			{
				StartProperty(Property, /*bFromAttributes*/ true);
			}
		}
		else
		{
			for (const EMaterialProperty Property : Properties)
			{
				StartProperty(Property, /*bFromAttributes*/ false);
			}
		}

		// Substrate：整个材质挂在 FrontMaterial 上，落点由 BSDF 节点按引脚名改写。
		if (FExpressionInput* Front = Base->GetExpressionInputForProperty(MP_FrontMaterial))
		{
			if (Front->GetTracedInput().Expression != nullptr)
			{
				FState State;
				State.Sink = EDreamTextureSink::Other;
				State.Interest = Bit::RGB;
				WalkInput(*Front, State);
			}
		}

		if (bTruncated)
		{
			Warn(NSLOCTEXT("DreamTextureMaterialGraph", "GraphTruncated", "The material graph is too large or contains a cycle, so the traversal was cut short and the result may be incomplete.").ToString());
		}
		return MoveTemp(Result);
	}

	//////////////////////////////////////////////////////////////////////////
	// 遍历

	void FWalker::WalkInput(const FExpressionInput& Input, const FState& State)
	{
		const FExpressionInput Traced = Input.GetTracedInput();
		if (Traced.Expression == nullptr)
		{
			return;
		}
		WalkExpression(Traced.Expression, Traced.OutputIndex, ConnectionChannels(Traced), State);
	}

	void FWalker::WalkAllInputs(UMaterialExpression* Expression, const FState& State)
	{
		for (FExpressionInputIterator It{ Expression }; It; ++It)
		{
			WalkInput(*It.Input, State);
		}
	}

	void FWalker::WalkExpression(UMaterialExpression* Expression, int32 OutputIndex, uint8 Channels, const FState& State)
	{
		FVisit Key;
		Key.Expression = Expression;
		Key.OutputIndex = OutputIndex;
		Key.Interest = State.Interest;
		Key.Channels = Channels;
		Key.Sink = static_cast<uint8>(State.Sink);
		Key.bApproximate = State.bApproximate ? 1 : 0;
		Key.Attribute = static_cast<int32>(State.Attribute);
		Key.StackHash = StackHash;
		if (Visited.Contains(Key))
		{
			return;
		}
		if (Depth >= MaxDepth || Visited.Num() >= MaxVisits)
		{
			bTruncated = true;
			return;
		}
		Visited.Add(Key);

		struct FDepthScope
		{
			int32& Value;
			explicit FDepthScope(int32& InValue) : Value(InValue) { ++Value; }
			~FDepthScope() { --Value; }
		} DepthScope(Depth);

		//~ 贴图节点。TextureObjectParameter 继承自 TextureSampleParameter，必须先判它。
		if (const UMaterialExpressionTextureObjectParameter* ObjectParameter = Cast<UMaterialExpressionTextureObjectParameter>(Expression))
		{
			// 走到贴图对象本身（没经过采样节点）：被谁用、读哪几个通道看不见，记下来当旁证。
			FState Approx = State;
			Approx.bApproximate = true;
			AddUse(ResolveTextureNode(ObjectParameter), Bit::RGBA, Approx);
			return;
		}
		if (const UMaterialExpressionTextureObject* Object = Cast<UMaterialExpressionTextureObject>(Expression))
		{
			FState Approx = State;
			Approx.bApproximate = true;
			AddUse(ResolveTextureNode(Object), Bit::RGBA, Approx);
			return;
		}
		if (UMaterialExpressionTextureSample* Sample = Cast<UMaterialExpressionTextureSample>(Expression))
		{
			WalkSample(Sample, Channels, State);
			return;
		}
		if (Expression->IsA<UMaterialExpressionTextureProperty>())
		{
			// 只取贴图的尺寸，不读像素。
			return;
		}
		if (Expression->IsA<UMaterialExpressionRuntimeVirtualTextureSample>())
		{
			Warn(NSLOCTEXT("DreamTextureMaterialGraph", "RuntimeVirtualTexture", "The material samples a runtime virtual texture (RVT), which does not read a texture asset in the project, so it was not analyzed.").ToString());
			return;
		}

		//~ 改写分量的节点
		if (const UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(Expression))
		{
			TArray<int32, TInlineAllocator<4>> Enabled;
			if (Mask->R) { Enabled.Add(0); }
			if (Mask->G) { Enabled.Add(1); }
			if (Mask->B) { Enabled.Add(2); }
			if (Mask->A) { Enabled.Add(3); }

			FState Next = State;
			if (Enabled.Num() > 0)
			{
				uint8 NewInterest = 0;
				for (int32 Component = 0; Component < Enabled.Num(); ++Component)
				{
					if (State.Interest & (1 << Component))
					{
						NewInterest |= static_cast<uint8>(1 << Enabled[Component]);
					}
				}
				if (NewInterest == 0)
				{
					NewInterest = static_cast<uint8>(1 << Enabled[0]);
				}
				Next.Interest = NewInterest;
			}
			WalkInput(Mask->Input, Next);
			return;
		}

		//~ 插值
		if (const UMaterialExpressionLinearInterpolate* Lerp = Cast<UMaterialExpressionLinearInterpolate>(Expression))
		{
			WalkInput(Lerp->A, State);
			WalkInput(Lerp->B, State);
			if (Lerp->Alpha.GetTracedInput().Expression != nullptr)
			{
				TSet<const UMaterialExpression*> Seen;
				const bool bAIsTextured = HasTextureUpstream(Lerp->A, Seen, 0);
				const bool bBIsTextured = HasTextureUpstream(Lerp->B, Seen, 0);
				if (bAIsTextured || bBIsTextured)
				{
					FState Mask = State;
					Mask.Sink = EDreamTextureSink::BlendMask;
					Mask.Interest = Bit::R;
					WalkInput(Lerp->Alpha, Mask);
				}
				else
				{
					WalkInput(Lerp->Alpha, State);
				}
			}
			return;
		}

		//~ 静态开关：只走当前生效的那一支
		if (const UMaterialExpressionStaticSwitchParameter* SwitchParameter = Cast<UMaterialExpressionStaticSwitchParameter>(Expression))
		{
			const bool bValue = StaticParameterValue(SwitchParameter->ParameterName, SwitchParameter->DefaultValue != 0);
			WalkInput(bValue ? SwitchParameter->A : SwitchParameter->B, State);
			return;
		}
		if (const UMaterialExpressionStaticSwitch* StaticSwitch = Cast<UMaterialExpressionStaticSwitch>(Expression))
		{
			TOptional<bool> Value;
			if (StaticSwitch->Value.GetTracedInput().Expression != nullptr)
			{
				Value = ResolveStaticBool(StaticSwitch->Value);
			}
			else
			{
				Value = StaticSwitch->DefaultValue != 0;
			}
			if (Value.IsSet())
			{
				WalkInput(Value.GetValue() ? StaticSwitch->A : StaticSwitch->B, State);
			}
			else
			{
				// 开关值解析不出来（来自没处理的节点）：两支都走，宁可多报也别漏。
				WalkInput(StaticSwitch->A, State);
				WalkInput(StaticSwitch->B, State);
			}
			return;
		}

		//~ 材质函数
		if (UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
		{
			WalkFunctionCall(Call, OutputIndex, State);
			return;
		}
		if (UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(Expression))
		{
			WalkFunctionInput(FunctionInput, State);
			return;
		}
		if (const UMaterialExpressionFunctionOutput* FunctionOutput = Cast<UMaterialExpressionFunctionOutput>(Expression))
		{
			WalkInput(FunctionOutput->A, State);
			return;
		}

		//~ 凹凸偏移：Height 引脚读的是高度图，坐标引脚照常往下走。
		if (const UMaterialExpressionBumpOffset* BumpOffset = Cast<UMaterialExpressionBumpOffset>(Expression))
		{
			FState Height = State;
			Height.Sink = EDreamTextureSink::Displacement;
			Height.Interest = Bit::R;
			WalkInput(BumpOffset->Height, Height);
			WalkInput(BumpOffset->Coordinate, State);
			WalkInput(BumpOffset->HeightRatioInput, State);
			return;
		}

		//~ 层堆栈：不解析
		if (Expression->IsA<UMaterialExpressionMaterialAttributeLayers>())
		{
			Warn(NSLOCTEXT("DreamTextureMaterialGraph", "MaterialLayers", "The material uses material attribute layers (a layer stack); the textures inside the layers were not analyzed.").ToString());
			return;
		}

		if (WalkMaterialAttributes(Expression, OutputIndex, State))
		{
			return;
		}
		if (WalkSubstrate(Expression, State))
		{
			return;
		}

		//~ 其余：当逐分量运算，往每个输入里走。
		// 知道的几个会改变分量数的节点（点乘、去饱和…）要把 Interest 放宽，否则只追第 0 个分量会漏掉后面的通道。
		FState Next = State;
		const FString ClassName = Expression->GetClass()->GetName();
		if (ClassName == TEXT("MaterialExpressionDesaturation") || ClassName == TEXT("MaterialExpressionDotProduct") || ClassName == TEXT("MaterialExpressionLength")
			|| ClassName == TEXT("MaterialExpressionDistance") || ClassName == TEXT("MaterialExpressionCrossProduct") || ClassName == TEXT("MaterialExpressionRgbToHsv")
			|| ClassName == TEXT("MaterialExpressionHsvToRgb") || ClassName == TEXT("MaterialExpressionDeriveNormalZ") || ClassName == TEXT("MaterialExpressionTransform"))
		{
			Next.Interest = Bit::RGBA;
		}
		else if (ClassName == TEXT("MaterialExpressionCustom"))
		{
			// 自定义 HLSL：里面怎么用输入看不见。
			Next.bApproximate = true;
			Next.Interest = Bit::RGBA;
		}
		WalkAllInputs(Expression, Next);
	}

	void FWalker::WalkSample(UMaterialExpressionTextureSample* Sample, uint8 Channels, const FState& State)
	{
		// TextureObject 引脚连了就以它为准（函数里的贴图参数是这么传进来的），否则用节点自己的贴图/参数。
		FResolved Resolved;
		if (Sample->TextureObject.GetTracedInput().Expression != nullptr)
		{
			Resolved = ResolveTextureObject(Sample->TextureObject);
		}
		else
		{
			Resolved = ResolveTextureNode(Sample);
		}
		// 采样器类型以采样节点为准（TextureObject 节点上那个对采样没作用）。
		Resolved.SamplerType = Sample->SamplerType;

		AddUse(Resolved, MapInterestToChannels(Channels, State.Interest), State);

		if (Sample->Coordinates.GetTracedInput().Expression != nullptr)
		{
			FState Coordinates;
			Coordinates.Sink = EDreamTextureSink::UVOffset;
			Coordinates.Interest = Bit::RGBA;
			Coordinates.bApproximate = State.bApproximate;
			WalkInput(Sample->Coordinates, Coordinates);
		}
	}

	void FWalker::WalkFunctionCall(UMaterialExpressionMaterialFunctionCall* Call, int32 OutputIndex, const FState& State)
	{
		if (Call->MaterialFunction == nullptr)
		{
			WalkAllInputs(Call, State);
			return;
		}

		const UMaterialExpressionFunctionOutput* OutputNode = FindOutputNode(Call, OutputIndex);
		if (OutputNode == nullptr)
		{
			Warn(FText::Format(NSLOCTEXT("DreamTextureMaterialGraph", "FunctionOutputMismatch", "The outputs of material function '{0}' do not line up, so the textures inside the function were not analyzed."), FText::FromString(Call->MaterialFunction->GetName())).ToString());
			WalkAllInputs(Call, State);
			return;
		}

		// 函数的结果正喂给采样坐标，而且函数名一看就是视差/凹凸偏移：里面读的贴图是高度图，不是任意的 UV 扭曲。
		FState Inner = State;
		if (State.Sink == EDreamTextureSink::UVOffset)
		{
			const FString FunctionName = Call->MaterialFunction->GetName();
			if (FunctionName.Contains(TEXT("Parallax"), ESearchCase::IgnoreCase) || FunctionName.Contains(TEXT("BumpOffset"), ESearchCase::IgnoreCase)
				|| FunctionName.Contains(TEXT("POM"), ESearchCase::CaseSensitive))
			{
				Inner.Sink = EDreamTextureSink::Displacement;
				Inner.Interest = Bit::RGBA;
			}
		}

		PushCall(Call);
		WalkInput(OutputNode->A, Inner);
		PopCall();
	}

	void FWalker::WalkFunctionInput(UMaterialExpressionFunctionInput* FunctionInput, const FState& State)
	{
		if (Stack.Num() > 0)
		{
			const UMaterialExpressionMaterialFunctionCall* Call = Stack.Last();
			const int32 Index = FindInputIndex(Call, FunctionInput);
			if (Index != INDEX_NONE && Call->FunctionInputs[Index].Input.GetTracedInput().Expression != nullptr)
			{
				// 跳回调用处：那条连线属于外一层的图，调用栈要先退一格。
				PopCall();
				WalkInput(Call->FunctionInputs[Index].Input, State);
				PushCall(Call);
				return;
			}
		}
		if (FunctionInput->bUsePreviewValueAsDefault)
		{
			WalkInput(FunctionInput->Preview, State);
		}
	}

	bool FWalker::WalkMaterialAttributes(UMaterialExpression* Expression, int32 OutputIndex, const FState& State)
	{
		if (const UMaterialExpressionMakeMaterialAttributes* Make = Cast<UMaterialExpressionMakeMaterialAttributes>(Expression))
		{
			if (State.Attribute == MP_MAX)
			{
				WalkAllInputs(Expression, State);
				return true;
			}
			if (const FExpressionInput* Input = MakeInputForProperty(*Make, State.Attribute))
			{
				FState Next = State;
				Next.Attribute = MP_MAX;
				WalkInput(*Input, Next);
			}
			return true;
		}

		if (const UMaterialExpressionSetMaterialAttributes* Set = Cast<UMaterialExpressionSetMaterialAttributes>(Expression))
		{
			if (State.Attribute == MP_MAX || Set->Inputs.Num() == 0)
			{
				WalkAllInputs(Expression, State);
				return true;
			}
			bool bOverridden = false;
			for (int32 Index = 0; Index < Set->AttributeSetTypes.Num(); ++Index)
			{
				if (FMaterialAttributeDefinitionMap::GetProperty(Set->AttributeSetTypes[Index]) == State.Attribute && Set->Inputs.IsValidIndex(Index + 1))
				{
					// 第 0 个输入是原来的结构，其后依次是各个被改写的属性。
					if (Set->Inputs[Index + 1].GetTracedInput().Expression != nullptr)
					{
						FState Next = State;
						Next.Attribute = MP_MAX;
						WalkInput(Set->Inputs[Index + 1], Next);
						bOverridden = true;
					}
					break;
				}
			}
			if (!bOverridden)
			{
				WalkInput(Set->Inputs[0], State);
			}
			return true;
		}

		if (const UMaterialExpressionBreakMaterialAttributes* Break = Cast<UMaterialExpressionBreakMaterialAttributes>(Expression))
		{
			const EMaterialProperty Property = PropertyForBreakOutput(OutputIndex);
			FState Next = State;
			Next.Attribute = Property;
			WalkInput(Break->MaterialAttributes, Next);
			return true;
		}

		if (const UMaterialExpressionGetMaterialAttributes* Get = Cast<UMaterialExpressionGetMaterialAttributes>(Expression))
		{
			FState Next = State;
			if (OutputIndex > 0 && Get->AttributeGetTypes.IsValidIndex(OutputIndex - 1))
			{
				Next.Attribute = FMaterialAttributeDefinitionMap::GetProperty(Get->AttributeGetTypes[OutputIndex - 1]);
			}
			WalkInput(Get->MaterialAttributes, Next);
			return true;
		}

		if (const UMaterialExpressionBlendMaterialAttributes* Blend = Cast<UMaterialExpressionBlendMaterialAttributes>(Expression))
		{
			WalkInput(Blend->A, State);
			WalkInput(Blend->B, State);
			if (Blend->Alpha.GetTracedInput().Expression != nullptr)
			{
				FState Mask = State;
				Mask.Sink = EDreamTextureSink::BlendMask;
				Mask.Interest = Bit::R;
				Mask.Attribute = MP_MAX;
				WalkInput(Blend->Alpha, Mask);
			}
			return true;
		}
		return false;
	}

	bool FWalker::WalkSubstrate(UMaterialExpression* Expression, const FState& State)
	{
		const bool bIsBSDF = Expression->IsA<UMaterialExpressionSubstrateBSDF>();
		const bool bIsMetalness = Expression->IsA<UMaterialExpressionSubstrateMetalnessToDiffuseAlbedoF0>();
		if (!bIsBSDF && !bIsMetalness)
		{
			return false;
		}

		for (FExpressionInputIterator It{ Expression }; It; ++It)
		{
			FState Next = State;
			EDreamTextureSink Sink;
			if (SinkFromPinName(NormalizePinName(Expression->GetInputName(It.Index).ToString()), Sink))
			{
				Next.Sink = Sink;
				Next.Interest = InterestForSink(Sink);
				// Substrate 的 F0 是颜色，传统管线的 Specular 才是标量。
				if (Sink == EDreamTextureSink::Specular)
				{
					Next.Interest = Bit::RGB;
				}
			}
			WalkInput(*It.Input, Next);
		}
		return true;
	}
}

namespace DreamTextureGraph
{
	FWalkResult Walk(UMaterialInterface* Material)
	{
		FWalker Walker(Material);
		return Walker.Run();
	}
}
