// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureReflowRun.h"

#include "DreamTextureClientModule.h"
#include "DreamTextureLog.h"
#include "DreamTextureProcess.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "DreamTextureReflowRun"

namespace
{
	/**
	 * 后端和各层给的原因常常自带句末标点（Go 的错误文本是英文句号），我们的句子接在它后面再加「。」就成了「it.。」。
	 * 去掉结尾的空白和句号，由我们的句子自己收尾。
	 */
	FString ReflowStripTrailingPunctuation(FString Text)
	{
		Text.TrimStartAndEndInline();
		while (Text.Len() > 0 && (Text.EndsWith(TEXT(".")) || Text.EndsWith(TEXT("。")) || Text.EndsWith(TEXT(";")) || Text.EndsWith(TEXT("；"))))
		{
			Text.LeftChopInline(1);
			Text.TrimEndInline();
		}
		return Text;
	}

	/** 此刻靠 KeepAliveWhileRunning 活着的运行数。给自动化测试验证「结束就放手」。 */
	int32 GKeptAliveCount = 0;
}

FText DescribeReflowStep(EDreamTextureReflowStep Step)
{
	switch (Step)
	{
	case EDreamTextureReflowStep::Backend: return LOCTEXT("StepBackend", "Start backend");
	case EDreamTextureReflowStep::Export: return LOCTEXT("StepExport", "Export");
	case EDreamTextureReflowStep::Upload: return LOCTEXT("StepUpload", "Upload");
	case EDreamTextureReflowStep::Submit: return LOCTEXT("StepSubmit", "Submit");
	case EDreamTextureReflowStep::Generate: return LOCTEXT("StepGenerate", "Generate");
	case EDreamTextureReflowStep::Download: return LOCTEXT("StepDownload", "Download");
	case EDreamTextureReflowStep::Import: return LOCTEXT("StepImport", "Import");
	case EDreamTextureReflowStep::Apply: return LOCTEXT("StepApply", "Apply to instance");
	default: return LOCTEXT("StepNone", "Prepare");
	}
}

//////////////////////////////////////////////////////////////////////////
// 默认钩子

FDreamTextureReflowRunHooks FDreamTextureReflowRunHooks::MakeDefault()
{
	FDreamTextureReflowRunHooks Out;

	Out.EnsureBackend = [](TFunction<void(bool, const FString&)> Done)
	{
		if (!FDreamTextureClientModule::IsAvailable() || !FDreamTextureClientModule::Get().GetProcess().IsValid())
		{
			// 客户端模块没就绪（commandlet 之类）：没法托管后端。交给后面的上传去试，连不上它自己会说。
			if (Done)
			{
				Done(true, FString());
			}
			return;
		}
		FDreamTextureClientModule::Get().GetProcess()->EnsureBackend(MoveTemp(Done));
	};

	Out.Export = [](const FDreamTextureMaterialAnalysis& Analysis, const FDreamTextureExportOptions& Options)
	{
		return FDreamTextureTextureExporter::ExportAnalysis(Analysis, Options);
	};
	Out.MakeTempDir = []()
	{
		return FDreamTextureTextureExporter::MakeTempDir();
	};
	Out.Upload = [](const FString& FilePath, FDreamTextureUploadApi::FOnUploaded Done)
	{
		FDreamTextureUploadApi::UploadImage(FilePath, MoveTemp(Done));
	};
	Out.GetTracker = []() -> TSharedPtr<FDreamTextureJobTracker>
	{
		return FDreamTextureClientModule::IsAvailable() ? FDreamTextureClientModule::Get().GetJobTracker() : nullptr;
	};
	Out.Enqueue = [](const FString& MaterialId, const FDreamTextureImportOptions& Options)
	{
		return FDreamTextureImportQueue::Get().Enqueue(MaterialId, Options);
	};
	Out.GetManifest = [](const FString& MaterialId, FDreamTextureApi::FOnManifest Done)
	{
		FDreamTextureApi::GetManifest(MaterialId, MoveTemp(Done));
	};
	Out.ApplyInPlace = [](UMaterialInstanceConstant* Instance, const TArray<FDreamTextureReflowOverwrite>& Items, const FDreamTextureImportResult& Imported)
	{
		return FDreamTextureReflowPlanner::ApplyInPlace(Instance, Items, Imported, /*bSave*/ true);
	};

	Out.WatchJobs = [](TFunction<void(const FDreamTextureJob&)> Callback) -> TFunction<void()>
	{
		const TSharedPtr<FDreamTextureJobTracker> Tracker = FDreamTextureClientModule::IsAvailable() ? FDreamTextureClientModule::Get().GetJobTracker() : nullptr;
		if (!Tracker.IsValid())
		{
			return []() {};
		}
		const FDelegateHandle Handle = Tracker->OnJobChanged.AddLambda([Callback = MoveTemp(Callback)](const FDreamTextureJob& Job)
		{
			Callback(Job);
		});
		const TWeakPtr<FDreamTextureJobTracker> Weak = Tracker;
		return [Weak, Handle]()
		{
			if (const TSharedPtr<FDreamTextureJobTracker> Pinned = Weak.Pin())
			{
				Pinned->OnJobChanged.Remove(Handle);
			}
		};
	};

	Out.WatchImports = [](TFunction<void(const FDreamTextureImportStatus&)> Callback) -> TFunction<void()>
	{
		const FDelegateHandle Handle = FDreamTextureImportQueue::Get().OnStatusChanged.AddLambda([Callback = MoveTemp(Callback)](const FDreamTextureImportStatus& Status)
		{
			Callback(Status);
		});
		return [Handle]()
		{
			FDreamTextureImportQueue::Get().OnStatusChanged.Remove(Handle);
		};
	};
	return Out;
}

//////////////////////////////////////////////////////////////////////////
// 构造与状态

FDreamTextureReflowRun::FDreamTextureReflowRun(FDreamTextureReflowRunSpec InSpec, FDreamTextureReflowRunHooks InHooks)
	: Spec(MoveTemp(InSpec))
	, Hooks(MoveTemp(InHooks))
{
}

int32 FDreamTextureReflowRun::GetNumKeptAlive()
{
	return GKeptAliveCount;
}

FDreamTextureReflowRun::~FDreamTextureReflowRun()
{
	if (UnwatchJobs)
	{
		UnwatchJobs();
	}
	if (UnwatchImports)
	{
		UnwatchImports();
	}
}

void FDreamTextureReflowRun::KeepAliveWhileRunning()
{
	bKeepAlive = true;
}

void FDreamTextureReflowRun::SetStep(EDreamTextureReflowStep NewStep, const FText& NewMessage, TOptional<float> NewProgress)
{
	Step = NewStep;
	Message = NewMessage;
	Progress = NewProgress;
	OnChanged.Broadcast();
}

void FDreamTextureReflowRun::Release()
{
	if (UnwatchJobs)
	{
		UnwatchJobs();
		UnwatchJobs = nullptr;
	}
	if (UnwatchImports)
	{
		UnwatchImports();
		UnwatchImports = nullptr;
	}
	if (SelfKeepAlive.IsValid())
	{
		--GKeptAliveCount;
		// 先挪到局部再放手：成员是最后一个强引用时，直接赋空会在这个函数还没返回时就把自己析构了。
		const TSharedPtr<FDreamTextureReflowRun> Last = MoveTemp(SelfKeepAlive);
	}
}

void FDreamTextureReflowRun::Fail(EDreamTextureReflowStep FailedStep, const FString& Reason)
{
	if (IsFinished())
	{
		return;
	}
	State = EState::Failed;
	Step = FailedStep;
	Progress.Reset();

	FString Why = ReflowStripTrailingPunctuation(Reason);
	// 各层给的原因常常自带「上传失败：」「提交失败：」这样的前缀，而我们的句子已经说了「在「上传」这一步失败」，别叠一遍。
	for (const TCHAR* Prefix : { TEXT("Upload failed: "), TEXT("Could not submit the job: "), TEXT("上传失败："), TEXT("提交失败：") })
	{
		if (Why.StartsWith(Prefix))
		{
			Why = Why.Mid(FCString::Strlen(Prefix));
			break;
		}
	}
	Message = FText::Format(LOCTEXT("Failed", "Failed at the \"{0}\" step: {1}."), DescribeReflowStep(FailedStep),
		FText::FromString(Why.IsEmpty() ? LOCTEXT("NoReason", "No reason was given").ToString() : Why));
	UE_LOG(LogDreamTexture, Warning, TEXT("Reflow (%s) failed at the \"%s\" step: %s"), *Spec.Title, *DescribeReflowStep(FailedStep).ToString(), *Why);

	const TSharedPtr<FDreamTextureReflowRun> Keep = AsShared();
	Release();
	OnChanged.Broadcast();
}

void FDreamTextureReflowRun::Finish(const FText& Summary)
{
	if (IsFinished())
	{
		return;
	}
	State = EState::Succeeded;
	Message = Summary;
	Progress = 1.0f;

	const TSharedPtr<FDreamTextureReflowRun> Keep = AsShared();
	Release();
	OnChanged.Broadcast();
}

bool FDreamTextureReflowRun::StopIfCancelRequested()
{
	if (!bCancelRequested || IsFinished())
	{
		return false;
	}
	// 停在取消发生时正在做的那一步（Step 还没切到下一步）。
	Fail(Step, LOCTEXT("Canceled", "Canceled").ToString());
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 开始与取消

void FDreamTextureReflowRun::Start()
{
	if (State != EState::Idle)
	{
		return;
	}
	State = EState::Running;

	// 硬上限在最低一层再拦一次：UV 展开的贴图改写外观时，重绘幅度超过上限会破坏 UV 对应关系，贴回模型就全乱了。
	// 页面上（CanRun）已经拦过；这里是兜底——别的入口、以后的自动化直接构造运行时也绕不过去。
	if (Spec.Mode == EDreamTextureReflowMode::Restyle)
	{
		if (const double* Denoise = Spec.Request.FloatParams.Find(TEXT("denoise")))
		{
			if (!DreamTextureReflow::IsDenoiseAllowed(Spec.UVKind, *Denoise))
			{
				Fail(EDreamTextureReflowStep::None, DreamTextureReflow::DescribeDenoiseLimit(*Denoise));
				return;
			}
		}
	}

	if (bKeepAlive)
	{
		SelfKeepAlive = AsShared();
		++GKeptAliveCount;
	}

	SetStep(EDreamTextureReflowStep::Backend, LOCTEXT("Starting", "Checking that the backend is running…"));

	if (!Hooks.EnsureBackend)
	{
		DoExport();
		return;
	}

	TWeakPtr<FDreamTextureReflowRun> WeakSelf = AsShared();
	Hooks.EnsureBackend([WeakSelf](bool bOK, const FString& Error)
	{
		const TSharedPtr<FDreamTextureReflowRun> Self = WeakSelf.Pin();
		if (!Self.IsValid() || Self->IsFinished())
		{
			return;
		}
		if (!bOK)
		{
			Self->Fail(EDreamTextureReflowStep::Backend, Error.IsEmpty() ? LOCTEXT("BackendWontStart", "The backend could not be started").ToString() : Error);
			return;
		}
		if (Self->StopIfCancelRequested())
		{
			return;
		}
		Self->DoExport();
	});
}

void FDreamTextureReflowRun::Cancel()
{
	if (State != EState::Running)
	{
		return;
	}
	bCancelRequested = true;

	// 已经提交了：打断后端的任务，等 job.failed(canceled) 回来收尾。
	if (!JobId.IsEmpty() && Step == EDreamTextureReflowStep::Generate)
	{
		const TSharedPtr<FDreamTextureJobTracker> Tracker = Hooks.GetTracker ? Hooks.GetTracker() : nullptr;
		if (Tracker.IsValid())
		{
			Message = LOCTEXT("Canceling", "Canceling…");
			Progress.Reset();
			OnChanged.Broadcast();

			TWeakPtr<FDreamTextureReflowRun> WeakSelf = AsShared();
			Tracker->Cancel(JobId, [WeakSelf](bool bOK, const FString& Error)
			{
				const TSharedPtr<FDreamTextureReflowRun> Self = WeakSelf.Pin();
				if (Self.IsValid() && !bOK && !Self->IsFinished())
				{
					// 取消请求本身没成（任务可能刚好结束了）：任务到了终态事件会自己收尾，这里只告诉用户。
					Self->Message = FText::Format(LOCTEXT("CancelFailed", "The cancel request did not succeed: {0}"), FText::FromString(Error));
					Self->bCancelRequested = false;
					Self->OnChanged.Broadcast();
				}
			});
			return;
		}
	}

	// 其余步骤都是一瞬间的事：做个记号，下一步开始前发现了就停下。导入已经交给了导入队列，停不下来，
	// 让它落完——落下来的是一套完整的新贴图，不会留半截。
	Message = LOCTEXT("CancelPending", "Canceling…");
	OnChanged.Broadcast();
}

//////////////////////////////////////////////////////////////////////////
// 导出 → 上传 → 提交

void FDreamTextureReflowRun::DoExport()
{
	if (StopIfCancelRequested())
	{
		return;
	}
	SetStep(EDreamTextureReflowStep::Export, FText::Format(LOCTEXT("Exporting", "Exporting the Base Color texture {0}…"), FText::FromString(Spec.SourceTextureName)));

	if (!Hooks.Export || !Hooks.MakeTempDir)
	{
		Fail(EDreamTextureReflowStep::Export, LOCTEXT("ExportNotReady", "The export function is not ready").ToString());
		return;
	}

	FDreamTextureExportOptions Options;
	Options.OutputDir = Hooks.MakeTempDir();
	Options.MaxEdge = Spec.ExportMaxEdge;

	const TArray<FDreamTextureExportedImage> Results = Hooks.Export(Spec.ExportAnalysis, Options);
	const FDreamTextureExportedImage* Mine = Results.FindByPredicate([this](const FDreamTextureExportedImage& Image)
	{
		return Image.TexturePath.Equals(Spec.SourceTexturePath, ESearchCase::CaseSensitive);
	});
	if (Mine == nullptr)
	{
		Fail(EDreamTextureReflowStep::Export, FText::Format(LOCTEXT("ExportNoResult", "The export returned no result for {0}"), FText::FromString(Spec.SourceTextureName)).ToString());
		return;
	}
	if (!Mine->bExported)
	{
		Fail(EDreamTextureReflowStep::Export, FText::Format(LOCTEXT("ExportCannot", "{0} cannot be exported: {1}"), FText::FromString(Spec.SourceTextureName), FText::FromString(Mine->Reason)).ToString());
		return;
	}
	if (Mine->Sha256.IsEmpty() || !FDreamTextureDerivedFrom::IsValidFingerprint(Mine->Sha256))
	{
		Fail(EDreamTextureReflowStep::Export, FText::Format(LOCTEXT("ExportNoFingerprint", "The exported file has no valid fingerprint ({0})"), FText::FromString(Mine->Sha256)).ToString());
		return;
	}

	ExportedFile = Mine->FilePath;
	ExportedSha = Mine->Sha256;
	DoUpload();
}

void FDreamTextureReflowRun::DoUpload()
{
	if (StopIfCancelRequested())
	{
		return;
	}
	SetStep(EDreamTextureReflowStep::Upload, LOCTEXT("Uploading", "Uploading to the backend…"));

	if (!Hooks.Upload)
	{
		Fail(EDreamTextureReflowStep::Upload, LOCTEXT("UploadNotReady", "The upload function is not ready").ToString());
		return;
	}

	TWeakPtr<FDreamTextureReflowRun> WeakSelf = AsShared();
	Hooks.Upload(ExportedFile, [WeakSelf](bool bOK, FString&& StoredName, const FString& Error)
	{
		const TSharedPtr<FDreamTextureReflowRun> Self = WeakSelf.Pin();
		if (!Self.IsValid() || Self->IsFinished())
		{
			return;
		}
		if (!bOK)
		{
			Self->Fail(EDreamTextureReflowStep::Upload, Error);
			return;
		}
		Self->UploadedName = MoveTemp(StoredName);
		Self->DoSubmit();
	});
}

void FDreamTextureReflowRun::DoSubmit()
{
	if (StopIfCancelRequested())
	{
		return;
	}
	SetStep(EDreamTextureReflowStep::Submit, LOCTEXT("Submitting", "Submitting the generation request…"));

	const TSharedPtr<FDreamTextureJobTracker> Tracker = Hooks.GetTracker ? Hooks.GetTracker() : nullptr;
	if (!Tracker.IsValid())
	{
		Fail(EDreamTextureReflowStep::Submit, LOCTEXT("NoTracker", "The job tracker is not ready (the client module is not loaded)").ToString());
		return;
	}

	// 请求里运行中才知道的两样：上传后后端起的文件名，和送上去的那份文件的指纹。
	Submitted = Spec.Request;
	Submitted.StringParams.Add(Spec.UploadParamKey, UploadedName);
	Submitted.DerivedFrom.MapsIn.Add(DreamTextureChannel::BaseColor, ExportedSha);

	// 先订阅再提交：提交应答回来之前，job.progress 事件可能已经到了。
	if (Hooks.WatchJobs && !UnwatchJobs)
	{
		TWeakPtr<FDreamTextureReflowRun> WeakSelf = AsShared();
		UnwatchJobs = Hooks.WatchJobs([WeakSelf](const FDreamTextureJob& Job)
		{
			if (const TSharedPtr<FDreamTextureReflowRun> Self = WeakSelf.Pin())
			{
				Self->HandleJob(Job);
			}
		});
	}

	TWeakPtr<FDreamTextureReflowRun> WeakSelf = AsShared();
	Tracker->Submit(Submitted,
		[WeakSelf](bool bOK, TArray<FDreamTextureJob>&& Jobs, const FString& Error)
		{
			const TSharedPtr<FDreamTextureReflowRun> Self = WeakSelf.Pin();
			if (!Self.IsValid() || Self->IsFinished())
			{
				return;
			}
			if (!bOK || Jobs.Num() == 0)
			{
				Self->Fail(EDreamTextureReflowStep::Submit, Error.IsEmpty() ? LOCTEXT("NotAccepted", "The backend did not accept the request").ToString() : Error);
				return;
			}

			Self->JobId = Jobs[0].Id;
			Self->MaterialId = Jobs[0].MaterialId;
			Self->SetStep(EDreamTextureReflowStep::Generate, LOCTEXT("Queued", "Queued…"));

			// 取消可能发生在提交途中：现在有任务 id 了，补一次真正的取消。
			if (Self->bCancelRequested)
			{
				Self->Cancel();
			}

			// 应答回来之前任务可能已经推进过：以追踪器里的现状为准。
			if (const TSharedPtr<FDreamTextureJobTracker> Tracker = Self->Hooks.GetTracker ? Self->Hooks.GetTracker() : nullptr)
			{
				if (const FDreamTextureJob* Current = Tracker->Find(Self->JobId))
				{
					const FDreamTextureJob Copy = *Current;
					Self->HandleJob(Copy);
				}
			}
		},
		GetOrigin());
}

//////////////////////////////////////////////////////////////////////////
// 生成

void FDreamTextureReflowRun::HandleJob(const FDreamTextureJob& Job)
{
	if (State != EState::Running || JobId.IsEmpty() || Job.Id != JobId)
	{
		return;
	}
	if (Step != EDreamTextureReflowStep::Generate)
	{
		return;
	}

	switch (Job.Status)
	{
	case EDreamTextureJobStatus::Succeeded:
		if (!Job.MaterialId.IsEmpty())
		{
			MaterialId = Job.MaterialId;
		}
		DoImport();
		return;

	case EDreamTextureJobStatus::Failed:
		Fail(EDreamTextureReflowStep::Generate, Job.Error.IsEmpty() ? LOCTEXT("BackendNoReason", "The backend did not give a reason").ToString() : Job.Error);
		return;

	case EDreamTextureJobStatus::Canceled:
		Fail(EDreamTextureReflowStep::Generate, FDreamTextureJob::IsPlainCancelReason(Job.Error) ? LOCTEXT("Canceled", "Canceled").ToString() : Job.Error);
		return;

	default:
		break;
	}

	// 这两个阶段没有采样进度可言：后端在等 ComfyUI 回来，或刚把活交出去。不画假进度条。
	const bool bNoRealProgress = Job.Stage == TEXT("等待 ComfyUI") || Job.Stage == TEXT("提交中");
	const FString Stage = Job.Stage.IsEmpty()
		? (Job.Status == EDreamTextureJobStatus::Queued ? LOCTEXT("StageQueued", "Queued").ToString() : LOCTEXT("StageRunning", "Generating").ToString())
		: FDreamTextureJob::LocalizeStage(Job.Stage).ToString();

	if (bCancelRequested)
	{
		SetStep(EDreamTextureReflowStep::Generate, LOCTEXT("CancelingJob", "Canceling…"));
		return;
	}

	if (Job.Status == EDreamTextureJobStatus::Running && !bNoRealProgress)
	{
		SetStep(EDreamTextureReflowStep::Generate,
			FText::Format(LOCTEXT("GeneratingPercent", "{0} {1}%"), FText::FromString(Stage), FText::AsNumber(FMath::RoundToInt(Job.Progress * 100.0f))),
			FMath::Clamp(Job.Progress, 0.0f, 1.0f));
	}
	else
	{
		SetStep(EDreamTextureReflowStep::Generate, FText::FromString(Stage + TEXT("…")));
	}
}

//////////////////////////////////////////////////////////////////////////
// 下载与导入

void FDreamTextureReflowRun::DoImport()
{
	if (MaterialId.IsEmpty())
	{
		Fail(EDreamTextureReflowStep::Generate, LOCTEXT("NoMaterialId", "The backend reported the job as finished but gave no material id").ToString());
		return;
	}

	if (Hooks.WatchJobs && UnwatchJobs)
	{
		UnwatchJobs();
		UnwatchJobs = nullptr;
	}

	LastImportStage = EDreamTextureImportStage::Queued;
	SetStep(EDreamTextureReflowStep::Download, LOCTEXT("ImportQueued", "Generation finished; queued for import…"));

	if (Hooks.WatchImports && !UnwatchImports)
	{
		TWeakPtr<FDreamTextureReflowRun> WeakSelf = AsShared();
		UnwatchImports = Hooks.WatchImports([WeakSelf](const FDreamTextureImportStatus& Status)
		{
			if (const TSharedPtr<FDreamTextureReflowRun> Self = WeakSelf.Pin())
			{
				Self->HandleImport(Status);
			}
		});
	}

	if (!Hooks.Enqueue || !Hooks.Enqueue(MaterialId, Spec.ImportOptions))
	{
		Fail(EDreamTextureReflowStep::Download, LOCTEXT("AlreadyQueued", "This material set is already in the import queue, so Reflow cannot queue it a second time").ToString());
	}
}

void FDreamTextureReflowRun::HandleImport(const FDreamTextureImportStatus& Status)
{
	if (State != EState::Running || MaterialId.IsEmpty() || !Status.MaterialId.Equals(MaterialId, ESearchCase::CaseSensitive))
	{
		return;
	}
	if (Step != EDreamTextureReflowStep::Download && Step != EDreamTextureReflowStep::Import)
	{
		return;
	}

	switch (Status.Stage)
	{
	case EDreamTextureImportStage::Queued:
	case EDreamTextureImportStage::FetchingManifest:
		LastImportStage = Status.Stage;
		SetStep(EDreamTextureReflowStep::Download, FText::FromString(Status.Message));
		return;

	case EDreamTextureImportStage::Downloading:
		LastImportStage = Status.Stage;
		SetStep(EDreamTextureReflowStep::Download, FText::FromString(Status.Message),
			Status.FilesTotal > 0 ? TOptional<float>(static_cast<float>(Status.FilesDone) / static_cast<float>(Status.FilesTotal)) : TOptional<float>());
		return;

	case EDreamTextureImportStage::Importing:
		LastImportStage = Status.Stage;
		SetStep(EDreamTextureReflowStep::Import, FText::FromString(Status.Message));
		return;

	case EDreamTextureImportStage::Failed:
		{
			// 导入队列失败时只留一句话，不留是在哪个阶段：拿失败前最后看到的阶段判断。
			// 拿 manifest 和下载是「下载」这一步；已经走到落资产的是「导入」这一步。
			const bool bAtImport = LastImportStage == EDreamTextureImportStage::Importing;
			Fail(bAtImport ? EDreamTextureReflowStep::Import : EDreamTextureReflowStep::Download, Status.Message);
			return;
		}

	case EDreamTextureImportStage::Succeeded:
		ImportResult = Status.Result;
		if (Spec.bInPlace)
		{
			DoApply();
			return;
		}
		{
			const FDreamTextureImportedTexture& MI = ImportResult.MaterialInstance;
			if (MI.Action == EDreamTextureImportAction::Created || MI.Action == EDreamTextureImportAction::Updated || MI.Action == EDreamTextureImportAction::Unchanged)
			{
				ResultInstancePath = MI.ObjectPath;
				int32 NumTextures = 0;
				for (const FDreamTextureImportedTexture& Texture : ImportResult.Textures)
				{
					NumTextures += (Texture.Action == EDreamTextureImportAction::Created || Texture.Action == EDreamTextureImportAction::Updated) ? 1 : 0;
				}

				FString Summary = FText::Format(LOCTEXT("SummaryCreated", "Created material instance {0} ({1} new texture(s)); the original material was not touched."), FText::FromString(FPaths::GetBaseFilename(MI.ObjectPath)), FText::AsNumber(NumTextures)).ToString();
				if (Spec.Mode == EDreamTextureReflowMode::FillChannels && Spec.ImportOptions.ReuseTextures.Contains(DreamTextureChannel::BaseColor))
				{
					Summary += TEXT(" ") + LOCTEXT("SummaryBaseKept", "Its Base Color points at your original texture.").ToString();
				}
				if (!MI.Note.IsEmpty())
				{
					Summary += TEXT("\n") + MI.Note;
				}
				for (const FString& Note : Spec.PlanNotes)
				{
					Summary += TEXT("\n") + Note;
				}
				FetchManifestForPreview();
				Finish(FText::FromString(Summary));
			}
			else
			{
				// 贴图落进来了，实例没建出来：用户要的是一个能用的材质，所以这算「导入」这一步失败。
				const FString Why = MI.Note.IsEmpty() ? LOCTEXT("InstanceNotCreated", "The material instance was not created").ToString() : MI.Note;
				Fail(EDreamTextureReflowStep::Import, Why);
			}
		}
		return;
	}
}

//////////////////////////////////////////////////////////////////////////
// 原地覆盖

void FDreamTextureReflowRun::DoApply()
{
	SetStep(EDreamTextureReflowStep::Apply, LOCTEXT("Applying", "Applying to the selected material instance…"));

	UMaterialInstanceConstant* Target = Spec.InPlaceTarget.Get();
	if (Target == nullptr)
	{
		Fail(EDreamTextureReflowStep::Apply, LOCTEXT("InstanceGone", "The material instance to change no longer exists (it was unloaded or deleted). The new textures were imported into the project, so you can swap them in by hand").ToString());
		return;
	}
	if (!Hooks.ApplyInPlace)
	{
		Fail(EDreamTextureReflowStep::Apply, LOCTEXT("OverwriteNotReady", "The overwrite-in-place function is not ready").ToString());
		return;
	}

	ApplyResult = Hooks.ApplyInPlace(Target, Spec.Overwrites, ImportResult);
	if (!ApplyResult.bOK)
	{
		Fail(EDreamTextureReflowStep::Apply, ApplyResult.Error);
		return;
	}
	if (ApplyResult.Changed.Num() == 0)
	{
		Fail(EDreamTextureReflowStep::Apply, ApplyResult.Skipped.Num() > 0
			? FText::Format(LOCTEXT("NothingChangedWhy", "No parameter was changed: {0}"), FText::FromString(ApplyResult.Skipped[0])).ToString()
			: LOCTEXT("NothingChanged", "No parameter was changed").ToString());
		return;
	}

	ResultInstancePath = Target->GetPathName();
	FString Summary = FText::Format(LOCTEXT("SummaryOverwritten", "Updated {0} ({1} change(s)): {2}. You can undo it with Ctrl+Z."),
		FText::FromString(Target->GetName()), FText::AsNumber(ApplyResult.Changed.Num()),
		FText::FromString(FString::Join(ApplyResult.Changed, *LOCTEXT("ListSeparator", ", ").ToString()))).ToString();
	for (const FString& Skipped : ApplyResult.Skipped)
	{
		Summary += TEXT("\n") + FText::Format(LOCTEXT("SummaryLeftAlone", "Left unchanged: {0}"), FText::FromString(Skipped)).ToString();
	}
	for (const FString& Note : Spec.PlanNotes)
	{
		Summary += TEXT("\n") + Note;
	}
	FetchManifestForPreview();
	Finish(FText::FromString(Summary));
}

//////////////////////////////////////////////////////////////////////////
// 预览要的 manifest

void FDreamTextureReflowRun::FetchManifestForPreview()
{
	if (!Hooks.GetManifest || MaterialId.IsEmpty())
	{
		return;
	}
	TWeakPtr<FDreamTextureReflowRun> WeakSelf = AsShared();
	Hooks.GetManifest(MaterialId, [WeakSelf](bool bOK, FDreamTextureManifest&& InManifest, const FString&)
	{
		// 拉不到只是没有预览，不影响这次运行的结论。
		const TSharedPtr<FDreamTextureReflowRun> Self = WeakSelf.Pin();
		if (Self.IsValid() && bOK)
		{
			Self->Manifest = MoveTemp(InManifest);
			Self->bHasManifest = true;
			Self->OnChanged.Broadcast();
		}
	});
}

#undef LOCTEXT_NAMESPACE
