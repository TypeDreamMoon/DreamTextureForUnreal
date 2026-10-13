// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureApi.h"
#include "DreamTextureConnection.h"
#include "DreamTextureLiveTestUtils.h"
#include "DreamTextureReflowTestUtils.h"
#include "DreamTextureSettings.h"
#include "Misc/FileHelper.h"

/*
 * DreamTexture.Live.Reflow：回流的请求对着固定数据后端真走一遍，验证后端接受我们发的东西、界面说得清楚。
 *
 * 固定数据后端的 ComfyUI 指向一个没人监听的端口，所以**任务不会成功**，也**不会很快失败**：
 * 后端 job/runner.go 的 waitComfyAvailable 遇到 ComfyUI 不在就排着等它回来，最长等 15 分钟，
 * 任务停在 running、阶段「等待 ComfyUI」（见 Live.Generate.Flow）。所以这里的剧本是：
 *
 *   0. 后端校验：补通道的请求不带 source_image，后端在提交时就拦下并点名参数；
 *   1. 真上传：multipart 的字段名、扩展名后端认了（走到「转存到 ComfyUI」才失败，而不是 400）——
 *      ComfyUI 不在，上传必然 502，界面说「在「上传」这一步失败」；
 *   2. 补通道的提交被接受：字段、derived_from 校验通过，任务进入「等待 ComfyUI」，
 *      后端把 derived_from 原样记下（读回来逐字段核对），运行停在「生成」这一步；
 *   3. 取消：后端发 job.failed(canceled)，运行说「在「生成」这一步失败：已取消」（英文界面是 Canceled）；
 *   4. 改写外观的提交同样被接受（提示词、重绘幅度、参考图、derived_from.mode=restyle），同样取消；
 *      补通道与改写外观的请求都按 UV 展开发（tileable=false，重绘幅度 0.30 在 0.35 的上限以内）——
 *      后端接受 tileable 这个布尔参数（上传段 / 出图段 / 分解段声明了它）；
 *      补通道与改写外观的请求都按 UV 展开发（tileable=false，重绘幅度 0.30 在 0.35 的上限以内），
 *      后端接受 tileable 这个布尔参数（出图段 / 分解段 / 上传段都声明了它）；
 *   5. 收尾：任务一定要是终态——测试后端只有一个 worker，不取消会把后面的测试全堵住 15 分钟。
 *
 * 只有 DREAMTEXTURE_LIVE_URL 有值才跑；后端没有「已有底图」出图段（旧后端）就跳过并说明。
 * 目标后端的 ComfyUI 是活的就拒绝继续——再往下走会真的出图，那不是这个测试该干的事。
 */

namespace
{
	using namespace DreamTextureLive;
	using namespace DreamTextureReflowTest;

	struct FReflowLiveState
	{
		FString LiveURL;
		FString PreviousURL;
		bool bUrlOverridden = false;

		TSharedPtr<FDreamTextureConnection> Conn;
		TSharedPtr<FDreamTextureJobTracker> Tracker;

		FDreamTextureWorkflowCatalog Catalog;
		bool bCatalogDone = false;
		bool bCatalogOK = false;
		FString CatalogError;

		FString FixturePng;
		FString FixtureSha;

		bool bAbort = false;

		// 步骤 0
		bool bMissingDone = false;
		bool bMissingOK = true;
		FString MissingError;

		// 步骤 1
		TSharedPtr<FDreamTextureReflowRun> UploadRun;

		// 步骤 2~3、4
		TSharedPtr<FDreamTextureReflowRun> FillRun;
		TSharedPtr<FDreamTextureReflowRun> RestyleRun;

		bool bGetDone = false;
		bool bGetOK = false;
		FDreamTextureJob GotJob;
		FString GetError;

		~FReflowLiveState()
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

		/** 运行的外部能力：导出用固定素材里的小 PNG（不需要真贴图），上传按需真发，其余走自己的追踪器。 */
		FDreamTextureReflowRunHooks MakeHooks(bool bRealUpload)
		{
			FDreamTextureReflowRunHooks Hooks;
			Hooks.EnsureBackend = [](TFunction<void(bool, const FString&)> Done) { Done(true, FString()); };
			Hooks.MakeTempDir = []() { return FString(TEXT("C:/Fake/Reflow/live")); };
			Hooks.Export = [this](const FDreamTextureMaterialAnalysis& Analysis, const FDreamTextureExportOptions&)
			{
				TArray<FDreamTextureExportedImage> Results;
				for (const FDreamTextureMaterialTexture& Row : Analysis.Textures)
				{
					FDreamTextureExportedImage Image;
					Image.TexturePath = Row.TexturePath;
					Image.bExported = true;
					Image.FilePath = FixturePng;
					Image.Sha256 = FixtureSha;
					Image.Width = Image.Height = 64;
					Results.Add(MoveTemp(Image));
				}
				return Results;
			};
			if (bRealUpload)
			{
				Hooks.Upload = [](const FString& FilePath, FDreamTextureUploadApi::FOnUploaded Done)
				{
					FDreamTextureUploadApi::UploadImage(FilePath, MoveTemp(Done));
				};
			}
			else
			{
				// 固定数据后端没有 ComfyUI，上传必然 502。要验证提交，就用一个后端会发的那种文件名顶上。
				Hooks.Upload = [](const FString&, FDreamTextureUploadApi::FOnUploaded Done)
				{
					FString Name = TEXT("dt_ref_livetest0001.png");
					Done(true, MoveTemp(Name), FString());
				};
			}
			const TSharedPtr<FDreamTextureJobTracker> TrackerRef = Tracker;
			Hooks.GetTracker = [TrackerRef]() { return TrackerRef; };
			Hooks.Enqueue = [](const FString&, const FDreamTextureImportOptions&) { return true; };
			Hooks.WatchJobs = [TrackerRef](TFunction<void(const FDreamTextureJob&)> Callback) -> TFunction<void()>
			{
				const FDelegateHandle Handle = TrackerRef->OnJobChanged.AddLambda([Callback = MoveTemp(Callback)](const FDreamTextureJob& Job) { Callback(Job); });
				const TWeakPtr<FDreamTextureJobTracker> Weak = TrackerRef;
				return [Weak, Handle]()
				{
					if (const TSharedPtr<FDreamTextureJobTracker> Pinned = Weak.Pin())
					{
						Pinned->OnJobChanged.Remove(Handle);
					}
				};
			};
			Hooks.WatchImports = [](TFunction<void(const FDreamTextureImportStatus&)>) -> TFunction<void()> { return []() {}; };
			return Hooks;
		}
	};

	constexpr const TCHAR* LiveAssetPath = TEXT("/Game/Live/MI_LiveReflow.MI_LiveReflow");

	/** 补通道的运行输入：已有底图 × 传统派生（不需要 CHORD 的模型文件，和 ComfyUI 在不在无关）。 */
	FDreamTextureReflowRunSpec MakeLiveFillSpec(const FString& WorkflowId)
	{
		FDreamTextureReflowRunSpec Spec = MakeFillSpec(*WorkflowId);
		Spec.MaterialAssetPath = LiveAssetPath;
		Spec.MaterialName = TEXT("MI_LiveReflow");
		Spec.Request.DerivedFrom.AssetPath = LiveAssetPath;
		Spec.Request.Name = TEXT("LiveReflow fill");
		Spec.Request.IntParams.Add(TEXT("resolution"), 1024);
		// UV 展开：不要平铺。
		Spec.UVKind = EDreamTextureUVKind::Unwrapped;
		Spec.Request.BoolParams.Add(TEXT("tileable"), false);
		return Spec;
	}

	FDreamTextureReflowRunSpec MakeLiveRestyleSpec(const FString& WorkflowId)
	{
		FDreamTextureReflowRunSpec Spec = MakeLiveFillSpec(WorkflowId);
		Spec.Mode = EDreamTextureReflowMode::Restyle;
		Spec.UploadParamKey = TEXT("reference");
		Spec.Request.Name = TEXT("LiveReflow 改写");
		Spec.Request.StringParams.Add(TEXT("prompt"), TEXT("mossy broken rock"));
		Spec.Request.FloatParams.Add(TEXT("denoise"), DreamTextureReflow::DefaultUnwrappedDenoise);
		Spec.Request.IntParams.Add(TEXT("reference_size"), 1024);
		Spec.Request.SetSeed(DreamTextureGenerateLimits::RandomSeed);
		Spec.Request.DerivedFrom.Mode = FDreamTextureDerivedFrom::ModeRestyle();
		Spec.ImportOptions = FDreamTextureImportOptions();
		return Spec;
	}

	bool IsWaitingForComfy(const FDreamTextureJob* Job)
	{
		return Job != nullptr && Job->Status == EDreamTextureJobStatus::Running && Job->Stage.Contains(TEXT("ComfyUI"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureLiveReflowTest, "DreamTexture.Live.Reflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureLiveReflowTest::RunTest(const FString& Parameters)
{
	const FString LiveURL = GetLiveURL();
	if (LiveURL.IsEmpty())
	{
		AddInfo(TEXT("已跳过：没有设置环境变量 DREAMTEXTURE_LIVE_URL。需要先起固定数据后端（Tools/FixtureBackend/Start-FixtureBackend.ps1），"
			"再把它设成 http://127.0.0.1:18777；Tools/Test.ps1 -Live 会替你做这两件事。"));
		return true;
	}

	FAutomationTestBase* Test = this;
	const TSharedRef<FReflowLiveState> S = MakeShared<FReflowLiveState>();
	S->LiveURL = LiveURL;

	// ---- 1. 连接、追踪器，指向固定数据后端；准备固定素材里的小 PNG ----
	EnqueueDo([S, Test]()
	{
		UDreamTextureSettings* Settings = GetMutableDefault<UDreamTextureSettings>();
		S->PreviousURL = Settings->BackendURL;
		Settings->BackendURL = S->LiveURL;
		S->bUrlOverridden = true;

		// 自己建一套，不用模块全局那套：全局追踪器挂着编辑器模块的自动导入，测试不该去碰它。
		S->Conn = MakeShared<FDreamTextureConnection>();
		S->Tracker = MakeShared<FDreamTextureJobTracker>();
		S->Tracker->Bind(S->Conn);

		S->FixturePng = FPaths::Combine(PluginDir(), TEXT("Tests"), TEXT("Fixtures"), TEXT("mat_20260827_777001_ref"), TEXT("basecolor.png"));
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *S->FixturePng))
		{
			Test->AddError(FString::Printf(TEXT("读不到固定素材里的底色图：%s"), *S->FixturePng));
			S->bAbort = true;
			return;
		}
		S->FixtureSha = FDreamTextureFetcher::HashBytes16(Bytes);
		S->Conn->Start();
	});

	EnqueueWait(Test, TEXT("链路到达 Live 且收到 ComfyUI 状态。后端必须已经在跑：先执行 Tools/FixtureBackend/Start-FixtureBackend.ps1"), 25.0,
		[S]() { return S->bAbort || (S->Conn->GetLink() == EDreamTextureLink::Live && !S->Conn->GetHealth().Mode.IsEmpty()); },
		nullptr, [S]() { S->bAbort = true; });

	// ---- 2. 目录：要有「已有底图」出图段，ComfyUI 不能是活的 ----
	EnqueueDo([S]()
	{
		if (!S->bAbort)
		{
			FDreamTextureGenerateApi::ListWorkflows([S](bool bOK, FDreamTextureWorkflowCatalog&& Catalog, const FString& Error)
			{
				S->bCatalogDone = true;
				S->bCatalogOK = bOK;
				S->Catalog = MoveTemp(Catalog);
				S->CatalogError = Error;
			});
		}
	});
	EnqueueWait(Test, TEXT("工作流目录读回来"), 20.0, [S]() { return S->bAbort || S->bCatalogDone; },
		[S, Test]()
		{
			if (S->bAbort)
			{
				return;
			}
			if (!S->bCatalogOK)
			{
				S->bAbort = true;
				Test->AddError(FString::Printf(TEXT("读取工作流目录失败：%s"), *S->CatalogError));
				return;
			}
			if (S->Catalog.FindComposed(TEXT("src-upload-v1"), TEXT("dec-derive-v1")) == nullptr)
			{
				S->bAbort = true;
				Test->AddInfo(TEXT("已跳过：这个后端的工作流目录里没有 src-upload-v1.dec-derive-v1（后端早于「补齐通道」那一版）。"
					"用 DREAMTEXTURE_BACKEND_EXE=<新后端的 dreamtexture.exe> 起新后端再跑。"));
				return;
			}
			if (S->Catalog.FindComposed(TEXT("src-local-realistic-v1"), TEXT("dec-derive-v1")) == nullptr)
			{
				S->bAbort = true;
				Test->AddInfo(TEXT("已跳过：目录里没有 src-local-realistic-v1.dec-derive-v1，没法验证改写外观。"));
				return;
			}
			const FDreamTextureHealth& Health = S->Conn->GetHealth();
			Test->AddInfo(FString::Printf(TEXT("Health：mode=%s alive=%d ready=%d reason=%s"), *Health.Mode, Health.bAlive, Health.bReady, *Health.Reason));
			if (Health.bAlive)
			{
				S->bAbort = true;
				Test->AddInfo(TEXT("已跳过：目标后端的 ComfyUI 是活的。这个测试只对着 ComfyUI 不在线的固定数据后端跑，否则下面的提交会真的出图。"));
			}
		});

	// ---- 3. 后端校验：补通道不给 source_image，提交时就被拦下并点名 ----
	EnqueueDo([S]()
	{
		if (S->bAbort)
		{
			return;
		}
		FDreamTextureGenerateRequest Request = MakeLiveFillSpec(TEXT("src-upload-v1.dec-derive-v1")).Request;
		S->Tracker->Submit(Request, [S](bool bOK, TArray<FDreamTextureJob>&&, const FString& Error)
		{
			S->bMissingDone = true;
			S->bMissingOK = bOK;
			S->MissingError = Error;
		}, FDreamTextureReflowRun::GetOrigin());
	});
	EnqueueWait(Test, TEXT("缺 source_image 的提交的应答"), 20.0, [S]() { return S->bAbort || S->bMissingDone; },
		[S, Test]()
		{
			if (S->bAbort)
			{
				return;
			}
			Test->TestFalse(TEXT("不带 source_image：后端拒绝"), S->bMissingOK);
			Test->TestTrue(TEXT("原因里点名了 source_image（必填参数在提交时就拦，不等 ComfyUI 去报「Value not in list」）"), S->MissingError.Contains(TEXT("source_image")));
			Test->AddInfo(FString::Printf(TEXT("缺必填参数：%s"), *S->MissingError));
			Test->TestEqual(TEXT("被拒的提交没有登记任何任务"), S->Tracker->GetNumTracked(), 0);
		});

	// ---- 4. 真上传：后端认了 multipart，走到转存 ComfyUI 才失败（502） ----
	EnqueueDo([S]()
	{
		if (S->bAbort)
		{
			return;
		}
		S->UploadRun = MakeShared<FDreamTextureReflowRun>(MakeLiveFillSpec(TEXT("src-upload-v1.dec-derive-v1")), S->MakeHooks(/*bRealUpload*/ true));
		S->UploadRun->Start();
	});
	EnqueueWait(Test, TEXT("真上传：运行走到终态（ComfyUI 不在，必然失败）"), 40.0,
		[S]() { return S->bAbort || (S->UploadRun.IsValid() && S->UploadRun->IsFinished()); },
		[S, Test]()
		{
			if (S->bAbort || !S->UploadRun.IsValid())
			{
				return;
			}
			const FString Message = S->UploadRun->GetMessage().ToString();
			Test->AddInfo(FString::Printf(TEXT("上传这一步的说法：%s"), *Message));
			Test->TestTrue(TEXT("失败了（固定数据后端没有 ComfyUI 可转存）"), S->UploadRun->GetState() == FDreamTextureReflowRun::EState::Failed);
			Test->TestTrue(TEXT("失败在「上传」这一步"), S->UploadRun->GetStep() == EDreamTextureReflowStep::Upload);
			Test->TestTrue(TEXT("说明点名「上传」"), Message.Contains(TEXT("Failed at the \"Upload\" step")));
			Test->TestTrue(TEXT("原因是转存 ComfyUI 失败（说明后端解析了我们的 multipart、认了 image 字段和 .png 扩展名，而不是 400）"),
				Message.Contains(TEXT("ComfyUI")) && !Message.Contains(TEXT("没有名为 image")) && !Message.Contains(TEXT("解析上传内容失败")));
			Test->TestFalse(TEXT("上传没成就没有提交任务"), S->Tracker->GetNumTracked() > 0);
		});

	// ---- 5. 补通道的提交被接受，停在「等待 ComfyUI」，derived_from 被后端记下 ----
	EnqueueDo([S]()
	{
		if (S->bAbort)
		{
			return;
		}
		S->FillRun = MakeShared<FDreamTextureReflowRun>(MakeLiveFillSpec(TEXT("src-upload-v1.dec-derive-v1")), S->MakeHooks(/*bRealUpload*/ false));
		S->FillRun->Start();
	});
	EnqueueWait(Test, TEXT("补通道：任务进入「等待 ComfyUI」（提交被后端接受）"), 30.0,
		[S]()
		{
			if (S->bAbort || !S->FillRun.IsValid() || S->FillRun->IsFinished())
			{
				return true;
			}
			return !S->FillRun->GetJobId().IsEmpty() && IsWaitingForComfy(S->Tracker->Find(S->FillRun->GetJobId()));
		},
		[S, Test]()
		{
			if (S->bAbort || !S->FillRun.IsValid())
			{
				return;
			}
			if (S->FillRun->IsFinished())
			{
				Test->AddError(FString::Printf(TEXT("补通道的运行意外结束了：%s"), *S->FillRun->GetMessage().ToString()));
				return;
			}
			const FDreamTextureJob* Job = S->Tracker->Find(S->FillRun->GetJobId());
			if (!Test->TestNotNull(TEXT("追踪器里有这个任务"), Job))
			{
				return;
			}
			Test->TestEqual(TEXT("工作流是组合 id"), Job->WorkflowId, FString(TEXT("src-upload-v1.dec-derive-v1")));
			Test->TestEqual(TEXT("来源标签是 reflow"), S->Tracker->GetOrigin(Job->Id), FString(TEXT("reflow")));
			Test->TestTrue(TEXT("运行停在「生成」这一步"), S->FillRun->GetStep() == EDreamTextureReflowStep::Generate && S->FillRun->IsRunning());
			Test->TestTrue(TEXT("界面上的说法点明在等 ComfyUI"), S->FillRun->GetMessage().ToString().Contains(TEXT("ComfyUI")));
			Test->TestFalse(TEXT("等 ComfyUI 时没有真实进度，不画假进度"), S->FillRun->GetProgress().IsSet());
			Test->AddInfo(FString::Printf(TEXT("补通道：任务 %s，运行说「%s」"), *Job->Id, *S->FillRun->GetMessage().ToString()));

			// 提交出去的请求自己也要过本地校验（derived_from 的枚举与指纹）。
			FString ValidateError;
			Test->TestTrue(FString::Printf(TEXT("提交的请求通过本地校验（%s）"), *ValidateError), S->FillRun->GetSubmittedRequest().Validate(ValidateError));

			// 读回后端记下的任务：derived_from 逐字段核对。
			FDreamTextureGenerateApi::GetJob(Job->Id, [S](bool bOK, FDreamTextureJob&& Got, const FString& Error, bool)
			{
				S->bGetDone = true;
				S->bGetOK = bOK;
				S->GotJob = MoveTemp(Got);
				S->GetError = Error;
			});
		});
	EnqueueWait(Test, TEXT("读回任务"), 20.0, [S]() { return S->bAbort || S->bGetDone || !S->FillRun.IsValid(); },
		[S, Test]()
		{
			if (S->bAbort || !S->FillRun.IsValid() || !S->bGetDone)
			{
				return;
			}
			if (!Test->TestTrue(FString::Printf(TEXT("读回任务成功（%s）"), *S->GetError), S->bGetOK))
			{
				return;
			}
			const FDreamTextureDerivedFrom& Got = S->GotJob.DerivedFrom;
			Test->TestEqual(TEXT("后端记下的 derived_from.kind"), Got.Kind, FString(TEXT("ue_material")));
			Test->TestEqual(TEXT("后端记下的 derived_from.mode"), Got.Mode, FString(TEXT("fill_channels")));
			Test->TestEqual(TEXT("后端记下的 derived_from.asset_path"), Got.AssetPath, FString(LiveAssetPath));
			Test->TestEqual(TEXT("后端记下的 derived_from.engine"), Got.Engine, FString(TEXT("5.8.2")));
			Test->TestEqual(TEXT("后端记下的 maps_in.basecolor 是送上去那份文件的指纹"), Got.MapsIn.FindRef(TEXT("basecolor")), S->FixtureSha);
			Test->TestEqual(TEXT("展示名"), S->GotJob.Name, FString(TEXT("LiveReflow fill")));
		});

	// ---- 6. 取消：后端发 job.failed(canceled)，运行说「在「生成」这一步失败：已取消」 ----
	EnqueueDo([S]()
	{
		if (!S->bAbort && S->FillRun.IsValid() && S->FillRun->IsRunning())
		{
			S->FillRun->Cancel();
		}
	});
	EnqueueWait(Test, TEXT("补通道：取消后运行走到终态"), 30.0,
		[S]() { return S->bAbort || !S->FillRun.IsValid() || S->FillRun->IsFinished(); },
		[S, Test]()
		{
			if (S->bAbort || !S->FillRun.IsValid())
			{
				return;
			}
			const FString Message = S->FillRun->GetMessage().ToString();
			Test->AddInfo(FString::Printf(TEXT("取消之后运行的说法：%s"), *Message));
			Test->TestTrue(TEXT("状态是失败（取消算在「生成」这一步上）"), S->FillRun->GetState() == FDreamTextureReflowRun::EState::Failed);
			Test->TestTrue(TEXT("失败在「生成」这一步"), S->FillRun->GetStep() == EDreamTextureReflowStep::Generate);
			Test->TestTrue(TEXT("说明点名「生成」并说已取消"), Message.Contains(TEXT("Failed at the \"Generate\" step")) && Message.Contains(TEXT("Canceled")));
			const FDreamTextureJob* Job = S->Tracker->Find(S->FillRun->GetJobId());
			Test->TestTrue(TEXT("任务状态是 canceled（不是 failed）"), Job != nullptr && Job->Status == EDreamTextureJobStatus::Canceled);
		});

	// ---- 7. 改写外观的提交同样被接受 ----
	EnqueueDo([S]()
	{
		if (S->bAbort)
		{
			return;
		}
		S->bGetDone = false;
		S->RestyleRun = MakeShared<FDreamTextureReflowRun>(MakeLiveRestyleSpec(TEXT("src-local-realistic-v1.dec-derive-v1")), S->MakeHooks(/*bRealUpload*/ false));
		S->RestyleRun->Start();
	});
	EnqueueWait(Test, TEXT("改写外观：任务进入「等待 ComfyUI」（提交被后端接受）"), 30.0,
		[S]()
		{
			if (S->bAbort || !S->RestyleRun.IsValid() || S->RestyleRun->IsFinished())
			{
				return true;
			}
			return !S->RestyleRun->GetJobId().IsEmpty() && IsWaitingForComfy(S->Tracker->Find(S->RestyleRun->GetJobId()));
		},
		[S, Test]()
		{
			if (S->bAbort || !S->RestyleRun.IsValid())
			{
				return;
			}
			if (S->RestyleRun->IsFinished())
			{
				Test->AddError(FString::Printf(TEXT("改写外观的运行意外结束了：%s"), *S->RestyleRun->GetMessage().ToString()));
				return;
			}
			const FDreamTextureJob* Job = S->Tracker->Find(S->RestyleRun->GetJobId());
			if (Test->TestNotNull(TEXT("追踪器里有这个任务"), Job))
			{
				Test->TestEqual(TEXT("工作流是组合 id"), Job->WorkflowId, FString(TEXT("src-local-realistic-v1.dec-derive-v1")));
				FDreamTextureGenerateApi::GetJob(Job->Id, [S](bool bOK, FDreamTextureJob&& Got, const FString& Error, bool)
				{
					S->bGetDone = true;
					S->bGetOK = bOK;
					S->GotJob = MoveTemp(Got);
					S->GetError = Error;
				});
			}
		});
	EnqueueWait(Test, TEXT("改写外观：读回任务"), 20.0, [S]() { return S->bAbort || S->bGetDone || !S->RestyleRun.IsValid(); },
		[S, Test]()
		{
			if (S->bAbort || !S->RestyleRun.IsValid() || !S->bGetDone)
			{
				return;
			}
			if (Test->TestTrue(FString::Printf(TEXT("读回任务成功（%s）"), *S->GetError), S->bGetOK))
			{
				Test->TestEqual(TEXT("后端记下的 derived_from.mode 是 restyle"), S->GotJob.DerivedFrom.Mode, FString(TEXT("restyle")));
				Test->TestEqual(TEXT("maps_in.basecolor"), S->GotJob.DerivedFrom.MapsIn.FindRef(TEXT("basecolor")), S->FixtureSha);
			}
		});

	// ---- 8. 收尾：任务一定要是终态，否则它占着后端唯一的 worker ----
	EnqueueDo([S]()
	{
		for (const TSharedPtr<FDreamTextureReflowRun>& Run : { S->FillRun, S->RestyleRun })
		{
			if (Run.IsValid() && Run->IsRunning())
			{
				Run->Cancel();
			}
		}
		// 运行还没拿到任务 id（卡在更早的步骤）时 Cancel 只做了记号；任务若已提交成功，追踪器里的活动任务逐个取消。
		if (S->Tracker.IsValid())
		{
			for (const FDreamTextureJob& Job : S->Tracker->GetActiveJobs())
			{
				S->Tracker->Cancel(Job.Id);
			}
		}
	});
	EnqueueWait(Test, TEXT("收尾：所有任务进入终态"), 30.0,
		[S]() { return !S->Tracker.IsValid() || S->Tracker->GetActiveJobs().Num() == 0; },
		[S, Test]()
		{
			if (S->bAbort || !S->RestyleRun.IsValid())
			{
				return;
			}
			const FString Message = S->RestyleRun->GetMessage().ToString();
			Test->AddInfo(FString::Printf(TEXT("改写外观取消之后运行的说法：%s"), *Message));
			Test->TestTrue(TEXT("改写外观：取消后失败在「生成」这一步"), S->RestyleRun->GetState() == FDreamTextureReflowRun::EState::Failed
				&& S->RestyleRun->GetStep() == EDreamTextureReflowStep::Generate && Message.Contains(TEXT("Canceled")));
		});

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
