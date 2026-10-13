// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureAssetsModule.h"
#include "DreamTextureImportQueue.h"

IMPLEMENT_MODULE(FDreamTextureAssetsModule, DreamTextureAssets)

FDreamTextureAssetsModule& FDreamTextureAssetsModule::Get()
{
	return FModuleManager::LoadModuleChecked<FDreamTextureAssetsModule>(TEXT("DreamTextureAssets"));
}

bool FDreamTextureAssetsModule::IsAvailable()
{
	return FModuleManager::Get().IsModuleLoaded(TEXT("DreamTextureAssets"));
}

void FDreamTextureAssetsModule::StartupModule()
{
}

void FDreamTextureAssetsModule::ShutdownModule()
{
	// 还在飞的下载回调可能晚于模块卸载才回来，队列要先把它们挡掉。
	FDreamTextureImportQueue::Get().Shutdown();
}
