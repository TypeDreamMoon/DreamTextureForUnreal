// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureBackendActions.h"

#include "DreamTextureClientModule.h"
#include "DreamTextureConnection.h"
#include "DreamTextureLog.h"
#include "DreamTextureProcess.h"
#include "DreamTextureSettings.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformProcess.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "DreamTextureBackendActions"

namespace DreamTextureBackendActions
{
	namespace
	{
		void Toast(const FText& Text, bool bSuccess)
		{
			FNotificationInfo Info(Text);
			Info.bFireAndForget = true;
			Info.bUseSuccessFailIcons = true;
			// 失败的原因是整句中文，读完要点时间。
			Info.ExpireDuration = bSuccess ? 4.0f : 10.0f;
			if (TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
			{
				Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
			}
		}
	}

	TSharedPtr<FDreamTextureProcess> GetProcess()
	{
		return FDreamTextureClientModule::IsAvailable() ? FDreamTextureClientModule::Get().GetProcess() : nullptr;
	}

	bool IsStarting()
	{
		const TSharedPtr<FDreamTextureProcess> Process = GetProcess();
		return Process.IsValid() && Process->IsStarting();
	}

	bool IsLaunchedByPlugin()
	{
		const TSharedPtr<FDreamTextureProcess> Process = GetProcess();
		return Process.IsValid() && Process->GetOrigin() == EDreamTextureBackendOrigin::Launched && Process->OwnsRunningProcess();
	}

	bool CanStart(FText* OutWhy)
	{
		auto Why = [OutWhy](const FText& Text) { if (OutWhy) { *OutWhy = Text; } return false; };

		const TSharedPtr<FDreamTextureProcess> Process = GetProcess();
		if (!Process.IsValid())
		{
			return Why(LOCTEXT("NoProcess", "The client module is not ready, so the backend process cannot be managed."));
		}
		if (Process->IsStarting())
		{
			return Why(LOCTEXT("Starting", "The backend is starting; waiting for it to respond."));
		}

		// 后端已经在应答就不用启动了。看的是连接的判断，和状态条上的「后端」标记同源。
		const TSharedPtr<FDreamTextureConnection> Conn = FDreamTextureClientModule::Get().GetConnection();
		if (Conn.IsValid() && Conn->IsBackendReachable())
		{
			return Why(LOCTEXT("AlreadyUp", "The backend is already responding, so there is nothing to start."));
		}

		if (OutWhy)
		{
			*OutWhy = UDreamTextureSettings::Get().BackendExecutable.FilePath.TrimStartAndEnd().IsEmpty()
				? LOCTEXT("StartTipNoExe", "No backend executable is configured yet. Clicking this tells you where to set it.")
				: LOCTEXT("StartTip", "Starts the backend using the 'Backend Executable' and 'Backend Arguments' project settings. If a backend is already listening on the port, the plugin attaches to it instead of starting another one.");
		}
		return true;
	}

	void Start()
	{
		const TSharedPtr<FDreamTextureProcess> Process = GetProcess();
		if (!Process.IsValid() || Process->IsStarting())
		{
			return;
		}

		Process->EnsureBackend([](bool bOK, const FString& Error)
		{
			if (!bOK)
			{
				Toast(FText::Format(LOCTEXT("StartFailed", "Could not start the backend: {0}"), FText::FromString(Error)), false);
				return;
			}

			const TSharedPtr<FDreamTextureProcess> P = GetProcess();
			const bool bLaunched = P.IsValid() && P->GetOrigin() == EDreamTextureBackendOrigin::Launched;
			Toast(bLaunched
				? LOCTEXT("StartedLaunched", "The backend has started.")
				: LOCTEXT("StartedAttached", "A backend was already running and the plugin has connected to it. The plugin did not start it, so it leaves it running when the editor exits."),
				true);
		});
	}

	void Stop()
	{
		const TSharedPtr<FDreamTextureProcess> Process = GetProcess();
		if (!Process.IsValid())
		{
			return;
		}
		if (Process->StopOwned())
		{
			Toast(LOCTEXT("Stopped", "Stopped the backend that the plugin started."), true);
		}
	}

	void OpenWebUI()
	{
		const FString URL = UDreamTextureSettings::Get().GetNormalizedBackendURL();

		FString LaunchError;
		FPlatformProcess::LaunchURL(*URL, nullptr, &LaunchError);
		if (!LaunchError.IsEmpty())
		{
			UE_LOG(LogDreamTexture, Warning, TEXT("Could not open the web UI: %s (%s)"), *LaunchError, *URL);
		}
	}

	FText DescribeOrigin()
	{
		const TSharedPtr<FDreamTextureProcess> Process = GetProcess();
		if (!Process.IsValid())
		{
			return FText::GetEmpty();
		}
		if (Process->IsStarting())
		{
			return LOCTEXT("DescStarting", "Starting the backend; waiting for it to respond...");
		}

		switch (Process->GetOrigin())
		{
		case EDreamTextureBackendOrigin::Launched:
			return FText::Format(LOCTEXT("DescLaunched", "Started by the plugin (pid {0}); it is stopped together with the editor."),
				FText::AsNumber(Process->GetOwnedProcessId()));
		case EDreamTextureBackendOrigin::Attached:
			return LOCTEXT("DescAttached", "Attached to an existing backend that the plugin did not start; it is left running when the editor exits.");
		default:
			return FText::GetEmpty();
		}
	}
}

#undef LOCTEXT_NAMESPACE
