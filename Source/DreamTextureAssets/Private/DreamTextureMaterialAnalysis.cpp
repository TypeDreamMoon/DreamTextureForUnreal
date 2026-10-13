// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureMaterialAnalysis.h"

#include "DreamTextureManifest.h"

/*
 * 解析结果的小方法：枚举与字符串互转、打包图排布、按路径找贴图。
 * 真正的分析逻辑在 DreamTextureMaterialAnalyzer.cpp。
 */

const TCHAR* LexToString(EDreamTextureRole Role)
{
	switch (Role)
	{
	case EDreamTextureRole::BaseColor: return TEXT("basecolor");
	case EDreamTextureRole::Normal: return TEXT("normal");
	case EDreamTextureRole::Roughness: return TEXT("roughness");
	case EDreamTextureRole::Metallic: return TEXT("metallic");
	case EDreamTextureRole::AO: return TEXT("ao");
	case EDreamTextureRole::Height: return TEXT("height");
	case EDreamTextureRole::Packed: return TEXT("packed");
	case EDreamTextureRole::Emissive: return TEXT("emissive");
	case EDreamTextureRole::Opacity: return TEXT("opacity");
	case EDreamTextureRole::Specular: return TEXT("specular");
	case EDreamTextureRole::Other: return TEXT("other");
	default: return TEXT("unknown");
	}
}

bool LexTryParseString(EDreamTextureRole& OutRole, const FString& Text)
{
	static const EDreamTextureRole All[] = {
		EDreamTextureRole::Unknown, EDreamTextureRole::BaseColor, EDreamTextureRole::Normal, EDreamTextureRole::Roughness, EDreamTextureRole::Metallic,
		EDreamTextureRole::AO, EDreamTextureRole::Height, EDreamTextureRole::Packed, EDreamTextureRole::Emissive, EDreamTextureRole::Opacity,
		EDreamTextureRole::Specular, EDreamTextureRole::Other,
	};
	for (const EDreamTextureRole Role : All)
	{
		if (Text.Equals(LexToString(Role), ESearchCase::IgnoreCase))
		{
			OutRole = Role;
			return true;
		}
	}
	return false;
}

const TCHAR* LexToString(EDreamTextureEvidence Evidence)
{
	switch (Evidence)
	{
	case EDreamTextureEvidence::UserOverride: return TEXT("user");
	case EDreamTextureEvidence::SourceRecord: return TEXT("source_record");
	case EDreamTextureEvidence::MaterialGraph: return TEXT("graph");
	case EDreamTextureEvidence::ParameterName: return TEXT("parameter_name");
	case EDreamTextureEvidence::AssetName: return TEXT("asset_name");
	case EDreamTextureEvidence::SamplerType: return TEXT("sampler_type");
	default: return TEXT("none");
	}
}

const TCHAR* LexToString(EDreamTextureSink Sink)
{
	switch (Sink)
	{
	case EDreamTextureSink::BaseColor: return TEXT("BaseColor");
	case EDreamTextureSink::Metallic: return TEXT("Metallic");
	case EDreamTextureSink::Specular: return TEXT("Specular");
	case EDreamTextureSink::Roughness: return TEXT("Roughness");
	case EDreamTextureSink::Anisotropy: return TEXT("Anisotropy");
	case EDreamTextureSink::Emissive: return TEXT("EmissiveColor");
	case EDreamTextureSink::Opacity: return TEXT("Opacity");
	case EDreamTextureSink::OpacityMask: return TEXT("OpacityMask");
	case EDreamTextureSink::Normal: return TEXT("Normal");
	case EDreamTextureSink::Tangent: return TEXT("Tangent");
	case EDreamTextureSink::AmbientOcclusion: return TEXT("AmbientOcclusion");
	case EDreamTextureSink::Subsurface: return TEXT("SubsurfaceColor");
	case EDreamTextureSink::Refraction: return TEXT("Refraction");
	case EDreamTextureSink::Displacement: return TEXT("Displacement");
	case EDreamTextureSink::WorldPositionOffset: return TEXT("WorldPositionOffset");
	case EDreamTextureSink::UVOffset: return TEXT("UV");
	case EDreamTextureSink::BlendMask: return TEXT("BlendMask");
	default: return TEXT("Other");
	}
}

bool FDreamTextureChannelLayout::HasUnknownRGB() const
{
	return Channels[0] == EDreamTextureRole::Unknown || Channels[1] == EDreamTextureRole::Unknown || Channels[2] == EDreamTextureRole::Unknown;
}

bool FDreamTextureChannelLayout::operator==(const FDreamTextureChannelLayout& Other) const
{
	for (int32 Index = 0; Index < 4; ++Index)
	{
		if (Channels[Index] != Other.Channels[Index])
		{
			return false;
		}
	}
	return true;
}

FString FDreamTextureChannelLayout::ToString() const
{
	static const TCHAR* const Names[4] = { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") };
	FString Out;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		if (Index > 0)
		{
			Out += TEXT(",");
		}
		Out += FString::Printf(TEXT("%s=%s"), Names[Index], LexToString(Channels[Index]));
	}
	return Out;
}

FDreamTextureChannelLayout FDreamTextureChannelLayout::MakeORM()
{
	FDreamTextureChannelLayout Layout;
	Layout.Channels[0] = EDreamTextureRole::AO;
	Layout.Channels[1] = EDreamTextureRole::Roughness;
	Layout.Channels[2] = EDreamTextureRole::Metallic;
	return Layout;
}

FString FDreamTextureMaterialTexture::GetManifestChannel() const
{
	switch (Role)
	{
	case EDreamTextureRole::BaseColor: return DreamTextureChannel::BaseColor;
	case EDreamTextureRole::Normal: return DreamTextureChannel::Normal;
	case EDreamTextureRole::Roughness: return DreamTextureChannel::Roughness;
	case EDreamTextureRole::Metallic: return DreamTextureChannel::Metallic;
	case EDreamTextureRole::AO: return DreamTextureChannel::AO;
	case EDreamTextureRole::Height: return DreamTextureChannel::Height;
	case EDreamTextureRole::Packed:
		{
			// 只看 R/G/B：A 在 ORM 里不存在，用户或图推断出的 A 不影响它是不是 ORM。
			const FDreamTextureChannelLayout Orm = FDreamTextureChannelLayout::MakeORM();
			if (Layout.Channels[0] == Orm.Channels[0] && Layout.Channels[1] == Orm.Channels[1] && Layout.Channels[2] == Orm.Channels[2])
			{
				return DreamTextureChannel::ORM;
			}
			return FString();
		}
	default:
		return FString();
	}
}

const FDreamTextureMaterialTexture* FDreamTextureMaterialAnalysis::FindTexture(const FString& TexturePath) const
{
	return Textures.FindByPredicate([&TexturePath](const FDreamTextureMaterialTexture& Entry) { return Entry.TexturePath.Equals(TexturePath, ESearchCase::CaseSensitive); });
}

FDreamTextureMaterialTexture* FDreamTextureMaterialAnalysis::FindTexture(const FString& TexturePath)
{
	return Textures.FindByPredicate([&TexturePath](const FDreamTextureMaterialTexture& Entry) { return Entry.TexturePath.Equals(TexturePath, ESearchCase::CaseSensitive); });
}
