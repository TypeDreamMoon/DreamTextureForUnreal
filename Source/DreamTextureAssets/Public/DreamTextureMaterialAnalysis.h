// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

class AActor;
class UMaterialInterface;
class UPrimitiveComponent;
class UTexture;

/**
 * 材质回流（M7）：选一个工程里已有的材质，解析出它用到哪些贴图、每张是什么通道。
 *
 * 这个头定义解析结果的结构。入口是 FDreamTextureMaterialAnalyzer::Analyze；
 * 导出 PNG 在 DreamTextureTextureExporter.h。
 */

/** 贴图在材质里是什么。前七个与 manifest 的通道（DreamTextureChannel）对应。 */
enum class EDreamTextureRole : uint8
{
	/** 没认出来。UI 上要请用户指认。 */
	Unknown,
	BaseColor,
	Normal,
	Roughness,
	Metallic,
	AO,
	Height,
	/** 打包图（ORM / MRA / RMA …）。各通道是什么看 FDreamTextureMaterialTexture::Layout。 */
	Packed,
	/** 以下几种材质里有、但 DreamTexture 后端没有对应通道的；认出来是为了不把它们误当成别的。 */
	Emissive,
	Opacity,
	Specular,
	/** 不是 PBR 数据（混合遮罩、噪声、流向图…），或用户指明「这张不管」。 */
	Other,
};

/**
 * 稳定的英文标识：unknown / basecolor / normal / roughness / metallic / ao / height / packed / emissive / opacity / specular / other。
 * 前几个与 manifest 的通道名一致。存盘（用户覆盖）、日志、测试断言都用它；界面上的中文由 Editor 模块另配。
 */
DREAMTEXTUREASSETS_API const TCHAR* LexToString(EDreamTextureRole Role);

/** LexToString 的反向。认不出返回 false（OutRole 不动）。大小写不敏感。 */
DREAMTEXTUREASSETS_API bool LexTryParseString(EDreamTextureRole& OutRole, const FString& Text);

/**
 * 结论的依据，由准到不准排列（数值小的优先）。每张贴图只记最终采用的那一条，
 * 其余落选的依据如果与结论冲突，写进 Notes。
 */
enum class EDreamTextureEvidence : uint8
{
	/** 没有任何依据。 */
	None,
	/** 用户手动指认过（按材质资产路径记在 Saved/DreamTexture 下）。 */
	UserOverride,
	/** 本插件导入的贴图：包元数据里的来源记录，通道是导入时写下的，最准。 */
	SourceRecord,
	/** 材质表达式图：这张贴图连到了哪个材质属性、用的是哪几个通道。 */
	MaterialGraph,
	/** 贴图参数的名字（BaseColor / Albedo / Normal / ORM …）。 */
	ParameterName,
	/** 贴图资产的名字（T_xxx_BC / _N / _ORM …）。比参数名更容易被改名搞错，所以排在后面。 */
	AssetName,
	/** 采样器类型（Normal 采样器 → 法线）或贴图自己的压缩设置（TC_Normalmap）。 */
	SamplerType,
};

DREAMTEXTUREASSETS_API const TCHAR* LexToString(EDreamTextureEvidence Evidence);

/** 贴图通道位。 */
namespace DreamTextureChannelBit
{
	constexpr uint8 R = 1;
	constexpr uint8 G = 2;
	constexpr uint8 B = 4;
	constexpr uint8 A = 8;
	constexpr uint8 RGB = R | G | B;
	constexpr uint8 RGBA = R | G | B | A;
}

/** 材质里读这张贴图的位置（落点）。 */
enum class EDreamTextureSink : uint8
{
	BaseColor,
	Metallic,
	Specular,
	Roughness,
	Anisotropy,
	Emissive,
	Opacity,
	OpacityMask,
	Normal,
	Tangent,
	AmbientOcclusion,
	Subsurface,
	Refraction,
	/** MP_Displacement。 */
	Displacement,
	WorldPositionOffset,
	/** 经 UV（采样坐标）间接用到：视差、UV 扭曲。 */
	UVOffset,
	/** 用作两路数据之间的混合权重（Lerp 的 Alpha，且两路本身也是贴图）。 */
	BlendMask,
	/** 其余的材质属性，或走到了解析不了的节点。 */
	Other,
};

DREAMTEXTUREASSETS_API const TCHAR* LexToString(EDreamTextureSink Sink);

/** 材质图里的一处使用：哪个属性、读了贴图的哪几个通道。 */
struct DREAMTEXTUREASSETS_API FDreamTextureTextureUse
{
	EDreamTextureSink Sink = EDreamTextureSink::Other;

	/** DreamTextureChannelBit 的或。 */
	uint8 Channels = 0;

	/** 采样节点是贴图参数时的参数名；直接引用贴图的采样节点为 None。 */
	FName Parameter;

	/**
	 * 图遍历走到了没有专门处理的节点（自定义 HLSL、层堆栈…），沿用了前面的落点，通道是估的。
	 * 为真时这条使用只作旁证，不拿它给打包图排通道。
	 */
	bool bApproximate = false;
};

/** 一张打包图里 R/G/B/A 各自存的是什么。下标 0..3 对应 R G B A。 */
struct DREAMTEXTUREASSETS_API FDreamTextureChannelLayout
{
	EDreamTextureRole Channels[4] = { EDreamTextureRole::Unknown, EDreamTextureRole::Unknown, EDreamTextureRole::Unknown, EDreamTextureRole::Unknown };

	/** R/G/B 里有没有认不出来的（A 不算：很多打包图根本没有 A）。 */
	bool HasUnknownRGB() const;

	bool operator==(const FDreamTextureChannelLayout& Other) const;

	/** "R=ao,G=roughness,B=metallic,A=unknown"，与 manifest 的 packing 写法相近。 */
	FString ToString() const;

	/** 标准的 ORM：R=AO，G=Roughness，B=Metallic。 */
	static FDreamTextureChannelLayout MakeORM();
};

/** 工程里一张被材质用到的贴图及对它的判定。 */
struct DREAMTEXTUREASSETS_API FDreamTextureMaterialTexture
{
	TWeakObjectPtr<UTexture> Texture;

	/** 如 /Game/DreamTexture/Rock_1f53/T_Rock_1f53_BC.T_Rock_1f53_BC。用户覆盖按它记。 */
	FString TexturePath;
	FString AssetName;

	/** 源数据的尺寸；没有源数据为 0。 */
	int32 SourceWidth = 0;
	int32 SourceHeight = 0;

	/** 绑它的贴图参数名（可能不止一个）；全是直接引用贴图的采样节点时为空。 */
	TArray<FName> Parameters;

	/** 这张图是被所选材质实例自己覆盖的值（true），还是从父级/母材质继承来的（false）。材质（非实例）恒为 false。 */
	bool bOverriddenBySelectedInstance = false;

	/** 材质图里的全部使用处（只含从材质输出走得到的）。 */
	TArray<FDreamTextureTextureUse> Uses;

	//~ 结论
	EDreamTextureRole Role = EDreamTextureRole::Unknown;

	/** Role 为 Packed 时有意义。 */
	FDreamTextureChannelLayout Layout;

	EDreamTextureEvidence Evidence = EDreamTextureEvidence::None;

	/** 依据的原话（中文，可直接显示）：来源记录里写了什么、连到了哪个属性、匹配了哪个词。 */
	FString EvidenceDetail;

	/** 与结论冲突的旁证、猜测的提示等（中文）。 */
	TArray<FString> Notes;

	/**
	 * 需要用户指认：没认出来，或打包图推不出全部通道。
	 * 用户覆盖之后为 false（除非他自己还留着 Unknown）。
	 */
	bool bNeedsUserInput = false;

	//~ 导出
	bool bExportable = false;
	/** 不能导出时的原因（中文）。 */
	FString ExportBlocker;

	/**
	 * 直接对应的 manifest 通道名：basecolor / normal / roughness / metallic / ao / height；
	 * Packed 且排布恰为标准 ORM 时给 "orm"。其余（Unknown、其它排布、Emissive…）返回空串。
	 */
	FString GetManifestChannel() const;

	bool HasParameter(const FName& Name) const { return Parameters.Contains(Name); }
};

/** 一次解析的结果。 */
struct DREAMTEXTUREASSETS_API FDreamTextureMaterialAnalysis
{
	/** false 时 Error 给中文原因，Textures 为空。材质里一张贴图都没有不算失败。 */
	bool bOK = false;
	FString Error;

	TWeakObjectPtr<UMaterialInterface> Material;

	/** 被解析对象自己的对象路径。 */
	FString MaterialPath;

	/**
	 * 用户覆盖按它记。通常等于 MaterialPath；材质实例动态体（运行时才有、不是资产）退到最近的资产父级。
	 */
	FString AssetPath;

	/** 给人看的名字。 */
	FString DisplayName;

	bool bIsInstance = false;

	/** 母材质的对象路径（实例沿父链走到根）。 */
	FString BaseMaterialPath;

	/** 每张贴图一行（同一张贴图被多个参数/采样点用到，合并成一行）。顺序稳定：按首次在图里出现的顺序。 */
	TArray<FDreamTextureMaterialTexture> Textures;

	/** 解析里遇到的、可能让结果不全的情况：层堆栈、解析不了的节点…（中文） */
	TArray<FString> Warnings;

	const FDreamTextureMaterialTexture* FindTexture(const FString& TexturePath) const;
	FDreamTextureMaterialTexture* FindTexture(const FString& TexturePath);
};

/** Actor 上的一个材质槽。 */
struct DREAMTEXTUREASSETS_API FDreamTextureMaterialSlot
{
	TWeakObjectPtr<UPrimitiveComponent> Component;
	int32 SlotIndex = 0;
	FName SlotName;
	TWeakObjectPtr<UMaterialInterface> Material;

	/** "SM_Rock · 槽 0（Rock）· MI_Rock"，给列表显示。 */
	FString Label;
};

/**
 * 这个材质的贴图是平铺的，还是 UV 展开的（一张图对应一块网格的展开：角色、道具、trim sheet）。
 * 回流（M8/M9）据此决定要不要让后端做无缝处理、分解模型默认选哪个、重绘幅度的上限。
 * 材质自己看不出来，只能用户说，所以按材质资产路径记住（见 FDreamTextureOverrideStore）。
 */
enum class EDreamTextureUVKind : uint8
{
	/** 无缝平铺的素材（默认）。 */
	Tiled,
	/** UV 展开的贴图：边缘不首尾相接，不能做无缝处理。 */
	Unwrapped,
};

/** 稳定的英文标识：tiled / unwrapped。存盘用。 */
DREAMTEXTUREASSETS_API const TCHAR* LexToString(EDreamTextureUVKind Kind);
DREAMTEXTUREASSETS_API bool LexTryParseString(EDreamTextureUVKind& OutKind, const FString& Text);

/** 用户对一张贴图的指认。 */
struct DREAMTEXTUREASSETS_API FDreamTextureRoleOverride
{
	EDreamTextureRole Role = EDreamTextureRole::Unknown;

	/** Role 为 Packed 时有意义。 */
	FDreamTextureChannelLayout Layout;
};

/**
 * 用户的指认，按「材质资产路径 + 贴图路径」记在工程的 Saved/DreamTexture/ReflowOverrides.json 里。
 *
 * 为什么存在 Saved 下而不写进用户的材质包：写包会把用户的资产弄脏（要重新存盘、版本控制里多一个改动），
 * 而这只是插件自己的记忆。代价是换一台机器、删了 Saved 就丢——可以接受，再指认一次就是了。
 *
 * 文件在第一次读写时才读入；写盘失败不抛错，GetLastError 给原因，已有的内存状态仍然有效。
 * **文件在但读不出来**（被别的进程独占、权限、磁盘错误）时不当成「没有记录」用：不标成已加载，
 * 下一次访问重新读盘，期间所有写操作（Set / SetUVKind）一律拒绝并给原因——否则内存里是空的，
 * 一次写盘就把用户攒下的全部指认覆盖掉。（文件内容不是有效 JSON 是另一回事：当空的用，保存时覆盖。）
 * 只在游戏线程用。
 */
class DREAMTEXTUREASSETS_API FDreamTextureOverrideStore
{
public:
	/** 默认库：<工程 Saved>/DreamTexture/ReflowOverrides.json。 */
	static FDreamTextureOverrideStore& Get();

	static FString GetDefaultFilePath();

	/** 自动化测试用：指到临时文件。 */
	explicit FDreamTextureOverrideStore(const FString& InFilePath);

	const FString& GetFilePath() const { return FilePath; }

	bool Find(const FString& MaterialAssetPath, const FString& TexturePath, FDreamTextureRoleOverride& OutOverride) const;

	/** 记下并立刻写盘。返回写盘是否成功；内存里总是已经记下。文件读不出来时拒绝写，返回 false，GetLastError 说明。 */
	bool Set(const FString& MaterialAssetPath, const FString& TexturePath, const FDreamTextureRoleOverride& Override);

	/** 撤销一条。没有这一条返回 false。 */
	bool Remove(const FString& MaterialAssetPath, const FString& TexturePath);

	/** 这个材质下有几条。 */
	int32 NumFor(const FString& MaterialAssetPath) const;

	//~ 平铺 / UV 展开（回流 M9）：同一个文件里的另一类记忆，同样按材质资产路径记，同样不写进用户的包。
	/** 没记过返回 false（OutKind 不动）——调用方按平铺处理。 */
	bool FindUVKind(const FString& MaterialAssetPath, EDreamTextureUVKind& OutKind) const;

	/** 记下并立刻写盘。选回默认的「平铺」时不留条目（没有记录就是平铺），免得文件里堆满默认值。返回写盘是否成功。 */
	bool SetUVKind(const FString& MaterialAssetPath, EDreamTextureUVKind Kind);

	/** 丢掉内存状态，下次访问重新读盘（测试里模拟「重开编辑器」）。 */
	void Reload();

	/** 最近一次读/写盘失败的原因；成功后清空。 */
	const FString& GetLastError() const { return LastError; }

private:
	void EnsureLoaded() const;
	bool Save();

	FString FilePath;
	mutable bool bLoaded = false;
	mutable FString LastError;
	/** 材质资产路径 → 贴图路径 → 指认。 */
	mutable TMap<FString, TMap<FString, FDreamTextureRoleOverride>> Data;

	/** 材质资产路径 → 该材质的贴图是 UV 展开的。只存非默认值。 */
	mutable TMap<FString, EDreamTextureUVKind> UVKinds;
};

struct FDreamTextureAnalyzeOptions
{
	/** 用户指认的库。nullptr = FDreamTextureOverrideStore::Get()。 */
	FDreamTextureOverrideStore* Overrides = nullptr;

	/** false 时不应用用户指认（想看「纯推断」是什么时用）。 */
	bool bApplyOverrides = true;
};

/** 参数名/资产名匹配的结果。公开是为了让测试直接验证词表。 */
struct DREAMTEXTUREASSETS_API FDreamTextureNameMatch
{
	EDreamTextureRole Role = EDreamTextureRole::Unknown;

	/** Role 为 Packed 时的排布；词表给不出排布（如 Packed、MaskMap）时 Unknown 的通道保持 Unknown。 */
	FDreamTextureChannelLayout Layout;

	/** 命中的词（原样，如 "ORM"）。 */
	FString Keyword;
};

class DREAMTEXTUREASSETS_API FDreamTextureMaterialAnalyzer
{
public:
	/**
	 * 解析一个材质或材质实例。**游戏线程。**
	 *
	 * 材质实例沿父链找到根材质的图，再用实例（及中间各层）覆盖的贴图参数值替换。
	 * 不编译材质、不碰着色器：-nullrhi 的自动化里也能跑。
	 */
	static FDreamTextureMaterialAnalysis Analyze(UMaterialInterface* Material, const FDreamTextureAnalyzeOptions& Options = FDreamTextureAnalyzeOptions());

	/** 用户覆盖的键：材质对象的资产路径；运行时才有的动态实例退到最近的资产父级。 */
	static FString GetAssetPathForOverrides(const UMaterialInterface* Material);

	/** Actor 上所有网格组件的材质槽。没有材质的槽也列出来（Material 为空），让界面能说清「这个槽是空的」。 */
	static TArray<FDreamTextureMaterialSlot> GetActorSlots(AActor* Actor);

	/** 按名字猜通道：贴图参数名或资产名。大小写、下划线不敏感；认不出返回 false。 */
	static bool MatchName(const FString& Name, FDreamTextureNameMatch& OutMatch);

	/** 把名字切成小写词（驼峰、下划线、数字边界都切）。公开给测试。 */
	static TArray<FString> TokenizeName(const FString& Name);
};
