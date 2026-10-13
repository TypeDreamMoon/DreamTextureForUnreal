// Copyright TypeDreamMoon. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureHttpUtil.h"
#include "DreamTextureProcess.h"
#include "DreamTextureSettings.h"
#include "DreamTextureTestUtil.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"

/*
 * DreamTexture.Client.Process.* —— 后端进程托管。
 *
 * 起的是一个**真的** dreamtexture.exe，但配置完全是测试自己写的临时文件：
 *   - 端口 18778（不是用户的 8777，也不是固定数据后端的 18777）；
 *   - comfy 走 attach 模式，base_url 指向一个没人监听的端口，所以不会拉起 ComfyUI、不碰显存；
 *   - data / output / logs 都在 Saved/DreamTextureTest/ 下的临时目录里。
 *
 * 可执行文件由环境变量 DREAMTEXTURE_BACKEND_EXE 指定（Tools/Test.ps1 会按 Tools/LocalSettings.psd1 的 BackendExe 替你设）。
 * 它**只是用来启动我们自己的实例**——不会去动用户在 8777 上跑着的那个。找不到可执行文件就跳过并说明。
 *
 * 端口 18778 上若已经有东西在应答（上一次测试的残留、或别的东西），整个测试跳过，绝不去动它。
 */

namespace
{
	using namespace DreamTextureTest;

	constexpr int32 TestPort = 18778;

	/** 没人监听的端口，用来测「地址上没后端」的几种失败。 */
	constexpr int32 DeadPort = 18780;

	FString TestURL()
	{
		return FString::Printf(TEXT("http://127.0.0.1:%d"), TestPort);
	}

	FString FindBackendExe()
	{
		FString Exe = FPlatformMisc::GetEnvironmentVariable(TEXT("DREAMTEXTURE_BACKEND_EXE")).TrimStartAndEnd();
		if (Exe.IsEmpty())
		{
			Exe = TEXT("C:/Tools/DreamTexture/dreamtexture.exe");
		}
		return Exe;
	}

	/**
	 * 异步探 /api/health：每次 Kick 一发，结果出来后 Result 有值。
	 * 状态放在共享指针里：请求可能在测试收尾之后才回来，回调不能去写已经释放的对象。
	 */
	struct FReachProbe
	{
		struct FShared
		{
			bool bInFlight = false;
			TOptional<bool> Result;
		};

		TSharedRef<FShared> Shared = MakeShared<FShared>();

		TOptional<bool>& Result() const { return Shared->Result; }

		void Kick(const FString& BaseURL)
		{
			if (Shared->bInFlight)
			{
				return;
			}
			Shared->bInFlight = true;
			Shared->Result.Reset();
			DreamTexture::SendRequest(TEXT("GET"), BaseURL + TEXT("/api/health"), 2.0f, FString(),
				[Shared = this->Shared](const DreamTexture::FHttpOutcome& Outcome)
				{
					Shared->bInFlight = false;
					Shared->Result = Outcome.bOK;
				});
		}
	};

	struct FProcState
	{
		TSharedPtr<FDreamTextureProcess> A = MakeShared<FDreamTextureProcess>();
		TSharedPtr<FDreamTextureProcess> B = MakeShared<FDreamTextureProcess>();
		FDreamTextureBackendTarget Target;
		FString TempDir;
		bool bSkip = false;

		int32 DoneCount = 0;
		bool bOK[3] = { false, false, false };
		FString Err[3];
		uint32 Pid = 0;

		FReachProbe Reach;
		bool bGone = false;

		// 一路上别的步骤要在探针对象析构前结束：探针的回调捕获了 this，所以 State 用 TSharedRef 持有，
		// 且析构里先收进程。
		~FProcState()
		{
			if (A.IsValid())
			{
				A->Shutdown();
			}
			if (B.IsValid())
			{
				B->Shutdown();
			}
		}
	};

	void ProcEnqueue(const TSharedPtr<IAutomationLatentCommand>& Command)
	{
		FAutomationTestFramework::Get().EnqueueLatentCommand(Command);
	}

	/** 排一个动作；整个测试被跳过时不执行。 */
	void EnqueueStep(const TSharedRef<FProcState>& State, TFunction<void()> Action)
	{
		ProcEnqueue(MakeShareable(new FDreamTextureDoCommand([State, Action = MoveTemp(Action)]()
		{
			if (!State->bSkip)
			{
				Action();
			}
		})));
	}

	void ProcEnqueueWait(const TSharedRef<FProcState>& State, FAutomationTestBase* Test, const FString& What, double TimeoutSecs,
		TFunction<bool()> IsDone, TFunction<void()> OnDone = nullptr)
	{
		ProcEnqueue(MakeShareable(new FDreamTextureWaitCommand(Test, What, TimeoutSecs,
			[State, IsDone = MoveTemp(IsDone)]() { return State->bSkip || IsDone(); },
			[State, OnDone = MoveTemp(OnDone)]()
			{
				if (!State->bSkip && OnDone)
				{
					OnDone();
				}
			})));
	}

	/** 等 BaseURL 上的后端变成（不）可达。 */
	void EnqueueWaitReachable(const TSharedRef<FProcState>& State, FAutomationTestBase* Test, bool bWantReachable,
		const FString& What, double TimeoutSecs = 15.0)
	{
		ProcEnqueueWait(State, Test, What, TimeoutSecs, [State, bWantReachable]()
		{
			// 先取结果再发下一发：Kick 会清掉上一次的结果，顺序反了就永远读不到。
			if (State->Reach.Result().IsSet())
			{
				const bool bReachable = State->Reach.Result().GetValue();
				State->Reach.Result().Reset();
				if (bReachable == bWantReachable)
				{
					return true;
				}
			}
			State->Reach.Kick(State->Target.BaseURL);
			return false;
		});
	}

	/**
	 * 备好临时目录与配置，并确认 18778 上没人。返回 false 表示这一轮该跳过（原因已写进 Test）。
	 * 配置写成 attach 模式 + 死端口的 comfy：见文件头。
	 */
	bool Prepare(FAutomationTestBase& Test, FProcState& State)
	{
		const FString Exe = FindBackendExe();
		if (!IFileManager::Get().FileExists(*Exe))
		{
			Test.AddInfo(FString::Printf(TEXT("已跳过：找不到测试用的后端可执行文件 %s（可用环境变量 DREAMTEXTURE_BACKEND_EXE 指定）。"), *Exe));
			return false;
		}

		State.TempDir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DreamTextureTest"),
			TEXT("ProcessBackend"), FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8)));
		const FString DataDir = FPaths::Combine(State.TempDir, TEXT("data"));
		const FString OutputDir = FPaths::Combine(State.TempDir, TEXT("output"));
		const FString LogDir = FPaths::Combine(State.TempDir, TEXT("logs"));
		IFileManager::Get().MakeDirectory(*DataDir, true);
		IFileManager::Get().MakeDirectory(*OutputDir, true);
		IFileManager::Get().MakeDirectory(*LogDir, true);

		// 后端要载入工作流，缺了起不来。用可执行文件旁边的那份（只读）。
		const FString WorkflowsDir = FPaths::Combine(FPaths::GetPath(Exe), TEXT("workflows"));

		// JSON 里路径一律用正斜杠，省去转义反斜杠；Go 在 Windows 上两种都认。
		const FString Config = FString::Printf(TEXT(
			"{\n"
			"  \"addr\": \"127.0.0.1:%d\",\n"
			"  \"output_dir\": \"%s\",\n"
			"  \"data_dir\": \"%s\",\n"
			"  \"workflows_dir\": \"%s\",\n"
			"  \"comfy\": {\n"
			"    \"mode\": \"attach\",\n"
			"    \"base_url\": \"http://127.0.0.1:%d\",\n"
			"    \"start_timeout\": \"2s\",\n"
			"    \"health_interval\": \"30s\",\n"
			"    \"auto_restart\": false,\n"
			"    \"max_queue_depth\": 8\n"
			"  }\n"
			"}\n"),
			TestPort, *OutputDir.Replace(TEXT("\\"), TEXT("/")), *DataDir.Replace(TEXT("\\"), TEXT("/")),
			*WorkflowsDir.Replace(TEXT("\\"), TEXT("/")), 18779);
		const FString ConfigPath = FPaths::Combine(State.TempDir, TEXT("backend.json"));
		FFileHelper::SaveStringToFile(Config, *ConfigPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

		State.Target.BaseURL = TestURL();
		State.Target.Executable = Exe;
		// -log-dir 很关键：不给的话后端把日志写到可执行文件旁边的 logs/，那是用户真实后端的日志。
		State.Target.Arguments = FString::Printf(TEXT("-config \"%s\" -log-dir \"%s\""), *ConfigPath, *LogDir);
		State.Target.StartupTimeoutSecs = 60.0f;
		return true;
	}

	/** 第一步：确认端口空闲。有人应答就把整个测试标记为跳过。 */
	void EnqueuePortFreeCheck(const TSharedRef<FProcState>& State, FAutomationTestBase* Test)
	{
		EnqueueStep(State, [State]() { State->Reach.Kick(State->Target.BaseURL); });
		ProcEnqueue(MakeShareable(new FDreamTextureWaitCommand(Test, TEXT("探测 18778 是否空闲"), 10.0,
			[State]() { return State->Reach.Result().IsSet(); },
			[State, Test]()
			{
				if (State->Reach.Result().GetValue())
				{
					State->bSkip = true;
					Test->AddInfo(FString::Printf(TEXT("已跳过：%s 上已经有后端在应答（不是本测试起的），不碰它。"), *TestURL()));
				}
				State->Reach.Result().Reset();
			})));
	}

	/** 收尾：无论前面成败都停掉自己起的进程、删临时目录。 */
	void EnqueueCleanup(const TSharedRef<FProcState>& State)
	{
		ProcEnqueue(MakeShareable(new FDreamTextureDoCommand([State]()
		{
			State->A->Shutdown();
			State->B->Shutdown();
			if (!State->TempDir.IsEmpty() && FPlatformMisc::GetEnvironmentVariable(TEXT("DREAMTEXTURE_KEEP_TEST_DIR")).IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(*State->TempDir, false, true);
			}
		})));
	}

	void StartEnsure(const TSharedRef<FProcState>& State, const TSharedPtr<FDreamTextureProcess>& Mgr, int32 Slot)
	{
		Mgr->EnsureBackend(State->Target, [State, Slot](bool bOK, const FString& Error)
		{
			State->bOK[Slot] = bOK;
			State->Err[Slot] = Error;
			++State->DoneCount;
		});
	}
}

//////////////////////////////////////////////////////////////////////////
// 拉起 → 可达 → 停掉自己起的

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureProcessLaunchTest, "DreamTexture.Client.Process.Launch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureProcessLaunchTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FProcState> State = MakeShared<FProcState>();
	if (!Prepare(*this, *State))
	{
		return true;
	}

	EnqueuePortFreeCheck(State, this);

	// 连调两次：第二次只是多一个等结果的人，不能起第二个进程。
	EnqueueStep(State, [State]()
	{
		StartEnsure(State, State->A, 0);
		StartEnsure(State, State->A, 1);
	});
	ProcEnqueueWait(State, this, TEXT("两次「确保后端在」都回调"), 70.0, [State]() { return State->DoneCount >= 2; },
		[State, this]()
		{
			TestTrue(*FString::Printf(TEXT("第一次成功（%s）"), *State->Err[0]), State->bOK[0]);
			TestTrue(*FString::Printf(TEXT("第二次成功（%s）"), *State->Err[1]), State->bOK[1]);
			TestTrue(TEXT("来源是插件启动的"), State->A->GetOrigin() == EDreamTextureBackendOrigin::Launched);
			TestFalse(TEXT("应答之后不再是启动中"), State->A->IsStarting());
			TestTrue(TEXT("握着一个活着的进程"), State->A->OwnsRunningProcess());
			State->Pid = State->A->GetOwnedProcessId();
			TestTrue(TEXT("pid 非零"), State->Pid != 0);
			TestTrue(TEXT("成功后 LastError 为空"), State->A->GetLastError().IsEmpty());
#if PLATFORM_WINDOWS
			TestTrue(TEXT("进程在 Job Object 里"), State->A->DebugIsOwnedProcessInJob());
#endif
		});

	// 端口上确实有后端在应答。
	EnqueueWaitReachable(State, this, true, TEXT("拉起后 /api/health 可达"));

	// 已经在了：再确保一次直接成功，不换进程。
	EnqueueStep(State, [State]()
	{
		State->DoneCount = 0;
		StartEnsure(State, State->A, 2);
	});
	ProcEnqueueWait(State, this, TEXT("已在时的「确保后端在」回调"), 10.0, [State]() { return State->DoneCount >= 1; },
		[State, this]()
		{
			TestTrue(TEXT("已在时直接成功"), State->bOK[2]);
			TestEqual(TEXT("没有换进程"), State->A->GetOwnedProcessId(), State->Pid);
		});

	// 停掉自己起的。
	EnqueueStep(State, [State, this]()
	{
		TestTrue(TEXT("StopOwned 确实停掉了一个进程"), State->A->StopOwned());
		TestTrue(TEXT("停掉后来源回到 None"), State->A->GetOrigin() == EDreamTextureBackendOrigin::None);
		TestFalse(TEXT("停掉后不再握有进程"), State->A->OwnsRunningProcess());
		TestFalse(TEXT("再停一次什么也没有"), State->A->StopOwned());
	});
	EnqueueWaitReachable(State, this, false, TEXT("停掉后端口释放"));
	EnqueueStep(State, [State, this]()
	{
		TestFalse(TEXT("进程真的没了"), FPlatformProcess::IsApplicationRunning(State->Pid));
	});

	EnqueueCleanup(State);
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 端口上已有别人起的后端：附着，退出时不杀

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureProcessAttachTest, "DreamTexture.Client.Process.Attach",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureProcessAttachTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FProcState> State = MakeShared<FProcState>();
	if (!Prepare(*this, *State))
	{
		return true;
	}

	EnqueuePortFreeCheck(State, this);

	// A 扮演「别人起的后端」。
	EnqueueStep(State, [State]() { StartEnsure(State, State->A, 0); });
	ProcEnqueueWait(State, this, TEXT("A 拉起后端"), 70.0, [State]() { return State->DoneCount >= 1; },
		[State, this]()
		{
			TestTrue(*FString::Printf(TEXT("A 成功（%s）"), *State->Err[0]), State->bOK[0]);
			State->Pid = State->A->GetOwnedProcessId();
			TestTrue(TEXT("A 拿到 pid"), State->Pid != 0);
			State->DoneCount = 0;
		});

	// B 是另一个编辑器：同样的目标，它面前端口上已经有后端了。
	EnqueueStep(State, [State]() { StartEnsure(State, State->B, 1); });
	ProcEnqueueWait(State, this, TEXT("B 附着"), 20.0, [State]() { return State->DoneCount >= 1; },
		[State, this]()
		{
			TestTrue(*FString::Printf(TEXT("B 成功（%s）"), *State->Err[1]), State->bOK[1]);
			TestTrue(TEXT("B 是附着，不是启动"), State->B->GetOrigin() == EDreamTextureBackendOrigin::Attached);
			TestFalse(TEXT("B 没有自己的进程"), State->B->OwnsRunningProcess());
			TestEqual(TEXT("B 没有 pid"), State->B->GetOwnedProcessId(), 0u);
			TestFalse(TEXT("B 不会停别人的后端"), State->B->StopOwned());
			TestTrue(TEXT("StopOwned 之后 B 还是附着"), State->B->GetOrigin() == EDreamTextureBackendOrigin::Attached);
			// 编辑器退出时走的是 Shutdown——它同样不能杀附着的后端。
			State->B->Shutdown();
		});

	EnqueueWaitReachable(State, this, true, TEXT("B 退出后 A 的后端还活着"));
	EnqueueStep(State, [State, this]()
	{
		TestTrue(TEXT("A 的进程还在"), FPlatformProcess::IsApplicationRunning(State->Pid));
		TestTrue(TEXT("A 仍是启动者"), State->A->GetOrigin() == EDreamTextureBackendOrigin::Launched);
		TestTrue(TEXT("A 自己停得掉"), State->A->StopOwned());
	});
	EnqueueWaitReachable(State, this, false, TEXT("A 停掉后端口释放"));

	EnqueueCleanup(State);
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 编辑器崩了：Job Object 把子进程带走

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureProcessJobKillTest, "DreamTexture.Client.Process.JobKill",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureProcessJobKillTest::RunTest(const FString& Parameters)
{
#if !PLATFORM_WINDOWS
	AddInfo(TEXT("已跳过：Job Object 只在 Windows 上。"));
	return true;
#else
	const TSharedRef<FProcState> State = MakeShared<FProcState>();
	if (!Prepare(*this, *State))
	{
		return true;
	}

	EnqueuePortFreeCheck(State, this);
	EnqueueStep(State, [State]() { StartEnsure(State, State->A, 0); });
	ProcEnqueueWait(State, this, TEXT("拉起后端"), 70.0, [State]() { return State->DoneCount >= 1; },
		[State, this]()
		{
			TestTrue(*FString::Printf(TEXT("拉起成功（%s）"), *State->Err[0]), State->bOK[0]);
			State->Pid = State->A->GetOwnedProcessId();
			TestTrue(TEXT("进程在 Job 里"), State->A->DebugIsOwnedProcessInJob());
		});
	EnqueueWaitReachable(State, this, true, TEXT("后端可达"));

	// 只关 Job 句柄，不调 TerminateProcess——这正是编辑器崩溃时系统替我们做的事。
	EnqueueStep(State, [State, this]()
	{
		TestTrue(TEXT("关掉了 Job 句柄"), State->A->DebugCloseJobHandleOnly());
	});
	EnqueueWaitReachable(State, this, false, TEXT("关 Job 句柄后后端被系统带走、端口释放"));
	ProcEnqueueWait(State, this, TEXT("管理器发现进程没了"), 5.0, [State]() { return !State->A->OwnsRunningProcess(); },
		[State, this]()
		{
			TestFalse(TEXT("进程真的没了"), FPlatformProcess::IsApplicationRunning(State->Pid));
		});

	// 进程意外退出，管理器的定时器应当发现并记下原因。
	ProcEnqueueWait(State, this, TEXT("管理器记下意外退出"), 5.0, [State]() { return State->A->GetOrigin() == EDreamTextureBackendOrigin::None; },
		[State, this]()
		{
			TestTrue(TEXT("原因里说了进程已退出"), State->A->GetLastError().Contains(TEXT("has exited")));
		});

	EnqueueCleanup(State);
	return true;
#endif
}

//////////////////////////////////////////////////////////////////////////
// 配不了 / 起不来：中文原因

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureProcessFailureTest, "DreamTexture.Client.Process.Failure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureProcessFailureTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FProcState> State = MakeShared<FProcState>();
	State->Target.BaseURL = FString::Printf(TEXT("http://127.0.0.1:%d"), DeadPort);
	State->Target.StartupTimeoutSecs = 30.0f;

	// 1) 没配可执行文件。
	State->Target.Executable.Reset();
	EnqueueStep(State, [State]() { StartEnsure(State, State->A, 0); });
	ProcEnqueueWait(State, this, TEXT("没配路径：失败回调"), 15.0, [State]() { return State->DoneCount >= 1; },
		[State, this]()
		{
			TestFalse(TEXT("没配路径：失败"), State->bOK[0]);
			TestTrue(*FString::Printf(TEXT("没配路径：原因说明了没配（%s）"), *State->Err[0]), State->Err[0].Contains(TEXT("no backend executable is configured")));
			TestFalse(TEXT("没配路径：不是启动中"), State->A->IsStarting());
			TestTrue(TEXT("没配路径：来源 None"), State->A->GetOrigin() == EDreamTextureBackendOrigin::None);
			TestFalse(TEXT("没配路径：没有进程"), State->A->OwnsRunningProcess());
			TestEqual(TEXT("没配路径：LastError 同步"), State->A->GetLastError(), State->Err[0]);
			State->DoneCount = 0;
		});

	// 2) 路径不存在。
	EnqueueStep(State, [State]()
	{
		State->Target.Executable = TEXT("C:/DreamTextureTest/does-not-exist/dreamtexture.exe");
		StartEnsure(State, State->A, 1);
	});
	ProcEnqueueWait(State, this, TEXT("路径不存在：失败回调"), 15.0, [State]() { return State->DoneCount >= 1; },
		[State, this]()
		{
			TestFalse(TEXT("路径不存在：失败"), State->bOK[1]);
			TestTrue(*FString::Printf(TEXT("路径不存在：原因说了不存在且带上路径（%s）"), *State->Err[1]),
				State->Err[1].Contains(TEXT("does not exist")) && State->Err[1].Contains(TEXT("does-not-exist")));
			TestFalse(TEXT("路径不存在：没有进程"), State->A->OwnsRunningProcess());
			State->DoneCount = 0;
		});

	// 3) 进程一起来就退出（拿 whoami 当「会立刻退出的程序」）：要说出退出码，而不是干等到超时。
	const FString SystemRoot = FPlatformMisc::GetEnvironmentVariable(TEXT("SystemRoot"));
	const FString Whoami = FPaths::Combine(SystemRoot, TEXT("System32"), TEXT("whoami.exe"));
	if (IFileManager::Get().FileExists(*Whoami))
	{
		EnqueueStep(State, [State, Whoami]()
		{
			State->Target.Executable = Whoami;
			StartEnsure(State, State->A, 2);
		});
		ProcEnqueueWait(State, this, TEXT("进程立刻退出：失败回调（远早于 30 秒超时）"), 15.0, [State]() { return State->DoneCount >= 1; },
			[State, this]()
			{
				TestFalse(TEXT("进程立刻退出：失败"), State->bOK[2]);
				TestTrue(*FString::Printf(TEXT("进程立刻退出：原因说了已退出和退出码（%s）"), *State->Err[2]),
					State->Err[2].Contains(TEXT("has exited")) && State->Err[2].Contains(TEXT("exit code")));
				TestTrue(TEXT("进程立刻退出：来源 None"), State->A->GetOrigin() == EDreamTextureBackendOrigin::None);
				TestFalse(TEXT("进程立刻退出：不再握有进程"), State->A->OwnsRunningProcess());
				TestFalse(TEXT("进程立刻退出：不是启动中"), State->A->IsStarting());
			});
	}
	else
	{
		AddInfo(TEXT("没找到 whoami.exe，跳过「进程立刻退出」这一段。"));
	}

	EnqueueCleanup(State);
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 设置 → 目标：相对路径按工程目录展开

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureProcessTargetTest, "DreamTexture.Client.Process.Target",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureProcessTargetTest::RunTest(const FString& Parameters)
{
	UDreamTextureSettings* Settings = GetMutableDefault<UDreamTextureSettings>();
	const FString PreviousURL = Settings->BackendURL;
	const FString PreviousExe = Settings->BackendExecutable.FilePath;
	const FString PreviousArgs = Settings->BackendArguments;

	Settings->BackendURL = TEXT("http://127.0.0.1:18778/");
	Settings->BackendExecutable.FilePath = TEXT("../Tools/dreamtexture.exe");
	Settings->BackendArguments = TEXT("  -config \"x.json\"  ");

	const FDreamTextureBackendTarget Relative = FDreamTextureBackendTarget::FromSettings();
	TestEqual(TEXT("地址去掉尾斜杠"), Relative.BaseURL, FString(TEXT("http://127.0.0.1:18778")));
	TestFalse(TEXT("相对路径被展开成绝对路径"), FPaths::IsRelative(Relative.Executable));
	TestTrue(*FString::Printf(TEXT("展开结果落在工程目录的上一级（%s）"), *Relative.Executable),
		Relative.Executable.EndsWith(TEXT("/Tools/dreamtexture.exe")) && !Relative.Executable.Contains(TEXT("..")));
	TestEqual(TEXT("参数去掉首尾空白"), Relative.Arguments, FString(TEXT("-config \"x.json\"")));
	TestEqual(TEXT("默认启动超时与桌面外壳一致"), Relative.StartupTimeoutSecs, 90.0f);

	Settings->BackendExecutable.FilePath = TEXT("D:/Some/Where/dreamtexture.exe");
	TestEqual(TEXT("绝对路径原样保留"), FDreamTextureBackendTarget::FromSettings().Executable, FString(TEXT("D:/Some/Where/dreamtexture.exe")));

	Settings->BackendExecutable.FilePath = TEXT("   ");
	TestTrue(TEXT("全空白当没配"), FDreamTextureBackendTarget::FromSettings().Executable.IsEmpty());

	Settings->BackendURL = PreviousURL;
	Settings->BackendExecutable.FilePath = PreviousExe;
	Settings->BackendArguments = PreviousArgs;
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
