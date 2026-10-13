// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureClientModule.h"

#include "DreamTextureConnection.h"
#include "DreamTextureJobTracker.h"
#include "DreamTextureLog.h"
#include "DreamTextureProcess.h"
#include "DreamTexturePushSession.h"
#include "DreamTextureSettings.h"

DEFINE_LOG_CATEGORY(LogDreamTexture);

IMPLEMENT_MODULE(FDreamTextureClientModule, DreamTextureClient)

namespace
{
	const FName DreamTextureClientModuleName(TEXT("DreamTextureClient"));
}

FDreamTextureClientModule& FDreamTextureClientModule::Get()
{
	return FModuleManager::LoadModuleChecked<FDreamTextureClientModule>(DreamTextureClientModuleName);
}

bool FDreamTextureClientModule::IsAvailable()
{
	return FModuleManager::Get().IsModuleLoaded(DreamTextureClientModuleName);
}

void FDreamTextureClientModule::StartupModule()
{
	/*
	 * 先把 WebSockets 与 HTTP 两个模块加载起来，再做别的。
	 *
	 * 编辑器退出时按「加载顺序」倒着卸载模块，而一个模块的加载顺序是在它的 StartupModule 返回之后才登记的
	 * （引擎 ModuleManager.cpp 的 LoadModuleWithFailureReason）：在这里加载的模块排在本模块前面，退出时比我们晚关。
	 * Build.cs 里写了依赖并不管这个顺序。不在这里加载的话，它们要等第一次拨号 / 第一个请求（下一帧以后）才被拉起来，
	 * 排到了我们后面，退出时先被卸掉——ShutdownModule 里关长连接，FLwsWebSocket::Close() 再去取 FWebSocketsModule，
	 * 等于在关闭途中重新加载一个已卸载的模块，断言崩溃（!ModuleInfo->bWasUnloadedAtShutdown）。
	 * 宿主工程里别的插件先加载了 WebSockets，所以一直没复现；只装了本插件的工程里一退出就崩。
	 */
	FModuleManager::Get().LoadModuleChecked(TEXT("WebSockets"));
	FModuleManager::Get().LoadModuleChecked(TEXT("HTTP"));

	// MakeShared 而不是 new：FDreamTextureConnection 继承 TSharedFromThis，
	// 它在异步回调里靠 AsWeak() 保命，必须先被共享指针持有才行。
	Connection = MakeShared<FDreamTextureConnection>();

	// cook / commandlet 里没人看界面，拨号出去只是白白拖慢启动，还会在日志里
	// 刷一串连不上的告警。真要在自动化里用，调用方自己 Start() 就行（幂等）。
	if (!IsRunningCommandlet())
	{
		Connection->Start();
	}

	// 追踪器订阅连接上的 job.* 事件与链路变化。必须在连接建好之后绑，且要被共享指针持有（AddSP）。
	JobTracker = MakeShared<FDreamTextureJobTracker>();
	JobTracker->Bind(Connection);

	// 推送会话同样订阅这条连接：变 Live 就注册、每 15 秒续期。commandlet 里连接压根没拨号，不绑。
	PushSession = MakeShared<FDreamTexturePushSession>();
	if (!IsRunningCommandlet())
	{
		PushSession->Bind(Connection);
	}

	Process = MakeShared<FDreamTextureProcess>();

	// 进程管理器把后端拉起来之后，连接还在退避里等下一次重试（最长 15 秒）。
	// 后端一应答就让连接立刻重试，状态条不用白白红那么久。
	Process->OnStateChanged.AddLambda([this]()
	{
		if (Connection.IsValid() && Process.IsValid() && !Process->IsStarting()
			&& Process->GetOrigin() != EDreamTextureBackendOrigin::None && !Connection->IsBackendReachable())
		{
			Connection->Reconnect();
		}
	});

	// 默认关（理由见设置项的注释）。只在配了路径时才有得可起；没配路径就不制造一条失败日志。
	if (!IsRunningCommandlet() && UDreamTextureSettings::Get().bAutoStartBackend
		&& !UDreamTextureSettings::Get().BackendExecutable.FilePath.TrimStartAndEnd().IsEmpty())
	{
		Process->EnsureBackend([](bool bOK, const FString& Error)
		{
			if (!bOK)
			{
				UE_LOG(LogDreamTexture, Warning, TEXT("Auto-starting the backend failed: %s"), *Error);
			}
		});
	}

	UE_LOG(LogDreamTexture, Log, TEXT("DreamTextureClient started"));
}

void FDreamTextureClientModule::ShutdownModule()
{
	// 先于连接收：编辑器退出时只停自己起的那个后端（附着的不碰）。
	if (Process.IsValid())
	{
		Process->Shutdown();
		Process.Reset();
	}

	// 会话先于连接收：趁连接和 HTTP 还在，把注销请求发出去（不等应答）。
	if (PushSession.IsValid())
	{
		PushSession->Shutdown();
		PushSession.Reset();
	}

	// 追踪器先于连接收：先解绑，不让它在连接关闭的广播里再被叫到。
	if (JobTracker.IsValid())
	{
		JobTracker->Unbind();
		JobTracker.Reset();
	}

	if (Connection.IsValid())
	{
		// 先 Stop() 再 Reset()：Stop() 会解绑 WS 委托、移掉定时器、关掉连接。
		// 光 Reset() 也不会泄漏（回调都是弱引用），但定时器要等下一帧才发现
		// 目标没了，模块卸载途中多这一帧很没必要。
		Connection->Stop();
		Connection.Reset();
	}

	UE_LOG(LogDreamTexture, Log, TEXT("DreamTextureClient shut down"));
}
