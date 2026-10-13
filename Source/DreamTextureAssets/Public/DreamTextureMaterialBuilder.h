// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DreamTextureImporter.h"

class UMaterial;
class UMaterialInstanceConstant;

/**
 * 母材质 M_DreamTexture_Master 的参数名。
 *
 * **改名等于让所有已有的材质实例丢参数。** 只能加，不能改、不能删。
 */
namespace DreamTextureMaterialParam
{
	// 贴图
	inline const FName BaseColor = TEXT("BaseColor");
	inline const FName Normal = TEXT("Normal");
	/** R=AO、G=Roughness、B=Metallic，与 manifest 的 orm.packing 一致。 */
	inline const FName ORM = TEXT("ORM");
	inline const FName Height = TEXT("Height");

	// 静态开关：对应贴图缺失时，实例把它关掉，走标量兜底，而不是去采一张默认贴图冒充。
	inline const FName UseNormal = TEXT("UseNormal");
	inline const FName UseORM = TEXT("UseORM");
	/** 视差遮蔽。贵，默认关，用户在实例上自己打开。 */
	inline const FName UsePOM = TEXT("UsePOM");

	// 标量 / 向量
	inline const FName UVTiling = TEXT("UVTiling");
	inline const FName BaseColorTint = TEXT("BaseColorTint");
	inline const FName NormalStrength = TEXT("NormalStrength");
	inline const FName RoughnessMin = TEXT("RoughnessMin");
	inline const FName RoughnessMax = TEXT("RoughnessMax");
	inline const FName AOStrength = TEXT("AOStrength");
	inline const FName HeightRatio = TEXT("HeightRatio");
	/** UseORM 关掉时的兜底值。 */
	inline const FName Roughness = TEXT("Roughness");
	inline const FName Metallic = TEXT("Metallic");
}

/**
 * 插件的母材质。**在代码里生成，落在工程的 ImportRoot 下，不放在插件的 Content 里。**
 *
 * 插件是 editor-only 的：材质实例若引用插件内容，打包或禁用插件就断链。生成到 /Game 里，
 * 产物就完全独立于插件——这是 M2 验收「禁用插件后重开工程，材质仍然正常」的根据。
 * 顺带让开源仓库里没有二进制资产，母材质的每一根连线都在代码里看得见、改得了。
 *
 * 用的是传统材质，不写 Substrate 版本：项目开了 Substrate 时引擎会自动转换（测试宿主也开着）。
 */
class DREAMTEXTUREASSETS_API FDreamTextureMasterMaterial
{
public:
	/**
	 * 图的版本。图结构变了就 +1。工程里那份版本低于它时**就地重建同一个资产**——
	 * 资产不换、参数名不变，已有的材质实例就不会断。版本记在包元数据里（与贴图的来源记录同一套做法）。
	 */
	static constexpr int32 GraphVersion = 1;

	/** M_DreamTexture_Master */
	static const TCHAR* GetAssetName();

	/** <ContentRoot>/M_DreamTexture_Master.M_DreamTexture_Master。ContentRoot 空 = UDreamTextureSettings::ImportRoot。 */
	static FString GetObjectPath(const FString& ContentRoot = FString());

	/**
	 * 找到工程里那份；没有就生成，版本旧了就就地重建。**游戏线程。**
	 * 失败返回 nullptr，OutError 给中文原因。
	 */
	static UMaterial* FindOrCreate(const FString& ContentRoot, bool bSave, FString& OutError);

	/**
	 * 这份母材质记着的图版本；没有记录（或不是带编辑器数据的构建）返回 0。
	 * 记在包元数据里，与贴图的来源记录同一套做法，cook 时不会带出去。
	 */
	static int32 ReadRecordedVersion(const UObject* Material);

	/**
	 * 这个材质上有没有版本记录。**没有记录 = 不是本插件生成的**（插件从来没发布过不带记录的版本），
	 * FindOrCreate 对这种材质按「路径被占用」处理，绝不清空重建——那是用户自己的材质。
	 * 注意区分：记录的值是 0 也是「有记录」，只是个比所有发布版都旧的版本。
	 */
	static bool HasRecordedVersion(const UObject* Material);

	/** 改写记录的版本。自动化测试拿它模拟「工程里的母材质是旧版本插件生成的」。 */
	static void WriteRecordedVersion(UObject* Material, int32 Version);

	/**
	 * **仅供自动化测试。** 大于等于 0 时，生成图建到第 N 个节点就人为失败，用来验证
	 * 「就地重建中途失败，母材质回到重建前的样子」。-1 = 关（默认）。生产代码绝不要碰它。
	 */
	static int32 DebugFailAfterNodeCount;
};

/** 材质实例 MI_<Slug> 的建与更新。 */
class DREAMTEXTUREASSETS_API FDreamTextureMaterialBuilder
{
public:
	/** MI_<Slug> */
	static FString GetInstanceAssetName(const FString& Slug);

	/**
	 * 给一套已经导进来的贴图建（或就地更新）材质实例，放在贴图同目录。**游戏线程。**
	 *
	 * 贴图从 Textures 里各通道的 ObjectPath 取（Created / Updated / Unchanged 的都算）；
	 * 缺哪张就把对应的静态开关关掉。已有实例只改插件管的那几个参数，用户在实例上调过的其它参数不动。
	 * 返回值直接填进 FDreamTextureImportResult::MaterialInstance。
	 */
	static FDreamTextureImportedTexture CreateOrUpdateInstance(const FDreamTextureManifest& Manifest, const FDreamTextureImportResult& Textures, const FString& ContentRoot, bool bSave);

	/**
	 * 这个材质实例上记着的、它是为哪套材质（manifest id）建的；没有记录返回空串。
	 *
	 * 新建实例时写在包元数据里（与贴图的来源记录同一套做法，cook 时不会带出去）。更新已有实例前要核对：
	 * 记录是别的材质、或没有记录（用户自己的同名实例），一律不动——只改自己建的东西。
	 */
	static FString ReadInstanceMaterialId(const UObject* Instance);
};
