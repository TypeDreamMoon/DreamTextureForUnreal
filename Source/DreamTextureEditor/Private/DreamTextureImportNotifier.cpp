// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureImportNotifier.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserModule.h"
#include "DreamTextureAssignModel.h"
#include "DreamTextureSettings.h"
#include "DreamTextureLog.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "IContentBrowserSingleton.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "DreamTextureImportNotifier"

FDreamTextureImportNotifier::~FDreamTextureImportNotifier()
{
	Stop();
}

void FDreamTextureImportNotifier::Start()
{
	if (Handle.IsValid())
	{
		return;
	}
	Handle = FDreamTextureImportQueue::Get().OnStatusChanged.AddSP(this, &FDreamTextureImportNotifier::HandleStatus);
}

void FDreamTextureImportNotifier::Stop()
{
	if (Handle.IsValid())
	{
		FDreamTextureImportQueue::Get().OnStatusChanged.Remove(Handle);
		Handle.Reset();
	}
}

void FDreamTextureImportNotifier::HandleStatus(const FDreamTextureImportStatus& Status)
{
	if (!Status.IsFinished())
	{
		// 回到进行中说明是新一轮导入：清掉「已通知」，下一次结束才会再弹。
		Notified.Remove(Status.MaterialId);
		return;
	}

	// 同一轮导入的结束状态只通知一次，万一实现方对同一个终态重复广播也不会连弹。
	if (Notified.Contains(Status.MaterialId))
	{
		return;
	}
	Notified.Add(Status.MaterialId);

	Show(Describe(Status));
}

FDreamTextureImportNotifier::FMaterialInstanceSummary FDreamTextureImportNotifier::DescribeMaterialInstance(const FDreamTextureImportResult& Result, bool bCreateEnabled)
{
	FMaterialInstanceSummary Out;
	const FDreamTextureImportedTexture& MI = Result.MaterialInstance;
	Out.Action = MI.Action;

	// ObjectPath 形如 /Game/X/MI_A.MI_A，名字取最后一个 '.' 之后。
	FString Name;
	if (!MI.ObjectPath.Split(TEXT("."), nullptr, &Name, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
	{
		Name = FPaths::GetCleanFilename(MI.ObjectPath);
	}

	const bool bPresent = !MI.ObjectPath.IsEmpty()
		&& (MI.Action == EDreamTextureImportAction::Created || MI.Action == EDreamTextureImportAction::Updated
			|| MI.Action == EDreamTextureImportAction::Unchanged);

	if (bPresent)
	{
		Out.Kind = FMaterialInstanceSummary::EKind::Present;
		Out.AssetName = Name;
		Out.ObjectPath = MI.ObjectPath;

		const FText ActionText = MI.Action == EDreamTextureImportAction::Created ? LOCTEXT("MIActionCreated", "created")
			: MI.Action == EDreamTextureImportAction::Updated ? LOCTEXT("MIActionUpdated", "updated") : LOCTEXT("MIActionUnchanged", "unchanged");
		Out.Sentence = FText::Format(LOCTEXT("MIPresent", "Material instance {0} {1}"),
			FText::FromString(Name), ActionText);
		return Out;
	}

	if (!bCreateEnabled && MI.Action != EDreamTextureImportAction::Failed)
	{
		Out.Kind = FMaterialInstanceSummary::EKind::DisabledBySetting;
		Out.Sentence = LOCTEXT("MIDisabled", "No material instance was created: 'Create Material Instance' is turned off in the plugin settings.");
		return Out;
	}

	Out.Kind = FMaterialInstanceSummary::EKind::NotCreated;
	Out.Reason = !MI.Note.IsEmpty() ? MI.Note : LOCTEXT("NoReasonGiven", "No reason was given.").ToString();
	Out.Sentence = FText::Format(LOCTEXT("MINotCreated", "No material instance was created: {0}"), FText::FromString(Out.Reason));
	return Out;
}

FText FDreamTextureImportNotifier::DescribeLicense(const FDreamTextureLicense& License)
{
	if (!License.bKnown || License.bCommercialUse)
	{
		return FText::GetEmpty();
	}

	const FString Reason = License.Reason.TrimStartAndEnd();
	if (Reason.IsEmpty())
	{
		return LOCTEXT("LicenseNonCommercialNoReason", "License: Not for commercial use");
	}
	return FText::Format(LOCTEXT("LicenseNonCommercial", "License: Not for commercial use ({0})"), FText::FromString(Reason));
}

FDreamTextureImportNotifier::FNoticeContent FDreamTextureImportNotifier::Describe(const FDreamTextureImportStatus& Status)
{
	return Describe(Status, UDreamTextureSettings::Get().bCreateMaterialInstance);
}

FDreamTextureImportNotifier::FNoticeContent FDreamTextureImportNotifier::Describe(const FDreamTextureImportStatus& Status, bool bCreateMaterialInstance)
{
	FNoticeContent Out;

	// 通知里用资产目录的最后一段（如 MossyRiverRocks_1f53）当名字：
	// 状态里没有材质名，而这一段本来就由名字和 id 尾巴拼成，人读得出是哪套。
	const FString Folder = Status.Result.AssetFolder;
	const FString ShortName = !Folder.IsEmpty() ? FPaths::GetCleanFilename(Folder) : Status.MaterialId;

	const bool bSucceeded = Status.Stage == EDreamTextureImportStage::Succeeded && Status.Result.bOK;
	if (!bSucceeded)
	{
		// Failed 阶段的 Message 就是失败原因；Succeeded 阶段但 bOK 为假（全部跳过之类）
		// 时原因在 Result.Error，Message 只是阶段说明。两种顺序不同，别混。
		const FString& First = Status.Stage == EDreamTextureImportStage::Failed ? Status.Message : Status.Result.Error;
		const FString& Second = Status.Stage == EDreamTextureImportStage::Failed ? Status.Result.Error : Status.Message;
		const FString& Reason = !First.IsEmpty() ? First : Second;

		Out.bIsError = true;
		Out.Message = FText::Format(LOCTEXT("ImportFailed", "Import of '{0}' failed: {1}"),
			FText::FromString(ShortName),
			Reason.IsEmpty() ? LOCTEXT("UnknownReason", "Unknown reason") : FText::FromString(Reason));
		return Out;
	}

	int32 Created = 0;
	int32 Updated = 0;
	int32 Unchanged = 0;
	int32 Problem = 0;
	for (const FDreamTextureImportedTexture& Tex : Status.Result.Textures)
	{
		switch (Tex.Action)
		{
		case EDreamTextureImportAction::Created:   ++Created; break;
		case EDreamTextureImportAction::Updated:   ++Updated; break;
		case EDreamTextureImportAction::Unchanged: ++Unchanged; break;
		case EDreamTextureImportAction::Skipped:
		case EDreamTextureImportAction::Failed:
		default:
			++Problem;
			// 逐张的原因只进日志，通知里放不下。
			UE_LOG(LogDreamTexture, Warning, TEXT("Import %s: channel %s was not written: %s"), *ShortName, *Tex.Channel, *Tex.Note);
			break;
		}
	}

	// {1} 直接传 int32：「texture(s)」的复数规则只认数值参数，传 FText 会求值失败。
	Out.Message = FText::Format(LOCTEXT("ImportDone", "Imported '{0}': {1} {1}|plural(one=texture,other=textures) created, {2} updated, {3} unchanged"),
		FText::FromString(ShortName), Created, FText::AsNumber(Updated), FText::AsNumber(Unchanged));

	if (Problem > 0)
	{
		Out.Message = FText::Format(LOCTEXT("ImportDoneWithProblems", "{0} ({1} more were not imported; see the Output Log for the reasons.)"),
			Out.Message, FText::AsNumber(Problem));
	}

	// 材质实例另起一行说：新建/更新/未变，或没建的原因。
	const FMaterialInstanceSummary MI = DescribeMaterialInstance(Status.Result, bCreateMaterialInstance);
	Out.Message = FText::Format(LOCTEXT("ImportDoneWithMI", "{0}\n{1}"), Out.Message, MI.Sentence);

	// 不可商用的产出（CHORD 分解）在这里就说出来，另起一行；可商用或 manifest 没写的不加这一行。
	const FText LicenseLine = DescribeLicense(Status.Result.License);
	if (!LicenseLine.IsEmpty())
	{
		Out.bNonCommercial = true;
		Out.Message = FText::Format(LOCTEXT("ImportDoneWithLicense", "{0}\n{1}"), Out.Message, LicenseLine);
	}

	// 有材质实例就选中它（用户要的是拿去用的那个），没有才退回跳到目录。
	Out.FolderToShow = Folder;
	if (MI.Kind == FMaterialInstanceSummary::EKind::Present)
	{
		Out.AssetToSelect = MI.ObjectPath;
	}
	return Out;
}

void FDreamTextureImportNotifier::ShowAssetInContentBrowser(const FString& ObjectPath, const FString& FallbackFolder)
{
	if (!ObjectPath.IsEmpty() && FModuleManager::Get().IsModuleLoaded(TEXT("ContentBrowser")))
	{
		FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		const FAssetData Data = Registry.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
		if (Data.IsValid())
		{
			FContentBrowserModule& Module = FModuleManager::GetModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
			TArray<FAssetData> Assets;
			Assets.Add(Data);
			Module.Get().SyncBrowserToAssets(Assets, /*bAllowLockedBrowsers*/ false, /*bFocusContentBrowser*/ true);
			return;
		}
	}

	// 资产注册表里还没有它（刚建、尚未登记）或内容浏览器没起来：退回跳目录，至少把人带到附近。
	ShowFolderInContentBrowser(FallbackFolder);
}

void FDreamTextureImportNotifier::ShowFolderInContentBrowser(const FString& Folder)
{
	if (Folder.IsEmpty() || !FModuleManager::Get().IsModuleLoaded(TEXT("ContentBrowser")))
	{
		return;
	}

	FContentBrowserModule& Module = FModuleManager::GetModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	TArray<FString> Folders;
	Folders.Add(Folder);
	Module.Get().SyncBrowserToFolders(Folders, /*bAllowLockedBrowsers*/ false, /*bFocusContentBrowser*/ true);
}

void FDreamTextureImportNotifier::Show(const FNoticeContent& Content)
{
	// 命令行/-nullrhi 的自动化里没有通知管理器。
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}

	FNotificationInfo Info(Content.Message);
	Info.bFireAndForget = true;
	Info.bUseSuccessFailIcons = true;
	// 失败的原因要读完，留得比成功的久。
	Info.ExpireDuration = Content.bIsError ? 10.0f : 8.0f;

	if (!Content.AssetToSelect.IsEmpty() || !Content.FolderToShow.IsEmpty())
	{
		const FString Folder = Content.FolderToShow;
		const FString Asset = Content.AssetToSelect;
		Info.HyperlinkText = LOCTEXT("ShowInContentBrowser", "Show in Content Browser");
		Info.Hyperlink = FSimpleDelegate::CreateLambda([Folder, Asset]()
		{
			if (!Asset.IsEmpty())
			{
				ShowAssetInContentBrowser(Asset, Folder);
			}
			else
			{
				ShowFolderInContentBrowser(Folder);
			}
		});
	}

	// 有材质实例就给一个「赋给选中物体」：导入完成的那一刻最想做的事就是看到场景变了。
	// 按钮的可点状态跟着选中实时变（没选中东西时灰掉，提示里说明），点下去先弹确认框，不闷头覆盖。
	if (!Content.AssetToSelect.IsEmpty())
	{
		const FString MaterialPath = Content.AssetToSelect;
		Info.ButtonDetails.Add(FNotificationButtonInfo(
			LOCTEXT("AssignToSelection", "Assign to Selection"),
			TAttribute<FText>::CreateLambda([]() { return FDreamTextureAssignProbe::Get(true, FText::GetEmpty()).Hint; }),
			FSimpleDelegate::CreateLambda([MaterialPath]() { DreamTextureAssignFlow::Run(MaterialPath); }),
			FNotificationButtonInfo::FVisibilityDelegate(),
			FNotificationButtonInfo::FIsEnabledDelegate::CreateLambda([](SNotificationItem::ECompletionState)
			{
				return FDreamTextureAssignProbe::Get(true, FText::GetEmpty()).bCanAssign;
			})));
	}

	if (TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
	{
		Item->SetCompletionState(Content.bIsError ? SNotificationItem::CS_Fail : SNotificationItem::CS_Success);
	}
}

#undef LOCTEXT_NAMESPACE
