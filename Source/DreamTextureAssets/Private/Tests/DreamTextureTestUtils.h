// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DreamTextureManifest.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "TextureCompiler.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

/** 资产落地层自动化测试共用的小工具。 */
namespace DreamTextureTestUtils
{
	inline const FString FixtureMaterialId = TEXT("mat_20260827_777001_ref");

	/** 插件目录下的固定素材：Tests/Fixtures/<id>。 */
	inline FString GetFixtureDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamTextureForUnreal"));
		return Plugin.IsValid() ? Plugin->GetBaseDir() / TEXT("Tests/Fixtures") / FixtureMaterialId : FString();
	}

	/**
	 * 读固定素材：manifest 解析好，每个通道配上本地 PNG 的绝对路径。
	 * 直接用插件目录里的原文件，不拷贝——导入只读它们。
	 */
	inline bool LoadFixture(FDreamTextureManifest& OutManifest, TMap<FString, FString>& OutFiles, FString& OutError)
	{
		const FString Dir = GetFixtureDir();
		FString Json;
		if (Dir.IsEmpty() || !FFileHelper::LoadFileToString(Json, *(Dir / TEXT("manifest.json"))))
		{
			OutError = FString::Printf(TEXT("读不到固定素材的 manifest：%s"), *Dir);
			return false;
		}
		if (!FDreamTextureManifest::FromJsonString(Json, OutManifest, OutError))
		{
			return false;
		}
		OutFiles.Reset();
		for (const TPair<FString, FDreamTextureMapInfo>& Pair : OutManifest.Maps)
		{
			OutFiles.Add(Pair.Key, Dir / Pair.Value.File);
		}
		return true;
	}

	/**
	 * 每次唯一的测试内容目录 /Game/DreamTextureTests/<Guid>，离开作用域时连包带文件一起删掉。
	 *
	 * 不能只删磁盘文件：贴图对象还在内存里的话，资产注册表和 GC 都会继续认它，
	 * 下一个测试的同名路径（虽然不会撞，Guid 不同）以及内容浏览器里都会留下鬼影。
	 */
	struct FScopedTestContent
	{
		FString Root;

		FScopedTestContent()
			: Root(TEXT("/Game/DreamTextureTests/") + FGuid::NewGuid().ToString(EGuidFormats::Digits))
		{
		}

		~FScopedTestContent()
		{
			Cleanup();
		}

		FScopedTestContent(const FScopedTestContent&) = delete;
		FScopedTestContent& operator=(const FScopedTestContent&) = delete;

		FString GetDiskDir() const
		{
			return FPackageName::LongPackageNameToFilename(Root);
		}

		/** 把测试目录下已加载的所有对象删掉，再清磁盘。可以重复调。 */
		void Cleanup()
		{
			// 贴图的异步编译还在读它们的源数据，删之前必须等完。
			FTextureCompilingManager::Get().FinishAllCompilation();

			TArray<UObject*> ToDelete;
			const FString Prefix = Root + TEXT("/");
			for (TObjectIterator<UPackage> It; It; ++It)
			{
				UPackage* Package = *It;
				if (Package->GetName().StartsWith(Prefix))
				{
					ForEachObjectWithPackage(Package, [&ToDelete](UObject* Object)
					{
						if (Object->IsAsset())
						{
							ToDelete.Add(Object);
						}
						return true;
					}, EGetObjectsFlags::None);
				}
			}
			if (ToDelete.Num() > 0)
			{
				ObjectTools::DeleteObjectsUnchecked(ToDelete);
			}

			IFileManager::Get().DeleteDirectory(*GetDiskDir(), /*RequireExists*/ false, /*Tree*/ true);

			// 上一级 Content/DreamTextureTests 也别留空壳。DeleteDirectory 不带 Tree 时只删空目录，
			// 万一别处还有东西就原样不动。
			IFileManager::Get().DeleteDirectory(*FPaths::GetPath(GetDiskDir()), /*RequireExists*/ false, /*Tree*/ false);

			// 资产注册表里这些路径是靠扫描建出来的；磁盘文件没了之后重扫一次，把残留条目清掉。
			if (FAssetRegistryModule* Module = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry")))
			{
				Module->Get().ScanPathsSynchronous({ Root }, /*bForceRescan*/ true);
			}
		}
	};

	// ---- latent 命令：队列的回调是异步的，测试得一边等一边让引擎跑 tick ----

	inline void AddLatent(TFunction<bool()> Update)
	{
		FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShareable(new FFunctionLatentCommand(MoveTemp(Update))));
	}

	/** 排一步只执行一次的动作。 */
	inline void AddLatentStep(TFunction<void()> Step)
	{
		AddLatent([Step = MoveTemp(Step)]()
		{
			Step();
			return true;
		});
	}

	/**
	 * 排一步「等到条件成立」。超时就记一条指明卡在哪一步的错误，并把 bAborted 置位，
	 * 后面的检查步骤据此跳过——不然它们会对着没有完成的状态一路报出一串无关的错。
	 */
	inline void AddLatentWait(FAutomationTestBase* Test, const FString& What, double TimeoutSecs, TFunction<bool()> IsDone, const TSharedRef<bool>& bAborted)
	{
		const TSharedRef<double> Deadline = MakeShared<double>(-1.0);
		AddLatent([Test, What, TimeoutSecs, IsDone = MoveTemp(IsDone), bAborted, Deadline]()
		{
			if (*bAborted)
			{
				return true;
			}
			const double Now = FPlatformTime::Seconds();
			if (*Deadline < 0.0)
			{
				*Deadline = Now + TimeoutSecs;
			}
			if (IsDone())
			{
				return true;
			}
			if (Now > *Deadline)
			{
				Test->AddError(FString::Printf(TEXT("等待超时（%.0f 秒）：%s"), TimeoutSecs, *What));
				*bAborted = true;
				return true;
			}
			return false;
		});
	}

	/** 按对象路径找已加载的贴图。 */
	inline UTexture2D* FindTexture(const FString& ObjectPath)
	{
		return FindObject<UTexture2D>(nullptr, *ObjectPath);
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
