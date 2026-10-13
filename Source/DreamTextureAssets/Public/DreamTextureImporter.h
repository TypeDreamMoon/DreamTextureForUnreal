// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/TextureDefines.h"
#include "DreamTextureManifest.h"

class UObject;

/**
 * 一个通道在 UE 里怎么落。数据来自 manifest-v1.md 的「UE 导入映射」表。
 *
 * sRGB 以 manifest 的 colorspace 为准（srgb → 开，linear → 关），
 * 只有 colorspace 缺失时才退回 bDefaultSRGB。
 */
struct DREAMTEXTUREASSETS_API FDreamTextureChannelRule
{
	FString Channel;

	/** 资产名后缀：BC / N / ORM / H / R / M / AO。 */
	FString Suffix;

	TEnumAsByte<TextureCompressionSettings> Compression = TC_Default;
	TEnumAsByte<TextureGroup> LODGroup = TEXTUREGROUP_World;
	bool bDefaultSRGB = false;

	/** 未知通道返回 nullptr——导入器把它记成 Skipped，不报错。 */
	static const FDreamTextureChannelRule* Find(const FString& Channel);
};

/** 一套材质落成资产的请求。**只认本地文件，不碰网络**——所以能脱离后端测试。 */
struct DREAMTEXTUREASSETS_API FDreamTextureImportRequest
{
	FDreamTextureManifest Manifest;

	/** 通道 → 本地 PNG 绝对路径。不在这里的通道不动（可能是 UpToDate，也可能是缺）。 */
	TMap<FString, FString> LocalFiles;

	/** 内容根目录，如 /Game/DreamTexture。空 = UDreamTextureSettings::ImportRoot。 */
	FString ContentRoot;

	/** 落完是否保存包。自动化测试里可以关掉。 */
	bool bSave = true;
};

enum class EDreamTextureImportAction : uint8
{
	/** 新建了资产。 */
	Created,
	/** 资产已存在，就地更新了源数据。引用它的材质不受影响。 */
	Updated,
	/** 资产已存在且哈希一致，什么都没做。 */
	Unchanged,
	/** 没有处理：未知通道、文件缺失等。见 Note。 */
	Skipped,
	Failed,
};

struct DREAMTEXTUREASSETS_API FDreamTextureImportedTexture
{
	FString Channel;
	EDreamTextureImportAction Action = EDreamTextureImportAction::Skipped;

	/** 如 /Game/DreamTexture/MossyRiverRocks_1f53/T_MossyRiverRocks_1f53_BC.T_MossyRiverRocks_1f53_BC */
	FString ObjectPath;

	/** 中文说明，Skipped / Failed 时必有。 */
	FString Note;
};

struct DREAMTEXTUREASSETS_API FDreamTextureImportResult
{
	/** 没有任何通道 Failed。全部 Skipped 也算 false（什么都没导进来）。 */
	bool bOK = false;
	FString Error;

	/** 如 /Game/DreamTexture/MossyRiverRocks_1f53 */
	FString AssetFolder;

	TArray<FDreamTextureImportedTexture> Textures;

	/**
	 * 这套材质的材质实例 MI_<Slug>（M2），与贴图同目录，父材质是工程里的 M_DreamTexture_Master。
	 * Channel 固定为空。设置里关了自动建材质实例、或一张贴图都没有时 Action 为 Skipped，Note 说明原因。
	 */
	FDreamTextureImportedTexture MaterialInstance;

	/**
	 * 这套材质的许可（manifest.license_flags，原样带过来）。
	 * 不可商用的组合（CHORD 分解）要在导入完成的那一刻就让用户看到，别等东西用进项目里才发现。
	 * 导入失败时也会填（只要读到了 manifest），界面按需取用。
	 */
	FDreamTextureLicense License;
};

/** 一张贴图身上记着的来源。ReadSourceRecord 读出来的就是这个。 */
struct DREAMTEXTUREASSETS_API FDreamTextureSourceRecord
{
	/** 如 mat_20260827_092650_1f53。 */
	FString MaterialId;

	/** manifest.maps 的键，如 basecolor。 */
	FString Channel;

	/** 导入那一刻 manifest 里这个通道的 sha256（前 16 位十六进制）。 */
	FString Sha256;
};

/**
 * 把本地 PNG 落成 UTexture2D。
 *
 * - 路径：<ContentRoot>/<Slug>/T_<Slug>_<后缀>，Slug 由 MakeSlug 决定，只取决于 manifest 的 name 和 id，
 *   所以同一套材质每次都落在同一个地方。
 * - **重复导入是就地更新**：资产已存在就替换它的源数据，不新建 _1 后缀的资产——
 *   否则已经连好的材质会指向旧的那份。
 * - **法线不翻 G 通道**：manifest 的 y 是 directx（UE 原生）。只有 y 为 opengl 时才翻。
 * - 每张贴图上记下来源（材质 id、通道、sha256），供 FindImportedHashes 去重。
 *   这份记录必须是 editor-only 的：不许让 cook 出来的资产引用任何只存在于编辑器模块里的类。
 */
class DREAMTEXTUREASSETS_API FDreamTextureImporter
{
public:
	/** **必须在游戏线程调。** 同步执行。 */
	static FDreamTextureImportResult Import(const FDreamTextureImportRequest& Request);

	/**
	 * <PascalCase 名字>_<id 最后一段>，如 "mossy river rocks" + mat_20260827_092650_1f53 → MossyRiverRocks_1f53。
	 * 名字只保留 ASCII 字母数字，最长 32 个字符。
	 * 一个都不剩（比如全中文）时改用 id 去掉 mat_ 前缀后的整段：mat_20260827_092650_1f53 → Mat_20260827_092650_1f53。
	 * 只取尾段的话 4 位十六进制就是全部的区分度，同一批中文提示词的变体会撞同名；整段 id 保证唯一。
	 */
	static FString MakeSlug(const FDreamTextureManifest& Manifest);

	/**
	 * 归一后的内容根目录：去空白、反斜杠换正斜杠、去尾斜杠、补前导斜杠。
	 * 传空 = UDreamTextureSettings::ImportRoot；设置也是空再退回 /Game/DreamTexture。
	 * 母材质与材质实例都要和贴图落在同一个根下，所以这条规则对外公开，各处不要各写一份。
	 */
	static FString ResolveContentRoot(const FString& ContentRoot = FString());

	/** <ContentRoot>/<Slug>。ContentRoot 空 = UDreamTextureSettings::ImportRoot。 */
	static FString GetAssetFolder(const FDreamTextureManifest& Manifest, const FString& ContentRoot = FString());

	/** T_<Slug>_<后缀>。未知通道返回空串。 */
	static FString GetTextureAssetName(const FString& Slug, const FString& Channel);

	/**
	 * 这套材质在工程里已经导入的通道及其 sha256（读资产上记的来源）。
	 * 只认来源记录里的材质 id 与 Manifest.Id 一致的资产。只在游戏线程调。
	 *
	 * 来源记录存在包的元数据里，所以没加载的资产要先把包载进来才读得到。
	 */
	static TMap<FString, FString> FindImportedHashes(const FDreamTextureManifest& Manifest, const FString& ContentRoot = FString());

	/**
	 * 读一张贴图上记的来源。不是本插件导入的、或记录不全，返回 false。
	 *
	 * 记录存在包的元数据（UPackage::GetMetaData）里，只在带编辑器数据的构建里存在，
	 * cook 之后就没了——所以运行时代码不能依赖它，只有编辑器里的逻辑（去重、之后的材质槽赋值）用。
	 */
	static bool ReadSourceRecord(const UObject* Texture, FDreamTextureSourceRecord& OutRecord);
};
