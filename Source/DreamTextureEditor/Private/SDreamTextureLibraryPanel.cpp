// Copyright TypeDreamMoon. All Rights Reserved.

#include "SDreamTextureLibraryPanel.h"

#include "DreamTextureAssignModel.h"
#include "DreamTextureClientModule.h"
#include "DreamTextureConnection.h"
#include "DreamTextureImportNotifier.h"
#include "DreamTextureSettings.h"
#include "DreamTextureThumbCache.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "DreamTextureLibraryPanel"

//////////////////////////////////////////////////////////////////////////
// 构造与析构

void SDreamTextureLibraryPanel::Construct(const FArguments& InArgs)
{
	Model = InArgs._Model;
	if (!Model.IsValid())
	{
		Model = MakeShared<FDreamTextureLibraryModel>();
	}
	Model->Initialize(InArgs._bBindExternal);
	ThumbCache = MakeShared<FDreamTextureThumbCache>();

	ListChangedHandle = Model->OnListChanged.AddSP(this, &SDreamTextureLibraryPanel::HandleListChanged);
	DetailChangedHandle = Model->OnDetailChanged.AddSP(this, &SDreamTextureLibraryPanel::HandleDetailChanged);
	NoticeHandle = Model->OnNotice.AddSP(this, &SDreamTextureLibraryPanel::HandleNotice);

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
			.Padding(FMargin(8.0f, 6.0f))
			[
				MakeToolbar()
			]
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				MakeGridArea()
			]

			// 详情在右侧，选中后才出现。放右边而不是下面：网格是纵向滚动的，
			// 占掉下半屏会把网格压得只剩一行。
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				MakeDetailArea()
			]
		]
	];

	RebuildDetail();

	if (InArgs._bAutoRefresh)
	{
		Model->Refresh();
	}
}

SDreamTextureLibraryPanel::~SDreamTextureLibraryPanel()
{
	// 模型可能比面板活得久（测试里是外面持有的），所以一定要摘。
	if (Model.IsValid())
	{
		Model->OnListChanged.Remove(ListChangedHandle);
		Model->OnDetailChanged.Remove(DetailChangedHandle);
		Model->OnNotice.Remove(NoticeHandle);
	}
}

//////////////////////////////////////////////////////////////////////////
// 工具条

TSharedRef<SWidget> SDreamTextureLibraryPanel::MakeToolbar()
{
	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
		[
			// 搜索框自带「停顿后才通知」：每敲一个字就查一次后端既浪费，
			// 又会让慢的旧请求和新请求赛跑。0.3 秒是敲字的自然停顿。
			SAssignNew(SearchBox, SSearchBox)
			.HintText(LOCTEXT("SearchHint", "Search names, prompts and tags..."))
			.DelayChangeNotificationsWhileTyping(true)
			.DelayChangeNotificationsWhileTypingSeconds(0.3f)
			.OnTextChanged(this, &SDreamTextureLibraryPanel::OnSearchChanged)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
		[
			SNew(SSegmentedControl<EStyleFilter>)
			.Value(this, &SDreamTextureLibraryPanel::GetStyleFilter)
			.OnValueChanged(this, &SDreamTextureLibraryPanel::OnStyleChanged)

			+ SSegmentedControl<EStyleFilter>::Slot(EStyleFilter::All)
			.Text(LOCTEXT("StyleAll", "All"))
			.ToolTip(LOCTEXT("StyleAllTip", "Do not filter by style."))

			+ SSegmentedControl<EStyleFilter>::Slot(EStyleFilter::Realistic)
			.Text(LOCTEXT("StyleRealistic", "Realistic"))
			.ToolTip(LOCTEXT("StyleRealisticTip", "Material sets made by the realistic pipeline (realistic)."))

			+ SSegmentedControl<EStyleFilter>::Slot(EStyleFilter::Stylized)
			.Text(LOCTEXT("StyleStylized", "Stylized"))
			.ToolTip(LOCTEXT("StyleStylizedTip", "Material sets made by the hand-painted pipeline (stylized)."))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
		[
			SNew(SCheckBox)
			.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
			.IsChecked(this, &SDreamTextureLibraryPanel::GetFavoriteOnlyState)
			.OnCheckStateChanged(this, &SDreamTextureLibraryPanel::OnFavoriteOnlyChanged)
			.ToolTipText(LOCTEXT("FavOnlyTip", "Show only favorited material sets."))
			.Padding(FMargin(8.0f, 2.0f))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 4.0f, 0.0f))
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.Star"))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("FavOnly", "Favorites"))
				]
			]
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SButton)
			.ContentPadding(FMargin(8.0f, 2.0f))
			.ToolTipText(LOCTEXT("RefreshTip", "Re-read the library. Material sets finished in the web UI also appear on their own; this is the manual fallback."))
			.IsEnabled(this, &SDreamTextureLibraryPanel::IsRefreshEnabled)
			.OnClicked(this, &SDreamTextureLibraryPanel::OnRefreshClicked)
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
					.Text(LOCTEXT("Refresh", "Refresh"))
				]
			]
		];
}

void SDreamTextureLibraryPanel::OnSearchChanged(const FText& NewText)
{
	Model->SetSearchText(NewText.ToString());
}

void SDreamTextureLibraryPanel::OnStyleChanged(EStyleFilter NewStyle)
{
	switch (NewStyle)
	{
	case EStyleFilter::Realistic: Model->SetStyleFilter(TEXT("realistic")); break;
	case EStyleFilter::Stylized:  Model->SetStyleFilter(TEXT("stylized")); break;
	case EStyleFilter::All:
	default:                      Model->SetStyleFilter(FString()); break;
	}
}

SDreamTextureLibraryPanel::EStyleFilter SDreamTextureLibraryPanel::GetStyleFilter() const
{
	const FString& Style = Model->GetStyleFilter();
	if (Style == TEXT("realistic"))
	{
		return EStyleFilter::Realistic;
	}
	if (Style == TEXT("stylized"))
	{
		return EStyleFilter::Stylized;
	}
	return EStyleFilter::All;
}

ECheckBoxState SDreamTextureLibraryPanel::GetFavoriteOnlyState() const
{
	return Model->IsFavoriteOnly() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SDreamTextureLibraryPanel::OnFavoriteOnlyChanged(ECheckBoxState NewState)
{
	Model->SetFavoriteOnly(NewState == ECheckBoxState::Checked);
}

bool SDreamTextureLibraryPanel::IsRefreshEnabled() const
{
	return !Model->IsLoading();
}

FReply SDreamTextureLibraryPanel::OnRefreshClicked()
{
	// 用户明确要重来：失败过的缩略图也立刻重试，不等自动重试的间隔。
	ThumbCache->ResetFailures();
	Model->Refresh();
	return FReply::Handled();
}

FReply SDreamTextureLibraryPanel::OnReconnectClicked()
{
	// 和状态页的「重连」同一套动作：Start() 幂等，补一刀防止没人启动过连接；
	// Reconnect() 跳过退避立刻重试。连上之后链路变 Live，模型会自己刷新一次，
	// 这里再主动刷新是为了「后端其实通着、只是 WS 没连上」的情形也能立刻见效。
	if (FDreamTextureClientModule::IsAvailable())
	{
		if (TSharedPtr<FDreamTextureConnection> Conn = FDreamTextureClientModule::Get().GetConnection())
		{
			Conn->Start();
			Conn->Reconnect();
		}
	}
	ThumbCache->ResetFailures();
	Model->Refresh();
	return FReply::Handled();
}

FReply SDreamTextureLibraryPanel::OnClearFiltersClicked()
{
	if (SearchBox.IsValid())
	{
		// 清文本框会触发 OnTextChanged，模型那边顺带清了；但文本框自己的延迟通知
		// 可能把旧值再推一次，所以下面直接把模型也置空，两边一致。
		SearchBox->SetText(FText::GetEmpty());
	}
	Model->SetSearchText(FString());
	Model->SetStyleFilter(FString());
	Model->SetFavoriteOnly(false);
	return FReply::Handled();
}

FReply SDreamTextureLibraryPanel::OnLoadMoreClicked()
{
	Model->LoadMore();
	return FReply::Handled();
}

//////////////////////////////////////////////////////////////////////////
// 网格区与它的四种状态

SDreamTextureLibraryPanel::EViewState SDreamTextureLibraryPanel::GetViewState() const
{
	if (Model->GetItems().Num() > 0)
	{
		return EViewState::Grid;
	}
	// 没有任何条目：先看失败，再看有没有读成功过。
	if (!Model->GetListError().IsEmpty())
	{
		return EViewState::Error;
	}
	if (Model->HasLoadedOnce())
	{
		return EViewState::Empty;
	}
	return EViewState::Loading;
}

int32 SDreamTextureLibraryPanel::GetViewStateIndex() const
{
	switch (GetViewState())
	{
	case EViewState::Grid:    return 0;
	case EViewState::Loading: return 1;
	case EViewState::Error:   return 2;
	case EViewState::Empty:
	default:                  return 3;
	}
}

TSharedRef<SWidget> SDreamTextureLibraryPanel::MakeGridArea()
{
	TSharedRef<SWidget> Grid =
		SNew(SVerticalBox)

		// 有旧数据但这次刷新失败：网格留着，上面挂一条提示，不把好端端的内容清空。
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
			.Padding(FMargin(8.0f, 4.0f))
			.Visibility(this, &SDreamTextureLibraryPanel::GetStaleErrorBannerVisibility)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.Warning"))
					.ColorAndOpacity(FStyleColors::Warning)
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SDreamTextureLibraryPanel::GetStaleErrorBannerText)
					.ColorAndOpacity(FStyleColors::Warning)
					.AutoWrapText(true)
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SButton)
					.ContentPadding(FMargin(8.0f, 0.0f))
					.OnClicked(this, &SDreamTextureLibraryPanel::OnReconnectClicked)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("Reconnect", "Reconnect"))
					]
				]
			]
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(TileView, STileView<FDreamTextureLibraryItemPtr>)
			.ListItemsSource(&Model->GetItems())
			.OnGenerateTile(this, &SDreamTextureLibraryPanel::OnGenerateTile)
			.OnSelectionChanged(this, &SDreamTextureLibraryPanel::OnSelectionChanged)
			.SelectionMode(ESelectionMode::Single)
			.ItemWidth(TileWidth)
			.ItemHeight(TileHeight)
			.ItemAlignment(EListItemAlignment::LeftAligned)
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
			.Padding(FMargin(8.0f, 4.0f))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SDreamTextureLibraryPanel::GetFooterCountText)
					.ColorAndOpacity(FStyleColors::Foreground)
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SButton)
					.ContentPadding(FMargin(10.0f, 1.0f))
					.Visibility(this, &SDreamTextureLibraryPanel::GetLoadMoreVisibility)
					.IsEnabled(this, &SDreamTextureLibraryPanel::IsLoadMoreEnabled)
					.OnClicked(this, &SDreamTextureLibraryPanel::OnLoadMoreClicked)
					[
						SNew(STextBlock)
						.Text(this, &SDreamTextureLibraryPanel::GetLoadMoreText)
					]
				]
			]
		];

	return SNew(SWidgetSwitcher)
		.WidgetIndex(this, &SDreamTextureLibraryPanel::GetViewStateIndex)

		+ SWidgetSwitcher::Slot()
		[
			Grid
		]

		+ SWidgetSwitcher::Slot()
		[
			MakeLoadingState()
		]

		+ SWidgetSwitcher::Slot()
		[
			MakeErrorState()
		]

		+ SWidgetSwitcher::Slot()
		[
			MakeEmptyState()
		];
}

TSharedRef<SWidget> SDreamTextureLibraryPanel::MakeLoadingState()
{
	return SNew(SBox)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				SNew(SCircularThrobber)
				.Radius(10.0f)
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Loading", "Reading the library..."))
				.ColorAndOpacity(FStyleColors::Foreground)
			]
		];
}

TSharedRef<SWidget> SDreamTextureLibraryPanel::MakeErrorState()
{
	return SNew(SBox)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.Padding(FMargin(24.0f))
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 8.0f))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
				[
					// Bad 用 Icons.X 而不是 Icons.Error：后者在 Starship 里和 Icons.Warning
					// 是同一张三角，灰度下分不出「警告」和「坏了」。
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.X"))
					.ColorAndOpacity(FStyleColors::Error)
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ErrorTitle", "Could not read the library"))
					.Font(FAppStyle::Get().GetFontStyle("NormalFontBold"))
					.ColorAndOpacity(FStyleColors::Error)
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 4.0f))
			[
				SNew(STextBlock)
				.Text(this, &SDreamTextureLibraryPanel::GetErrorHeadline)
				.AutoWrapText(true)
				.Justification(ETextJustify::Center)
			]

			// 连接层自己的最近一次失败原因（后端那边已经是中文，原样显示）。
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 12.0f))
			[
				SNew(STextBlock)
				.Text(this, &SDreamTextureLibraryPanel::GetConnectionErrorText)
				.Visibility(this, &SDreamTextureLibraryPanel::GetConnectionErrorVisibility)
				.ColorAndOpacity(FStyleColors::AccentGray)
				.AutoWrapText(true)
				.Justification(ETextJustify::Center)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
				[
					SNew(SButton)
					.ContentPadding(FMargin(12.0f, 2.0f))
					.ToolTipText(LOCTEXT("ReconnectTip", "Retry the connection right now instead of waiting for the backoff. Also click it after changing the backend address."))
					.OnClicked(this, &SDreamTextureLibraryPanel::OnReconnectClicked)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("Reconnect", "Reconnect"))
					]
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.ContentPadding(FMargin(12.0f, 2.0f))
					.IsEnabled(this, &SDreamTextureLibraryPanel::IsRefreshEnabled)
					.OnClicked(this, &SDreamTextureLibraryPanel::OnRefreshClicked)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("Retry", "Retry"))
					]
				]
			]
		];
}

TSharedRef<SWidget> SDreamTextureLibraryPanel::MakeEmptyState()
{
	return SNew(SBox)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.Padding(FMargin(24.0f))
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 8.0f))
			[
				SNew(STextBlock)
				.Text_Lambda([this]()
				{
					return Model->HasActiveFilter()
						? LOCTEXT("EmptyFiltered", "No material sets match the filters.")
						: LOCTEXT("EmptyAll", "The library is still empty. Generate a material set in the web UI and it will appear here on its own when it finishes.");
				})
				.AutoWrapText(true)
				.Justification(ETextJustify::Center)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			[
				SNew(SButton)
				.ContentPadding(FMargin(12.0f, 2.0f))
				.Visibility_Lambda([this]()
				{
					return Model->HasActiveFilter() ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.OnClicked(this, &SDreamTextureLibraryPanel::OnClearFiltersClicked)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ClearFilters", "Clear filters"))
				]
			]
		];
}

FText SDreamTextureLibraryPanel::GetErrorHeadline() const
{
	return FText::FromString(Model->GetListError());
}

FText SDreamTextureLibraryPanel::GetConnectionErrorText() const
{
	if (FDreamTextureClientModule::IsAvailable())
	{
		if (TSharedPtr<FDreamTextureConnection> Conn = FDreamTextureClientModule::Get().GetConnection())
		{
			const FString& LastError = Conn->GetLastError();
			// 和列表错误一样的话不重复显示。
			if (!LastError.IsEmpty() && LastError != Model->GetListError())
			{
				return FText::Format(LOCTEXT("ConnErrFmt", "Connection: {0}"), FText::FromString(LastError));
			}
		}
	}
	return FText::GetEmpty();
}

EVisibility SDreamTextureLibraryPanel::GetConnectionErrorVisibility() const
{
	return GetConnectionErrorText().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility SDreamTextureLibraryPanel::GetStaleErrorBannerVisibility() const
{
	return (!Model->GetListError().IsEmpty() && Model->GetItems().Num() > 0) ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SDreamTextureLibraryPanel::GetStaleErrorBannerText() const
{
	return FText::Format(LOCTEXT("StaleBanner", "Refresh failed. Showing what was read last time: {0}"), FText::FromString(Model->GetListError()));
}

FText SDreamTextureLibraryPanel::GetFooterCountText() const
{
	return FText::Format(LOCTEXT("FooterCount", "{0} {0}|plural(one=material set,other=material sets) loaded{1}"),
		Model->GetItems().Num(),
		Model->IsLoading() ? LOCTEXT("FooterRefreshing", " · refreshing...") : FText::GetEmpty());
}

EVisibility SDreamTextureLibraryPanel::GetLoadMoreVisibility() const
{
	return Model->HasMore() ? EVisibility::Visible : EVisibility::Collapsed;
}

bool SDreamTextureLibraryPanel::IsLoadMoreEnabled() const
{
	return Model->HasMore() && !Model->IsLoading() && !Model->IsLoadingMore();
}

FText SDreamTextureLibraryPanel::GetLoadMoreText() const
{
	return Model->IsLoadingMore() ? LOCTEXT("LoadingMore", "Loading...") : LOCTEXT("LoadMore", "Load more");
}

//////////////////////////////////////////////////////////////////////////
// 卡片

FText SDreamTextureLibraryPanel::StyleLabel(const FString& Style)
{
	if (Style.Equals(TEXT("realistic"), ESearchCase::IgnoreCase))
	{
		return LOCTEXT("StyleRealisticLabel", "Realistic");
	}
	if (Style.Equals(TEXT("stylized"), ESearchCase::IgnoreCase))
	{
		return LOCTEXT("StyleStylizedLabel", "Stylized");
	}
	// 后端将来加了新风格：显示原文，不要硬翻成「未知」。
	return Style.IsEmpty() ? LOCTEXT("StyleNone", "Uncategorized") : FText::FromString(Style);
}

TSharedRef<ITableRow> SDreamTextureLibraryPanel::OnGenerateTile(FDreamTextureLibraryItemPtr Item, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(STableRow<FDreamTextureLibraryItemPtr>, Owner)
		.Padding(FMargin(4.0f))
		[
			MakeTileContent(Item)
		];
}

TSharedRef<SWidget> SDreamTextureLibraryPanel::MakeThumbnail(const FString& Id, float Size)
{
	// 缓存只被面板持有，属性回调活在控件树里；按弱指针取，面板析构后不会碰到一个死缓存。
	TWeakPtr<FDreamTextureThumbCache> WeakCache = ThumbCache;

	return SNew(SBox)
		.WidthOverride(Size)
		.HeightOverride(Size)
		[
			SNew(SOverlay)

			+ SOverlay::Slot()
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				.Visibility_Lambda([WeakCache, Id]()
				{
					const TSharedPtr<FDreamTextureThumbCache> Cache = WeakCache.Pin();
					return (Cache.IsValid() && Cache->HasBrush(Id)) ? EVisibility::Collapsed : EVisibility::Visible;
				})
				[
					// 占位要说清是「还在加载」还是「确实没有」，两者的处置完全不同。
					SNew(STextBlock)
					.Text_Lambda([WeakCache, Id]()
					{
						const TSharedPtr<FDreamTextureThumbCache> Cache = WeakCache.Pin();
						return (Cache.IsValid() && Cache->IsFailed(Id))
							? LOCTEXT("ThumbMissing", "No preview")
							: LOCTEXT("ThumbLoading", "Loading...");
					})
					.ColorAndOpacity(FStyleColors::AccentGray)
				]
			]

			+ SOverlay::Slot()
			[
				// 这个 Image 属性回调同时是加载的触发点：每帧问一次缓存要画刷，
				// 没有就顺手排一次加载。所以它不能随占位一起 Collapsed——
				// 被折叠的控件不会被绘制，回调就永远不会跑。
				SNew(SImage)
				.Image_Lambda([WeakCache, Id]() -> const FSlateBrush*
				{
					const TSharedPtr<FDreamTextureThumbCache> Cache = WeakCache.Pin();
					return Cache.IsValid() ? Cache->GetBrush(Id) : nullptr;
				})
			]
		];
}

TSharedRef<SWidget> SDreamTextureLibraryPanel::MakeTileContent(const FDreamTextureLibraryItemPtr& Item)
{
	const FDreamTextureMaterialSummary& Summary = Item->Summary;

	return SNew(SBox)
		.WidthOverride(TileWidth - 8.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
			.Padding(FMargin(4.0f))
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SOverlay)

					+ SOverlay::Slot()
					[
						MakeThumbnail(Summary.Id, ThumbSize)
					]

					+ SOverlay::Slot()
					.HAlign(HAlign_Right)
					.VAlign(VAlign_Top)
					.Padding(FMargin(3.0f))
					[
						// 垫一层半透明深色：缩略图底色任意，星标直接叠上去可能看不清。
						SNew(SBorder)
						.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
						.BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.55f))
						.Padding(FMargin(0.0f))
						[
							MakeFavoriteButton(Item)
						]
					]
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
				[
					SNew(STextBlock)
					.Text(FText::FromString(Summary.Name.IsEmpty() ? Summary.Id : Summary.Name))
					.ToolTipText(FText::FromString(Summary.Name))
					.Font(FAppStyle::Get().GetFontStyle("NormalFontBold"))
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 1.0f, 0.0f, 0.0f))
				[
					SNew(STextBlock)
					.Text(FText::Format(LOCTEXT("TileSubtitle", "{0} · {1}²"),
						StyleLabel(Summary.Style), FText::AsNumber(Summary.Resolution, &FNumberFormattingOptions::DefaultNoGrouping())))
					.ColorAndOpacity(FStyleColors::AccentGray)
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
				[
					MakeImportControls(Summary.Id)
				]
			]
		];
}

void SDreamTextureLibraryPanel::OnSelectionChanged(FDreamTextureLibraryItemPtr Item, ESelectInfo::Type SelectInfo)
{
	Model->Select(Item.IsValid() ? Item->Summary.Id : FString());
}

//////////////////////////////////////////////////////////////////////////
// 收藏

TSharedRef<SWidget> SDreamTextureLibraryPanel::MakeFavoriteButton(const FDreamTextureLibraryItemPtr& Item)
{
	// 条目对象在刷新时按 id 复用，所以按钮持有它的弱引用就够了，不必每帧按 id 在列表里找。
	TWeakPtr<FDreamTextureLibraryItem> WeakItem = Item;
	TWeakPtr<FDreamTextureLibraryModel> WeakModel = Model;
	const FString Id = Item->Summary.Id;

	auto IsFav = [WeakItem]() -> bool
	{
		const TSharedPtr<FDreamTextureLibraryItem> Pinned = WeakItem.Pin();
		return Pinned.IsValid() && Pinned->Summary.bFavorite;
	};

	return SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "SimpleButton")
		.ContentPadding(FMargin(3.0f))
		.ToolTipText_Lambda([IsFav]()
		{
			return IsFav()
				? LOCTEXT("UnfavoriteTip", "Favorited. Click to remove it from favorites.")
				: LOCTEXT("FavoriteTip", "Click to add to favorites.");
		})
		.OnClicked_Lambda([WeakModel, Id]()
		{
			if (TSharedPtr<FDreamTextureLibraryModel> Pinned = WeakModel.Pin())
			{
				Pinned->ToggleFavorite(Id);
			}
			return FReply::Handled();
		})
		[
			SNew(SImage)
			.Image_Lambda([IsFav]() -> const FSlateBrush*
			{
				return FAppStyle::GetBrush(IsFav() ? "Icons.Star" : "Icons.Star.Outline");
			})
			.ColorAndOpacity_Lambda([IsFav]() -> FSlateColor
			{
				return IsFav() ? FSlateColor(FLinearColor(1.0f, 0.78f, 0.12f)) : FSlateColor::UseForeground();
			})
		];
}

//////////////////////////////////////////////////////////////////////////
// 导入控件

namespace
{
	/** 一次导入此刻该怎么画。每帧由 EvaluateImport 现算，不缓存。 */
	struct FImportView
	{
		/** 排队/下载/写入中：显示进度，不显示按钮。 */
		bool bActive = false;

		/** 进度 0..1；未知时为空，进度条走跑马灯。 */
		TOptional<float> Percent;
		FText StageText;
		FText StageTooltip;

		enum class EResult : uint8 { None, Succeeded, Failed };
		EResult Result = EResult::None;
		FText ResultText;
		FText ResultTooltip;

		FText ButtonText;
	};

	FImportView EvaluateImport(const FDreamTextureLibraryModel* Model, const FString& Id)
	{
		FImportView View;
		View.ButtonText = NSLOCTEXT("DreamTextureLibraryPanel", "ImportBtn", "Import");

		const FDreamTextureImportStatus* Status = Model ? Model->FindImportStatus(Id) : nullptr;
		if (!Status)
		{
			return View;
		}

		View.StageTooltip = FText::FromString(Status->Message);

		switch (Status->Stage)
		{
		case EDreamTextureImportStage::Queued:
			View.bActive = true;
			View.StageText = NSLOCTEXT("DreamTextureLibraryPanel", "StageQueued", "Queued...");
			break;
		case EDreamTextureImportStage::FetchingManifest:
			View.bActive = true;
			View.StageText = NSLOCTEXT("DreamTextureLibraryPanel", "StageManifest", "Reading manifest...");
			break;
		case EDreamTextureImportStage::Downloading:
			View.bActive = true;
			if (Status->FilesTotal > 0)
			{
				View.StageText = FText::Format(NSLOCTEXT("DreamTextureLibraryPanel", "StageDownloading", "Downloading {0}/{1}"),
					FText::AsNumber(Status->FilesDone), FText::AsNumber(Status->FilesTotal));
				View.Percent = FMath::Clamp(static_cast<float>(Status->FilesDone) / static_cast<float>(Status->FilesTotal), 0.0f, 1.0f);
			}
			else
			{
				View.StageText = NSLOCTEXT("DreamTextureLibraryPanel", "StageDownloadingNoCount", "Downloading...");
			}
			break;
		case EDreamTextureImportStage::Importing:
			View.bActive = true;
			View.StageText = NSLOCTEXT("DreamTextureLibraryPanel", "StageImporting", "Writing to project...");
			break;
		case EDreamTextureImportStage::Succeeded:
			if (Status->Result.bOK)
			{
				View.Result = FImportView::EResult::Succeeded;
				View.ResultText = NSLOCTEXT("DreamTextureLibraryPanel", "ResultOk", "Imported");
				View.ResultTooltip = FText::FromString(Status->Result.AssetFolder);
				View.ButtonText = NSLOCTEXT("DreamTextureLibraryPanel", "ImportAgain", "Import again");
				break;
			}
			// bOK 为假的「完成」（全部跳过之类）按失败画，原因在 Result.Error。
			[[fallthrough]];
		case EDreamTextureImportStage::Failed:
		default:
			{
				View.Result = FImportView::EResult::Failed;
				View.ResultText = NSLOCTEXT("DreamTextureLibraryPanel", "ResultFailed", "Import failed");
				const FString& Reason = Status->Stage == EDreamTextureImportStage::Failed
					? (Status->Message.IsEmpty() ? Status->Result.Error : Status->Message)
					: (Status->Result.Error.IsEmpty() ? Status->Message : Status->Result.Error);
				View.ResultTooltip = FText::FromString(Reason);
				View.ButtonText = NSLOCTEXT("DreamTextureLibraryPanel", "ImportRetry", "Retry");
			}
			break;
		}
		return View;
	}
}

TSharedRef<SWidget> SDreamTextureLibraryPanel::MakeImportControls(const FString& Id)
{
	TWeakPtr<FDreamTextureLibraryModel> WeakModel = Model;

	auto Eval = [WeakModel, Id]() -> FImportView
	{
		const TSharedPtr<FDreamTextureLibraryModel> Pinned = WeakModel.Pin();
		return EvaluateImport(Pinned.Get(), Id);
	};

	// 空闲/结束态：[导入] 按钮 + 一行结果。
	TSharedRef<SWidget> Idle =
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
			.ContentPadding(FMargin(8.0f, 1.0f))
			.ToolTipText_Lambda([]()
			{
				// 开关关着时别许诺会建材质实例。
				return UDreamTextureSettings::Get().bCreateMaterialInstance
					? LOCTEXT("ImportTip", "Imports this material set's textures into the project and creates a material instance. Textures that were already imported and have not changed are not downloaded again.")
					: LOCTEXT("ImportTipNoMI", "Imports only this material set's textures into the project (Create Material Instance is turned off in the settings). Textures that were already imported and have not changed are not downloaded again.");
			})
			.OnClicked_Lambda([WeakModel, Id]()
			{
				if (TSharedPtr<FDreamTextureLibraryModel> Pinned = WeakModel.Pin())
				{
					Pinned->RequestImport(Id);
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
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.Download"))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([Eval]() { return Eval().ButtonText; })
				]
			]
		]

		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			// 结果 = 图标形状（圆对勾 / 叉）+ 中文，颜色只是第三重冗余。
			SNew(SHorizontalBox)
			.Visibility_Lambda([Eval]()
			{
				return Eval().Result == FImportView::EResult::None ? EVisibility::Hidden : EVisibility::Visible;
			})
			.ToolTipText_Lambda([Eval]() { return Eval().ResultTooltip; })

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 3.0f, 0.0f))
			[
				SNew(SImage)
				.Image_Lambda([Eval]() -> const FSlateBrush*
				{
					return FAppStyle::GetBrush(Eval().Result == FImportView::EResult::Failed ? "Icons.X" : "Icons.Success");
				})
				.ColorAndOpacity_Lambda([Eval]() -> FSlateColor
				{
					return Eval().Result == FImportView::EResult::Failed ? FStyleColors::Error : FStyleColors::Success;
				})
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([Eval]() { return Eval().ResultText; })
				.ColorAndOpacity_Lambda([Eval]() -> FSlateColor
				{
					return Eval().Result == FImportView::EResult::Failed ? FStyleColors::Error : FStyleColors::Success;
				})
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
		];

	// 进行中态：阶段文字 + 进度条，按钮收起，防止重复点。
	TSharedRef<SWidget> Active =
		SNew(SVerticalBox)
		.ToolTipText_Lambda([Eval]() { return Eval().StageTooltip; })

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 4.0f, 0.0f))
			[
				SNew(SCircularThrobber)
				.Radius(6.0f)
				.NumPieces(6)
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([Eval]() { return Eval().StageText; })
				.ColorAndOpacity(FStyleColors::AccentBlue)
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 3.0f, 0.0f, 0.0f))
		[
			SNew(SBox)
			.HeightOverride(6.0f)
			[
				SNew(SProgressBar)
				.Percent_Lambda([Eval]() { return Eval().Percent; })
			]
		];

	return SNew(SBox)
		.MinDesiredHeight(34.0f)
		.VAlign(VAlign_Center)
		[
			SNew(SWidgetSwitcher)
			.WidgetIndex_Lambda([Eval]() { return Eval().bActive ? 1 : 0; })

			+ SWidgetSwitcher::Slot()
			[
				Idle
			]

			+ SWidgetSwitcher::Slot()
			[
				Active
			]
		];
}

//////////////////////////////////////////////////////////////////////////
// 材质实例

namespace
{
	/** 详情栏里材质实例这一段此刻该显示什么。每帧现算。 */
	struct FMIView
	{
		bool bShow = false;
		FDreamTextureImportNotifier::FMaterialInstanceSummary Summary;
	};

	FMIView EvaluateMaterialInstance(const FDreamTextureLibraryModel* Model, const FString& Id)
	{
		FMIView View;
		const FDreamTextureImportStatus* Status = Model ? Model->FindImportStatus(Id) : nullptr;
		// 只在这套材质导入成功之后显示：没导入过就谈不上材质实例；
		// 导入失败的原因已经在导入控件那一行说了，这里不重复。
		if (Status && Status->Stage == EDreamTextureImportStage::Succeeded && Status->Result.bOK)
		{
			View.bShow = true;
			View.Summary = FDreamTextureImportNotifier::DescribeMaterialInstance(
				Status->Result, UDreamTextureSettings::Get().bCreateMaterialInstance);
		}
		return View;
	}
}

TSharedRef<SWidget> SDreamTextureLibraryPanel::MakeMaterialInstanceSection(const FString& Id)
{
	TWeakPtr<FDreamTextureLibraryModel> WeakModel = Model;
	using EKind = FDreamTextureImportNotifier::FMaterialInstanceSummary::EKind;

	auto Eval = [WeakModel, Id]() -> FMIView
	{
		const TSharedPtr<FDreamTextureLibraryModel> Pinned = WeakModel.Pin();
		return EvaluateMaterialInstance(Pinned.Get(), Id);
	};

	return SNew(SHorizontalBox)
		.Visibility_Lambda([Eval]() { return Eval().bShow ? EVisibility::Visible : EVisibility::Collapsed; })

		// 三种情形图标形状各不相同（对勾 / 信息 / 三角），颜色只是冗余。
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Top)
		.Padding(FMargin(0.0f, 1.0f, 6.0f, 0.0f))
		[
			SNew(SImage)
			.Image_Lambda([Eval]() -> const FSlateBrush*
			{
				switch (Eval().Summary.Kind)
				{
				case EKind::Present:           return FAppStyle::GetBrush("Icons.Success");
				case EKind::DisabledBySetting: return FAppStyle::GetBrush("Icons.Info");
				case EKind::NotCreated:
				default:                       return FAppStyle::GetBrush("Icons.Warning");
				}
			})
			.ColorAndOpacity_Lambda([Eval]() -> FSlateColor
			{
				switch (Eval().Summary.Kind)
				{
				case EKind::Present:           return FStyleColors::Success;
				case EKind::DisabledBySetting: return FStyleColors::AccentGray;
				case EKind::NotCreated:
				default:                       return FStyleColors::Warning;
				}
			})
		]

		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Top)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text_Lambda([Eval]() { return Eval().Summary.Sentence; })
				.AutoWrapText(true)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Left)
			.Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.ContentPadding(FMargin(8.0f, 0.0f))
				.Visibility_Lambda([Eval]()
				{
					return Eval().Summary.Kind == EKind::Present ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.ToolTipText(LOCTEXT("ShowMITip", "Select this material instance in the Content Browser."))
				.OnClicked_Lambda([Eval, WeakModel, Id]()
				{
					const FMIView View = Eval();
					if (View.Summary.Kind == EKind::Present)
					{
						// 找不到资产时退回跳到目录：目录取状态里的 AssetFolder。
						FString Folder;
						if (const TSharedPtr<FDreamTextureLibraryModel> Pinned = WeakModel.Pin())
						{
							if (const FDreamTextureImportStatus* Status = Pinned->FindImportStatus(Id))
							{
								Folder = Status->Result.AssetFolder;
							}
						}
						FDreamTextureImportNotifier::ShowAssetInContentBrowser(View.Summary.ObjectPath, Folder);
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
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.FolderOpen"))
						.ColorAndOpacity(FSlateColor::UseForeground())
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("ShowMI", "Show in Content Browser"))
					]
				]
			]

			// 赋给选中物体：导进来就能直接看到场景变了。
			// 没有材质实例时按钮仍显示但灰掉并说明原因（M2 的实例没建出来时用户才知道为什么点不了）。
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Left)
			.Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.ContentPadding(FMargin(8.0f, 0.0f))
				.IsEnabled_Lambda([Eval]()
				{
					const FMIView View = Eval();
					return View.Summary.Kind == EKind::Present
						&& FDreamTextureAssignProbe::Get(true, FText::GetEmpty()).bCanAssign;
				})
				.ToolTipText_Lambda([Eval]()
				{
					const FMIView View = Eval();
					if (View.Summary.Kind != EKind::Present)
					{
						return LOCTEXT("AssignNoMI", "There is no material instance to assign: this material set has not produced one yet (see the line above for the reason).");
					}
					return FDreamTextureAssignProbe::Get(true, FText::GetEmpty()).Hint;
				})
				.OnClicked_Lambda([Eval]()
				{
					const FMIView View = Eval();
					if (View.Summary.Kind == EKind::Present)
					{
						DreamTextureAssignFlow::Run(View.Summary.ObjectPath);
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
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.Check"))
						.ColorAndOpacity(FSlateColor::UseForeground())
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("AssignToSelection", "Assign to Selection"))
					]
				]
			]
		];
}

//////////////////////////////////////////////////////////////////////////
// 详情区

TSharedRef<SWidget> SDreamTextureLibraryPanel::MakeDetailArea()
{
	// 栏一直在、宽度固定：选中/取消选中只换里面的内容，网格宽度不会跟着跳。
	return SNew(SBox)
		.WidthOverride(DetailWidth)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
			.Padding(FMargin(0.0f))
			[
				SAssignNew(DetailHost, SBox)
			]
		];
}

void SDreamTextureLibraryPanel::RebuildDetail()
{
	if (DetailHost.IsValid())
	{
		DetailHost->SetContent(BuildDetailContent());
	}
}

FText SDreamTextureLibraryPanel::FormatCreatedAt(const FDateTime& UtcTime)
{
	if (UtcTime.GetTicks() == 0)
	{
		return LOCTEXT("TimeUnknown", "Unknown");
	}

	// 后端给的是 UTC（RFC3339），换成本机时区再显示，否则「刚生成的」会显示成八小时前。
	// 偏移量取整到分钟，免得 Now 和 UtcNow 两次取值的毫秒差漏进显示。
	const double OffsetMinutes = FMath::RoundToDouble((FDateTime::Now() - FDateTime::UtcNow()).GetTotalMinutes());
	const FDateTime Local = UtcTime + FTimespan::FromMinutes(OffsetMinutes);
	return FText::FromString(Local.ToString(TEXT("%Y-%m-%d %H:%M")));
}

TSharedRef<SWidget> SDreamTextureLibraryPanel::MakeFieldRow(const FText& Label, const FText& Value)
{
	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Top)
		[
			// 标签列按英文定宽："Negative prompt""Material set ID" 在 64 下会被截掉；中文标签更短，宽一点无妨。
			SNew(SBox)
			.WidthOverride(112.0f)
			.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				SNew(STextBlock)
				.Text(Label)
				.ColorAndOpacity(FStyleColors::AccentGray)
			]
		]

		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Top)
		[
			SNew(STextBlock)
			.Text(Value)
			.AutoWrapText(true)
		];
}

TSharedRef<SWidget> SDreamTextureLibraryPanel::MakeLicenseBox(const FDreamTextureManifest& Manifest)
{
	const FDreamTextureLicense& License = Manifest.License;

	// 三种情形必须分开：「清单里写了不可商用」「清单里写了可商用」「清单压根没写」。
	// 第三种不能当成可商用——没依据就别替用户做这个判断，原样说「没声明」。
	if (!License.bKnown)
	{
		return SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Top)
			.Padding(FMargin(0.0f, 1.0f, 6.0f, 0.0f))
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.Help"))
				.ColorAndOpacity(FStyleColors::AccentGray)
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("LicenseUnknown", "License: the manifest does not declare one. Do not assume it allows commercial use; check it yourself before using it."))
				.ColorAndOpacity(FStyleColors::AccentGray)
				.AutoWrapText(true)
			];
	}

	if (License.bCommercialUse)
	{
		return SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Top)
			.Padding(FMargin(0.0f, 1.0f, 6.0f, 0.0f))
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.Success"))
				.ColorAndOpacity(FStyleColors::Success)
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(STextBlock)
				.Text(License.Reason.IsEmpty()
					? LOCTEXT("LicenseCommercial", "License: commercial use allowed.")
					: FText::Format(LOCTEXT("LicenseCommercialReason", "License: commercial use allowed. {0}"), FText::FromString(License.Reason)))
				.ColorAndOpacity(FStyleColors::Foreground)
				.AutoWrapText(true)
			];
	}

	// 不可商用：整块用警告色底 + 左侧色条 + 三角图标 + 加粗中文，四重提示——
	// CHORD 的许可是 research-only，这套材质拿去做商业项目会出事，必须醒目。
	const FLinearColor WarnColor = FStyleColors::Warning.GetSpecifiedColor();

	return SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
		.BorderBackgroundColor(FLinearColor(WarnColor.R, WarnColor.G, WarnColor.B, 0.16f))
		.Padding(FMargin(0.0f))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
				.BorderBackgroundColor(WarnColor)
				.Padding(FMargin(2.0f, 0.0f))
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.Padding(FMargin(8.0f, 6.0f))
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.Warning"))
						.ColorAndOpacity(FStyleColors::Warning)
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("LicenseResearchOnly", "Research use only"))
						.Font(FAppStyle::Get().GetFontStyle("NormalFontBold"))
						.ColorAndOpacity(FStyleColors::Warning)
					]
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("LicenseNoCommercial", "Not for commercial use."))
					.ColorAndOpacity(FStyleColors::Foreground)
					.AutoWrapText(true)
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 2.0f, 0.0f, 0.0f))
				[
					// 原因是清单里带的原文（如 "CHORD: Ubisoft ML License (research-only)"），不翻译不加工。
					SNew(STextBlock)
					.Text(FText::FromString(License.Reason))
					.Visibility(License.Reason.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
					.ColorAndOpacity(FStyleColors::AccentGray)
					.AutoWrapText(true)
				]
			]
		];
}

TSharedRef<SWidget> SDreamTextureLibraryPanel::MakeDetailPlaceholder()
{
	return SNew(SBox)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.Padding(FMargin(16.0f))
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 6.0f))
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.Info"))
				.ColorAndOpacity(FStyleColors::AccentGray)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("DetailPlaceholder", "Select a material set to see its details"))
				.ColorAndOpacity(FStyleColors::AccentGray)
				.Justification(ETextJustify::Center)
			]
		];
}

TSharedRef<SWidget> SDreamTextureLibraryPanel::MakeCloudSourceBox()
{
	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Top)
		.Padding(FMargin(0.0f, 1.0f, 6.0f, 0.0f))
		[
			SNew(SImage)
			.Image(FAppStyle::GetBrush("Icons.Cloud"))
			.ColorAndOpacity(FStyleColors::AccentBlue)
		]

		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("CloudSource", "The base image came from the cloud and cannot be reproduced with the same parameters (the cloud API does not support seeds)."))
			.ColorAndOpacity(FStyleColors::AccentBlue)
			.AutoWrapText(true)
		];
}

TSharedRef<SWidget> SDreamTextureLibraryPanel::BuildDetailContent()
{
	const FDreamTextureLibraryItemPtr Item = Model->FindItem(Model->GetSelectedId());
	if (!Item.IsValid())
	{
		return MakeDetailPlaceholder();
	}

	const FDreamTextureMaterialSummary& Summary = Item->Summary;
	const EDreamTextureDetailState State = Model->GetDetailState();
	const FDreamTextureManifest& Manifest = Model->GetDetailManifest();

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	// ── 大图 ──
	Box->AddSlot()
	.AutoHeight()
	.HAlign(HAlign_Center)
	.Padding(FMargin(0.0f, 0.0f, 0.0f, 8.0f))
	[
		MakeThumbnail(Summary.Id, DetailThumbSize)
	];

	// ── 名字 + 收藏 ──
	Box->AddSlot()
	.AutoHeight()
	.Padding(FMargin(0.0f, 0.0f, 0.0f, 8.0f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Summary.Name.IsEmpty() ? Summary.Id : Summary.Name))
			.Font(FAppStyle::Get().GetFontStyle("NormalFontBold"))
			.AutoWrapText(true)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			MakeFavoriteButton(Item)
		]
	];

	// ── 导入 ──
	Box->AddSlot()
	.AutoHeight()
	.Padding(FMargin(0.0f, 0.0f, 0.0f, 8.0f))
	[
		MakeImportControls(Summary.Id)
	];

	// ── 材质实例：导入完成后才有内容，期间折叠 ──
	Box->AddSlot()
	.AutoHeight()
	.Padding(FMargin(0.0f, 0.0f, 0.0f, 8.0f))
	[
		MakeMaterialInstanceSection(Summary.Id)
	];

	// ── 许可与云端提示：manifest 异步来，三种状态各给一个说法 ──
	if (State == EDreamTextureDetailState::Loading)
	{
		Box->AddSlot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 0.0f, 0.0f, 8.0f))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
			[
				SNew(SCircularThrobber)
				.Radius(7.0f)
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ManifestLoading", "Reading the license information..."))
				.ColorAndOpacity(FStyleColors::AccentGray)
			]
		];
	}
	else if (State == EDreamTextureDetailState::Failed)
	{
		Box->AddSlot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 0.0f, 0.0f, 8.0f))
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Top)
				.Padding(FMargin(0.0f, 1.0f, 6.0f, 0.0f))
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.X"))
					.ColorAndOpacity(FStyleColors::Error)
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					// 没拿到许可信息要明说，不能静默地什么都不显示：
					// 空白会被读成「没有限制」。
					SNew(STextBlock)
					.Text(FText::Format(LOCTEXT("ManifestFailed", "The license information could not be read. Check for yourself whether commercial use is allowed before importing: {0}"),
						FText::FromString(Model->GetDetailError())))
					.ColorAndOpacity(FStyleColors::Error)
					.AutoWrapText(true)
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Left)
			.Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.ContentPadding(FMargin(8.0f, 0.0f))
				.OnClicked_Lambda([this]()
				{
					Model->RetryDetail();
					return FReply::Handled();
				})
				[
					SNew(STextBlock)
					.Text(LOCTEXT("RetryManifest", "Retry"))
				]
			]
		];
	}
	else if (State == EDreamTextureDetailState::Ready)
	{
		Box->AddSlot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 0.0f, 0.0f, 8.0f))
		[
			MakeLicenseBox(Manifest)
		];

		if (Manifest.bHasCloudSource)
		{
			Box->AddSlot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 8.0f))
			[
				MakeCloudSourceBox()
			];
		}
	}

	Box->AddSlot()
	.AutoHeight()
	.Padding(FMargin(0.0f, 0.0f, 0.0f, 8.0f))
	[
		SNew(SSeparator)
		.Thickness(1.0f)
	];

	// ── 字段 ──
	auto AddRow = [&Box, this](const FText& Label, const FText& Value)
	{
		Box->AddSlot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 0.0f, 0.0f, 4.0f))
		[
			MakeFieldRow(Label, Value)
		];
	};

	AddRow(LOCTEXT("FieldStyle", "Style"), StyleLabel(Summary.Style));

	AddRow(LOCTEXT("FieldPrompt", "Prompt"),
		Summary.Prompt.IsEmpty() ? LOCTEXT("None", "None") : FText::FromString(Summary.Prompt));

	if (State == EDreamTextureDetailState::Ready && !Manifest.Negative.IsEmpty())
	{
		AddRow(LOCTEXT("FieldNegative", "Negative prompt"), FText::FromString(Manifest.Negative));
	}

	// 种子可能大到 2^53 量级，别走 FText::AsNumber 的本地化分组（会带逗号，复制出去没法用）。
	AddRow(LOCTEXT("FieldSeed", "Seed"), FText::FromString(FString::Printf(TEXT("%lld"), Summary.Seed)));

	{
		FText Resolution = Summary.Resolution > 0
			? FText::Format(LOCTEXT("ResolutionFmt", "{0}×{0}"), FText::AsNumber(Summary.Resolution, &FNumberFormattingOptions::DefaultNoGrouping()))
			: LOCTEXT("ResolutionUnknown", "Unknown");
		if (State == EDreamTextureDetailState::Ready)
		{
			Resolution = FText::Format(LOCTEXT("ResolutionTileFmt", "{0} · {1}"), Resolution,
				Manifest.bTileable ? LOCTEXT("Tileable", "Tileable") : LOCTEXT("NotTileable", "Seamless tiling not guaranteed"));
		}
		AddRow(LOCTEXT("FieldResolution", "Resolution"), Resolution);
	}

	AddRow(LOCTEXT("FieldCreated", "Created"), FormatCreatedAt(Summary.CreatedAt));

	{
		FString Workflow = Summary.WorkflowId;
		if (State == EDreamTextureDetailState::Ready && Manifest.WorkflowVersion > 0)
		{
			Workflow += FString::Printf(TEXT(" v%d"), Manifest.WorkflowVersion);
		}
		if (!Workflow.IsEmpty())
		{
			AddRow(LOCTEXT("FieldWorkflow", "Workflow"), FText::FromString(Workflow));
		}
	}

	{
		// Tags 是 TArray，后端的 null 在解析层已经容忍成空数组。
		const FString Joined = FString::Join(Summary.Tags, *LOCTEXT("TagSeparator", ", ").ToString());
		AddRow(LOCTEXT("FieldTags", "Tags"), Joined.IsEmpty() ? LOCTEXT("None2", "None") : FText::FromString(Joined));
	}

	AddRow(LOCTEXT("FieldId", "Material set ID"), FText::FromString(Summary.Id));

	return SNew(SScrollBox)
		+ SScrollBox::Slot()
		.Padding(FMargin(12.0f))
		[
			Box
		];
}

//////////////////////////////////////////////////////////////////////////
// 模型事件

void SDreamTextureLibraryPanel::HandleListChanged()
{
	if (TileView.IsValid())
	{
		TileView->RequestListRefresh();

		// 模型因为选中项消失而清了选择：网格上的高亮也要跟着清，
		// 否则会留着一个高亮但详情是空的格子。
		if (Model->GetSelectedId().IsEmpty() && TileView->GetNumItemsSelected() > 0)
		{
			TileView->ClearSelection();
		}
	}
}

void SDreamTextureLibraryPanel::HandleDetailChanged()
{
	RebuildDetail();
}

void SDreamTextureLibraryPanel::BuildWidgetsForTest()
{
	for (const FDreamTextureLibraryItemPtr& Item : Model->GetItems())
	{
		MakeTileContent(Item);
	}
	RebuildDetail();
	GetViewStateIndex();
	GetErrorHeadline();
	GetConnectionErrorText();
	GetStaleErrorBannerText();
	GetFooterCountText();
	GetLoadMoreText();
}

void SDreamTextureLibraryPanel::HandleNotice(const FText& Message, bool bIsError)
{
	// 通知只在有 Slate 的进程里弹；-nullrhi 的测试里 IsInitialized 可能为假。
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}

	FNotificationInfo Info(Message);
	Info.ExpireDuration = bIsError ? 6.0f : 3.0f;
	Info.bUseSuccessFailIcons = bIsError;
	Info.bFireAndForget = true;
	if (TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
	{
		Item->SetCompletionState(bIsError ? SNotificationItem::CS_Fail : SNotificationItem::CS_None);
	}
}

#undef LOCTEXT_NAMESPACE
