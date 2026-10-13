// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * manifest.maps 的键。后端只会产出这几个，但消费端必须容忍未知键（manifest v1 的兼容性约定）。
 */
namespace DreamTextureChannel
{
	inline const FString BaseColor = TEXT("basecolor");
	inline const FString Normal = TEXT("normal");
	inline const FString Roughness = TEXT("roughness");
	inline const FString Metallic = TEXT("metallic");
	inline const FString AO = TEXT("ao");
	inline const FString Height = TEXT("height");
	/** R=AO、G=Roughness、B=Metallic。以 FDreamTextureMapInfo::Packing 为准，别凭这句注释假设。 */
	inline const FString ORM = TEXT("orm");
}

/** 贴图的色彩空间。UE 里据此设 sRGB 开关；不靠文件名猜。 */
enum class EDreamTextureColorSpace : uint8
{
	Unknown,
	SRGB,
	Linear,
};

/** manifest.maps 里的一项。字段对应后端仓库（https://github.com/TypeDreamMoon/DreamTexture）的 docs/manifest-v1.md。 */
struct DREAMTEXTURECLIENT_API FDreamTextureMapInfo
{
	/** maps 的键，见 DreamTextureChannel。 */
	FString Channel;

	/** 套装目录里的文件名，如 "basecolor.png"。 */
	FString File;

	EDreamTextureColorSpace ColorSpace = EDreamTextureColorSpace::Unknown;

	/**
	 * 仅法线有：directx | opengl。
	 *
	 * v1 两条管线统一输出 directx（UE 原生），导入时**不要**再翻 G 通道。
	 * 字段为 opengl 时才需要翻——到今天为止后端还没产出过这种。
	 */
	FString NormalY;

	/** 仅打包图有，如 "R=ao,G=roughness,B=metallic"。 */
	FString Packing;

	int32 Width = 0;
	int32 Height = 0;
	int64 Bytes = 0;

	/**
	 * 文件**全部字节** SHA-256 的小写十六进制，取前 16 位。
	 * 算法见后端 internal/material/manifest.go。用于去重和下载校验。
	 */
	FString Sha256;
};

/** manifest.license_flags。 */
struct DREAMTEXTURECLIENT_API FDreamTextureLicense
{
	/** manifest 里有没有这个字段。没有时不要当成「可商用」。 */
	bool bKnown = false;

	bool bCommercialUse = false;

	/** 中文或英文原因，直接显示，如 "CHORD: Ubisoft ML License (research-only)"。 */
	FString Reason;
};

/**
 * 一套材质的 manifest（schema dreamtexture/material-manifest，v1 已冻结）。
 *
 * 消费端只认 manifest，不假设贴图是怎么生成的。
 */
struct DREAMTEXTURECLIENT_API FDreamTextureManifest
{
	static constexpr int32 SupportedSchemaVersion = 1;

	/** 材质 ID，同时是后端 output 下的目录名，形如 mat_20260827_092650_1f53。 */
	FString Id;
	FString Name;

	/** realistic | stylized */
	FString Style;

	FString WorkflowId;
	int32 WorkflowVersion = 0;

	FString Prompt;
	FString Negative;
	int64 Seed = 0;
	int32 Resolution = 0;

	/** 按次判定的，不是模板上的静态值。 */
	bool bTileable = false;

	/** 键是通道名。缺某个通道是合法的，消费端要降级而不是报错。 */
	TMap<FString, FDreamTextureMapInfo> Maps;

	/** 3×3 平铺预览的文件名，通常是 preview.png。 */
	FString Preview;

	/** RFC3339 原文。 */
	FString CreatedAt;

	FDreamTextureLicense License;

	/**
	 * manifest 里有 source 字段：底图来自云端模型。
	 *
	 * 这个字段一出现就说明**同样的参数复现不了**——云端接口不支持种子。
	 * 界面上的「用相同参数再来一张」要据此禁用。
	 */
	bool bHasCloudSource = false;

	/** 整份 JSON，给还没建模的字段兜底。 */
	TSharedPtr<FJsonObject> Raw;

	/**
	 * 解析并校验。
	 *
	 * schema 不是 dreamtexture/material-manifest、或 schema_version 高于 SupportedSchemaVersion 时
	 * 返回 false，OutError 给中文原因。其余字段缺失或为 null 一律走默认值，不报错——
	 * Go 的 nil 切片会序列化成 null，后端仓库踩过两次。未知字段忽略。
	 */
	static bool FromJson(const TSharedPtr<FJsonObject>& Obj, FDreamTextureManifest& Out, FString& OutError);
	static bool FromJsonString(const FString& Json, FDreamTextureManifest& Out, FString& OutError);

	/** 没有返回 nullptr。 */
	const FDreamTextureMapInfo* FindMap(const FString& Channel) const;
};

/** GET /api/materials 列表里的一行（后端 internal/store.Material）。不含贴图信息，要的话再拉 manifest。 */
struct DREAMTEXTURECLIENT_API FDreamTextureMaterialSummary
{
	FString Id;
	FString Name;
	FString Style;
	FString WorkflowId;
	FString Prompt;
	int64 Seed = 0;
	int32 Resolution = 0;
	bool bFavorite = false;
	TArray<FString> Tags;
	FDateTime CreatedAt;

	static bool FromJson(const TSharedPtr<FJsonObject>& Obj, FDreamTextureMaterialSummary& Out);
};

/** GET /api/materials 的查询参数，名字与后端 listMaterials 一致。 */
struct DREAMTEXTURECLIENT_API FDreamTextureMaterialQuery
{
	/** q：全文搜索，后端有 FTS 时按前缀匹配。 */
	FString Text;

	/** style：空 = 全部。 */
	FString Style;

	/** fav=1 */
	bool bFavoriteOnly = false;

	/** 后端 ≤0 或 >500 时按 60 处理。 */
	int32 Limit = 60;
	int32 Offset = 0;
};
