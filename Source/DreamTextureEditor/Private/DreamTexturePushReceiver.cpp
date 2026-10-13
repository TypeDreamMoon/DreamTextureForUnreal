// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTexturePushReceiver.h"

#include "DreamTextureImportQueue.h"
#include "DreamTextureLog.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "DreamTexturePushReceiver"

FDreamTexturePushReceiver::FHooks FDreamTexturePushReceiver::FHooks::MakeDefault()
{
	FHooks Out;
	Out.IsActive = [](const FString& MaterialId)
	{
		return FDreamTextureImportQueue::Get().IsActive(MaterialId);
	};
	Out.Enqueue = [](const FString& MaterialId)
	{
		return FDreamTextureImportQueue::Get().Enqueue(MaterialId);
	};
	Out.ShowNotice = [](const FText& Message)
	{
		Show(Message);
	};
	return Out;
}

FDreamTexturePushReceiver::FDreamTexturePushReceiver(FHooks InHooks)
	: Hooks(MoveTemp(InHooks))
{
}

FDreamTexturePushReceiver::~FDreamTexturePushReceiver()
{
	Stop();
}

void FDreamTexturePushReceiver::Start(const TSharedPtr<FDreamTexturePushSession>& InSession)
{
	Stop();
	if (!InSession.IsValid())
	{
		return;
	}

	Session = InSession;
	Handle = InSession->OnImportPushed.AddSP(this, &FDreamTexturePushReceiver::HandlePushed);
}

void FDreamTexturePushReceiver::Stop()
{
	// 会话归客户端模块所有，可能比我们先没，所以先 Pin。
	if (const TSharedPtr<FDreamTexturePushSession> Pinned = Session.Pin())
	{
		Pinned->OnImportPushed.Remove(Handle);
	}
	Session.Reset();
	Handle.Reset();
}

void FDreamTexturePushReceiver::HandlePushed(const FString& MaterialId)
{
	++NumReceived;

	// 先看在不在队里再排：Enqueue 对在队的 id 返回 false，那和「排不进去」分不开，而通知里要如实说是哪一种。
	const bool bAlreadyActive = Hooks.IsActive && Hooks.IsActive(MaterialId);
	const bool bQueued = !bAlreadyActive && Hooks.Enqueue && Hooks.Enqueue(MaterialId);

	UE_LOG(LogDreamTexture, Log, TEXT("The web UI pushed material %s: %s"), *MaterialId,
		bQueued ? TEXT("queued for import") : (bAlreadyActive ? TEXT("already in the import queue, not queued again") : TEXT("could not be queued for import")));

	if (Hooks.ShowNotice)
	{
		Hooks.ShowNotice(Describe(MaterialId, bQueued || bAlreadyActive));
	}
}

FText FDreamTexturePushReceiver::Describe(const FString& MaterialId, bool bImporting)
{
	return bImporting
		? FText::Format(LOCTEXT("PushedQueued", "The web UI sent a material set: {0}. It is being imported, and you will be notified again when it finishes."), FText::FromString(MaterialId))
		: FText::Format(LOCTEXT("PushedNotQueued", "The web UI sent a material set: {0}, but it could not be queued for import. See the Output Log for the reason."), FText::FromString(MaterialId));
}

void FDreamTexturePushReceiver::Show(const FText& Message)
{
	// 命令行 / -nullrhi 的自动化里没有通知管理器。
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}

	FNotificationInfo Info(Message);
	Info.bFireAndForget = true;
	// 只说「收到了」，几秒就收；结果那条（导入完成 / 失败）才是要读的。
	Info.ExpireDuration = 5.0f;
	Info.FadeOutDuration = 0.5f;
	Info.bUseSuccessFailIcons = false;
	Info.bUseThrobber = false;

	if (TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
	{
		// 不打对勾：对勾会让人以为已经导完了。
		Item->SetCompletionState(SNotificationItem::CS_None);
	}
}

#undef LOCTEXT_NAMESPACE
