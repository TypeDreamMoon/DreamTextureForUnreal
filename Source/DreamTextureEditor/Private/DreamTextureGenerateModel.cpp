// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureGenerateModel.h"

#include "Containers/Ticker.h"
#include "DreamTextureClientModule.h"
#include "DreamTextureConnection.h"
#include "DreamTextureSettings.h"

#define LOCTEXT_NAMESPACE "DreamTextureGenerateModel"

namespace
{
	/**
	 * 后端给的原因常常自带句末标点（Go 的错误文本，英文句号），我们的句子接在它后面再加一个「。」就成了「it.。」。
	 * 去掉结尾的空白和句号，由我们的句子自己收尾。
	 */
	FString StripTrailingPunctuation(FString Text)
	{
		Text.TrimEndInline();
		while (Text.Len() > 0 && (Text.EndsWith(TEXT(".")) || Text.EndsWith(TEXT("。")) || Text.EndsWith(TEXT(";")) || Text.EndsWith(TEXT("；"))))
		{
			Text.LeftChopInline(1);
			Text.TrimEndInline();
		}
		return Text;
	}
}

FDreamTextureGenerateBackend FDreamTextureGenerateBackend::MakeDefault()
{
	FDreamTextureGenerateBackend Out;
	Out.ListWorkflows = [](FDreamTextureGenerateApi::FOnCatalog Done)
	{
		FDreamTextureGenerateApi::ListWorkflows(MoveTemp(Done));
	};
	Out.ListRefineModels = [](FDreamTextureGenerateApi::FOnRefineModels Done)
	{
		FDreamTextureGenerateApi::ListRefineModels(MoveTemp(Done));
	};
	Out.ListImagenProviders = [](FDreamTextureGenerateApi::FOnImagenProviders Done)
	{
		FDreamTextureGenerateApi::ListImagenProviders(MoveTemp(Done));
	};
	Out.Refine = [](const FString& Prompt, EDreamTextureRefinePurpose Purpose, const FString& Model, FDreamTextureGenerateApi::FOnRefined Done)
	{
		FDreamTextureGenerateApi::RefinePrompt(Prompt, Purpose, Model, MoveTemp(Done));
	};
	Out.EnsureBackend = [](FDreamTextureProcess::FEnsureDone Done)
	{
		const TSharedPtr<FDreamTextureProcess> Process = FDreamTextureClientModule::IsAvailable() ? FDreamTextureClientModule::Get().GetProcess() : nullptr;
		if (Process.IsValid())
		{
			Process->EnsureBackend(MoveTemp(Done));
			return;
		}

		// 进程管理器不在（客户端模块没加载）：和它自己的约定一致，失败也异步回调。
		FTSTicker::GetCoreTicker().AddTicker(TEXT("DreamTexture.GenerateEnsureBackend"), 0.0f,
			[Done = MoveTemp(Done)](float) -> bool
			{
				Done(false, LOCTEXT("NoEnsure", "The client module is not ready, so it cannot manage the backend process.").ToString());
				return false;
			});
	};
	Out.HasBackendExecutable = []()
	{
		return !UDreamTextureSettings::Get().BackendExecutable.FilePath.TrimStartAndEnd().IsEmpty();
	};
	Out.Enqueue = [](const FString& MaterialId)
	{
		return FDreamTextureImportQueue::Get().Enqueue(MaterialId);
	};
	Out.FindImportStatus = [](const FString& MaterialId) -> const FDreamTextureImportStatus*
	{
		return FDreamTextureImportQueue::Get().FindStatus(MaterialId);
	};
	Out.GetTracker = []() -> TSharedPtr<FDreamTextureJobTracker>
	{
		return FDreamTextureClientModule::IsAvailable() ? FDreamTextureClientModule::Get().GetJobTracker() : nullptr;
	};
	Out.GetConnection = []() -> TSharedPtr<FDreamTextureConnection>
	{
		// 客户端模块可能没加载（commandlet、自动化里），GetConnection() 在那时会炸，必须先问。
		return FDreamTextureClientModule::IsAvailable() ? FDreamTextureClientModule::Get().GetConnection() : nullptr;
	};
	Out.GetAutoImport = []()
	{
		return UDreamTextureSettings::Get().bAutoImportGenerated;
	};
	Out.SetAutoImport = [](bool bAuto)
	{
		// 和项目设置里是同一个值：这里改了，设置面板里立刻跟着变，反过来也一样。
		// 写进工程的 DefaultEditor.ini 才算「记住」；写不进去（只读、没检出）时只在本次编辑器里生效，
		// TryUpdateDefaultConfigFile 自己会告警。
		UDreamTextureSettings* Settings = GetMutableDefault<UDreamTextureSettings>();
		Settings->bAutoImportGenerated = bAuto;
		Settings->TryUpdateDefaultConfigFile();
	};
	return Out;
}

FDreamTextureGenerateModel::FDreamTextureGenerateModel(FDreamTextureGenerateBackend InBackend)
	: Backend(MoveTemp(InBackend))
{
}

FDreamTextureGenerateModel::~FDreamTextureGenerateModel()
{
	if (bBoundExternal)
	{
		// AddSP 绑的是弱引用，漏摘不会悬空，但死条目要等到下一次广播才被清掉。
		// 面板是 nomad tab，开开关关频繁，不攒着。
		if (const TSharedPtr<FDreamTextureConnection> Conn = BoundConnection.Pin())
		{
			Conn->OnLinkChanged.Remove(LinkHandle);
		}
	}
}

void FDreamTextureGenerateModel::Initialize(bool bBindExternal)
{
	// 找回上一批：任务在追踪器里，面板关掉再打开，进度条该接着走，而不是回到「还没生成过」。
	RestoreLatestBatch();

	if (!bBindExternal || bBoundExternal)
	{
		return;
	}
	bBoundExternal = true;

	if (Backend.GetConnection)
	{
		if (const TSharedPtr<FDreamTextureConnection> Conn = Backend.GetConnection())
		{
			BoundConnection = Conn;
			LinkHandle = Conn->OnLinkChanged.AddSP(this, &FDreamTextureGenerateModel::HandleLinkChanged);
		}
	}
}

//////////////////////////////////////////////////////////////////////////
// 目录

void FDreamTextureGenerateModel::LoadCatalog()
{
	if (!Backend.ListWorkflows)
	{
		return;
	}

	// 重复调用作废在飞的那次：回来对不上代数就丢。
	const uint64 Gen = ++CatalogGeneration;
	CatalogState = ECatalogState::Loading;
	CatalogError.Reset();
	OnFormChanged.Broadcast();

	TWeakPtr<FDreamTextureGenerateModel> WeakSelf = AsShared();
	Backend.ListWorkflows(
		[WeakSelf, Gen](bool bOK, FDreamTextureWorkflowCatalog&& InCatalog, const FString& Error)
		{
			if (const TSharedPtr<FDreamTextureGenerateModel> Self = WeakSelf.Pin())
			{
				Self->ApplyCatalog(Gen, bOK, MoveTemp(InCatalog), Error);
			}
		});
}

void FDreamTextureGenerateModel::ApplyCatalog(uint64 Generation, bool bOK, FDreamTextureWorkflowCatalog&& InCatalog, const FString& Error)
{
	if (Generation != CatalogGeneration)
	{
		return;
	}

	if (!bOK)
	{
		// 失败时保留旧目录：已经在用的选择别因为一次重连失败就清空。
		CatalogState = Catalog.Workflows.Num() > 0 ? ECatalogState::Ready : ECatalogState::Failed;
		CatalogError = Error.IsEmpty() ? LOCTEXT("CatalogReadFailed", "Could not read the workflow list.").ToString() : Error;
		OnFormChanged.Broadcast();

		// 后端是起来了，可没拿到工作流列表，就没法按表单提交：把原因摆出来，别悄悄停在半路。
		if (AutoStartState == EAutoStartState::LoadingCatalog)
		{
			FailAutoStart(FText::FromString(CatalogError));
		}
		return;
	}

	Catalog = MoveTemp(InCatalog);
	CatalogState = ECatalogState::Ready;
	CatalogError.Reset();
	EnsureSelection();
	MaybeLoadCloudModels();
	OnFormChanged.Broadcast();

	if (AutoStartState == EAutoStartState::LoadingCatalog)
	{
		FinishAutoStart();
	}
}

void FDreamTextureGenerateModel::EnsureSelection()
{
	const TArray<const FDreamTextureWorkflowMeta*> Sources = Catalog.GetSourceSegments();
	const TArray<const FDreamTextureWorkflowMeta*> Decomposes = Catalog.GetDecomposeSegments();

	const bool bHasSource = Sources.ContainsByPredicate([this](const FDreamTextureWorkflowMeta* Meta) { return Meta->Id == SourceId; });
	if (!bHasSource)
	{
		SourceId.Reset();
		// 默认优先本地出图：云端底图要令牌、按量计费，第一次打开页面不该替用户选一个会花钱的。
		for (const FDreamTextureWorkflowMeta* Meta : Sources)
		{
			if (!Meta->bCloudSource)
			{
				SourceId = Meta->Id;
				break;
			}
		}
		if (SourceId.IsEmpty() && Sources.Num() > 0)
		{
			SourceId = Sources[0]->Id;
		}
	}

	const bool bHasDecompose = Decomposes.ContainsByPredicate([this](const FDreamTextureWorkflowMeta* Meta) { return Meta->Id == DecomposeId; });
	if (!bHasDecompose)
	{
		// 与 Web 端一致：按 id 排序的第一个。许可提示在选中后马上显示，不靠默认值回避。
		DecomposeId = Decomposes.Num() > 0 ? Decomposes[0]->Id : FString();
	}
}

void FDreamTextureGenerateModel::SetSourceId(const FString& Id)
{
	if (Id == SourceId)
	{
		return;
	}
	SourceId = Id;
	MaybeLoadCloudModels();
	OnFormChanged.Broadcast();
}

void FDreamTextureGenerateModel::SetDecomposeId(const FString& Id)
{
	if (Id == DecomposeId)
	{
		return;
	}
	DecomposeId = Id;
	OnFormChanged.Broadcast();
}

const FDreamTextureWorkflowMeta* FDreamTextureGenerateModel::GetCurrentWorkflow() const
{
	return Catalog.FindComposed(SourceId, DecomposeId);
}

//////////////////////////////////////////////////////////////////////////
// 表单

TArray<int32> FDreamTextureGenerateModel::GetResolutionOptions() const
{
	TArray<int32> Options;
	if (const FDreamTextureWorkflowMeta* Workflow = GetCurrentWorkflow())
	{
		if (const FDreamTextureParamSpec* Spec = Workflow->FindParam(TEXT("resolution")))
		{
			Spec->TryGetIntOptions(Options);
		}
	}
	return Options;
}

void FDreamTextureGenerateModel::SetResolution(int32 InResolution)
{
	ResolutionChoice = InResolution;
}

int32 FDreamTextureGenerateModel::GetResolution() const
{
	const FDreamTextureWorkflowMeta* Workflow = GetCurrentWorkflow();
	const FDreamTextureParamSpec* Spec = Workflow != nullptr ? Workflow->FindParam(TEXT("resolution")) : nullptr;
	if (Spec == nullptr)
	{
		return 0;
	}

	const TArray<int32> Options = GetResolutionOptions();
	if (Options.Num() > 0)
	{
		// 换了出图/分解模型后，之前选的分辨率可能不在新的候选里：回到默认，别提交一个会被后端拒掉的值。
		if (ResolutionChoice > 0 && Options.Contains(ResolutionChoice))
		{
			return ResolutionChoice;
		}
		const int32 Default = Spec->GetDefaultInt(Options[0]);
		return Options.Contains(Default) ? Default : Options[0];
	}
	return ResolutionChoice > 0 ? ResolutionChoice : Spec->GetDefaultInt(0);
}

void FDreamTextureGenerateModel::SetVariants(int32 InVariants)
{
	Variants = FMath::Clamp(InVariants, 1, DreamTextureGenerateLimits::MaxVariants);
}

void FDreamTextureGenerateModel::SetFixedSeed(int64 InSeed)
{
	FixedSeed = FMath::Clamp<int64>(InSeed, 0, GetMaxSeedForUI());
}

//////////////////////////////////////////////////////////////////////////
// 云端底图

bool FDreamTextureGenerateModel::IsCloudSource() const
{
	const FDreamTextureWorkflowMeta* Source = Catalog.FindSegment(SourceId);
	return Source != nullptr && Source->bCloudSource;
}

void FDreamTextureGenerateModel::MaybeLoadCloudModels()
{
	if (IsCloudSource() && (CloudState == ECloudState::NotLoaded || CloudState == ECloudState::Failed))
	{
		LoadCloudModels();
	}
}

void FDreamTextureGenerateModel::LoadCloudModels()
{
	if (!Backend.ListImagenProviders)
	{
		return;
	}

	const uint64 Gen = ++CloudGeneration;
	CloudState = ECloudState::Loading;
	CloudError.Reset();
	OnFormChanged.Broadcast();

	TWeakPtr<FDreamTextureGenerateModel> WeakSelf = AsShared();
	Backend.ListImagenProviders(
		[WeakSelf, Gen](bool bOK, FDreamTextureImagenProviders&& Providers, const FString& Error)
		{
			if (const TSharedPtr<FDreamTextureGenerateModel> Self = WeakSelf.Pin())
			{
				Self->ApplyCloudModels(Gen, bOK, MoveTemp(Providers), Error);
			}
		});
}

void FDreamTextureGenerateModel::ApplyCloudModels(uint64 Generation, bool bOK, FDreamTextureImagenProviders&& Providers, const FString& Error)
{
	if (Generation != CloudGeneration)
	{
		return;
	}

	if (!bOK)
	{
		CloudState = ECloudState::Failed;
		CloudError = Error.IsEmpty() ? LOCTEXT("CloudReadFailed", "Could not read the cloud model list.").ToString() : Error;
		OnFormChanged.Broadcast();

		if (AutoStartState == EAutoStartState::LoadingCloud)
		{
			FailAutoStart(FText::FromString(CloudError));
		}
		return;
	}

	CloudProviders = MoveTemp(Providers);
	CloudState = ECloudState::Ready;
	CloudError.Reset();
	OnFormChanged.Broadcast();

	if (AutoStartState == EAutoStartState::LoadingCloud)
	{
		FinishAutoStart();
	}
}

const FDreamTextureImagenProvider* FDreamTextureGenerateModel::GetCloudProvider() const
{
	const FDreamTextureWorkflowMeta* Source = Catalog.FindSegment(SourceId);
	if (Source == nullptr || !Source->bCloudSource || CloudState != ECloudState::Ready)
	{
		return nullptr;
	}
	return CloudProviders.Find(Source->CloudProvider);
}

TArray<const FDreamTextureImagenModel*> FDreamTextureGenerateModel::GetCloudModels() const
{
	TArray<const FDreamTextureImagenModel*> Out;
	if (const FDreamTextureImagenProvider* Provider = GetCloudProvider())
	{
		for (const FDreamTextureImagenModel& Model : Provider->Models)
		{
			Out.Add(&Model);
		}
	}
	return Out;
}

void FDreamTextureGenerateModel::SetCloudModel(const FString& Id)
{
	for (const FDreamTextureImagenModel* Model : GetCloudModels())
	{
		if (Model->Id == Id)
		{
			CloudModelChoice = Id;
			return;
		}
	}
}

FString FDreamTextureGenerateModel::GetCloudModelId() const
{
	const TArray<const FDreamTextureImagenModel*> Models = GetCloudModels();
	if (Models.Num() == 0)
	{
		return FString();
	}
	// 选的那个换了来源之后可能不在新清单里：落到第一个。后端把认得的、推荐的排在前面。
	for (const FDreamTextureImagenModel* Model : Models)
	{
		if (Model->Id == CloudModelChoice)
		{
			return Model->Id;
		}
	}
	return Models[0]->Id;
}

TArray<FString> FDreamTextureGenerateModel::GetCloudQualityOptions() const
{
	const FDreamTextureWorkflowMeta* Workflow = GetCurrentWorkflow();
	const FDreamTextureParamSpec* Spec = Workflow != nullptr ? Workflow->FindParam(TEXT("api_quality")) : nullptr;
	return Spec != nullptr ? Spec->Options : TArray<FString>();
}

FString FDreamTextureGenerateModel::GetCloudQuality() const
{
	const TArray<FString> Options = GetCloudQualityOptions();
	if (Options.Contains(CloudQuality))
	{
		return CloudQuality;
	}
	const FDreamTextureWorkflowMeta* Workflow = GetCurrentWorkflow();
	const FDreamTextureParamSpec* Spec = Workflow != nullptr ? Workflow->FindParam(TEXT("api_quality")) : nullptr;
	if (Spec != nullptr && Spec->bHasDefault && Options.Contains(Spec->Default))
	{
		return Spec->Default;
	}
	return Options.Num() > 0 ? Options[0] : FString();
}

bool FDreamTextureGenerateModel::CheckCloudReady(FText* OutReason) const
{
	auto Fail = [OutReason](const FText& Why)
	{
		if (OutReason)
		{
			*OutReason = Why;
		}
		return false;
	};

	switch (CloudState)
	{
	case ECloudState::Ready:
		break;
	case ECloudState::Failed:
		return Fail(FText::Format(LOCTEXT("CloudFailed", "Could not look up the available cloud models: {0}"), FText::FromString(CloudError)));
	default:
		return Fail(LOCTEXT("CloudLoading", "Looking up the available cloud models…"));
	}

	const FDreamTextureWorkflowMeta* Source = Catalog.FindSegment(SourceId);
	const FDreamTextureImagenProvider* Provider = GetCloudProvider();
	if (Provider == nullptr)
	{
		return Fail(FText::Format(LOCTEXT("CloudNoProvider", "The backend has no cloud source registered as \"{0}\"."),
			FText::FromString(Source != nullptr ? Source->CloudProvider : FString())));
	}

	const FText Label = FText::FromString(Provider->Label.IsEmpty() ? Provider->Id : Provider->Label);
	if (!Provider->bConfigured)
	{
		// 没填令牌不是错误，是要去设置：指路，别只说「不行」。
		return Fail(FText::Format(LOCTEXT("CloudNoToken",
			"{0} has no API key yet. Enter it under Models > Settings in the web UI, then come back and pick a cloud model."), Label));
	}
	if (!Provider->Error.IsEmpty())
	{
		return Fail(FText::Format(LOCTEXT("CloudError", "Could not reach {0}: {1}"), Label, FText::FromString(Provider->Error)));
	}
	if (Provider->Models.Num() == 0)
	{
		return Fail(FText::Format(LOCTEXT("CloudNoModels", "{0} has no models available."), Label));
	}
	if (GetCloudModelId().IsEmpty())
	{
		return Fail(LOCTEXT("CloudPickModel", "Pick a cloud model first."));
	}
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 许可

FDreamTextureGenerateLicenseHint FDreamTextureGenerateModel::GetLicenseHint() const
{
	FDreamTextureGenerateLicenseHint Hint;

	// 许可看并集，且以后端拼出来的组合为准（和 Web 端 current.licenses 一致）。
	// 组合暂时找不到时退一步，自己把两段的许可并起来——不能因为目录还没到齐就不提醒。
	FDreamTextureWorkflowMeta Union;
	const FDreamTextureWorkflowMeta* Source = &Union;
	if (const FDreamTextureWorkflowMeta* Workflow = GetCurrentWorkflow())
	{
		Source = Workflow;
	}
	else
	{
		for (const FString& Id : { SourceId, DecomposeId })
		{
			if (const FDreamTextureWorkflowMeta* Segment = Catalog.FindSegment(Id))
			{
				Union.Licenses.Append(Segment->Licenses);
			}
		}
	}

	Hint.bNonCommercial = !Source->IsCommercial();
	Hint.Reason = Source->GetNonCommercialReason();
	for (const FDreamTextureLicenseNotice& Notice : Source->Licenses)
	{
		if (!Notice.bCommercial)
		{
			Hint.Components.Add(Notice.Component);
		}
	}
	Hint.Mismatch = Source->Mismatch;

	if (Hint.bNonCommercial)
	{
		for (const FDreamTextureWorkflowMeta* Meta : Catalog.GetDecomposeSegments())
		{
			if (Meta->IsCommercial())
			{
				Hint.CommercialDecomposeNames.Add(Meta->Name.IsEmpty() ? Meta->Id : Meta->Name);
			}
		}
	}
	return Hint;
}

//////////////////////////////////////////////////////////////////////////
// 能不能提交

FDreamTextureGenerateReadiness FDreamTextureGenerateModel::EvaluateReadiness(const TSharedPtr<FDreamTextureConnection>& Conn)
{
	FDreamTextureGenerateReadiness Out;

	// ── 第 1 级：后端进程 ──
	if (!Conn.IsValid())
	{
		Out.Tier = EDreamTextureGenerateTier::Backend;
		Out.Reason = LOCTEXT("NoConnection", "The client module is not ready, so there is no backend connection (tier 1: backend process). Open the Connection Status page, click Reconnect, and try again.");
		return Out;
	}

	if (!Conn->IsBackendReachable())
	{
		Out.Tier = EDreamTextureGenerateTier::Backend;

		// Offline 有两种含义：「还没开始连」和「连过又断了」。前者说成「没应答」会让人以为坏了。
		const bool bKnown = Conn->GetLink() != EDreamTextureLink::Offline || !Conn->GetLastError().IsEmpty();
		Out.bConnecting = !bKnown;
		Out.Reason = bKnown
			? LOCTEXT("BackendDown", "The backend process is not responding (tier 1). Make sure dreamtexture.exe is running and the address and port are correct, then click Reconnect on the Connection Status page.")
			: LOCTEXT("BackendConnecting", "Connecting to the backend process (tier 1)…");
		return Out;
	}

	// ── 第 2 级：长连接。只收不发，挡不住提交；但它不通的时候第 3 级的状态是过期的，也收不到进度 ──
	if (Conn->GetLink() != EDreamTextureLink::Live)
	{
		Out.bCanSubmit = true;
		Out.Tier = EDreamTextureGenerateTier::Link;
		Out.Note = LOCTEXT("LinkNotLive", "The event stream is not connected (tier 2). You can still submit, but progress only catches up after it reconnects, and live ComfyUI status is unavailable.");
		return Out;
	}

	// ── 第 3 级：ComfyUI ──
	const FDreamTextureHealth& Health = Conn->GetHealth();

	// 连上后第一条消息必定是 comfy.status，Mode 为空说明它还没到：状态未知，不拦。
	if (Health.Mode.IsEmpty())
	{
		Out.bCanSubmit = true;
		return Out;
	}

	// bStarting 排在最前：冷启动几十秒到几分钟是常态，这一档的意思是「等着，别动」。
	if (Health.bStarting)
	{
		Out.Tier = EDreamTextureGenerateTier::Comfy;
		Out.Reason = FText::Format(LOCTEXT("ComfyStarting",
			"ComfyUI is starting (tier 3), {0} s elapsed. A cold start takes from tens of seconds to a few minutes. The button enables itself once ComfyUI is ready, so do not restart anything."),
			FText::AsNumber(Health.StartingSecs));
		return Out;
	}

	if (Health.bUserStopped)
	{
		Out.Tier = EDreamTextureGenerateTier::Comfy;
		Out.Reason = LOCTEXT("ComfyStopped", "ComfyUI was stopped manually (tier 3). The backend process is fine; start ComfyUI from the Connection Status page.");
		return Out;
	}

	if (!Health.bAlive)
	{
		Out.Tier = EDreamTextureGenerateTier::Comfy;
		Out.Reason = Health.Reason.IsEmpty()
			? LOCTEXT("ComfyDown", "ComfyUI is not running (tier 3). The backend process is fine, so do not restart the backend.")
			: FText::Format(LOCTEXT("ComfyDownWhy", "ComfyUI is not running (tier 3): {0}. The backend process is fine, so do not restart the backend."), FText::FromString(StripTrailingPunctuation(Health.Reason)));
		return Out;
	}

	Out.bCanSubmit = true;
	if (!Health.bReady)
	{
		// Alive 但不 Ready 是常态（队列积压），不是故障：提交后后端会排队，不拦。
		Out.Tier = EDreamTextureGenerateTier::Comfy;
		Out.Note = Health.Reason.IsEmpty()
			? LOCTEXT("ComfyBusy", "ComfyUI is not accepting new work right now (tier 3). Submitted jobs will queue until it is.")
			: FText::Format(LOCTEXT("ComfyBusyWhy", "ComfyUI is not accepting new work right now (tier 3): {0}. Submitted jobs will queue until it is."), FText::FromString(StripTrailingPunctuation(Health.Reason)));
	}
	return Out;
}

FDreamTextureGenerateReadiness FDreamTextureGenerateModel::GetReadiness() const
{
	return EvaluateReadiness(Backend.GetConnection ? Backend.GetConnection() : nullptr);
}

bool FDreamTextureGenerateModel::BuildRequest(FDreamTextureGenerateRequest& OutRequest, FText* OutError) const
{
	const FDreamTextureWorkflowMeta* Workflow = GetCurrentWorkflow();
	if (Workflow == nullptr)
	{
		if (OutError)
		{
			*OutError = SourceId.IsEmpty() || DecomposeId.IsEmpty()
				? LOCTEXT("NoSelection", "Pick an image model and a decompose model first.")
				: FText::Format(LOCTEXT("NoCombo", "The backend has no workflow for the combination \"{0} × {1}\". A segment may have just been added and the backend has not reloaded its workflows yet."),
					FText::FromString(SourceId), FText::FromString(DecomposeId));
		}
		return false;
	}

	FDreamTextureGenerateRequest Request;
	Request.WorkflowId = Workflow->Id;
	Request.Variants = Variants;
	Request.StringParams.Add(TEXT("prompt"), Prompt.TrimStartAndEnd());
	Request.Name = FDreamTextureGenerateRequest::MakeNameFromPrompt(Prompt);

	const int32 Resolution = GetResolution();
	if (Resolution > 0)
	{
		Request.IntParams.Add(TEXT("resolution"), Resolution);
	}
	Request.SetSeed(bRandomSeed ? DreamTextureGenerateLimits::RandomSeed : FixedSeed);

	if (IsCloudSource())
	{
		// 云端模型填到哪个参数里，看声明里的控件提示，不认识任何具体参数名；
		// 声明没带提示（更老的后端）时退回约定的名字。
		const FDreamTextureParamSpec* ModelParam = Workflow->FindParamByWidget(TEXT("imagen-model"));
		Request.StringParams.Add(ModelParam != nullptr ? ModelParam->Key : FString(TEXT("api_model")), GetCloudModelId());

		if (Workflow->FindParam(TEXT("api_quality")) != nullptr)
		{
			Request.StringParams.Add(TEXT("api_quality"), GetCloudQuality());
		}
	}

	OutRequest = MoveTemp(Request);
	return true;
}

bool FDreamTextureGenerateModel::CanSubmit(FText* OutReason) const
{
	return CheckSubmittable(OutReason, /*bAfterAutoStart*/ false);
}

bool FDreamTextureGenerateModel::CheckSubmittable(FText* OutReason, bool bAfterAutoStart) const
{
	auto Fail = [OutReason](const FText& Why)
	{
		if (OutReason)
		{
			*OutReason = Why;
		}
		return false;
	};

	// 先看连接这三级：后端不在的时候目录也读不到，那时候告诉人「后端没应答」比「读不到工作流列表」有用得多。
	// 「启动后端并生成」的那一次提交跳过这一步：后端刚应答，连接对象要过几秒才追上，它此刻说的是旧话。
	if (!bAfterAutoStart)
	{
		const FDreamTextureGenerateReadiness Readiness = GetReadiness();
		if (!Readiness.bCanSubmit)
		{
			return Fail(Readiness.Reason);
		}
	}

	if (bSubmitting)
	{
		return Fail(LOCTEXT("Submitting", "Submitting…"));
	}

	switch (CatalogState)
	{
	case ECatalogState::Ready:
		break;
	case ECatalogState::Failed:
		return Fail(FText::Format(LOCTEXT("CatalogFailed", "Could not read the workflow list: {0}"), FText::FromString(CatalogError)));
	default:
		return Fail(LOCTEXT("CatalogLoading", "Reading the workflow list…"));
	}

	if (Prompt.TrimStartAndEnd().IsEmpty())
	{
		return Fail(LOCTEXT("NeedPrompt", "Write a prompt first."));
	}

	FText CloudWhy;
	if (IsCloudSource() && !CheckCloudReady(&CloudWhy))
	{
		return Fail(CloudWhy);
	}

	FDreamTextureGenerateRequest Request;
	FText BuildError;
	if (!BuildRequest(Request, &BuildError))
	{
		return Fail(BuildError);
	}

	// 最后一道：Client 的校验。界面上的上限已经卡过一遍，这里是兜底，不是第一道。
	FString ValidateError;
	if (!Request.Validate(ValidateError))
	{
		return Fail(FText::FromString(ValidateError));
	}
	return true;
}

void FDreamTextureGenerateModel::Submit()
{
	// 提交在途时再点（快速双击、回车连按）：静默忽略。别走下面的校验——那会把「正在提交」当成一条错误弹出来，
	// 还会把上一次提交留下的错误状态冲掉。「启动后端并生成」走着的时候同理：那条流程自己会提交。
	if (bSubmitting || IsAutoStarting())
	{
		return;
	}
	SubmitInternal(/*bAfterAutoStart*/ false);
}

void FDreamTextureGenerateModel::SubmitInternal(bool bAfterAutoStart)
{
	FText Why;
	if (!CheckSubmittable(&Why, bAfterAutoStart))
	{
		SubmitError = Why;
		OnNotice.Broadcast(Why, true);
		OnFormChanged.Broadcast();
		return;
	}

	FDreamTextureGenerateRequest Request;
	BuildRequest(Request);

	const TSharedPtr<FDreamTextureJobTracker> Tracker = Backend.GetTracker ? Backend.GetTracker() : nullptr;
	if (!Tracker.IsValid())
	{
		SubmitError = LOCTEXT("NoTracker", "The job tracker is not ready (the client module is not loaded), so nothing can be submitted.");
		OnNotice.Broadcast(SubmitError, true);
		OnFormChanged.Broadcast();
		return;
	}

	SubmitError = FText::GetEmpty();
	bSubmitting = true;
	const uint64 Gen = ++SubmitGeneration;
	OnFormChanged.Broadcast();

	TWeakPtr<FDreamTextureGenerateModel> WeakSelf = AsShared();
	Tracker->Submit(Request,
		[WeakSelf, Gen](bool bOK, TArray<FDreamTextureJob>&& Jobs, const FString& Error)
		{
			if (const TSharedPtr<FDreamTextureGenerateModel> Self = WeakSelf.Pin())
			{
				Self->ApplySubmitResult(Gen, bOK, MoveTemp(Jobs), Error);
			}
		});
}

//////////////////////////////////////////////////////////////////////////
// 启动后端并生成

bool FDreamTextureGenerateModel::HasBackendExecutable() const
{
	return Backend.HasBackendExecutable && Backend.HasBackendExecutable();
}

FDreamTextureGenerateModel::EPrimaryAction FDreamTextureGenerateModel::GetPrimaryAction() const
{
	const FDreamTextureGenerateReadiness Readiness = GetReadiness();

	// 只在「连过了确实没应答」时才提供启动：还在连的时候多半几秒内自己就好，抢着拉起一个进程反而添乱；
	// 而 EnsureBackend 本身会先探端口，端口上有后端就附着，所以连接对象判断得稍微滞后也不会起出第二个。
	if (!Readiness.bCanSubmit && Readiness.Tier == EDreamTextureGenerateTier::Backend && !Readiness.bConnecting
		&& HasBackendExecutable() && Backend.EnsureBackend)
	{
		return EPrimaryAction::StartAndSubmit;
	}
	return EPrimaryAction::Submit;
}

bool FDreamTextureGenerateModel::NeedsExecutableSetting() const
{
	const FDreamTextureGenerateReadiness Readiness = GetReadiness();
	return !Readiness.bCanSubmit && Readiness.Tier == EDreamTextureGenerateTier::Backend && !Readiness.bConnecting
		&& !HasBackendExecutable();
}

bool FDreamTextureGenerateModel::ShouldShowOfflineForm() const
{
	return GetPrimaryAction() == EPrimaryAction::StartAndSubmit;
}

bool FDreamTextureGenerateModel::CanAutoStart(FText* OutReason) const
{
	auto Fail = [OutReason](const FText& Why)
	{
		if (OutReason)
		{
			*OutReason = Why;
		}
		return false;
	};

	if (IsAutoStarting())
	{
		return Fail(LOCTEXT("AutoStarting", "Starting the backend…"));
	}
	if (bSubmitting)
	{
		return Fail(LOCTEXT("Submitting", "Submitting…"));
	}
	if (!Backend.EnsureBackend)
	{
		return Fail(LOCTEXT("NoEnsure", "The client module is not ready, so it cannot manage the backend process."));
	}
	if (!HasBackendExecutable())
	{
		return Fail(LOCTEXT("NoExecutable", "No backend executable is configured, so the plugin does not know which program to launch. Set one in Project Settings."));
	}
	if (Prompt.TrimStartAndEnd().IsEmpty())
	{
		return Fail(LOCTEXT("NeedPrompt", "Write a prompt first."));
	}
	return true;
}

bool FDreamTextureGenerateModel::CanActivatePrimary(FText* OutReason) const
{
	if (IsAutoStarting())
	{
		if (OutReason)
		{
			*OutReason = LOCTEXT("AutoStarting", "Starting the backend…");
		}
		return false;
	}
	return GetPrimaryAction() == EPrimaryAction::StartAndSubmit ? CanAutoStart(OutReason) : CanSubmit(OutReason);
}

void FDreamTextureGenerateModel::ActivatePrimary()
{
	if (IsAutoStarting())
	{
		return;
	}
	if (GetPrimaryAction() == EPrimaryAction::StartAndSubmit)
	{
		StartBackendAndSubmit();
	}
	else
	{
		Submit();
	}
}

void FDreamTextureGenerateModel::StartBackendAndSubmit()
{
	// 正在启动 / 正在提交时再点：静默忽略，不再起第二个。
	if (IsAutoStarting() || bSubmitting)
	{
		return;
	}

	FText Why;
	if (!CanAutoStart(&Why))
	{
		SubmitError = Why;
		OnNotice.Broadcast(Why, true);
		OnFormChanged.Broadcast();
		return;
	}

	SubmitError = FText::GetEmpty();
	AutoStartState = EAutoStartState::StartingBackend;
	const uint64 Gen = ++AutoStartGeneration;
	OnFormChanged.Broadcast();

	TWeakPtr<FDreamTextureGenerateModel> WeakSelf = AsShared();
	Backend.EnsureBackend(
		[WeakSelf, Gen](bool bOK, const FString& Error)
		{
			if (const TSharedPtr<FDreamTextureGenerateModel> Self = WeakSelf.Pin())
			{
				Self->HandleBackendEnsured(Gen, bOK, Error);
			}
		});
}

void FDreamTextureGenerateModel::HandleBackendEnsured(uint64 Generation, bool bOK, const FString& Error)
{
	// 用户取消了（或又开了新的一轮）：这一轮的结果不再算数。后端起来了就让它活着，我们没有资格去停。
	if (Generation != AutoStartGeneration || AutoStartState != EAutoStartState::StartingBackend)
	{
		return;
	}

	if (!bOK)
	{
		// 原因是进程管理器给的中文，原样显示，不加工。
		FailAutoStart(FText::FromString(Error.IsEmpty() ? LOCTEXT("StartBackendFailed", "Could not start the backend.").ToString() : Error));
		return;
	}

	// 后端应答了。目录没拉到就补拉（之前后端不在，目录请求多半失败过），拉到再提交。
	if (CatalogState != ECatalogState::Ready)
	{
		AutoStartState = EAutoStartState::LoadingCatalog;
		LoadCatalog();
		return;
	}
	FinishAutoStart();
}

void FDreamTextureGenerateModel::FinishAutoStart()
{
	// 所选是云端出图模型：云端清单也要等到位，否则提交会被「正在查云端可用的模型」挡回来。
	if (IsCloudSource() && CloudState != ECloudState::Ready)
	{
		AutoStartState = EAutoStartState::LoadingCloud;
		if (CloudState != ECloudState::Loading)
		{
			LoadCloudModels();
		}
		return;
	}

	AutoStartState = EAutoStartState::Idle;
	++AutoStartGeneration;
	OnFormChanged.Broadcast();
	SubmitInternal(/*bAfterAutoStart*/ true);
}

void FDreamTextureGenerateModel::FailAutoStart(const FText& Why)
{
	AutoStartState = EAutoStartState::Idle;
	++AutoStartGeneration;
	SubmitError = Why;
	OnNotice.Broadcast(Why, true);
	OnFormChanged.Broadcast();
}

void FDreamTextureGenerateModel::CancelAutoStart()
{
	if (!IsAutoStarting())
	{
		return;
	}

	// 只是不再提交。在飞的启动请求、目录请求照样会回来，回来时对不上代数 / 状态，被忽略。
	AutoStartState = EAutoStartState::Idle;
	++AutoStartGeneration;
	OnNotice.Broadcast(LOCTEXT("AutoStartCanceled", "Canceled. Nothing more will be submitted, and the backend is not stopped."), false);
	OnFormChanged.Broadcast();
}

void FDreamTextureGenerateModel::ApplySubmitResult(uint64 Generation, bool bOK, TArray<FDreamTextureJob>&& Jobs, const FString& Error)
{
	if (Generation != SubmitGeneration)
	{
		return;
	}
	bSubmitting = false;

	if (!bOK)
	{
		SubmitError = FText::FromString(Error.IsEmpty() ? LOCTEXT("SubmitFailed", "The submission failed.").ToString() : Error);
		OnNotice.Broadcast(SubmitError, true);
		OnFormChanged.Broadcast();
		return;
	}

	SetBatch(Jobs.Num() > 0 ? Jobs[0].BatchId : FString(), Jobs);
	OnNotice.Broadcast(FText::Format(LOCTEXT("Submitted", "Submitted {0} variant(s)."), FText::AsNumber(Jobs.Num())), false);
	OnFormChanged.Broadcast();
}

//////////////////////////////////////////////////////////////////////////
// 本批

void FDreamTextureGenerateModel::RestoreLatestBatch()
{
	const TSharedPtr<FDreamTextureJobTracker> Tracker = Backend.GetTracker ? Backend.GetTracker() : nullptr;
	if (!Tracker.IsValid())
	{
		return;
	}
	const FString Latest = Tracker->GetLatestBatchId(FDreamTextureJobTracker::GetDefaultOrigin());
	if (!Latest.IsEmpty())
	{
		SetBatch(Latest, Tracker->GetBatch(Latest));

		// 面板关着的这段时间里事件照常推进了追踪器，但事件流本身会丢（后端给每个订阅者的缓冲只有 64 格），
		// 所以重新打开时对没结束的任务各问一次，补上可能错过的终态。结束了的不问。
		if (HasActiveJobs())
		{
			Tracker->Reconcile();
		}
	}
}

void FDreamTextureGenerateModel::SetBatch(const FString& InBatchId, const TArray<FDreamTextureJob>& Jobs)
{
	BatchId = InBatchId;
	Rows.Reset();
	int32 Number = 0;
	for (const FDreamTextureJob& Job : Jobs)
	{
		const FRowPtr Row = MakeShared<FRow>();
		Row->JobId = Job.Id;
		Row->Number = ++Number;
		Rows.Add(Row);
	}
	OnBatchChanged.Broadcast();
}

const FDreamTextureJob* FDreamTextureGenerateModel::FindJob(const FString& JobId) const
{
	const TSharedPtr<FDreamTextureJobTracker> Tracker = Backend.GetTracker ? Backend.GetTracker() : nullptr;
	return Tracker.IsValid() ? Tracker->Find(JobId) : nullptr;
}

bool FDreamTextureGenerateModel::HasActiveJobs() const
{
	for (const FRowPtr& Row : Rows)
	{
		const FDreamTextureJob* Job = FindJob(Row->JobId);
		if (Job != nullptr && !Job->IsTerminal())
		{
			return true;
		}
	}
	return false;
}

int32 FDreamTextureGenerateModel::GetNumFinished() const
{
	int32 Count = 0;
	for (const FRowPtr& Row : Rows)
	{
		const FDreamTextureJob* Job = FindJob(Row->JobId);
		if (Job != nullptr && Job->IsTerminal())
		{
			++Count;
		}
	}
	return Count;
}

FDreamTextureGenerateRowView FDreamTextureGenerateModel::GetRowView(const FRowPtr& Row) const
{
	if (!Row.IsValid())
	{
		return FDreamTextureGenerateRowView();
	}

	const TSharedPtr<FDreamTextureJobTracker> Tracker = Backend.GetTracker ? Backend.GetTracker() : nullptr;
	const FDreamTextureJob* Job = Tracker.IsValid() ? Tracker->Find(Row->JobId) : nullptr;
	const bool bCancelRequested = Tracker.IsValid() && Tracker->IsCancelRequested(Row->JobId);

	const FDreamTextureImportStatus* ImportStatus = nullptr;
	if (Job != nullptr && Job->Status == EDreamTextureJobStatus::Succeeded && !Job->MaterialId.IsEmpty() && Backend.FindImportStatus)
	{
		ImportStatus = Backend.FindImportStatus(Job->MaterialId);
	}
	return EvaluateRow(Row->Number, Job, bCancelRequested, ImportStatus);
}

FDreamTextureGenerateRowView FDreamTextureGenerateModel::EvaluateRow(int32 Number, const FDreamTextureJob* Job, bool bCancelRequested,
	const FDreamTextureImportStatus* ImportStatus)
{
	using EKind = FDreamTextureGenerateRowView::EKind;
	using EImport = FDreamTextureGenerateRowView::EImport;

	FDreamTextureGenerateRowView View;
	View.Title = FText::Format(LOCTEXT("RowTitle", "Variant {0}"), FText::AsNumber(Number));
	View.ImportButton = LOCTEXT("ImportBtn", "Import");

	if (Job == nullptr)
	{
		View.Kind = EKind::Unknown;
		View.Stage = LOCTEXT("RowUnknown", "Job status unknown");
		return View;
	}

	// 种子可能大到 2^53 量级，别走 FText::AsNumber 的本地化分组（会带逗号，复制出去没法用）。
	{
		TArray<FString> Parts;
		if (Job->bHasSeed)
		{
			Parts.Add(FText::Format(LOCTEXT("RowSeed", "Seed {0}"), FText::FromString(FString::Printf(TEXT("%lld"), Job->Seed))).ToString());
		}
		if (Job->Resolution > 0)
		{
			Parts.Add(FString::Printf(TEXT("%d²"), Job->Resolution));
		}
		View.Subtitle = FText::FromString(FString::Join(Parts, TEXT(" · ")));
	}

	// 这两个阶段没有采样进度可言：后端在等 ComfyUI 回来，或刚把活交出去。给假进度条是在骗人。
	const bool bNoRealProgress = Job->Stage == TEXT("等待 ComfyUI") || Job->Stage == TEXT("提交中");

	if (!Job->IsTerminal() && bCancelRequested)
	{
		View.Kind = EKind::Canceling;
		View.Stage = LOCTEXT("RowCanceling", "Canceling…");
		View.bShowProgress = true;
		return View;
	}

	switch (Job->Status)
	{
	case EDreamTextureJobStatus::Queued:
		View.Kind = EKind::Waiting;
		View.Stage = Job->Stage.IsEmpty() ? LOCTEXT("RowQueued", "Queued…") : FDreamTextureJob::LocalizeStage(Job->Stage);
		View.bCanCancel = true;
		break;

	case EDreamTextureJobStatus::Running:
		View.Kind = EKind::Running;
		View.Stage = Job->Stage.IsEmpty() ? LOCTEXT("RowRunning", "Generating…") : FDreamTextureJob::LocalizeStage(Job->Stage);
		View.bShowProgress = true;
		if (!bNoRealProgress)
		{
			View.Percent = FMath::Clamp(Job->Progress, 0.0f, 1.0f);
		}
		View.bCanCancel = true;
		break;

	case EDreamTextureJobStatus::Succeeded:
		View.Kind = EKind::Done;
		View.Stage = LOCTEXT("RowDone", "Done");
		View.bCanImport = !Job->MaterialId.IsEmpty();
		break;

	case EDreamTextureJobStatus::Failed:
		View.Kind = EKind::Failed;
		View.Stage = LOCTEXT("RowFailed", "Failed");
		View.Detail = Job->Error.IsEmpty() ? LOCTEXT("RowFailedNoReason", "The backend did not give a reason.") : FText::FromString(Job->Error);
		break;

	case EDreamTextureJobStatus::Canceled:
		View.Kind = EKind::Canceled;
		View.Stage = LOCTEXT("RowCanceled", "Canceled");
		View.Detail = FDreamTextureJob::IsPlainCancelReason(Job->Error) ? FText::GetEmpty() : FText::FromString(Job->Error);
		break;

	case EDreamTextureJobStatus::Unknown:
	default:
		View.Kind = EKind::Unknown;
		View.Stage = Job->Stage.IsEmpty() ? LOCTEXT("RowUnknownStatus", "Status unknown") : FDreamTextureJob::LocalizeStage(Job->Stage);
		break;
	}

	// 导入：只有完成的变体才谈得上。
	if (View.Kind == EKind::Done && ImportStatus != nullptr)
	{
		switch (ImportStatus->Stage)
		{
		case EDreamTextureImportStage::Queued:
			View.Import = EImport::Active;
			View.ImportText = LOCTEXT("ImportQueued", "Queued for import…");
			break;
		case EDreamTextureImportStage::FetchingManifest:
			View.Import = EImport::Active;
			View.ImportText = LOCTEXT("ImportManifest", "Reading manifest…");
			break;
		case EDreamTextureImportStage::Downloading:
			View.Import = EImport::Active;
			View.ImportText = ImportStatus->FilesTotal > 0
				? FText::Format(LOCTEXT("ImportDownloading", "Downloading {0}/{1}"),
					FText::AsNumber(ImportStatus->FilesDone), FText::AsNumber(ImportStatus->FilesTotal))
				: LOCTEXT("ImportDownloadingNoCount", "Downloading…");
			break;
		case EDreamTextureImportStage::Importing:
			View.Import = EImport::Active;
			View.ImportText = LOCTEXT("ImportWriting", "Writing to the project…");
			break;
		case EDreamTextureImportStage::Succeeded:
			if (ImportStatus->Result.bOK)
			{
				View.Import = EImport::Succeeded;
				View.ImportText = LOCTEXT("ImportOk", "Imported");
				View.ImportTooltip = FText::FromString(ImportStatus->Result.AssetFolder);
				View.ImportButton = LOCTEXT("ImportAgain", "Re-import");
				break;
			}
			// bOK 为假的「完成」（全部跳过之类）按失败画，原因在 Result.Error。
			[[fallthrough]];
		case EDreamTextureImportStage::Failed:
		default:
			{
				View.Import = EImport::Failed;
				View.ImportText = LOCTEXT("ImportFailed", "Import failed");
				const FString& Reason = ImportStatus->Stage == EDreamTextureImportStage::Failed
					? (ImportStatus->Message.IsEmpty() ? ImportStatus->Result.Error : ImportStatus->Message)
					: (ImportStatus->Result.Error.IsEmpty() ? ImportStatus->Message : ImportStatus->Result.Error);
				View.ImportTooltip = FText::FromString(Reason);
				View.ImportButton = LOCTEXT("ImportRetry", "Retry");
			}
			break;
		}

		// 导入进行中不让再点，防止重复排。
		if (View.Import == EImport::Active)
		{
			View.bCanImport = false;
		}
	}
	return View;
}

void FDreamTextureGenerateModel::CancelJob(const FString& JobId)
{
	const TSharedPtr<FDreamTextureJobTracker> Tracker = Backend.GetTracker ? Backend.GetTracker() : nullptr;
	if (!Tracker.IsValid())
	{
		return;
	}
	const FDreamTextureJob* Job = Tracker->Find(JobId);
	if (Job == nullptr || Job->IsTerminal() || Tracker->IsCancelRequested(JobId))
	{
		return;
	}

	TWeakPtr<FDreamTextureGenerateModel> WeakSelf = AsShared();
	Tracker->Cancel(JobId,
		[WeakSelf](bool bOK, const FString& Error)
		{
			if (bOK)
			{
				return;
			}
			if (const TSharedPtr<FDreamTextureGenerateModel> Self = WeakSelf.Pin())
			{
				Self->OnNotice.Broadcast(FText::FromString(Error.IsEmpty() ? LOCTEXT("CancelJobFailed", "Could not cancel the job.").ToString() : Error), true);
			}
		});
}

void FDreamTextureGenerateModel::CancelBatch()
{
	for (const FRowPtr& Row : Rows)
	{
		CancelJob(Row->JobId);
	}
}

void FDreamTextureGenerateModel::SyncJobs()
{
	if (const TSharedPtr<FDreamTextureJobTracker> Tracker = Backend.GetTracker ? Backend.GetTracker() : nullptr)
	{
		Tracker->Reconcile();
	}
}

void FDreamTextureGenerateModel::RequestImport(const FString& JobId)
{
	const FDreamTextureJob* Job = FindJob(JobId);
	if (Job == nullptr || Job->Status != EDreamTextureJobStatus::Succeeded || Job->MaterialId.IsEmpty() || !Backend.Enqueue)
	{
		return;
	}

	if (!Backend.Enqueue(Job->MaterialId))
	{
		// 契约：同一个 id 已在队列里或正在跑时返回 false。不是错误，说一声就行。
		OnNotice.Broadcast(LOCTEXT("AlreadyQueued", "This material set is already in the import queue."), false);
	}
}

bool FDreamTextureGenerateModel::GetAutoImport() const
{
	return Backend.GetAutoImport ? Backend.GetAutoImport() : false;
}

void FDreamTextureGenerateModel::SetAutoImport(bool bInAuto)
{
	if (Backend.SetAutoImport)
	{
		Backend.SetAutoImport(bInAuto);
	}
}

//////////////////////////////////////////////////////////////////////////
// 扩写

void FDreamTextureGenerateModel::LoadRefineModels()
{
	if (!Backend.ListRefineModels || bRefineModelsLoading)
	{
		return;
	}
	bRefineModelsLoading = true;

	TWeakPtr<FDreamTextureGenerateModel> WeakSelf = AsShared();
	Backend.ListRefineModels(
		[WeakSelf](bool bOK, FDreamTextureRefineModels&& Models, const FString& Error)
		{
			const TSharedPtr<FDreamTextureGenerateModel> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			Self->bRefineModelsLoading = false;
			if (bOK)
			{
				Self->bRefineModelsLoaded = true;
				Self->RefineModels = MoveTemp(Models.Models);
				Self->RefineDefault = MoveTemp(Models.Default);

				// 列不出来不算错：网关不提供 /models、令牌没有列举权限都很常见。
				// 这时只是没有下拉可选，用后端的默认模型照样能扩写。
				Self->RefineModelsError = MoveTemp(Models.Error);
			}
			else
			{
				Self->RefineModelsError = Error;
			}
			Self->OnFormChanged.Broadcast();
		});
}

void FDreamTextureGenerateModel::Refine()
{
	if (RefineState == ERefineState::Working)
	{
		return;
	}

	const FString Trimmed = Prompt.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		OnNotice.Broadcast(LOCTEXT("RefineNeedPrompt", "Write a prompt first, then use Refine prompt."), false);
		return;
	}
	if (!Backend.Refine)
	{
		return;
	}

	RefineState = ERefineState::Working;
	RefineError.Reset();
	const uint64 Gen = ++RefineGeneration;
	OnRefineChanged.Broadcast();

	TWeakPtr<FDreamTextureGenerateModel> WeakSelf = AsShared();
	// 材质的提示词要带上正交平光可平铺那套硬约束，所以 purpose 是 texture；
	// 普通图片不带——这里只出材质。
	Backend.Refine(Trimmed, EDreamTextureRefinePurpose::Texture, RefineModel,
		[WeakSelf, Gen](bool bOK, FDreamTextureRefineResult&& Result, const FString& Error)
		{
			const TSharedPtr<FDreamTextureGenerateModel> Self = WeakSelf.Pin();
			if (!Self.IsValid() || Gen != Self->RefineGeneration)
			{
				return;
			}

			if (bOK)
			{
				// 只摆出来，不直接盖掉输入框：模型有时会自作主张加东西，
				// 直接替换会让人莫名其妙地丢掉自己写的要求。
				Self->RefinedPrompt = MoveTemp(Result.Prompt);
				Self->RefinedModel = MoveTemp(Result.Model);
				Self->RefineState = ERefineState::HasResult;
			}
			else
			{
				Self->RefineState = ERefineState::Idle;
				Self->RefineError = Error.IsEmpty() ? LOCTEXT("RefineFailed", "Could not refine the prompt.").ToString() : Error;
				Self->OnNotice.Broadcast(FText::FromString(Self->RefineError), true);
			}
			Self->OnRefineChanged.Broadcast();
		});
}

void FDreamTextureGenerateModel::AcceptRefine()
{
	if (RefineState != ERefineState::HasResult)
	{
		return;
	}
	Prompt = RefinedPrompt;
	RefinedPrompt.Reset();
	RefinedModel.Reset();
	RefineState = ERefineState::Idle;
	OnPromptReplaced.Broadcast();
	OnRefineChanged.Broadcast();
	OnFormChanged.Broadcast();
}

void FDreamTextureGenerateModel::DiscardRefine()
{
	// 丢弃也作废在飞的那次，回来的结果不该再冒出来。
	++RefineGeneration;
	RefinedPrompt.Reset();
	RefinedModel.Reset();
	RefineState = ERefineState::Idle;
	OnRefineChanged.Broadcast();
}

//////////////////////////////////////////////////////////////////////////
// 自动重试

void FDreamTextureGenerateModel::HandleLinkChanged(EDreamTextureLink NewLink)
{
	if (NewLink != EDreamTextureLink::Live)
	{
		return;
	}

	// 后端刚起来、或断线重连后：之前没拉到的补上。已经拉到的不重拉，目录很少变。
	if (CatalogState == ECatalogState::NotLoaded || CatalogState == ECatalogState::Failed)
	{
		LoadCatalog();
	}
	if (!bRefineModelsLoaded)
	{
		LoadRefineModels();
	}
	MaybeLoadCloudModels();
}

#undef LOCTEXT_NAMESPACE
