// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureConnection.h"
#include "DreamTextureGenerate.h"
#include "DreamTextureGenerateModel.h"
#include "DreamTextureJobTracker.h"
#include "DreamTextureLiveTestUtils.h"
#include "DreamTextureSettings.h"

/*
 * DreamTexture.Live.Generate.Flow：对着固定数据后端提交一次生成，验证从提交到任务事件到界面说法的整条通路。
 *
 * 固定数据后端的 ComfyUI 指向一个没人监听的端口（Tools/FixtureBackend/README.md），所以**任务不会成功**。
 * 这正好用来验证失败路径，但和直觉不同的是它**不会自己很快失败**：后端 job/runner.go 的 waitComfyAvailable
 * 遇到 ComfyUI 不在就排着等它回来，最长等 15 分钟才判失败（用户点了生成却因为 ComfyUI 正在重启而瞬间失败是更糟的体验）。
 * 实测：任务 queued → running，阶段停在「等待 ComfyUI」。所以这里的剧本是：
 *
 *   1. 三级状态：后端在、长连接通、ComfyUI 不在 —— 页面模型必须把它判成第 3 级并挡住提交（真实环境里验证，不是假连接）；
 *   2. 提交失败路径：不存在的工作流、不在候选里的分辨率，后端回 400，原因要原样交回来；
 *   3. 提交一个固定种子的变体：提交成功，种子原样往返（后端用 float64 存）；
 *   4. 事件到达：job.progress 把它推到「等待 ComfyUI」—— 不轮询，追踪器只靠事件；
 *   5. 取消：后端发 job.failed（状态 canceled，原因「已取消」），卡片据此显示「已取消」；原因只是这句通用话时不重复显示；
 *   6. 收尾：任务一定要是终态，否则它占着后端唯一的 worker，15 分钟里排在后面的任务全被堵住。
 *
 * 叶子名带 .Flow：同目录下还有 .AutoStart（后端没开时点生成，见 DreamTextureGenerateAutoStartLiveTests.cpp），
 * Docs/contributing.md 规定一个测试的全名不能是别的测试名的前缀，否则 RunTests 上级前缀时会漏掉它。
 *
 * 只有 DREAMTEXTURE_LIVE_URL 有值才跑；后端不支持这些接口（404 / 501，或者没有工作流分段）就跳过并说明。
 * 如果目标后端的 ComfyUI 是活的就拒绝继续——再往下走会真的出图，那不是这个测试该干的事。
 */

namespace
{
	using namespace DreamTextureLive;

	struct FLiveState
	{
		FString LiveURL;
		FString PreviousURL;
		bool bUrlOverridden = false;

		TSharedPtr<FDreamTextureConnection> Conn;
		TSharedPtr<FDreamTextureJobTracker> Tracker;
		TSharedPtr<FDreamTextureGenerateModel> Model;

		/** 前面某一步失败（或判定要跳过）到没法继续了：后续检查步骤直接跳过，但收尾照常执行。 */
		bool bAbort = false;

		// 步骤 2 的两个失败路径
		bool bBadWorkflowDone = false;
		bool bBadWorkflowOK = true;
		FString BadWorkflowError;
		bool bBadResolutionDone = false;
		bool bBadResolutionOK = true;
		FString BadResolutionError;

		// 步骤 3 的真提交
		FDreamTextureGenerateRequest Request;
		bool bSubmitDone = false;
		bool bSubmitOK = false;
		FString SubmitError;
		TArray<FDreamTextureJob> Submitted;
		FString JobId;

		// 连接上看到的该任务的事件类型
		TSet<FString> EventTypes;
		FString PendingEventJobId;

		bool bCancelRequestDone = false;
		bool bCancelOK = false;
		FString CancelError;

		~FLiveState()
		{
			if (Tracker.IsValid())
			{
				Tracker->Unbind();
			}
			if (Conn.IsValid())
			{
				Conn->Stop();
			}
			if (bUrlOverridden)
			{
				GetMutableDefault<UDreamTextureSettings>()->BackendURL = PreviousURL;
			}
		}
	};

	constexpr int64 LiveSeed = 12345;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureLiveGenerateTest, "DreamTexture.Live.Generate.Flow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureLiveGenerateTest::RunTest(const FString& Parameters)
{
	const FString LiveURL = GetLiveURL();
	if (LiveURL.IsEmpty())
	{
		AddInfo(TEXT("已跳过：没有设置环境变量 DREAMTEXTURE_LIVE_URL。需要先起固定数据后端（Tools/FixtureBackend/Start-FixtureBackend.ps1），"
			"再把它设成 http://127.0.0.1:18777；Tools/Test.ps1 -Live 会替你做这两件事。"));
		return true;
	}

	FAutomationTestBase* Test = this;
	const TSharedRef<FLiveState> S = MakeShared<FLiveState>();
	S->LiveURL = LiveURL;

	// ---- 1. 建连接、追踪器、页面模型，指向固定数据后端 ----
	EnqueueDo([S]()
	{
		UDreamTextureSettings* Settings = GetMutableDefault<UDreamTextureSettings>();
		S->PreviousURL = Settings->BackendURL;
		Settings->BackendURL = S->LiveURL;
		S->bUrlOverridden = true;

		// 自己建一套，不用模块全局那套：全局追踪器挂着编辑器模块的自动导入，测试不该去碰它。
		S->Conn = MakeShared<FDreamTextureConnection>();
		S->Tracker = MakeShared<FDreamTextureJobTracker>();
		S->Tracker->Bind(S->Conn);

		FDreamTextureGenerateBackend Backend = FDreamTextureGenerateBackend::MakeDefault();
		const TSharedPtr<FDreamTextureConnection> Conn = S->Conn;
		const TSharedPtr<FDreamTextureJobTracker> Tracker = S->Tracker;
		Backend.GetConnection = [Conn]() { return Conn; };
		Backend.GetTracker = [Tracker]() { return Tracker; };
		// 不碰项目设置，也不真的排导入。
		Backend.GetAutoImport = []() { return false; };
		Backend.SetAutoImport = [](bool) {};
		Backend.Enqueue = [](const FString&) { return true; };
		S->Model = MakeShared<FDreamTextureGenerateModel>(MoveTemp(Backend));
		S->Model->Initialize(/*bBindExternal*/ false);

		// 记下连接上这个任务的事件类型：验证进度是推过来的，不是轮询出来的。
		const TWeakPtr<FLiveState> Weak = S;
		S->Conn->OnEvent.AddLambda([Weak](const FDreamTextureEvent& Event)
		{
			const TSharedPtr<FLiveState> State = Weak.Pin();
			FDreamTextureJob Job;
			if (State.IsValid() && FDreamTextureJob::FromEvent(Event, Job) && Job.Id == State->PendingEventJobId)
			{
				State->EventTypes.Add(Event.Type);
			}
		});

		S->Conn->Start();
	});

	// ---- 2. 链路 Live，并且收到了第一条 comfy.status ----
	EnqueueWait(Test, TEXT("链路到达 Live 且收到 ComfyUI 状态。后端必须已经在跑：先执行 Tools/FixtureBackend/Start-FixtureBackend.ps1"), 25.0,
		[S]() { return S->Conn->GetLink() == EDreamTextureLink::Live && !S->Conn->GetHealth().Mode.IsEmpty(); },
		nullptr,
		[S]() { S->bAbort = true; });

	// ---- 3. 目录 + 三级状态 ----
	EnqueueDo([S]()
	{
		if (!S->bAbort)
		{
			S->Model->LoadCatalog();
		}
	});
	EnqueueWait(Test, TEXT("工作流目录读回来"), 20.0,
		[S]() { return S->bAbort || S->Model->GetCatalogState() != FDreamTextureGenerateModel::ECatalogState::Loading; },
		[S, Test]()
		{
			if (S->bAbort)
			{
				return;
			}

			if (S->Model->GetCatalogState() != FDreamTextureGenerateModel::ECatalogState::Ready)
			{
				const FString& Error = S->Model->GetCatalogError();
				S->bAbort = true;
				if (Error.Contains(TEXT("404")) || Error.Contains(TEXT("501")))
				{
					Test->AddInfo(FString::Printf(TEXT("已跳过：后端不支持 GET /api/workflows（%s）。"), *Error));
				}
				else
				{
					Test->AddError(FString::Printf(TEXT("读取工作流目录失败：%s"), *Error));
				}
				return;
			}

			if (S->Model->GetSourceOptions().Num() == 0 || S->Model->GetDecomposeOptions().Num() == 0)
			{
				S->bAbort = true;
				Test->AddInfo(TEXT("已跳过：这个后端的 /api/workflows 里没有出图段/分解段（后端早于「工作流分段」那一版）。"));
				return;
			}

			Test->AddInfo(FString::Printf(TEXT("目录：%d 个出图模型 × %d 个分解模型，默认选 %s × %s"),
				S->Model->GetSourceOptions().Num(), S->Model->GetDecomposeOptions().Num(),
				*S->Model->GetSourceId(), *S->Model->GetDecomposeId()));

			if (!Test->TestNotNull(TEXT("默认选择拼得出组合工作流"), S->Model->GetCurrentWorkflow()))
			{
				S->bAbort = true;
				return;
			}

			// 固定数据后端：后端进程在、长连接通、ComfyUI 不在。这是 Docs/contributing.md 要求分开显示的那一档，
			// 这里拿真的后端验证页面模型的判定，而不是只靠假连接。
			const FDreamTextureHealth& Health = S->Conn->GetHealth();
			Test->AddInfo(FString::Printf(TEXT("Health：mode=%s alive=%d ready=%d reason=%s"), *Health.Mode, Health.bAlive, Health.bReady, *Health.Reason));
			if (Health.bAlive)
			{
				S->bAbort = true;
				Test->AddInfo(TEXT("已跳过：目标后端的 ComfyUI 是活的。这个测试只对着 ComfyUI 不在线的固定数据后端跑，否则下面的提交会真的出图。"));
				return;
			}

			S->Model->SetPrompt(TEXT("mossy rocks"));
			const FDreamTextureGenerateReadiness Readiness = S->Model->GetReadiness();
			Test->TestFalse(TEXT("ComfyUI 不在时不能提交"), Readiness.bCanSubmit);
			Test->TestTrue(TEXT("被挡在第 3 级（ComfyUI），而不是第 1 级（后端进程）"), Readiness.Tier == EDreamTextureGenerateTier::Comfy);
			Test->TestTrue(TEXT("后端进程这一级是通的"), S->Conn->IsBackendReachable());
			Test->TestTrue(TEXT("长连接这一级是通的"), S->Conn->GetLink() == EDreamTextureLink::Live);
			Test->TestTrue(TEXT("原因里点明是 ComfyUI、且说别去重启后端"),
				Readiness.Reason.ToString().Contains(TEXT("ComfyUI")) && Readiness.Reason.ToString().Contains(TEXT("do not restart the backend")));
			Test->AddInfo(FString::Printf(TEXT("提交按钮旁边的说明：%s"), *Readiness.Reason.ToString()));
			FText Why;
			Test->TestFalse(TEXT("CanSubmit 也为假"), S->Model->CanSubmit(&Why));
			Test->TestEqual(TEXT("CanSubmit 给的原因就是这一级的"), Why.ToString(), Readiness.Reason.ToString());
		});

	// ---- 4. 提交失败路径：后端回 400，原因原样交回 ----
	EnqueueDo([S]()
	{
		if (S->bAbort)
		{
			return;
		}

		FDreamTextureGenerateRequest Bad;
		Bad.WorkflowId = TEXT("no-such-source.no-such-decompose");
		Bad.StringParams.Add(TEXT("prompt"), TEXT("x"));
		S->Tracker->Submit(Bad, [S](bool bOK, TArray<FDreamTextureJob>&&, const FString& Error)
		{
			S->bBadWorkflowDone = true;
			S->bBadWorkflowOK = bOK;
			S->BadWorkflowError = Error;
		});

		// 分辨率不在候选里：后端 coerce 一个 enum 参数时拒绝。
		FDreamTextureGenerateRequest BadRes;
		BadRes.WorkflowId = S->Model->GetCurrentWorkflow()->Id;
		BadRes.StringParams.Add(TEXT("prompt"), TEXT("x"));
		BadRes.IntParams.Add(TEXT("resolution"), 999);
		S->Tracker->Submit(BadRes, [S](bool bOK, TArray<FDreamTextureJob>&&, const FString& Error)
		{
			S->bBadResolutionDone = true;
			S->bBadResolutionOK = bOK;
			S->BadResolutionError = Error;
		});
	});
	EnqueueWait(Test, TEXT("两个非法提交的应答"), 20.0,
		[S]() { return S->bAbort || (S->bBadWorkflowDone && S->bBadResolutionDone); },
		[S, Test]()
		{
			if (S->bAbort)
			{
				return;
			}
			Test->TestFalse(TEXT("不存在的工作流提交失败"), S->bBadWorkflowOK);
			Test->TestTrue(TEXT("原因里带着后端的话（工作流不存在）"), S->BadWorkflowError.Contains(TEXT("不存在")));
			Test->AddInfo(FString::Printf(TEXT("不存在的工作流：%s"), *S->BadWorkflowError));
			Test->TestFalse(TEXT("不在候选里的分辨率提交失败"), S->bBadResolutionOK);
			Test->TestTrue(TEXT("原因里点名了参数"), S->BadResolutionError.Contains(TEXT("resolution")));
			Test->AddInfo(FString::Printf(TEXT("非法分辨率：%s"), *S->BadResolutionError));
			Test->TestEqual(TEXT("失败的提交没有登记任何任务"), S->Tracker->GetNumTracked(), 0);
		});

	// ---- 5. 提交一个固定种子的变体 ----
	EnqueueDo([S, Test]()
	{
		if (S->bAbort)
		{
			return;
		}

		S->Model->SetVariants(1);
		S->Model->SetRandomSeed(false);
		S->Model->SetFixedSeed(LiveSeed);
		if (!Test->TestTrue(TEXT("拼得出请求"), S->Model->BuildRequest(S->Request)))
		{
			S->bAbort = true;
			return;
		}

		FString Body;
		FString Error;
		if (S->Request.ToJsonString(Body, Error))
		{
			Test->AddInfo(FString::Printf(TEXT("请求体：%s"), *Body));
		}

		S->Tracker->Submit(S->Request, [S](bool bOK, TArray<FDreamTextureJob>&& Jobs, const FString& Error)
		{
			S->bSubmitDone = true;
			S->bSubmitOK = bOK;
			S->SubmitError = Error;
			S->Submitted = MoveTemp(Jobs);
			if (S->Submitted.Num() > 0)
			{
				S->JobId = S->Submitted[0].Id;
				S->PendingEventJobId = S->JobId;
			}
		});
	});
	EnqueueWait(Test, TEXT("提交的应答"), 20.0,
		[S]() { return S->bAbort || S->bSubmitDone; },
		[S, Test]()
		{
			if (S->bAbort)
			{
				return;
			}
			if (!Test->TestTrue(*FString::Printf(TEXT("提交成功（失败原因：%s）"), *S->SubmitError), S->bSubmitOK))
			{
				S->bAbort = true;
				return;
			}
			if (!Test->TestEqual(TEXT("一个变体一个任务"), S->Submitted.Num(), 1))
			{
				S->bAbort = true;
				return;
			}

			const FDreamTextureJob& Job = S->Submitted[0];
			Test->TestFalse(TEXT("任务有 id"), Job.Id.IsEmpty());
			Test->TestFalse(TEXT("任务有材质 id"), Job.MaterialId.IsEmpty());
			Test->TestFalse(TEXT("任务有批次 id"), Job.BatchId.IsEmpty());
			Test->TestEqual(TEXT("工作流是组合 id"), Job.WorkflowId, S->Request.WorkflowId);
			Test->TestTrue(TEXT("读到了种子"), Job.bHasSeed);
			Test->TestEqual(TEXT("种子原样往返（后端用 float64 存）"), Job.Seed, LiveSeed);
			Test->TestEqual(TEXT("分辨率"), Job.Resolution, S->Model->GetResolution());
			Test->TestEqual(TEXT("展示名取自提示词"), Job.Name, FDreamTextureGenerateRequest::MakeNameFromPrompt(TEXT("mossy rocks")));
			Test->TestTrue(TEXT("刚提交是排队或已经开始"), Job.Status == EDreamTextureJobStatus::Queued || Job.Status == EDreamTextureJobStatus::Running);
			Test->TestEqual(TEXT("追踪器登记了它"), S->Tracker->GetNumTracked(), 1);
			Test->AddInfo(FString::Printf(TEXT("任务 %s，批次 %s，种子 %lld"), *Job.Id, *Job.BatchId, Job.Seed));
		});

	// ---- 6. 事件把它推到「等待 ComfyUI」 ----
	EnqueueWait(Test, TEXT("job.progress 事件把任务推到「等待 ComfyUI」（追踪器不轮询，只能靠事件）"), 25.0,
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
				Test->AddInfo(FString::Printf(TEXT("阶段：%s，进度 %.2f"), *Job->Stage, Job->Progress));
				const FDreamTextureGenerateRowView View = FDreamTextureGenerateModel::EvaluateRow(1, Job, false, nullptr);
				Test->TestTrue(TEXT("卡片显示运行中"), View.Kind == FDreamTextureGenerateRowView::EKind::Running);
				Test->TestTrue(TEXT("没有真实进度可言：进度条走跑马灯，不画假进度"), View.bShowProgress && !View.Percent.IsSet());
				Test->TestTrue(TEXT("可以取消"), View.bCanCancel);
			}
		});

	// ---- 7. 取消 ----
	EnqueueDo([S]()
	{
		if (S->bAbort || S->JobId.IsEmpty())
		{
			return;
		}
		S->Tracker->Cancel(S->JobId, [S](bool bOK, const FString& Error)
		{
			S->bCancelRequestDone = true;
			S->bCancelOK = bOK;
			S->CancelError = Error;
		});
	});
	EnqueueWait(Test, TEXT("取消请求的应答"), 20.0,
		[S]() { return S->bAbort || S->JobId.IsEmpty() || S->bCancelRequestDone; },
		[S, Test]()
		{
			if (S->bAbort || S->JobId.IsEmpty())
			{
				return;
			}
			Test->TestTrue(*FString::Printf(TEXT("取消请求成功（失败原因：%s）"), *S->CancelError), S->bCancelOK);
		});

	// ---- 8. job.failed 事件把它推到终态，原因能显示 ----
	EnqueueWait(Test, TEXT("任务进入终态（后端对取消发 job.failed，状态是 canceled）"), 25.0,
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
			if (!Test->TestNotNull(TEXT("追踪器里有这个任务"), Job))
			{
				return;
			}
			Test->TestTrue(TEXT("状态是 canceled 而不是 failed——两种说法"), Job->Status == EDreamTextureJobStatus::Canceled);
			Test->TestFalse(TEXT("有原因"), Job->Error.IsEmpty());
			Test->AddInfo(FString::Printf(TEXT("终态：%s，原因：%s"), LexToString(Job->Status), *Job->Error));

			// 界面上这张卡片怎么说。
			const FDreamTextureGenerateRowView View = FDreamTextureGenerateModel::EvaluateRow(1, Job, /*bCancelRequested*/ true, nullptr);
			Test->TestTrue(TEXT("卡片显示已取消"), View.Kind == FDreamTextureGenerateRowView::EKind::Canceled);
			Test->TestEqual(TEXT("阶段"), View.Stage.ToString(), FString(TEXT("Canceled")));
			Test->TestTrue(TEXT("原因只是通用的「已取消」，不重复显示"), !FDreamTextureJob::IsPlainCancelReason(Job->Error) || View.Detail.IsEmpty());
			Test->TestFalse(TEXT("终态不能再取消"), View.bCanCancel);
			Test->TestFalse(TEXT("也不能导入"), View.bCanImport);
		});

	// 事件类型：job.queued 在后端 Submit 里同步发（早于 HTTP 应答），job.progress 是 worker 开始跑，job.failed 是取消的收尾。
	// 都是 WS 推过来的。多给几秒：它们和 REST 应答走的是两条路。
	EnqueueWait(Test, TEXT("连接上看到 job.queued / job.progress / job.failed 三种事件"), 10.0,
		[S]()
		{
			return S->bAbort || S->JobId.IsEmpty()
				|| (S->EventTypes.Contains(TEXT("job.queued")) && S->EventTypes.Contains(TEXT("job.progress")) && S->EventTypes.Contains(TEXT("job.failed")));
		},
		[S, Test]()
		{
			if (S->bAbort || S->JobId.IsEmpty())
			{
				return;
			}
			TArray<FString> Types = S->EventTypes.Array();
			Types.Sort();
			Test->AddInfo(FString::Printf(TEXT("该任务在连接上收到的事件类型：%s"), *FString::Join(Types, TEXT("、"))));
			Test->TestTrue(TEXT("收到 job.queued"), S->EventTypes.Contains(TEXT("job.queued")));
			Test->TestTrue(TEXT("收到 job.progress"), S->EventTypes.Contains(TEXT("job.progress")));
			Test->TestTrue(TEXT("收到 job.failed"), S->EventTypes.Contains(TEXT("job.failed")));
			Test->TestFalse(TEXT("没有 job.done：固定数据后端出不了图"), S->EventTypes.Contains(TEXT("job.done")));
		});

	// ---- 9. 收尾：不能把后端唯一的 worker 留在「等待 ComfyUI」里 ----
	EnqueueDo([S]()
	{
		if (S->JobId.IsEmpty())
		{
			return;
		}
		const FDreamTextureJob* Job = S->Tracker->Find(S->JobId);
		if (Job != nullptr && !Job->IsTerminal())
		{
			S->Tracker->Cancel(S->JobId);
		}
	});
	EnqueueWait(Test, TEXT("收尾：任务进入终态"), 20.0,
		[S]()
		{
			if (S->JobId.IsEmpty())
			{
				return true;
			}
			const FDreamTextureJob* Job = S->Tracker->Find(S->JobId);
			return Job == nullptr || Job->IsTerminal();
		});

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
