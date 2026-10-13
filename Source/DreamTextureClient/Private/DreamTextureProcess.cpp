// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureProcess.h"

#include "Containers/Ticker.h"
#include "DreamTextureHttpUtil.h"
#include "DreamTextureLog.h"
#include "DreamTextureSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/WindowsHWrapper.h"
#endif

namespace
{
	/** 轮询间隔（秒）。后端冷启动是秒级的，再密只是白发请求。 */
	constexpr float PulseIntervalSecs = 0.5f;

	/** 单次 /api/health 探针的超时（秒）。在本机上，2 秒没应答就是没人在听。 */
	constexpr float ProbeTimeoutSecs = 2.0f;

	/** 终止进程后最多等多久它真的退出（秒）。TerminateProcess 之后通常是毫秒级的。 */
	constexpr double TerminateWaitSecs = 5.0;

	FString HealthURL(const FString& BaseURL)
	{
		return BaseURL + TEXT("/api/health");
	}

	/** 后端日志的位置提示。默认在程序旁边的 logs/（internal 日志约定，见后端 docs/backend.md）。 */
	FString LogHint(const FString& Executable)
	{
		const FString Dir = FPaths::GetPath(Executable);
		FString Log = FPaths::Combine(Dir, TEXT("logs"), TEXT("dreamtexture.log"));
		FPaths::MakePlatformFilename(Log);
		return FText::Format(NSLOCTEXT("DreamTextureProcess", "LogHint", "The backend's own log is at {0} by default (or in the folder given by -log-dir in the launch arguments)."), FText::FromString(Log)).ToString();
	}
}

FDreamTextureBackendTarget FDreamTextureBackendTarget::FromSettings()
{
	const UDreamTextureSettings& Settings = UDreamTextureSettings::Get();

	FDreamTextureBackendTarget Target;
	Target.BaseURL = Settings.GetNormalizedBackendURL();
	Target.Arguments = Settings.BackendArguments.TrimStartAndEnd();

	FString Exe = Settings.BackendExecutable.FilePath.TrimStartAndEnd();
	if (!Exe.IsEmpty())
	{
		// 设置项带 RelativeToGameDir：选文件时会存成相对工程目录的路径。
		// 相对路径若按 cwd 解析，编辑器从哪儿启动就会得到不同结果，所以显式按工程目录展开。
		if (FPaths::IsRelative(Exe))
		{
			// ProjectDir() 自己在编辑器里常常也是相对路径（相对引擎二进制目录），所以先把它展开成绝对的，
			// 否则拼出来的还是一条以 ../../ 打头的相对路径。
			Exe = FPaths::ConvertRelativePathToFull(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), Exe);
		}
		FPaths::NormalizeFilename(Exe);
		FPaths::CollapseRelativeDirectories(Exe);
	}
	Target.Executable = Exe;
	return Target;
}

//////////////////////////////////////////////////////////////////////////
// 实现细节

struct FDreamTextureProcess::FImpl
{
	FDreamTextureBackendTarget Target;
	TArray<FEnsureDone> Waiters;

	/** 自己起的进程。有效 ⇔ 这个进程是我们起的（见头文件「怎么判断是自己起的」）。 */
	FProcHandle Proc;
	uint32 Pid = 0;

	/** Windows 的 Job Object 句柄（HANDLE），非 Windows 或创建失败时为空。 */
	void* Job = nullptr;

	FTSTicker::FDelegateHandle PulseHandle;
	bool bPulseActive = false;
	bool bInPulse = false;

	/**
	 * 探针世代。重新开始 / 停止 / 关闭时自增，在途的探针回来对不上就丢弃——
	 * 否则上一轮迟到的应答会把这一轮的状态改掉。
	 */
	uint64 ProbeEpoch = 0;
	bool bProbeInFlight = false;

	/** 我们起的进程已经退出，正在做最后一次探针（也许别人的后端占了端口并应答）。 */
	bool bFinalProbe = false;
	FString ExitMessage;

	double StartedAt = 0.0;
	bool bShutdown = false;

	void InvalidateProbes()
	{
		++ProbeEpoch;
		bProbeInFlight = false;
	}
};

#if PLATFORM_WINDOWS
namespace
{
	/**
	 * 建一个 KILL_ON_JOB_CLOSE 的 Job Object。和后端 internal/comfy/procgroup_windows.go 同一套做法。
	 * 句柄只在本进程里，不可继承——所以别的进程谁也关不掉它，只有本进程退出（无论怎么退出）时系统才会关。
	 */
	void* CreateKillOnCloseJob(FString& OutError)
	{
		HANDLE Job = ::CreateJobObjectW(nullptr, nullptr);
		if (Job == nullptr)
		{
			OutError = FString::Printf(TEXT("CreateJobObject failed (error code %u)"), static_cast<uint32>(::GetLastError()));
			return nullptr;
		}

		JOBOBJECT_EXTENDED_LIMIT_INFORMATION Info = {};
		Info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if (!::SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &Info, sizeof(Info)))
		{
			OutError = FString::Printf(TEXT("SetInformationJobObject failed (error code %u)"), static_cast<uint32>(::GetLastError()));
			::CloseHandle(Job);
			return nullptr;
		}
		return Job;
	}
}
#endif

//////////////////////////////////////////////////////////////////////////
// 生命周期

FDreamTextureProcess::FDreamTextureProcess()
	: Impl(MakeUnique<FImpl>())
{
}

FDreamTextureProcess::~FDreamTextureProcess()
{
	Shutdown();
}

void FDreamTextureProcess::Shutdown()
{
	if (!Impl.IsValid() || Impl->bShutdown)
	{
		return;
	}
	Impl->bShutdown = true;
	Impl->InvalidateProbes();

	// 只停自己起的。附着的后端没有句柄，ReleaseOwnedHandles 里什么也不会发生。
	ReleaseOwnedHandles(/*bTerminate*/ true);
	Origin = EDreamTextureBackendOrigin::None;
	bStarting = false;

	// 不在 Pulse 自己的回调里移除 ticker——FTSTicker::RemoveTicker 在该 ticker 自己的回调里调用是死锁
	// （见 Docs/contributing.md 的 UE 5.8 pitfalls）。在回调里时只置 bShutdown，Pulse 看到就返回 false，ticker 自己消失。
	if (Impl->PulseHandle.IsValid() && !Impl->bInPulse)
	{
		FTSTicker::GetCoreTicker().RemoveTicker(Impl->PulseHandle);
	}
	Impl->PulseHandle.Reset();
	Impl->bPulseActive = false;

	// 还在等的人不再有回音：编辑器都在退出了，回调里的界面多半已经不在。
	Impl->Waiters.Reset();
	OnStateChanged.Clear();
}

//////////////////////////////////////////////////////////////////////////
// 查询

EDreamTextureBackendOrigin FDreamTextureProcess::GetOrigin() const
{
	return Origin;
}

bool FDreamTextureProcess::IsStarting() const
{
	return bStarting;
}

bool FDreamTextureProcess::OwnsRunningProcess() const
{
	if (!Impl.IsValid() || !Impl->Proc.IsValid())
	{
		return false;
	}
	// IsProcRunning 要非 const 的句柄引用；拷贝一份即可（FProcHandle 只是个 HANDLE 的包装）。
	FProcHandle Copy = Impl->Proc;
	return FPlatformProcess::IsProcRunning(Copy);
}

uint32 FDreamTextureProcess::GetOwnedProcessId() const
{
	return OwnsRunningProcess() ? Impl->Pid : 0;
}

//////////////////////////////////////////////////////////////////////////
// 确保后端在

void FDreamTextureProcess::EnsureBackend(FEnsureDone Done)
{
	EnsureBackend(FDreamTextureBackendTarget::FromSettings(), MoveTemp(Done));
}

void FDreamTextureProcess::EnsureBackend(const FDreamTextureBackendTarget& Target, FEnsureDone Done)
{
	if (Impl->bShutdown)
	{
		DreamTexture::RunNextTick([Done = MoveTemp(Done)]()
		{
			if (Done)
			{
				Done(false, NSLOCTEXT("DreamTextureProcess", "ManagementShutDown", "Backend management has shut down (the editor is exiting).").ToString());
			}
		});
		return;
	}

	if (Done)
	{
		Impl->Waiters.Add(MoveTemp(Done));
	}

	// 已经在启动了：这次调用只是多一个等结果的人。第二个进程绝不能起。
	if (bStarting)
	{
		return;
	}

	Impl->Target = Target;
	BeginAttempt();
}

void FDreamTextureProcess::BeginAttempt()
{
	bStarting = true;
	LastError.Reset();
	Impl->bFinalProbe = false;
	Impl->InvalidateProbes();
	OnStateChanged.Broadcast();

	// 先看端口：已经有人应答就附着。放在启动之前是整套规矩的第一条——
	// 端口上有别人起的后端时再去起一个，只会抢端口然后双双失败。
	ProbeHealth([this](bool bOK) { HandleInitialProbe(bOK); });
}

void FDreamTextureProcess::ProbeHealth(TFunction<void(bool)> Done)
{
	Impl->bProbeInFlight = true;
	const uint64 MyEpoch = Impl->ProbeEpoch;
	const TWeakPtr<FDreamTextureProcess> WeakThis = AsWeak();

	DreamTexture::SendRequest(TEXT("GET"), HealthURL(Impl->Target.BaseURL), ProbeTimeoutSecs, FString(),
		[WeakThis, MyEpoch, Done = MoveTemp(Done)](const DreamTexture::FHttpOutcome& Outcome)
		{
			const TSharedPtr<FDreamTextureProcess> Self = WeakThis.Pin();
			if (!Self.IsValid() || Self->Impl->bShutdown || Self->Impl->ProbeEpoch != MyEpoch)
			{
				return;
			}
			Self->Impl->bProbeInFlight = false;
			Done(Outcome.bOK);
		});
}

void FDreamTextureProcess::HandleInitialProbe(bool bOK)
{
	if (bOK)
	{
		// 若我们之前拉起过、还握着一个活着的进程，那这个应答多半就是它的（超时后它终于起来了）。
		Succeed(OwnsRunningProcess() ? EDreamTextureBackendOrigin::Launched : EDreamTextureBackendOrigin::Attached);
		return;
	}

	// 握着一个还活着但没应答的进程：不再起第二个，接着等它。
	if (OwnsRunningProcess())
	{
		Impl->StartedAt = FPlatformTime::Seconds();
		EnsurePulse();
		return;
	}

	LaunchProcess();
}

void FDreamTextureProcess::LaunchProcess()
{
	const FDreamTextureBackendTarget& Target = Impl->Target;

	if (Target.Executable.IsEmpty())
	{
		Fail(FText::Format(
			NSLOCTEXT("DreamTextureProcess", "NoExecutableConfigured", "No backend is answering at {0}, and no backend executable is configured. Set Backend Executable (dreamtexture.exe) in Project Settings > Plugins > DreamTexture, or start the backend yourself first."),
			FText::FromString(Target.BaseURL)).ToString());
		return;
	}

	if (!IFileManager::Get().FileExists(*Target.Executable))
	{
		Fail(FText::Format(
			NSLOCTEXT("DreamTextureProcess", "ExecutableMissing", "No backend is answering at {0}, and the configured backend executable does not exist: {1}. Correct the dreamtexture.exe path in Project Settings > Plugins > DreamTexture."),
			FText::FromString(Target.BaseURL), FText::FromString(Target.Executable)).ToString());
		return;
	}

	// 上一个进程（已经退出的）的句柄先收掉。
	ReleaseOwnedHandles(/*bTerminate*/ false);

#if PLATFORM_WINDOWS
	// Job 要在进程启动**之前**建好，启动之后立刻放进去，窗口越小越好：
	// 后端是在启动过程中才去拉起 ComfyUI 的，放进去得比这一步早。
	FString JobError;
	Impl->Job = CreateKillOnCloseJob(JobError);
	if (Impl->Job == nullptr)
	{
		UE_LOG(LogDreamTexture, Warning, TEXT("Could not create a Job Object (%s): if the editor crashes, the backend will not be taken down with it."), *JobError);
	}
#endif

	// 工作目录设成可执行文件所在目录：后端默认读 configs/dreamtexture.json、写 logs/，都是相对程序的。
	const FString WorkingDir = FPaths::GetPath(Target.Executable);

	uint32 Pid = 0;
	// bLaunchDetached + bLaunchReallyHidden 在 Windows 上就是 CREATE_NO_WINDOW：不弹控制台窗口。
	// 不传管道：5.8 的 CreateProc 此时 bInheritHandles 为 false，编辑器进程的句柄不会漏进后端。
	Impl->Proc = FPlatformProcess::CreateProc(*Target.Executable, *Target.Arguments,
		/*bLaunchDetached*/ true, /*bLaunchHidden*/ true, /*bLaunchReallyHidden*/ true, &Pid,
		/*PriorityModifier*/ 0, *WorkingDir, /*PipeWriteChild*/ nullptr, /*PipeReadChild*/ nullptr);

	if (!Impl->Proc.IsValid())
	{
		ReleaseOwnedHandles(false);
		Fail(FText::Format(NSLOCTEXT("DreamTextureProcess", "LaunchFailed", "Could not start the backend: {0} {1} (the system could not create the process; the editor log has the details)."),
			FText::FromString(Target.Executable), FText::FromString(Target.Arguments)).ToString());
		return;
	}
	Impl->Pid = Pid;

#if PLATFORM_WINDOWS
	if (Impl->Job != nullptr)
	{
		if (!::AssignProcessToJobObject(Impl->Job, Impl->Proc.Get()))
		{
			UE_LOG(LogDreamTexture, Warning, TEXT("Could not put backend process %u into the Job Object (error code %u): if the editor crashes, it will not be taken down with it."),
				Pid, static_cast<uint32>(::GetLastError()));
			::CloseHandle(Impl->Job);
			Impl->Job = nullptr;
		}
	}
#endif

	// 句柄到手就算「自己起的」，不必等它应答：超时后它还活着时，用户点「停止后端」或编辑器退出都得能停到它。
	Origin = EDreamTextureBackendOrigin::Launched;
	Impl->StartedAt = FPlatformTime::Seconds();
	UE_LOG(LogDreamTexture, Log, TEXT("Started the backend: pid %u, %s %s"), Pid, *Target.Executable, *Target.Arguments);

	OnStateChanged.Broadcast();
	EnsurePulse();
}

void FDreamTextureProcess::EnsurePulse()
{
	if (Impl->bPulseActive || Impl->bShutdown)
	{
		return;
	}
	Impl->bPulseActive = true;

	const TWeakPtr<FDreamTextureProcess> WeakThis = AsWeak();
	Impl->PulseHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([WeakThis](float) -> bool
		{
			const TSharedPtr<FDreamTextureProcess> Self = WeakThis.Pin();
			return Self.IsValid() && Self->Pulse();
		}),
		PulseIntervalSecs);
}

bool FDreamTextureProcess::Pulse()
{
	Impl->bInPulse = true;
	ON_SCOPE_EXIT { if (Impl.IsValid()) { Impl->bInPulse = false; } };

	if (Impl->bShutdown)
	{
		Impl->bPulseActive = false;
		return false;
	}

	// 1) 我们起的进程还活着吗。
	if (Impl->Proc.IsValid() && !OwnsRunningProcess())
	{
		HandleOwnedExited();
	}

	// 2) 启动中：超时判定，或者再探一次。最后一次探针在途时不重复发。
	if (bStarting && !Impl->bFinalProbe)
	{
		const double Elapsed = FPlatformTime::Seconds() - Impl->StartedAt;
		if (Elapsed > Impl->Target.StartupTimeoutSecs)
		{
			const bool bAlive = OwnsRunningProcess();
			Fail(FText::Format(
				NSLOCTEXT("DreamTextureProcess", "StartupTimeout", "The backend {0}, but {2} did not respond within {1} seconds. {3}Check the configuration file in the launch arguments: its addr must match the backend address in the project settings. {4}"),
				bAlive ? NSLOCTEXT("DreamTextureProcess", "ProcessAlive", "process is still running") : NSLOCTEXT("DreamTextureProcess", "ProcessGone", "process has exited"),
				FText::AsNumber(FMath::RoundToInt(Impl->Target.StartupTimeoutSecs)), FText::FromString(HealthURL(Impl->Target.BaseURL)),
				bAlive ? NSLOCTEXT("DreamTextureProcess", "StopHint", "It was not terminated; you can use Stop Backend. ") : FText::GetEmpty(),
				FText::FromString(LogHint(Impl->Target.Executable))).ToString());
		}
		else if (!Impl->bProbeInFlight)
		{
			ProbeHealth([this](bool bOK) { HandleStartupProbe(bOK); });
		}
	}

	// 3) 还需要继续盯吗：启动中，或者握着一个活着的进程（意外退出要能发现）。
	const bool bKeep = !Impl->bShutdown && (bStarting || OwnsRunningProcess());
	if (!bKeep)
	{
		Impl->bPulseActive = false;
	}
	return bKeep;
}

void FDreamTextureProcess::HandleStartupProbe(bool bOK)
{
	if (bOK && bStarting)
	{
		Succeed(OwnsRunningProcess() ? EDreamTextureBackendOrigin::Launched : EDreamTextureBackendOrigin::Attached);
	}
}

void FDreamTextureProcess::HandleOwnedExited()
{
	int32 ExitCode = 0;
	if (Impl->Proc.IsValid())
	{
		FPlatformProcess::GetProcReturnCode(Impl->Proc, &ExitCode);
	}

	const FString Message = FText::Format(NSLOCTEXT("DreamTextureProcess", "ProcessExited", "The backend process has exited (exit code {0}). If the port is already in use, pick another port or end the program that holds it. {1}"),
		FText::AsNumber(ExitCode), FText::FromString(LogHint(Impl->Target.Executable))).ToString();

	UE_LOG(LogDreamTexture, Warning, TEXT("%s"), *Message);

	ReleaseOwnedHandles(/*bTerminate*/ false);
	Origin = EDreamTextureBackendOrigin::None;

	if (bStarting)
	{
		// 进程没了不代表没有后端：两个编辑器同时启动时，输的那个会因端口被占而退出，而赢的那个正在应答。
		// 所以退出之后还要再探一次，通了就当附着，没通才算失败。
		Impl->bFinalProbe = true;
		Impl->ExitMessage = Message;
		Impl->InvalidateProbes();
		ProbeHealth([this](bool bOK)
		{
			if (bOK)
			{
				Succeed(EDreamTextureBackendOrigin::Attached);
			}
			else
			{
				Fail(Impl->ExitMessage);
			}
		});
	}
	else
	{
		// 用户用着用着，我们起的后端自己退出了（崩溃或被人杀掉）。记下原因，状态页上能看到。
		LastError = Message;
		OnStateChanged.Broadcast();
	}
}

void FDreamTextureProcess::Succeed(EDreamTextureBackendOrigin NewOrigin)
{
	bStarting = false;
	Impl->bFinalProbe = false;
	Origin = NewOrigin;
	LastError.Reset();
	UE_LOG(LogDreamTexture, Log, TEXT("Backend available: %s (%s)"), *Impl->Target.BaseURL,
		NewOrigin == EDreamTextureBackendOrigin::Launched ? TEXT("started by the plugin") : TEXT("attached to an existing one"));

	OnStateChanged.Broadcast();
	FlushWaiters(true, FString());
}

void FDreamTextureProcess::Fail(const FString& Error)
{
	bStarting = false;
	Impl->bFinalProbe = false;
	LastError = Error;
	if (!Impl->Proc.IsValid())
	{
		Origin = EDreamTextureBackendOrigin::None;
	}
	UE_LOG(LogDreamTexture, Warning, TEXT("Ensuring the backend is up failed: %s"), *Error);

	OnStateChanged.Broadcast();
	FlushWaiters(false, Error);
}

void FDreamTextureProcess::FlushWaiters(bool bOK, const FString& Error)
{
	// 先整体搬出来再调：回调里可能再调 EnsureBackend / StopOwned，不能在遍历自己的数组时被改。
	TArray<FEnsureDone> Waiters = MoveTemp(Impl->Waiters);
	Impl->Waiters.Reset();
	for (FEnsureDone& Waiter : Waiters)
	{
		if (Waiter)
		{
			Waiter(bOK, Error);
		}
	}
}

//////////////////////////////////////////////////////////////////////////
// 停

bool FDreamTextureProcess::StopOwned()
{
	if (!Impl->Proc.IsValid())
	{
		// 附着的后端、或者什么都没起过：按规矩不碰。
		return false;
	}

	const bool bWasRunning = OwnsRunningProcess();
	Impl->InvalidateProbes();
	ReleaseOwnedHandles(/*bTerminate*/ true);
	Origin = EDreamTextureBackendOrigin::None;

	if (bStarting)
	{
		// 用户在启动中途点了停止：把等着的人放走，别让他们挂着。
		Fail(NSLOCTEXT("DreamTextureProcess", "Stopped", "Stopped.").ToString());
	}
	else
	{
		OnStateChanged.Broadcast();
	}
	return bWasRunning;
}

void FDreamTextureProcess::ReleaseOwnedHandles(bool bTerminate)
{
	if (Impl->Proc.IsValid())
	{
		if (bTerminate && FPlatformProcess::IsProcRunning(Impl->Proc))
		{
			// 先终止后端本身。后端被终止时它自己的 Job 句柄随之关闭，ComfyUI 也就跟着没了。
			FPlatformProcess::TerminateProc(Impl->Proc, /*KillTree*/ Impl->Job == nullptr);
		}
	}

#if PLATFORM_WINDOWS
	if (Impl->Job != nullptr)
	{
		// 关 Job 句柄 = 终止 Job 里剩下的一切（KILL_ON_JOB_CLOSE）。进程已经没了时它只是释放句柄。
		::CloseHandle(Impl->Job);
		Impl->Job = nullptr;
	}
#endif

	if (Impl->Proc.IsValid())
	{
		if (bTerminate)
		{
			// 等它真的退出再返回：紧接着的启动不会撞上还没放手的端口。
			const double Deadline = FPlatformTime::Seconds() + TerminateWaitSecs;
			while (FPlatformProcess::IsProcRunning(Impl->Proc) && FPlatformTime::Seconds() < Deadline)
			{
				FPlatformProcess::Sleep(0.02f);
			}
		}
		FPlatformProcess::CloseProc(Impl->Proc);
	}
	Impl->Pid = 0;
}

//////////////////////////////////////////////////////////////////////////
// 测试用

bool FDreamTextureProcess::DebugCloseJobHandleOnly()
{
#if PLATFORM_WINDOWS
	if (Impl->Job != nullptr)
	{
		::CloseHandle(Impl->Job);
		Impl->Job = nullptr;
		return true;
	}
#endif
	return false;
}

bool FDreamTextureProcess::DebugIsOwnedProcessInJob() const
{
#if PLATFORM_WINDOWS
	if (Impl->Job != nullptr && Impl->Proc.IsValid())
	{
		BOOL bInJob = FALSE;
		return ::IsProcessInJob(Impl->Proc.Get(), Impl->Job, &bInJob) && bInJob;
	}
#endif
	return false;
}

#if PLATFORM_WINDOWS
#include "Windows/HideWindowsPlatformTypes.h"
#endif
