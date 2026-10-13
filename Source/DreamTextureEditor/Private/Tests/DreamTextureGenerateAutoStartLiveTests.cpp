// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureClientModule.h"
#include "DreamTextureConnection.h"
#include "DreamTextureGenerate.h"
#include "DreamTextureGenerateModel.h"
#include "DreamTextureJobTracker.h"
#include "DreamTextureLiveTestUtils.h"
#include "DreamTextureProcess.h"
#include "DreamTextureSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

/*
 * DreamTexture.Live.Generate.AutoStart：后端没开的时候点「生成」，插件自己把它拉起来并把任务送进队列。
 *
 * 这条走的是**全套真东西**：模块里那个进程管理器、那条全局连接、那个追踪器，加上页面模型的默认后端函数——
 * 和用户在编辑器里点按钮时一模一样，没有假的。起的是一个真的 dreamtexture.exe，但配置完全是测试自己写的：
 *   - 端口 18793（不是用户的 8777，也不是固定数据后端的 18777、进程托管测试的 18778）；
 *   - comfy 走 attach 模式、指向 18779（没人监听），所以不会拉起 ComfyUI、不碰显存；
 *   - data / output / logs 都在 Saved/DreamTextureTest/ 下的临时目录里（-log-dir 必须给，否则后端把日志写进
 *     可执行文件旁边，那是用户真实后端的日志目录）。
 * 可执行文件由环境变量 DREAMTEXTURE_BACKEND_EXE 指定（Tools/Test.ps1 会按 Tools/LocalSettings.psd1 的 BackendExe 替你设）；
 * 它**只是用来启动我们自己的实例**，不会去动用户在 8777 上跑着的那个。
 *
 * 剧本：
 *   1. 把项目设置临时改成指向 18793 + 可执行文件 + 临时配置（只改内存里的 CDO，不写 ini，结束时还原），让全局连接重连过去；
 *   2. 打开页面的样子：目录请求失败、后端进程没应答 → 主按钮必须是「启动后端并生成」，左栏要显示表单；
 *   3. 写提示词、点主按钮：后端被拉起来（来源 Launched），目录补拉到，**一个**变体提交成功，种子原样往返；
 *      启动中再点不会起第二个进程；
 *   4. 全局连接追上后端，事件把任务推到「等待 ComfyUI」（ComfyUI 是死的，任务会排着等它，卡片上看得出来）；
 *   5. 取消任务，状态 canceled；
 *   6. 停掉自己起的后端，端口释放。
 * 无论成败，收尾都会停掉自己起的后端、还原设置、删临时目录。
 *
 * 只有 DREAMTEXTURE_LIVE_URL 有值才跑（和别的 Live 测试一样由 Tools/Test.ps1 -Live 控制）；
 * 找不到可执行文件、18793 上已经有后端在应答、进程管理器此刻已经握着后端，都跳过并说明，不去动它们。
 */

namespace
{
	using namespace DreamTextureLive;

	constexpr int32 AutoStartPort = 18793;
	constexpr int32 DeadComfyPort = 18779;
	constexpr int64 AutoStartSeed = 54321;

	FString FindBackendExe()
	{
		FString Exe = FPlatformMisc::GetEnvironmentVariable(TEXT("DREAMTEXTURE_BACKEND_EXE")).TrimStartAndEnd();
		if (Exe.IsEmpty())
		{
			Exe = TEXT("C:/Tools/DreamTexture/dreamtexture.exe");
		}
		return Exe;
	}

	struct FAutoState : public TSharedFromThis<FAutoState>
	{
		FString Exe;
		FString TempDir;

		FString PreviousURL;
		FString PreviousExe;
		FString PreviousArgs;
		bool bOverridden = false;

		TSharedPtr<FDreamTextureProcess> Process;
		TSharedPtr<FDreamTextureConnection> Conn;
		TSharedPtr<FDreamTextureJobTracker> Tracker;
		TSharedPtr<FDreamTextureGenerateModel> Model;

		/** 前面某一步失败（或判定要跳过）到没法继续了：后续检查步骤直接跳过，但收尾照常执行。 */
		bool bAbort = false;

		/** 点过主按钮。开始时进程管理器一定是空的，所以之后它握着的任何后端都是我们起的。 */
		bool bClicked = false;

		/** 已经由本测试停掉了自己起的后端（收尾阶段据此决定要不要等端口释放）。 */
		bool bStopped = false;

		uint32 Pid = 0;
		FString JobId;

		// 「端口上还有没有后端在应答」的探针（借工作流列表请求）。
		bool bProbeInFlight = false;
		TOptional<bool> ProbeResult;

		~FAutoState()
		{
			Cleanup();
		}

		void Cleanup()
		{
			// 只停自己起的：开始时进程管理器是空的（见 Prepare 的检查），所以 Launched 一定是我们点出来的。
			if (bClicked && Process.IsValid() && Process->GetOrigin() == EDreamTextureBackendOrigin::Launched)
			{
				Process->StopOwned();
			}

			if (bOverridden)
			{
				UDreamTextureSettings* Settings = GetMutableDefault<UDreamTextureSettings>();
				Settings->BackendURL = PreviousURL;
				Settings->BackendExecutable.FilePath = PreviousExe;
				Settings->BackendArguments = PreviousArgs;
				bOverridden = false;

				// 全局连接从我们的后端回到原来的地址（测试编辑器启动时压到的那个）。
				if (Conn.IsValid())
				{
					Conn->Reconnect();
				}
			}

			if (!TempDir.IsEmpty() && FPlatformMisc::GetEnvironmentVariable(TEXT("DREAMTEXTURE_KEEP_TEST_DIR")).IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(*TempDir, false, true);
				TempDir.Reset();
			}
		}

		/** 发一发探针，结果出来后 ProbeResult 有值（true = 有后端在应答）。 */
		void KickProbe()
		{
			if (bProbeInFlight)
			{
				return;
			}
			bProbeInFlight = true;
			ProbeResult.Reset();
			// 弱引用：请求可能在测试收尾、状态销毁之后才回来。
			const TWeakPtr<FAutoState> Weak = AsWeak();
			FDreamTextureGenerateApi::ListWorkflows([Weak](bool bOK, FDreamTextureWorkflowCatalog&&, const FString&)
			{
				if (const TSharedPtr<FAutoState> Self = Weak.Pin())
				{
					Self->bProbeInFlight = false;
					Self->ProbeResult = bOK;
				}
			});
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureLiveGenerateAutoStartTest, "DreamTexture.Live.Generate.AutoStart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureLiveGenerateAutoStartTest::RunTest(const FString& Parameters)
{
	if (GetLiveURL().IsEmpty())
	{
		AddInfo(TEXT("已跳过：没有设置环境变量 DREAMTEXTURE_LIVE_URL（这条测试由 Tools/Test.ps1 -Live 一起放行）。"));
		return true;
	}

	const FString Exe = FindBackendExe();
	if (!IFileManager::Get().FileExists(*Exe))
	{
		AddInfo(FString::Printf(TEXT("已跳过：找不到测试用的后端可执行文件 %s（可用环境变量 DREAMTEXTURE_BACKEND_EXE 指定）。"), *Exe));
		return true;
	}

	if (!FDreamTextureClientModule::IsAvailable())
	{
		AddError(TEXT("客户端模块没有加载。"));
		return false;
	}
	const TSharedPtr<FDreamTextureProcess> Process = FDreamTextureClientModule::Get().GetProcess();
	const TSharedPtr<FDreamTextureConnection> Conn = FDreamTextureClientModule::Get().GetConnection();
	const TSharedPtr<FDreamTextureJobTracker> Tracker = FDreamTextureClientModule::Get().GetJobTracker();
	if (!Process.IsValid() || !Conn.IsValid() || !Tracker.IsValid())
	{
		AddError(TEXT("客户端模块没有给出进程管理器 / 连接 / 追踪器。"));
		return false;
	}
	if (Process->GetOrigin() != EDreamTextureBackendOrigin::None || Process->IsStarting())
	{
		// 进程管理器此刻已经握着一个后端（或在启动它）：那不是我们的，不碰。
		AddInfo(TEXT("已跳过：进程管理器此刻已经有后端（附着的或在启动中），这条测试不去动它。"));
		return true;
	}

	FAutomationTestBase* Test = this;
	const TSharedRef<FAutoState> S = MakeShared<FAutoState>();
	S->Exe = Exe;
	S->Process = Process;
	S->Conn = Conn;
	S->Tracker = Tracker;

	// ---- 1. 备好临时配置，把设置指向它，让全局连接重连到 18793 ----
	EnqueueDo([S, Test]()
	{
		S->TempDir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DreamTextureTest"),
			TEXT("AutoStartBackend"), FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8)));
		const FString DataDir = FPaths::Combine(S->TempDir, TEXT("data"));
		const FString OutputDir = FPaths::Combine(S->TempDir, TEXT("output"));
		const FString LogDir = FPaths::Combine(S->TempDir, TEXT("logs"));
		IFileManager::Get().MakeDirectory(*DataDir, true);
		IFileManager::Get().MakeDirectory(*OutputDir, true);
		IFileManager::Get().MakeDirectory(*LogDir, true);

		// 后端要载入工作流，缺了起不来。用可执行文件旁边的那份（只读）。
		const FString WorkflowsDir = FPaths::Combine(FPaths::GetPath(S->Exe), TEXT("workflows"));

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
			AutoStartPort, *OutputDir.Replace(TEXT("\\"), TEXT("/")), *DataDir.Replace(TEXT("\\"), TEXT("/")),
			*WorkflowsDir.Replace(TEXT("\\"), TEXT("/")), DeadComfyPort);
		const FString ConfigPath = FPaths::Combine(S->TempDir, TEXT("backend.json"));
		if (!FFileHelper::SaveStringToFile(Config, *ConfigPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Test->AddError(FString::Printf(TEXT("写不了临时配置：%s"), *ConfigPath));
			S->bAbort = true;
			return;
		}

		UDreamTextureSettings* Settings = GetMutableDefault<UDreamTextureSettings>();
		S->PreviousURL = Settings->BackendURL;
		S->PreviousExe = Settings->BackendExecutable.FilePath;
		S->PreviousArgs = Settings->BackendArguments;
		S->bOverridden = true;
		Settings->BackendURL = FString::Printf(TEXT("http://127.0.0.1:%d"), AutoStartPort);
		Settings->BackendExecutable.FilePath = S->Exe;
		Settings->BackendArguments = FString::Printf(TEXT("-config \"%s\" -log-dir \"%s\""), *ConfigPath, *LogDir);

		// 页面模型用默认后端函数：进程管理器、追踪器、连接、设置全是模块里那套真的。
		S->Model = MakeShared<FDreamTextureGenerateModel>();
		S->Model->Initialize(/*bBindExternal*/ true);

		// 全局连接重连到新地址；然后像面板打开时那样去拉目录——此刻 18793 上没有后端，会失败。
		S->Conn->Reconnect();
		S->Model->LoadCatalog();
	});

	// ---- 2. 页面此刻的样子：目录失败，后端进程没应答 ----
	EnqueueWait(Test, TEXT("目录请求失败、且连接判定「后端确实没应答」（不是还在连）"), 25.0,
		[S]()
		{
			if (S->bAbort)
			{
				return true;
			}
			return S->Model->GetCatalogState() != FDreamTextureGenerateModel::ECatalogState::Loading
				&& S->Model->GetPrimaryAction() == FDreamTextureGenerateModel::EPrimaryAction::StartAndSubmit;
		},
		[S, Test]()
		{
			if (S->bAbort)
			{
				return;
			}
			if (S->Model->GetCatalogState() == FDreamTextureGenerateModel::ECatalogState::Ready)
			{
				// 18793 上居然有人在应答：不是我们起的，不碰。
				S->bAbort = true;
				Test->AddInfo(FString::Printf(TEXT("已跳过：%d 上已经有后端在应答（不是本测试起的），不碰它。"), AutoStartPort));
				return;
			}

			Test->TestTrue(TEXT("目录没拉到"), S->Model->GetCatalogState() == FDreamTextureGenerateModel::ECatalogState::Failed);
			Test->TestFalse(TEXT("后端进程确实没应答"), S->Conn->IsBackendReachable());
			Test->TestTrue(TEXT("主按钮是「启动后端并生成」"),
				S->Model->GetPrimaryAction() == FDreamTextureGenerateModel::EPrimaryAction::StartAndSubmit);
			Test->TestTrue(TEXT("左栏该显示表单，而不是「读不到工作流列表」"), S->Model->ShouldShowOfflineForm());
			Test->TestFalse(TEXT("不需要去设置里配可执行文件"), S->Model->NeedsExecutableSetting());
			Test->TestTrue(TEXT("进程管理器此刻是空的"), S->Process->GetOrigin() == EDreamTextureBackendOrigin::None);
			Test->AddInfo(FString::Printf(TEXT("目录失败原因：%s"), *S->Model->GetCatalogError()));
		},
		[S]() { S->bAbort = true; });

	// ---- 3. 写表单、点主按钮 ----
	EnqueueDo([S, Test]()
	{
		if (S->bAbort)
		{
			return;
		}

		S->Model->SetPrompt(TEXT("mossy rocks"));
		S->Model->SetVariants(1);
		S->Model->SetRandomSeed(false);
		S->Model->SetFixedSeed(AutoStartSeed);

		if (!Test->TestTrue(TEXT("主按钮可点"), S->Model->CanActivatePrimary()))
		{
			S->bAbort = true;
			return;
		}

		S->bClicked = true;
		S->Model->ActivatePrimary();
		Test->TestTrue(TEXT("进入启动流程"), S->Model->IsAutoStarting());
		Test->TestTrue(TEXT("进程管理器在拉起后端"), S->Process->IsStarting());

		// 启动中再点：不会起第二个。
		S->Model->ActivatePrimary();
		S->Model->StartBackendAndSubmit();
		Test->TestFalse(TEXT("启动中主按钮不可点"), S->Model->CanActivatePrimary());
	});

	// 后端被拉起来，目录补拉到，提交成功——页面上多出一个变体。
	EnqueueWait(Test, TEXT("后端被拉起、目录补拉到、一个变体提交成功"), 120.0,
		[S]()
		{
			// 启动失败 / 提交失败时流程会回到 Idle 并留下原因：不用干等到超时。
			return S->bAbort || S->Model->GetRows().Num() > 0
				|| (!S->Model->IsAutoStarting() && !S->Model->IsSubmitting() && !S->Model->GetSubmitError().IsEmpty());
		},
		[S, Test]()
		{
			if (S->bAbort)
			{
				return;
			}
			if (S->Model->GetRows().Num() == 0)
			{
				Test->AddError(FString::Printf(TEXT("没有提交出任务。原因：%s"), *S->Model->GetSubmitError().ToString()));
				S->bAbort = true;
				return;
			}

			Test->TestTrue(TEXT("后端是插件拉起来的"), S->Process->GetOrigin() == EDreamTextureBackendOrigin::Launched);
			Test->TestTrue(TEXT("握着一个活着的进程"), S->Process->OwnsRunningProcess());
			S->Pid = S->Process->GetOwnedProcessId();
			Test->TestTrue(TEXT("pid 非零"), S->Pid != 0);
			Test->TestFalse(TEXT("进程管理器不再是启动中"), S->Process->IsStarting());
			Test->TestTrue(TEXT("启动流程走完"), S->Model->GetAutoStartState() == FDreamTextureGenerateModel::EAutoStartState::Idle);
			Test->TestTrue(TEXT("目录补拉到了"), S->Model->GetCatalogState() == FDreamTextureGenerateModel::ECatalogState::Ready);
			Test->TestTrue(TEXT("没有错误"), S->Model->GetSubmitError().IsEmpty());
			Test->TestEqual(TEXT("只提交了一个变体"), S->Model->GetRows().Num(), 1);
			Test->AddInfo(FString::Printf(TEXT("后端 pid %u，默认选择 %s × %s"), S->Pid, *S->Model->GetSourceId(), *S->Model->GetDecomposeId()));

			S->JobId = S->Model->GetRows()[0]->JobId;
			const FDreamTextureJob* Job = S->Tracker->Find(S->JobId);
			if (Test->TestNotNull(TEXT("追踪器里有这个任务"), Job))
			{
				Test->TestTrue(TEXT("读到了种子"), Job->bHasSeed);
				Test->TestEqual(TEXT("用户当时填的种子原样往返"), Job->Seed, AutoStartSeed);
				Test->TestEqual(TEXT("工作流是默认选择拼出的组合"), Job->WorkflowId,
					FDreamTextureWorkflowCatalog::MakeComposedId(S->Model->GetSourceId(), S->Model->GetDecomposeId()));
				Test->TestEqual(TEXT("展示名取自提示词"), Job->Name, FDreamTextureGenerateRequest::MakeNameFromPrompt(TEXT("mossy rocks")));
			}
		},
		[S]() { S->bAbort = true; });

	// ---- 4. 全局连接追上后端，事件把任务推到「等待 ComfyUI」 ----
	EnqueueWait(Test, TEXT("全局连接连上新后端，任务进入「等待 ComfyUI」（ComfyUI 是死的，任务排着等它）"), 60.0,
		[S]()
		{
			if (S->bAbort || S->JobId.IsEmpty())
			{
				return true;
			}
			const FDreamTextureJob* Job = S->Tracker->Find(S->JobId);
			return Job != nullptr && Job->Status == EDreamTextureJobStatus::Running && Job->Stage.Contains(TEXT("ComfyUI"));
		},
		[S, Test]()
		{
			if (S->bAbort || S->JobId.IsEmpty())
			{
				return;
			}
			const FDreamTextureJob* Job = S->Tracker->Find(S->JobId);
			if (Test->TestNotNull(TEXT("追踪器里有这个任务"), Job))
			{
				Test->AddInfo(FString::Printf(TEXT("阶段：%s"), *Job->Stage));
				const FDreamTextureGenerateRowView View = S->Model->GetRowView(S->Model->GetRows()[0]);
				Test->TestTrue(TEXT("卡片显示运行中"), View.Kind == FDreamTextureGenerateRowView::EKind::Running);
				Test->TestTrue(TEXT("阶段看得出来是在等 ComfyUI"), View.Stage.ToString().Contains(TEXT("ComfyUI")));
				Test->TestFalse(TEXT("没有真实进度：不画假进度"), View.Percent.IsSet());
				Test->TestTrue(TEXT("可以取消"), View.bCanCancel);
			}
			Test->TestTrue(TEXT("全局连接已连上新后端"), S->Conn->GetLink() == EDreamTextureLink::Live && S->Conn->IsBackendReachable());
		},
		[S]() { S->bAbort = true; });

	// ---- 5. 取消 ----
	EnqueueDo([S]()
	{
		if (!S->bAbort && !S->JobId.IsEmpty())
		{
			S->Model->CancelJob(S->JobId);
		}
	});
	EnqueueWait(Test, TEXT("任务被取消"), 30.0,
		[S]()
		{
			if (S->bAbort || S->JobId.IsEmpty())
			{
				return true;
			}
			const FDreamTextureJob* Job = S->Tracker->Find(S->JobId);
			return Job != nullptr && Job->IsTerminal();
		},
		[S, Test]()
		{
			if (S->bAbort || S->JobId.IsEmpty())
			{
				return;
			}
			const FDreamTextureJob* Job = S->Tracker->Find(S->JobId);
			if (Test->TestNotNull(TEXT("追踪器里有这个任务"), Job))
			{
				Test->TestTrue(TEXT("状态是 canceled"), Job->Status == EDreamTextureJobStatus::Canceled);
			}
		});

	// ---- 6. 停掉自己起的后端，端口释放 ----
	EnqueueDo([S, Test]()
	{
		// 没起过后端（前面跳过了 / 点之前就失败了）就没有东西可停。
		if (S->Process->GetOrigin() != EDreamTextureBackendOrigin::Launched)
		{
			return;
		}
		S->bStopped = Test->TestTrue(TEXT("StopOwned 确实停掉了一个进程"), S->Process->StopOwned());
		Test->TestTrue(TEXT("停掉后来源回到 None"), S->Process->GetOrigin() == EDreamTextureBackendOrigin::None);
		Test->TestFalse(TEXT("不再握有进程"), S->Process->OwnsRunningProcess());
		S->bClicked = false;
	});
	EnqueueWait(Test, TEXT("端口 18793 上不再有后端应答"), 20.0,
		[S]()
		{
			if (!S->bStopped)
			{
				return true;
			}
			if (S->ProbeResult.IsSet())
			{
				const bool bAnswering = S->ProbeResult.GetValue();
				S->ProbeResult.Reset();
				if (!bAnswering)
				{
					return true;
				}
			}
			S->KickProbe();
			return false;
		});

	// ---- 7. 收尾：还原设置、删临时目录（失败的话析构里也会做） ----
	EnqueueDo([S]()
	{
		S->Cleanup();
	});

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
