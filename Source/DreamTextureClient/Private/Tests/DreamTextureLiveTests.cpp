// Copyright TypeDreamMoon. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureApi.h"
#include "DreamTextureConnection.h"
#include "DreamTextureManifest.h"
#include "DreamTextureSettings.h"
#include "DreamTextureTestUtil.h"
#include "DreamTextureTypes.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"

/*
 * 连真后端的测试（DreamTexture.Live.*）。
 *
 * 只在环境变量 DREAMTEXTURE_LIVE_URL 有值时才跑，没有就跳过并说明。它指向「固定数据后端」：
 * Tools/FixtureBackend/Start-FixtureBackend.ps1 起在 127.0.0.1:18777，output 是 Tests/Fixtures 的拷贝，
 * ComfyUI 指向一个没人监听的端口（attach 模式）。Tools/Test.ps1 会替你起它并设好环境变量。
 *
 * 测试期间临时把 UDreamTextureSettings::BackendURL 改成这个地址，结束（或中途失败）时还原；
 * 只改内存里的 CDO，不写任何 ini。
 *
 * 全是 latent 命令链：每一步各带超时，卡住时报告能直接说出卡在哪一步，而不是笼统的「整个测试超时」。
 */

namespace
{
	using namespace DreamTextureTest;

	double Now()
	{
		return FPlatformTime::Seconds();
	}

	/** 排一条 latent 命令。等价于 ADD_LATENT_AUTOMATION_COMMAND，只是不用在每个调用点重复写 MakeShareable。 */
	void LiveEnqueue(const TSharedPtr<IAutomationLatentCommand>& Command)
	{
		FAutomationTestFramework::Get().EnqueueLatentCommand(Command);
	}

	void LiveEnqueueDo(TFunction<void()> Action)
	{
		LiveEnqueue(MakeShareable(new FDreamTextureDoCommand(MoveTemp(Action))));
	}

	void LiveEnqueueWait(FAutomationTestBase* Test, const FString& What, double TimeoutSecs, TFunction<bool()> IsDone,
		TFunction<void()> OnDone = nullptr, TFunction<void()> OnTimeout = nullptr)
	{
		LiveEnqueue(MakeShareable(new FDreamTextureWaitCommand(Test, What, TimeoutSecs, MoveTemp(IsDone), MoveTemp(OnDone), MoveTemp(OnTimeout))));
	}

	/**
	 * 排一个「发起异步操作 → 等它的回调 → 检查结果」的两步。
	 * Start 收到一个 Complete 函数，在异步回调里调用它表示操作结束。
	 */
	void EnqueueAsyncStep(FAutomationTestBase* Test, const FString& What, double TimeoutSecs,
		TFunction<void(TFunction<void()> /*Complete*/)> Start, TFunction<void()> Check)
	{
		const TSharedRef<bool> bComplete = MakeShared<bool>(false);
		LiveEnqueueDo([Start = MoveTemp(Start), bComplete]()
		{
			Start([bComplete]() { *bComplete = true; });
		});
		LiveEnqueueWait(Test, What, TimeoutSecs, [bComplete]() { return *bComplete; }, MoveTemp(Check));
	}

	/** 脚本进程是否已经结束；结束时取退出码并关掉句柄。没有句柄视为已结束（退出码 -1）。 */
	bool PollProcess(FProcHandle& Proc, int32& OutExitCode)
	{
		if (!Proc.IsValid())
		{
			OutExitCode = -1;
			return true;
		}
		if (FPlatformProcess::IsProcRunning(Proc))
		{
			return false;
		}
		OutExitCode = -1;
		FPlatformProcess::GetProcReturnCode(Proc, &OutExitCode);
		FPlatformProcess::CloseProc(Proc);
		return true;
	}

	// ======================================================================
	// DreamTexture.Live.Connection
	// ======================================================================

	struct FLiveConnectionState
	{
		FString LiveURL;
		TUniquePtr<FBackendUrlOverride> UrlOverride;
		TSharedPtr<FDreamTextureConnection> Conn;
		FProcHandle Proc;

		/** 前面某一步失败到没法继续了：后续的检查步骤直接跳过，但清理步骤照常执行。 */
		bool bAbort = false;

		/** 我们停了后端、还没确认它回来了。清理阶段据此补一次 Start，免得把环境留成停着的。 */
		bool bBackendStopped = false;

		bool bHealthSeen = false;
		FDreamTextureHealth LastHealth;

		/** 最近一个脚本进程的退出码。进程结束时由 PollScript 记下，因为句柄一结束就关掉了。 */
		int32 LastExitCode = -1;

		double T0 = 0.0;
		double StopLaunchedAt = 0.0;
		double StopDoneAt = 0.0;
		double LeftLiveAt = -1.0;
		double UnreachableAt = -1.0;
		double StartDoneAt = 0.0;

		TArray<FString> LinkLog;

		/** 脚本是否已经结束，结束时顺手记下退出码。 */
		bool PollScript()
		{
			return PollProcess(Proc, LastExitCode);
		}

		/**
		 * 记下「第一次看到链路离开 Live」和「第一次看到后端不可达」的时刻。
		 * 在等停止脚本的那一段也要调：脚本要几秒才返回，只在它返回后才开始观察的话，
		 * 报出来的时间会被脚本耗时垫高，看不出连接层自己多快发现的。
		 */
		void ObserveOutage()
		{
			if (StopLaunchedAt <= 0.0 || !Conn.IsValid())
			{
				return;
			}
			if (Conn->GetLink() != EDreamTextureLink::Live && LeftLiveAt < 0.0)
			{
				LeftLiveAt = Now();
			}
			if (!Conn->IsBackendReachable() && UnreachableAt < 0.0)
			{
				UnreachableAt = Now();
			}
		}

		~FLiveConnectionState()
		{
			if (Conn.IsValid())
			{
				Conn->Stop();
			}
			if (Proc.IsValid())
			{
				FPlatformProcess::TerminateProc(Proc);
				FPlatformProcess::CloseProc(Proc);
			}
		}
	};

	void RunLiveConnection(FAutomationTestBase* Test, const FString& LiveURL)
	{
		const TSharedRef<FLiveConnectionState> S = MakeShared<FLiveConnectionState>();
		S->LiveURL = LiveURL;

		// ---- 1. 建连接，指向固定数据后端 ----
		LiveEnqueueDo([S]()
		{
			S->UrlOverride = MakeUnique<FBackendUrlOverride>(S->LiveURL);
			S->Conn = MakeShared<FDreamTextureConnection>();

			// lambda 里只拿弱引用：状态持有连接、连接持有委托，委托再强引用状态就是个环，永远释放不掉。
			const TWeakPtr<FLiveConnectionState> Weak = S;
			S->Conn->OnHealthChanged.AddLambda([Weak](const FDreamTextureHealth& Health)
			{
				if (const TSharedPtr<FLiveConnectionState> State = Weak.Pin())
				{
					State->bHealthSeen = true;
					State->LastHealth = Health;
				}
			});
			S->Conn->OnLinkChanged.AddLambda([Weak](EDreamTextureLink Link)
			{
				if (const TSharedPtr<FLiveConnectionState> State = Weak.Pin())
				{
					State->LinkLog.Add(FString::Printf(TEXT("+%.2fs %s"), Now() - State->T0, LexToString(Link)));
					if (Link != EDreamTextureLink::Live && State->StopLaunchedAt > 0.0 && State->LeftLiveAt < 0.0)
					{
						State->LeftLiveAt = Now();
					}
				}
			});

			S->T0 = Now();
			S->Conn->Start();
		});

		// ---- 2. 到达 Live，并且是「后端在、ComfyUI 不在」 ----
		LiveEnqueueWait(Test, TEXT("链路到达 Live。后端必须已经在跑：先执行 Tools/FixtureBackend/Start-FixtureBackend.ps1"), 20.0,
			[S]() { return S->Conn->GetLink() == EDreamTextureLink::Live; },
			[S, Test]()
			{
				Test->TestTrue(TEXT("到达 Live 时 IsBackendReachable() 为真"), S->Conn->IsBackendReachable());
				Test->AddInfo(FString::Printf(TEXT("启动后 %.1f 秒到达 Live"), Now() - S->T0));
			},
			[S, Test]()
			{
				S->bAbort = true;
				Test->AddError(FString::Printf(TEXT("没能到达 Live。链路 %s，后端可达 %s，最近错误：%s"),
					LexToString(S->Conn->GetLink()), S->Conn->IsBackendReachable() ? TEXT("是") : TEXT("否"), *S->Conn->GetLastError()));
			});

		LiveEnqueueWait(Test, TEXT("收到第一条 comfy.status（Health）"), 5.0,
			[S]() { return S->bAbort || S->bHealthSeen; },
			[S, Test]()
			{
				if (S->bAbort)
				{
					return;
				}
				const FDreamTextureHealth& Health = S->LastHealth;
				Test->AddInfo(FString::Printf(TEXT("Health：mode=%s alive=%d ready=%d reason=%s"),
					*Health.Mode, Health.bAlive, Health.bReady, *Health.Reason));

				// 固定数据后端把 ComfyUI 指向没人监听的端口：后端进程在、WS 通，ComfyUI 不在。
				// 这正是 Docs/contributing.md 要求的三级状态分开显示的那个场景。
				Test->TestFalse(TEXT("ComfyUI 应为不在线（bAlive）"), Health.bAlive);
				Test->TestFalse(TEXT("ComfyUI 应为未就绪（bReady）"), Health.bReady);
				Test->TestTrue(TEXT("链路仍是 Live：ComfyUI 不在不影响长连接"), S->Conn->GetLink() == EDreamTextureLink::Live);
				Test->TestTrue(TEXT("后端仍可达"), S->Conn->IsBackendReachable());
			});

		// ---- 3. 停掉后端 ----
		LiveEnqueueDo([S, Test]()
		{
			if (S->bAbort)
			{
				return;
			}
			if (!FixtureScriptExists(TEXT("Stop-FixtureBackend.ps1")))
			{
				S->bAbort = true;
				Test->AddError(TEXT("找不到 Tools/FixtureBackend/Stop-FixtureBackend.ps1"));
				return;
			}

			S->Proc = LaunchFixtureScript(TEXT("Stop-FixtureBackend.ps1"), S->LiveURL);
			if (!S->Proc.IsValid())
			{
				S->bAbort = true;
				Test->AddError(TEXT("拉不起 pwsh.exe 来执行 Stop-FixtureBackend.ps1（PATH 里有 PowerShell 7 吗？）"));
				return;
			}
			S->bBackendStopped = true;
			S->StopLaunchedAt = Now();
			S->bHealthSeen = false;
		});

		LiveEnqueueWait(Test, TEXT("Stop-FixtureBackend.ps1 执行结束"), 60.0,
			[S]()
			{
				S->ObserveOutage();
				return S->bAbort || S->PollScript();
			},
			[S, Test]()
			{
				if (S->bAbort)
				{
					return;
				}
				S->StopDoneAt = Now();
				Test->AddInfo(FString::Printf(TEXT("停止脚本用了 %.1f 秒，退出码 %d"), S->StopDoneAt - S->StopLaunchedAt, S->LastExitCode));
				Test->TestEqual(TEXT("Stop-FixtureBackend.ps1 退出码"), S->LastExitCode, 0);
			},
			[S]()
			{
				// 脚本自己卡住了：别留着它。后面的清理会再补一次 Start。
				S->bAbort = true;
			});

		// ---- 4. 10 秒内离开 Live，并且后端变为不可达 ----
		// 判据是计时从「停止脚本返回」算起的 10 秒。后端进程一死，OS 就会关掉 WebSocket，
		// 所以离开 Live 通常在脚本返回之前就发生了；不可达要等下一次 health 探针失败（退避 1 秒）。
		LiveEnqueueWait(Test, TEXT("停掉后端后链路离开 Live 且 IsBackendReachable() 变假（10 秒内）"), 10.0,
			[S]()
			{
				if (S->bAbort)
				{
					return true;
				}
				S->ObserveOutage();
				return S->Conn->GetLink() != EDreamTextureLink::Live && !S->Conn->IsBackendReachable();
			},
			[S, Test]()
			{
				if (S->bAbort)
				{
					return;
				}
				Test->AddInfo(FString::Printf(TEXT("停止脚本启动后：%.1f 秒离开 Live；%.1f 秒后端被判为不可达（停止脚本 %.1f 秒时返回）"),
					S->LeftLiveAt - S->StopLaunchedAt, S->UnreachableAt - S->StopLaunchedAt, S->StopDoneAt - S->StopLaunchedAt));
			},
			[S, Test]()
			{
				Test->AddError(FString::Printf(TEXT("停掉后端 10 秒后：链路 %s，后端可达 %s，最近错误：%s"),
					LexToString(S->Conn->GetLink()), S->Conn->IsBackendReachable() ? TEXT("是") : TEXT("否"), *S->Conn->GetLastError()));
			});

		// ---- 5. 先让后端停一阵，把退避涨到上限 ----
		// 刚停就重启的话，退避还停在 1~2 秒，重连几乎立刻成功——那样测不到「上限」这个行为。
		// 退避序列是 1、2、4、8 秒然后封顶 15 秒；再算上 Windows 上本机连接被拒绝要等约 2 秒才报错，
		// 25 秒足够让它涨到封顶，之后恢复要等的就是最坏的那一档。
		constexpr double OutageHoldSecs = 25.0;
		const TSharedRef<double> HoldStart = MakeShared<double>(0.0);
		LiveEnqueueWait(Test, TEXT("让后端保持停止，使退避涨到上限"), OutageHoldSecs + 30.0,
			[S, HoldStart]()
			{
				if (S->bAbort)
				{
					return true;
				}
				if (*HoldStart == 0.0)
				{
					*HoldStart = Now();
				}
				return Now() - *HoldStart >= OutageHoldSecs;
			},
			[S, Test]()
			{
				if (!S->bAbort)
				{
					Test->TestTrue(TEXT("停着的这段时间里链路没有凭空回到 Live"), S->Conn->GetLink() != EDreamTextureLink::Live);
					Test->TestFalse(TEXT("停着的这段时间里后端始终不可达"), S->Conn->IsBackendReachable());
				}
			});

		// ---- 6. 重新起后端。**不调用 Reconnect()**，看退避重连能不能自己回来 ----
		LiveEnqueueDo([S, Test]()
		{
			if (S->bAbort)
			{
				return;
			}
			if (!FixtureScriptExists(TEXT("Start-FixtureBackend.ps1")))
			{
				S->bAbort = true;
				Test->AddError(TEXT("找不到 Tools/FixtureBackend/Start-FixtureBackend.ps1"));
				return;
			}

			S->Proc = LaunchFixtureScript(TEXT("Start-FixtureBackend.ps1"), S->LiveURL);
			if (!S->Proc.IsValid())
			{
				S->bAbort = true;
				Test->AddError(TEXT("拉不起 pwsh.exe 来执行 Start-FixtureBackend.ps1"));
				return;
			}
			S->bHealthSeen = false;
		});

		LiveEnqueueWait(Test, TEXT("Start-FixtureBackend.ps1 执行结束（后端应答 /api/health 后才返回）"), 90.0,
			[S]() { return S->bAbort || S->PollScript(); },
			[S, Test]()
			{
				if (S->bAbort)
				{
					return;
				}
				S->StartDoneAt = Now();
				if (S->LastExitCode == 0)
				{
					S->bBackendStopped = false;
				}
				else
				{
					// 起不来就没必要等重连了，清理阶段会再试一次。
					S->bAbort = true;
					Test->AddError(FString::Printf(TEXT("Start-FixtureBackend.ps1 退出码 %d，后端没能重新起来"), S->LastExitCode));
				}
			},
			[S]() { S->bAbort = true; });

		// 退避上限默认 15 秒；再给 10 秒余量，覆盖 health 探针 + WS 握手 + 机器偶尔的抖动。
		const double RecoverTimeout = FMath::Max(200, GetDefault<UDreamTextureSettings>()->ReconnectMaxMs) / 1000.0 + 10.0;
		LiveEnqueueWait(Test,
			FString::Printf(TEXT("后端回来后，**不调用 Reconnect** 也自己回到 Live（退避上限 + 10 秒 = %.0f 秒）"), RecoverTimeout), RecoverTimeout,
			[S]() { return S->bAbort || (S->Conn->GetLink() == EDreamTextureLink::Live && S->Conn->IsBackendReachable()); },
			[S, Test]()
			{
				if (!S->bAbort)
				{
					Test->AddInfo(FString::Printf(TEXT("后端应答后 %.1f 秒自动回到 Live（没有调用 Reconnect）"), Now() - S->StartDoneAt));
				}
			},
			[S, Test]()
			{
				Test->AddError(FString::Printf(TEXT("后端已经回来，链路 %s，后端可达 %s，最近错误：%s"),
					LexToString(S->Conn->GetLink()), S->Conn->IsBackendReachable() ? TEXT("是") : TEXT("否"), *S->Conn->GetLastError()));
			});

		LiveEnqueueWait(Test, TEXT("重连后再次收到 comfy.status"), 5.0,
			[S]() { return S->bAbort || S->bHealthSeen; },
			[S, Test]()
			{
				if (!S->bAbort)
				{
					Test->TestFalse(TEXT("重连后 ComfyUI 仍应是不在线"), S->LastHealth.bAlive);
				}
			});

		// ---- 7. 清理：该补的补，该还原的还原 ----
		LiveEnqueueDo([S, Test]()
		{
			if (S->bBackendStopped && FixtureScriptExists(TEXT("Start-FixtureBackend.ps1")))
			{
				// 测试中途失败、后端被我们停着：补一次 Start，别把环境留成停着的。
				Test->AddInfo(TEXT("清理：测试中途失败，重新启动固定数据后端"));
				S->Proc = LaunchFixtureScript(TEXT("Start-FixtureBackend.ps1"), S->LiveURL);
			}
		});
		LiveEnqueueWait(Test, TEXT("清理阶段重启后端"), 90.0,
			[S]() { return S->PollScript(); },
			[S, Test]()
			{
				if (S->LastExitCode > 0)
				{
					Test->AddError(FString::Printf(TEXT("清理阶段重启后端失败，退出码 %d。固定数据后端现在可能是停着的"), S->LastExitCode));
				}
			});
		LiveEnqueueDo([S, Test]()
		{
			if (S->Conn.IsValid())
			{
				S->Conn->Stop();
				Test->TestTrue(TEXT("Stop() 之后链路是 Offline"), S->Conn->GetLink() == EDreamTextureLink::Offline);
				Test->TestFalse(TEXT("Stop() 之后 IsBackendReachable() 为假"), S->Conn->IsBackendReachable());
			}
			Test->AddInfo(FString::Printf(TEXT("链路迁移记录：%s"), *FString::Join(S->LinkLog, TEXT(" → "))));

			S->Conn.Reset();
			S->UrlOverride.Reset();
		});
	}

	// ======================================================================
	// DreamTexture.Live.Fetch
	// ======================================================================

	/** 一次 DownloadMaps 的观察记录。 */
	struct FObservedDownload
	{
		int32 DoneCount = 0;
		FDreamTextureFetchResult Result;
		TArray<TPair<int32, int32>> Progress;
	};

	struct FLiveFetchState
	{
		TUniquePtr<FBackendUrlOverride> UrlOverride;

		/** 本地固定素材的 manifest：下载回来的东西拿它当标准答案。 */
		FDreamTextureManifest Local;

		/** 从后端拉到的 manifest。 */
		FDreamTextureManifest Remote;
		bool bRemoteOK = false;
		FString RemoteError;

		FString DownloadDir;

		/** 测试期间把「导入分通道图」钉成关，结束（或中途失败）时还原。 */
		bool bSavedSeparateChannels = false;
		bool bHaveSavedSeparateChannels = false;

		~FLiveFetchState()
		{
			if (bHaveSavedSeparateChannels)
			{
				GetMutableDefault<UDreamTextureSettings>()->bImportSeparateChannels = bSavedSeparateChannels;
			}
		}
	};

	TSharedRef<FObservedDownload> StartDownload(const FDreamTextureManifest& Manifest, const FDreamTextureFetchOptions& Options,
		const TFunction<void()>& Complete)
	{
		const TSharedRef<FObservedDownload> Observed = MakeShared<FObservedDownload>();
		FDreamTextureFetcher::DownloadMaps(Manifest, Options,
			[Observed, Complete](FDreamTextureFetchResult&& Result)
			{
				++Observed->DoneCount;
				Observed->Result = MoveTemp(Result);
				Complete();
			},
			[Observed](int32 Done, int32 Total)
			{
				Observed->Progress.Emplace(Done, Total);
			});
		return Observed;
	}

	void RunLiveFetch(FAutomationTestBase* Test, const FString& LiveURL, const FDreamTextureManifest& Local)
	{
		const TSharedRef<FLiveFetchState> S = MakeShared<FLiveFetchState>();
		S->Local = Local;
		S->DownloadDir = FDreamTextureFetcher::GetDownloadDir(FixtureId());

		LiveEnqueueDo([S, LiveURL]()
		{
			S->UrlOverride = MakeUnique<FBackendUrlOverride>(LiveURL);

			// 默认通道取决于「导入分通道图」设置，钉成关；还原由状态的析构负责。
			UDreamTextureSettings* Settings = GetMutableDefault<UDreamTextureSettings>();
			S->bSavedSeparateChannels = Settings->bImportSeparateChannels;
			S->bHaveSavedSeparateChannels = true;
			Settings->bImportSeparateChannels = false;

			// 每次从干净的下载目录开始，不然上一次的残留会让「文件都在」这个断言变成假阳性。
			IFileManager::Get().DeleteDirectory(*S->DownloadDir, false, true);
		});

		// ---- GetManifest ----
		EnqueueAsyncStep(Test, TEXT("GetManifest 的回调"), 15.0,
			[S](TFunction<void()> Complete)
			{
				FDreamTextureApi::GetManifest(FixtureId(), [S, Complete](bool bOK, FDreamTextureManifest&& Manifest, const FString& Error)
				{
					S->bRemoteOK = bOK;
					S->RemoteError = Error;
					if (bOK)
					{
						S->Remote = MoveTemp(Manifest);
					}
					Complete();
				});
			},
			[S, Test]()
			{
				if (!Test->TestTrue(FString::Printf(TEXT("GetManifest 成功（连不上多半是固定数据后端没起，见 Tools/FixtureBackend）：%s"), *S->RemoteError), S->bRemoteOK))
				{
					return;
				}
				Test->TestEqual(TEXT("manifest.id"), S->Remote.Id, FixtureId());
				Test->TestEqual(TEXT("通道数"), S->Remote.Maps.Num(), 7);
				Test->TestEqual(TEXT("seed"), S->Remote.Seed, S->Local.Seed);
				Test->TestFalse(TEXT("固定素材没有 source"), S->Remote.bHasCloudSource);
				for (const TPair<FString, FDreamTextureMapInfo>& Pair : S->Local.Maps)
				{
					const FDreamTextureMapInfo* Remote = S->Remote.FindMap(Pair.Key);
					if (Test->TestNotNull(FString::Printf(TEXT("后端的 manifest 里有 %s"), *Pair.Key), Remote))
					{
						Test->TestEqual(FString::Printf(TEXT("%s 的 sha256 与本地固定素材一致"), *Pair.Key), Remote->Sha256, Pair.Value.Sha256);
					}
				}
			});

		// ---- 默认通道全量下载 ----
		const TSharedRef<TSharedPtr<FObservedDownload>> FullRun = MakeShared<TSharedPtr<FObservedDownload>>();
		EnqueueAsyncStep(Test, TEXT("默认通道下载的 Done 回调"), 30.0,
			[S, FullRun](TFunction<void()> Complete)
			{
				if (!S->bRemoteOK)
				{
					Complete();
					return;
				}
				*FullRun = StartDownload(S->Remote, FDreamTextureFetchOptions(), Complete);
			},
			[S, Test, FullRun]()
			{
				if (!FullRun->IsValid())
				{
					return;
				}
				const FObservedDownload& Observed = **FullRun;
				const FDreamTextureFetchResult& Result = Observed.Result;

				Test->TestTrue(FString::Printf(TEXT("[全量下载] bOK 为真（%s）"), *Result.Error), Result.bOK);
				Test->TestEqual(TEXT("[全量下载] Done 只回调一次"), Observed.DoneCount, 1);
				Test->TestEqual(TEXT("[全量下载] 落盘目录"), Result.LocalDir, S->DownloadDir);
				Test->TestEqual(TEXT("[全量下载] 默认 4 个通道都拿到"), Result.LocalFiles.Num(), 4);
				Test->TestEqual(TEXT("[全量下载] 没有 UpToDate"), Result.UpToDate.Num(), 0);
				Test->TestEqual(TEXT("[全量下载] 没有 Skipped"), Result.Skipped.Num(), 0);

				// Progress：每个文件一次，次数 = 文件数，最后一次是 (N, N)，且计数单调递增。
				Test->TestEqual(TEXT("[全量下载] Progress 每个文件回调一次"), Observed.Progress.Num(), 4);
				for (int32 Index = 0; Index < Observed.Progress.Num(); ++Index)
				{
					Test->TestEqual(FString::Printf(TEXT("[全量下载] Progress[%d] 已完成数"), Index), Observed.Progress[Index].Key, Index + 1);
					Test->TestEqual(FString::Printf(TEXT("[全量下载] Progress[%d] 总数"), Index), Observed.Progress[Index].Value, 4);
				}

				// 文件都在、哈希都对；并且逐字节等于本地固定素材（后端的 output 就是它的拷贝）。
				for (const TPair<FString, FString>& Pair : Result.LocalFiles)
				{
					const FDreamTextureMapInfo* Info = S->Local.FindMap(Pair.Key);
					TArray<uint8> OnDisk;
					TArray<uint8> Reference;
					if (!Test->TestNotNull(FString::Printf(TEXT("[全量下载] %s 在 manifest 里"), *Pair.Key), Info)
						|| !Test->TestTrue(FString::Printf(TEXT("[全量下载] %s 已落盘：%s"), *Pair.Key, *Pair.Value), FFileHelper::LoadFileToArray(OnDisk, *Pair.Value)))
					{
						continue;
					}
					Test->TestEqual(FString::Printf(TEXT("[全量下载] %s 的 sha256"), *Pair.Key), FDreamTextureFetcher::HashBytes16(OnDisk), Info->Sha256);
					if (LoadFixtureBytes(Info->File, Reference))
					{
						Test->TestTrue(FString::Printf(TEXT("[全量下载] %s 与本地固定素材逐字节一致"), *Pair.Key), OnDisk == Reference);
					}
					Test->TestTrue(FString::Printf(TEXT("[全量下载] %s 落在下载目录里"), *Pair.Key), Pair.Value.StartsWith(S->DownloadDir));
				}

				// 目录里不留临时文件。
				TArray<FString> Leftovers;
				IFileManager::Get().FindFiles(Leftovers, *FPaths::Combine(S->DownloadDir, TEXT("*.part")), true, false);
				Test->TestEqual(TEXT("[全量下载] 没有残留的 .part"), Leftovers.Num(), 0);
			});

		// ---- 指定通道：分通道图 + 一个 manifest 里没有的通道 ----
		const TSharedRef<TSharedPtr<FObservedDownload>> SplitRun = MakeShared<TSharedPtr<FObservedDownload>>();
		EnqueueAsyncStep(Test, TEXT("指定通道下载的 Done 回调"), 30.0,
			[S, SplitRun](TFunction<void()> Complete)
			{
				if (!S->bRemoteOK)
				{
					Complete();
					return;
				}
				FDreamTextureFetchOptions Options;
				Options.Channels = { DreamTextureChannel::Roughness, DreamTextureChannel::Metallic, DreamTextureChannel::AO, TEXT("emissive") };
				*SplitRun = StartDownload(S->Remote, Options, Complete);
			},
			[Test, SplitRun]()
			{
				if (!SplitRun->IsValid())
				{
					return;
				}
				const FDreamTextureFetchResult& Result = (**SplitRun).Result;
				Test->TestTrue(FString::Printf(TEXT("[指定通道] 缺一个 manifest 里没有的通道不算失败（%s）"), *Result.Error), Result.bOK);
				Test->TestEqual(TEXT("[指定通道] 拿到 3 个"), Result.LocalFiles.Num(), 3);
				Test->TestTrue(TEXT("[指定通道] emissive 记进 Skipped"), Result.Skipped.Contains(TEXT("emissive")));
				Test->TestEqual(TEXT("[指定通道] Skipped 里只有 emissive"), Result.Skipped.Num(), 1);
				Test->TestEqual(TEXT("[指定通道] Progress 只算真的下载的 3 个"), (**SplitRun).Progress.Num(), 3);
			});

		// ---- KnownHashes 全部一致：一个文件都不下 ----
		const TSharedRef<TSharedPtr<FObservedDownload>> AllKnownRun = MakeShared<TSharedPtr<FObservedDownload>>();
		EnqueueAsyncStep(Test, TEXT("全部已知哈希的 Done 回调"), 15.0,
			[S, AllKnownRun](TFunction<void()> Complete)
			{
				if (!S->bRemoteOK)
				{
					Complete();
					return;
				}
				// 先把下载目录删干净：之后目录若又出现了，就说明确实有东西被下载了。
				IFileManager::Get().DeleteDirectory(*S->DownloadDir, false, true);

				FDreamTextureFetchOptions Options;
				for (const TPair<FString, FDreamTextureMapInfo>& Pair : S->Remote.Maps)
				{
					Options.KnownHashes.Add(Pair.Key, Pair.Value.Sha256);
				}
				*AllKnownRun = StartDownload(S->Remote, Options, Complete);
			},
			[S, Test, AllKnownRun]()
			{
				if (!AllKnownRun->IsValid())
				{
					return;
				}
				const FObservedDownload& Observed = **AllKnownRun;
				Test->TestTrue(FString::Printf(TEXT("[全部已知] bOK 为真（%s）"), *Observed.Result.Error), Observed.Result.bOK);
				Test->TestEqual(TEXT("[全部已知] 4 个默认通道全是 UpToDate"), Observed.Result.UpToDate.Num(), 4);
				Test->TestEqual(TEXT("[全部已知] 一个文件都没下"), Observed.Result.LocalFiles.Num(), 0);
				Test->TestEqual(TEXT("[全部已知] 没有 Skipped"), Observed.Result.Skipped.Num(), 0);
				Test->TestEqual(TEXT("[全部已知] 没有 Progress"), Observed.Progress.Num(), 0);
				Test->TestFalse(TEXT("[全部已知] 下载目录没有被重新建出来 = 确实什么都没写"), IFileManager::Get().DirectoryExists(*S->DownloadDir));
			});

		// ---- KnownHashes 部分一致：只下剩下的 ----
		const TSharedRef<TSharedPtr<FObservedDownload>> PartialRun = MakeShared<TSharedPtr<FObservedDownload>>();
		EnqueueAsyncStep(Test, TEXT("部分已知哈希的 Done 回调"), 30.0,
			[S, PartialRun](TFunction<void()> Complete)
			{
				if (!S->bRemoteOK)
				{
					Complete();
					return;
				}
				FDreamTextureFetchOptions Options;
				for (const FString& Channel : { DreamTextureChannel::BaseColor, DreamTextureChannel::Normal })
				{
					Options.KnownHashes.Add(Channel, S->Remote.FindMap(Channel)->Sha256);
				}
				*PartialRun = StartDownload(S->Remote, Options, Complete);
			},
			[Test, PartialRun]()
			{
				if (!PartialRun->IsValid())
				{
					return;
				}
				const FObservedDownload& Observed = **PartialRun;
				Test->TestTrue(FString::Printf(TEXT("[部分已知] bOK 为真（%s）"), *Observed.Result.Error), Observed.Result.bOK);
				Test->TestEqual(TEXT("[部分已知] basecolor / normal 是 UpToDate"), Observed.Result.UpToDate.Num(), 2);
				Test->TestTrue(TEXT("[部分已知] 只下了 orm 和 height"),
					Observed.Result.LocalFiles.Num() == 2 && Observed.Result.LocalFiles.Contains(DreamTextureChannel::ORM) && Observed.Result.LocalFiles.Contains(DreamTextureChannel::Height));
				Test->TestEqual(TEXT("[部分已知] Progress 只算下载的 2 个"), Observed.Progress.Num(), 2);
			});

		// ---- 哈希不符：下回来的内容与 manifest 对不上，必须拒收且不留文件 ----
		const TSharedRef<TSharedPtr<FObservedDownload>> TamperedRun = MakeShared<TSharedPtr<FObservedDownload>>();
		EnqueueAsyncStep(Test, TEXT("篡改哈希下载的 Done 回调"), 30.0,
			[S, TamperedRun](TFunction<void()> Complete)
			{
				if (!S->bRemoteOK)
				{
					Complete();
					return;
				}
				// 用一份被改过 height 哈希的 manifest 去下：后端送回来的内容是对的，但「manifest 说它应该是另一个哈希」，
				// 效果等同于内容被损坏——这是端到端地验证校验这道门真的会拦。
				IFileManager::Get().DeleteDirectory(*S->DownloadDir, false, true);

				FDreamTextureManifest Tampered = S->Remote;
				FString& Hash = Tampered.Maps.FindChecked(DreamTextureChannel::Height).Sha256;
				Hash = Hash.Left(Hash.Len() - 1) + (Hash.EndsWith(TEXT("0")) ? TEXT("1") : TEXT("0"));

				FDreamTextureFetchOptions Options;
				Options.Channels = { DreamTextureChannel::Height, DreamTextureChannel::ORM };
				*TamperedRun = StartDownload(Tampered, Options, Complete);
			},
			[S, Test, TamperedRun]()
			{
				if (!TamperedRun->IsValid())
				{
					return;
				}
				const FDreamTextureFetchResult& Result = (**TamperedRun).Result;
				const FString HeightPath = FPaths::Combine(S->DownloadDir, TEXT("height.png"));
				Test->TestFalse(TEXT("[哈希不符] bOK 为假"), Result.bOK);
				Test->TestEqual(TEXT("[哈希不符] Done 只回调一次"), (**TamperedRun).DoneCount, 1);
				Test->TestTrue(TEXT("[哈希不符] height 记进 Skipped"), Result.Skipped.Contains(DreamTextureChannel::Height));
				Test->TestTrue(TEXT("[哈希不符] 原因提到 sha256"), Result.Skipped.FindRef(DreamTextureChannel::Height).Contains(TEXT("sha256")));
				Test->TestFalse(TEXT("[哈希不符] height 没有进 LocalFiles"), Result.LocalFiles.Contains(DreamTextureChannel::Height));
				Test->TestFalse(TEXT("[哈希不符] height.png 没有出现在磁盘上"), IFileManager::Get().FileExists(*HeightPath));
				Test->TestTrue(TEXT("[哈希不符] 好的那个（orm）照常拿到"), Result.LocalFiles.Contains(DreamTextureChannel::ORM));

				TArray<FString> Leftovers;
				IFileManager::Get().FindFiles(Leftovers, *FPaths::Combine(S->DownloadDir, TEXT("*.part")), true, false);
				Test->TestEqual(TEXT("[哈希不符] 没有残留的 .part"), Leftovers.Num(), 0);
			});

		// ---- ListMaterials ----
		// 固定数据后端没有素材索引：返回空数组或 null 都是对的，正好测「空列表」的容错。
		const TSharedRef<bool> bListOK = MakeShared<bool>(false);
		const TSharedRef<int32> ListCount = MakeShared<int32>(-1);
		const TSharedRef<FString> ListError = MakeShared<FString>();
		EnqueueAsyncStep(Test, TEXT("ListMaterials 的回调"), 15.0,
			[bListOK, ListCount, ListError](TFunction<void()> Complete)
			{
				FDreamTextureApi::ListMaterials(FDreamTextureMaterialQuery(),
					[=](bool bOK, TArray<FDreamTextureMaterialSummary>&& Materials, const FString& Error)
					{
						*bListOK = bOK;
						*ListCount = Materials.Num();
						*ListError = Error;
						Complete();
					});
			},
			[Test, bListOK, ListCount, ListError]()
			{
				Test->TestTrue(FString::Printf(TEXT("ListMaterials 成功（空列表也算成功）：%s"), **ListError), *bListOK);
				Test->AddInfo(FString::Printf(TEXT("ListMaterials 返回 %d 项"), *ListCount));
			});

		// 带全部筛选参数、带中文：确认查询串拼得对（后端不会因为参数形状报 400/500）。
		const TSharedRef<bool> bFilteredOK = MakeShared<bool>(false);
		const TSharedRef<FString> FilteredError = MakeShared<FString>();
		EnqueueAsyncStep(Test, TEXT("带筛选的 ListMaterials 回调"), 15.0,
			[bFilteredOK, FilteredError](TFunction<void()> Complete)
			{
				FDreamTextureMaterialQuery Query;
				Query.Text = TEXT("苔藓 rocks");
				Query.Style = TEXT("realistic");
				Query.bFavoriteOnly = true;
				Query.Limit = 10;
				Query.Offset = 0;
				FDreamTextureApi::ListMaterials(Query,
					[=](bool bOK, TArray<FDreamTextureMaterialSummary>&&, const FString& Error)
					{
						*bFilteredOK = bOK;
						*FilteredError = Error;
						Complete();
					});
			},
			[Test, bFilteredOK, FilteredError]()
			{
				Test->TestTrue(FString::Printf(TEXT("带筛选和中文的 ListMaterials 成功：%s"), **FilteredError), *bFilteredOK);
			});

		// ---- SetFavorite（后端不检查材质是否存在，对索引外的 ID 是空操作，只验证请求格式被接受）----
		const TSharedRef<bool> bFavOK = MakeShared<bool>(false);
		const TSharedRef<FString> FavError = MakeShared<FString>();
		EnqueueAsyncStep(Test, TEXT("SetFavorite 的回调"), 15.0,
			[bFavOK, FavError](TFunction<void()> Complete)
			{
				FDreamTextureApi::SetFavorite(FixtureId(), true, [=](bool bOK, const FString& Error)
				{
					*bFavOK = bOK;
					*FavError = Error;
					FDreamTextureApi::SetFavorite(FixtureId(), false, [=](bool bOK2, const FString& Error2)
					{
						*bFavOK = *bFavOK && bOK2;
						*FavError += Error2;
						Complete();
					});
				});
			},
			[Test, bFavOK, FavError]()
			{
				Test->TestTrue(FString::Printf(TEXT("SetFavorite true/false 都成功：%s"), **FavError), *bFavOK);
			});

		// ---- FetchFile ----
		const TSharedRef<bool> bFileOK = MakeShared<bool>(false);
		const TSharedRef<TArray<uint8>> FileBytes = MakeShared<TArray<uint8>>();
		const TSharedRef<FString> FileError = MakeShared<FString>();
		EnqueueAsyncStep(Test, TEXT("FetchFile 的回调"), 15.0,
			[bFileOK, FileBytes, FileError](TFunction<void()> Complete)
			{
				FDreamTextureApi::FetchFile(FixtureId(), TEXT("preview.png"), [=](bool bOK, TArray<uint8>&& Bytes, const FString& Error)
				{
					*bFileOK = bOK;
					*FileBytes = MoveTemp(Bytes);
					*FileError = Error;
					Complete();
				});
			},
			[Test, bFileOK, FileBytes, FileError]()
			{
				if (!Test->TestTrue(FString::Printf(TEXT("FetchFile(preview.png) 成功：%s"), **FileError), *bFileOK))
				{
					return;
				}
				TArray<uint8> Reference;
				if (Test->TestTrue(TEXT("读得到本地的 preview.png"), LoadFixtureBytes(TEXT("preview.png"), Reference)))
				{
					Test->TestTrue(TEXT("FetchFile 拿到的字节与本地固定素材一致"), *FileBytes == Reference);
				}
			});

		// ---- 不存在的材质：失败要带上后端的 HTTP 状态 ----
		const TSharedRef<bool> bMissingOK = MakeShared<bool>(true);
		const TSharedRef<FString> MissingError = MakeShared<FString>();
		EnqueueAsyncStep(Test, TEXT("读取不存在的材质的回调"), 15.0,
			[bMissingOK, MissingError](TFunction<void()> Complete)
			{
				FDreamTextureApi::GetManifest(TEXT("mat_does_not_exist"), [=](bool bOK, FDreamTextureManifest&&, const FString& Error)
				{
					*bMissingOK = bOK;
					*MissingError = Error;
					Complete();
				});
			},
			[Test, bMissingOK, MissingError]()
			{
				Test->TestFalse(TEXT("读取不存在的材质应当失败"), *bMissingOK);
				Test->TestTrue(FString::Printf(TEXT("错误里带有 HTTP 404（实际：%s）"), **MissingError), MissingError->Contains(TEXT("404")));
			});

		// ---- 清理 ----
		LiveEnqueueDo([S]()
		{
			IFileManager::Get().DeleteDirectory(*S->DownloadDir, false, true);
			S->UrlOverride.Reset();
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureLiveConnectionTest, "DreamTexture.Live.Connection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureLiveConnectionTest::RunTest(const FString& Parameters)
{
	if (SkipIfNoLiveBackend(*this))
	{
		return true;
	}
	RunLiveConnection(this, GetLiveURL());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureLiveFetchTest, "DreamTexture.Live.Fetch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureLiveFetchTest::RunTest(const FString& Parameters)
{
	if (SkipIfNoLiveBackend(*this))
	{
		return true;
	}

	// 标准答案取自本地固定素材。读不到就没法比对，直接失败。
	FString Text;
	FString Error;
	FDreamTextureManifest Local;
	if (!LoadFixtureText(TEXT("manifest.json"), Text) || !FDreamTextureManifest::FromJsonString(Text, Local, Error))
	{
		AddError(FString::Printf(TEXT("读不到或解析不了本地固定素材的 manifest：%s"), *Error));
		return false;
	}

	RunLiveFetch(this, GetLiveURL(), Local);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
