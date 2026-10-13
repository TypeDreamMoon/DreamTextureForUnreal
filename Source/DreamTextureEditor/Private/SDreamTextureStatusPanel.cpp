// Copyright TypeDreamMoon. All Rights Reserved.

#include "SDreamTextureStatusPanel.h"

#include "DreamTextureBackendActions.h"
#include "DreamTextureClientModule.h"
#include "DreamTextureConnection.h"
#include "DreamTextureLog.h"
#include "DreamTextureProcess.h"
#include "DreamTexturePushSession.h"
#include "DreamTextureSettings.h"
#include "HAL/PlatformProcess.h"
#include "ISettingsModule.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "DreamTextureStatusPanel"

namespace DreamTextureStatusPanel
{
	/** 三行状态左边那列标题的宽度。写死是为了三行的图标、标题、值都对齐成一条线。 */
	static constexpr float TitleColumnWidth = 124.0f;

	/** 值那一列的最小宽度，免得「就绪」和「运行中，未就绪」把后面的说明推来推去。 */
	static constexpr float ValueColumnWidth = 132.0f;
}

void SDreamTextureStatusPanel::Construct(const FArguments& InArgs)
{
	BindConnection();
	BindPush();

	ChildSlot
	[
		SNew(SVerticalBox)

		// ── 顶栏：地址 + 三个动作按钮 ─────────────────────────────
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
			.Padding(FMargin(12.0f, 8.0f))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.Server"))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(TAttribute<FText>(this, &SDreamTextureStatusPanel::GetBackendUrlText))
					.ToolTipText(LOCTEXT("BackendUrlTip", "Change it in Project Settings > Plugins > DreamTexture. After changing it, click Reconnect to apply it immediately instead of waiting for the backoff."))
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.HAlign(HAlign_Right)
				.VAlign(VAlign_Center)
				[
					MakeToolbar()
				]
			]
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SScrollBox)

			// ── 三级状态。分三行是这个面板存在的理由，见头文件注释。 ──
			+ SScrollBox::Slot()
			.Padding(FMargin(12.0f, 12.0f, 12.0f, 0.0f))
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
				.Padding(FMargin(12.0f))
				[
					SNew(SVerticalBox)

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.0f, 0.0f, 0.0f, 6.0f))
					[
						MakeStatusRow(
							LOCTEXT("RowBackend", "Backend process"),
							TAttribute<EStatusTone>(this, &SDreamTextureStatusPanel::GetBackendTone),
							TAttribute<FText>(this, &SDreamTextureStatusPanel::GetBackendText),
							TAttribute<FText>(this, &SDreamTextureStatusPanel::GetBackendNote))
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.0f, 0.0f, 0.0f, 6.0f))
					[
						MakeStatusRow(
							LOCTEXT("RowLink", "Event stream"),
							TAttribute<EStatusTone>(this, &SDreamTextureStatusPanel::GetLinkTone),
							TAttribute<FText>(this, &SDreamTextureStatusPanel::GetLinkText),
							TAttribute<FText>(this, &SDreamTextureStatusPanel::GetLinkNote))
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.0f, 0.0f, 0.0f, 6.0f))
					[
						MakeStatusRow(
							LOCTEXT("RowComfy", "ComfyUI"),
							TAttribute<EStatusTone>(this, &SDreamTextureStatusPanel::GetComfyTone),
							TAttribute<FText>(this, &SDreamTextureStatusPanel::GetComfyText),
							TAttribute<FText>(this, &SDreamTextureStatusPanel::GetComfyNote))
					]

					// 第四行不属于「三级状态」那条因果链（后端 → 长连接 → ComfyUI），是它们之上的一个功能：
					// Web 端点「发送到 UE」，材质能不能出现在这个编辑器里。单独一行，别混进上面三级里。
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						MakeStatusRow(
							LOCTEXT("RowPush", "Push"),
							TAttribute<EStatusTone>(this, &SDreamTextureStatusPanel::GetPushTone),
							TAttribute<FText>(this, &SDreamTextureStatusPanel::GetPushText),
							TAttribute<FText>(this, &SDreamTextureStatusPanel::GetPushNote))
					]
				]
			]

			// ── 最近一次失败的原因。后端给的已经是中文，原样显示，不再加工。 ──
			+ SScrollBox::Slot()
			.Padding(FMargin(12.0f, 8.0f, 12.0f, 0.0f))
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
				.Padding(FMargin(12.0f))
				.Visibility(TAttribute<EVisibility>(this, &SDreamTextureStatusPanel::GetErrorVisibility))
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Top)
					.Padding(FMargin(0.0f, 1.0f, 8.0f, 0.0f))
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.X"))
						.ColorAndOpacity(FStyleColors::Error)
					]

					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNew(STextBlock)
						.Text(TAttribute<FText>(this, &SDreamTextureStatusPanel::GetErrorText))
						.ColorAndOpacity(FStyleColors::Error)
						.AutoWrapText(true)
					]
				]
			]

			// ── ComfyUI 明细。数据一不新鲜就整块收起，理由见 IsHealthFresh()。 ──
			+ SScrollBox::Slot()
			.Padding(FMargin(12.0f, 8.0f, 12.0f, 0.0f))
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
				.Padding(FMargin(12.0f))
				.Visibility(TAttribute<EVisibility>(this, &SDreamTextureStatusPanel::GetDetailVisibility))
				[
					SNew(STextBlock)
					.Text(TAttribute<FText>(this, &SDreamTextureStatusPanel::GetDetailText))
					.AutoWrapText(true)
				]
			]

			// ── 环境自检 ──
			+ SScrollBox::Slot()
			.Padding(FMargin(12.0f, 8.0f, 12.0f, 12.0f))
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
				.Padding(FMargin(12.0f))
				[
					SNew(SVerticalBox)

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(SButton)
							.ContentPadding(FMargin(8.0f, 2.0f))
							.IsEnabled(TAttribute<bool>(this, &SDreamTextureStatusPanel::IsChecksButtonEnabled))
							.ToolTipText(LOCTEXT("ChecksTip", "Calls /api/checks once to run through the nodes, models, VRAM and output folder."))
							.OnClicked(this, &SDreamTextureStatusPanel::OnRunChecksClicked)
							[
								SNew(STextBlock)
								.Text(TAttribute<FText>(this, &SDreamTextureStatusPanel::GetChecksButtonText))
							]
						]
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.0f, 8.0f, 0.0f, 0.0f))
					[
						SAssignNew(ChecksHost, SBox)
					]
				]
			]
		]
	];

	RebuildChecks();
}

SDreamTextureStatusPanel::~SDreamTextureStatusPanel()
{
	UnbindPush();
	UnbindConnection();
}

//////////////////////////////////////////////////////////////////////////
// 订阅

void SDreamTextureStatusPanel::BindConnection()
{
	if (ConnectionWeak.IsValid())
	{
		return;
	}

	// 客户端模块可能压根没加载（打包成 commandlet、或者它自己启动失败）。
	// 这不是崩溃的理由，界面照常显示，只是三行状态全是「未知」。
	if (!FDreamTextureClientModule::IsAvailable())
	{
		return;
	}

	TSharedPtr<FDreamTextureConnection> Conn = FDreamTextureClientModule::Get().GetConnection();
	if (!Conn.IsValid())
	{
		// 模块在，连接是空的：模块正在关，或者还没建好。头文件明确说了要判空。
		return;
	}

	ConnectionWeak = Conn;
	LinkChangedHandle = Conn->OnLinkChanged.AddSP(this, &SDreamTextureStatusPanel::HandleLinkChanged);
	HealthChangedHandle = Conn->OnHealthChanged.AddSP(this, &SDreamTextureStatusPanel::HandleHealthChanged);

	// Start() 按契约是幂等的（DreamTextureConnection.h：「重复调用无副作用」）。
	// 这里补一刀是为了不让面板上那句「连接中」说谎——万一没人调过 Start()，
	// 显示「连接中」却根本没人在连，比老老实实显示「未连接」还糟。
	Conn->Start();

	RefreshFromConnection();
}

void SDreamTextureStatusPanel::UnbindConnection()
{
	if (TSharedPtr<FDreamTextureConnection> Conn = ConnectionWeak.Pin())
	{
		// AddSP 绑的是弱引用，漏摘不会留下悬空指针；但委托列表里的死条目要等到
		// 下一次广播才被清掉。面板是 nomad tab，开开关关很频繁，攒着不好看也没必要。
		Conn->OnLinkChanged.Remove(LinkChangedHandle);
		Conn->OnHealthChanged.Remove(HealthChangedHandle);
	}

	LinkChangedHandle.Reset();
	HealthChangedHandle.Reset();
	ConnectionWeak.Reset();
}

void SDreamTextureStatusPanel::HandleLinkChanged(EDreamTextureLink NewLink)
{
	bStateKnown = true;
	RefreshFromConnection();
}

void SDreamTextureStatusPanel::HandleHealthChanged(const FDreamTextureHealth& InHealth)
{
	bStateKnown = true;
	RefreshFromConnection();
}

void SDreamTextureStatusPanel::RefreshFromConnection()
{
	TSharedPtr<FDreamTextureConnection> Conn = ConnectionWeak.Pin();
	if (!Conn.IsValid())
	{
		return;
	}

	// 两个回调都只是「有事发生了」的信号，真正的值一律从连接上重新读一遍。
	// 这样界面的四个字段永远是同一时刻的快照，不会出现 Link 已经更新、
	// LastError 还是上一轮的错位。
	CachedLink = Conn->GetLink();
	CachedHealth = Conn->GetHealth();
	bCachedBackendReachable = Conn->IsBackendReachable();
	CachedLastError = Conn->GetLastError();

	// Offline 有两种含义：「还没开始连」和「连过又断了」。前者说成「未连接」
	// 会让人以为坏了。只要连接动过（不是 Offline、后端应答过、或者攒下了错误），
	// 就认为后面的状态可信。
	if (CachedLink != EDreamTextureLink::Offline || bCachedBackendReachable || !CachedLastError.IsEmpty())
	{
		bStateKnown = true;
	}
}

void SDreamTextureStatusPanel::BindPush()
{
	if (PushWeak.IsValid() || !FDreamTextureClientModule::IsAvailable())
	{
		return;
	}

	const TSharedPtr<FDreamTexturePushSession> Push = FDreamTextureClientModule::Get().GetPushSession();
	if (!Push.IsValid())
	{
		return;
	}

	PushWeak = Push;
	PushChangedHandle = Push->OnStateChanged.AddSP(this, &SDreamTextureStatusPanel::HandlePushChanged);
	RefreshFromPush();
}

void SDreamTextureStatusPanel::UnbindPush()
{
	if (const TSharedPtr<FDreamTexturePushSession> Push = PushWeak.Pin())
	{
		Push->OnStateChanged.Remove(PushChangedHandle);
	}
	PushChangedHandle.Reset();
	PushWeak.Reset();
}

void SDreamTextureStatusPanel::HandlePushChanged()
{
	RefreshFromPush();
}

void SDreamTextureStatusPanel::RefreshFromPush()
{
	const TSharedPtr<FDreamTexturePushSession> Push = PushWeak.Pin();
	if (!Push.IsValid())
	{
		return;
	}

	CachedPushState = Push->GetState();
	CachedPushName = Push->GetSessionName();
	CachedPushId = Push->GetSessionId();
	CachedPushError = Push->GetLastError();
}

bool SDreamTextureStatusPanel::HasConnection() const
{
	return ConnectionWeak.IsValid();
}

//////////////////////////////////////////////////////////////////////////
// 构件

TSharedRef<SWidget> SDreamTextureStatusPanel::MakeToneIcon(TAttribute<EStatusTone> InTone)
{
	return SNew(SBox)
		.WidthOverride(16.0f)
		.HeightOverride(16.0f)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SOverlay)

			+ SOverlay::Slot()
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(SImage)
				.Image(TAttribute<const FSlateBrush*>::CreateLambda([InTone]()
				{
					return SDreamTextureStatusPanel::ToneBrush(InTone.Get());
				}))
				.ColorAndOpacity(TAttribute<FSlateColor>::CreateLambda([InTone]()
				{
					return SDreamTextureStatusPanel::ToneColor(InTone.Get());
				}))
				.Visibility(TAttribute<EVisibility>::CreateLambda([InTone]()
				{
					return InTone.Get() == EStatusTone::Busy ? EVisibility::Collapsed : EVisibility::Visible;
				}))
			]

			+ SOverlay::Slot()
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				// 「在路上，等着就行」只有动起来才表达得清楚：静态图标不管画成什么样，
				// 都会被读成一个结论，而这一档恰恰是「还没有结论」。
				SNew(SCircularThrobber)
				.Radius(8.0f)
				.NumPieces(6)
				.Period(0.9f)
				.ColorAndOpacity(FStyleColors::AccentBlue)
				.Visibility(TAttribute<EVisibility>::CreateLambda([InTone]()
				{
					return InTone.Get() == EStatusTone::Busy ? EVisibility::Visible : EVisibility::Collapsed;
				}))
			]
		];
}

TSharedRef<SWidget> SDreamTextureStatusPanel::MakeStatusRow(const FText& InTitle, TAttribute<EStatusTone> InTone,
	TAttribute<FText> InValue, TAttribute<FText> InNote)
{
	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
		[
			MakeToneIcon(InTone)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(DreamTextureStatusPanel::TitleColumnWidth)
			[
				SNew(STextBlock)
				.Text(InTitle)
				.ColorAndOpacity(FStyleColors::Foreground)
			]
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(0.0f, 0.0f, 12.0f, 0.0f))
		[
			SNew(SBox)
			.MinDesiredWidth(DreamTextureStatusPanel::ValueColumnWidth)
			[
				// 颜色是第三重冗余：图标形状 + 这句中文已经把状态说全了，
				// 色觉障碍或者被压过的截图里也读得出来。
				SNew(STextBlock)
				.Text(InValue)
				.Font(FAppStyle::Get().GetFontStyle("NormalFontBold"))
				.ColorAndOpacity(TAttribute<FSlateColor>::CreateLambda([InTone]()
				{
					return SDreamTextureStatusPanel::ToneColor(InTone.Get());
				}))
			]
		]

		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(InNote)
			.ColorAndOpacity(FStyleColors::Foreground)
			.AutoWrapText(true)
		];
}

TSharedRef<SWidget> SDreamTextureStatusPanel::MakeToolbar()
{
	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(0.0f, 0.0f, 4.0f, 0.0f))
		[
			// 故意不跟着「有没有连接」变灰：客户端模块没就绪的时候，
			// 这个按钮正是用来再抓一次连接的。
			SNew(SButton)
			.ContentPadding(FMargin(8.0f, 2.0f))
			.ToolTipText(LOCTEXT("ReconnectTip", "Retry right now instead of waiting for the backoff. Also click it after changing the backend address."))
			.OnClicked(this, &SDreamTextureStatusPanel::OnReconnectClicked)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 4.0f, 0.0f))
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.Refresh"))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Reconnect", "Reconnect"))
				]
			]
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(0.0f, 0.0f, 4.0f, 0.0f))
		[
			SNew(SButton)
			.ContentPadding(FMargin(8.0f, 2.0f))
			.IsEnabled(TAttribute<bool>(this, &SDreamTextureStatusPanel::IsStartEnabled))
			.ToolTipText(TAttribute<FText>(this, &SDreamTextureStatusPanel::GetStartTooltip))
			.OnClicked(this, &SDreamTextureStatusPanel::OnStartBackendClicked)
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
					.Text(TAttribute<FText>(this, &SDreamTextureStatusPanel::GetStartButtonText))
				]
			]
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(0.0f, 0.0f, 4.0f, 0.0f))
		[
			SNew(SButton)
			.ContentPadding(FMargin(8.0f, 2.0f))
			.Visibility(TAttribute<EVisibility>(this, &SDreamTextureStatusPanel::GetStopVisibility))
			.ToolTipText(LOCTEXT("StopBackendTip", "Stops the backend that the plugin started, along with the ComfyUI it manages. A backend that was already running and that the plugin only attached to is left alone."))
			.OnClicked(this, &SDreamTextureStatusPanel::OnStopBackendClicked)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 4.0f, 0.0f))
				[
					SNew(SImage)
					// Starship 只有 20px 的 Toolbar.Stop，压到和别的按钮图标一样的 16px。
					.Image(FAppStyle::GetBrush("Icons.Toolbar.Stop"))
					.DesiredSizeOverride(FVector2D(16.0f, 16.0f))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("StopBackend", "Stop Backend"))
				]
			]
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(0.0f, 0.0f, 4.0f, 0.0f))
		[
			SNew(SButton)
			.ContentPadding(FMargin(8.0f, 2.0f))
			.ToolTipText(LOCTEXT("OpenWebTip", "Open the web UI that ships with the backend in your default browser."))
			.OnClicked(this, &SDreamTextureStatusPanel::OnOpenWebClicked)
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
					.Text(LOCTEXT("OpenWeb", "Open Web UI"))
				]
			]
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(SButton)
			.ContentPadding(FMargin(8.0f, 2.0f))
			.ToolTipText(LOCTEXT("SettingsTip", "Jump to Project Settings > Plugins > DreamTexture."))
			.OnClicked(this, &SDreamTextureStatusPanel::OnSettingsClicked)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 4.0f, 0.0f))
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.Settings"))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Settings", "Settings"))
				]
			]
		];
}

//////////////////////////////////////////////////////////////////////////
// 第一级：后端进程在不在

SDreamTextureStatusPanel::EStatusTone SDreamTextureStatusPanel::GetBackendTone() const
{
	if (!HasConnection())
	{
		return EStatusTone::Unknown;
	}
	if (bCachedBackendReachable)
	{
		return EStatusTone::Ok;
	}
	// 插件正在拉起后端：等着就行，别报红。
	if (DreamTextureBackendActions::IsStarting())
	{
		return EStatusTone::Busy;
	}
	return bStateKnown ? EStatusTone::Bad : EStatusTone::Busy;
}

FText SDreamTextureStatusPanel::GetBackendText() const
{
	switch (GetBackendTone())
	{
	case EStatusTone::Ok:      return LOCTEXT("BackendOk", "Responding");
	case EStatusTone::Busy:
		return DreamTextureBackendActions::IsStarting() ? LOCTEXT("BackendStarting", "Starting") : LOCTEXT("BackendBusy", "Connecting");
	case EStatusTone::Bad:     return LOCTEXT("BackendBad", "Not responding");
	default:                   return LOCTEXT("BackendUnknown", "Unknown");
	}
}

FText SDreamTextureStatusPanel::GetBackendNote() const
{
	if (!HasConnection())
	{
		return LOCTEXT("BackendNoteNoModule", "The client module is not ready, so there is no connection object. Click Reconnect to try again.");
	}

	// 谁起的这个后端，单独说一句：关系到编辑器退出时会不会把它停掉。
	const FText Origin = DreamTextureBackendActions::DescribeOrigin();
	const FText Separator = LOCTEXT("PartSeparator", " ");

	switch (GetBackendTone())
	{
	case EStatusTone::Ok:
		return Origin.IsEmpty()
			? LOCTEXT("BackendNoteOk", "A REST request has been answered, so the backend process is alive.")
			: FText::Join(Separator, LOCTEXT("BackendNoteOk", "A REST request has been answered, so the backend process is alive."), Origin);
	case EStatusTone::Busy:
		return Origin.IsEmpty() ? LOCTEXT("BackendNoteBusy", "Establishing the connection.") : Origin;
	default:
		return LOCTEXT("BackendNoteBad", "The backend is not responding. Click Start Backend above, or make sure dreamtexture.exe is running and check the address and port.");
	}
}

//////////////////////////////////////////////////////////////////////////
// 第二级：WS 长连接

SDreamTextureStatusPanel::EStatusTone SDreamTextureStatusPanel::GetLinkTone() const
{
	if (!HasConnection())
	{
		return EStatusTone::Unknown;
	}

	switch (CachedLink)
	{
	case EDreamTextureLink::Live:       return EStatusTone::Ok;
	case EDreamTextureLink::Connecting: return EStatusTone::Busy;
	case EDreamTextureLink::Stale:      return EStatusTone::Warn;
	case EDreamTextureLink::Offline:
	default:
		return bStateKnown ? EStatusTone::Bad : EStatusTone::Busy;
	}
}

FText SDreamTextureStatusPanel::GetLinkText() const
{
	if (!HasConnection())
	{
		return LOCTEXT("LinkUnknown", "Unknown");
	}

	switch (CachedLink)
	{
	case EDreamTextureLink::Live:       return LOCTEXT("LinkLive", "Connected");
	case EDreamTextureLink::Connecting: return LOCTEXT("LinkConnecting", "Connecting");
	case EDreamTextureLink::Stale:      return LOCTEXT("LinkStale", "Timed out");
	case EDreamTextureLink::Offline:
	default:
		return bStateKnown ? LOCTEXT("LinkOffline", "Disconnected") : LOCTEXT("LinkStarting", "Connecting");
	}
}

FText SDreamTextureStatusPanel::GetLinkNote() const
{
	if (!HasConnection())
	{
		return FText::GetEmpty();
	}

	switch (CachedLink)
	{
	case EDreamTextureLink::Live:
		return LOCTEXT("LinkNoteLive", "The heartbeat is healthy; the backend sends one frame every 15 seconds.");
	case EDreamTextureLink::Connecting:
		return LOCTEXT("LinkNoteConnecting", "Connecting, or waiting in the backoff for the next retry.");
	case EDreamTextureLink::Stale:
		// Stale 单独一档就是为了这句话：界面上的数字已经是旧的，但用户什么都不用做。
		return LOCTEXT("LinkNoteStale", "No message arrived before the timeout, so the numbers above are out of date. It reconnects automatically, so there is nothing you need to do.");
	case EDreamTextureLink::Offline:
	default:
		return bStateKnown
			? LOCTEXT("LinkNoteOffline", "The event stream is down. It only carries status updates; every operation goes through REST.")
			: LOCTEXT("LinkNoteStarting", "Establishing the connection.");
	}
}

//////////////////////////////////////////////////////////////////////////
// 第三级：ComfyUI

bool SDreamTextureStatusPanel::IsHealthFresh() const
{
	// 后端没应答，或者长连接已经失联——两种情况下 Health 都是上一轮的残值。
	// 把残值当真报成「未运行」，会让人去启动一个可能本来就好好活着的 ComfyUI；
	// 报成「就绪」更糟，那是在后端已经死了的时候给人一个绿灯。
	return HasConnection() && bCachedBackendReachable && CachedLink != EDreamTextureLink::Stale;
}

SDreamTextureStatusPanel::EStatusTone SDreamTextureStatusPanel::GetComfyTone() const
{
	if (!IsHealthFresh())
	{
		return EStatusTone::Unknown;
	}

	// bStarting 必须排在最前面：冷启动几十秒到几分钟是常态，这一档的意思是
	// 「等着，别动」，落进任何一个别的档都会让人去点重启。
	if (CachedHealth.bStarting)
	{
		return EStatusTone::Busy;
	}
	if (CachedHealth.bReady)
	{
		return EStatusTone::Ok;
	}
	if (CachedHealth.bUserStopped || CachedHealth.bAlive)
	{
		// 「自己停的」和「活着但没就绪」都不是故障，别报红。
		return EStatusTone::Warn;
	}
	return EStatusTone::Bad;
}

FText SDreamTextureStatusPanel::GetComfyText() const
{
	if (!IsHealthFresh())
	{
		return LOCTEXT("ComfyUnknown", "Unknown");
	}
	return GetComfyStateText();
}

FText SDreamTextureStatusPanel::GetComfyStateText() const
{
	if (CachedHealth.bStarting)
	{
		// 秒数是后端算的，不在本地掐表：面板可能刚打开，也可能刚从 Stale 恢复。
		return FText::Format(LOCTEXT("ComfyStartingFmt", "Starting ({0}s)"),
			FText::AsNumber(CachedHealth.StartingSecs));
	}
	if (CachedHealth.bReady)
	{
		return LOCTEXT("ComfyReady", "Ready");
	}
	if (CachedHealth.bUserStopped)
	{
		return LOCTEXT("ComfyUserStopped", "Stopped");
	}
	if (CachedHealth.bAlive)
	{
		return LOCTEXT("ComfyAlive", "Running, not ready");
	}
	return LOCTEXT("ComfyDown", "Not running");
}

FText SDreamTextureStatusPanel::GetComfyNote() const
{
	if (!HasConnection())
	{
		return FText::GetEmpty();
	}
	if (!bCachedBackendReachable)
	{
		return LOCTEXT("ComfyNoteNoBackend", "The backend is not responding, so the status of ComfyUI cannot be known. Fix the row above first.");
	}
	if (CachedLink == EDreamTextureLink::Stale)
	{
		// 信息不丢：状态退回「未知」，但把失联前最后一次的读数留在说明里，
		// 排查的时候还用得上。
		return FText::Format(
			LOCTEXT("ComfyNoteStaleFmt", "The event stream timed out, so this shows the state from before it was lost. The last reading was '{0}'."),
			GetComfyStateText());
	}

	TArray<FText> Parts;

	if (CachedHealth.bStarting)
	{
		Parts.Add(LOCTEXT("ComfyNoteStarting", "A cold start takes anywhere from tens of seconds to a few minutes, and a custom node's first run also installs its dependencies. Just wait; restarting would only start over."));
	}
	else if (CachedHealth.bUserStopped)
	{
		Parts.Add(LOCTEXT("ComfyNoteUserStopped", "It was stopped manually; it did not crash."));
	}
	else if (!CachedHealth.bReady && !CachedHealth.bAlive)
	{
		Parts.Add(LOCTEXT("ComfyNoteDown", "ComfyUI is not running. The backend itself is still alive, so do not restart the backend."));
	}

	// Reason 是后端给的中文，直接显示。
	if (!CachedHealth.Reason.IsEmpty())
	{
		Parts.Add(FText::FromString(CachedHealth.Reason));
	}

	// attach 模式下后端不负责启动 ComfyUI，「没起来」得自己去起——这个区别不说清楚会白等。
	if (CachedHealth.Mode.Equals(TEXT("attach"), ESearchCase::IgnoreCase))
	{
		Parts.Add(LOCTEXT("ComfyNoteAttach", "(Attach mode: the backend does not start ComfyUI, so you have to start it yourself.)"));
	}

	return FText::Join(LOCTEXT("PartSeparator", " "), Parts);
}

//////////////////////////////////////////////////////////////////////////
// 推送（M6）

SDreamTextureStatusPanel::EStatusTone SDreamTextureStatusPanel::GetPushTone() const
{
	if (!PushWeak.IsValid())
	{
		return EStatusTone::Unknown;
	}

	switch (CachedPushState)
	{
	case EDreamTexturePushState::Registered:   return EStatusTone::Ok;
	case EDreamTexturePushState::Registering:  return EStatusTone::Busy;
	case EDreamTexturePushState::Unsupported:  return EStatusTone::Unknown;
	case EDreamTexturePushState::Unregistered:
	default:
		// 长连接没通时「未注册」是预期之内的，不报警；链路通了还没注册上才是该看一眼的事。
		return CachedLink == EDreamTextureLink::Live && !CachedPushError.IsEmpty() ? EStatusTone::Warn : EStatusTone::Unknown;
	}
}

FText SDreamTextureStatusPanel::GetPushText() const
{
	if (!PushWeak.IsValid())
	{
		return LOCTEXT("PushUnknown", "Unknown");
	}

	switch (CachedPushState)
	{
	case EDreamTexturePushState::Registered:   return LOCTEXT("PushRegistered", "Registered");
	case EDreamTexturePushState::Registering:  return LOCTEXT("PushRegistering", "Registering");
	case EDreamTexturePushState::Unsupported:  return LOCTEXT("PushUnsupported", "Not supported by backend");
	case EDreamTexturePushState::Unregistered:
	default:
		return LOCTEXT("PushUnregistered", "Not registered");
	}
}

FText SDreamTextureStatusPanel::GetPushNote() const
{
	if (!PushWeak.IsValid())
	{
		return LOCTEXT("PushNoteNoModule", "The client module is not ready, so there is no push session.");
	}

	switch (CachedPushState)
	{
	case EDreamTexturePushState::Registered:
		return FText::Format(LOCTEXT("PushNoteRegistered", "Session '{0}' ({1}). \"Send to UE\" on a material set's detail page in the web UI lists this editor; click it and the material set arrives here."),
			FText::FromString(CachedPushName), FText::FromString(CachedPushId));
	case EDreamTexturePushState::Registering:
		return LOCTEXT("PushNoteRegistering", "Registering this editor with the backend.");
	case EDreamTexturePushState::Unsupported:
		return LOCTEXT("PushNoteUnsupported", "This backend does not have the API that Send to UE needs (it is an older version). Everything else is unaffected. Upgrade the backend and click Reconnect, and the editor registers automatically.");
	case EDreamTexturePushState::Unregistered:
	default:
		if (CachedLink != EDreamTextureLink::Live)
		{
			return LOCTEXT("PushNoteNoLink", "The event stream is not up yet; the editor registers automatically once it connects.");
		}
		return CachedPushError.IsEmpty()
			? LOCTEXT("PushNoteUnregistered", "The event stream is up; registering shortly.")
			: FText::Format(LOCTEXT("PushNoteFailed", "The last registration did not succeed: {0}. It retries automatically every 15 seconds."), FText::FromString(CachedPushError));
	}
}

//////////////////////////////////////////////////////////////////////////
// 其它显示项

FText SDreamTextureStatusPanel::GetBackendUrlText() const
{
	// 每次读设置而不是缓存：用户可能刚在项目设置里改完地址就回来看这里。
	return FText::FromString(UDreamTextureSettings::Get().GetNormalizedBackendURL());
}

EVisibility SDreamTextureStatusPanel::GetDetailVisibility() const
{
	// 数据一不新鲜就整块收起。Web 端踩过这个坑：后端被杀干净了，页面照旧显示
	// 「ComfyUI 就绪、显存 14.8G」——一份带小数点的过期数字比空白更能骗人。
	return IsHealthFresh() ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SDreamTextureStatusPanel::GetDetailText() const
{
	TArray<FText> Lines;

	{
		FFormatNamedArguments Args;
		Args.Add(TEXT("Mode"), CachedHealth.Mode.IsEmpty()
			? LOCTEXT("ValueUnknown", "Unknown")
			: FText::FromString(CachedHealth.Mode));
		Args.Add(TEXT("Version"), CachedHealth.ComfyUIVersion.IsEmpty()
			? LOCTEXT("ValueUnknown2", "Unknown")
			: FText::FromString(CachedHealth.ComfyUIVersion));
		Args.Add(TEXT("Device"), CachedHealth.Device.IsEmpty()
			? LOCTEXT("ValueUnknown3", "Unknown")
			: FText::FromString(CachedHealth.Device));
		Lines.Add(FText::Format(LOCTEXT("DetailLine1", "Mode {Mode} · Version {Version} · Device {Device}"), Args));
	}

	if (CachedHealth.VRAMTotalMB > 0)
	{
		FFormatNamedArguments Args;
		Args.Add(TEXT("Free"), FText::FromString(FString::Printf(TEXT("%.1f"), CachedHealth.VRAMFreeMB / 1024.0)));
		Args.Add(TEXT("Total"), FText::FromString(FString::Printf(TEXT("%.1f"), CachedHealth.VRAMTotalMB / 1024.0)));
		Lines.Add(FText::Format(LOCTEXT("DetailVRAM", "VRAM: {Free} GB free of {Total} GB"), Args));
	}

	{
		FFormatNamedArguments Args;
		Args.Add(TEXT("Queue"), FText::AsNumber(CachedHealth.QueueDepth));
		Args.Add(TEXT("Restarts"), FText::AsNumber(CachedHealth.Restarts));
		Lines.Add(FText::Format(LOCTEXT("DetailQueue", "Queue {Queue} · Restarts {Restarts}"), Args));
	}

	return FText::Join(FText::FromString(TEXT("\n")), Lines);
}

EVisibility SDreamTextureStatusPanel::GetErrorVisibility() const
{
	return GetShownError().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
}

FText SDreamTextureStatusPanel::GetErrorText() const
{
	// LastError 后端那边已经是中文了，不再翻译不再包装。
	return FText::FromString(GetShownError());
}

FString SDreamTextureStatusPanel::GetShownError() const
{
	// 启动后端失败（没配路径、进程退出、等不到应答）比「连不上」更具体，也更可操作，优先显示。
	// 后端一应答，进程管理器会清掉它自己的错误，这里自然回到连接的错误。
	if (const TSharedPtr<FDreamTextureProcess> Process = DreamTextureBackendActions::GetProcess())
	{
		if (!Process->GetLastError().IsEmpty())
		{
			return Process->GetLastError();
		}
	}
	return CachedLastError;
}

FText SDreamTextureStatusPanel::GetChecksButtonText() const
{
	return bChecksInFlight ? LOCTEXT("ChecksRunningBtn", "Checking...") : LOCTEXT("ChecksBtn", "Environment check");
}

bool SDreamTextureStatusPanel::IsChecksButtonEnabled() const
{
	return HasConnection() && !bChecksInFlight;
}

//////////////////////////////////////////////////////////////////////////
// 按钮

FReply SDreamTextureStatusPanel::OnReconnectClicked()
{
	// 连接可能是在面板打开之后才建好的（客户端模块晚一步启动），
	// 所以每次都先补一次订阅——这也是这个按钮不跟着变灰的原因。
	BindConnection();

	if (TSharedPtr<FDreamTextureConnection> Conn = ConnectionWeak.Pin())
	{
		Conn->Start();
		Conn->Reconnect();
		RefreshFromConnection();
	}

	return FReply::Handled();
}

FReply SDreamTextureStatusPanel::OnOpenWebClicked()
{
	DreamTextureBackendActions::OpenWebUI();
	return FReply::Handled();
}

FReply SDreamTextureStatusPanel::OnStartBackendClicked()
{
	// 连接可能是面板打开之后才建好的；起来之后连接要能立刻去连，所以先补订阅。
	BindConnection();
	DreamTextureBackendActions::Start();
	return FReply::Handled();
}

FReply SDreamTextureStatusPanel::OnStopBackendClicked()
{
	DreamTextureBackendActions::Stop();
	return FReply::Handled();
}

bool SDreamTextureStatusPanel::IsStartEnabled() const
{
	return DreamTextureBackendActions::CanStart();
}

FText SDreamTextureStatusPanel::GetStartTooltip() const
{
	FText Why;
	DreamTextureBackendActions::CanStart(&Why);
	return Why;
}

FText SDreamTextureStatusPanel::GetStartButtonText() const
{
	return DreamTextureBackendActions::IsStarting() ? LOCTEXT("StartingBackend", "Starting...") : LOCTEXT("StartBackend", "Start Backend");
}

EVisibility SDreamTextureStatusPanel::GetStopVisibility() const
{
	// 只有插件自己起的才给「停止」。附着的后端不归我们管，不给按钮就不会误点。
	return DreamTextureBackendActions::IsLaunchedByPlugin() ? EVisibility::Visible : EVisibility::Collapsed;
}

FReply SDreamTextureStatusPanel::OnSettingsClicked()
{
	// 不写死 "Project"/"Plugins"/"DreamTexture" 这三个字符串：UDeveloperSettings
	// 自己知道它注册到哪儿了，而 UDreamTextureSettings 还重写过 GetCategoryName()。
	// 照抄字面量迟早会和设置类对不上，而且对不上的时候是静默失败。
	if (ISettingsModule* SettingsModule = FModuleManager::LoadModulePtr<ISettingsModule>("Settings"))
	{
		const UDreamTextureSettings& Settings = UDreamTextureSettings::Get();
		SettingsModule->ShowViewer(Settings.GetContainerName(), Settings.GetCategoryName(), Settings.GetSectionName());
	}
	else
	{
		UE_LOG(LogDreamTexture, Warning, TEXT("The Settings module is not loaded, so Project Settings cannot be opened."));
	}

	return FReply::Handled();
}

FReply SDreamTextureStatusPanel::OnRunChecksClicked()
{
	TSharedPtr<FDreamTextureConnection> Conn = ConnectionWeak.Pin();
	if (!Conn.IsValid() || bChecksInFlight)
	{
		return FReply::Handled();
	}

	bChecksInFlight = true;
	bHasChecks = false;
	ChecksError.Reset();
	ChecksResult = FDreamTextureChecksResult();
	RebuildChecks();

	// FetchChecks 保证回调在游戏线程上跑，但不保证面板还在：nomad tab 关掉之后
	// 这个 widget 就没了。所以走弱引用，Pin 不到就安静退出。
	TWeakPtr<SDreamTextureStatusPanel> WeakSelf = SharedThis(this);
	Conn->FetchChecks([WeakSelf](bool bOK, FDreamTextureChecksResult&& Result, const FString& Error)
	{
		TSharedPtr<SDreamTextureStatusPanel> Self = WeakSelf.Pin();
		if (!Self.IsValid())
		{
			return;
		}

		Self->bChecksInFlight = false;
		Self->bHasChecks = bOK;
		Self->ChecksError = bOK ? FString() : Error;
		if (bOK)
		{
			Self->ChecksResult = MoveTemp(Result);
		}
		Self->RebuildChecks();
	});

	return FReply::Handled();
}

//////////////////////////////////////////////////////////////////////////
// 自检结果

void SDreamTextureStatusPanel::RebuildChecks()
{
	if (ChecksHost.IsValid())
	{
		// 结果是异步来的，整块换掉比增删 slot 干净，也不用担心旧行残留。
		ChecksHost->SetContent(BuildChecksContent());
	}
}

TSharedRef<SWidget> SDreamTextureStatusPanel::BuildChecksContent() const
{
	if (bChecksInFlight)
	{
		return SNew(STextBlock)
			.Text(LOCTEXT("ChecksInFlight", "Running the environment check..."))
			.ColorAndOpacity(FStyleColors::Foreground);
	}

	if (!ChecksError.IsEmpty())
	{
		return SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Top)
			.Padding(FMargin(0.0f, 1.0f, 8.0f, 0.0f))
			[
				SNew(SImage)
				.Image(ToneBrush(EStatusTone::Bad))
				.ColorAndOpacity(ToneColor(EStatusTone::Bad))
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(ChecksError))
				.ColorAndOpacity(FStyleColors::Error)
				.AutoWrapText(true)
			];
	}

	if (!bHasChecks)
	{
		return SNew(STextBlock)
			.Text(LOCTEXT("ChecksIdle", "The environment check has not been run yet. Click the button above to call /api/checks."))
			.ColorAndOpacity(FStyleColors::AccentGray)
			.AutoWrapText(true);
	}

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	// 总评：后端已经把所有项里最差的那个填在 Status 里了，不用自己再算一遍。
	{
		const EStatusTone OverallTone = ToneFromCheckStatus(ChecksResult.Status);
		FText OverallText;
		if (ChecksResult.Status.Equals(TEXT("ok"), ESearchCase::IgnoreCase))
		{
			OverallText = LOCTEXT("ChecksAllOk", "All checks passed");
		}
		else if (ChecksResult.Status.Equals(TEXT("warn"), ESearchCase::IgnoreCase))
		{
			OverallText = LOCTEXT("ChecksWarn", "Warnings: it works, but there are risks");
		}
		else if (ChecksResult.Status.Equals(TEXT("fail"), ESearchCase::IgnoreCase))
		{
			OverallText = LOCTEXT("ChecksFail", "Some checks failed");
		}
		else
		{
			OverallText = LOCTEXT("ChecksUnknownStatus", "The result status was not recognized");
		}

		Box->AddSlot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 0.0f, 0.0f, 8.0f))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				SNew(SImage)
				.Image(ToneBrush(OverallTone))
				.ColorAndOpacity(ToneColor(OverallTone))
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(OverallText)
				.Font(FAppStyle::Get().GetFontStyle("NormalFontBold"))
				.ColorAndOpacity(ToneColor(OverallTone))
			]
		];

		Box->AddSlot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 0.0f, 0.0f, 8.0f))
		[
			SNew(SSeparator)
			.Thickness(1.0f)
		];
	}

	// Checks 有可能是空的：Go 的 nil 切片序列化成 null 而不是 []，这个坑后端仓库
	// 里踩过两次。空数组走到这儿就是一行不画，不能当成错误。
	for (const FDreamTextureCheck& Check : ChecksResult.Checks)
	{
		const EStatusTone Tone = ToneFromCheckStatus(Check.Status);

		TSharedRef<SVerticalBox> Item = SNew(SVerticalBox);

		Item->AddSlot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				SNew(SImage)
				.Image(ToneBrush(Tone))
				.ColorAndOpacity(ToneColor(Tone))
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				// Label 是后端给的中文，直接显示。
				SNew(STextBlock)
				.Text(FText::FromString(Check.Label))
				.Font(FAppStyle::Get().GetFontStyle("NormalFontBold"))
				.ColorAndOpacity(ToneColor(Tone))
			]
		];

		if (!Check.Detail.IsEmpty())
		{
			Item->AddSlot()
			.AutoHeight()
			.Padding(FMargin(24.0f, 2.0f, 0.0f, 0.0f))
			[
				SNew(STextBlock)
				.Text(FText::FromString(Check.Detail))
				.ColorAndOpacity(FStyleColors::Foreground)
				.AutoWrapText(true)
			];
		}

		// Items 是「到底缺了哪几个」：缺失的节点类名、模型名。这才是修的时候
		// 真正用得上的东西，别折叠也别截断。
		for (const FString& One : Check.Items)
		{
			Item->AddSlot()
			.AutoHeight()
			.Padding(FMargin(24.0f, 2.0f, 0.0f, 0.0f))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.BulletPoint"))
					.ColorAndOpacity(FStyleColors::AccentGray)
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(One))
					.ColorAndOpacity(FStyleColors::AccentGray)
					.AutoWrapText(true)
				]
			];
		}

		Box->AddSlot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 0.0f, 0.0f, 10.0f))
		[
			Item
		];
	}

	return Box;
}

//////////////////////////////////////////////////////////////////////////
// 分档 → 画笔 / 颜色

const FSlateBrush* SDreamTextureStatusPanel::ToneBrush(EStatusTone InTone)
{
	// 五档各挑一个**形状不一样**的图标。特别注意 Icons.Warning 和 Icons.Error
	// 在 Starship 里是同一张 alert-triangle，只有颜色不同——所以 Bad 这一档
	// 换成了 Icons.X，不然「警告」和「坏了」在灰度下完全分不出来。
	switch (InTone)
	{
	case EStatusTone::Ok:      return FAppStyle::GetBrush("Icons.Success");  // 圆圈 + 对勾
	case EStatusTone::Warn:    return FAppStyle::GetBrush("Icons.Warning");  // 三角 + 叹号
	case EStatusTone::Bad:     return FAppStyle::GetBrush("Icons.X");        // 叉
	case EStatusTone::Busy:    return FAppStyle::GetBrush("Icons.Refresh");  // 只在转圈被遮住时才会看到
	case EStatusTone::Unknown:
	default:                   return FAppStyle::GetBrush("Icons.Help");     // 问号
	}
}

FSlateColor SDreamTextureStatusPanel::ToneColor(EStatusTone InTone)
{
	switch (InTone)
	{
	case EStatusTone::Ok:      return FStyleColors::Success;
	case EStatusTone::Warn:    return FStyleColors::Warning;
	case EStatusTone::Bad:     return FStyleColors::Error;
	case EStatusTone::Busy:    return FStyleColors::AccentBlue;
	case EStatusTone::Unknown:
	default:                   return FStyleColors::AccentGray;
	}
}

SDreamTextureStatusPanel::EStatusTone SDreamTextureStatusPanel::ToneFromCheckStatus(const FString& InStatus)
{
	if (InStatus.Equals(TEXT("ok"), ESearchCase::IgnoreCase))
	{
		return EStatusTone::Ok;
	}
	if (InStatus.Equals(TEXT("warn"), ESearchCase::IgnoreCase))
	{
		return EStatusTone::Warn;
	}
	if (InStatus.Equals(TEXT("fail"), ESearchCase::IgnoreCase))
	{
		return EStatusTone::Bad;
	}
	// 后端加了新状态值也不要瞎猜成 ok——问号比一个错误的对勾安全。
	return EStatusTone::Unknown;
}

#undef LOCTEXT_NAMESPACE
