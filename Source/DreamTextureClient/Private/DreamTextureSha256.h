// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace DreamTexture
{
	/**
	 * 一次性计算 SHA-256（FIPS 180-4），摘要写进 OutDigest。
	 *
	 * 为什么自己写而不是用引擎的：UE 5.8 里 SHA-256 只有两处现成的，都不合用。
	 * FPlatformMisc::GetSHA256Signature 在 Windows 上没有平台实现，落到
	 * FGenericPlatformMisc 那个桩里，桩的第一句是 checkf(false, "No SHA256 Platform implementation")，
	 * 一调就断言。另一处是 S3Client 直接调 OpenSSL 的 SHA256()，要为这几十行代码给整个
	 * 模块加一条 OpenSSL 的静态链接依赖，不值得。
	 *
	 * 这份实现由 DreamTexture.Client.Hash.* 守着：NIST 的标准向量、填充边界的各个长度，
	 * 以及固定素材里每个文件与后端算出来的哈希逐一对照。
	 */
	void Sha256(TConstArrayView<uint8> Data, uint8 (&OutDigest)[32]);
}
