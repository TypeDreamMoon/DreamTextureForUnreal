// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureSettings.h"

#include "GenericPlatform/GenericPlatformHttp.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

/**
 * DreamTexture.* 自动化测试共用的小工具。只给 Private/Tests/ 下的 *Tests.cpp 用。
 */
namespace DreamTextureTest
{
	/** 固定素材的材质 ID。后端的固定数据后端（Tools/FixtureBackend）服务的也是它。 */
	inline FString FixtureId()
	{
		return TEXT("mat_20260827_777001_ref");
	}

	inline FString GetPluginDir()
	{
		// 不用相对路径或硬编码：宿主工程里的插件目录是 worktree，主 checkout 里又是另一个位置，
		// 只有让插件管理器告诉我们才对。
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamTextureForUnreal"));
		return Plugin.IsValid() ? Plugin->GetBaseDir() : FString();
	}

	inline FString GetFixtureDir()
	{
		return FPaths::Combine(GetPluginDir(), TEXT("Tests"), TEXT("Fixtures"), FixtureId());
	}

	inline bool LoadFixtureText(const FString& FileName, FString& OutText)
	{
		return FFileHelper::LoadFileToString(OutText, *FPaths::Combine(GetFixtureDir(), FileName));
	}

	inline bool LoadFixtureBytes(const FString& FileName, TArray<uint8>& OutBytes)
	{
		return FFileHelper::LoadFileToArray(OutBytes, *FPaths::Combine(GetFixtureDir(), FileName));
	}

	/** 固定素材里的 7 个通道，和 manifest 一一对应。 */
	inline TArray<FString> FixtureChannels()
	{
		return { TEXT("basecolor"), TEXT("normal"), TEXT("roughness"), TEXT("metallic"), TEXT("ao"), TEXT("height"), TEXT("orm") };
	}

	/**
	 * 只在这个对象存活期间把 BackendURL 改成给定值，销毁时（或手动 Restore）还原。
	 *
	 * 只改内存里的 CDO，不 SaveConfig，所以不会写进任何 ini。
	 * 放在析构里还原是为了 latent 命令链中途失败时也不会把用户的设置留成测试地址。
	 */
	struct FBackendUrlOverride
	{
		explicit FBackendUrlOverride(const FString& NewURL)
		{
			UDreamTextureSettings* Settings = GetMutableDefault<UDreamTextureSettings>();
			Previous = Settings->BackendURL;
			Settings->BackendURL = NewURL;
			bActive = true;
		}

		~FBackendUrlOverride()
		{
			Restore();
		}

		void Restore()
		{
			if (bActive)
			{
				GetMutableDefault<UDreamTextureSettings>()->BackendURL = Previous;
				bActive = false;
			}
		}

	private:
		FString Previous;
		bool bActive = false;
	};

	/** DREAMTEXTURE_LIVE_URL。没设（或全是空白）返回空串，调用方据此跳过。 */
	inline FString GetLiveURL()
	{
		return FPlatformMisc::GetEnvironmentVariable(TEXT("DREAMTEXTURE_LIVE_URL")).TrimStartAndEnd();
	}

	/** 没有 DREAMTEXTURE_LIVE_URL 时统一的跳过说明。返回 true 表示应当跳过。 */
	inline bool SkipIfNoLiveBackend(FAutomationTestBase& Test)
	{
		if (!GetLiveURL().IsEmpty())
		{
			return false;
		}
		Test.AddInfo(TEXT("已跳过：没有设置环境变量 DREAMTEXTURE_LIVE_URL。需要先起固定数据后端（Tools/FixtureBackend/Start-FixtureBackend.ps1），"
			"再把它设成 http://127.0.0.1:18777；Tools/Test.ps1 会替你做这两件事。"));
		return true;
	}

	/** 执行 Tools/FixtureBackend 下的 ps1，立即返回进程句柄（脚本要几秒，不能在游戏线程上等）。 */
	inline FProcHandle LaunchFixtureScript(const TCHAR* ScriptName, const FString& LiveURL)
	{
		const FString Script = FPaths::Combine(GetPluginDir(), TEXT("Tools"), TEXT("FixtureBackend"), ScriptName);

		// 端口跟着 DREAMTEXTURE_LIVE_URL 走，两边不会各说各话。
		FString Args = FString::Printf(TEXT("-NoProfile -NonInteractive -File \"%s\""), *Script);
		const TOptional<uint16> Port = FGenericPlatformHttp::GetUrlPort(LiveURL);
		if (Port.IsSet())
		{
			Args += FString::Printf(TEXT(" -Port %d"), static_cast<int32>(Port.GetValue()));
		}

		return FPlatformProcess::CreateProc(TEXT("pwsh.exe"), *Args, /*bLaunchDetached*/ false, /*bLaunchHidden*/ true,
			/*bLaunchReallyHidden*/ true, nullptr, 0, nullptr, nullptr);
	}

	inline bool FixtureScriptExists(const TCHAR* ScriptName)
	{
		return FPaths::FileExists(FPaths::Combine(GetPluginDir(), TEXT("Tools"), TEXT("FixtureBackend"), ScriptName));
	}
}

/** 一次性执行一个函数就结束的 latent 命令，用来把「发起动作」排进命令链里。 */
class FDreamTextureDoCommand : public IAutomationLatentCommand
{
public:
	explicit FDreamTextureDoCommand(TFunction<void()> InAction)
		: Action(MoveTemp(InAction))
	{
	}

	virtual bool Update() override
	{
		if (Action)
		{
			Action();
		}
		return true;
	}

private:
	TFunction<void()> Action;
};

/**
 * 等一个条件成立，带超时。
 *
 * 超时只做两件事：给测试记一条带说明的错误，再调 OnTimeout（让后续步骤知道该跳过了）。
 * 不用整条命令链的超时兜底——那种超时只会告诉你「卡住了」，不会说卡在哪一步。
 */
class FDreamTextureWaitCommand : public IAutomationLatentCommand
{
public:
	FDreamTextureWaitCommand(FAutomationTestBase* InTest, FString InWhat, double InTimeoutSecs,
		TFunction<bool()> InIsDone, TFunction<void()> InOnDone = nullptr, TFunction<void()> InOnTimeout = nullptr)
		: Test(InTest)
		, What(MoveTemp(InWhat))
		, TimeoutSecs(InTimeoutSecs)
		, IsDone(MoveTemp(InIsDone))
		, OnDone(MoveTemp(InOnDone))
		, OnTimeout(MoveTemp(InOnTimeout))
	{
	}

	virtual bool Update() override
	{
		if (IsDone())
		{
			if (OnDone)
			{
				OnDone();
			}
			return true;
		}

		if (GetCurrentRunTime() > TimeoutSecs)
		{
			Test->AddError(FString::Printf(TEXT("等待超时（%.0f 秒）：%s"), TimeoutSecs, *What));
			if (OnTimeout)
			{
				OnTimeout();
			}
			return true;
		}
		return false;
	}

private:
	FAutomationTestBase* Test;
	FString What;
	double TimeoutSecs;
	TFunction<bool()> IsDone;
	TFunction<void()> OnDone;
	TFunction<void()> OnTimeout;
};

#endif // WITH_DEV_AUTOMATION_TESTS
