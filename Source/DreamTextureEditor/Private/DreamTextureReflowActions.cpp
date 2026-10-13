// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureReflowActions.h"

#include "DreamTextureClientModule.h"
#include "DreamTextureConnection.h"
#include "DreamTextureGenerateModel.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "DreamTextureReflowActions"

namespace
{
	/** 后端出图段里 denoise 的范围（workflows/src-local-*.params.json）。声明里读不到时用这个。 */
	constexpr double DenoiseFallbackMax = 1.0;

	/** 「MI_Rock」→「Rock」：新材质实例叫 MI_<名字>_<id>，名字里再带 MI_ 就成了 MI_MIRock。 */
	FString StripAssetPrefix(const FString& Name)
	{
		for (const TCHAR* Prefix : { TEXT("MI_"), TEXT("M_") })
		{
			if (Name.StartsWith(Prefix, ESearchCase::CaseSensitive) && Name.Len() > FCString::Strlen(Prefix))
			{
				return Name.Mid(FCString::Strlen(Prefix));
			}
		}
		return Name;
	}

	TArray<const FDreamTextureWorkflowMeta*> SortedById(TArray<const FDreamTextureWorkflowMeta*> Items)
	{
		Items.Sort([](const FDreamTextureWorkflowMeta& A, const FDreamTextureWorkflowMeta& B) { return A.Id < B.Id; });
		return Items;
	}

	bool IsMaterialPipeline(const FDreamTextureWorkflowMeta& Meta)
	{
		return !Meta.Kind.Equals(TEXT("image"), ESearchCase::IgnoreCase);
	}
}

//////////////////////////////////////////////////////////////////////////
// 默认后端

FDreamTextureReflowActionsBackend FDreamTextureReflowActionsBackend::MakeDefault()
{
	FDreamTextureReflowActionsBackend Out;
	Out.ListWorkflows = [](FDreamTextureGenerateApi::FOnCatalog Done)
	{
		FDreamTextureGenerateApi::ListWorkflows(MoveTemp(Done));
	};
	Out.GetConnection = []() -> TSharedPtr<FDreamTextureConnection>
	{
		return FDreamTextureClientModule::IsAvailable() ? FDreamTextureClientModule::Get().GetConnection() : nullptr;
	};
	Out.GetEngineVersion = []()
	{
		return FEngineVersion::Current().ToString(EVersionComponent::Patch);
	};
	Out.MakeRun = [](FDreamTextureReflowRunSpec&& Spec)
	{
		const TSharedRef<FDreamTextureReflowRun> Run = MakeShared<FDreamTextureReflowRun>(MoveTemp(Spec));
		// 一次生成几十秒到几分钟，用户常常点完就关面板：运行自己活着，导入照常落。
		Run->KeepAliveWhileRunning();
		return Run;
	};
	return Out;
}

//////////////////////////////////////////////////////////////////////////
// 构造与订阅

FDreamTextureReflowActions::FDreamTextureReflowActions(TSharedRef<FDreamTextureReflowModel> InModel, FDreamTextureReflowActionsBackend InBackend)
	: Model(InModel)
	, Backend(MoveTemp(InBackend))
{
}

FDreamTextureReflowActions::~FDreamTextureReflowActions()
{
	Model->OnChanged.Remove(ModelHandle);
	if (CurrentRun.IsValid())
	{
		CurrentRun->OnChanged.Remove(RunHandle);
	}
	if (bBoundExternal)
	{
		if (const TSharedPtr<FDreamTextureConnection> Conn = BoundConnection.Pin())
		{
			Conn->OnLinkChanged.Remove(LinkHandle);
		}
	}
}

void FDreamTextureReflowActions::Initialize(bool bBindExternal)
{
	ModelHandle = Model->OnChanged.AddSP(this, &FDreamTextureReflowActions::HandleModelChanged);

	if (bBindExternal && !bBoundExternal)
	{
		bBoundExternal = true;
		if (Backend.GetConnection)
		{
			if (const TSharedPtr<FDreamTextureConnection> Conn = Backend.GetConnection())
			{
				BoundConnection = Conn;
				LinkHandle = Conn->OnLinkChanged.AddSP(this, &FDreamTextureReflowActions::HandleLinkChanged);
			}
		}
	}

	LoadCatalog();
	// 面板建出来时模型里可能已经有选区了（重新打开面板）。
	HandleModelChanged();
}

void FDreamTextureReflowActions::HandleLinkChanged(EDreamTextureLink NewLink)
{
	if (NewLink == EDreamTextureLink::Live && (CatalogState == ECatalogState::NotLoaded || CatalogState == ECatalogState::Failed))
	{
		LoadCatalog();
	}
}

//////////////////////////////////////////////////////////////////////////
// 目录

void FDreamTextureReflowActions::LoadCatalog()
{
	if (!Backend.ListWorkflows)
	{
		return;
	}

	const uint64 Gen = ++CatalogGeneration;
	CatalogState = ECatalogState::Loading;
	CatalogError.Reset();
	OnChanged.Broadcast();

	TWeakPtr<FDreamTextureReflowActions> WeakSelf = AsShared();
	Backend.ListWorkflows([WeakSelf, Gen](bool bOK, FDreamTextureWorkflowCatalog&& InCatalog, const FString& Error)
	{
		if (const TSharedPtr<FDreamTextureReflowActions> Self = WeakSelf.Pin())
		{
			Self->ApplyCatalog(Gen, bOK, MoveTemp(InCatalog), Error);
		}
	});
}

void FDreamTextureReflowActions::ApplyCatalog(uint64 Generation, bool bOK, FDreamTextureWorkflowCatalog&& InCatalog, const FString& Error)
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
		OnChanged.Broadcast();
		return;
	}

	Catalog = MoveTemp(InCatalog);
	CatalogState = ECatalogState::Ready;
	CatalogError.Reset();
	OnChanged.Broadcast();
}

const FDreamTextureWorkflowMeta* FDreamTextureReflowActions::FindUploadSource() const
{
	for (const FDreamTextureWorkflowMeta& Meta : Catalog.Segments)
	{
		if (Meta.Segment != EDreamTextureSegmentKind::Source || !IsMaterialPipeline(Meta) || Meta.bCloudSource)
		{
			continue;
		}
		const FDreamTextureParamSpec* Param = Meta.FindParam(TEXT("source_image"));
		if (Param != nullptr && Param->Type.Equals(TEXT("image"), ESearchCase::IgnoreCase))
		{
			return &Meta;
		}
	}
	return nullptr;
}

TArray<const FDreamTextureWorkflowMeta*> FDreamTextureReflowActions::GetRestyleSources() const
{
	TArray<const FDreamTextureWorkflowMeta*> Out;
	for (const FDreamTextureWorkflowMeta& Meta : Catalog.Segments)
	{
		if (Meta.Segment != EDreamTextureSegmentKind::Source || !IsMaterialPipeline(Meta) || Meta.bCloudSource)
		{
			continue;
		}
		// 云端出图段的 reference 是发给云端做图像编辑的，没有 denoise；只有本地的才是 img2img。
		const FDreamTextureParamSpec* Reference = Meta.FindParam(TEXT("reference"));
		if (Reference != nullptr && Reference->Type.Equals(TEXT("image"), ESearchCase::IgnoreCase)
			&& Meta.FindParam(TEXT("denoise")) != nullptr && Meta.FindParam(TEXT("prompt")) != nullptr)
		{
			Out.Add(&Meta);
		}
	}
	return SortedById(MoveTemp(Out));
}

TArray<const FDreamTextureWorkflowMeta*> FDreamTextureReflowActions::GetDecomposeOptions() const
{
	TArray<const FDreamTextureWorkflowMeta*> Out;
	for (const FDreamTextureWorkflowMeta& Meta : Catalog.Segments)
	{
		if (Meta.Segment == EDreamTextureSegmentKind::Decompose && IsMaterialPipeline(Meta))
		{
			Out.Add(&Meta);
		}
	}
	return SortedById(MoveTemp(Out));
}

bool FDreamTextureReflowActions::IsTileDependent(const FDreamTextureWorkflowMeta& Segment)
{
	return Segment.FindParam(TEXT("tileable")) != nullptr;
}

bool FDreamTextureReflowActions::IsOutOfDistribution(const FDreamTextureWorkflowMeta& Segment) const
{
	return UVKind == EDreamTextureUVKind::Unwrapped && IsTileDependent(Segment);
}

FText FDreamTextureReflowActions::DescribeSegment(const FDreamTextureWorkflowMeta& Segment, EDreamTextureUVKind Kind)
{
	FText Text = FText::FromString(Segment.Name.IsEmpty() ? Segment.Id : Segment.Name);
	if (!Segment.IsCommercial())
	{
		Text = FText::Format(LOCTEXT("SegmentResearchOnly", "{0} · Research use only"), Text);
	}
	if (Kind == EDreamTextureUVKind::Unwrapped && IsTileDependent(Segment))
	{
		Text = FText::Format(LOCTEXT("SegmentOutOfDistribution", "{0} · Out of distribution, quality not guaranteed"), Text);
	}
	return Text;
}

//////////////////////////////////////////////////////////////////////////
// 模式、分解模型

void FDreamTextureReflowActions::SetMode(EDreamTextureReflowMode InMode)
{
	if (InMode == Mode)
	{
		return;
	}
	Mode = InMode;
	// 默认勾选随模式变（改写外观时底色要勾），所以换模式就回到默认；用户的手动勾选是针对上一个模式的。
	bSelectionTouched = false;
	RebuildStates(/*bMaterialChanged*/ false);
	OnChanged.Broadcast();
}

void FDreamTextureReflowActions::SetDecomposeId(const FString& Id)
{
	if (Id == DecomposeChoice)
	{
		return;
	}
	DecomposeChoice = Id;
	OnChanged.Broadcast();
}

FString FDreamTextureReflowActions::GetDecomposeId() const
{
	const TArray<const FDreamTextureWorkflowMeta*> Options = GetDecomposeOptions();
	for (const FDreamTextureWorkflowMeta* Meta : Options)
	{
		if (Meta->Id == DecomposeChoice)
		{
			return Meta->Id;
		}
	}
	// UV 展开：默认选传统派生，不是 CHORD——CHORD 在平铺素材上训练，UV 展开图对它是分布外输入。
	// 用户明确选过的不动（上面已经返回了）；CHORD 仍可手动选，旁边标「分布外，质量不保证」。
	if (UVKind == EDreamTextureUVKind::Unwrapped)
	{
		for (const FDreamTextureWorkflowMeta* Meta : Options)
		{
			if (!IsTileDependent(*Meta))
			{
				return Meta->Id;
			}
		}
	}

	// 平铺：与生成页、Web 端一致，按 id 排序的第一个。许可提示在选中后马上显示，不靠默认值回避。
	return Options.Num() > 0 ? Options[0]->Id : FString();
}

void FDreamTextureReflowActions::SetRestyleSourceId(const FString& Id)
{
	if (Id == RestyleSourceChoice)
	{
		return;
	}
	RestyleSourceChoice = Id;
	OnChanged.Broadcast();
}

FString FDreamTextureReflowActions::GetRestyleSourceId() const
{
	const TArray<const FDreamTextureWorkflowMeta*> Options = GetRestyleSources();
	for (const FDreamTextureWorkflowMeta* Meta : Options)
	{
		if (Meta->Id == RestyleSourceChoice)
		{
			return Meta->Id;
		}
	}
	return Options.Num() > 0 ? Options[0]->Id : FString();
}

const FDreamTextureWorkflowMeta* FDreamTextureReflowActions::GetCurrentWorkflow() const
{
	if (CatalogState != ECatalogState::Ready && Catalog.Workflows.Num() == 0)
	{
		return nullptr;
	}
	const FDreamTextureWorkflowMeta* Source = nullptr;
	if (Mode == EDreamTextureReflowMode::FillChannels)
	{
		Source = FindUploadSource();
	}
	else
	{
		Source = Catalog.FindSegment(GetRestyleSourceId());
	}
	if (Source == nullptr)
	{
		return nullptr;
	}
	return Catalog.FindComposed(Source->Id, GetDecomposeId());
}

FDreamTextureReflowDecomposeHint FDreamTextureReflowActions::GetDecomposeHint() const
{
	FDreamTextureReflowDecomposeHint Hint;

	// 许可看组合（两段的并集）；组合暂时找不到时退回只看所选的分解段，不能因为目录没到齐就不提醒。
	if (const FDreamTextureWorkflowMeta* Workflow = GetCurrentWorkflow())
	{
		Hint.bNonCommercial = !Workflow->IsCommercial();
		Hint.LicenseReason = Workflow->GetNonCommercialReason();
		Hint.Mismatch = Workflow->Mismatch;
	}
	else if (const FDreamTextureWorkflowMeta* Decompose = Catalog.FindSegment(GetDecomposeId()))
	{
		Hint.bNonCommercial = !Decompose->IsCommercial();
		Hint.LicenseReason = Decompose->GetNonCommercialReason();
	}

	if (const FDreamTextureWorkflowMeta* Decompose = Catalog.FindSegment(GetDecomposeId()))
	{
		if (IsOutOfDistribution(*Decompose))
		{
			Hint.bOutOfDistribution = true;
			Hint.OutOfDistributionText = FText::Format(
				LOCTEXT("OutOfDistributionText", "Out of distribution, quality not guaranteed: {0} was trained on tileable material, so a UV-unwrapped texture is an out-of-distribution input for it, and Normal and Height may come out blurry or with artifacts. For a safe result, choose Classic derivation."),
				FText::FromString(Decompose->Name.IsEmpty() ? Decompose->Id : Decompose->Name)).ToString();
		}
	}
	return Hint;
}

//////////////////////////////////////////////////////////////////////////
// 底图

TArray<FString> FDreamTextureReflowActions::GetSourceCandidates() const
{
	TArray<FString> Out;
	if (Model->GetViewState() == FDreamTextureReflowModel::EViewState::Ready)
	{
		for (const FDreamTextureMaterialTexture* Row : FDreamTextureReflowPlanner::FindBaseColorSources(Model->GetAnalysis()))
		{
			Out.Add(Row->TexturePath);
		}
	}
	return Out;
}

void FDreamTextureReflowActions::SetSourceTexture(const FString& TexturePath)
{
	SourceTextureChoice = TexturePath;
	OnChanged.Broadcast();
}

FString FDreamTextureReflowActions::GetSourceTexturePath() const
{
	const TArray<FString> Candidates = GetSourceCandidates();
	if (Candidates.Contains(SourceTextureChoice))
	{
		return SourceTextureChoice;
	}
	return Candidates.Num() > 0 ? Candidates[0] : FString();
}

const FDreamTextureMaterialTexture* FDreamTextureReflowActions::GetSourceTexture() const
{
	const FString Path = GetSourceTexturePath();
	return Path.IsEmpty() ? nullptr : Model->FindTexture(Path);
}

//////////////////////////////////////////////////////////////////////////
// 通道

FString FDreamTextureReflowActions::GetCurrentAssetPath() const
{
	return Model->GetViewState() == FDreamTextureReflowModel::EViewState::Ready ? Model->GetAnalysis().AssetPath : FString();
}

void FDreamTextureReflowActions::RebuildStates(bool bMaterialChanged)
{
	States.Reset();
	if (Model->GetViewState() == FDreamTextureReflowModel::EViewState::Ready)
	{
		const FDreamTextureReflowCandidate* Candidate = Model->GetSelectedCandidate();
		States = FDreamTextureReflowPlanner::Describe(Model->GetAnalysis(), Candidate != nullptr ? Candidate->Material.Get() : nullptr);
	}

	if (bMaterialChanged)
	{
		bSelectionTouched = false;
	}
	Selection = bSelectionTouched
		? FDreamTextureReflowPlanner::Normalize(States, Selection, Mode)
		: FDreamTextureReflowPlanner::DefaultSelection(States, Mode);
}

void FDreamTextureReflowActions::HandleModelChanged()
{
	const FString NewPath = GetCurrentAssetPath();
	const bool bMaterialChanged = NewPath != CurrentAssetPath;

	if (bMaterialChanged)
	{
		CurrentAssetPath = NewPath;
		SourceTextureChoice.Reset();
		bInPlace = false;
		bDenoiseTouched = false;

		// 平铺 / UV 展开按材质记着：换到哪个材质就读哪个的选择，没记过的就是平铺。
		UVKind = EDreamTextureUVKind::Tiled;
		if (!NewPath.IsEmpty())
		{
			Model->GetOverrideStore().FindUVKind(NewPath, UVKind);
		}

		// 上一次运行属于上一个材质：不再显示在这个材质的页面上（它自己照常跑完、照常落）。
		if (CurrentRun.IsValid() && CurrentRun->GetSpec().MaterialAssetPath != NewPath)
		{
			CurrentRun->OnChanged.Remove(RunHandle);
			CurrentRun.Reset();
		}
	}

	RebuildStates(bMaterialChanged);
	OnChanged.Broadcast();
}

void FDreamTextureReflowActions::SetChannelChecked(const FString& Channel, bool bChecked)
{
	bSelectionTouched = true;
	Selection = FDreamTextureReflowPlanner::Toggle(States, Selection, Mode, Channel, bChecked);
	OnChanged.Broadcast();
}

void FDreamTextureReflowActions::ResetSelection()
{
	bSelectionTouched = false;
	Selection = FDreamTextureReflowPlanner::DefaultSelection(States, Mode);
	OnChanged.Broadcast();
}

TArray<FDreamTextureReflowChannelView> FDreamTextureReflowActions::GetChannelViews() const
{
	using EPresence = FDreamTextureReflowChannelState::EPresence;

	TArray<FDreamTextureReflowChannelView> Views;
	for (const FDreamTextureReflowChannelState& State : States)
	{
		FDreamTextureReflowChannelView View;
		View.Channel = State.Channel;
		View.Label = FText::FromString(DreamTextureReflow::GetChannelLabel(State.Channel));
		View.bPresent = State.IsPresent();
		View.bChecked = Selection.Contains(State.Channel);
		View.bEnabled = !FDreamTextureReflowPlanner::IsLocked(State.Channel, Mode);

		switch (State.Presence)
		{
		case EPresence::Missing:
			View.Status = LOCTEXT("StatusMissing", "Missing");
			break;

		case EPresence::Dedicated:
			{
				FString Where;
				if (State.Parameters.Num() == 0)
				{
					Where = LOCTEXT("WhereDirect", "direct reference, no parameter").ToString();
				}
				else
				{
					TArray<FString> Names;
					for (const FName& Name : State.Parameters)
					{
						Names.Add(Name.ToString());
					}
					Where = FText::Format(LOCTEXT("WhereParams", "parameter(s) {0}"), FText::FromString(FString::Join(Names, *LOCTEXT("ListSeparator", ", ").ToString()))).ToString();
				}
				View.Status = FText::Format(LOCTEXT("StatusDedicated", "Present: {0} ({1})"), FText::FromString(State.TextureName), FText::FromString(Where));
			}
			break;

		case EPresence::InStandardORM:
		case EPresence::InOtherPacked:
			{
				TArray<FString> Others;
				for (const FString& Tied : State.TiedChannels)
				{
					if (Tied != State.Channel)
					{
						Others.Add(DreamTextureReflow::GetChannelLabel(Tied));
					}
				}
				View.Status = State.Presence == EPresence::InStandardORM
					? FText::Format(LOCTEXT("StatusOrm", "Present: in {0} (same texture as {1})"), FText::FromString(State.TextureName), FText::FromString(FString::Join(Others, *LOCTEXT("ListSeparator", ", ").ToString())))
					: FText::Format(LOCTEXT("StatusOtherPacked", "Present: in {0} (layout is not standard ORM)"), FText::FromString(State.TextureName));
			}
			break;
		}

		TArray<FString> Tips;
		if (!View.bEnabled)
		{
			Tips.Add(LOCTEXT("TipBaseLocked", "The Base Color is not regenerated when filling channels: the new material instance's Base Color points straight at your original texture. The backend's Base Color has been resized and re-encoded, so swapping it in would quietly change your Base Color.").ToString());
		}
		else if (Mode == EDreamTextureReflowMode::Restyle && State.Channel == DreamTextureChannel::BaseColor)
		{
			Tips.Add(LOCTEXT("TipBaseRestyle", "Restyle regenerates the Base Color. If you clear this check, the new instance keeps the original Base Color, so the look does not change.").ToString());
		}
		else if (State.IsPresent())
		{
			Tips.Add(LOCTEXT("TipPresent", "The material already has this channel, so it is left alone by default. Checking it replaces it with the generated one.").ToString());
		}
		if (State.TiedChannels.Num() > 1)
		{
			Tips.Add(LOCTEXT("TipTied", "Channels in the same packed texture are replaced together: checking one checks the rest of its group.").ToString());
		}
		if (!State.Note.IsEmpty())
		{
			Tips.Add(State.Note);
		}

		if (bInPlace && View.bChecked && View.bEnabled)
		{
			FString Why;
			if (!FDreamTextureReflowPlanner::CanOverwriteChannel(State, Why))
			{
				View.bInPlaceBlocked = true;
				Tips.Add(Why);
			}
		}
		View.Tooltip = FText::FromString(FString::Join(Tips, TEXT("\n")));
		Views.Add(MoveTemp(View));
	}
	return Views;
}

//////////////////////////////////////////////////////////////////////////
// 平铺 / UV 展开

void FDreamTextureReflowActions::SetUVKind(EDreamTextureUVKind Kind)
{
	// 没选东西时页面上没有这个开关；不在 Ready 状态不记（没有可记的材质）。
	if (Kind == UVKind || Model->GetViewState() != FDreamTextureReflowModel::EViewState::Ready)
	{
		return;
	}
	UVKind = Kind;

	if (CurrentAssetPath.IsEmpty())
	{
		OnNotice.Broadcast(LOCTEXT("UVKindNoAsset", "This material is not an asset in the project (it was created at runtime), so the choice between Tileable and UV-unwrapped cannot be remembered. It applies this time, and you will need to choose again next time."), true);
	}
	else
	{
		FDreamTextureOverrideStore& Store = Model->GetOverrideStore();
		if (!Store.SetUVKind(CurrentAssetPath, Kind))
		{
			OnNotice.Broadcast(FText::Format(LOCTEXT("UVKindSaveFailed", "Could not remember this choice: {0}. It still applies for this editor session but will be lost after a restart."), FText::FromString(Store.GetLastError())), true);
		}
	}
	OnChanged.Broadcast();
}

//////////////////////////////////////////////////////////////////////////
// 结果预览

FString FDreamTextureReflowActions::MakeFileKey(const FString& FilePath)
{
	return TEXT("file:") + FilePath;
}

FString FDreamTextureReflowActions::MakeMaterialFileKey(const FString& MaterialId, const FString& FileName)
{
	return FString::Printf(TEXT("mat:%s/%s"), *MaterialId, *FileName);
}

FDreamTextureReflowPreviewPlan FDreamTextureReflowActions::GetPreviewPlan() const
{
	FDreamTextureReflowPreviewPlan Plan;
	const FDreamTextureReflowRun* Run = CurrentRun.Get();
	if (Run == nullptr || Run->GetState() != FDreamTextureReflowRun::EState::Succeeded || Run->GetMaterialId().IsEmpty())
	{
		return Plan;
	}

	// 平铺：后端出的 3×3 平铺预览，看接缝首尾相不相接。
	if (Run->GetSpec().UVKind == EDreamTextureUVKind::Tiled)
	{
		const FString File = Run->HasManifest() && !Run->GetManifest().Preview.IsEmpty() ? Run->GetManifest().Preview : FString(TEXT("preview.png"));
		Plan.Kind = FDreamTextureReflowPreviewPlan::EKind::Tiled;
		Plan.LeftKey = MakeMaterialFileKey(Run->GetMaterialId(), File);
		Plan.LeftLabel = LOCTEXT("PreviewTiled", "Tiled preview (3×3)");
		return Plan;
	}

	// UV 展开：3×3 平铺图没有意义（边缘本来就不相接），改成「原图 | 结果」并排。
	// 要 manifest 才知道结果里有哪些通道；没拿到就先不画（说明文字已经在了）。
	if (!Run->HasManifest() || Run->GetExportedFile().IsEmpty())
	{
		return Plan;
	}

	// 挑最能说明问题的一张：
	//   补通道：底色是原样进分解的，拿它和原图并排什么也看不出来；法线是新补出来的，一眼就看得出补了什么。
	//   改写外观：底色就是重画的结果，和原图并排最直观。
	const bool bRestyle = Run->GetSpec().Mode == EDreamTextureReflowMode::Restyle;
	const TArray<FString> Order = bRestyle
		? TArray<FString>({ DreamTextureChannel::BaseColor, DreamTextureChannel::Normal })
		: TArray<FString>({ DreamTextureChannel::Normal, DreamTextureChannel::Roughness, DreamTextureChannel::AO, DreamTextureChannel::Height });

	for (const FString& Channel : Order)
	{
		const FDreamTextureMapInfo* Info = Run->GetManifest().FindMap(Channel);
		if (Info == nullptr || Info->File.IsEmpty())
		{
			continue;
		}
		Plan.Kind = FDreamTextureReflowPreviewPlan::EKind::SideBySide;
		Plan.LeftKey = MakeFileKey(Run->GetExportedFile());
		Plan.RightKey = MakeMaterialFileKey(Run->GetMaterialId(), Info->File);
		Plan.LeftLabel = LOCTEXT("PreviewOriginal", "Original");
		Plan.RightLabel = bRestyle
			? FText::Format(LOCTEXT("PreviewRestyled", "Restyled {0}"), FText::FromString(DreamTextureReflow::GetChannelLabel(Channel)))
			: FText::Format(LOCTEXT("PreviewFilled", "Generated {0}"), FText::FromString(DreamTextureReflow::GetChannelLabel(Channel)));
		return Plan;
	}
	return Plan;
}

//////////////////////////////////////////////////////////////////////////
// 提示词、重绘幅度、分辨率

void FDreamTextureReflowActions::SetPrompt(const FString& InPrompt)
{
	Prompt = InPrompt;
}

void FDreamTextureReflowActions::SetDenoise(double InDenoise)
{
	DenoiseChoice = InDenoise;
	bDenoiseTouched = true;
	OnChanged.Broadcast();
}

double FDreamTextureReflowActions::GetDenoise() const
{
	double Min = DreamTextureReflow::MinDenoise;
	double Max = DenoiseFallbackMax;
	if (const FDreamTextureWorkflowMeta* Workflow = GetCurrentWorkflow())
	{
		if (const FDreamTextureParamSpec* Spec = Workflow->FindParam(TEXT("denoise")))
		{
			Min = Spec->Min.Get(Min);
			Max = Spec->Max.Get(Max);
		}
	}
	const double Default = UVKind == EDreamTextureUVKind::Unwrapped ? DreamTextureReflow::DefaultUnwrappedDenoise : DreamTextureReflow::DefaultTiledDenoise;
	return FMath::Clamp(bDenoiseTouched ? DenoiseChoice : Default, Min, Max);
}

bool FDreamTextureReflowActions::IsDenoiseBlocked(FString* OutReason) const
{
	// 只有改写外观才有重绘幅度。
	if (Mode != EDreamTextureReflowMode::Restyle)
	{
		return false;
	}
	const double Denoise = GetDenoise();
	if (DreamTextureReflow::IsDenoiseAllowed(UVKind, Denoise))
	{
		return false;
	}
	if (OutReason != nullptr)
	{
		*OutReason = DreamTextureReflow::DescribeDenoiseLimit(Denoise);
	}
	return true;
}

TArray<int32> FDreamTextureReflowActions::GetResolutionOptions() const
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

void FDreamTextureReflowActions::SetResolution(int32 InResolution)
{
	ResolutionChoice = InResolution;
	OnChanged.Broadcast();
}

int32 FDreamTextureReflowActions::GetResolution() const
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
		// 换了模式 / 分解模型后，之前选的分辨率可能不在新的候选里：回到默认，别提交一个会被后端拒掉的值。
		if (ResolutionChoice > 0 && Options.Contains(ResolutionChoice))
		{
			return ResolutionChoice;
		}
		const int32 Default = Spec->GetDefaultInt(Options[0]);
		return Options.Contains(Default) ? Default : Options[0];
	}
	return ResolutionChoice > 0 ? ResolutionChoice : Spec->GetDefaultInt(0);
}

//////////////////////////////////////////////////////////////////////////
// 落回

void FDreamTextureReflowActions::SetInPlace(bool bInInPlace)
{
	// 不成立的原地覆盖不让选上：置灰的控件点不动，这里是兜底（测试和以后别的入口）。
	if (bInInPlace && !CanOverwriteInPlace())
	{
		return;
	}
	if (bInPlace == bInInPlace)
	{
		return;
	}
	bInPlace = bInInPlace;
	OnChanged.Broadcast();
}

bool FDreamTextureReflowActions::CanOverwriteInPlace(FString* OutReason) const
{
	FString Why;
	bool bOK = false;
	if (Model->GetViewState() != FDreamTextureReflowModel::EViewState::Ready)
	{
		Why = LOCTEXT("NothingToOverwrite", "No material has been analyzed yet, so there is nothing to change.").ToString();
	}
	else
	{
		const FDreamTextureReflowCandidate* Candidate = Model->GetSelectedCandidate();
		bOK = FDreamTextureReflowPlanner::CanOverwriteInPlace(Model->GetAnalysis(), Candidate != nullptr ? Candidate->Material.Get() : nullptr, Why);
	}
	if (OutReason != nullptr)
	{
		*OutReason = Why;
	}
	return bOK;
}

//////////////////////////////////////////////////////////////////////////
// 运行

bool FDreamTextureReflowActions::BuildSpec(FDreamTextureReflowRunSpec& OutSpec, FText* OutError) const
{
	auto Fail = [OutError](const FText& Why)
	{
		if (OutError != nullptr)
		{
			*OutError = Why;
		}
		return false;
	};

	if (Model->GetViewState() != FDreamTextureReflowModel::EViewState::Ready)
	{
		return Fail(Model->GetStatusText());
	}
	const FDreamTextureMaterialAnalysis& Analysis = Model->GetAnalysis();

	// ---- 底图 ----
	const FDreamTextureMaterialTexture* Source = GetSourceTexture();
	if (Source == nullptr)
	{
		// 说清楚是哪种没有：根本没认出底色，还是认出了但导不出。
		const FDreamTextureMaterialTexture* Blocked = Analysis.Textures.FindByPredicate([](const FDreamTextureMaterialTexture& Row)
		{
			return Row.Role == EDreamTextureRole::BaseColor && !Row.bExportable;
		});
		if (Blocked != nullptr)
		{
			return Fail(FText::Format(LOCTEXT("SourceNotExportable", "The Base Color texture {0} cannot be exported: {1}"), FText::FromString(Blocked->AssetName), FText::FromString(Blocked->ExportBlocker)));
		}
		return Fail(LOCTEXT("NoSource", "No Base Color texture was recognized in this material, and Reflow needs one as its input. In the table above, assign the Base Color texture manually as Base Color."));
	}

	// ---- 工作流 ----
	switch (CatalogState)
	{
	case ECatalogState::Ready:
		break;
	case ECatalogState::Failed:
		return Fail(FText::Format(LOCTEXT("CatalogFailed", "Could not read the workflow list: {0}"), FText::FromString(CatalogError)));
	default:
		return Fail(LOCTEXT("CatalogLoading", "Reading the workflow list…"));
	}

	if (Mode == EDreamTextureReflowMode::FillChannels && FindUploadSource() == nullptr)
	{
		return Fail(LOCTEXT("NoUploadSource", "This backend does not support Fill missing channels yet: the workflow catalog has no source segment that takes an existing image. Update the backend and try again."));
	}
	if (Mode == EDreamTextureReflowMode::Restyle && GetRestyleSources().Num() == 0)
	{
		return Fail(LOCTEXT("NoRestyleSource", "This backend has no local image segment that can Restyle (it needs a local image workflow that takes a reference image and a denoise strength). Update the backend and try again."));
	}
	if (GetDecomposeOptions().Num() == 0)
	{
		return Fail(LOCTEXT("NoDecompose", "The workflow catalog has no decompose model."));
	}
	const FDreamTextureWorkflowMeta* Workflow = GetCurrentWorkflow();
	if (Workflow == nullptr)
	{
		const FString SourceId = Mode == EDreamTextureReflowMode::FillChannels ? FindUploadSource()->Id : GetRestyleSourceId();
		return Fail(FText::Format(LOCTEXT("NoCombo", "The backend has no workflow for the combination \"{0} × {1}\". A segment may have just been added and the backend has not reloaded its workflows yet."),
			FText::FromString(SourceId), FText::FromString(GetDecomposeId())));
	}

	// ---- 请求 ----
	FDreamTextureGenerateRequest Request;
	Request.WorkflowId = Workflow->Id;
	Request.Variants = 1;
	const FString BaseName = StripAssetPrefix(Analysis.DisplayName);
	Request.Name = BaseName + (Mode == EDreamTextureReflowMode::FillChannels ? LOCTEXT("NameSuffixFill", " fill").ToString() : LOCTEXT("NameSuffixRestyle", " restyle").ToString());

	const int32 Resolution = GetResolution();
	if (Resolution > 0)
	{
		Request.IntParams.Add(TEXT("resolution"), Resolution);
	}

	FString UploadKey;
	if (Mode == EDreamTextureReflowMode::FillChannels)
	{
		UploadKey = TEXT("source_image");
	}
	else
	{
		UploadKey = TEXT("reference");
		const FString Trimmed = Prompt.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return Fail(LOCTEXT("NeedPrompt", "Restyle needs a prompt that says what the material should become."));
		}
		Request.StringParams.Add(TEXT("prompt"), Trimmed);
		// UV 展开的硬上限：超过就拦住，说人话——重绘会破坏 UV 对应关系，贴回模型就全乱了。
		FString DenoiseWhy;
		if (IsDenoiseBlocked(&DenoiseWhy))
		{
			return Fail(FText::FromString(DenoiseWhy));
		}
		Request.FloatParams.Add(TEXT("denoise"), GetDenoise());
		Request.SetSeed(DreamTextureGenerateLimits::RandomSeed);

		// 参考图要缩到与出图一致的尺寸，免得比例不对时出怪东西（后端文档：应与出图分辨率一致）。
		if (const FDreamTextureParamSpec* RefSize = Workflow->FindParam(TEXT("reference_size")))
		{
			TArray<int32> Options;
			if (Resolution > 0 && RefSize->TryGetIntOptions(Options) && Options.Contains(Resolution))
			{
				Request.IntParams.Add(TEXT("reference_size"), Resolution);
			}
		}
	}

	// 平铺 / UV 展开：声明了 tileable 才发（旧后端没有这个参数，后端只认声明过的键，发了也是白发）。
	// UV 展开时 tileable=false：出图段关环绕填充、提示词去掉平铺词，CHORD 也关环绕；manifest 如实记为不可平铺。
	if (Workflow->FindParam(TEXT("tileable")) != nullptr)
	{
		Request.BoolParams.Add(TEXT("tileable"), UVKind == EDreamTextureUVKind::Tiled);
	}

	Request.DerivedFrom.Kind = FDreamTextureDerivedFrom::KindUEMaterial();
	Request.DerivedFrom.AssetPath = Analysis.AssetPath;
	Request.DerivedFrom.Engine = Backend.GetEngineVersion ? Backend.GetEngineVersion() : FString();
	Request.DerivedFrom.Mode = LexToString(Mode);
	// maps_in 要等导出之后才知道指纹，由运行填。

	FString ValidateError;
	if (!Request.Validate(ValidateError))
	{
		return Fail(FText::FromString(ValidateError));
	}

	// ---- 通道计划 ----
	FDreamTextureImportOptions ImportOptions;
	TArray<FDreamTextureReflowOverwrite> Overwrites;
	TArray<FString> Notes;
	TWeakObjectPtr<UMaterialInstanceConstant> Target;

	if (bInPlace)
	{
		FString Why;
		if (!CanOverwriteInPlace(&Why))
		{
			return Fail(FText::FromString(Why));
		}
		const FDreamTextureReflowInPlacePlan Plan = FDreamTextureReflowPlanner::PlanInPlace(States, Selection, Mode);
		if (!Plan.bOK)
		{
			FString Message = Plan.Error;
			for (const FString& Skipped : Plan.Skipped)
			{
				Message += TEXT("\n") + Skipped;
			}
			return Fail(FText::FromString(Message));
		}
		ImportOptions = Plan.Options;
		Overwrites = Plan.Items;
		for (const FString& Skipped : Plan.Skipped)
		{
			Notes.Add(FText::Format(LOCTEXT("NoteLeftAlone", "Left unchanged: {0}"), FText::FromString(Skipped)).ToString());
		}
		const FDreamTextureReflowCandidate* Candidate = Model->GetSelectedCandidate();
		Target = Candidate != nullptr ? Cast<UMaterialInstanceConstant>(Candidate->Material.Get()) : nullptr;
	}
	else
	{
		const FDreamTextureReflowCreatePlan Plan = FDreamTextureReflowPlanner::PlanCreate(States, Selection, Mode, Source->TexturePath);
		if (!Plan.bOK)
		{
			return Fail(FText::FromString(Plan.Error));
		}
		ImportOptions = Plan.Options;
		Notes = Plan.Notes;
	}

	FDreamTextureReflowRunSpec Spec;
	Spec.Mode = Mode;
	Spec.UVKind = UVKind;
	Spec.bInPlace = bInPlace;
	Spec.Title = FText::Format(LOCTEXT("RunTitle", "{0} · {1}{2}"),
	Mode == EDreamTextureReflowMode::FillChannels ? LOCTEXT("RunTitleFill", "Fill missing channels") : LOCTEXT("RunTitleRestyle", "Restyle"),
	FText::FromString(Analysis.DisplayName),
	UVKind == EDreamTextureUVKind::Unwrapped ? LOCTEXT("RunTitleUnwrapped", " (UV-unwrapped)") : FText::GetEmpty()).ToString();
	Spec.MaterialAssetPath = Analysis.AssetPath;
	Spec.MaterialName = Analysis.DisplayName;
	Spec.ExportAnalysis = Analysis;
	Spec.ExportAnalysis.Textures = { *Source };
	Spec.SourceTexturePath = Source->TexturePath;
	Spec.SourceTextureName = Source->AssetName;
	Spec.ExportMaxEdge = Model->GetExportMaxEdge() > 0
		? FMath::Min(Model->GetExportMaxEdge(), DreamTextureReflow::DefaultExportMaxEdge)
		: DreamTextureReflow::DefaultExportMaxEdge;
	Spec.Request = MoveTemp(Request);
	Spec.UploadParamKey = UploadKey;
	Spec.ImportOptions = MoveTemp(ImportOptions);
	Spec.InPlaceTarget = Target;
	Spec.Overwrites = MoveTemp(Overwrites);
	Spec.PlanNotes = MoveTemp(Notes);
	OutSpec = MoveTemp(Spec);
	return true;
}

bool FDreamTextureReflowActions::CanRun(FText* OutReason) const
{
	auto Fail = [OutReason](const FText& Why)
	{
		if (OutReason != nullptr)
		{
			*OutReason = Why;
		}
		return false;
	};

	if (IsRunActive())
	{
		return Fail(LOCTEXT("RunActive", "The previous reflow is still in progress."));
	}

	FDreamTextureReflowRunSpec Spec;
	FText Why;
	if (!BuildSpec(Spec, &Why))
	{
		return Fail(Why);
	}

	// 后端没起不拦：开始时会先把它拉起来。只有 ComfyUI 明确没在跑（第 3 级）才拦——
	// 提交了任务也只会停在「等待 ComfyUI」，最长等 15 分钟。三级不要混成一个灯。
	const FDreamTextureGenerateReadiness Readiness = FDreamTextureGenerateModel::EvaluateReadiness(Backend.GetConnection ? Backend.GetConnection() : nullptr);
	if (!Readiness.bCanSubmit && Readiness.Tier == EDreamTextureGenerateTier::Comfy)
	{
		return Fail(Readiness.Reason);
	}
	return true;
}

FText FDreamTextureReflowActions::GetRunNote() const
{
	TArray<FString> Notes;

	const FDreamTextureGenerateReadiness Readiness = FDreamTextureGenerateModel::EvaluateReadiness(Backend.GetConnection ? Backend.GetConnection() : nullptr);
	if (!Readiness.bCanSubmit && Readiness.Tier == EDreamTextureGenerateTier::Backend)
	{
		Notes.Add(LOCTEXT("NoteBackendDown", "The backend process is not responding. Clicking Start will try to launch it first.").ToString());
	}
	else if (!Readiness.Note.IsEmpty())
	{
		Notes.Add(Readiness.Note.ToString());
	}

	if (const FDreamTextureMaterialTexture* Source = GetSourceTexture())
	{
		if (Source->SourceWidth > 0 && Source->SourceHeight > 0 && Source->SourceWidth != Source->SourceHeight)
		{
			Notes.Add(FText::Format(LOCTEXT("NoteNotSquare", "The Base Color texture is not square ({0}×{1}): the backend center-crops it to a square, so the generated channels only line up with the middle part. Crop it to a square first."),
			FText::AsNumber(Source->SourceWidth, &FNumberFormattingOptions::DefaultNoGrouping()),
			FText::AsNumber(Source->SourceHeight, &FNumberFormattingOptions::DefaultNoGrouping())).ToString());
		}
	}
	return FText::FromString(FString::Join(Notes, TEXT("\n")));
}

void FDreamTextureReflowActions::Run()
{
	if (IsRunActive())
	{
		return;
	}

	FText Why;
	if (!CanRun(&Why))
	{
		OnNotice.Broadcast(Why, true);
		OnChanged.Broadcast();
		return;
	}

	FDreamTextureReflowRunSpec Spec;
	BuildSpec(Spec);

	if (!Backend.MakeRun)
	{
		return;
	}

	if (CurrentRun.IsValid())
	{
		CurrentRun->OnChanged.Remove(RunHandle);
	}
	CurrentRun = Backend.MakeRun(MoveTemp(Spec));
	bRunNotified = false;
	RunHandle = CurrentRun->OnChanged.AddSP(this, &FDreamTextureReflowActions::HandleRunChanged);
	OnChanged.Broadcast();
	CurrentRun->Start();
}

void FDreamTextureReflowActions::CancelRun()
{
	if (CurrentRun.IsValid())
	{
		CurrentRun->Cancel();
	}
}

void FDreamTextureReflowActions::HandleRunChanged()
{
	// 收尾只通知一次：结束之后拉到 manifest 还会再广播一次变化。
	if (CurrentRun.IsValid() && CurrentRun->IsFinished() && !bRunNotified)
	{
		bRunNotified = true;
		const bool bOK = CurrentRun->GetState() == FDreamTextureReflowRun::EState::Succeeded;
		OnNotice.Broadcast(CurrentRun->GetMessage(), !bOK);

		// 原地覆盖改了实例：表里的现状跟着刷新。（新建不动原材质，没必要。）
		if (bOK && CurrentRun->GetSpec().bInPlace)
		{
			Model->Reanalyze();
		}
	}
	OnChanged.Broadcast();
}

#undef LOCTEXT_NAMESPACE
