// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DreamTextureImportQueue.h"
#include "DreamTextureMaterialAnalysis.h"

class UMaterialInstanceConstant;
class UMaterialInterface;

/**
 * 材质回流（M8 平铺路径 / M9 UV 展开路径）里「材质这一头」的逻辑：
 * 解析出的贴图对应哪些通道、用户勾选之后要导入什么、原地覆盖时改哪些参数。
 *
 * 不碰网络、不碰界面：后端怎么调、页面怎么摆，是 Editor 模块的事（DreamTextureReflowRun / Actions）。
 * 这里只回答三个问题——
 *
 *   1. 这个材质里，六个通道哪些「已有」、哪些「缺」？（Describe）
 *   2. 用户勾了这几个通道，**新建**一套材质实例该导入什么、哪些沿用用户原来的贴图？（PlanCreate）
 *   3. 同样的勾选，**原地覆盖**选中的材质实例该改它的哪些参数？（PlanInPlace / ApplyInPlace）
 */

/** 回流的两种用法，与后端 derived_from.mode 同名同义。 */
enum class EDreamTextureReflowMode : uint8
{
	/** 补齐缺失通道：原图原样进分解段，画面不变。 */
	FillChannels,

	/** 改写外观：原图当参考图，配提示词与重绘幅度走 img2img，再整套分解。 */
	Restyle,
};

/** fill_channels / restyle，即 derived_from.mode 的取值。 */
DREAMTEXTUREASSETS_API const TCHAR* LexToString(EDreamTextureReflowMode Mode);

namespace DreamTextureReflow
{
	/**
	 * UV 展开的贴图，改写外观时重绘幅度的硬上限。
	 *
	 * 重绘会改变画面的内容布局；UV 展开图上每一块内容都对应网格上的一块面，布局一变，
	 * 贴回模型就全乱了（接缝对不上、眼睛长到别处）。0.35 以内后端文档的说法是「几乎照抄原图，
	 * 只做细节翻新」（docs/reference-image.md 的档位表），是能保住对应关系的上限。
	 */
	constexpr double MaxUnwrappedDenoise = 0.35;

	/** 后端 denoise 参数声明的下限（workflows/src-local-*.params.json）。 */
	constexpr double MinDenoise = 0.1;

	/** 平铺素材改写外观的默认重绘幅度：「保留原图结构，换材质表现」那一档。 */
	constexpr double DefaultTiledDenoise = 0.55;

	/** UV 展开的默认重绘幅度：在上限以内取中间偏小的一档。 */
	constexpr double DefaultUnwrappedDenoise = 0.30;

	/**
	 * 导出底色图时长边的默认上限。后端会把上传的图居中裁切缩放到 768/1024 的工作分辨率，
	 * 送 4K、8K 上去没有任何好处，只会撞上 32 MB 的上传上限。
	 */
	constexpr int32 DefaultExportMaxEdge = 2048;

	/** 能回流的通道，按界面上的顺序：basecolor normal roughness metallic ao height。名字与 manifest 一致。 */
	DREAMTEXTUREASSETS_API const TArray<FString>& GetChannels();

	/** 通道的中文名：底色 / 法线 / 粗糙度 / 金属度 / 环境光遮蔽 / 高度。不认识的原样返回。 */
	DREAMTEXTUREASSETS_API FString GetChannelLabel(const FString& Channel);

	/** 重绘幅度在这个平铺方式下是否被允许。平铺不设上限；UV 展开不得超过 MaxUnwrappedDenoise。 */
	DREAMTEXTUREASSETS_API bool IsDenoiseAllowed(EDreamTextureUVKind Kind, double Denoise);

	/** 重绘幅度超限时对用户说的话（人话：重绘会破坏 UV 对应关系，贴回模型就全乱了）。 */
	DREAMTEXTUREASSETS_API FString DescribeDenoiseLimit(double Denoise);
}

/** 一个通道在选中材质里的现状。 */
struct DREAMTEXTUREASSETS_API FDreamTextureReflowChannelState
{
	enum class EPresence : uint8
	{
		/** 材质里没有这个通道的贴图。 */
		Missing,

		/** 有一张专门的贴图（BaseColor / Normal / Roughness …）。 */
		Dedicated,

		/** 在一张标准 ORM（R=AO G=Roughness B=Metallic）里。 */
		InStandardORM,

		/** 在一张排布不是标准 ORM 的打包图里（RMA、MRA、带高度的 ORMH …）。 */
		InOtherPacked,
	};

	/** manifest 通道名。 */
	FString Channel;

	EPresence Presence = EPresence::Missing;

	/** 持有它的贴图。Missing 时为空。 */
	FString TexturePath;
	FString TextureName;

	/** 绑它的贴图参数；空 = 贴图直接写在母材质的图里，实例改不了。 */
	TArray<FName> Parameters;

	/**
	 * 与它同在一张贴图里的通道（含它自己）。独立贴图只有自己。
	 * 打包图里的通道必须一起处理：整张换，不能只换其中一路。
	 */
	TArray<FString> TiedChannels;

	/** 解析结果里扮演这个通道的贴图有几张。≥2 时只对第一张动手，Note 里说明。 */
	int32 NumCandidates = 0;

	/**
	 * 不是从解析结果里认出来的，而是在插件自己的材质实例上直接看参数得知的。
	 * 例：插件建的 MI_<名字> 默认 UsePOM 关，Height 贴图所在的分支不生效，解析器（只走生效的分支）看不到它，
	 * 但它实实在在挂在实例的 Height 参数上，是用户的东西，不能当缺失去覆盖。
	 */
	bool bFromPluginParameter = false;

	/** 值得告诉用户的附注（中文）。 */
	FString Note;

	bool IsPresent() const { return Presence != EPresence::Missing; }
};

/** 新建一套材质实例的计划。 */
struct DREAMTEXTUREASSETS_API FDreamTextureReflowCreatePlan
{
	/** false 时 Error 是中文原因（一个通道都没勾……）。 */
	bool bOK = false;
	FString Error;

	/** 交给导入队列：导入哪些通道、哪些沿用工程里已有的贴图、不要有多余的东西。 */
	FDreamTextureImportOptions Options;

	/** 没法带到新实例里的用户原有贴图等，给用户看的说明。 */
	TArray<FString> Notes;
};

/** 原地覆盖要改的一处：把一张贴图（一个或几个参数）换成后端生成的某个通道。 */
struct DREAMTEXTUREASSETS_API FDreamTextureReflowOverwrite
{
	/** 用后端的哪个通道换：basecolor / normal / height / roughness / metallic / ao / orm。 */
	FString ImportChannel;

	/** 这一处覆盖了用户勾的哪些通道（orm 覆盖 ao+roughness+metallic）。 */
	TArray<FString> Covers;

	/** 要改的参数名。 */
	TArray<FName> Parameters;

	/** 被换掉的那张贴图，给说明用。 */
	FString OldTexturePath;
};

/** 原地覆盖的计划。 */
struct DREAMTEXTUREASSETS_API FDreamTextureReflowInPlacePlan
{
	bool bOK = false;
	FString Error;

	/** 交给导入队列：只导入要换的那几个通道，不新建材质实例。 */
	FDreamTextureImportOptions Options;

	TArray<FDreamTextureReflowOverwrite> Items;

	/** 勾了却改不了的通道，每条带原因（中文）。 */
	TArray<FString> Skipped;
};

/** 原地覆盖执行的结果。 */
struct DREAMTEXTUREASSETS_API FDreamTextureReflowApplyResult
{
	bool bOK = false;
	FString Error;

	/** 改了的参数，如「Normal → T_Rock_1f53_N」。 */
	TArray<FString> Changed;

	/** 没改的，带原因。 */
	TArray<FString> Skipped;

	/** 是否写进了撤销栈（没有编辑器时为假；什么都没改时也为假）。 */
	bool bTransacted = false;
};

class DREAMTEXTUREASSETS_API FDreamTextureReflowPlanner
{
public:
	/**
	 * 六个通道的现状，顺序同 DreamTextureReflow::GetChannels()。**游戏线程**（要读材质实例的参数）。
	 *
	 * Material 可以是空（只看解析结果）。给了的话，对「根材质是插件母材质」的实例，
	 * 额外把四个插件管的贴图参数（BaseColor / Normal / ORM / Height）上实际挂着的用户贴图补进来——
	 * 见 FDreamTextureReflowChannelState::bFromPluginParameter。引擎自带的默认贴图不算（那是占位）。
	 * 对用户自己的材质不做这一步：他们的参数名怎么叫、默认值是什么，我们无从知道。
	 */
	static TArray<FDreamTextureReflowChannelState> Describe(const FDreamTextureMaterialAnalysis& Analysis, const UMaterialInterface* Material);

	/** 解析结果里能当「底图」送上去的候选：角色是底色、且能导出的，按解析顺序。 */
	static TArray<const FDreamTextureMaterialTexture*> FindBaseColorSources(const FDreamTextureMaterialAnalysis& Analysis);

	/** 补通道模式下底色不重新生成，永远沿用原图：它不可勾选。 */
	static bool IsLocked(const FString& Channel, EDreamTextureReflowMode Mode);

	/**
	 * 默认勾选：缺的勾上，已有的不勾（默认不动用户的东西）。
	 * 改写外观时底色一定要勾——不重画它就谈不上改写。已有的其余通道仍然不勾。
	 */
	static TSet<FString> DefaultSelection(const TArray<FDreamTextureReflowChannelState>& States, EDreamTextureReflowMode Mode);

	/** 把任意一个勾选规整成合法的：同一张打包图里的通道一起勾或一起不勾，被锁住的通道去掉，不认识的通道去掉。 */
	static TSet<FString> Normalize(const TArray<FDreamTextureReflowChannelState>& States, const TSet<FString>& Selection, EDreamTextureReflowMode Mode);

	/** 勾上或取消一个通道，同组的跟着动。返回规整后的勾选。 */
	static TSet<FString> Toggle(const TArray<FDreamTextureReflowChannelState>& States, const TSet<FString>& Selection, EDreamTextureReflowMode Mode, const FString& Channel, bool bOn);

	/**
	 * 新建：新贴图 + 新的 MI_<名字>，原材质一根线都不动。
	 *
	 * @param SourceBaseColorPath 送上去当底图的那张贴图的对象路径。补通道时它就是新实例的 BaseColor。
	 */
	static FDreamTextureReflowCreatePlan PlanCreate(const TArray<FDreamTextureReflowChannelState>& States, const TSet<FString>& Selection,
		EDreamTextureReflowMode Mode, const FString& SourceBaseColorPath);

	/**
	 * 原地覆盖这件事对这个材质成不成立。成立要求是工程里的材质实例资产——母材质里贴图硬连在图上，
	 * 没有参数可改；运行时才有的动态实例也不是资产。不成立时 OutReason 是中文原因。
	 */
	static bool CanOverwriteInPlace(const FDreamTextureMaterialAnalysis& Analysis, const UMaterialInterface* Material, FString& OutReason);

	/** 这个通道在原地覆盖下改不改得了。改不了时 OutReason 是中文原因。 */
	static bool CanOverwriteChannel(const FDreamTextureReflowChannelState& State, FString& OutReason);

	/** 原地覆盖：只改勾选的通道，按选中材质自己的参数名。 */
	static FDreamTextureReflowInPlacePlan PlanInPlace(const TArray<FDreamTextureReflowChannelState>& States, const TSet<FString>& Selection, EDreamTextureReflowMode Mode);

	/**
	 * 执行原地覆盖。**游戏线程。** 整个改动一个 FScopedTransaction，可撤销；什么都没改时事务被取消，撤销栈里不留空条目。
	 *
	 * 新贴图从 Imported（导入队列的结果）里按 ImportChannel 找。每个参数改之前核对采样器类型——
	 * 贴图的压缩设置与参数的采样器不一致，材质会编译报错——不一致的那个参数不改，原因记进 Skipped。
	 */
	static FDreamTextureReflowApplyResult ApplyInPlace(UMaterialInstanceConstant* Instance, const TArray<FDreamTextureReflowOverwrite>& Items,
		const FDreamTextureImportResult& Imported, bool bSave);
};
