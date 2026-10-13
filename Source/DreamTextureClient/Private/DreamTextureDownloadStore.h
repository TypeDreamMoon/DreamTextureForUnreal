// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace DreamTexture
{
	/**
	 * 把已经在内存里的下载内容校验后落盘。拆出来是为了不连后端也能测。
	 *
	 * 顺序是：大小 → sha256（都在内存里比）→ 写临时名 → 改名成正式名。
	 * 任何一步不通过都返回 false、OutError 给中文原因，并且**正式名上不会出现新文件**
	 * （原来就有的旧文件也不动）——半截的、被截断的、被换过的内容不能被当成好文件。
	 * 校验放在落盘之前，所以坏数据连临时文件都不会写。
	 *
	 * 临时名带随机后缀：同一套材质被连点两次导入时，两条下载不会踩同一个临时文件。
	 *
	 * 可以在任意线程调用，不碰任何共享状态。
	 *
	 * @param ExpectedBytes   manifest 里的 bytes。<= 0 表示 manifest 没给，不比大小。
	 * @param ExpectedSha256  manifest 里的 sha256（前 16 位小写十六进制），不分大小写比较。
	 */
	bool StoreVerifiedFile(TConstArrayView<uint8> Bytes, int64 ExpectedBytes, const FString& ExpectedSha256, const FString& FinalPath, FString& OutError);
}
