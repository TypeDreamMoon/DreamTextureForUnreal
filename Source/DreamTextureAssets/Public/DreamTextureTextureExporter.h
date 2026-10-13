// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DreamTextureMaterialAnalysis.h"

class UTexture;

/** 一张贴图导出的结果。 */
struct DREAMTEXTUREASSETS_API FDreamTextureExportedImage
{
	bool bExported = false;

	/** 对象路径，与 FDreamTextureMaterialTexture::TexturePath 一致，用来对回解析结果里的行。 */
	FString TexturePath;

	/** 写出的 PNG 的绝对路径。 */
	FString FilePath;

	/**
	 * 写出的 PNG 文件**全部字节**的 SHA-256，小写十六进制取前 16 位（FDreamTextureFetcher::HashBytes16，
	 * 与后端 manifest 的 sha256 同一算法）。M8 写进 manifest 的 derived_from 用它。
	 */
	FString Sha256;

	/** 写出的尺寸（缩过的话是缩后的）。 */
	int32 Width = 0;
	int32 Height = 0;

	/** 源数据的尺寸。 */
	int32 SourceWidth = 0;
	int32 SourceHeight = 0;

	bool bResized = false;

	/**
	 * 这张贴图勾了 Flip Green Channel，导出时已经把 G 通道翻过（见类注释「Flip Green Channel」）。
	 * 调用方据此知道：写出的 PNG 与源数据在 G 通道上互为反相，与 UE 里实际用的一致（DirectX 约定）。
	 * 灰度格式（G8 / G16）没有绿通道，勾了也不翻，此时为假。
	 */
	bool bFlippedGreen = false;

	/** 源数据格式：G8 / G16 / BGRA8 / RGBA16。PNG 保持同样的位深与通道数。 */
	FString SourceFormat;

	/** 位深：8 或 16。 */
	int32 BitsPerChannel = 0;

	/** 通道数：1（灰度）或 4（彩色带 Alpha）。 */
	int32 NumChannels = 0;

	/**
	 * 贴图的 sRGB 开关。PNG 里不写任何色彩空间信息——像素就是源数据的字节，
	 * 开着的（底色）是 sRGB 编码，关着的（法线、ORM…）是线性。调用方据此记 colorspace。
	 */
	bool bSRGB = false;

	/** 没导出时的原因（中文）。 */
	FString Reason;
};

struct FDreamTextureExportOptions
{
	/** 输出目录。空 = MakeTempDir() 新建一个。 */
	FString OutputDir;

	/** 长边缩到不超过这个值（保持比例）。≤0 或源图没这么大 = 不缩。 */
	int32 MaxEdge = 0;
};

/**
 * 把工程里的贴图导出成 PNG（M7）。
 *
 * ## 只从源数据导，不退化
 *
 * 数据取自 UTexture::Source——导入进来的原始像素。**取不到源数据就明确报「导不出」，不会退化成
 * 平台数据里解压出来的 mip0**：那是 BC 块压缩之后的结果，拿它去做分解，块状伪影会被放大进法线。
 * 不能导的情形：不是 2D 贴图、资产里没有源数据（程序生成、源被剥掉、运行时虚拟纹理）、
 * UDIM 多块/多层、浮点 HDR 格式。有源数据的虚拟纹理（VT）资产可以导——它的 Source 与普通贴图无异。
 *
 * ## Flip Green Channel
 *
 * 引擎只在**构建派生数据**（压缩出平台数据）时才应用贴图上的 bFlipGreenChannel，翻的是 G = 1 - G，
 * **不改 Source**。导入进来的 OpenGL 法线（manifest 的 y 为 opengl）就是靠它在 UE 里变成 DirectX 的。
 * 所以导出时如果只拷 Source，导出的是 OpenGL 的图，而 UE 里实际用的是翻过的——两边方向相反，
 * 拿去做分解、再回流，凹凸就整个颠倒。这里在写出前按引擎的做法翻一遍 G（8 位：255 - G，16 位：65535 - G），
 * 让导出的图与 UE 里实际用的一致，并在 FDreamTextureExportedImage::bFlippedGreen 里说明。
 * 翻的是读出来的副本，Source 本身原样不动。
 *
 * ## 保真
 *
 * PNG 的像素就是源数据的字节：8 位保持 8 位，16 位保持 16 位，灰度保持灰度，
 * 不做 sRGB↔线性转换（底色本来就是 sRGB 编码，线性图本来就是线性）。只有指定了 MaxEdge 且源图更大时才缩放，
 * 此时在贴图自己的色彩空间里重采样（sRGB 图按线性光滤波）。
 */
class DREAMTEXTUREASSETS_API FDreamTextureTextureExporter
{
public:
	/** 这张贴图能不能导。不能时 OutReason 给中文原因。**游戏线程。** */
	static bool CanExport(const UTexture* Texture, FString& OutReason);

	/** 导出一张。失败时 bExported 为假、Reason 说明。**游戏线程。** FilePath 的目录会被建出来。 */
	static FDreamTextureExportedImage Export(UTexture* Texture, const FString& FilePath, int32 MaxEdge = 0);

	/**
	 * 导出一次解析里的所有贴图。返回值与 Analysis.Textures 一一对应（同下标）；不能导的那几行带原因。
	 * 文件名取资产名，同名的加 _2、_3 后缀。**游戏线程。**
	 */
	static TArray<FDreamTextureExportedImage> ExportAnalysis(const FDreamTextureMaterialAnalysis& Analysis, const FDreamTextureExportOptions& Options = FDreamTextureExportOptions());

	/** <工程 Saved>/DreamTexture/Reflow/<时间戳>-<随机>，已建好。每次调用都是新目录。 */
	static FString MakeTempDir();

	/**
	 * 资产名 → 导出文件名（不含扩展名）。去掉文件名里不合法的字符，并避开 Windows 的保留设备名：
	 * CON / PRN / AUX / NUL / COM1~COM9 / LPT1~LPT9（大小写不敏感）。这些名字在 Windows 上带不带扩展名都不是文件，
	 * 往 "CON.png" 里写东西不会创建文件而是写到控制台设备，写出的 PNG 凭空消失。碰上就加 "Texture_" 前缀。
	 * 名字空了也给个兜底。导出器自己用它，公开出来是为了能被直接测试。
	 */
	static FString MakeSafeFileStem(const FString& AssetName);
};
