// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

/** 母材质与材质实例两处共用的小工具。贴图导入器那份在它自己的匿名命名空间里，没动。 */
namespace DreamTextureAssetUtil
{
	/**
	 * 找已有的资产：内存里有就直接用，否则磁盘上有这个包就加载进来，都没有返回 nullptr。
	 *
	 * 来源与版本记录都在包元数据里，不加载包就读不到，所以「已存在吗」必须走加载，
	 * 不能只问资产注册表。
	 */
	inline UObject* FindOrLoad(const FString& PackageName, const FString& AssetName)
	{
		const FString ObjectPath = PackageName + TEXT(".") + AssetName;
		if (UObject* InMemory = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return InMemory;
		}
		if (FPackageName::DoesPackageExist(PackageName))
		{
			return LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		return nullptr;
	}
}
