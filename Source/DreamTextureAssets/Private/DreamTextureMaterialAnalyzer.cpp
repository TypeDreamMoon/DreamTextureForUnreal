// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureMaterialAnalysis.h"

#include "Components/MeshComponent.h"
#include "DreamTextureImporter.h"
#include "DreamTextureManifest.h"
#include "DreamTextureMaterialGraph.h"
#include "DreamTextureTextureExporter.h"
#include "Engine/Texture.h"
#include "GameFramework/Actor.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/Package.h"

/*
 * 从材质图的使用记录（DreamTextureMaterialGraph）推出每张贴图是什么。
 *
 * 结论的依据按「先准后猜」排：用户指认 > 来源记录 > 材质图 > 参数名 > 资产名 > 采样器类型。
 * 每个依据各自产出一个候选，取第一个有效的当结论；落选的候选如果与结论冲突，写进 Notes，
 * 这样界面上能看到「参数叫 Normal，但材质把它连到了 BaseColor」这种值得人看一眼的矛盾。
 *
 * 只有一处例外：材质图给出的打包图排布不全时（比如只连了 G 通道），允许用名字里的排布词
 * （ORM / MRA…）补全——前提是名字的排布与图里已经确定的通道不矛盾。
 */

namespace
{
	namespace Bit = DreamTextureChannelBit;

	/** 一个依据的结论。 */
	struct FCandidate
	{
		bool bValid = false;
		EDreamTextureEvidence Evidence = EDreamTextureEvidence::None;
		EDreamTextureRole Role = EDreamTextureRole::Unknown;
		FDreamTextureChannelLayout Layout;
		FString Detail;
		TArray<FString> Notes;

		/**
		 * 只是个兜底的猜测：有别的有效依据就让位给它。
		 * 目前只有「这张图只在 UV 偏移里用到」——流向图、噪声、视差高度图都长这样，图本身分不清，名字说得出来就听名字的。
		 */
		bool bWeak = false;
	};

	FText EvidenceLabel(EDreamTextureEvidence Evidence)
	{
		switch (Evidence)
		{
		case EDreamTextureEvidence::UserOverride: return NSLOCTEXT("DreamTextureAnalyzer", "EvidenceUserOverride", "manual assignment");
		case EDreamTextureEvidence::SourceRecord: return NSLOCTEXT("DreamTextureAnalyzer", "EvidenceSourceRecord", "provenance record");
		case EDreamTextureEvidence::MaterialGraph: return NSLOCTEXT("DreamTextureAnalyzer", "EvidenceMaterialGraph", "material graph");
		case EDreamTextureEvidence::ParameterName: return NSLOCTEXT("DreamTextureAnalyzer", "EvidenceParameterName", "parameter name");
		case EDreamTextureEvidence::AssetName: return NSLOCTEXT("DreamTextureAnalyzer", "EvidenceAssetName", "asset name");
		case EDreamTextureEvidence::SamplerType: return NSLOCTEXT("DreamTextureAnalyzer", "EvidenceSamplerType", "sampler type");
		default: return NSLOCTEXT("DreamTextureAnalyzer", "EvidenceNone", "none");
		}
	}

	/** 「材质图：……」这一类依据说明的统一前缀。 */
	FString GraphDetail(const FString& Describe)
	{
		return FText::Format(NSLOCTEXT("DreamTextureAnalyzer", "DetailGraph", "Material graph: {0}"), FText::FromString(Describe)).ToString();
	}

	FString ChannelsToString(uint8 Mask)
	{
		FString Out;
		if (Mask & Bit::R) { Out += TEXT("R"); }
		if (Mask & Bit::G) { Out += TEXT("G"); }
		if (Mask & Bit::B) { Out += TEXT("B"); }
		if (Mask & Bit::A) { Out += TEXT("A"); }
		return Out;
	}

	int32 CountBits(uint8 Mask)
	{
		int32 Count = 0;
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Count += (Mask >> Index) & 1;
		}
		return Count;
	}

	//////////////////////////////////////////////////////////////////////////
	// 名字匹配

	struct FKeyword
	{
		const TCHAR* Word;
		EDreamTextureRole Role;
	};

	const TArray<FKeyword>& GetKeywords()
	{
		static const TArray<FKeyword> Keywords = {
			{ TEXT("basecolor"), EDreamTextureRole::BaseColor }, { TEXT("basecolour"), EDreamTextureRole::BaseColor },
			{ TEXT("albedo"), EDreamTextureRole::BaseColor }, { TEXT("alb"), EDreamTextureRole::BaseColor },
			{ TEXT("diffuse"), EDreamTextureRole::BaseColor }, { TEXT("diff"), EDreamTextureRole::BaseColor },
			{ TEXT("color"), EDreamTextureRole::BaseColor }, { TEXT("colour"), EDreamTextureRole::BaseColor },
			{ TEXT("col"), EDreamTextureRole::BaseColor }, { TEXT("bc"), EDreamTextureRole::BaseColor },
			{ TEXT("base"), EDreamTextureRole::BaseColor },

			{ TEXT("normal"), EDreamTextureRole::Normal }, { TEXT("normals"), EDreamTextureRole::Normal },
			{ TEXT("nrm"), EDreamTextureRole::Normal }, { TEXT("nor"), EDreamTextureRole::Normal },
			{ TEXT("nrml"), EDreamTextureRole::Normal }, { TEXT("norm"), EDreamTextureRole::Normal },
			{ TEXT("nm"), EDreamTextureRole::Normal },

			{ TEXT("roughness"), EDreamTextureRole::Roughness }, { TEXT("rough"), EDreamTextureRole::Roughness },
			{ TEXT("rgh"), EDreamTextureRole::Roughness },

			{ TEXT("metallic"), EDreamTextureRole::Metallic }, { TEXT("metalness"), EDreamTextureRole::Metallic },
			{ TEXT("metal"), EDreamTextureRole::Metallic }, { TEXT("mtl"), EDreamTextureRole::Metallic },

			{ TEXT("ao"), EDreamTextureRole::AO }, { TEXT("occlusion"), EDreamTextureRole::AO },
			{ TEXT("ambientocclusion"), EDreamTextureRole::AO }, { TEXT("ambientocc"), EDreamTextureRole::AO },
			{ TEXT("occ"), EDreamTextureRole::AO },

			{ TEXT("height"), EDreamTextureRole::Height }, { TEXT("heightmap"), EDreamTextureRole::Height },
			{ TEXT("displacement"), EDreamTextureRole::Height }, { TEXT("displace"), EDreamTextureRole::Height },
			{ TEXT("disp"), EDreamTextureRole::Height }, { TEXT("bump"), EDreamTextureRole::Height },
			{ TEXT("parallax"), EDreamTextureRole::Height }, { TEXT("pom"), EDreamTextureRole::Height },

			{ TEXT("emissive"), EDreamTextureRole::Emissive }, { TEXT("emission"), EDreamTextureRole::Emissive },
			{ TEXT("emit"), EDreamTextureRole::Emissive }, { TEXT("glow"), EDreamTextureRole::Emissive },

			{ TEXT("opacity"), EDreamTextureRole::Opacity }, { TEXT("alpha"), EDreamTextureRole::Opacity },
			{ TEXT("transparency"), EDreamTextureRole::Opacity },

			{ TEXT("specular"), EDreamTextureRole::Specular }, { TEXT("spec"), EDreamTextureRole::Specular },
			{ TEXT("specularity"), EDreamTextureRole::Specular },
		};
		return Keywords;
	}

	bool IsScalarMapRole(EDreamTextureRole Role)
	{
		return Role == EDreamTextureRole::Roughness || Role == EDreamTextureRole::Metallic || Role == EDreamTextureRole::AO || Role == EDreamTextureRole::Height;
	}

	/** 单个词是不是打包图的代号：orm / arm / mra / rma，以及任意 3~4 个互不重复的 o a r m h 字母。 */
	bool TryPackedCode(const FString& Token, FDreamTextureChannelLayout& OutLayout)
	{
		if (Token.Len() < 3 || Token.Len() > 4)
		{
			return false;
		}
		FDreamTextureChannelLayout Layout;
		TArray<EDreamTextureRole, TInlineAllocator<4>> Seen;
		for (int32 Index = 0; Index < Token.Len(); ++Index)
		{
			EDreamTextureRole Role = EDreamTextureRole::Unknown;
			switch (Token[Index])
			{
			case TEXT('o'):
			case TEXT('a'):
				Role = EDreamTextureRole::AO;
				break;
			case TEXT('r'):
				Role = EDreamTextureRole::Roughness;
				break;
			case TEXT('m'):
				Role = EDreamTextureRole::Metallic;
				break;
			case TEXT('h'):
				Role = EDreamTextureRole::Height;
				break;
			default:
				return false;
			}
			if (Seen.Contains(Role))
			{
				return false;
			}
			Seen.Add(Role);
			Layout.Channels[Index] = Role;
		}
		// 至少要有粗糙度/金属度中的一个，否则 "aoh" 这类不像打包图。
		if (!Seen.Contains(EDreamTextureRole::Roughness) && !Seen.Contains(EDreamTextureRole::Metallic))
		{
			return false;
		}
		OutLayout = Layout;
		return true;
	}

	bool MatchTokens(const TArray<FString>& Tokens, TCHAR SingleLetterSuffix, FDreamTextureNameMatch& OutMatch)
	{
		// 1) 打包图代号优先：不能让 "ORM" 被别的词抢先，也不能让 "normal" 里的 "orm" 误中（词是按边界切的，不是子串）。
		for (const FString& Token : Tokens)
		{
			FDreamTextureChannelLayout Layout;
			if (TryPackedCode(Token, Layout))
			{
				OutMatch.Role = EDreamTextureRole::Packed;
				OutMatch.Layout = Layout;
				OutMatch.Keyword = Token.ToUpper();
				return true;
			}
		}
		static const TCHAR* const GenericPacked[] = { TEXT("packed"), TEXT("pack"), TEXT("maskmap"), TEXT("masks"), TEXT("channelpacked") };
		for (int32 Start = 0; Start < Tokens.Num(); ++Start)
		{
			FString Joined;
			for (int32 End = Start; End < FMath::Min(Tokens.Num(), Start + 3); ++End)
			{
				Joined += Tokens[End];
				for (const TCHAR* Word : GenericPacked)
				{
					if (Joined == Word)
					{
						OutMatch.Role = EDreamTextureRole::Packed;
						OutMatch.Keyword = Joined;
						return true;
					}
				}
			}
		}

		// 2) 普通词：单词、相邻两词、相邻三词拼起来的形式都试（BaseColor → base+color）。
		struct FHit
		{
			EDreamTextureRole Role;
			FString Word;
		};
		TArray<FHit> Hits;
		const TArray<FKeyword>& Keywords = GetKeywords();
		for (int32 Start = 0; Start < Tokens.Num(); ++Start)
		{
			FString Joined;
			for (int32 End = Start; End < FMath::Min(Tokens.Num(), Start + 3); ++End)
			{
				Joined += Tokens[End];
				for (const FKeyword& Keyword : Keywords)
				{
					if (Joined == Keyword.Word)
					{
						Hits.Add({ Keyword.Role, Joined });
					}
				}
			}
		}

		// 单字母后缀（T_Rock_N、T_Rock_R…）由调用方按「下划线之后恰好一个字母」判定后传进来。
		// 不能在切好的词上判：导入器的 slug 以 id 尾段结尾（1f5d），按数字/字母边界一切，末尾就冒出个孤零零的 "d"。
		if (SingleLetterSuffix != 0)
		{
			EDreamTextureRole Role = EDreamTextureRole::Unknown;
			switch (SingleLetterSuffix)
			{
			case TEXT('n'): Role = EDreamTextureRole::Normal; break;
			case TEXT('r'): Role = EDreamTextureRole::Roughness; break;
			case TEXT('m'): Role = EDreamTextureRole::Metallic; break;
			case TEXT('h'): Role = EDreamTextureRole::Height; break;
			case TEXT('d'): Role = EDreamTextureRole::BaseColor; break;
			default: break;
			}
			if (Role != EDreamTextureRole::Unknown)
			{
				Hits.Add({ Role, FString(1, &SingleLetterSuffix).ToUpper() });
			}
		}

		if (Hits.Num() == 0)
		{
			return false;
		}

		// 3) 多个词同时命中时的取舍：
		//    - 有 emissive / opacity 就认它（EmissiveColor 里的 color 不该把它拉成 BaseColor）；
		//    - 命中的全是粗糙度/金属度/AO/高度中的两种以上，是一张打包图但排布词表给不出来；
		//    - 否则取最后一个（DetailNormal、BaseNormal 都是后面那个词说了算）。
		for (const EDreamTextureRole Priority : { EDreamTextureRole::Emissive, EDreamTextureRole::Opacity })
		{
			for (const FHit& Hit : Hits)
			{
				if (Hit.Role == Priority)
				{
					OutMatch.Role = Hit.Role;
					OutMatch.Keyword = Hit.Word;
					return true;
				}
			}
		}

		TArray<EDreamTextureRole, TInlineAllocator<4>> Distinct;
		bool bAllScalar = true;
		for (const FHit& Hit : Hits)
		{
			Distinct.AddUnique(Hit.Role);
			bAllScalar &= IsScalarMapRole(Hit.Role);
		}
		if (Distinct.Num() >= 2 && bAllScalar)
		{
			OutMatch.Role = EDreamTextureRole::Packed;
			FString Words;
			for (const FHit& Hit : Hits)
			{
				Words += (Words.IsEmpty() ? TEXT("") : TEXT("+")) + Hit.Word;
			}
			OutMatch.Keyword = Words;
			return true;
		}

		OutMatch.Role = Hits.Last().Role;
		OutMatch.Keyword = Hits.Last().Word;
		return true;
	}

	//////////////////////////////////////////////////////////////////////////
	// 各依据的候选

	bool MakeSourceRecordCandidate(const UTexture* Texture, FCandidate& Out)
	{
		FDreamTextureSourceRecord Record;
		if (!FDreamTextureImporter::ReadSourceRecord(Texture, Record))
		{
			return false;
		}

		EDreamTextureRole Role = EDreamTextureRole::Unknown;
		if (Record.Channel.Equals(DreamTextureChannel::BaseColor, ESearchCase::CaseSensitive)) { Role = EDreamTextureRole::BaseColor; }
		else if (Record.Channel.Equals(DreamTextureChannel::Normal, ESearchCase::CaseSensitive)) { Role = EDreamTextureRole::Normal; }
		else if (Record.Channel.Equals(DreamTextureChannel::Roughness, ESearchCase::CaseSensitive)) { Role = EDreamTextureRole::Roughness; }
		else if (Record.Channel.Equals(DreamTextureChannel::Metallic, ESearchCase::CaseSensitive)) { Role = EDreamTextureRole::Metallic; }
		else if (Record.Channel.Equals(DreamTextureChannel::AO, ESearchCase::CaseSensitive)) { Role = EDreamTextureRole::AO; }
		else if (Record.Channel.Equals(DreamTextureChannel::Height, ESearchCase::CaseSensitive)) { Role = EDreamTextureRole::Height; }
		else if (Record.Channel.Equals(DreamTextureChannel::ORM, ESearchCase::CaseSensitive)) { Role = EDreamTextureRole::Packed; }
		else
		{
			// 来源记录里写了个不认识的通道（以后后端加了新通道、插件还没跟上）：不当依据，别猜。
			return false;
		}

		Out.bValid = true;
		Out.Evidence = EDreamTextureEvidence::SourceRecord;
		Out.Role = Role;
		if (Role == EDreamTextureRole::Packed)
		{
			// 记录里没存 packing，ORM 的排布在 manifest v1 里是固定的 R=AO G=Roughness B=Metallic。
			Out.Layout = FDreamTextureChannelLayout::MakeORM();
		}
		Out.Detail = FText::Format(NSLOCTEXT("DreamTextureAnalyzer", "DetailSourceRecord", "Texture imported by this plugin; provenance record: channel {1} of material {0}"), FText::FromString(Record.MaterialId), FText::FromString(Record.Channel)).ToString();
		return true;
	}

	/** 贴图是不是单通道的（G8 / G16 / R16F / R32F 源，或灰度类压缩设置）。 */
	bool IsSingleChannelTexture(const UTexture* Texture)
	{
		if (Texture->Source.IsValid())
		{
			const ETextureSourceFormat Format = Texture->Source.GetFormat();
			return Format == TSF_G8 || Format == TSF_G16 || Format == TSF_R16F || Format == TSF_R32F;
		}
		return Texture->CompressionSettings == TC_Grayscale || Texture->CompressionSettings == TC_Alpha || Texture->CompressionSettings == TC_Displacementmap;
	}

	bool MakeGraphCandidate(const TArray<FDreamTextureTextureUse>& Uses, const UTexture* Texture, FCandidate& Out)
	{
		constexpr int32 SinkCount = static_cast<int32>(EDreamTextureSink::Other) + 1;
		uint8 Mask[SinkCount] = {};
		bool bApproximate[SinkCount] = {};
		for (const FDreamTextureTextureUse& Use : Uses)
		{
			const int32 Index = static_cast<int32>(Use.Sink);
			if (Use.bApproximate)
			{
				bApproximate[Index] = true;
			}
			else
			{
				Mask[Index] |= Use.Channels;
			}
		}
		auto MaskOf = [&Mask](EDreamTextureSink Sink) { return Mask[static_cast<int32>(Sink)]; };
		auto ApproxOf = [&bApproximate](EDreamTextureSink Sink) { return bApproximate[static_cast<int32>(Sink)]; };

		Out.bValid = true;
		Out.Evidence = EDreamTextureEvidence::MaterialGraph;

		// 描述：所有非近似的使用处。
		FString Describe;
		for (int32 Index = 0; Index < SinkCount; ++Index)
		{
			if (Mask[Index] != 0)
			{
				Describe += (Describe.IsEmpty() ? FString() : NSLOCTEXT("DreamTextureAnalyzer", "ListSeparator", ", ").ToString()) + FString::Printf(TEXT("%s ← %s"), LexToString(static_cast<EDreamTextureSink>(Index)), *ChannelsToString(Mask[Index]));
			}
		}

		// ---- 法线：连到 Normal 就是法线 ----
		if (MaskOf(EDreamTextureSink::Normal) != 0 || ApproxOf(EDreamTextureSink::Normal))
		{
			Out.Role = EDreamTextureRole::Normal;
			Out.Detail = GraphDetail(Describe.IsEmpty() ? NSLOCTEXT("DreamTextureAnalyzer", "NormalViaCustomNode", "connected to Normal through a custom node").ToString() : Describe);
			for (int32 Index = 0; Index < SinkCount; ++Index)
			{
				const EDreamTextureSink Sink = static_cast<EDreamTextureSink>(Index);
				if (Sink != EDreamTextureSink::Normal && Sink != EDreamTextureSink::UVOffset && Mask[Index] != 0)
				{
					Out.Notes.Add(FText::Format(NSLOCTEXT("DreamTextureAnalyzer", "NoteAlsoConnected", "Besides Normal, this texture is also connected to {0}."), FText::FromString(LexToString(Sink))).ToString());
				}
			}
			return true;
		}

		// ---- 标量角色：金属度 / 粗糙度 / AO / 高度 / 不透明度 / 高光 ----
		struct FRoleMask
		{
			EDreamTextureRole Role;
			uint8 Mask;
		};
		TArray<FRoleMask, TInlineAllocator<8>> Scalars;
		auto AddScalar = [&Scalars](EDreamTextureRole Role, uint8 ChannelMask)
		{
			if (ChannelMask == 0)
			{
				return;
			}
			if (FRoleMask* Existing = Scalars.FindByPredicate([Role](const FRoleMask& Entry) { return Entry.Role == Role; }))
			{
				Existing->Mask |= ChannelMask;
			}
			else
			{
				Scalars.Add({ Role, ChannelMask });
			}
		};
		AddScalar(EDreamTextureRole::Roughness, MaskOf(EDreamTextureSink::Roughness));
		AddScalar(EDreamTextureRole::Metallic, MaskOf(EDreamTextureSink::Metallic));
		AddScalar(EDreamTextureRole::AO, MaskOf(EDreamTextureSink::AmbientOcclusion));
		AddScalar(EDreamTextureRole::Height, MaskOf(EDreamTextureSink::Displacement));
		AddScalar(EDreamTextureRole::Specular, MaskOf(EDreamTextureSink::Specular));
		AddScalar(EDreamTextureRole::Opacity, MaskOf(EDreamTextureSink::Opacity) | MaskOf(EDreamTextureSink::OpacityMask));
		AddScalar(EDreamTextureRole::Other, MaskOf(EDreamTextureSink::Anisotropy));

		const uint8 BaseMask = MaskOf(EDreamTextureSink::BaseColor);
		const bool bFullBaseColor = (BaseMask & Bit::RGB) == Bit::RGB;
		const bool bFullEmissive = (MaskOf(EDreamTextureSink::Emissive) & Bit::RGB) == Bit::RGB;

		if (bFullBaseColor || bFullEmissive)
		{
			// 底色/自发光占 RGB 三个通道。A 通道如果被当不透明度用，说明这是带 Alpha 的彩色图，仍然是底色/自发光。
			Out.Role = bFullBaseColor ? EDreamTextureRole::BaseColor : EDreamTextureRole::Emissive;
			Out.Detail = GraphDetail(Describe);
			for (const FRoleMask& Scalar : Scalars)
			{
				Out.Notes.Add(FText::Format(NSLOCTEXT("DreamTextureAnalyzer", "NoteChannelAlsoUsed", "The {0} channel of the same texture is also used as {1}."), FText::FromString(ChannelsToString(Scalar.Mask)), FText::FromString(LexToString(Scalar.Role))).ToString());
			}
			return true;
		}

		// 不满三通道的底色，当作一个占着若干通道的角色并入打包图判断。
		AddScalar(EDreamTextureRole::BaseColor, BaseMask);

		if (Scalars.Num() > 0)
		{
			FDreamTextureChannelLayout Layout;
			for (const FRoleMask& Scalar : Scalars)
			{
				for (int32 Channel = 0; Channel < 4; ++Channel)
				{
					if (!(Scalar.Mask & (1 << Channel)))
					{
						continue;
					}
					if (Layout.Channels[Channel] == EDreamTextureRole::Unknown)
					{
						Layout.Channels[Channel] = Scalar.Role;
					}
					else if (Layout.Channels[Channel] != Scalar.Role)
					{
						Out.Notes.Add(FText::Format(NSLOCTEXT("DreamTextureAnalyzer", "NoteChannelConflict", "The {0} channel is used as both {1} and {2}; the first one is taken."), FText::FromString(ChannelsToString(static_cast<uint8>(1 << Channel))), FText::FromString(LexToString(Layout.Channels[Channel])), FText::FromString(LexToString(Scalar.Role))).ToString());
					}
				}
			}

			if (Scalars.Num() >= 2)
			{
				Out.Role = EDreamTextureRole::Packed;
				Out.Layout = Layout;
				Out.Detail = GraphDetail(Describe);
				return true;
			}

			const FRoleMask& Only = Scalars[0];
			if ((Only.Mask & Bit::R) || IsSingleChannelTexture(Texture))
			{
				// 灰度图的常规接法：第一个分量就是它。
				Out.Role = Only.Role;
				Out.Detail = GraphDetail(Describe);
				if (Only.Role == EDreamTextureRole::BaseColor && CountBits(Only.Mask) < 3)
				{
					Out.Notes.Add(NSLOCTEXT("DreamTextureAnalyzer", "NoteBaseColorPartial", "Base Color reads only some of this texture's channels.").ToString());
				}
				return true;
			}

			// 只用了 G / B / A 里的某一个：这是一张打包图，其余通道材质没用到，靠图推不出它们是什么。
			Out.Role = EDreamTextureRole::Packed;
			Out.Layout = Layout;
			Out.Detail = GraphDetail(Describe);
			Out.Notes.Add(NSLOCTEXT("DreamTextureAnalyzer", "NoteSingleChannelRead", "The material reads only one channel, so what the other channels hold cannot be inferred.").ToString());
			return true;
		}

		if (MaskOf(EDreamTextureSink::Emissive) != 0)
		{
			Out.Role = EDreamTextureRole::Emissive;
			Out.Detail = GraphDetail(Describe);
			return true;
		}

		// ---- 间接用法 ----
		if (MaskOf(EDreamTextureSink::UVOffset) != 0 || ApproxOf(EDreamTextureSink::UVOffset))
		{
			Out.Role = EDreamTextureRole::Height;
			Out.Detail = NSLOCTEXT("DreamTextureAnalyzer", "DetailUVOffsetOnly", "Material graph: only used in a UV offset").ToString();
			Out.Notes.Add(NSLOCTEXT("DreamTextureAnalyzer", "NoteUVOffsetOnly", "It is only used in a UV offset, so it could be a parallax height map or a flow map or noise; it is guessed as Height for now. Change it manually if that is wrong.").ToString());
			Out.bWeak = true;
			return true;
		}
		if (MaskOf(EDreamTextureSink::BlendMask) != 0 || ApproxOf(EDreamTextureSink::BlendMask))
		{
			Out.Role = EDreamTextureRole::Other;
			Out.Detail = NSLOCTEXT("DreamTextureAnalyzer", "DetailBlendMask", "Material graph: used as a blend mask").ToString();
			return true;
		}

		// ---- 只经过自定义节点等近似路径 ----
		static const struct { EDreamTextureSink Sink; EDreamTextureRole Role; } ApproxOrder[] = {
			{ EDreamTextureSink::BaseColor, EDreamTextureRole::BaseColor }, { EDreamTextureSink::Roughness, EDreamTextureRole::Roughness },
			{ EDreamTextureSink::Metallic, EDreamTextureRole::Metallic }, { EDreamTextureSink::AmbientOcclusion, EDreamTextureRole::AO },
			{ EDreamTextureSink::Displacement, EDreamTextureRole::Height }, { EDreamTextureSink::Emissive, EDreamTextureRole::Emissive },
			{ EDreamTextureSink::Opacity, EDreamTextureRole::Opacity }, { EDreamTextureSink::OpacityMask, EDreamTextureRole::Opacity },
			{ EDreamTextureSink::Specular, EDreamTextureRole::Specular },
		};
		for (const auto& Entry : ApproxOrder)
		{
			if (ApproxOf(Entry.Sink))
			{
				Out.Role = Entry.Role;
				Out.Detail = FText::Format(NSLOCTEXT("DreamTextureAnalyzer", "DetailViaCustomNode", "Material graph: connected to {0} through a custom node"), FText::FromString(LexToString(Entry.Sink))).ToString();
				Out.Notes.Add(NSLOCTEXT("DreamTextureAnalyzer", "NoteViaCustomNode", "It passes through nodes that cannot be parsed, such as a custom HLSL node, so the channels are estimated.").ToString());
				return true;
			}
		}

		if (MaskOf(EDreamTextureSink::WorldPositionOffset) != 0 || MaskOf(EDreamTextureSink::Tangent) != 0 || MaskOf(EDreamTextureSink::Subsurface) != 0
			|| MaskOf(EDreamTextureSink::Refraction) != 0)
		{
			Out.Role = EDreamTextureRole::Other;
			Out.Detail = GraphDetail(Describe);
			return true;
		}

		// 只落在「其它」上：图里说不出什么，交给名字去猜。
		Out = FCandidate();
		return false;
	}

	/** 贴图自己的压缩设置与采样器类型。这是最后一道，只认最明确的几个。 */
	bool MakeSamplerCandidate(const UTexture* Texture, const TArray<EMaterialSamplerType>& SamplerTypes, FCandidate& Out)
	{
		Out.Evidence = EDreamTextureEvidence::SamplerType;

		if (Texture->CompressionSettings == TC_Normalmap || SamplerTypes.Contains(SAMPLERTYPE_Normal))
		{
			Out.bValid = true;
			Out.Role = EDreamTextureRole::Normal;
			Out.Detail = Texture->CompressionSettings == TC_Normalmap ? NSLOCTEXT("DreamTextureAnalyzer", "DetailCompressionNormal", "The texture compression setting is Normalmap (TC_Normalmap)").ToString() : NSLOCTEXT("DreamTextureAnalyzer", "DetailSamplerNormal", "The sampler type is Normal").ToString();
			return true;
		}
		if (Texture->CompressionSettings == TC_Masks || SamplerTypes.Contains(SAMPLERTYPE_Masks))
		{
			Out.bValid = true;
			Out.Role = EDreamTextureRole::Packed;
			Out.Detail = Texture->CompressionSettings == TC_Masks ? NSLOCTEXT("DreamTextureAnalyzer", "DetailCompressionMasks", "The texture compression setting is Masks (TC_Masks)").ToString() : NSLOCTEXT("DreamTextureAnalyzer", "DetailSamplerMasks", "The sampler type is Masks").ToString();
			Out.Notes.Add(NSLOCTEXT("DreamTextureAnalyzer", "NoteMasks", "Masks textures are usually packed textures, but what each channel holds cannot be inferred.").ToString());
			return true;
		}
		if (Texture->SRGB && SamplerTypes.Contains(SAMPLERTYPE_Color))
		{
			Out.bValid = true;
			Out.Role = EDreamTextureRole::BaseColor;
			Out.Detail = NSLOCTEXT("DreamTextureAnalyzer", "DetailSamplerColor", "The sampler type is Color (sRGB color)").ToString();
			Out.Notes.Add(NSLOCTEXT("DreamTextureAnalyzer", "NoteColorGuess", "It can only be guessed as Base Color because it is a color texture; please check.").ToString());
			return true;
		}
		return false;
	}

	bool MakeNameCandidate(const FString& Name, bool bIsAssetName, FCandidate& Out)
	{
		FDreamTextureNameMatch Match;
		if (!FDreamTextureMaterialAnalyzer::MatchName(Name, Match))
		{
			return false;
		}
		Out.bValid = true;
		Out.Evidence = bIsAssetName ? EDreamTextureEvidence::AssetName : EDreamTextureEvidence::ParameterName;
		Out.Role = Match.Role;
		Out.Layout = Match.Layout;
		Out.Detail = FText::Format(bIsAssetName
			? NSLOCTEXT("DreamTextureAnalyzer", "DetailAssetName", "Asset name '{0}' contains '{1}'")
			: NSLOCTEXT("DreamTextureAnalyzer", "DetailParameterName", "Parameter name '{0}' contains '{1}'"),
			FText::FromString(Name), FText::FromString(Match.Keyword)).ToString();
		return true;
	}

	/** 图给出的（可能不全的）排布与名字给出的排布是否矛盾：图里已确定的通道，名字必须不反对。 */
	bool LayoutsConsistent(const FDreamTextureChannelLayout& FromGraph, const FDreamTextureChannelLayout& FromName)
	{
		for (int32 Index = 0; Index < 4; ++Index)
		{
			if (FromGraph.Channels[Index] != EDreamTextureRole::Unknown && FromName.Channels[Index] != EDreamTextureRole::Unknown
				&& FromGraph.Channels[Index] != FromName.Channels[Index])
			{
				return false;
			}
		}
		return true;
	}

	bool IsPackedGraphResult(const FCandidate& Candidate)
	{
		return Candidate.Role == EDreamTextureRole::Packed;
	}
}

//////////////////////////////////////////////////////////////////////////
// 名字

TArray<FString> FDreamTextureMaterialAnalyzer::TokenizeName(const FString& Name)
{
	TArray<FString> Tokens;
	FString Current;
	auto Flush = [&Tokens, &Current]()
	{
		if (!Current.IsEmpty())
		{
			Tokens.Add(Current.ToLower());
			Current.Reset();
		}
	};

	auto IsUpper = [](TCHAR C) { return C >= TEXT('A') && C <= TEXT('Z'); };
	auto IsLower = [](TCHAR C) { return C >= TEXT('a') && C <= TEXT('z'); };
	auto IsDigit = [](TCHAR C) { return C >= TEXT('0') && C <= TEXT('9'); };

	for (int32 Index = 0; Index < Name.Len(); ++Index)
	{
		const TCHAR C = Name[Index];
		if (!IsUpper(C) && !IsLower(C) && !IsDigit(C))
		{
			Flush();
			continue;
		}
		if (!Current.IsEmpty())
		{
			const TCHAR Prev = Current[Current.Len() - 1];
			// 小写→大写（baseColor）、数字↔字母、连续大写后接小写（ORMMap → ORM + Map）都是词边界。
			const bool bLowerToUpper = IsLower(Prev) && IsUpper(C);
			const bool bDigitBoundary = IsDigit(Prev) != IsDigit(C);
			const bool bAcronymEnd = IsUpper(Prev) && IsUpper(C) && Index + 1 < Name.Len() && IsLower(Name[Index + 1]);
			if (bLowerToUpper || bDigitBoundary || bAcronymEnd)
			{
				Flush();
			}
		}
		Current.AppendChar(C);
	}
	Flush();
	return Tokens;
}

bool FDreamTextureMaterialAnalyzer::MatchName(const FString& Name, FDreamTextureNameMatch& OutMatch)
{
	const TArray<FString> Tokens = TokenizeName(Name);
	if (Tokens.Num() == 0)
	{
		return false;
	}
	// 单字母后缀：最后一个非字母数字分隔符之后恰好一个字母（T_Rock_N）。参数名里的 Tex_N 同样按这条认，
	// 这种写法在参数名里也基本就是那个意思；单独一个 "N" 没有分隔符，不认。
	TCHAR Suffix = 0;
	int32 SeparatorIndex = INDEX_NONE;
	for (int32 Index = Name.Len() - 1; Index >= 0; --Index)
	{
		const TCHAR C = Name[Index];
		const bool bAlnum = (C >= TEXT('0') && C <= TEXT('9')) || (C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('A') && C <= TEXT('Z'));
		if (!bAlnum)
		{
			SeparatorIndex = Index;
			break;
		}
	}
	if (SeparatorIndex != INDEX_NONE && SeparatorIndex == Name.Len() - 2)
	{
		const TCHAR Letter = FChar::ToLower(Name[Name.Len() - 1]);
		if (Letter >= TEXT('a') && Letter <= TEXT('z'))
		{
			Suffix = Letter;
		}
	}
	return MatchTokens(Tokens, Suffix, OutMatch);
}

//////////////////////////////////////////////////////////////////////////
// 材质与槽位

FString FDreamTextureMaterialAnalyzer::GetAssetPathForOverrides(const UMaterialInterface* Material)
{
	const UMaterialInterface* Current = Material;
	// 动态实例（MID）只存在于运行时/构造脚本，不是资产，路径在 /Engine/Transient 之类。退到它的父级去。
	while (Current != nullptr)
	{
		const UMaterialInstanceDynamic* Dynamic = Cast<UMaterialInstanceDynamic>(Current);
		const bool bTransient = Dynamic != nullptr || Current->GetOutermost() == GetTransientPackage();
		if (!bTransient)
		{
			return Current->GetPathName();
		}
		const UMaterialInstance* Instance = Cast<UMaterialInstance>(Current);
		Current = Instance != nullptr ? Instance->Parent.Get() : nullptr;
	}
	return FString();
}

TArray<FDreamTextureMaterialSlot> FDreamTextureMaterialAnalyzer::GetActorSlots(AActor* Actor)
{
	TArray<FDreamTextureMaterialSlot> Out;
	if (Actor == nullptr)
	{
		return Out;
	}

	TArray<UMeshComponent*> Components;
	Actor->GetComponents<UMeshComponent>(Components);
	for (UMeshComponent* Component : Components)
	{
		const int32 NumSlots = Component->GetNumMaterials();
		const TArray<FName> SlotNames = Component->GetMaterialSlotNames();
		for (int32 Index = 0; Index < NumSlots; ++Index)
		{
			FDreamTextureMaterialSlot Slot;
			Slot.Component = Component;
			Slot.SlotIndex = Index;
			Slot.SlotName = SlotNames.IsValidIndex(Index) ? SlotNames[Index] : NAME_None;
			UMaterialInterface* Material = Component->GetMaterial(Index);
			Slot.Material = Material;
			Slot.Label = FText::Format(NSLOCTEXT("DreamTextureAnalyzer", "SlotLabel", "{0} · slot {1}{2} · {3}"),
				FText::FromString(Component->GetName()), Index,
				Slot.SlotName.IsNone() ? FText::GetEmpty() : FText::Format(NSLOCTEXT("DreamTextureAnalyzer", "SlotNameSuffix", " ({0})"), FText::FromString(Slot.SlotName.ToString())),
				Material != nullptr ? FText::FromString(Material->GetName()) : NSLOCTEXT("DreamTextureAnalyzer", "SlotEmpty", "(empty)")).ToString();
			Out.Add(MoveTemp(Slot));
		}
	}
	return Out;
}

//////////////////////////////////////////////////////////////////////////
// 解析

FDreamTextureMaterialAnalysis FDreamTextureMaterialAnalyzer::Analyze(UMaterialInterface* Material, const FDreamTextureAnalyzeOptions& Options)
{
	check(IsInGameThread());

	FDreamTextureMaterialAnalysis Out;
	if (Material == nullptr)
	{
		Out.Error = NSLOCTEXT("DreamTextureAnalyzer", "NoMaterialSelected", "No material is selected.").ToString();
		return Out;
	}

	Out.Material = Material;
	Out.MaterialPath = Material->GetPathName();
	Out.DisplayName = Material->GetName();
	Out.bIsInstance = Material->IsA<UMaterialInstance>();
	Out.AssetPath = GetAssetPathForOverrides(Material);

	UMaterial* Base = Material->GetMaterial();
	if (Base == nullptr)
	{
		Out.Error = FText::Format(NSLOCTEXT("DreamTextureAnalyzer", "NoRootMaterial", "Could not find the root material of {0} (is the parent chain broken?)."), FText::FromString(Out.DisplayName)).ToString();
		return Out;
	}
	Out.BaseMaterialPath = Base->GetPathName();

	const DreamTextureGraph::FWalkResult Walk = DreamTextureGraph::Walk(Material);
	Out.Warnings = Walk.Warnings;

	// 按贴图分组，保持首次出现的顺序。
	TMap<const UTexture*, int32> RowOf;
	TArray<TArray<EMaterialSamplerType>> SamplerTypes;
	for (const DreamTextureGraph::FRawUse& Raw : Walk.Uses)
	{
		int32* Found = RowOf.Find(Raw.Texture);
		if (Found == nullptr)
		{
			FDreamTextureMaterialTexture Row;
			Row.Texture = Raw.Texture;
			Row.TexturePath = Raw.Texture->GetPathName();
			Row.AssetName = Raw.Texture->GetName();
			if (Raw.Texture->Source.IsValid())
			{
				Row.SourceWidth = static_cast<int32>(Raw.Texture->Source.GetSizeX());
				Row.SourceHeight = static_cast<int32>(Raw.Texture->Source.GetSizeY());
			}
			Row.bExportable = FDreamTextureTextureExporter::CanExport(Raw.Texture, Row.ExportBlocker);
			RowOf.Add(Raw.Texture, Out.Textures.Num());
			Found = &RowOf.FindChecked(Raw.Texture);
			Out.Textures.Add(MoveTemp(Row));
			SamplerTypes.AddDefaulted();
		}

		FDreamTextureMaterialTexture& Row = Out.Textures[*Found];
		if (!Raw.Parameter.IsNone())
		{
			Row.Parameters.AddUnique(Raw.Parameter);
		}
		Row.bOverriddenBySelectedInstance |= Raw.bOverriddenBySelected;
		Row.Uses.Add(Raw.Use);
		SamplerTypes[*Found].AddUnique(Raw.SamplerType);
	}

	for (int32 Index = 0; Index < Out.Textures.Num(); ++Index)
	{
		FDreamTextureMaterialTexture& Row = Out.Textures[Index];
		UTexture* Texture = Row.Texture.Get();
		if (Texture == nullptr)
		{
			continue;
		}

		// ---- 收集各依据的候选 ----
		FCandidate Source;
		FCandidate Graph;
		TArray<FCandidate> ParameterNames;
		FCandidate AssetNameCandidate;
		FCandidate Sampler;

		MakeSourceRecordCandidate(Texture, Source);
		MakeGraphCandidate(Row.Uses, Texture, Graph);
		for (const FName& Parameter : Row.Parameters)
		{
			FCandidate Candidate;
			if (MakeNameCandidate(Parameter.ToString(), /*bIsAssetName*/ false, Candidate))
			{
				ParameterNames.Add(MoveTemp(Candidate));
			}
		}
		MakeNameCandidate(Row.AssetName, /*bIsAssetName*/ true, AssetNameCandidate);
		MakeSamplerCandidate(Texture, SamplerTypes[Index], Sampler);

		// ---- 取第一个有效的 ----
		TArray<const FCandidate*> Ordered;
		Ordered.Add(&Source);
		Ordered.Add(&Graph);
		for (const FCandidate& Candidate : ParameterNames)
		{
			Ordered.Add(&Candidate);
		}
		Ordered.Add(&AssetNameCandidate);
		Ordered.Add(&Sampler);

		FCandidate Winner;
		int32 WinnerIndex = INDEX_NONE;
		for (int32 CandidateIndex = 0; CandidateIndex < Ordered.Num(); ++CandidateIndex)
		{
			if (Ordered[CandidateIndex]->bValid && !Ordered[CandidateIndex]->bWeak)
			{
				Winner = *Ordered[CandidateIndex];
				WinnerIndex = CandidateIndex;
				break;
			}
		}
		if (WinnerIndex == INDEX_NONE)
		{
			// 没有靠谱的依据：退一步用兜底的猜测。
			for (int32 CandidateIndex = 0; CandidateIndex < Ordered.Num(); ++CandidateIndex)
			{
				if (Ordered[CandidateIndex]->bValid)
				{
					Winner = *Ordered[CandidateIndex];
					WinnerIndex = CandidateIndex;
					break;
				}
			}
		}

		// ---- 图给的打包图排布不全时，用名字补全 ----
		if (WinnerIndex != INDEX_NONE && Winner.Evidence == EDreamTextureEvidence::MaterialGraph && IsPackedGraphResult(Winner) && Winner.Layout.HasUnknownRGB())
		{
			for (const FCandidate* Candidate : Ordered)
			{
				if (!Candidate->bValid || (Candidate->Evidence != EDreamTextureEvidence::ParameterName && Candidate->Evidence != EDreamTextureEvidence::AssetName)
					|| Candidate->Role != EDreamTextureRole::Packed || Candidate->Layout.HasUnknownRGB())
				{
					continue;
				}
				if (LayoutsConsistent(Winner.Layout, Candidate->Layout))
				{
					for (int32 Channel = 0; Channel < 4; ++Channel)
					{
						if (Winner.Layout.Channels[Channel] == EDreamTextureRole::Unknown)
						{
							Winner.Layout.Channels[Channel] = Candidate->Layout.Channels[Channel];
						}
					}
					Winner.Detail += FText::Format(NSLOCTEXT("DreamTextureAnalyzer", "DetailCompletedFrom", "; the remaining channels are filled in from: {0}"), FText::FromString(Candidate->Detail)).ToString();
					break;
				}
				Winner.Notes.Add(FText::Format(NSLOCTEXT("DreamTextureAnalyzer", "NoteLayoutRejected", "The layout suggested by '{0}' ({1}) contradicts the channels fixed by the material graph and was not used."), FText::FromString(Candidate->Detail), FText::FromString(Candidate->Layout.ToString())).ToString());
			}
		}

		// ---- 落选依据与结论冲突的，记成提示 ----
		if (WinnerIndex != INDEX_NONE)
		{
			for (int32 CandidateIndex = 0; CandidateIndex < Ordered.Num(); ++CandidateIndex)
			{
				const FCandidate* Candidate = Ordered[CandidateIndex];
				if (CandidateIndex == WinnerIndex || !Candidate->bValid)
				{
					continue;
				}
				const bool bRoleDiffers = Candidate->Role != Winner.Role;
				const bool bLayoutDiffers = Candidate->Role == EDreamTextureRole::Packed && Winner.Role == EDreamTextureRole::Packed
					&& !Candidate->Layout.HasUnknownRGB() && !LayoutsConsistent(Winner.Layout, Candidate->Layout);
				if (bRoleDiffers || bLayoutDiffers)
				{
					Winner.Notes.Add(FText::Format(NSLOCTEXT("DreamTextureAnalyzer", "NoteDisagrees", "'{0}' says it is {1}, which disagrees with the chosen result ({2})."), FText::FromString(Candidate->Detail), FText::FromString(LexToString(Candidate->Role)), FText::FromString(LexToString(Winner.Role))).ToString());
				}
			}
		}

		Row.Role = Winner.Role;
		Row.Layout = Winner.Layout;
		Row.Evidence = WinnerIndex != INDEX_NONE ? Winner.Evidence : EDreamTextureEvidence::None;
		Row.EvidenceDetail = WinnerIndex != INDEX_NONE ? Winner.Detail : NSLOCTEXT("DreamTextureAnalyzer", "NoEvidence", "No evidence was found").ToString();
		Row.Notes = Winner.Notes;
		Row.bNeedsUserInput = (Row.Role == EDreamTextureRole::Unknown) || (Row.Role == EDreamTextureRole::Packed && Row.Layout.HasUnknownRGB());

		// ---- 用户的指认压过一切推断 ----
		if (Options.bApplyOverrides && !Out.AssetPath.IsEmpty())
		{
			FDreamTextureOverrideStore& Store = Options.Overrides != nullptr ? *Options.Overrides : FDreamTextureOverrideStore::Get();
			FDreamTextureRoleOverride Override;
			if (Store.Find(Out.AssetPath, Row.TexturePath, Override))
			{
				// 自动推断的结果留作一条提示：用户事后想知道「当初插件是怎么认的」。
				if (Row.Evidence != EDreamTextureEvidence::None)
				{
					Row.Notes.Insert(FText::Format(NSLOCTEXT("DreamTextureAnalyzer", "NoteAutoInferred", "Automatically inferred as {0} (evidence: {1})."), FText::FromString(LexToString(Row.Role)), FText::FromString(Row.EvidenceDetail)).ToString(), 0);
				}
				Row.Role = Override.Role;
				Row.Layout = Override.Role == EDreamTextureRole::Packed ? Override.Layout : FDreamTextureChannelLayout();
				Row.Evidence = EDreamTextureEvidence::UserOverride;
				Row.EvidenceDetail = NSLOCTEXT("DreamTextureAnalyzer", "DetailUserOverride", "Your manual assignment").ToString();
				Row.bNeedsUserInput = (Row.Role == EDreamTextureRole::Unknown) || (Row.Role == EDreamTextureRole::Packed && Row.Layout.HasUnknownRGB());
			}
		}
	}

	Out.bOK = true;
	return Out;
}
