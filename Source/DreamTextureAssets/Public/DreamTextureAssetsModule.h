// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/** 资产落地模块。不碰 Slate：通知、内容浏览器跳转是界面模块的事。 */
class DREAMTEXTUREASSETS_API FDreamTextureAssetsModule : public IModuleInterface
{
public:
	static FDreamTextureAssetsModule& Get();
	static bool IsAvailable();

	//~ IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
