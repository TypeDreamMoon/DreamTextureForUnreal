// Copyright TypeDreamMoon. All Rights Reserved.

#include "SDreamTextureReflowActions.h"

#include "DreamTextureApi.h"
#include "DreamTextureImportNotifier.h"
#include "DreamTextureThumbCache.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "DreamTextureReflowActionsPanel"

namespace
{
	TSharedRef<SWidget> MakeLabel(const FText& Text)
	{
		return SNew(STextBlock)
			.Text(Text)
			.Font(FAppStyle::GetFontStyle("BoldFont"));
	}

	/** 单选按钮：用复选框控件配 Radio 样式，选中与否由模型说了算。 */
	TSharedRef<SWidget> MakeRadio(const FText& Label, const FText& Tip, TFunction<bool()> IsOn, TFunction<void()> OnPick, TFunction<bool()> IsEnabled = nullptr)
	{
		return SNew(SCheckBox)
			.Style(&FCoreStyle::Get().GetWidgetStyle<FCheckBoxStyle>("RadioButton"))
			.IsEnabled_Lambda([IsEnabled]() { return !IsEnabled || IsEnabled(); })
			.IsChecked_Lambda([IsOn]() { return IsOn() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([OnPick](ECheckBoxState State)
			{
				if (State == ECheckBoxState::Checked)
				{
					OnPick();
				}
			})
			.ToolTipText(Tip)
			[
				SNew(STextBlock).Text(Label)
			];
	}

	/**
	 * 结果预览的取图：键由 FDreamTextureReflowActions 的 MakeFileKey / MakeMaterialFileKey 造——
	 * 本地文件（原图，刚导出的 PNG）直接读，后端材质里的文件走 REST。缩略图缓存只管解码、缩小、建画刷。
	 */
	void FetchPreviewBytes(const FString& Key, FDreamTextureApi::FOnBytes Done)
	{
		static const FString FilePrefix(TEXT("file:"));
		static const FString MaterialPrefix(TEXT("mat:"));

		if (Key.StartsWith(FilePrefix))
		{
			TArray<uint8> Bytes;
			if (FFileHelper::LoadFileToArray(Bytes, *Key.Mid(FilePrefix.Len())))
			{
				Done(true, MoveTemp(Bytes), FString());
			}
			else
			{
				Done(false, TArray<uint8>(), FText::Format(LOCTEXT("PreviewFileMissing", "Could not read the local preview file: {0}"), FText::FromString(Key.Mid(FilePrefix.Len()))).ToString());
			}
			return;
		}
		if (Key.StartsWith(MaterialPrefix))
		{
			FString Id;
			FString File;
			if (Key.Mid(MaterialPrefix.Len()).Split(TEXT("/"), &Id, &File) && !Id.IsEmpty() && !File.IsEmpty())
			{
				FDreamTextureApi::FetchFile(Id, File, MoveTemp(Done));
				return;
			}
		}
		Done(false, TArray<uint8>(), FText::Format(LOCTEXT("PreviewKeyUnknown", "Unrecognized preview key: {0}"), FText::FromString(Key)).ToString());
	}

	/** 重绘幅度各档的人话（后端 docs/reference-image.md 的档位表）。 */
	FText DescribeDenoiseTier(double Denoise)
	{
		if (Denoise <= 0.35)
		{
			return LOCTEXT("TierLow", "Nearly a copy of the original, only refreshing details");
		}
		if (Denoise <= 0.55)
		{
			return LOCTEXT("TierMid", "Keeps the original's structure, changes the material look");
		}
		if (Denoise <= 0.75)
		{
			return LOCTEXT("TierHigh", "Borrows the overall colors and composition, regenerates the rest");
		}
		return LOCTEXT("TierMax", "Largely ignores the original");
	}
}

void SDreamTextureReflowActions::Construct(const FArguments& InArgs)
{
	Actions = InArgs._Actions;
	check(Actions.IsValid());
	bThumbnails = InArgs._bThumbnails;
	if (bThumbnails)
	{
		// 预览比卡片缩略图看得细：长边 512。键带 file:/mat: 前缀，不会与素材库的缓存文件撞名。
		PreviewCache = MakeShared<FDreamTextureThumbCache>(8, &FetchPreviewBytes, 512);
	}

	ChangedHandle = Actions->OnChanged.AddSP(this, &SDreamTextureReflowActions::HandleActionsChanged);
	Actions->Initialize(InArgs._bBindExternal);

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(FMargin(10.0f, 8.0f))
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeLabel(LOCTEXT("Title", "Reflow: send this material to the backend to fill missing channels or restyle it, then bring the result back into the project"))
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
			[
				MakeModeBlock()
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 8.0f, 0.0f, 0.0f))
			[
				MakeChannelBlock()
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 8.0f, 0.0f, 0.0f))
			[
				MakeDecomposeBlock()
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 8.0f, 0.0f, 0.0f))
			[
				MakeRestyleBlock()
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 8.0f, 0.0f, 0.0f))
			[
				MakeLandingBlock()
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 8.0f, 0.0f, 0.0f))
			[
				SNew(SSeparator).Thickness(1.0f)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 8.0f, 0.0f, 0.0f))
			[
				MakeRunBlock()
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
			[
				MakeResultBlock()
			]
		]
	];

	RefreshOptions();
}

SDreamTextureReflowActions::~SDreamTextureReflowActions()
{
	if (Actions.IsValid())
	{
		Actions->OnChanged.Remove(ChangedHandle);
	}
}

//////////////////////////////////////////////////////////////////////////
// 构件

TSharedRef<SWidget> SDreamTextureReflowActions::MakeModeBlock()
{
	const TWeakPtr<FDreamTextureReflowActions> Weak = Actions;
	auto IsMode = [Weak](EDreamTextureReflowMode Mode)
	{
		const TSharedPtr<FDreamTextureReflowActions> Pinned = Weak.Pin();
		return Pinned.IsValid() && Pinned->GetMode() == Mode;
	};
	auto Pick = [Weak](EDreamTextureReflowMode Mode)
	{
		if (const TSharedPtr<FDreamTextureReflowActions> Pinned = Weak.Pin())
		{
			Pinned->SetMode(Mode);
		}
	};

	return SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0.0f, 0.0f, 16.0f, 0.0f))
			[
				MakeRadio(LOCTEXT("ModeFill", "Fill missing channels"),
					LOCTEXT("ModeFillTip", "Runs the decompose step only: the original Base Color goes in unchanged so the picture does not change, and the channels the material lacks (Normal, Roughness, Ambient Occlusion, Height and so on) are generated."),
					[IsMode]() { return IsMode(EDreamTextureReflowMode::FillChannels); },
					[Pick]() { Pick(EDreamTextureReflowMode::FillChannels); })
			]

			+ SHorizontalBox::Slot().AutoWidth()
			[
				MakeRadio(LOCTEXT("ModeRestyle", "Restyle"),
					LOCTEXT("ModeRestyleTip", "Uses the original Base Color as a reference image and redraws it according to the prompt (img2img), then decomposes the whole set of channels."),
					[IsMode]() { return IsMode(EDreamTextureReflowMode::Restyle); },
					[Pick]() { Pick(EDreamTextureReflowMode::Restyle); })
			]
		]

		// 平铺还是 UV 展开：按材质记住。
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
		[
			MakeUVKindBlock()
		]

		// 认出多张底色时，让用户挑送哪一张。
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
		[
			SNew(SHorizontalBox)
			.Visibility(this, &SDreamTextureReflowActions::GetSourceRowVisibility)

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				SNew(STextBlock).Text(LOCTEXT("SourceLabel", "Base Color to send"))
			]

			+ SHorizontalBox::Slot().AutoWidth()
			[
				SAssignNew(SourceCombo, SComboBox<FOptionPtr>)
				.OptionsSource(&SourceOptions)
				.OnGenerateWidget_Lambda([this](FOptionPtr Item)
				{
					const FDreamTextureMaterialTexture* Row = Item.IsValid() ? Actions->GetModel()->FindTexture(*Item) : nullptr;
					return SNew(STextBlock).Text(FText::FromString(Row != nullptr ? Row->AssetName : (Item.IsValid() ? *Item : FString())));
				})
				.OnSelectionChanged_Lambda([this](FOptionPtr Item, ESelectInfo::Type SelectInfo)
				{
					if (SelectInfo != ESelectInfo::Direct && Item.IsValid())
					{
						Actions->SetSourceTexture(*Item);
					}
				})
				.ToolTipText(LOCTEXT("SourceTip", "More than one Base Color texture was recognized in this material. Pick the one to send as the input."))
				[
					SNew(STextBlock).Text(this, &SDreamTextureReflowActions::GetSourceLabel)
				]
			]
		];
}

TSharedRef<SWidget> SDreamTextureReflowActions::MakeUVKindBlock()
{
	const TWeakPtr<FDreamTextureReflowActions> Weak = Actions;
	auto IsKind = [Weak](EDreamTextureUVKind Kind)
	{
		const TSharedPtr<FDreamTextureReflowActions> Pinned = Weak.Pin();
		return Pinned.IsValid() && Pinned->GetUVKind() == Kind;
	};
	auto Pick = [Weak](EDreamTextureUVKind Kind)
	{
		if (const TSharedPtr<FDreamTextureReflowActions> Pinned = Weak.Pin())
		{
			Pinned->SetUVKind(Kind);
		}
	};
	auto CanChange = [Weak]()
	{
		const TSharedPtr<FDreamTextureReflowActions> Pinned = Weak.Pin();
		return Pinned.IsValid() && !Pinned->IsRunActive();
	};

	return SNew(SHorizontalBox)
		.Visibility(this, &SDreamTextureReflowActions::GetUVKindVisibility)

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.0f, 0.0f, 12.0f, 0.0f))
		[
			SNew(STextBlock).Text(LOCTEXT("UVKindLabel", "Texture type"))
		]

		+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0.0f, 0.0f, 16.0f, 0.0f))
		[
			MakeRadio(LOCTEXT("KindTiled", "Tileable"),
				LOCTEXT("KindTiledTip", "Seamlessly tileable material (ground, walls, rock...). The result comes with a 3×3 tiled preview whose edges meet end to end."),
				[IsKind]() { return IsKind(EDreamTextureUVKind::Tiled); },
				[Pick]() { Pick(EDreamTextureUVKind::Tiled); },
				CanChange)
		]

		+ SHorizontalBox::Slot().AutoWidth()
		[
			MakeRadio(LOCTEXT("KindUnwrapped", "UV-unwrapped"),
				LOCTEXT("KindUnwrappedTip", "For characters, props and trim sheets: one image maps to the unwrapped layout of one mesh, and its edges do not meet end to end.\nWhen you choose it: no seamless processing is applied; the decompose model defaults to Classic derivation; the denoise strength of Restyle cannot exceed 0.35 (redrawing would break the UV correspondence); and the result preview becomes Original | Result side by side.\nThe choice is remembered per material under the project's Saved/DreamTexture and is not written into your material package."),
				[IsKind]() { return IsKind(EDreamTextureUVKind::Unwrapped); },
				[Pick]() { Pick(EDreamTextureUVKind::Unwrapped); },
				CanChange)
		];
}

TSharedRef<SWidget> SDreamTextureReflowActions::MakeChannelBlock()
{
	ChannelRows = SNew(SVerticalBox);

	// 六个通道固定，行建一次；每行的内容从缓存的 Views 里按下标取。
	for (int32 Index = 0; Index < DreamTextureReflow::GetChannels().Num(); ++Index)
	{
		const int32 Row = Index;
		ChannelRows->AddSlot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 1.0f))
			[
				SNew(SHorizontalBox)
				.Visibility_Lambda([this, Row]() { return Views.IsValidIndex(Row) ? EVisibility::Visible : EVisibility::Collapsed; })
				.ToolTipText_Lambda([this, Row]() { return Views.IsValidIndex(Row) ? Views[Row].Tooltip : FText::GetEmpty(); })

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SCheckBox)
					.IsEnabled_Lambda([this, Row]() { return Views.IsValidIndex(Row) && Views[Row].bEnabled && !Actions->IsRunActive(); })
					.IsChecked_Lambda([this, Row]() { return Views.IsValidIndex(Row) && Views[Row].bChecked ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this, Row](ECheckBoxState State)
					{
						if (Views.IsValidIndex(Row))
						{
							Actions->SetChannelChecked(Views[Row].Channel, State == ECheckBoxState::Checked);
						}
					})
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(6.0f, 0.0f, 12.0f, 0.0f))
				[
					// 按最长的英文通道名（Ambient Occlusion）定宽，几行的「已有」说明才对得齐。
					SNew(SBox)
					.MinDesiredWidth(124.0f)
					[
						SNew(STextBlock)
						.Text_Lambda([this, Row]() { return Views.IsValidIndex(Row) ? Views[Row].Label : FText::GetEmpty(); })
					]
				]

				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([this, Row]() { return Views.IsValidIndex(Row) ? Views[Row].Status : FText::GetEmpty(); })
					.ColorAndOpacity_Lambda([this, Row]()
					{
						if (!Views.IsValidIndex(Row))
						{
							return FSlateColor::UseForeground();
						}
						// 勾上了却原地改不了：警告色。其余：已有的用淡色（默认不动），缺的用正常色。
						if (Views[Row].bInPlaceBlocked)
						{
							return FSlateColor(FStyleColors::Warning);
						}
						return Views[Row].bPresent ? FSlateColor::UseSubduedForeground() : FSlateColor::UseForeground();
					})
					.AutoWrapText(true)
				]
			];
	}

	return SNew(SVerticalBox)
		.Visibility_Lambda([this]() { return Views.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed; })

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				MakeLabel(LOCTEXT("ChannelsLabel", "Channels to overwrite (channels the material already has are left alone by default)"))
			]

			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.ContentPadding(FMargin(8.0f, 0.0f))
				.IsEnabled_Lambda([this]() { return !Actions->IsRunActive(); })
				.ToolTipText(LOCTEXT("ResetTip", "Back to the default: missing channels checked, existing ones unchecked."))
				.OnClicked_Lambda([this]()
				{
					Actions->ResetSelection();
					return FReply::Handled();
				})
				[
					SNew(STextBlock).Text(LOCTEXT("Reset", "Reset to default"))
				]
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
		[
			ChannelRows.ToSharedRef()
		];
}

TSharedRef<SWidget> SDreamTextureReflowActions::MakeDecomposeBlock()
{
	return SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				MakeLabel(LOCTEXT("DecomposeLabel", "Decompose model"))
			]

			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SAssignNew(DecomposeCombo, SComboBox<FOptionPtr>)
				.OptionsSource(&DecomposeOptions)
				.IsEnabled_Lambda([this]() { return !Actions->IsRunActive(); })
				.OnGenerateWidget_Lambda([this](FOptionPtr Item)
				{
					const FDreamTextureWorkflowMeta* Meta = Item.IsValid() ? Actions->GetCatalog().FindSegment(*Item) : nullptr;
					return SNew(STextBlock).Text(Meta != nullptr ? FDreamTextureReflowActions::DescribeSegment(*Meta, Actions->GetUVKind()) : FText::GetEmpty());
				})
				.OnSelectionChanged_Lambda([this](FOptionPtr Item, ESelectInfo::Type SelectInfo)
				{
					if (SelectInfo != ESelectInfo::Direct && Item.IsValid())
					{
						Actions->SetDecomposeId(*Item);
					}
				})
				.ToolTipText(LOCTEXT("DecomposeTip", "The step that splits the source image into Normal, Roughness, Metallic, Ambient Occlusion and Height."))
				[
					SNew(STextBlock).Text(this, &SDreamTextureReflowActions::GetDecomposeLabel)
				]
			]
		]

		// 选 CHORD 时标出「仅限研究用途」；搭不上的提醒只提醒不拦。
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
		[
			SNew(STextBlock)
			.Visibility(this, &SDreamTextureReflowActions::GetDecomposeWarningVisibility)
			.Text(this, &SDreamTextureReflowActions::GetDecomposeWarning)
			.ColorAndOpacity(FStyleColors::Warning)
			.AutoWrapText(true)
		];
}

TSharedRef<SWidget> SDreamTextureReflowActions::MakeRestyleBlock()
{
	const TWeakPtr<FDreamTextureReflowActions> Weak = Actions;

	return SNew(SVerticalBox)
		.Visibility(this, &SDreamTextureReflowActions::GetRestyleVisibility)

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			.Visibility_Lambda([this]() { return RestyleSourceOptions.Num() > 1 ? EVisibility::Visible : EVisibility::Collapsed; })

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				MakeLabel(LOCTEXT("RestyleSourceLabel", "Image model"))
			]

			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SAssignNew(RestyleSourceCombo, SComboBox<FOptionPtr>)
				.OptionsSource(&RestyleSourceOptions)
				.IsEnabled_Lambda([this]() { return !Actions->IsRunActive(); })
				.OnGenerateWidget_Lambda([this](FOptionPtr Item)
				{
					const FDreamTextureWorkflowMeta* Meta = Item.IsValid() ? Actions->GetCatalog().FindSegment(*Item) : nullptr;
					return SNew(STextBlock).Text(Meta != nullptr ? FText::FromString(Meta->Name.IsEmpty() ? Meta->Id : Meta->Name) : FText::GetEmpty());
				})
				.OnSelectionChanged_Lambda([this](FOptionPtr Item, ESelectInfo::Type SelectInfo)
				{
					if (SelectInfo != ESelectInfo::Direct && Item.IsValid())
					{
						Actions->SetRestyleSourceId(*Item);
					}
				})
				.ToolTipText(LOCTEXT("RestyleSourceTip", "Restyle uses local image generation (img2img). Realistic or Stylized decides the style of the redraw. Cloud image models have no denoise strength and are not offered here."))
				[
					SNew(STextBlock).Text(this, &SDreamTextureReflowActions::GetRestyleSourceLabel)
				]
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
		[
			MakeLabel(LOCTEXT("PromptLabel", "Prompt (what it should become)"))
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 3.0f, 0.0f, 0.0f))
		[
			SNew(SBox)
			.MinDesiredHeight(60.0f)
			.MaxDesiredHeight(140.0f)
			[
				SAssignNew(PromptBox, SMultiLineEditableTextBox)
				.Text(FText::FromString(Actions->GetPrompt()))
				.HintText(LOCTEXT("PromptHint", "For example: cracked ancient rock covered in moss"))
				.AutoWrapText(true)
				.IsEnabled_Lambda([this]() { return !Actions->IsRunActive(); })
				.OnTextChanged_Lambda([Weak](const FText& Text)
				{
					if (const TSharedPtr<FDreamTextureReflowActions> Pinned = Weak.Pin())
					{
						Pinned->SetPrompt(Text.ToString());
					}
				})
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				MakeLabel(LOCTEXT("DenoiseLabel", "Denoise strength"))
			]

			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SBox)
				.WidthOverride(120.0f)
				[
					SNew(SSpinBox<float>)
					.MinValue(static_cast<float>(DreamTextureReflow::MinDenoise))
					.MaxValue(1.0f)
					.MinSliderValue(static_cast<float>(DreamTextureReflow::MinDenoise))
					.MaxSliderValue(1.0f)
					.Delta(0.05f)
					.MinFractionalDigits(2)
					.MaxFractionalDigits(2)
					.IsEnabled_Lambda([this]() { return !Actions->IsRunActive(); })
					.Value_Lambda([Weak]() -> float
					{
						const TSharedPtr<FDreamTextureReflowActions> Pinned = Weak.Pin();
						return Pinned.IsValid() ? static_cast<float>(Pinned->GetDenoise()) : static_cast<float>(DreamTextureReflow::DefaultTiledDenoise);
					})
					.OnValueChanged_Lambda([Weak](float Value)
					{
						if (const TSharedPtr<FDreamTextureReflowActions> Pinned = Weak.Pin())
						{
							Pinned->SetDenoise(Value);
						}
					})
				]
			]

			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(FMargin(10.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(STextBlock)
				.Text(this, &SDreamTextureReflowActions::GetDenoiseTierText)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.AutoWrapText(true)
			]
		]

		// UV 展开的硬上限：超了就拦住，说清楚为什么，并给一个一键调回去的按钮。
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
		[
			SNew(SVerticalBox)
			.Visibility(this, &SDreamTextureReflowActions::GetDenoiseWarningVisibility)

			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(this, &SDreamTextureReflowActions::GetDenoiseWarning)
				.ColorAndOpacity(FStyleColors::Error)
				.AutoWrapText(true)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.ContentPadding(FMargin(10.0f, 1.0f))
				.HAlign(HAlign_Left)
				.IsEnabled_Lambda([this]() { return !Actions->IsRunActive(); })
				.OnClicked_Lambda([this]()
				{
					Actions->SetDenoise(DreamTextureReflow::MaxUnwrappedDenoise);
					return FReply::Handled();
				})
				[
					SNew(STextBlock).Text(LOCTEXT("ClampDenoise", "Set to the maximum, 0.35"))
				]
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
		[
			SNew(SHorizontalBox)
			.Visibility(this, &SDreamTextureReflowActions::GetResolutionVisibility)

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				MakeLabel(LOCTEXT("ResolutionLabel", "Resolution"))
			]

			+ SHorizontalBox::Slot().AutoWidth()
			[
				SAssignNew(ResolutionCombo, SComboBox<FOptionPtr>)
				.OptionsSource(&ResolutionOptions)
				.IsEnabled_Lambda([this]() { return !Actions->IsRunActive(); })
				.OnGenerateWidget_Lambda([](FOptionPtr Item)
				{
					return SNew(STextBlock).Text(Item.IsValid() ? FText::FromString(*Item) : FText::GetEmpty());
				})
				.OnSelectionChanged_Lambda([this](FOptionPtr Item, ESelectInfo::Type SelectInfo)
				{
					if (SelectInfo != ESelectInfo::Direct && Item.IsValid())
					{
						Actions->SetResolution(FCString::Atoi(**Item));
					}
				})
				.ToolTipText(LOCTEXT("ResolutionTip", "Edge length of the redraw in pixels. CHORD works best at 1024."))
				[
					SNew(STextBlock).Text(this, &SDreamTextureReflowActions::GetResolutionLabel)
				]
			]
		];
}

TSharedRef<SWidget> SDreamTextureReflowActions::MakeLandingBlock()
{
	const TWeakPtr<FDreamTextureReflowActions> Weak = Actions;
	return SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeLabel(LOCTEXT("LandingLabel", "Bring back into the project"))
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 3.0f, 0.0f, 0.0f))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0.0f, 0.0f, 16.0f, 0.0f))
			[
				MakeRadio(LOCTEXT("LandNew", "Create new"),
					LOCTEXT("LandNewTip", "New textures plus a new MI_<name>; the original material is left completely untouched."),
					[Weak]() { const TSharedPtr<FDreamTextureReflowActions> P = Weak.Pin(); return P.IsValid() && !P->IsInPlace(); },
					[Weak]() { if (const TSharedPtr<FDreamTextureReflowActions> P = Weak.Pin()) { P->SetInPlace(false); } },
					[Weak]() { const TSharedPtr<FDreamTextureReflowActions> P = Weak.Pin(); return P.IsValid() && !P->IsRunActive(); })
			]

			+ SHorizontalBox::Slot().AutoWidth()
			[
				MakeRadio(LOCTEXT("LandInPlace", "Overwrite in place"),
					LOCTEXT("LandInPlaceTip", "Changes the texture parameters of the selected material instance, only for the channels checked above, and can be undone with Ctrl+Z. Not available when a master material is selected: its textures are hard-wired into the graph, so there are no parameters to change."),
					[Weak]() { const TSharedPtr<FDreamTextureReflowActions> P = Weak.Pin(); return P.IsValid() && P->IsInPlace(); },
					[Weak]() { if (const TSharedPtr<FDreamTextureReflowActions> P = Weak.Pin()) { P->SetInPlace(true); } },
					[Weak]() { const TSharedPtr<FDreamTextureReflowActions> P = Weak.Pin(); return P.IsValid() && !P->IsRunActive() && P->CanOverwriteInPlace(); })
			]
		]

		// 原地覆盖置灰时说清楚为什么。
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 3.0f, 0.0f, 0.0f))
		[
			SNew(STextBlock)
			.Visibility(this, &SDreamTextureReflowActions::GetInPlaceHintVisibility)
			.Text(this, &SDreamTextureReflowActions::GetInPlaceHint)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.AutoWrapText(true)
		];
}

TSharedRef<SWidget> SDreamTextureReflowActions::MakeRunBlock()
{
	return SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Visibility(this, &SDreamTextureReflowActions::GetRunNoteVisibility)
			.Text(this, &SDreamTextureReflowActions::GetRunNote)
			.ColorAndOpacity(FStyleColors::Warning)
			.AutoWrapText(true)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				SNew(SButton)
				.ContentPadding(FMargin(16.0f, 3.0f))
				.IsEnabled(this, &SDreamTextureReflowActions::IsRunEnabled)
				.OnClicked(this, &SDreamTextureReflowActions::OnRunClicked)
				.ToolTipText(LOCTEXT("RunTip", "Export Base Color → upload → submit → wait for the backend to generate → download → bring into the project. If the backend is not running, it is started for you first."))
				[
					SNew(STextBlock).Text(LOCTEXT("Run", "Start"))
				]
			]

			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				SNew(SButton)
				.ContentPadding(FMargin(12.0f, 3.0f))
				.Visibility(this, &SDreamTextureReflowActions::GetCancelVisibility)
				.OnClicked(this, &SDreamTextureReflowActions::OnCancelClicked)
				[
					SNew(STextBlock).Text(LOCTEXT("Cancel", "Cancel"))
				]
			]

			// 开始不了的原因。
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Visibility(this, &SDreamTextureReflowActions::GetRunReasonVisibility)
				.Text(this, &SDreamTextureReflowActions::GetRunReason)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.AutoWrapText(true)
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
		[
			SNew(SVerticalBox)
			.Visibility(this, &SDreamTextureReflowActions::GetRunStatusVisibility)

			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(this, &SDreamTextureReflowActions::GetStepsText)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
			[
				SNew(SBox)
				.HeightOverride(6.0f)
				.Visibility(this, &SDreamTextureReflowActions::GetProgressVisibility)
				[
					// Percent 未设值 = 不知道进度（等 ComfyUI、取消中），走跑马灯，不画假进度。
					SNew(SProgressBar).Percent(this, &SDreamTextureReflowActions::GetRunProgress)
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
			[
				SNew(STextBlock)
				.Text(this, &SDreamTextureReflowActions::GetRunMessage)
				.ColorAndOpacity(this, &SDreamTextureReflowActions::GetRunMessageColor)
				.AutoWrapText(true)
			]
		];
}

TSharedRef<SWidget> SDreamTextureReflowActions::MakeResultBlock()
{
	return SNew(SVerticalBox)
		.Visibility(this, &SDreamTextureReflowActions::GetResultVisibility)

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SButton)
			.ContentPadding(FMargin(10.0f, 2.0f))
			.Visibility(this, &SDreamTextureReflowActions::GetShowButtonVisibility)
			.OnClicked(this, &SDreamTextureReflowActions::OnShowClicked)
			[
				SNew(STextBlock).Text(LOCTEXT("Show", "Show in Content Browser"))
			]
		]

		// 结果预览：平铺素材看后端出的 3×3 平铺预览；UV 展开的 3×3 没有意义，改成「原图 | 结果」并排。
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
		[
			SNew(SHorizontalBox)
			.Visibility(this, &SDreamTextureReflowActions::GetPreviewVisibility)

			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0.0f, 0.0f, 10.0f, 0.0f))
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(this, &SDreamTextureReflowActions::GetLeftPreviewLabel)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 2.0f, 0.0f, 0.0f))
				[
					SNew(SBox)
					.WidthOverride(256.0f)
					.HeightOverride(256.0f)
					[
						SNew(SImage).Image(this, &SDreamTextureReflowActions::GetLeftPreviewBrush)
					]
				]
			]

			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SVerticalBox)
				.Visibility(this, &SDreamTextureReflowActions::GetRightPreviewVisibility)

				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(this, &SDreamTextureReflowActions::GetRightPreviewLabel)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 2.0f, 0.0f, 0.0f))
				[
					SNew(SBox)
					.WidthOverride(256.0f)
					.HeightOverride(256.0f)
					[
						SNew(SImage).Image(this, &SDreamTextureReflowActions::GetRightPreviewBrush)
					]
				]
			]
		];
}

//////////////////////////////////////////////////////////////////////////
// 模型事件

void SDreamTextureReflowActions::RefreshOptions()
{
	auto Rebuild = [](TArray<FOptionPtr>& Options, const TArray<FString>& Ids)
	{
		bool bSame = Options.Num() == Ids.Num();
		for (int32 Index = 0; bSame && Index < Ids.Num(); ++Index)
		{
			bSame = *Options[Index] == Ids[Index];
		}
		if (!bSame)
		{
			Options.Reset();
			for (const FString& Id : Ids)
			{
				Options.Add(MakeShared<FString>(Id));
			}
		}
		return !bSame;
	};

	TArray<FString> DecomposeIds;
	for (const FDreamTextureWorkflowMeta* Meta : Actions->GetDecomposeOptions())
	{
		DecomposeIds.Add(Meta->Id);
	}
	TArray<FString> RestyleIds;
	for (const FDreamTextureWorkflowMeta* Meta : Actions->GetRestyleSources())
	{
		RestyleIds.Add(Meta->Id);
	}
	TArray<FString> ResolutionIds;
	for (const int32 Option : Actions->GetResolutionOptions())
	{
		ResolutionIds.Add(FString::FromInt(Option));
	}

	const bool bDecomposeChanged = Rebuild(DecomposeOptions, DecomposeIds);
	const bool bRestyleChanged = Rebuild(RestyleSourceOptions, RestyleIds);
	const bool bSourceChanged = Rebuild(SourceOptions, Actions->GetSourceCandidates());
	const bool bResolutionChanged = Rebuild(ResolutionOptions, ResolutionIds);

	if (bDecomposeChanged && DecomposeCombo.IsValid())
	{
		DecomposeCombo->RefreshOptions();
	}
	if (bRestyleChanged && RestyleSourceCombo.IsValid())
	{
		RestyleSourceCombo->RefreshOptions();
	}
	if (bSourceChanged && SourceCombo.IsValid())
	{
		SourceCombo->RefreshOptions();
	}
	if (bResolutionChanged && ResolutionCombo.IsValid())
	{
		ResolutionCombo->RefreshOptions();
	}
}

void SDreamTextureReflowActions::HandleActionsChanged()
{
	Views = Actions->GetChannelViews();
	RefreshOptions();
}

//////////////////////////////////////////////////////////////////////////
// 取值

FText SDreamTextureReflowActions::GetDecomposeLabel() const
{
	const FDreamTextureWorkflowMeta* Meta = Actions->GetCatalog().FindSegment(Actions->GetDecomposeId());
	if (Meta != nullptr)
	{
		return FDreamTextureReflowActions::DescribeSegment(*Meta, Actions->GetUVKind());
	}
	switch (Actions->GetCatalogState())
	{
	case FDreamTextureReflowActions::ECatalogState::Failed: return LOCTEXT("DecomposeFailed", "(could not read the workflow list)");
	case FDreamTextureReflowActions::ECatalogState::Ready: return LOCTEXT("DecomposeNone", "(no decompose models)");
	default: return LOCTEXT("DecomposeLoading", "Loading…");
	}
}

FText SDreamTextureReflowActions::GetDecomposeWarning() const
{
	const FDreamTextureReflowDecomposeHint Hint = Actions->GetDecomposeHint();
	TArray<FString> Lines;
	if (Hint.bNonCommercial)
	{
		Lines.Add(FText::Format(LOCTEXT("WarnResearchOnly", "Research use only, not for commercial use ({0}). For commercial use, choose a different decompose model."), FText::FromString(Hint.LicenseReason)).ToString());
	}
	if (Hint.bOutOfDistribution)
	{
		Lines.Add(Hint.OutOfDistributionText);
	}
	if (!Hint.Mismatch.IsEmpty())
	{
		Lines.Add(Hint.Mismatch);
	}
	return FText::FromString(FString::Join(Lines, TEXT("\n")));
}

EVisibility SDreamTextureReflowActions::GetDecomposeWarningVisibility() const
{
	const FDreamTextureReflowDecomposeHint Hint = Actions->GetDecomposeHint();
	return Hint.bNonCommercial || Hint.bOutOfDistribution || !Hint.Mismatch.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SDreamTextureReflowActions::GetRestyleSourceLabel() const
{
	const FDreamTextureWorkflowMeta* Meta = Actions->GetCatalog().FindSegment(Actions->GetRestyleSourceId());
	return Meta != nullptr ? FText::FromString(Meta->Name.IsEmpty() ? Meta->Id : Meta->Name) : FText::GetEmpty();
}

EVisibility SDreamTextureReflowActions::GetRestyleVisibility() const
{
	return Actions->GetMode() == EDreamTextureReflowMode::Restyle ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SDreamTextureReflowActions::GetSourceRowVisibility() const
{
	return SourceOptions.Num() > 1 ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SDreamTextureReflowActions::GetSourceLabel() const
{
	const FDreamTextureMaterialTexture* Row = Actions->GetSourceTexture();
	return Row != nullptr ? FText::FromString(Row->AssetName) : FText::GetEmpty();
}

EVisibility SDreamTextureReflowActions::GetResolutionVisibility() const
{
	return ResolutionOptions.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SDreamTextureReflowActions::GetResolutionLabel() const
{
	return FText::AsNumber(Actions->GetResolution());
}

FText SDreamTextureReflowActions::GetDenoiseTierText() const
{
	return DescribeDenoiseTier(Actions->GetDenoise());
}

FText SDreamTextureReflowActions::GetDenoiseWarning() const
{
	FString Why;
	Actions->IsDenoiseBlocked(&Why);
	return FText::FromString(Why);
}

EVisibility SDreamTextureReflowActions::GetDenoiseWarningVisibility() const
{
	return Actions->IsDenoiseBlocked() ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SDreamTextureReflowActions::GetUVKindVisibility() const
{
	// 没有解析出材质时没有东西可记，开关不出现。
	return Actions->GetModel()->GetViewState() == FDreamTextureReflowModel::EViewState::Ready ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SDreamTextureReflowActions::GetInPlaceHint() const
{
	FString Why;
	Actions->CanOverwriteInPlace(&Why);
	return FText::FromString(Why);
}

EVisibility SDreamTextureReflowActions::GetInPlaceHintVisibility() const
{
	return Actions->CanOverwriteInPlace() ? EVisibility::Collapsed : EVisibility::Visible;
}

bool SDreamTextureReflowActions::IsRunEnabled() const
{
	return Actions->CanRun();
}

FText SDreamTextureReflowActions::GetRunReason() const
{
	FText Why;
	Actions->CanRun(&Why);
	return Why;
}

EVisibility SDreamTextureReflowActions::GetRunReasonVisibility() const
{
	return Actions->CanRun() ? EVisibility::Collapsed : EVisibility::Visible;
}

FText SDreamTextureReflowActions::GetRunNote() const
{
	return Actions->GetRunNote();
}

EVisibility SDreamTextureReflowActions::GetRunNoteVisibility() const
{
	return Actions->GetRunNote().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility SDreamTextureReflowActions::GetRunStatusVisibility() const
{
	const TSharedPtr<FDreamTextureReflowRun> Run = Actions->GetRun();
	return Run.IsValid() && Run->GetState() != FDreamTextureReflowRun::EState::Idle ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SDreamTextureReflowActions::GetStepsText() const
{
	const TSharedPtr<FDreamTextureReflowRun> Run = Actions->GetRun();
	if (!Run.IsValid())
	{
		return FText::GetEmpty();
	}

	// 「导出 → 上传 → 提交 → 生成 → 下载 → 导入」，当前（或失败）的那一步前后加记号。
	TArray<EDreamTextureReflowStep> Steps = {
		EDreamTextureReflowStep::Export, EDreamTextureReflowStep::Upload, EDreamTextureReflowStep::Submit,
		EDreamTextureReflowStep::Generate, EDreamTextureReflowStep::Download, EDreamTextureReflowStep::Import,
	};
	if (Run->GetSpec().bInPlace)
	{
		Steps.Add(EDreamTextureReflowStep::Apply);
	}

	TArray<FString> Parts;
	const EDreamTextureReflowStep Current = Run->GetStep();
	for (const EDreamTextureReflowStep Step : Steps)
	{
		FString Name = DescribeReflowStep(Step).ToString();
		if (Step == Current)
		{
			Name = Run->GetState() == FDreamTextureReflowRun::EState::Failed ? FString::Printf(TEXT("✗ %s"), *Name) : FString::Printf(TEXT("● %s"), *Name);
		}
		Parts.Add(Name);
	}
	return FText::FromString(FString::Join(Parts, TEXT(" → ")));
}

FText SDreamTextureReflowActions::GetRunMessage() const
{
	const TSharedPtr<FDreamTextureReflowRun> Run = Actions->GetRun();
	return Run.IsValid() ? Run->GetMessage() : FText::GetEmpty();
}

FSlateColor SDreamTextureReflowActions::GetRunMessageColor() const
{
	const TSharedPtr<FDreamTextureReflowRun> Run = Actions->GetRun();
	if (!Run.IsValid())
	{
		return FSlateColor::UseForeground();
	}
	switch (Run->GetState())
	{
	case FDreamTextureReflowRun::EState::Failed: return FSlateColor(FStyleColors::Error);
	case FDreamTextureReflowRun::EState::Succeeded: return FSlateColor(FStyleColors::Success);
	default: return FSlateColor::UseForeground();
	}
}

TOptional<float> SDreamTextureReflowActions::GetRunProgress() const
{
	const TSharedPtr<FDreamTextureReflowRun> Run = Actions->GetRun();
	return Run.IsValid() ? Run->GetProgress() : TOptional<float>();
}

EVisibility SDreamTextureReflowActions::GetProgressVisibility() const
{
	const TSharedPtr<FDreamTextureReflowRun> Run = Actions->GetRun();
	return Run.IsValid() && Run->IsRunning() ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SDreamTextureReflowActions::GetCancelVisibility() const
{
	return Actions->IsRunActive() ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SDreamTextureReflowActions::GetResultVisibility() const
{
	const TSharedPtr<FDreamTextureReflowRun> Run = Actions->GetRun();
	return Run.IsValid() && Run->GetState() == FDreamTextureReflowRun::EState::Succeeded ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SDreamTextureReflowActions::GetShowButtonVisibility() const
{
	const TSharedPtr<FDreamTextureReflowRun> Run = Actions->GetRun();
	return Run.IsValid() && !Run->GetResultInstancePath().IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
}

const FSlateBrush* SDreamTextureReflowActions::GetLeftPreviewBrush() const
{
	const FDreamTextureReflowPreviewPlan Plan = Actions->GetPreviewPlan();
	return PreviewCache.IsValid() && !Plan.LeftKey.IsEmpty() ? PreviewCache->GetBrush(Plan.LeftKey) : nullptr;
}

const FSlateBrush* SDreamTextureReflowActions::GetRightPreviewBrush() const
{
	const FDreamTextureReflowPreviewPlan Plan = Actions->GetPreviewPlan();
	return PreviewCache.IsValid() && !Plan.RightKey.IsEmpty() ? PreviewCache->GetBrush(Plan.RightKey) : nullptr;
}

FText SDreamTextureReflowActions::GetLeftPreviewLabel() const
{
	return Actions->GetPreviewPlan().LeftLabel;
}

FText SDreamTextureReflowActions::GetRightPreviewLabel() const
{
	return Actions->GetPreviewPlan().RightLabel;
}

EVisibility SDreamTextureReflowActions::GetPreviewVisibility() const
{
	// 左边那张已经有画刷了才占地方；加载中不画空框。
	const FDreamTextureReflowPreviewPlan Plan = Actions->GetPreviewPlan();
	return PreviewCache.IsValid() && Plan.Kind != FDreamTextureReflowPreviewPlan::EKind::None && PreviewCache->HasBrush(Plan.LeftKey)
		? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SDreamTextureReflowActions::GetRightPreviewVisibility() const
{
	const FDreamTextureReflowPreviewPlan Plan = Actions->GetPreviewPlan();
	return Plan.Kind == FDreamTextureReflowPreviewPlan::EKind::SideBySide ? EVisibility::Visible : EVisibility::Collapsed;
}

//////////////////////////////////////////////////////////////////////////
// 按钮

FReply SDreamTextureReflowActions::OnRunClicked()
{
	Actions->Run();
	return FReply::Handled();
}

FReply SDreamTextureReflowActions::OnCancelClicked()
{
	Actions->CancelRun();
	return FReply::Handled();
}

FReply SDreamTextureReflowActions::OnShowClicked()
{
	const TSharedPtr<FDreamTextureReflowRun> Run = Actions->GetRun();
	if (Run.IsValid() && !Run->GetResultInstancePath().IsEmpty())
	{
		FDreamTextureImportNotifier::ShowAssetInContentBrowser(Run->GetResultInstancePath(), FPackageName::GetLongPackagePath(FPackageName::ObjectPathToPackageName(Run->GetResultInstancePath())));
	}
	return FReply::Handled();
}

//////////////////////////////////////////////////////////////////////////
// 测试钩子

void SDreamTextureReflowActions::BuildWidgetsForTest()
{
	HandleActionsChanged();
	GetDecomposeLabel();
	GetDecomposeWarning();
	GetDecomposeWarningVisibility();
	GetRestyleSourceLabel();
	GetRestyleVisibility();
	GetSourceRowVisibility();
	GetSourceLabel();
	GetResolutionVisibility();
	GetResolutionLabel();
	GetDenoiseTierText();
	GetInPlaceHint();
	GetInPlaceHintVisibility();
	IsRunEnabled();
	GetRunReason();
	GetRunReasonVisibility();
	GetRunNote();
	GetRunNoteVisibility();
	GetRunStatusVisibility();
	GetStepsText();
	GetRunMessage();
	GetRunMessageColor();
	GetRunProgress();
	GetProgressVisibility();
	GetCancelVisibility();
	GetResultVisibility();
	GetShowButtonVisibility();
	GetDenoiseWarning();
	GetDenoiseWarningVisibility();
	GetUVKindVisibility();
	GetLeftPreviewBrush();
	GetRightPreviewBrush();
	GetLeftPreviewLabel();
	GetRightPreviewLabel();
	GetPreviewVisibility();
	GetRightPreviewVisibility();

	// 真走一遍 Slate 的布局：SlatePrepass 递归算每个可见控件的期望尺寸，期间所有属性回调（文字、可见性、颜色）
	// 都会被调到，通道行、下拉、进度条的取值代码才算真跑过。
	SlatePrepass(1.0f);
}

#undef LOCTEXT_NAMESPACE
