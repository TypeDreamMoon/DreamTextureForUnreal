// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DataDrivenShaderPlatformInfo.h"
#include "HAL/IConsoleManager.h"
#include "MaterialShared.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "RHI.h"
#include "RHIGlobals.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"

/** 需要真实 RHI 的测试（DreamTexture.Render.*）共用的小工具。 */
namespace DreamTextureRenderTestUtils
{
	/**
	 * 这个进程能不能真的编着色器、渲染。-nullrhi（Tools/Test.ps1 默认）下不能：
	 * 材质 PostEditChange 根本不会去缓存着色器，GetCompileErrors 永远是空的，「没有错误」就成了空话。
	 */
	inline bool CanRender(FString& OutWhy)
	{
		if (!FApp::CanEverRender())
		{
			OutWhy = TEXT("当前进程不渲染（-nullrhi，FApp::CanEverRender() 为假）");
			return false;
		}
		if (!GIsRHIInitialized || GDynamicRHI == nullptr)
		{
			OutWhy = TEXT("RHI 没有初始化");
			return false;
		}
		if (RHIGetInterfaceType() == ERHIInterfaceType::Null)
		{
			OutWhy = TEXT("RHI 是 Null 实现");
			return false;
		}
		if (GShaderCompilingManager == nullptr)
		{
			OutWhy = TEXT("没有着色器编译管理器");
			return false;
		}
		return true;
	}

	/** 一句话说明在什么渲染环境下跑的，打进 AddInfo，方便对照日志。 */
	inline FString DescribeRenderer()
	{
		const IConsoleVariable* Substrate = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Substrate"));
		return FString::Printf(TEXT("渲染环境：RHI=%s，着色器平台=%s，r.Substrate=%d"),
			GDynamicRHI != nullptr ? GDynamicRHI->GetName() : TEXT("?"),
			*FDataDrivenShaderPlatformInfo::GetName(GMaxRHIShaderPlatform).ToString(),
			Substrate != nullptr ? Substrate->GetInt() : -1);
	}

	struct FCompileOutcome
	{
		bool bFinished = false;
		/** 编出了可用的着色器表。编译失败时它是空的，引擎退回默认材质。 */
		bool bHasShaderMap = false;
		/** 着色器表完整：该编的着色器都编出来了（不是提交前的空壳）。 */
		bool bComplete = false;
		/** 着色器表里的着色器个数。0 说明表是空壳——没真编。 */
		int32 NumShaders = 0;
		/** 翻译器与着色器编译器报的错，原文。 */
		TArray<FString> Errors;
	};

	/**
	 * 把这个材质（或实例）的着色器真的编出来，然后读结果。
	 *
	 * 光 PostEditChange 或 FinishAllCompilation 不够：这个引擎版本里，材质的着色器任务是**按需提交**的——
	 * 刷新之后得到的只是一张空壳着色器表，要有人要用它（渲染、或编辑器里看统计）才会提交编译任务。
	 * 所以这里照 UMaterialEditingLibrary::GetStatistics 的做法：表不完整就主动提交，再等它编完。
	 * 不这么做会得到「没有错误」「着色器表非空」「着色器 0 个」的假通过。
	 *
	 * 另外 5.8 里 GetMaterialResource 按「着色器平台」取，按特性级别取的重载标了弃用且直接返回 nullptr——
	 * 沿用老写法会拿到空指针。
	 */
	inline FCompileOutcome WaitForCompile(UMaterialInterface* Material)
	{
		FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform);
		FCompileOutcome Outcome;
		if (Resource == nullptr)
		{
			Outcome.Errors.Add(TEXT("取不到这个材质在当前着色器平台上的资源（GetMaterialResource 返回空）"));
			return Outcome;
		}

		if (!Resource->IsGameThreadShaderMapComplete())
		{
			Resource->SubmitCompileJobs_GameThread(EShaderCompileJobPriority::High);
		}
		Resource->FinishCompilation();
		GShaderCompilingManager->FinishAllCompilation();
		FlushRenderingCommands();

		Outcome.bFinished = Resource->IsCompilationFinished();
		Outcome.bComplete = Resource->IsGameThreadShaderMapComplete();
		Outcome.bHasShaderMap = Resource->GetGameThreadShaderMap() != nullptr;
		Outcome.NumShaders = Outcome.bHasShaderMap ? static_cast<int32>(Resource->GetGameThreadShaderMap()->GetShaderNum()) : 0;
		Outcome.Errors = Resource->GetCompileErrors();
		if (!Outcome.bFinished)
		{
			Outcome.Errors.Add(TEXT("等了所有着色器编译之后，这个材质的编译仍未完成"));
		}
		return Outcome;
	}

	/**
	 * 渲染产物（预览图）放哪：Tools/Test.ps1 通过环境变量给这一次运行的产物目录；
	 * 手动在编辑器里跑时没有这个变量，落在 Saved/DreamTextureTest/Manual 下。
	 */
	inline FString GetOutputDir()
	{
		FString Base = FPlatformMisc::GetEnvironmentVariable(TEXT("DREAMTEXTURE_TEST_OUTPUT_DIR")).TrimStartAndEnd();
		if (Base.IsEmpty())
		{
			Base = FPaths::ProjectSavedDir() / TEXT("DreamTextureTest/Manual");
		}
		return FPaths::ConvertRelativePathToFull(Base) / TEXT("render");
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
