// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * POST /api/uploads：把本地一张图传给后端，换回一个能直接填进 `LoadImage` 的文件名。
 *
 * 回流（M8）靠它把导出的 BaseColor 送上去：补通道填进 `source_image`，改写外观填进 `reference`。
 * 出处：后端 internal/api/uploads.go，说明见后端 docs/reference-image.md「上传」一节。
 *
 * ## 后端的规矩
 *
 * - multipart/form-data，**字段名是 `image`**；
 * - 只收 PNG / JPG / WebP / BMP（按文件扩展名判），上限 32 MB，超了早点拒绝；
 * - 文件名由后端定（`dt_ref_<随机>.<扩展名>`），所以返回的文件名要原样拿去用，不要自己猜；
 * - 后端要把图转存到 ComfyUI 的 input 目录，ComfyUI 不在时回 502「转存到 ComfyUI 失败」。
 *
 * 全是静态函数，基地址每次现取 UDreamTextureSettings。回调一定异步、在游戏线程。
 */
class DREAMTEXTURECLIENT_API FDreamTextureUploadApi
{
public:
	/** 后端的上传上限（internal/api/uploads.go 的 maxUploadBytes）。发之前自己先量一遍，别等后端拒绝。 */
	static constexpr int64 MaxUploadBytes = 32ll * 1024 * 1024;

	/** bOK 为真时 StoredName 是后端起的文件名；否则 Error 是可直接显示的中文原因。 */
	using FOnUploaded = TFunction<void(bool /*bOK*/, FString&& /*StoredName*/, const FString& /*Error*/)>;

	/**
	 * 读文件并上传。文件读不了、扩展名后端不收、超过 MaxUploadBytes 都不发请求，直接异步回调失败。
	 */
	static void UploadImage(const FString& FilePath, FOnUploaded Done);

	/** 后端接受的扩展名（小写，带点）。 */
	static bool IsSupportedExtension(const FString& FileName);

	/**
	 * 拼 multipart/form-data 的请求体：一个文件字段。纯函数，测试直接验证字节。
	 * 文件名里的引号与换行会被替换掉，免得破坏头部。
	 */
	static void BuildMultipartBody(const FString& Boundary, const FString& FieldName, const FString& FileName,
		const FString& ContentType, TConstArrayView<uint8> Bytes, TArray<uint8>& OutBody);

	/** 按扩展名给 Content-Type。后端按扩展名判，这个头只是让请求规整。 */
	static FString GuessContentType(const FString& FileName);

	/** 解析 {"name": …}。没有 name 字段（或是空串）返回 false。 */
	static bool ParseResponse(const FString& Json, FString& OutStoredName);
};
