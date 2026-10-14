// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

/**
 * 这个后端是谁起的。决定编辑器退出时动不动它。
 */
enum class EDreamTextureBackendOrigin : uint8
{
	/** 此刻没有已知的后端（还没确认过，或者上一次确认失败 / 我们起的那个已经退出了）。 */
	None,

	/**
	 * 端口上本来就有后端在应答，我们只是连上它——不是我们起的，**永远不杀**。
	 * 典型情形：用户先开了桌面外壳或者手动起了 dreamtexture.exe。
	 */
	Attached,

	/** 插件自己拉起来的，进程句柄在我们手上；编辑器退出（或点「停止后端」）时只停这一个。 */
	Launched,
};

/**
 * 「确保后端在」要连、要起的目标。
 *
 * 拆成一个独立的值而不是直接读 UDreamTextureSettings，是为了自动化测试能用自己的端口和
 * 临时配置起一个后端，而不去改用户的设置，更不去碰 8777。
 */
struct DREAMTEXTURECLIENT_API FDreamTextureBackendTarget
{
	/** 后端地址，没有尾斜杠，形如 http://127.0.0.1:8777。 */
	FString BaseURL;

	/** dreamtexture.exe 的绝对路径。空表示没配。 */
	FString Executable;

	/** 原样拼在命令行上的参数。 */
	FString Arguments;

	/**
	 * 拉起后等它应答的时限（秒）。
	 *
	 * 90 秒与桌面外壳的 STARTUP_TIMEOUT 一致（DreamTextureDesktop 仓库的 src-tauri/src/main.rs）：后端冷启动要建 SQLite、
	 * 载工作流，托管模式下还要等 ComfyUI 起来之前的那一段，机器忙的时候不止几秒。
	 */
	float StartupTimeoutSecs = 90.0f;

	/** 按项目设置组装：后端地址、可执行文件（相对路径按工程目录解析成绝对路径）、启动参数。 */
	static FDreamTextureBackendTarget FromSettings();
};

/**
 * 后端进程托管：「确保后端在」。
 *
 * 做的事和桌面外壳（https://github.com/TypeDreamMoon/DreamTextureDesktop 的 src-tauri）一样，规矩也一样：
 *
 * 1. **先看端口**：GET /api/health 应答 2xx 就直接附着，不启动、也不记在自己名下。
 * 2. 没人应答、且配了可执行文件，才 CreateProc 拉起来，轮询 /api/health 等它应答。
 * 3. **谁起的谁负责关**：只有 Origin 是 Launched 时才会停进程；附着的后端永远不碰。
 *
 * ## 怎么判断「是自己起的」
 *
 * 不靠 pid 文件、不靠进程名、不靠端口：只看我们**手里有没有这个进程的句柄**。Windows 的进程句柄会让
 * 进程对象一直存活，pid 在句柄关闭之前不会被复用，所以「我们手里的这个句柄」不可能指到别的进程上。
 * 端口上已经有后端时我们根本没有句柄，StopOwned() 于是什么都做不了——这是结构上的保证，
 * 不是靠一个 if 守住的。
 *
 * ## 编辑器崩了也不留僵尸
 *
 * Windows 上把拉起的进程放进一个设了 JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 的 Job Object。
 * Job 的句柄只在本进程手里，编辑器无论怎么死（崩溃、被任务管理器杀），系统都会关掉它，
 * 连同 Job 里的整棵进程树一起终止。后端自己给 ComfyUI 也建了一个 Job（internal/comfy/procgroup_windows.go），
 * 嵌套 Job 在 Windows 8 之后是允许的：我们杀后端 → 后端的 Job 句柄随之关闭 → ComfyUI 也没了。
 *
 * ## 不弹控制台、不接管道
 *
 * 以 CREATE_NO_WINDOW 拉起；不给 CreateProc 任何管道，UE 5.8 里此时 bInheritHandles 也是 false，
 * 编辑器进程的句柄（包括调用者捕获 stdout 的管道）不会漏进后端。接了管道的话，后端活得比调用者久，
 * 管道就一直不关，读输出的人永远等不到 EOF——Tools/FixtureBackend/README.md 记过这个坑。
 * 后端的日志自己写在 logs\ 下，不需要我们收。
 *
 * 所有回调和委托都在游戏线程上。
 */
class DREAMTEXTURECLIENT_API FDreamTextureProcess : public TSharedFromThis<FDreamTextureProcess>
{
public:
	/** 结果。失败时 Error 是可直接显示的中文原因。 */
	using FEnsureDone = TFunction<void(bool /*bOK*/, const FString& /*Error*/)>;

	DECLARE_MULTICAST_DELEGATE(FOnStateChanged);

	FDreamTextureProcess();
	~FDreamTextureProcess();

	/**
	 * 确保后端在：已经在应答就直接回调；不在且配了路径就拉起来，应答后回调；没配路径就失败。
	 *
	 * 回调**一定是异步的**（至少推迟到下一帧），调用方不必担心回调在自己设好状态之前就跑完了。
	 * 正在启动时再调只是多排一个回调，不会再起第二个进程。
	 *
	 * 按项目设置里的后端地址 / 可执行文件 / 启动参数。
	 */
	void EnsureBackend(FEnsureDone Done);

	/** 同上，但目标由调用方给（自动化测试用）。 */
	void EnsureBackend(const FDreamTextureBackendTarget& Target, FEnsureDone Done);

	/**
	 * 停掉**自己起的**后端。附着的后端、没起过后端，都什么也不做并返回 false。
	 * 返回 true 表示确实终止了一个进程（同步等到它退出，最多几秒）。
	 */
	bool StopOwned();

	/** 模块关闭 / 编辑器退出时调：停掉自己起的，撤掉定时器，丢弃还没回调的等待者。幂等。 */
	void Shutdown();

	EDreamTextureBackendOrigin GetOrigin() const;

	/** 正在拉起 / 等应答。 */
	bool IsStarting() const;

	/** 手里是否握着一个还活着的、自己起的进程。 */
	bool OwnsRunningProcess() const;

	/** 自己起的进程的 pid；没有时是 0。给界面显示用，不要拿它去杀东西。 */
	uint32 GetOwnedProcessId() const;

	/** 最近一次「确保后端在」失败的原因，或我们起的那个进程意外退出的说明。成功后清空。 */
	const FString& GetLastError() const { return LastError; }

	/** 状态变了（开始启动、起来了、失败了、进程退出了、被停掉了）。 */
	FOnStateChanged OnStateChanged;

	/**
	 * 仅供自动化测试：只关掉 Job 句柄、不调 TerminateProcess，模拟「编辑器崩了」，
	 * 用来验证 KILL_ON_JOB_CLOSE 真的把子进程带走。非 Windows 上什么也不做并返回 false。
	 */
	bool DebugCloseJobHandleOnly();

	/** 仅供自动化测试：我们起的进程是否确实在我们的 Job 里。 */
	bool DebugIsOwnedProcessInJob() const;

private:
	struct FImpl;

	void BeginAttempt();
	void ProbeHealth(TFunction<void(bool /*bOK*/)> Done);
	void HandleInitialProbe(bool bOK);
	void LaunchProcess();
	/** 定时器每 0.5 秒来一次：等应答 / 看进程还活着没有。返回 false 即不再需要。 */
	bool Pulse();
	void EnsurePulse();
	void HandleStartupProbe(bool bOK);
	void HandleOwnedExited();
	void Succeed(EDreamTextureBackendOrigin NewOrigin);
	void Fail(const FString& Error);
	void FlushWaiters(bool bOK, const FString& Error);
	void ReleaseOwnedHandles(bool bTerminate);

	TUniquePtr<FImpl> Impl;

	EDreamTextureBackendOrigin Origin = EDreamTextureBackendOrigin::None;
	bool bStarting = false;
	FString LastError;
};
