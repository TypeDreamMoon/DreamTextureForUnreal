// Copyright TypeDreamMoon. All Rights Reserved.

#include "SDreamTextureConnectionStrip.h"

#include "DreamTextureBackendActions.h"
#include "DreamTextureClientModule.h"
#include "DreamTextureConnection.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "DreamTextureConnectionStrip"

TSharedPtr<FDreamTextureConnection> SDreamTextureConnectionStrip::GetConnection() const
{
	if (TSharedPtr<FDreamTextureConnection> Conn = ConnectionWeak.Pin())
	{
		return Conn;
	}

	// 连接可能是面板打开之后才建好的（客户端模块晚一步起来），所以丢了就再抓一次。
	// 客户端模块没加载时 GetConnection() 会炸，必须先问 IsAvailable()。
	if (FDreamTextureClientModule::IsAvailable())
	{
		// const 方法里更新缓存的弱指针，所以弱指针是 mutable 语义——这里借 const_cast，
		// 比为这一个缓存把所有取值函数都改成非 const 要省事。
		const_cast<SDreamTextureConnectionStrip*>(this)->ConnectionWeak = FDreamTextureClientModule::Get().GetConnection();
		return ConnectionWeak.Pin();
	}
	return nullptr;
}

//////////////////////////////////////////////////////////////////////////
// 取值：三级状态各算各的

namespace
{
	/**
	 * Offline 有两种含义：「还没开始连」和「连过又断了」。前者说成「未连接」会让人
	 * 以为坏了。连接动过（不是 Offline、后端应答过、或攒下了错误）才算有依据。
	 */
	bool IsStateKnown(const FDreamTextureConnection& Conn)
	{
		return Conn.GetLink() != EDreamTextureLink::Offline || Conn.IsBackendReachable() || !Conn.GetLastError().IsEmpty();
	}
}

SDreamTextureConnectionStrip::FMarker SDreamTextureConnectionStrip::EvaluateBackend(const TSharedPtr<FDreamTextureConnection>& Conn)
{
	FMarker Out;
	if (!Conn.IsValid())
	{
		Out.Tone = ETone::Unknown;
		Out.Value = LOCTEXT("Unknown", "Unknown");
		Out.Tooltip = LOCTEXT("BackendNoConn", "The client module is not ready, so there is no connection object. Click Reconnect on the Connection Status page to try again.");
		return Out;
	}

	if (Conn->IsBackendReachable())
	{
		Out.Tone = ETone::Ok;
		Out.Value = LOCTEXT("BackendOk", "Responding");
		Out.Tooltip = LOCTEXT("BackendOkTip", "Backend process: it has answered a REST request, so the process is alive.");

		// 谁起的：插件拉起的会在编辑器退出时停掉，附着的不会。关系到用户要不要自己去关，写进提示里。
		const FText Origin = DreamTextureBackendActions::DescribeOrigin();
		if (!Origin.IsEmpty())
		{
			Out.Tooltip = FText::Format(LOCTEXT("BackendOkOriginTip", "{0}\n{1}"), Out.Tooltip, Origin);
		}
		if (DreamTextureBackendActions::IsLaunchedByPlugin())
		{
			Out.Value = LOCTEXT("BackendOkLaunched", "Responding · started by the plugin");
		}
	}
	else if (DreamTextureBackendActions::IsStarting())
	{
		Out.Tone = ETone::Busy;
		Out.Value = LOCTEXT("BackendStarting", "Starting");
		Out.Tooltip = LOCTEXT("BackendStartingTip", "Backend process: the plugin is starting it and waiting for it to respond.");
	}
	else if (IsStateKnown(*Conn))
	{
		Out.Tone = ETone::Bad;
		Out.Value = LOCTEXT("BackendBad", "Not responding");
		Out.Tooltip = LOCTEXT("BackendBadTip", "The backend process is not responding. Click Start Backend on the right, or make sure dreamtexture.exe is running and check the address and port.");
	}
	else
	{
		Out.Tone = ETone::Busy;
		Out.Value = LOCTEXT("Connecting", "Connecting");
		Out.Tooltip = LOCTEXT("BackendBusyTip", "Establishing the connection.");
	}
	return Out;
}

SDreamTextureConnectionStrip::FMarker SDreamTextureConnectionStrip::EvaluateLink(const TSharedPtr<FDreamTextureConnection>& Conn)
{
	FMarker Out;
	if (!Conn.IsValid())
	{
		Out.Tone = ETone::Unknown;
		Out.Value = LOCTEXT("Unknown", "Unknown");
		return Out;
	}

	switch (Conn->GetLink())
	{
	case EDreamTextureLink::Live:
		Out.Tone = ETone::Ok;
		Out.Value = LOCTEXT("LinkLive", "Connected");
		Out.Tooltip = LOCTEXT("LinkLiveTip", "Event stream: the heartbeat is healthy. Material sets newly generated in the web UI appear in the library on their own.");
		break;
	case EDreamTextureLink::Connecting:
		Out.Tone = ETone::Busy;
		Out.Value = LOCTEXT("Connecting", "Connecting");
		Out.Tooltip = LOCTEXT("LinkConnectingTip", "Event stream: connecting, or waiting in the backoff for the next retry.");
		break;
	case EDreamTextureLink::Stale:
		Out.Tone = ETone::Warn;
		Out.Value = LOCTEXT("LinkStale", "Timed out");
		// Stale 单独一档就是为了这句话：界面上的数字已经是旧的，但用户什么都不用做。
		Out.Tooltip = LOCTEXT("LinkStaleTip", "Event stream: no message arrived before the timeout. It reconnects automatically, so there is nothing you need to do.");
		break;
	case EDreamTextureLink::Offline:
	default:
		if (IsStateKnown(*Conn))
		{
			Out.Tone = ETone::Bad;
			Out.Value = LOCTEXT("LinkOffline", "Disconnected");
			Out.Tooltip = LOCTEXT("LinkOfflineTip", "The event stream is down. It only carries status updates; every operation goes through REST.");
		}
		else
		{
			Out.Tone = ETone::Busy;
			Out.Value = LOCTEXT("Connecting", "Connecting");
			Out.Tooltip = LOCTEXT("LinkStartingTip", "Establishing the connection.");
		}
		break;
	}
	return Out;
}

SDreamTextureConnectionStrip::FMarker SDreamTextureConnectionStrip::EvaluateComfy(const TSharedPtr<FDreamTextureConnection>& Conn)
{
	FMarker Out;
	Out.Tone = ETone::Unknown;
	Out.Value = LOCTEXT("Unknown", "Unknown");

	if (!Conn.IsValid())
	{
		return Out;
	}

	// Health 是 WS 的 comfy.status 推来的：后端没应答、或链路已 Stale，它就是上一轮的残值。
	// 把残值报成「就绪」是在后端已经死了的时候给人一个绿灯，报成「未运行」又会让人
	// 去动一个可能好好活着的 ComfyUI，所以宁可退回「未知」。
	if (!Conn->IsBackendReachable())
	{
		Out.Tooltip = LOCTEXT("ComfyNoBackend", "ComfyUI: the backend is not responding, so its status cannot be known.");
		return Out;
	}
	if (Conn->GetLink() == EDreamTextureLink::Stale)
	{
		Out.Tooltip = LOCTEXT("ComfyStale", "ComfyUI: the event stream timed out, so the numbers shown here are out of date.");
		return Out;
	}

	const FDreamTextureHealth& Health = Conn->GetHealth();

	// bStarting 必须排在最前面：冷启动几十秒到几分钟是常态，这一档的意思是「等着，别动」。
	if (Health.bStarting)
	{
		Out.Tone = ETone::Busy;
		Out.Value = FText::Format(LOCTEXT("ComfyStarting", "Starting {0}s"), FText::AsNumber(Health.StartingSecs));
		Out.Tooltip = LOCTEXT("ComfyStartingTip", "ComfyUI: a cold start takes anywhere from tens of seconds to a few minutes. Just wait; restarting would only start over.");
	}
	else if (Health.bReady)
	{
		Out.Tone = ETone::Ok;
		Out.Value = LOCTEXT("ComfyReady", "Ready");
		Out.Tooltip = LOCTEXT("ComfyReadyTip", "ComfyUI: ready to take jobs.");
	}
	else if (Health.bUserStopped)
	{
		Out.Tone = ETone::Warn;
		Out.Value = LOCTEXT("ComfyStopped", "Stopped");
		Out.Tooltip = LOCTEXT("ComfyStoppedTip", "ComfyUI: it was stopped manually; it did not crash.");
	}
	else if (Health.bAlive)
	{
		Out.Tone = ETone::Warn;
		Out.Value = LOCTEXT("ComfyAlive", "Not ready");
		Out.Tooltip = Health.Reason.IsEmpty()
			? LOCTEXT("ComfyAliveTip", "ComfyUI: the process is running but cannot take jobs yet.")
			: FText::FromString(Health.Reason);
	}
	else
	{
		Out.Tone = ETone::Bad;
		Out.Value = LOCTEXT("ComfyDown", "Not running");
		Out.Tooltip = LOCTEXT("ComfyDownTip", "ComfyUI is not running. The backend itself is still alive, so do not restart the backend.");
	}
	return Out;
}

//////////////////////////////////////////////////////////////////////////
// 分档 → 画笔 / 颜色

const FSlateBrush* SDreamTextureConnectionStrip::ToneBrush(ETone InTone)
{
	// 形状各不相同。Icons.Warning 和 Icons.Error 在 Starship 里是同一张三角、只有
	// 颜色不同，灰度下分不出来，所以「坏了」用 Icons.X。
	switch (InTone)
	{
	case ETone::Ok:      return FAppStyle::GetBrush("Icons.Success");
	case ETone::Warn:    return FAppStyle::GetBrush("Icons.Warning");
	case ETone::Bad:     return FAppStyle::GetBrush("Icons.X");
	case ETone::Busy:    return FAppStyle::GetBrush("Icons.Refresh");
	case ETone::Unknown:
	default:             return FAppStyle::GetBrush("Icons.Help");
	}
}

FSlateColor SDreamTextureConnectionStrip::ToneColor(ETone InTone)
{
	switch (InTone)
	{
	case ETone::Ok:      return FStyleColors::Success;
	case ETone::Warn:    return FStyleColors::Warning;
	case ETone::Bad:     return FStyleColors::Error;
	case ETone::Busy:    return FStyleColors::AccentBlue;
	case ETone::Unknown:
	default:             return FStyleColors::AccentGray;
	}
}

//////////////////////////////////////////////////////////////////////////
// 构件

void SDreamTextureConnectionStrip::Construct(const FArguments& InArgs)
{
	OnClickedDelegate = InArgs._OnClicked;
	ConnectionWeak = FDreamTextureClientModule::IsAvailable()
		? FDreamTextureClientModule::Get().GetConnection()
		: nullptr;

	ChildSlot
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
		[
			MakeMarker(LOCTEXT("TitleBackend", "Backend"), EWhich::Backend)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
		[
			MakeMarker(LOCTEXT("TitleLink", "Event stream"), EWhich::Link)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
		[
			MakeMarker(LOCTEXT("TitleComfy", "ComfyUI"), EWhich::Comfy)
		]

		// 后端没应答又没在启动时才露出来；应答之后让位，不占状态条的宽度。
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(0.0f, 0.0f, 4.0f, 0.0f))
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ContentPadding(FMargin(4.0f, 2.0f))
			.Visibility(TAttribute<EVisibility>::CreateLambda([]()
			{
				return DreamTextureBackendActions::CanStart() ? EVisibility::Visible : EVisibility::Collapsed;
			}))
			.ToolTipText(TAttribute<FText>::CreateLambda([]()
			{
				FText Why;
				DreamTextureBackendActions::CanStart(&Why);
				return Why;
			}))
			.OnClicked_Lambda([]()
			{
				DreamTextureBackendActions::Start();
				return FReply::Handled();
			})
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 4.0f, 0.0f))
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.Play"))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("StartBackend", "Start Backend"))
				]
			]
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ContentPadding(FMargin(4.0f, 2.0f))
			.ToolTipText(LOCTEXT("OpenWebTip", "Open the web UI that ships with the backend in your default browser."))
			.OnClicked_Lambda([]()
			{
				DreamTextureBackendActions::OpenWebUI();
				return FReply::Handled();
			})
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 4.0f, 0.0f))
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.Link"))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("OpenWeb", "Web UI"))
				]
			]
		]
	];
}

SDreamTextureConnectionStrip::FMarker SDreamTextureConnectionStrip::Evaluate(EWhich InWhich) const
{
	const TSharedPtr<FDreamTextureConnection> Conn = GetConnection();
	switch (InWhich)
	{
	case EWhich::Backend: return EvaluateBackend(Conn);
	case EWhich::Link:    return EvaluateLink(Conn);
	case EWhich::Comfy:
	default:              return EvaluateComfy(Conn);
	}
}

TSharedRef<SWidget> SDreamTextureConnectionStrip::MakeMarker(const FText& InTitle, EWhich InWhich)
{
	// 弱捕获 this：lambda 活在这个 widget 自己的属性里，widget 没了它也跟着没了，
	// 但按 SharedThis 取弱指针更稳，不依赖这个推理。
	TWeakPtr<SDreamTextureConnectionStrip> WeakSelf = SharedThis(this);

	auto Eval = [WeakSelf, InWhich]() -> FMarker
	{
		if (TSharedPtr<SDreamTextureConnectionStrip> Self = WeakSelf.Pin())
		{
			return Self->Evaluate(InWhich);
		}
		return FMarker();
	};

	return SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "SimpleButton")
		.ContentPadding(FMargin(4.0f, 2.0f))
		.ToolTipText(TAttribute<FText>::CreateLambda([Eval]() { return Eval().Tooltip; }))
		.OnClicked_Lambda([WeakSelf]()
		{
			if (TSharedPtr<SDreamTextureConnectionStrip> Self = WeakSelf.Pin())
			{
				Self->OnClickedDelegate.ExecuteIfBound();
			}
			return FReply::Handled();
		})
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 4.0f, 0.0f))
			[
				SNew(SBox)
				.WidthOverride(14.0f)
				.HeightOverride(14.0f)
				[
					SNew(SOverlay)

					+ SOverlay::Slot()
					[
						SNew(SImage)
						.Image(TAttribute<const FSlateBrush*>::CreateLambda([Eval]() { return ToneBrush(Eval().Tone); }))
						.ColorAndOpacity(TAttribute<FSlateColor>::CreateLambda([Eval]() { return ToneColor(Eval().Tone); }))
						.Visibility(TAttribute<EVisibility>::CreateLambda([Eval]()
						{
							return Eval().Tone == ETone::Busy ? EVisibility::Collapsed : EVisibility::Visible;
						}))
					]

					+ SOverlay::Slot()
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					[
						// 「在路上，等着就行」只有动起来才表达得清楚：静态图标会被读成一个结论。
						SNew(SCircularThrobber)
						.Radius(7.0f)
						.NumPieces(6)
						.Period(0.9f)
						.ColorAndOpacity(FStyleColors::AccentBlue)
						.Visibility(TAttribute<EVisibility>::CreateLambda([Eval]()
						{
							return Eval().Tone == ETone::Busy ? EVisibility::Visible : EVisibility::Collapsed;
						}))
					]
				]
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 4.0f, 0.0f))
			[
				SNew(STextBlock)
				.Text(InTitle)
				.ColorAndOpacity(FStyleColors::Foreground)
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(TAttribute<FText>::CreateLambda([Eval]() { return Eval().Value; }))
				.Font(FAppStyle::Get().GetFontStyle("NormalFontBold"))
				.ColorAndOpacity(TAttribute<FSlateColor>::CreateLambda([Eval]() { return ToneColor(Eval().Tone); }))
			]
		];
}

#undef LOCTEXT_NAMESPACE
