// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"

/**
 * Editor 模块里 DreamTexture.Live.* 共用的 latent 小工具。只给 Private/Tests/ 下的 *Tests.cpp 用。
 *
 * Client / Assets 模块各有自己的一份（那两个模块的测试头在它们的 Private 里，Editor 够不着），
 * 这里是 Editor 这一份。
 */
namespace DreamTextureLive
{
	/** DREAMTEXTURE_LIVE_URL。没设（或全是空白）返回空串，调用方据此跳过。 */
	inline FString GetLiveURL()
	{
		return FPlatformMisc::GetEnvironmentVariable(TEXT("DREAMTEXTURE_LIVE_URL")).TrimStartAndEnd();
	}

	/** 一次性执行一个函数就结束的 latent 命令。 */
	class FDoCommand : public IAutomationLatentCommand
	{
	public:
		explicit FDoCommand(TFunction<void()> InAction)
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

	/** 等一个条件成立，带超时。超时记一条带说明的错误（说清楚卡在哪一步），再调 OnTimeout。 */
	class FWaitCommand : public IAutomationLatentCommand
	{
	public:
		FWaitCommand(FAutomationTestBase* InTest, FString InWhat, double InTimeoutSecs, TFunction<bool()> InIsDone,
			TFunction<void()> InOnDone, TFunction<void()> InOnTimeout)
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

	inline void Enqueue(const TSharedPtr<IAutomationLatentCommand>& Command)
	{
		FAutomationTestFramework::Get().EnqueueLatentCommand(Command);
	}

	inline void EnqueueDo(TFunction<void()> Action)
	{
		Enqueue(MakeShareable(new FDoCommand(MoveTemp(Action))));
	}

	inline void EnqueueWait(FAutomationTestBase* Test, const FString& What, double TimeoutSecs, TFunction<bool()> IsDone,
		TFunction<void()> OnDone = nullptr, TFunction<void()> OnTimeout = nullptr)
	{
		Enqueue(MakeShareable(new FWaitCommand(Test, What, TimeoutSecs, MoveTemp(IsDone), MoveTemp(OnDone), MoveTemp(OnTimeout))));
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
