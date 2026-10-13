// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DreamTextureManifest.h"

/**
 * 后端素材相关 REST 接口的薄封装。
 *
 * 全是静态函数，基地址每次现取 UDreamTextureSettings，所以设置里改了地址立刻生效。
 * **回调一律在游戏线程上跑。** 这层不管调用方的生命周期：回调里要碰 UI 对象，
 * 调用方自己捕获弱指针。
 */
class DREAMTEXTURECLIENT_API FDreamTextureApi
{
public:
	using FOnList = TFunction<void(bool bOK, TArray<FDreamTextureMaterialSummary>&& Materials, const FString& Error)>;
	using FOnManifest = TFunction<void(bool bOK, FDreamTextureManifest&& Manifest, const FString& Error)>;
	using FOnBytes = TFunction<void(bool bOK, TArray<uint8>&& Bytes, const FString& Error)>;
	using FOnDone = TFunction<void(bool bOK, const FString& Error)>;

	/** GET /api/materials?q=&style=&fav=&limit=&offset= → 响应体的 materials 数组（可能是 null）。 */
	static void ListMaterials(const FDreamTextureMaterialQuery& Query, FOnList Done);

	/** GET /api/materials/{id} → 响应体是 {"manifest":{...},"index":{...}}，取 manifest。 */
	static void GetManifest(const FString& MaterialId, FOnManifest Done);

	/** POST /api/materials/{id}/favorite，body {"favorite":true|false}。 */
	static void SetFavorite(const FString& MaterialId, bool bFavorite, FOnDone Done);

	/** GET /api/materials/{id}/files/{name}，整个读进内存。缩略图用；大文件走 FDreamTextureFetcher。 */
	static void FetchFile(const FString& MaterialId, const FString& FileName, FOnBytes Done);

	/** 拼文件地址。id 和文件名都做 URL 编码。 */
	static FString MakeFileURL(const FString& MaterialId, const FString& FileName);
};

/** 下载一套材质的选项。 */
struct DREAMTEXTURECLIENT_API FDreamTextureFetchOptions
{
	/** 要拉哪些通道。空 = FDreamTextureFetcher::DefaultChannels()。 */
	TArray<FString> Channels;

	/**
	 * 工程里已经有、且记着哈希的通道：通道 → sha256。
	 *
	 * 与 manifest 里的哈希一致就不下载，记进结果的 UpToDate。这是 M1 的去重：
	 * 同一套材质再导一次，什么都不拉。
	 */
	TMap<FString, FString> KnownHashes;
};

/** 一次下载的结果。 */
struct DREAMTEXTURECLIENT_API FDreamTextureFetchResult
{
	/** 至少拿到了一张、或全部都是 UpToDate，且没有任何一张校验失败。 */
	bool bOK = false;

	/** bOK 为 false 时的中文原因。 */
	FString Error;

	/** 通道 → 本地绝对路径。只含下载完且 sha256 校验通过的。 */
	TMap<FString, FString> LocalFiles;

	/** 哈希与 KnownHashes 一致、因此没下载的通道。导入端对它们什么都不做。 */
	TArray<FString> UpToDate;

	/** 没拿到的通道 → 中文原因（manifest 里没有、设置里不导、下载失败、校验不符…）。 */
	TMap<FString, FString> Skipped;

	/** 本次下载的落盘目录。 */
	FString LocalDir;
};

/**
 * 把一套材质的贴图拉到本地。
 *
 * 落盘位置是工程 Saved/DreamTexture/Downloads/<id>/。每个文件先写临时名、校验 sha256
 * 通过后再改名——半截文件不能被当成好文件。
 */
class DREAMTEXTURECLIENT_API FDreamTextureFetcher
{
public:
	using FOnProgress = TFunction<void(int32 FilesDone, int32 FilesTotal)>;
	using FOnFetched = TFunction<void(FDreamTextureFetchResult&& Result)>;

	/**
	 * 按 manifest 下载。所有文件结束（成功或失败）后**在游戏线程上回调一次** Done。
	 * Progress 每完成一个文件调一次，也在游戏线程。
	 */
	static void DownloadMaps(const FDreamTextureManifest& Manifest, const FDreamTextureFetchOptions& Options, FOnFetched Done, FOnProgress Progress = nullptr);

	/** basecolor / normal / orm / height；UDreamTextureSettings::bImportSeparateChannels 为真时再加 roughness / metallic / ao。 */
	static TArray<FString> DefaultChannels();

	static FString GetDownloadDir(const FString& MaterialId);

	/** 与后端完全一致：全部字节的 SHA-256，小写十六进制，取前 16 位。 */
	static FString HashBytes16(TConstArrayView<uint8> Bytes);

	/**
	 * 一个材质 ID 或文件名能不能放心地拼进本地路径和 URL。
	 *
	 * 非空，只含 [A-Za-z0-9_.-]，且不是 "."、不含 ".."。光检查字符集不够：
	 * ".." 的每个字符都在白名单里，却正好是路径穿越。规则与后端 safeSegment 对齐。
	 * 下载端用它挡 manifest 里的 id 和文件名；别的地方要拿 ID 拼路径（资产名之类）也该先过它。
	 */
	static bool IsSafePathSegment(const FString& Segment);
};
