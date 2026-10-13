// Copyright TypeDreamMoon. All Rights Reserved.

#include "SDreamTextureGeneratePanel.h"

#include "DreamTextureSettings.h"
#include "DreamTextureThumbCache.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "ISettingsModule.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "DreamTextureGeneratePanel"

namespace
{
	/** 画面类型 → 中文。后端将来加了新类型：显示原文，不要硬翻成「未知」。 */
	FText DomainLabel(const FString& Domain)
	{
		if (Domain.Equals(TEXT("realistic"), ESearchCase::IgnoreCase))
		{
			return NSLOCTEXT("DreamTextureGeneratePanel", "DomainRealistic", "Realistic");
		}
		if (Domain.Equals(TEXT("stylized"), ESearchCase::IgnoreCase))
		{
			return NSLOCTEXT("DreamTextureGeneratePanel", "DomainStylized", "Stylized");
		}
		return FText::FromString(Domain);
	}

	FSlateColor KindColor(FDreamTextureGenerateRowView::EKind Kind)
	{
		using EKind = FDreamTextureGenerateRowView::EKind;
		switch (Kind)
		{
		case EKind::Done:
			return FStyleColors::Success;
		case EKind::Failed:
			return FStyleColors::Error;
		case EKind::Running:
		case EKind::Canceling:
			return FStyleColors::AccentBlue;
		case EKind::Waiting:
		case EKind::Canceled:
		case EKind::Unknown:
		default:
			return FStyleColors::AccentGray;
		}
	}
}

//////////////////////////////////////////////////////////////////////////
// 构造与析构

void SDreamTextureGeneratePanel::Construct(const FArguments& InArgs)
{
	Model = InArgs._Model;
	if (!Model.IsValid())
	{
		Model = MakeShared<FDreamTextureGenerateModel>();
	}
	Model->Initialize(InArgs._bBindExternal);
	ThumbCache = MakeShared<FDreamTextureThumbCache>();

	FormChangedHandle = Model->OnFormChanged.AddSP(this, &SDreamTextureGeneratePanel::HandleFormChanged);
	BatchChangedHandle = Model->OnBatchChanged.AddSP(this, &SDreamTextureGeneratePanel::HandleBatchChanged);
	RefineChangedHandle = Model->OnRefineChanged.AddSP(this, &SDreamTextureGeneratePanel::HandleRefineChanged);
	PromptReplacedHandle = Model->OnPromptReplaced.AddSP(this, &SDreamTextureGeneratePanel::HandlePromptReplaced);
	NoticeHandle = Model->OnNotice.AddSP(this, &SDreamTextureGeneratePanel::HandleNotice);

	ChildSlot
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			MakeLeftColumn()
		]

		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			MakeRightColumn()
		]
	];

	RebuildOptions();
	RebuildCards();

	if (InArgs._bAutoLoad)
	{
		Model->LoadCatalog();
		Model->LoadRefineModels();
	}
}

SDreamTextureGeneratePanel::~SDreamTextureGeneratePanel()
{
	// 模型可能比面板活得久（测试里是外面持有的），所以一定要摘。
	if (Model.IsValid())
	{
		Model->OnFormChanged.Remove(FormChangedHandle);
		Model->OnBatchChanged.Remove(BatchChangedHandle);
		Model->OnRefineChanged.Remove(RefineChangedHandle);
		Model->OnPromptReplaced.Remove(PromptReplacedHandle);
		Model->OnNotice.Remove(NoticeHandle);
	}
}

//////////////////////////////////////////////////////////////////////////
// 左栏

SDreamTextureGeneratePanel::EFormState SDreamTextureGeneratePanel::GetFormState() const
{
	if (Model->GetCatalogState() == FDreamTextureGenerateModel::ECatalogState::Ready)
	{
		return EFormState::Form;
	}

	// 后端没应答、又配了可执行文件：目录当然拉不到，但这时候要的是写提示词、点「启动后端并生成」，
	// 而不是被一句「读不到工作流列表」挡在表单外面。启动流程走着的时候也留在表单上，别闪成转圈再闪回来。
	if (Model->IsAutoStarting() || Model->ShouldShowOfflineForm())
	{
		return EFormState::Form;
	}

	switch (Model->GetCatalogState())
	{
	case FDreamTextureGenerateModel::ECatalogState::Failed:
		return EFormState::Failed;
	case FDreamTextureGenerateModel::ECatalogState::NotLoaded:
	case FDreamTextureGenerateModel::ECatalogState::Loading:
	default:
		return EFormState::Loading;
	}
}

int32 SDreamTextureGeneratePanel::GetFormStateIndex() const
{
	switch (GetFormState())
	{
	case EFormState::Form:    return 0;
	case EFormState::Loading: return 1;
	case EFormState::Failed:
	default:                  return 2;
	}
}

TSharedRef<SWidget> SDreamTextureGeneratePanel::MakeLeftColumn()
{
	// 栏定宽是刻意的：表单行宽一旦跟着窗口变，下拉和输入框的比例就会在不同屏幕上完全不一样；
	// 右栏吃掉剩余空间去排变体卡片。
	return SNew(SBox)
		.WidthOverride(LeftColumnWidth)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
			.Padding(FMargin(0.0f))
			[
				SNew(SWidgetSwitcher)
				.WidgetIndex(this, &SDreamTextureGeneratePanel::GetFormStateIndex)

				+ SWidgetSwitcher::Slot()
				[
					MakeForm()
				]

				+ SWidgetSwitcher::Slot()
				[
					MakeLoadingState()
				]

				+ SWidgetSwitcher::Slot()
				[
					MakeFailedState()
				]
			]
		];
}

TSharedRef<SWidget> SDreamTextureGeneratePanel::MakeLoadingState()
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
				.Text(LOCTEXT("CatalogLoading", "Reading the workflow list…"))
				.ColorAndOpacity(FStyleColors::Foreground)
			]
		];
}

TSharedRef<SWidget> SDreamTextureGeneratePanel::MakeFailedState()
{
	return SNew(SBox)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.Padding(FMargin(20.0f))
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
					// Bad 用 Icons.X 而不是 Icons.Error：后者在 Starship 里和 Icons.Warning 是同一张三角。
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.X"))
					.ColorAndOpacity(FStyleColors::Error)
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("CatalogFailedTitle", "Could not read the workflow list"))
					.Font(FAppStyle::Get().GetFontStyle("NormalFontBold"))
					.ColorAndOpacity(FStyleColors::Error)
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 12.0f))
			[
				SNew(STextBlock)
				.Text(this, &SDreamTextureGeneratePanel::GetCatalogErrorText)
				.AutoWrapText(true)
				.Justification(ETextJustify::Center)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			[
				SNew(SButton)
				.ContentPadding(FMargin(12.0f, 2.0f))
				.OnClicked(this, &SDreamTextureGeneratePanel::OnRetryCatalogClicked)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Retry", "Retry"))
				]
			]

			// 后端进程不通、又没配可执行文件：点「重试」没用，出路在设置里。
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(FMargin(0.0f, 8.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.ContentPadding(FMargin(12.0f, 2.0f))
				.Visibility(this, &SDreamTextureGeneratePanel::GetSettingsLinkVisibility)
				.ToolTipText(LOCTEXT("OpenSettingsFailedTip", "The backend process is not responding and no backend executable is set, so the plugin cannot launch it. Once one is set in Project Settings, this button can start the backend in one click."))
				.OnClicked(this, &SDreamTextureGeneratePanel::OnOpenSettingsClicked)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("OpenSettings", "Open Project Settings"))
				]
			]
		];
}

TSharedRef<SWidget> SDreamTextureGeneratePanel::MakeLabel(const FText& Text)
{
	return SNew(STextBlock)
		.Text(Text)
		.ColorAndOpacity(FStyleColors::AccentGray);
}

TSharedRef<SWidget> SDreamTextureGeneratePanel::MakeOptionWidget(FOptionPtr Item)
{
	if (!Item.IsValid())
	{
		return SNullWidget::NullWidget;
	}

	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(Item->Label)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(8.0f, 0.0f, 0.0f, 0.0f))
		[
			SNew(STextBlock)
			.Text(Item->Tag)
			.Visibility(Item->Tag.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
			.ColorAndOpacity(Item->bWarn ? FStyleColors::Warning : FStyleColors::AccentGray)
		];
}

TSharedRef<SWidget> SDreamTextureGeneratePanel::MakeForm()
{
	TWeakPtr<FDreamTextureGenerateModel> WeakModel = Model;

	return SNew(SScrollBox)

		+ SScrollBox::Slot()
		.Padding(FMargin(12.0f))
		[
			SNew(SVerticalBox)

			// ── 出图模型 ──
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				MakeLabel(LOCTEXT("SourceLabel", "Image model"))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 3.0f, 0.0f, 0.0f))
			[
				SAssignNew(SourceCombo, SComboBox<FOptionPtr>)
				.OptionsSource(&SourceOptions)
				.OnGenerateWidget(this, &SDreamTextureGeneratePanel::MakeOptionWidget)
				.OnSelectionChanged(this, &SDreamTextureGeneratePanel::OnSourcePicked)
				.ToolTipText(LOCTEXT("SourceTip", "Where the source image comes from: a local SDXL model, or a cloud image model. Cloud models are billed per use."))
				[
					SNew(STextBlock)
					.Text(this, &SDreamTextureGeneratePanel::GetSourceLabel)
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
			[
				SNew(STextBlock)
				.Text(this, &SDreamTextureGeneratePanel::GetSourceNote)
				.Visibility(this, &SDreamTextureGeneratePanel::GetSourceNoteVisibility)
				.ColorAndOpacity(FStyleColors::AccentBlue)
				.AutoWrapText(true)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 8.0f, 0.0f, 0.0f))
			[
				MakeCloudBlock()
			]

			// ── 分解模型 ──
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 10.0f, 0.0f, 0.0f))
			[
				MakeLabel(LOCTEXT("DecomposeLabel", "Decompose model"))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 3.0f, 0.0f, 0.0f))
			[
				SAssignNew(DecomposeCombo, SComboBox<FOptionPtr>)
				.OptionsSource(&DecomposeOptions)
				.OnGenerateWidget(this, &SDreamTextureGeneratePanel::MakeOptionWidget)
				.OnSelectionChanged(this, &SDreamTextureGeneratePanel::OnDecomposePicked)
				.ToolTipText(LOCTEXT("DecomposeTip", "The step that splits the source image into Base Color, Normal, Roughness, Metallic, Ambient Occlusion and Height."))
				[
					SNew(STextBlock)
					.Text(this, &SDreamTextureGeneratePanel::GetDecomposeLabel)
				]
			]

			// 两段搭不上只提醒、不拦：手绘图对 CHORD 是分布外输入，法线会糊，但要不要试是用户的事。
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
			[
				SNew(STextBlock)
				.Text(this, &SDreamTextureGeneratePanel::GetMismatchText)
				.Visibility(this, &SDreamTextureGeneratePanel::GetMismatchVisibility)
				.ColorAndOpacity(FStyleColors::AccentGray)
				.AutoWrapText(true)
			]

			// ── 提示词 ──
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 12.0f, 0.0f, 0.0f))
			[
				MakeLabel(LOCTEXT("PromptLabel", "Prompt"))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 3.0f, 0.0f, 0.0f))
			[
				SNew(SBox)
				.MinDesiredHeight(96.0f)
				.MaxDesiredHeight(240.0f)
				[
					SAssignNew(PromptBox, SMultiLineEditableTextBox)
					.Text(FText::FromString(Model->GetPrompt()))
					.HintText(LOCTEXT("PromptHint", "For example: cracked ancient rock covered in moss"))
					.AutoWrapText(true)
					.OnTextChanged(this, &SDreamTextureGeneratePanel::OnPromptTextChanged)
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
			[
				MakeRefineArea()
			]

			// ── 分辨率 / 变体数 ──
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 12.0f, 0.0f, 0.0f))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
				[
					SNew(SVerticalBox)
					.Visibility(this, &SDreamTextureGeneratePanel::GetResolutionVisibility)

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						MakeLabel(LOCTEXT("ResolutionLabel", "Resolution"))
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.0f, 3.0f, 0.0f, 0.0f))
					[
						SAssignNew(ResolutionCombo, SComboBox<FOptionPtr>)
						.OptionsSource(&ResolutionOptions)
						.OnGenerateWidget(this, &SDreamTextureGeneratePanel::MakeOptionWidget)
						.OnSelectionChanged(this, &SDreamTextureGeneratePanel::OnResolutionPicked)
						.ToolTipText(LOCTEXT("ResolutionTip", "Texture edge length in pixels. The options come from the selected pipeline; CHORD works best at 1024."))
						[
							SNew(STextBlock)
							.Text(this, &SDreamTextureGeneratePanel::GetResolutionLabel)
						]
					]
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
				[
					SNew(SVerticalBox)

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						MakeLabel(LOCTEXT("VariantsLabel", "Variants (1–8)"))
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.0f, 3.0f, 0.0f, 0.0f))
					[
						SNew(SSpinBox<int32>)
						.MinValue(1)
						.MaxValue(DreamTextureGenerateLimits::MaxVariants)
						.MinSliderValue(1)
						.MaxSliderValue(DreamTextureGenerateLimits::MaxVariants)
						.Value_Lambda([WeakModel]() -> int32
						{
							const TSharedPtr<FDreamTextureGenerateModel> Pinned = WeakModel.Pin();
							return Pinned.IsValid() ? Pinned->GetVariants() : 1;
						})
						.OnValueChanged_Lambda([WeakModel](int32 NewValue)
						{
							if (const TSharedPtr<FDreamTextureGenerateModel> Pinned = WeakModel.Pin())
							{
								Pinned->SetVariants(NewValue);
							}
						})
						.ToolTipText(LOCTEXT("VariantsTip", "Generating several at once and picking the best is faster than retrying one at a time. Variants run one after another, so a larger number means a longer wait."))
					]
				]
			]

			// ── 种子 ──
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 12.0f, 0.0f, 0.0f))
			[
				MakeSeedRow()
			]

			// ── 自动导入 ──
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 12.0f, 0.0f, 0.0f))
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([WeakModel]()
				{
					const TSharedPtr<FDreamTextureGenerateModel> Pinned = WeakModel.Pin();
					return (Pinned.IsValid() && Pinned->GetAutoImport()) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([WeakModel](ECheckBoxState NewState)
				{
					if (const TSharedPtr<FDreamTextureGenerateModel> Pinned = WeakModel.Pin())
					{
						Pinned->SetAutoImport(NewState == ECheckBoxState::Checked);
					}
				})
				.ToolTipText(LOCTEXT("AutoImportTip", "Import each variant into the project as soon as it finishes generating (the same path as the Import button in the Library). Off by default: you generate several to choose from, and importing all of them leaves a pile of assets you do not want. You can also change this in Project Settings."))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("AutoImport", "Import automatically when done"))
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 12.0f, 0.0f, 0.0f))
			[
				SNew(SSeparator)
				.Thickness(1.0f)
			]

			// ── 许可 + 提交 ──
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 12.0f, 0.0f, 0.0f))
			[
				MakeLicenseBox()
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 8.0f, 0.0f, 0.0f))
			[
				MakeSubmitArea()
			]
		];
}

TSharedRef<SWidget> SDreamTextureGeneratePanel::MakeCloudBlock()
{
	// 只在选了云端底图的出图模型时出现。云端出图要先选一个云端模型（后端在提交时拦下没选的），
	// 要有令牌、按量计费；这里把这几件事在提交之前就摆清楚，而不是等后端回一个 400。
	return SNew(SVerticalBox)
		.Visibility(this, &SDreamTextureGeneratePanel::GetCloudVisibility)

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeLabel(LOCTEXT("CloudModelLabel", "Cloud model"))
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 3.0f, 0.0f, 0.0f))
		[
			SAssignNew(CloudModelCombo, SComboBox<FOptionPtr>)
			.OptionsSource(&CloudModelOptions)
			.OnGenerateWidget(this, &SDreamTextureGeneratePanel::MakeOptionWidget)
			.OnSelectionChanged(this, &SDreamTextureGeneratePanel::OnCloudModelPicked)
			.ToolTipText(LOCTEXT("CloudModelTip", "The cloud model that produces the source image. The list is fetched from the server on demand, with recognized and recommended models first."))
			[
				SNew(STextBlock)
				.Text(this, &SDreamTextureGeneratePanel::GetCloudModelLabel)
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
		]

		// 没填令牌 / 连不上 / 清单还没到：说清楚，并给重新读取。
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
		[
			SNew(SHorizontalBox)
			.Visibility(this, &SDreamTextureGeneratePanel::GetCloudStatusVisibility)

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SDreamTextureGeneratePanel::GetCloudStatusText)
				.ColorAndOpacity(FStyleColors::Warning)
				.AutoWrapText(true)
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.ContentPadding(FMargin(8.0f, 0.0f))
				.Visibility(this, &SDreamTextureGeneratePanel::GetCloudRetryVisibility)
				.OnClicked(this, &SDreamTextureGeneratePanel::OnRetryCloudClicked)
				.ToolTipText(LOCTEXT("CloudRetryTip", "Reload the cloud model list. Click it after entering an API key in the web UI."))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("CloudRetry", "Reload"))
				]
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 8.0f, 0.0f, 0.0f))
		[
			SNew(SVerticalBox)
			.Visibility(this, &SDreamTextureGeneratePanel::GetCloudQualityVisibility)

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				MakeLabel(LOCTEXT("CloudQualityLabel", "Quality"))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 3.0f, 0.0f, 0.0f))
			[
				SAssignNew(CloudQualityCombo, SComboBox<FOptionPtr>)
				.OptionsSource(&CloudQualityOptions)
				.OnGenerateWidget(this, &SDreamTextureGeneratePanel::MakeOptionWidget)
				.OnSelectionChanged(this, &SDreamTextureGeneratePanel::OnCloudQualityPicked)
				.ToolTipText(LOCTEXT("CloudQualityTip", "Higher quality costs more and takes longer. The high setting takes two to three minutes per image, which is slower than the whole local pipeline, so try prompts with medium first."))
				[
					SNew(STextBlock)
					.Text(this, &SDreamTextureGeneratePanel::GetCloudQualityLabel)
				]
			]
		]

		// 变体数直接乘上去：每个变体一次云端调用。不给具体金额——实际按 token 结算，报精确到分的数字只会被当成账单。
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
		[
			SNew(STextBlock)
			.Text(this, &SDreamTextureGeneratePanel::GetCloudCostText)
			.ColorAndOpacity(FStyleColors::AccentGray)
			.AutoWrapText(true)
		];
}

TSharedRef<SWidget> SDreamTextureGeneratePanel::MakeRefineArea()
{
	TWeakPtr<FDreamTextureGenerateModel> WeakModel = Model;

	return SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
			[
				SNew(SButton)
				.ContentPadding(FMargin(10.0f, 1.0f))
				.IsEnabled_Lambda([WeakModel]()
				{
					const TSharedPtr<FDreamTextureGenerateModel> Pinned = WeakModel.Pin();
					return Pinned.IsValid() && Pinned->GetRefineState() != FDreamTextureGenerateModel::ERefineState::Working;
				})
				.OnClicked(this, &SDreamTextureGeneratePanel::OnRefineClicked)
				.ToolTipText(LOCTEXT("RefineTip", "Ask a text model to expand this sentence into a prompt suited to image generation, adding the hard constraints a texture needs: orthographic top-down view, flat lighting without shadows, and tileable. The result is shown first, and you decide whether to use it."))
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
						.Visibility_Lambda([WeakModel]()
						{
							const TSharedPtr<FDreamTextureGenerateModel> Pinned = WeakModel.Pin();
							return (Pinned.IsValid() && Pinned->GetRefineState() == FDreamTextureGenerateModel::ERefineState::Working)
								? EVisibility::Visible : EVisibility::Collapsed;
						})
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text_Lambda([WeakModel]()
						{
							const TSharedPtr<FDreamTextureGenerateModel> Pinned = WeakModel.Pin();
							return (Pinned.IsValid() && Pinned->GetRefineState() == FDreamTextureGenerateModel::ERefineState::Working)
								? LOCTEXT("Refining", "Refining…") : LOCTEXT("Refine", "Refine prompt");
						})
					]
				]
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SAssignNew(RefineModelCombo, SComboBox<FOptionPtr>)
				.OptionsSource(&RefineModelOptions)
				.OnGenerateWidget(this, &SDreamTextureGeneratePanel::MakeOptionWidget)
				.OnSelectionChanged(this, &SDreamTextureGeneratePanel::OnRefineModelPicked)
				.ToolTipText(LOCTEXT("RefineModelTip", "The text model used to refine the prompt. When the list cannot be fetched, only Default is available."))
				[
					SNew(STextBlock)
					.Text(this, &SDreamTextureGeneratePanel::GetRefineModelLabel)
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				]
			]
		]

		// 扩写失败的原因（网关不提供对话接口、令牌没配…）。
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
		[
			SNew(STextBlock)
			.Text_Lambda([WeakModel]()
			{
				const TSharedPtr<FDreamTextureGenerateModel> Pinned = WeakModel.Pin();
				return Pinned.IsValid() ? FText::FromString(Pinned->GetRefineError()) : FText::GetEmpty();
			})
			.Visibility_Lambda([WeakModel]()
			{
				const TSharedPtr<FDreamTextureGenerateModel> Pinned = WeakModel.Pin();
				return (Pinned.IsValid() && !Pinned->GetRefineError().IsEmpty()) ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.ColorAndOpacity(FStyleColors::Error)
			.AutoWrapText(true)
		]

		// 扩写结果：摆出来让人看一眼再决定。
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			.Padding(FMargin(8.0f))
			.Visibility_Lambda([WeakModel]()
			{
				const TSharedPtr<FDreamTextureGenerateModel> Pinned = WeakModel.Pin();
				return (Pinned.IsValid() && Pinned->GetRefineState() == FDreamTextureGenerateModel::ERefineState::HasResult)
					? EVisibility::Visible : EVisibility::Collapsed;
			})
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text_Lambda([WeakModel]()
					{
						const TSharedPtr<FDreamTextureGenerateModel> Pinned = WeakModel.Pin();
						return Pinned.IsValid()
							? FText::Format(LOCTEXT("RefineResultTitle", "Refined prompt ({0})"), FText::FromString(Pinned->GetRefinedModel()))
							: FText::GetEmpty();
					})
					.ColorAndOpacity(FStyleColors::AccentGray)
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 4.0f, 0.0f, 6.0f))
				[
					SNew(SBox)
					.MaxDesiredHeight(200.0f)
					[
						SNew(SScrollBox)

						+ SScrollBox::Slot()
						[
							SNew(STextBlock)
							.Text_Lambda([WeakModel]()
							{
								const TSharedPtr<FDreamTextureGenerateModel> Pinned = WeakModel.Pin();
								return Pinned.IsValid() ? FText::FromString(Pinned->GetRefinedPrompt()) : FText::GetEmpty();
							})
							.AutoWrapText(true)
						]
					]
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
						.ContentPadding(FMargin(10.0f, 1.0f))
						.OnClicked(this, &SDreamTextureGeneratePanel::OnAcceptRefineClicked)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("AcceptRefine", "Use it"))
						]
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.ContentPadding(FMargin(10.0f, 1.0f))
						.OnClicked(this, &SDreamTextureGeneratePanel::OnDiscardRefineClicked)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("DiscardRefine", "Discard"))
						]
					]
				]
			]
		];
}

TSharedRef<SWidget> SDreamTextureGeneratePanel::MakeSeedRow()
{
	TWeakPtr<FDreamTextureGenerateModel> WeakModel = Model;

	return SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeLabel(LOCTEXT("SeedLabel", "Seed"))
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 3.0f, 0.0f, 0.0f))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				SNew(SSegmentedControl<bool>)
				.Value_Lambda([WeakModel]()
				{
					const TSharedPtr<FDreamTextureGenerateModel> Pinned = WeakModel.Pin();
					return !Pinned.IsValid() || Pinned->IsRandomSeed();
				})
				.OnValueChanged_Lambda([WeakModel](bool bRandom)
				{
					if (const TSharedPtr<FDreamTextureGenerateModel> Pinned = WeakModel.Pin())
					{
						Pinned->SetRandomSeed(bRandom);
					}
				})

				+ SSegmentedControl<bool>::Slot(true)
				.Text(LOCTEXT("SeedRandom", "Random"))
				.ToolTip(LOCTEXT("SeedRandomTip", "Each variant gets its own random seed."))

				+ SSegmentedControl<bool>::Slot(false)
				.Text(LOCTEXT("SeedFixed", "Fixed"))
				.ToolTip(LOCTEXT("SeedFixedTip", "Reproduce a result with the seed you enter. Variants in one batch use the seed, seed+1, seed+2 and so on, so they differ from each other."))
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				// 上限和 Client 校验是同一个口径（见 FDreamTextureGenerateRequest::MaxFixedSeed）：
				// 后端用 float64 存种子，超过 2^53 会被悄悄改写，manifest 里记的种子就复现不出原图了。
				SNew(SNumericEntryBox<int64>)
				.AllowSpin(false)
				.MinValue(TOptional<int64>(0))
				.MaxValue(TOptional<int64>(FDreamTextureGenerateModel::GetMaxSeedForUI()))
				.IsEnabled_Lambda([WeakModel]()
				{
					const TSharedPtr<FDreamTextureGenerateModel> Pinned = WeakModel.Pin();
					return Pinned.IsValid() && !Pinned->IsRandomSeed();
				})
				.Value_Lambda([WeakModel]() -> TOptional<int64>
				{
					const TSharedPtr<FDreamTextureGenerateModel> Pinned = WeakModel.Pin();
					return Pinned.IsValid() ? TOptional<int64>(Pinned->GetFixedSeed()) : TOptional<int64>();
				})
				.OnValueCommitted_Lambda([WeakModel](int64 NewValue, ETextCommit::Type)
				{
					if (const TSharedPtr<FDreamTextureGenerateModel> Pinned = WeakModel.Pin())
					{
						Pinned->SetFixedSeed(NewValue);
					}
				})
				.ToolTipText(FText::Format(LOCTEXT("SeedBoxTip", "0 to {0}. The backend stores seeds as float64, so larger values would be silently truncated."),
					FText::FromString(FString::Printf(TEXT("%lld"), FDreamTextureGenerateModel::GetMaxSeedForUI()))))
			]
		];
}

TSharedRef<SWidget> SDreamTextureGeneratePanel::MakeLicenseBox()
{
	// 不可商用：整块用警告色底 + 左侧色条 + 三角图标 + 加粗中文，四重提示——
	// CHORD 的许可是 research-only，这套材质拿去做商业项目会出事，必须醒目。
	// 只在选了不可商用的组合时才显示；可商用或没有许可限制的组合不占地方。
	const FLinearColor WarnColor = FStyleColors::Warning.GetSpecifiedColor();

	return SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
		.BorderBackgroundColor(FLinearColor(WarnColor.R, WarnColor.G, WarnColor.B, 0.16f))
		.Padding(FMargin(0.0f))
		.Visibility(this, &SDreamTextureGeneratePanel::GetLicenseVisibility)
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
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(this, &SDreamTextureGeneratePanel::GetLicenseHeadline)
						.Font(FAppStyle::Get().GetFontStyle("NormalFontBold"))
						.ColorAndOpacity(FStyleColors::Warning)
						.AutoWrapText(true)
					]
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
				[
					// 原因是后端带来的原文（"CHORD (…): Ubisoft Machine Learning License (Research-Only, Copyleft)"），不翻译不加工。
					SNew(STextBlock)
					.Text(this, &SDreamTextureGeneratePanel::GetLicenseReasonText)
					.ColorAndOpacity(FStyleColors::Foreground)
					.AutoWrapText(true)
				]
			]
		];
}

TSharedRef<SWidget> SDreamTextureGeneratePanel::MakeSubmitArea()
{
	return SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
				.HAlign(HAlign_Center)
				.ContentPadding(FMargin(12.0f, 4.0f))
				.IsEnabled(this, &SDreamTextureGeneratePanel::IsSubmitEnabled)
				.OnClicked(this, &SDreamTextureGeneratePanel::OnSubmitClicked)
				[
					SNew(STextBlock)
					.Text(this, &SDreamTextureGeneratePanel::GetSubmitText)
				]
			]

			// 启动后端的过程中可以取消：只是不再提交，不去停后端。
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.HAlign(HAlign_Center)
				.ContentPadding(FMargin(12.0f, 4.0f))
				.Visibility(this, &SDreamTextureGeneratePanel::GetCancelAutoStartVisibility)
				.ToolTipText(LOCTEXT("CancelAutoStartTip", "Stop before submitting. A start request that was already sent is not withdrawn; if the backend comes up it is left running, and this does not stop it."))
				.OnClicked(this, &SDreamTextureGeneratePanel::OnCancelAutoStartClicked)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("CancelAutoStart", "Cancel"))
				]
			]
		]

		// 上一次提交 / 启动失败的原因，原样显示（启动失败时就是进程管理器给的那句中文）。
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
		[
			SNew(STextBlock)
			.Text(this, &SDreamTextureGeneratePanel::GetSubmitErrorText)
			.Visibility(this, &SDreamTextureGeneratePanel::GetSubmitErrorVisibility)
			.ColorAndOpacity(FStyleColors::Error)
			.AutoWrapText(true)
		]

		// 提交不了时说明原因，而且点明是三级里的哪一级：后端进程、长连接、ComfyUI 各有各的修法，
		// 混成一句「连不上」会让人去重启错误的东西。
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
		[
			SNew(STextBlock)
			.Text(this, &SDreamTextureGeneratePanel::GetSubmitHintText)
			.Visibility(this, &SDreamTextureGeneratePanel::GetSubmitHintVisibility)
			.ColorAndOpacity(this, &SDreamTextureGeneratePanel::GetSubmitHintColor)
			.AutoWrapText(true)
		]

		// 没配可执行文件：保持上面的说明，另给一个跳到设置的入口。
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Left)
		.Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
		[
			SNew(SButton)
			.ContentPadding(FMargin(10.0f, 1.0f))
			.Visibility(this, &SDreamTextureGeneratePanel::GetSettingsLinkVisibility)
			.ToolTipText(LOCTEXT("OpenSettingsTip", "Set the backend executable in Project Settings. Once it is set, this button becomes Start backend and generate whenever the backend is not running."))
			.OnClicked(this, &SDreamTextureGeneratePanel::OnOpenSettingsClicked)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("OpenSettings", "Open Project Settings"))
			]
		];
}

//////////////////////////////////////////////////////////////////////////
// 右栏

TSharedRef<SWidget> SDreamTextureGeneratePanel::MakeRightColumn()
{
	return SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
			.Padding(FMargin(8.0f, 6.0f))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SDreamTextureGeneratePanel::GetBatchSummaryText)
					.ColorAndOpacity(FStyleColors::Foreground)
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
				[
					SNew(SButton)
					.ContentPadding(FMargin(8.0f, 2.0f))
					.OnClicked(this, &SDreamTextureGeneratePanel::OnSyncClicked)
					.ToolTipText(LOCTEXT("SyncTip", "Ask the backend about each unfinished job. Progress arrives as pushes over the event stream, and pushes are occasionally lost; click this to catch up on the latest state."))
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
							.Text(LOCTEXT("Sync", "Sync"))
						]
					]
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SButton)
					.ContentPadding(FMargin(8.0f, 2.0f))
					.IsEnabled(this, &SDreamTextureGeneratePanel::IsCancelAllEnabled)
					.OnClicked(this, &SDreamTextureGeneratePanel::OnCancelAllClicked)
					.ToolTipText(LOCTEXT("CancelAllTip", "Cancel every unfinished variant in this batch. Queued variants are canceled at once; running ones interrupt ComfyUI."))
					[
						SNew(STextBlock)
						.Text(LOCTEXT("CancelAll", "Cancel all"))
					]
				]
			]
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SOverlay)

			+ SOverlay::Slot()
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				.Visibility(this, &SDreamTextureGeneratePanel::GetEmptyHintVisibility)

				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("EmptyHint", "Nothing has been generated yet."))
					.ColorAndOpacity(FStyleColors::AccentGray)
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				.Padding(FMargin(0.0f, 2.0f, 0.0f, 0.0f))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("EmptyHint2", "Write a prompt on the left and click Generate."))
					.ColorAndOpacity(FStyleColors::AccentGray)
				]
			]

			+ SOverlay::Slot()
			[
				SNew(SScrollBox)
				.Visibility(this, &SDreamTextureGeneratePanel::GetCardsVisibility)

				+ SScrollBox::Slot()
				.Padding(FMargin(10.0f))
				[
					SAssignNew(CardBox, SWrapBox)
					.UseAllottedSize(true)
					.InnerSlotPadding(FVector2D(10.0f, 10.0f))
				]
			]
		];
}

TSharedRef<SWidget> SDreamTextureGeneratePanel::MakeCardThumbnail(const FDreamTextureGenerateModel::FRowPtr& Row, float Size)
{
	// 缓存只被面板持有，属性回调活在控件树里；按弱指针取，面板析构后不会碰到一个死缓存。
	TWeakPtr<FDreamTextureThumbCache> WeakCache = ThumbCache;
	TWeakPtr<FDreamTextureGenerateModel> WeakModel = Model;
	const FString JobId = Row->JobId;

	// 只有完成的任务才有 preview.png：材质目录是任务成功落盘时才建的。
	// 没完成就去问缩略图缓存要画刷，会白白发一个 404 请求并被记成「失败 30 秒」，完成时还得等重试间隔。
	auto GetDoneMaterialId = [WeakModel, JobId]() -> FString
	{
		const TSharedPtr<FDreamTextureGenerateModel> Pinned = WeakModel.Pin();
		const FDreamTextureJob* Job = Pinned.IsValid() ? Pinned->FindJob(JobId) : nullptr;
		return (Job != nullptr && Job->Status == EDreamTextureJobStatus::Succeeded) ? Job->MaterialId : FString();
	};

	FDreamTextureGenerateModel::FRowPtr RowCopy = Row;

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
				.Visibility_Lambda([WeakCache, GetDoneMaterialId]()
				{
					const FString MaterialId = GetDoneMaterialId();
					const TSharedPtr<FDreamTextureThumbCache> Cache = WeakCache.Pin();
					return (!MaterialId.IsEmpty() && Cache.IsValid() && Cache->HasBrush(MaterialId))
						? EVisibility::Collapsed : EVisibility::Visible;
				})
				[
					// 占位要说清楚此刻是什么状态：还在排队/生成，还是已经完成只是预览在路上。
					SNew(STextBlock)
					.Text_Lambda([WeakModel, WeakCache, RowCopy, GetDoneMaterialId]()
					{
						const TSharedPtr<FDreamTextureGenerateModel> Pinned = WeakModel.Pin();
						if (!Pinned.IsValid())
						{
							return FText::GetEmpty();
						}
						const FDreamTextureGenerateRowView View = Pinned->GetRowView(RowCopy);
						if (View.Kind == FDreamTextureGenerateRowView::EKind::Done)
						{
							const FString MaterialId = GetDoneMaterialId();
							const TSharedPtr<FDreamTextureThumbCache> Cache = WeakCache.Pin();
							return (Cache.IsValid() && !MaterialId.IsEmpty() && Cache->IsFailed(MaterialId))
								? LOCTEXT("ThumbMissing", "No preview") : LOCTEXT("ThumbLoading", "Loading…");
						}
						return View.Stage;
					})
					.ColorAndOpacity(FStyleColors::AccentGray)
				]
			]

			+ SOverlay::Slot()
			[
				// 这个 Image 属性回调同时是加载的触发点：每帧问一次缓存要画刷，没有就顺手排一次加载。
				// 所以它不能随占位一起 Collapsed——被折叠的控件不会被绘制，回调就永远不会跑。
				SNew(SImage)
				.Image_Lambda([WeakCache, GetDoneMaterialId]() -> const FSlateBrush*
				{
					const FString MaterialId = GetDoneMaterialId();
					const TSharedPtr<FDreamTextureThumbCache> Cache = WeakCache.Pin();
					return (!MaterialId.IsEmpty() && Cache.IsValid()) ? Cache->GetBrush(MaterialId) : nullptr;
				})
			]
		];
}

TSharedRef<SWidget> SDreamTextureGeneratePanel::MakeRowCard(const FDreamTextureGenerateModel::FRowPtr& Row)
{
	using EKind = FDreamTextureGenerateRowView::EKind;
	using EImport = FDreamTextureGenerateRowView::EImport;

	TWeakPtr<FDreamTextureGenerateModel> WeakModel = Model;
	const FString JobId = Row->JobId;

	// 每帧取一次整张卡片的视图，各控件从里面挑自己要的：状态在追踪器里，进度变化不需要重建任何控件。
	auto Eval = [WeakModel, Row]() -> FDreamTextureGenerateRowView
	{
		const TSharedPtr<FDreamTextureGenerateModel> Pinned = WeakModel.Pin();
		return Pinned.IsValid() ? Pinned->GetRowView(Row) : FDreamTextureGenerateRowView();
	};

	return SNew(SBox)
		.WidthOverride(CardWidth)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
			.Padding(FMargin(6.0f))
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					MakeCardThumbnail(Row, CardThumbSize)
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
				[
					SNew(STextBlock)
					.Text_Lambda([Eval]() { return Eval().Title; })
					.Font(FAppStyle::Get().GetFontStyle("NormalFontBold"))
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 1.0f, 0.0f, 0.0f))
				[
					SNew(STextBlock)
					.Text_Lambda([Eval]() { return Eval().Subtitle; })
					.ColorAndOpacity(FStyleColors::AccentGray)
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				]

				// 阶段：图标形状（转圈 / 对勾 / 叉 / 信息）+ 中文，颜色只是第三重冗余。
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
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
							SNew(SWidgetSwitcher)
							.WidgetIndex_Lambda([Eval]()
							{
								switch (Eval().Kind)
								{
								case EKind::Waiting:
								case EKind::Running:
								case EKind::Canceling:
									return 0;
								default:
									return 1;
								}
							})

							+ SWidgetSwitcher::Slot()
							[
								SNew(SCircularThrobber)
								.Radius(6.0f)
								.NumPieces(6)
							]

							+ SWidgetSwitcher::Slot()
							[
								SNew(SImage)
								.Image_Lambda([Eval]() -> const FSlateBrush*
								{
									switch (Eval().Kind)
									{
									case EKind::Done:     return FAppStyle::GetBrush("Icons.Success");
									case EKind::Failed:   return FAppStyle::GetBrush("Icons.X");
									default:              return FAppStyle::GetBrush("Icons.Info");
									}
								})
								.ColorAndOpacity_Lambda([Eval]() { return KindColor(Eval().Kind); })
							]
						]
					]

					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text_Lambda([Eval]() { return Eval().Stage; })
						.ColorAndOpacity_Lambda([Eval]() { return KindColor(Eval().Kind); })
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					]
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
				[
					SNew(SBox)
					.HeightOverride(6.0f)
					.Visibility_Lambda([Eval]() { return Eval().bShowProgress ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						// Percent 未设值 = 不知道进度（等 ComfyUI、取消中），走跑马灯，不画假进度。
						SNew(SProgressBar)
						.Percent_Lambda([Eval]() { return Eval().Percent; })
					]
				]

				// 失败/取消的原因。
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
				[
					SNew(STextBlock)
					.Text_Lambda([Eval]() { return Eval().Detail; })
					.ToolTipText_Lambda([Eval]() { return Eval().Detail; })
					.Visibility_Lambda([Eval]() { return Eval().Detail.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
					.ColorAndOpacity_Lambda([Eval]() { return Eval().Kind == EKind::Failed ? FSlateColor(FStyleColors::Error) : FSlateColor(FStyleColors::AccentGray); })
					.AutoWrapText(true)
				]

				// 导入 / 取消。
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
						.ContentPadding(FMargin(8.0f, 1.0f))
						.Visibility_Lambda([Eval]()
						{
							const FDreamTextureGenerateRowView View = Eval();
							return (View.Kind == EKind::Done && View.Import != EImport::Active) ? EVisibility::Visible : EVisibility::Collapsed;
						})
						.IsEnabled_Lambda([Eval]() { return Eval().bCanImport; })
						.ToolTipText(LOCTEXT("ImportTip", "Import this material set into the project: textures and a material instance. Textures that were already imported and have not changed are not downloaded again."))
						.OnClicked_Lambda([WeakModel, JobId]()
						{
							if (const TSharedPtr<FDreamTextureGenerateModel> Pinned = WeakModel.Pin())
							{
								Pinned->RequestImport(JobId);
							}
							return FReply::Handled();
						})
						[
							SNew(STextBlock)
							.Text_Lambda([Eval]() { return Eval().ImportButton; })
						]
					]

					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text_Lambda([Eval]() { return Eval().ImportText; })
						.ToolTipText_Lambda([Eval]() { return Eval().ImportTooltip; })
						.Visibility_Lambda([Eval]() { return Eval().Import == EImport::None ? EVisibility::Collapsed : EVisibility::Visible; })
						.ColorAndOpacity_Lambda([Eval]() -> FSlateColor
						{
							switch (Eval().Import)
							{
							case EImport::Succeeded: return FStyleColors::Success;
							case EImport::Failed:    return FStyleColors::Error;
							default:                 return FStyleColors::AccentBlue;
							}
						})
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(SButton)
						.ContentPadding(FMargin(8.0f, 1.0f))
						.Visibility_Lambda([Eval]() { return Eval().bCanCancel ? EVisibility::Visible : EVisibility::Collapsed; })
						.ToolTipText(LOCTEXT("CancelTip", "Cancel this variant. A queued variant is canceled at once; a running one interrupts ComfyUI."))
						.OnClicked_Lambda([WeakModel, JobId]()
						{
							if (const TSharedPtr<FDreamTextureGenerateModel> Pinned = WeakModel.Pin())
							{
								Pinned->CancelJob(JobId);
							}
							return FReply::Handled();
						})
						[
							SNew(STextBlock)
							.Text(LOCTEXT("Cancel", "Cancel"))
						]
					]
				]
			]
		];
}

void SDreamTextureGeneratePanel::RebuildCards()
{
	if (!CardBox.IsValid())
	{
		return;
	}
	CardBox->ClearChildren();
	for (const FDreamTextureGenerateModel::FRowPtr& Row : Model->GetRows())
	{
		CardBox->AddSlot()
		[
			MakeRowCard(Row)
		];
	}
}

//////////////////////////////////////////////////////////////////////////
// 下拉

SDreamTextureGeneratePanel::FOptionPtr SDreamTextureGeneratePanel::FindOption(const TArray<FOptionPtr>& List, const FString& Id) const
{
	for (const FOptionPtr& Option : List)
	{
		if (Option.IsValid() && Option->Id == Id)
		{
			return Option;
		}
	}
	return nullptr;
}

void SDreamTextureGeneratePanel::RebuildOptions()
{
	SourceOptions.Reset();
	for (const FDreamTextureWorkflowMeta* Meta : Model->GetSourceOptions())
	{
		const FOptionPtr Option = MakeShared<FOption>();
		Option->Id = Meta->Id;
		Option->Label = FText::FromString(Meta->Name.IsEmpty() ? Meta->Id : Meta->Name);

		// 标签：画面类型 + 底图在哪出。「要不要花钱」值得一眼看见。
		Option->Tag = FText::Format(LOCTEXT("SourceTag", "{0} · {1}"), DomainLabel(Meta->Domain),
			Meta->bCloudSource ? LOCTEXT("TagCloud", "Cloud image") : LOCTEXT("TagLocal", "Local"));
		SourceOptions.Add(Option);
	}

	DecomposeOptions.Reset();
	for (const FDreamTextureWorkflowMeta* Meta : Model->GetDecomposeOptions())
	{
		const FOptionPtr Option = MakeShared<FOption>();
		Option->Id = Meta->Id;
		Option->Label = FText::FromString(Meta->Name.IsEmpty() ? Meta->Id : Meta->Name);
		if (!Meta->IsCommercial())
		{
			// 选之前就看得见：不能商用的分解模型在下拉里就带着标签。
			Option->Tag = LOCTEXT("TagResearch", "Research use only");
			Option->bWarn = true;
		}
		DecomposeOptions.Add(Option);
	}

	ResolutionOptions.Reset();
	for (const int32 Value : Model->GetResolutionOptions())
	{
		const FOptionPtr Option = MakeShared<FOption>();
		Option->Id = FString::Printf(TEXT("%d"), Value);
		Option->Label = FText::FromString(Option->Id);
		ResolutionOptions.Add(Option);
	}

	RefineModelOptions.Reset();
	{
		const FOptionPtr Default = MakeShared<FOption>();
		Default->Label = Model->GetRefineDefaultModel().IsEmpty()
			? LOCTEXT("RefineDefault", "Default")
			: FText::Format(LOCTEXT("RefineDefaultNamed", "Default ({0})"), FText::FromString(Model->GetRefineDefaultModel()));
		RefineModelOptions.Add(Default);
	}
	for (const FString& ModelName : Model->GetRefineModels())
	{
		const FOptionPtr Option = MakeShared<FOption>();
		Option->Id = ModelName;
		Option->Label = FText::FromString(ModelName);
		RefineModelOptions.Add(Option);
	}

	CloudModelOptions.Reset();
	for (const FDreamTextureImagenModel* CloudModel : Model->GetCloudModels())
	{
		const FOptionPtr Option = MakeShared<FOption>();
		Option->Id = CloudModel->Id;
		Option->Label = FText::FromString(CloudModel->Label.IsEmpty() ? CloudModel->Id : CloudModel->Label);
		// 各模型的速度与花费差着一个量级，光看名字选不出来：说明跟在后面；服务端新发现的模型参数是推定的，也标出来。
		Option->Tag = CloudModel->bKnown ? FText::FromString(CloudModel->Note) : LOCTEXT("CloudUnknownModel", "Not in the catalog; parameters are inferred from the same family");
		Option->bWarn = !CloudModel->bKnown;
		CloudModelOptions.Add(Option);
	}

	CloudQualityOptions.Reset();
	for (const FString& Quality : Model->GetCloudQualityOptions())
	{
		const FOptionPtr Option = MakeShared<FOption>();
		Option->Id = Quality;
		Option->Label = FText::FromString(Quality);
		CloudQualityOptions.Add(Option);
	}

	// 选中项只能在选项刷新之后设；SetSelectedItem 触发的选择变化带的是 ESelectInfo::Direct，OnXPicked 据此不回写模型，免得循环。
	auto Sync = [](const TSharedPtr<SComboBox<FOptionPtr>>& Combo, const FOptionPtr& Selected)
	{
		if (Combo.IsValid())
		{
			Combo->RefreshOptions();
			Combo->SetSelectedItem(Selected);
		}
	};
	Sync(SourceCombo, FindOption(SourceOptions, Model->GetSourceId()));
	Sync(DecomposeCombo, FindOption(DecomposeOptions, Model->GetDecomposeId()));
	Sync(ResolutionCombo, FindOption(ResolutionOptions, FString::Printf(TEXT("%d"), Model->GetResolution())));
	Sync(RefineModelCombo, FindOption(RefineModelOptions, Model->GetRefineModel()));
	Sync(CloudModelCombo, FindOption(CloudModelOptions, Model->GetCloudModelId()));
	Sync(CloudQualityCombo, FindOption(CloudQualityOptions, Model->GetCloudQuality()));
}

void SDreamTextureGeneratePanel::OnSourcePicked(FOptionPtr Item, ESelectInfo::Type SelectInfo)
{
	if (Item.IsValid() && SelectInfo != ESelectInfo::Direct)
	{
		Model->SetSourceId(Item->Id);
	}
}

void SDreamTextureGeneratePanel::OnDecomposePicked(FOptionPtr Item, ESelectInfo::Type SelectInfo)
{
	if (Item.IsValid() && SelectInfo != ESelectInfo::Direct)
	{
		Model->SetDecomposeId(Item->Id);
	}
}

void SDreamTextureGeneratePanel::OnResolutionPicked(FOptionPtr Item, ESelectInfo::Type SelectInfo)
{
	if (Item.IsValid() && SelectInfo != ESelectInfo::Direct)
	{
		Model->SetResolution(FCString::Atoi(*Item->Id));
	}
}

void SDreamTextureGeneratePanel::OnRefineModelPicked(FOptionPtr Item, ESelectInfo::Type SelectInfo)
{
	if (Item.IsValid() && SelectInfo != ESelectInfo::Direct)
	{
		Model->SetRefineModel(Item->Id);
	}
}

void SDreamTextureGeneratePanel::OnCloudModelPicked(FOptionPtr Item, ESelectInfo::Type SelectInfo)
{
	if (Item.IsValid() && SelectInfo != ESelectInfo::Direct)
	{
		Model->SetCloudModel(Item->Id);
	}
}

void SDreamTextureGeneratePanel::OnCloudQualityPicked(FOptionPtr Item, ESelectInfo::Type SelectInfo)
{
	if (Item.IsValid() && SelectInfo != ESelectInfo::Direct)
	{
		Model->SetCloudQuality(Item->Id);
	}
}

FText SDreamTextureGeneratePanel::GetCloudModelLabel() const
{
	const FOptionPtr Option = FindOption(CloudModelOptions, Model->GetCloudModelId());
	return Option.IsValid() ? Option->Label : LOCTEXT("CloudNoModel", "(no models available)");
}

FText SDreamTextureGeneratePanel::GetCloudQualityLabel() const
{
	const FString Quality = Model->GetCloudQuality();
	return Quality.IsEmpty() ? LOCTEXT("NoneSelected", "(none)") : FText::FromString(Quality);
}

FText SDreamTextureGeneratePanel::GetSourceLabel() const
{
	const FOptionPtr Option = FindOption(SourceOptions, Model->GetSourceId());
	return Option.IsValid() ? Option->Label : LOCTEXT("NoneSelected", "(none)");
}

FText SDreamTextureGeneratePanel::GetDecomposeLabel() const
{
	const FOptionPtr Option = FindOption(DecomposeOptions, Model->GetDecomposeId());
	return Option.IsValid() ? Option->Label : LOCTEXT("NoneSelected", "(none)");
}

FText SDreamTextureGeneratePanel::GetResolutionLabel() const
{
	const int32 Resolution = Model->GetResolution();
	return Resolution > 0 ? FText::FromString(FString::Printf(TEXT("%d"), Resolution)) : LOCTEXT("NoneSelected", "(none)");
}

FText SDreamTextureGeneratePanel::GetRefineModelLabel() const
{
	const FOptionPtr Option = FindOption(RefineModelOptions, Model->GetRefineModel());
	return Option.IsValid() ? Option->Label : (RefineModelOptions.Num() > 0 ? RefineModelOptions[0]->Label : LOCTEXT("RefineDefault", "Default"));
}

//////////////////////////////////////////////////////////////////////////
// 模型事件

void SDreamTextureGeneratePanel::HandleFormChanged()
{
	RebuildOptions();
}

void SDreamTextureGeneratePanel::HandleBatchChanged()
{
	RebuildCards();
}

void SDreamTextureGeneratePanel::HandleRefineChanged()
{
	// 扩写的状态与结果都由属性回调每帧读，不需要重建任何控件。
}

void SDreamTextureGeneratePanel::HandlePromptReplaced()
{
	if (PromptBox.IsValid())
	{
		PromptBox->SetText(FText::FromString(Model->GetPrompt()));
	}
}

void SDreamTextureGeneratePanel::HandleNotice(const FText& Message, bool bIsError)
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

void SDreamTextureGeneratePanel::OnPromptTextChanged(const FText& NewText)
{
	Model->SetPrompt(NewText.ToString());
}

//////////////////////////////////////////////////////////////////////////
// 状态区取值

FText SDreamTextureGeneratePanel::GetCatalogErrorText() const
{
	return FText::FromString(Model->GetCatalogError());
}

FText SDreamTextureGeneratePanel::GetSourceNote() const
{
	const FDreamTextureWorkflowMeta* Source = Model->GetCatalog().FindSegment(Model->GetSourceId());
	if (Source != nullptr && Source->bCloudSource)
	{
		return LOCTEXT("CloudNote", "Cloud source image: billed per use, and the same parameters cannot reproduce a result (the cloud API does not support seeds).");
	}
	return FText::GetEmpty();
}

EVisibility SDreamTextureGeneratePanel::GetSourceNoteVisibility() const
{
	return GetSourceNote().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
}

FText SDreamTextureGeneratePanel::GetMismatchText() const
{
	const FDreamTextureWorkflowMeta* Workflow = Model->GetCurrentWorkflow();
	return Workflow != nullptr ? FText::FromString(Workflow->Mismatch) : FText::GetEmpty();
}

EVisibility SDreamTextureGeneratePanel::GetCloudVisibility() const
{
	return Model->IsCloudSource() ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SDreamTextureGeneratePanel::GetCloudQualityVisibility() const
{
	return Model->GetCloudQualityOptions().Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SDreamTextureGeneratePanel::GetCloudStatusText() const
{
	// 云端这一路走得通就不说话；走不通时的原因与提交按钮旁边是同一句。
	FText Why;
	return Model->CheckCloudReady(&Why) ? FText::GetEmpty() : Why;
}

EVisibility SDreamTextureGeneratePanel::GetCloudStatusVisibility() const
{
	return GetCloudStatusText().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility SDreamTextureGeneratePanel::GetCloudRetryVisibility() const
{
	// 读取中不让点；其余说不通的情形（失败、没填令牌、连不上）都可能因为重新读取而好转。
	return (!GetCloudStatusText().IsEmpty() && Model->GetCloudState() != FDreamTextureGenerateModel::ECloudState::Loading)
		? EVisibility::Visible : EVisibility::Collapsed;
}

FText SDreamTextureGeneratePanel::GetCloudCostText() const
{
	return FText::Format(LOCTEXT("CloudCost", "This will call the cloud API {0} time(s) and is billed per token."), FText::AsNumber(Model->GetVariants()));
}

EVisibility SDreamTextureGeneratePanel::GetMismatchVisibility() const
{
	return GetMismatchText().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility SDreamTextureGeneratePanel::GetResolutionVisibility() const
{
	return Model->GetResolutionOptions().Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SDreamTextureGeneratePanel::GetLicenseHeadline() const
{
	return LOCTEXT("LicenseHeadline", "The output of this combination is for research use only and cannot be used commercially.");
}

FText SDreamTextureGeneratePanel::GetLicenseReasonText() const
{
	const FDreamTextureGenerateLicenseHint Hint = Model->GetLicenseHint();
	if (Hint.CommercialDecomposeNames.Num() == 0)
	{
		return FText::FromString(Hint.Reason);
	}

	// 光说「不行」不够：告诉人换哪个分解模型就没有这个限制了。
	return FText::Format(LOCTEXT("LicenseWithAlternative", "{0}\nFor commercial use, switch the decompose model to: {1}."),
		FText::FromString(Hint.Reason), FText::FromString(FString::Join(Hint.CommercialDecomposeNames, *LOCTEXT("ListSeparator", ", ").ToString())));
}

EVisibility SDreamTextureGeneratePanel::GetLicenseVisibility() const
{
	return Model->GetLicenseHint().bNonCommercial ? EVisibility::Visible : EVisibility::Collapsed;
}

bool SDreamTextureGeneratePanel::IsSubmitEnabled() const
{
	return Model->CanActivatePrimary();
}

FText SDreamTextureGeneratePanel::GetSubmitErrorText() const
{
	return Model->GetSubmitError();
}

EVisibility SDreamTextureGeneratePanel::GetSubmitErrorVisibility() const
{
	return Model->GetSubmitError().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility SDreamTextureGeneratePanel::GetSettingsLinkVisibility() const
{
	return Model->NeedsExecutableSetting() ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SDreamTextureGeneratePanel::GetCancelAutoStartVisibility() const
{
	return Model->IsAutoStarting() ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SDreamTextureGeneratePanel::GetSubmitText() const
{
	using EAuto = FDreamTextureGenerateModel::EAutoStartState;
	switch (Model->GetAutoStartState())
	{
	case EAuto::StartingBackend:
		return LOCTEXT("AutoStartStarting", "Starting the backend…");
	case EAuto::LoadingCatalog:
		return LOCTEXT("AutoStartCatalog", "The backend has responded; reading the workflow list…");
	case EAuto::LoadingCloud:
		return LOCTEXT("AutoStartCloud", "Looking up the available cloud models…");
	case EAuto::Idle:
	default:
		break;
	}

	if (Model->IsSubmitting())
	{
		return LOCTEXT("SubmitBusy", "Submitting…");
	}

	// 在点下去的地方再说一遍：许可框离按钮有一段距离，眼睛不一定扫过。
	const bool bNonCommercial = Model->GetLicenseHint().bNonCommercial;
	if (Model->GetPrimaryAction() == FDreamTextureGenerateModel::EPrimaryAction::StartAndSubmit)
	{
		return bNonCommercial
			? LOCTEXT("StartNonCommercial", "Start backend and generate (output not for commercial use)")
			: LOCTEXT("StartAndSubmit", "Start backend and generate");
	}
	return bNonCommercial
		? LOCTEXT("SubmitNonCommercial", "Generate (output not for commercial use)")
		: LOCTEXT("Submit", "Generate");
}

FText SDreamTextureGeneratePanel::GetSubmitHintText() const
{
	if (Model->IsAutoStarting())
	{
		return LOCTEXT("AutoStartHint",
			"Once the backend is up and the workflow list has been read, the current form is submitted automatically. Cancel only stops the submission; it does not stop the backend.");
	}

	if (Model->GetPrimaryAction() == FDreamTextureGenerateModel::EPrimaryAction::StartAndSubmit)
	{
		// 第 1 级的原因照说，再告诉人这个按钮会干什么；缺提示词之类的表单问题也一并带上。
		const FText Tier = Model->GetReadiness().Reason;
		FText Why;
		if (!Model->CanAutoStart(&Why))
		{
			return FText::Format(LOCTEXT("OfflineHintBlocked", "{0}\n{1}"), Tier, Why);
		}
		return FText::Format(LOCTEXT("OfflineHint",
			"{0}\nClick Start backend and generate to launch the backend from the backend executable set in Settings (an existing backend on that port is reused), and the job is submitted automatically once it is up. If ComfyUI is still starting, the job waits in the backend queue until it is ready, so you do not need to wait for it."), Tier);
	}

	FText Why;
	if (!Model->CanSubmit(&Why))
	{
		return Why;
	}
	const FDreamTextureGenerateReadiness Readiness = Model->GetReadiness();
	return Readiness.Note;
}

FSlateColor SDreamTextureGeneratePanel::GetSubmitHintColor() const
{
	if (Model->IsAutoStarting())
	{
		return FStyleColors::AccentBlue;
	}
	// 后端不通但能一键拉起来：这是「有话要说」，不是死路，用黄而不是红。
	if (Model->GetPrimaryAction() == FDreamTextureGenerateModel::EPrimaryAction::StartAndSubmit)
	{
		return FStyleColors::Warning;
	}

	const FDreamTextureGenerateReadiness Readiness = Model->GetReadiness();

	// 被连接这三级挡住：红，这是故障或需要等；只是缺提示词之类的表单问题：灰；能提交但有话要说：黄。
	if (!Readiness.bCanSubmit)
	{
		return FStyleColors::Error;
	}
	return Model->CanSubmit() ? FStyleColors::Warning : FStyleColors::AccentGray;
}

EVisibility SDreamTextureGeneratePanel::GetSubmitHintVisibility() const
{
	return GetSubmitHintText().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
}

FText SDreamTextureGeneratePanel::GetBatchSummaryText() const
{
	const int32 Total = Model->GetRows().Num();
	if (Total == 0)
	{
		return LOCTEXT("NoBatch", "This batch: none yet");
	}
	return FText::Format(LOCTEXT("BatchSummary", "This batch: {0} variant(s) · {1} finished"),
		FText::AsNumber(Total), FText::AsNumber(Model->GetNumFinished()));
}

EVisibility SDreamTextureGeneratePanel::GetEmptyHintVisibility() const
{
	return Model->GetRows().Num() == 0 ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SDreamTextureGeneratePanel::GetCardsVisibility() const
{
	return Model->GetRows().Num() == 0 ? EVisibility::Collapsed : EVisibility::Visible;
}

bool SDreamTextureGeneratePanel::IsCancelAllEnabled() const
{
	return Model->HasActiveJobs();
}

//////////////////////////////////////////////////////////////////////////
// 动作

FReply SDreamTextureGeneratePanel::OnSubmitClicked()
{
	// 后端没应答又配了可执行文件时，这个按钮是「启动后端并生成」；由模型按当时的状态决定做哪件事。
	Model->ActivatePrimary();
	return FReply::Handled();
}

FReply SDreamTextureGeneratePanel::OnCancelAutoStartClicked()
{
	Model->CancelAutoStart();
	return FReply::Handled();
}

FReply SDreamTextureGeneratePanel::OnOpenSettingsClicked()
{
	// 不写死 "Project"/"Plugins"/"DreamTexture" 这三个字符串：UDeveloperSettings 自己知道它注册到哪儿了
	// （与状态页的「设置」按钮同一做法）。
	if (ISettingsModule* SettingsModule = FModuleManager::LoadModulePtr<ISettingsModule>("Settings"))
	{
		const UDreamTextureSettings& Settings = UDreamTextureSettings::Get();
		SettingsModule->ShowViewer(Settings.GetContainerName(), Settings.GetCategoryName(), Settings.GetSectionName());
	}
	return FReply::Handled();
}

FReply SDreamTextureGeneratePanel::OnRefineClicked()
{
	Model->Refine();
	return FReply::Handled();
}

FReply SDreamTextureGeneratePanel::OnAcceptRefineClicked()
{
	Model->AcceptRefine();
	return FReply::Handled();
}

FReply SDreamTextureGeneratePanel::OnDiscardRefineClicked()
{
	Model->DiscardRefine();
	return FReply::Handled();
}

FReply SDreamTextureGeneratePanel::OnRetryCatalogClicked()
{
	Model->LoadCatalog();
	return FReply::Handled();
}

FReply SDreamTextureGeneratePanel::OnRetryCloudClicked()
{
	Model->LoadCloudModels();
	return FReply::Handled();
}

FReply SDreamTextureGeneratePanel::OnSyncClicked()
{
	Model->SyncJobs();
	return FReply::Handled();
}

FReply SDreamTextureGeneratePanel::OnCancelAllClicked()
{
	Model->CancelBatch();
	return FReply::Handled();
}

void SDreamTextureGeneratePanel::BuildWidgetsForTest()
{
	RebuildOptions();
	RebuildCards();

	// 真走一遍 Slate 的布局：SlatePrepass 会递归算每个可见控件的期望尺寸，期间所有属性回调（文字、可见性、颜色）
	// 都经由控件自己的代码路径求值。光调下面这些取值函数只证明了函数本身，没证明控件树接得对。
	SlatePrepass(1.0f);
	GetDesiredSize();

	for (const FDreamTextureGenerateModel::FRowPtr& Row : Model->GetRows())
	{
		MakeRowCard(Row);
		MakeCardThumbnail(Row, CardThumbSize);
		Model->GetRowView(Row);
	}

	GetFormStateIndex();
	GetCatalogErrorText();
	GetSourceNote();
	GetSourceNoteVisibility();
	GetMismatchText();
	GetMismatchVisibility();
	GetResolutionVisibility();
	GetLicenseHeadline();
	GetLicenseReasonText();
	GetLicenseVisibility();
	IsSubmitEnabled();
	GetSubmitText();
	GetSubmitHintText();
	GetSubmitHintColor();
	GetSubmitHintVisibility();
	GetBatchSummaryText();
	GetEmptyHintVisibility();
	GetCardsVisibility();
	IsCancelAllEnabled();
	GetSourceLabel();
	GetDecomposeLabel();
	GetResolutionLabel();
	GetRefineModelLabel();
	GetCloudVisibility();
	GetCloudQualityVisibility();
	GetCloudStatusText();
	GetCloudStatusVisibility();
	GetCloudCostText();
	GetCloudRetryVisibility();
	GetCloudModelLabel();
	GetCloudQualityLabel();
	GetSubmitErrorText();
	GetSubmitErrorVisibility();
	GetSettingsLinkVisibility();
	GetCancelAutoStartVisibility();
}

#undef LOCTEXT_NAMESPACE
