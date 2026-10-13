// Copyright TypeDreamMoon. All Rights Reserved.

#include "SDreamTextureReflowPanel.h"

#include "AssetRegistry/AssetData.h"
#include "DreamTextureReflowActions.h"
#include "SDreamTextureReflowActions.h"
#include "AssetThumbnail.h"
#include "Engine/Texture.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "DreamTextureReflowPanel"

namespace
{
	const FName ColThumb(TEXT("Thumb"));
	const FName ColTexture(TEXT("Texture"));
	const FName ColRole(TEXT("Role"));
	const FName ColEvidence(TEXT("Evidence"));
	const FName ColExport(TEXT("Export"));

	constexpr float ThumbSize = 56.0f;

	/** 下拉里的一项：一个角色，或「自动」（撤销指认）。 */
	struct FChoice
	{
		EDreamTextureRole Role = EDreamTextureRole::Unknown;
		bool bAuto = false;
	};
	using FChoicePtr = TSharedPtr<FChoice>;

	const TArray<FChoicePtr>& GetRoleChoices()
	{
		static const TArray<FChoicePtr> Choices = []()
		{
			TArray<FChoicePtr> Out;
			auto Add = [&Out](EDreamTextureRole Role, bool bAuto)
			{
				FChoicePtr Choice = MakeShared<FChoice>();
				Choice->Role = Role;
				Choice->bAuto = bAuto;
				Out.Add(Choice);
			};
			Add(EDreamTextureRole::Unknown, true);
			for (const EDreamTextureRole Role : { EDreamTextureRole::BaseColor, EDreamTextureRole::Normal, EDreamTextureRole::Roughness, EDreamTextureRole::Metallic,
				EDreamTextureRole::AO, EDreamTextureRole::Height, EDreamTextureRole::Packed, EDreamTextureRole::Emissive, EDreamTextureRole::Opacity,
				EDreamTextureRole::Specular, EDreamTextureRole::Other })
			{
				Add(Role, false);
			}
			return Out;
		}();
		return Choices;
	}

	/** 打包图单个通道里可以放的东西。 */
	const TArray<FChoicePtr>& GetChannelChoices()
	{
		static const TArray<FChoicePtr> Choices = []()
		{
			TArray<FChoicePtr> Out;
			for (const EDreamTextureRole Role : { EDreamTextureRole::Unknown, EDreamTextureRole::AO, EDreamTextureRole::Roughness, EDreamTextureRole::Metallic,
				EDreamTextureRole::Height, EDreamTextureRole::Opacity, EDreamTextureRole::Specular, EDreamTextureRole::Other })
			{
				FChoicePtr Choice = MakeShared<FChoice>();
				Choice->Role = Role;
				Out.Add(Choice);
			}
			return Out;
		}();
		return Choices;
	}

	FText MaxEdgeLabel(int32 MaxEdge)
	{
		return MaxEdge <= 0 ? LOCTEXT("MaxEdgeOriginal", "Original size") : FText::Format(LOCTEXT("MaxEdgeValue", "Long edge ≤ {0}"), FText::AsNumber(MaxEdge));
	}

	//////////////////////////////////////////////////////////////////////////
	// 结果表的一行

	class SDreamTextureReflowRow : public SMultiColumnTableRow<FDreamTextureReflowRowItemPtr>
	{
	public:
		SLATE_BEGIN_ARGS(SDreamTextureReflowRow) {}
			SLATE_ARGUMENT(FDreamTextureReflowRowItemPtr, Item)
			SLATE_ARGUMENT(TSharedPtr<FDreamTextureReflowModel>, Model)
			SLATE_ARGUMENT(TSharedPtr<FAssetThumbnailPool>, ThumbnailPool)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable)
		{
			Item = InArgs._Item;
			Model = InArgs._Model;
			ThumbnailPool = InArgs._ThumbnailPool;
			SMultiColumnTableRow<FDreamTextureReflowRowItemPtr>::Construct(FSuperRowType::FArguments().Padding(FMargin(0.0f, 3.0f)), OwnerTable);
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
		{
			if (ColumnName == ColThumb)
			{
				return MakeThumbnail();
			}
			if (ColumnName == ColTexture)
			{
				return MakeTextureCell();
			}
			if (ColumnName == ColRole)
			{
				return MakeRoleCell();
			}
			if (ColumnName == ColEvidence)
			{
				return MakeEvidenceCell();
			}
			return MakeExportCell();
		}

	private:
		const FDreamTextureMaterialTexture* Data() const
		{
			return Model.IsValid() && Item.IsValid() ? Model->FindTexture(Item->TexturePath) : nullptr;
		}

		TSharedRef<SWidget> MakeThumbnail()
		{
			const FDreamTextureMaterialTexture* Row = Data();
			UTexture* Texture = Row != nullptr ? Row->Texture.Get() : nullptr;
			if (ThumbnailPool.IsValid() && Texture != nullptr)
			{
				const TSharedRef<FAssetThumbnail> Thumbnail = MakeShared<FAssetThumbnail>(FAssetData(Texture), static_cast<uint32>(ThumbSize), static_cast<uint32>(ThumbSize), ThumbnailPool);
				return SNew(SBox)
					.WidthOverride(ThumbSize)
					.HeightOverride(ThumbSize)
					[
						Thumbnail->MakeThumbnailWidget()
					];
			}
			return SNew(SBox)
				.WidthOverride(ThumbSize)
				.HeightOverride(ThumbSize)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
				];
		}

		//~ 贴图列
		FText GetNameText() const
		{
			const FDreamTextureMaterialTexture* Row = Data();
			return Row != nullptr ? FText::FromString(Row->AssetName) : FText::GetEmpty();
		}

		FText GetPathText() const
		{
			const FDreamTextureMaterialTexture* Row = Data();
			return Row != nullptr ? FText::FromString(Row->TexturePath) : FText::GetEmpty();
		}

		FText GetBindingText() const
		{
			const FDreamTextureMaterialTexture* Row = Data();
			if (Row == nullptr)
			{
				return FText::GetEmpty();
			}
			FText Binding;
			if (Row->Parameters.Num() == 0)
			{
				Binding = LOCTEXT("DirectRef", "Direct reference (no parameter)");
			}
			else
			{
				TArray<FString> Names;
				for (const FName& Name : Row->Parameters)
				{
					Names.Add(Name.ToString());
				}
				Binding = FText::Format(LOCTEXT("ParamNames", "Parameter(s): {0}"), FText::FromString(FString::Join(Names, *LOCTEXT("ListSeparator", ", ").ToString())));
			}
			if (Row->bOverriddenBySelectedInstance)
			{
				Binding = FText::Format(LOCTEXT("OverriddenHere", "{0} (overridden on this instance)"), Binding);
			}
			return Binding;
		}

		FText GetSizeText() const
		{
			const FDreamTextureMaterialTexture* Row = Data();
			if (Row == nullptr || Row->SourceWidth <= 0)
			{
				return LOCTEXT("NoSourceSize", "No source data");
			}
			return FText::Format(LOCTEXT("SizeText", "{0}×{1}"), FText::AsNumber(Row->SourceWidth, &FNumberFormattingOptions::DefaultNoGrouping()), FText::AsNumber(Row->SourceHeight, &FNumberFormattingOptions::DefaultNoGrouping()));
		}

		FText GetUsesText() const
		{
			const FDreamTextureMaterialTexture* Row = Data();
			return Row != nullptr ? FDreamTextureReflowModel::DescribeUses(*Row) : FText::GetEmpty();
		}

		TSharedRef<SWidget> MakeTextureCell()
		{
			return SNew(SVerticalBox)
				.ToolTipText(this, &SDreamTextureReflowRow::GetPathText)

				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(this, &SDreamTextureReflowRow::GetNameText)
					.Font(FAppStyle::GetFontStyle("BoldFont"))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(this, &SDreamTextureReflowRow::GetBindingText)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(this, &SDreamTextureReflowRow::GetSizeText)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(this, &SDreamTextureReflowRow::GetUsesText)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				];
		}

		//~ 通道列
		FText GetRoleText() const
		{
			const FDreamTextureMaterialTexture* Row = Data();
			return Row != nullptr ? FDreamTextureReflowModel::RoleDisplayName(Row->Role) : FText::GetEmpty();
		}

		EVisibility GetLayoutVisibility() const
		{
			const FDreamTextureMaterialTexture* Row = Data();
			return Row != nullptr && Row->Role == EDreamTextureRole::Packed ? EVisibility::Visible : EVisibility::Collapsed;
		}

		EVisibility GetNeedsInputVisibility() const
		{
			const FDreamTextureMaterialTexture* Row = Data();
			return Row != nullptr && Row->bNeedsUserInput ? EVisibility::Visible : EVisibility::Collapsed;
		}

		FText GetNeedsInputText() const
		{
			const FDreamTextureMaterialTexture* Row = Data();
			if (Row != nullptr && Row->Role == EDreamTextureRole::Packed)
			{
				return LOCTEXT("NeedsLayout", "Needs a manual assignment: choose what R, G and B each hold");
			}
			return LOCTEXT("NeedsRole", "Needs a manual assignment: not recognized, choose one");
		}

		FText GetChannelText(int32 Channel) const
		{
			const FDreamTextureMaterialTexture* Row = Data();
			return Row != nullptr ? FDreamTextureReflowModel::ChannelContentDisplayName(Row->Layout.Channels[Channel]) : FText::GetEmpty();
		}

		void OnRoleChosen(FChoicePtr Choice, ESelectInfo::Type SelectInfo)
		{
			if (SelectInfo == ESelectInfo::Direct || !Choice.IsValid() || !Model.IsValid() || !Item.IsValid())
			{
				return;
			}
			if (Choice->bAuto)
			{
				Model->ClearOverride(Item->TexturePath);
			}
			else
			{
				Model->SetRole(Item->TexturePath, Choice->Role);
			}
			// 下拉自己记着上次选的项，用户再选同一项时不会触发回调；每次选完清掉。
			if (RoleCombo.IsValid())
			{
				RoleCombo->ClearSelection();
			}
		}

		TSharedRef<SWidget> MakeChannelCombo(int32 Channel)
		{
			TSharedPtr<SComboBox<FChoicePtr>> Combo;
			TSharedRef<SWidget> Widget = SAssignNew(Combo, SComboBox<FChoicePtr>)
				.OptionsSource(&GetChannelChoices())
				.OnGenerateWidget_Lambda([](FChoicePtr Choice)
				{
					return SNew(STextBlock).Text(FDreamTextureReflowModel::ChannelContentDisplayName(Choice->Role));
				})
				.OnSelectionChanged_Lambda([this, Channel](FChoicePtr Choice, ESelectInfo::Type SelectInfo)
				{
					if (SelectInfo != ESelectInfo::Direct && Choice.IsValid() && Model.IsValid() && Item.IsValid())
					{
						Model->SetLayoutChannel(Item->TexturePath, Channel, Choice->Role);
						ChannelCombos[Channel]->ClearSelection();
					}
				})
				[
					SNew(STextBlock)
					.Text_Lambda([this, Channel]() { return GetChannelText(Channel); })
				];
			ChannelCombos[Channel] = Combo;
			return Widget;
		}

		TSharedRef<SWidget> MakeRoleCell()
		{
			static const TCHAR* const ChannelNames[4] = { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") };

			TSharedRef<SHorizontalBox> Layout = SNew(SHorizontalBox);
			for (int32 Channel = 0; Channel < 4; ++Channel)
			{
				Layout->AddSlot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(FMargin(Channel == 0 ? 0.0f : 6.0f, 0.0f, 2.0f, 0.0f))
					[
						SNew(STextBlock)
						.Text(FText::FromString(ChannelNames[Channel]))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					];
				Layout->AddSlot()
					.AutoWidth()
					[
						MakeChannelCombo(Channel)
					];
			}

			return SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight()
				[
					SAssignNew(RoleCombo, SComboBox<FChoicePtr>)
					.OptionsSource(&GetRoleChoices())
					.OnGenerateWidget_Lambda([](FChoicePtr Choice)
					{
						return SNew(STextBlock).Text(Choice->bAuto ? LOCTEXT("AutoChoice", "Automatic (undo my assignment)") : FDreamTextureReflowModel::RoleDisplayName(Choice->Role));
					})
					.OnSelectionChanged(this, &SDreamTextureReflowRow::OnRoleChosen)
					.ToolTipText(LOCTEXT("RoleComboTip", "What this texture is in the material. Changes are remembered under Saved/DreamTexture in the project and never modify your material."))
					[
						SNew(STextBlock).Text(this, &SDreamTextureReflowRow::GetRoleText)
					]
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
				[
					SNew(SBox)
					.Visibility(this, &SDreamTextureReflowRow::GetLayoutVisibility)
					[
						Layout
					]
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
				[
					SNew(STextBlock)
					.Visibility(this, &SDreamTextureReflowRow::GetNeedsInputVisibility)
					.Text(this, &SDreamTextureReflowRow::GetNeedsInputText)
					.ColorAndOpacity(FStyleColors::Warning)
					.AutoWrapText(true)
				];
		}

		//~ 依据列
		FText GetEvidenceText() const
		{
			const FDreamTextureMaterialTexture* Row = Data();
			return Row != nullptr ? FDreamTextureReflowModel::EvidenceDisplayName(Row->Evidence) : FText::GetEmpty();
		}

		FSlateColor GetEvidenceColor() const
		{
			const FDreamTextureMaterialTexture* Row = Data();
			if (Row == nullptr)
			{
				return FSlateColor::UseForeground();
			}
			switch (Row->Evidence)
			{
			case EDreamTextureEvidence::UserOverride:
			case EDreamTextureEvidence::SourceRecord:
			case EDreamTextureEvidence::MaterialGraph:
				return FStyleColors::Success;
			case EDreamTextureEvidence::None:
				return FStyleColors::Error;
			default:
				// 名字、采样器类型都是猜：用警告色提醒别全信。
				return FStyleColors::Warning;
			}
		}

		FText GetEvidenceDetailText() const
		{
			const FDreamTextureMaterialTexture* Row = Data();
			return Row != nullptr ? FText::FromString(Row->EvidenceDetail) : FText::GetEmpty();
		}

		FText GetNotesText() const
		{
			const FDreamTextureMaterialTexture* Row = Data();
			return Row != nullptr ? FText::FromString(FString::Join(Row->Notes, TEXT("\n"))) : FText::GetEmpty();
		}

		EVisibility GetNotesVisibility() const
		{
			const FDreamTextureMaterialTexture* Row = Data();
			return Row != nullptr && Row->Notes.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed;
		}

		TSharedRef<SWidget> MakeEvidenceCell()
		{
			return SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(this, &SDreamTextureReflowRow::GetEvidenceText)
					.ColorAndOpacity(this, &SDreamTextureReflowRow::GetEvidenceColor)
					.Font(FAppStyle::GetFontStyle("BoldFont"))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(this, &SDreamTextureReflowRow::GetEvidenceDetailText)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 2.0f, 0.0f, 0.0f))
				[
					SNew(STextBlock)
					.Visibility(this, &SDreamTextureReflowRow::GetNotesVisibility)
					.Text(this, &SDreamTextureReflowRow::GetNotesText)
					.ColorAndOpacity(FStyleColors::Warning)
					.AutoWrapText(true)
				];
		}

		//~ 导出列
		FText GetExportText() const
		{
			const FDreamTextureMaterialTexture* Row = Data();
			if (Row == nullptr)
			{
				return FText::GetEmpty();
			}
			if (const FDreamTextureExportedImage* Exported = Model->FindExport(Row->TexturePath))
			{
				if (Exported->bExported)
				{
					return FText::Format(LOCTEXT("Exported", "Exported {0}×{1}{2}\n{3}"),
						FText::AsNumber(Exported->Width), FText::AsNumber(Exported->Height),
						Exported->bResized ? LOCTEXT("ExportedResized", " (resized down)") : FText::GetEmpty(),
						FText::FromString(FPaths::GetCleanFilename(Exported->FilePath)));
				}
				return FText::Format(LOCTEXT("ExportFailed", "Cannot export: {0}"), FText::FromString(Exported->Reason));
			}
			if (Row->bExportable)
			{
				return LOCTEXT("Exportable", "Can be exported");
			}
			return FText::Format(LOCTEXT("NotExportable", "Cannot export: {0}"), FText::FromString(Row->ExportBlocker));
		}

		FSlateColor GetExportColor() const
		{
			const FDreamTextureMaterialTexture* Row = Data();
			if (Row == nullptr)
			{
				return FSlateColor::UseForeground();
			}
			if (const FDreamTextureExportedImage* Exported = Model->FindExport(Row->TexturePath))
			{
				return Exported->bExported ? FStyleColors::Success : FStyleColors::Error;
			}
			return Row->bExportable ? FSlateColor::UseForeground() : FSlateColor(FStyleColors::Error);
		}

		FText GetExportTip() const
		{
			const FDreamTextureMaterialTexture* Row = Data();
			if (Row == nullptr)
			{
				return FText::GetEmpty();
			}
			if (const FDreamTextureExportedImage* Exported = Model->FindExport(Row->TexturePath))
			{
				if (Exported->bExported)
				{
					return FText::Format(LOCTEXT("ExportedTip", "{0}\nsha256 (first 16 characters): {1}\nColor space: {2}"), FText::FromString(Exported->FilePath), FText::FromString(Exported->Sha256),
						Exported->bSRGB ? LOCTEXT("TipSrgb", "sRGB") : LOCTEXT("TipLinear", "Linear"));
				}
				return FText::FromString(Exported->Reason);
			}
			return Row->bExportable ? LOCTEXT("ExportableTip", "Exported straight from the source data, preserving bit depth, with no sRGB conversion.") : FText::FromString(Row->ExportBlocker);
		}

		TSharedRef<SWidget> MakeExportCell()
		{
			return SNew(STextBlock)
				.Text(this, &SDreamTextureReflowRow::GetExportText)
				.ColorAndOpacity(this, &SDreamTextureReflowRow::GetExportColor)
				.ToolTipText(this, &SDreamTextureReflowRow::GetExportTip)
				.AutoWrapText(true);
		}

		FDreamTextureReflowRowItemPtr Item;
		TSharedPtr<FDreamTextureReflowModel> Model;
		TSharedPtr<FAssetThumbnailPool> ThumbnailPool;
		TSharedPtr<SComboBox<FChoicePtr>> RoleCombo;
		TSharedPtr<SComboBox<FChoicePtr>> ChannelCombos[4];
	};
}

//////////////////////////////////////////////////////////////////////////
// 面板

void SDreamTextureReflowPanel::Construct(const FArguments& InArgs)
{
	Model = InArgs._Model;
	if (!Model.IsValid())
	{
		Model = MakeShared<FDreamTextureReflowModel>();
	}
	bThumbnails = InArgs._bThumbnails;
	if (bThumbnails)
	{
		ThumbnailPool = MakeShared<FAssetThumbnailPool>(64);
	}
	for (const int32 Edge : { 0, 4096, 2048, 1024, 512 })
	{
		MaxEdgeChoices.Add(MakeShared<int32>(Edge));
	}

	ChangedHandle = Model->OnChanged.AddSP(this, &SDreamTextureReflowPanel::HandleChanged);
	NoticeHandle = Model->OnNotice.AddSP(this, &SDreamTextureReflowPanel::HandleNotice);

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
		.AutoHeight()
		[
			MakeCandidateList()
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeHeader()
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SWidgetSwitcher)
			.WidgetIndex(this, &SDreamTextureReflowPanel::GetViewIndex)

			// 0：没有结果可看，说明为什么。
			+ SWidgetSwitcher::Slot()
			[
				SNew(SBox)
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				.Padding(FMargin(24.0f))
				[
					SNew(STextBlock)
					.Text(this, &SDreamTextureReflowPanel::GetStatusText)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
					.Justification(ETextJustify::Center)
				]
			]

			// 1：结果表。
			+ SWidgetSwitcher::Slot()
			[
				MakeTable()
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
			.Padding(FMargin(8.0f, 6.0f))
			[
				MakeFooter()
			]
		]
	];

	// 动作区（M8）：放进给它留的容器里。自己带滚动并限高：动作区有十几行控件，
	// 不限的话它会把上面的结果表挤到只剩一条缝。
	if (InArgs._bActions)
	{
		Actions = InArgs._Actions;
		if (!Actions.IsValid())
		{
			Actions = MakeShared<FDreamTextureReflowActions>(Model.ToSharedRef());
		}
		ActionsWidget = SNew(SDreamTextureReflowActions).Actions(Actions).bThumbnails(bThumbnails);
		ActionsHost->SetContent(
			SNew(SBox)
			.MaxDesiredHeight(380.0f)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					ActionsWidget.ToSharedRef()
				]
			]);
	}

	if (InArgs._bAutoRefresh)
	{
		Model->RefreshSelection();
	}
}

SDreamTextureReflowPanel::~SDreamTextureReflowPanel()
{
	// 模型可能比面板活得久（测试里是外面持有的），所以一定要摘。
	if (Model.IsValid())
	{
		Model->OnChanged.Remove(ChangedHandle);
		Model->OnNotice.Remove(NoticeHandle);
	}
}

TSharedRef<SBox> SDreamTextureReflowPanel::GetActionsHost() const
{
	return ActionsHost.ToSharedRef();
}

//////////////////////////////////////////////////////////////////////////
// 构件

TSharedRef<SWidget> SDreamTextureReflowPanel::MakeToolbar()
{
	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
		[
			SNew(SButton)
			.ContentPadding(FMargin(8.0f, 2.0f))
			.ToolTipText(LOCTEXT("RefreshTip", "Read the material or material instance selected in the Content Browser, and the material slots of the Actor selected in the level, then analyze them again."))
			.OnClicked(this, &SDreamTextureReflowPanel::OnRefreshClicked)
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
					SNew(STextBlock).Text(LOCTEXT("Refresh", "Get Selection"))
				]
			]
		]

		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("ToolbarHint", "Select a material or material instance in the Content Browser, or an Actor in the level, then click the button on the left."))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.AutoWrapText(true)
		];
}

TSharedRef<SWidget> SDreamTextureReflowPanel::MakeCandidateList()
{
	return SNew(SBox)
		.Visibility(this, &SDreamTextureReflowPanel::GetCandidateListVisibility)
		.MaxDesiredHeight(140.0f)
		.Padding(FMargin(8.0f, 4.0f))
		[
			SAssignNew(CandidateList, SListView<TSharedPtr<int32>>)
			.ListItemsSource(&CandidateItems)
			.SelectionMode(ESelectionMode::Single)
			.OnGenerateRow(this, &SDreamTextureReflowPanel::OnGenerateCandidate)
			.OnSelectionChanged(this, &SDreamTextureReflowPanel::OnCandidateSelected)
		];
}

TSharedRef<SWidget> SDreamTextureReflowPanel::MakeHeader()
{
	return SNew(SVerticalBox)
		.Visibility(this, &SDreamTextureReflowPanel::GetHeaderVisibility)

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 6.0f, 8.0f, 0.0f))
		[
			SNew(STextBlock)
			.Text(this, &SDreamTextureReflowPanel::GetHeaderText)
			.Font(FAppStyle::GetFontStyle("BoldFont"))
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 0.0f, 8.0f, 4.0f))
		[
			SNew(STextBlock)
			.Text(this, &SDreamTextureReflowPanel::GetHeaderDetailText)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.AutoWrapText(true)
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 0.0f, 8.0f, 4.0f))
		[
			SNew(STextBlock)
			.Visibility(this, &SDreamTextureReflowPanel::GetWarningsVisibility)
			.Text(this, &SDreamTextureReflowPanel::GetWarningsText)
			.ColorAndOpacity(FStyleColors::Warning)
			.AutoWrapText(true)
		];
}

TSharedRef<SWidget> SDreamTextureReflowPanel::MakeTable()
{
	return SAssignNew(RowList, SListView<FDreamTextureReflowRowItemPtr>)
		.ListItemsSource(&Model->GetRows())
		.SelectionMode(ESelectionMode::None)
		.OnGenerateRow(this, &SDreamTextureReflowPanel::OnGenerateRow)
		.HeaderRow
		(
			SAssignNew(HeaderRow, SHeaderRow)

			+ SHeaderRow::Column(ColThumb).DefaultLabel(FText::GetEmpty()).FixedWidth(ThumbSize + 8.0f)
			+ SHeaderRow::Column(ColTexture).DefaultLabel(LOCTEXT("ColTexture", "Texture")).FillWidth(0.28f)
			+ SHeaderRow::Column(ColRole).DefaultLabel(LOCTEXT("ColRole", "Channel")).FillWidth(0.26f)
			+ SHeaderRow::Column(ColEvidence).DefaultLabel(LOCTEXT("ColEvidence", "Evidence")).FillWidth(0.28f)
			+ SHeaderRow::Column(ColExport).DefaultLabel(LOCTEXT("ColExport", "Export")).FillWidth(0.18f)
		);
}

TSharedRef<SWidget> SDreamTextureReflowPanel::MakeFooter()
{
	return SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				SAssignNew(MaxEdgeCombo, SComboBox<TSharedPtr<int32>>)
				.OptionsSource(&MaxEdgeChoices)
				.ToolTipText(LOCTEXT("MaxEdgeTip", "Before export, shrink the long edge to at most this value (shrink only, never enlarge). Use it when a decompose model cannot take a very large image."))
				.OnGenerateWidget_Lambda([](TSharedPtr<int32> Choice)
				{
					return SNew(STextBlock).Text(MaxEdgeLabel(*Choice));
				})
				.OnSelectionChanged_Lambda([this](TSharedPtr<int32> Choice, ESelectInfo::Type SelectInfo)
				{
					if (SelectInfo != ESelectInfo::Direct && Choice.IsValid())
					{
						Model->SetExportMaxEdge(*Choice);
					}
				})
				[
					SNew(STextBlock).Text(this, &SDreamTextureReflowPanel::GetMaxEdgeText)
				]
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				SNew(SButton)
				.ContentPadding(FMargin(10.0f, 2.0f))
				.ToolTipText(LOCTEXT("ExportTip", "Export the exportable textures from their source data as PNG into a new temporary folder under the project's Saved/DreamTexture/Reflow. Textures without source data are not forced out."))
				.IsEnabled(this, &SDreamTextureReflowPanel::IsExportEnabled)
				.OnClicked(this, &SDreamTextureReflowPanel::OnExportClicked)
				[
					SNew(STextBlock).Text(this, &SDreamTextureReflowPanel::GetExportButtonText)
				]
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				SNew(SButton)
				.Visibility(this, &SDreamTextureReflowPanel::GetOpenFolderVisibility)
				.ContentPadding(FMargin(10.0f, 2.0f))
				.OnClicked(this, &SDreamTextureReflowPanel::OnOpenFolderClicked)
				[
					SNew(STextBlock).Text(LOCTEXT("OpenFolder", "Open folder"))
				]
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SDreamTextureReflowPanel::GetExportSummaryText)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.AutoWrapText(true)
			]
		]

		// 回流动作区挂在这里（见 Construct 末尾）。没建动作区时它是个空容器，不占地方。
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
		[
			SAssignNew(ActionsHost, SBox)
		];
}

//////////////////////////////////////////////////////////////////////////
// 行

TSharedRef<ITableRow> SDreamTextureReflowPanel::OnGenerateRow(FDreamTextureReflowRowItemPtr Item, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(SDreamTextureReflowRow, Owner)
		.Item(Item)
		.Model(Model)
		.ThumbnailPool(bThumbnails ? ThumbnailPool : nullptr);
}

TSharedRef<ITableRow> SDreamTextureReflowPanel::OnGenerateCandidate(TSharedPtr<int32> Item, const TSharedRef<STableViewBase>& Owner)
{
	const TSharedPtr<FDreamTextureReflowModel> Captured = Model;
	const int32 Index = *Item;
	return SNew(STableRow<TSharedPtr<int32>>, Owner)
		.Padding(FMargin(6.0f, 3.0f))
		[
			SNew(STextBlock)
			.Text_Lambda([Captured, Index]()
			{
				if (Captured.IsValid() && Captured->GetCandidates().IsValidIndex(Index))
				{
					const FDreamTextureReflowCandidate& Candidate = Captured->GetCandidates()[Index];
					const FText Kind = Candidate.Kind == FDreamTextureReflowCandidate::EKind::Asset ? LOCTEXT("KindAsset", "Asset") : LOCTEXT("KindSlot", "Material slot");
					return FText::Format(LOCTEXT("CandidateLabel", "[{0}] {1}"), Kind, FText::FromString(Candidate.Label));
				}
				return FText::GetEmpty();
			})
		];
}

void SDreamTextureReflowPanel::OnCandidateSelected(TSharedPtr<int32> Item, ESelectInfo::Type SelectInfo)
{
	if (bSyncingSelection || SelectInfo == ESelectInfo::Direct || !Item.IsValid())
	{
		return;
	}
	Model->SelectCandidate(*Item);
}

//////////////////////////////////////////////////////////////////////////
// 模型事件

void SDreamTextureReflowPanel::SyncCandidateSelection()
{
	if (!CandidateList.IsValid())
	{
		return;
	}
	bSyncingSelection = true;
	CandidateList->ClearSelection();
	const int32 Selected = Model->GetSelectedIndex();
	if (CandidateItems.IsValidIndex(Selected))
	{
		CandidateList->SetItemSelection(CandidateItems[Selected], true, ESelectInfo::Direct);
	}
	bSyncingSelection = false;
}

void SDreamTextureReflowPanel::HandleChanged()
{
	// 候选条目只是下标，位置上稳定：数量变了才重建。
	if (CandidateItems.Num() != Model->GetCandidates().Num())
	{
		CandidateItems.Reset();
		for (int32 Index = 0; Index < Model->GetCandidates().Num(); ++Index)
		{
			CandidateItems.Add(MakeShared<int32>(Index));
		}
	}
	if (CandidateList.IsValid())
	{
		CandidateList->RequestListRefresh();
	}
	if (RowList.IsValid())
	{
		RowList->RequestListRefresh();
	}
	SyncCandidateSelection();
}

void SDreamTextureReflowPanel::HandleNotice(const FText& Message, bool bIsError)
{
	// 通知只在有 Slate 的进程里弹；-nullrhi 的测试里 IsInitialized 可能为假。
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}

	FNotificationInfo Info(Message);
	Info.ExpireDuration = bIsError ? 6.0f : 4.0f;
	Info.bUseSuccessFailIcons = bIsError;
	Info.bFireAndForget = true;
	if (TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
	{
		Item->SetCompletionState(bIsError ? SNotificationItem::CS_Fail : SNotificationItem::CS_None);
	}
}

//////////////////////////////////////////////////////////////////////////
// 按钮

FReply SDreamTextureReflowPanel::OnRefreshClicked()
{
	Model->RefreshSelection();
	return FReply::Handled();
}

FReply SDreamTextureReflowPanel::OnExportClicked()
{
	Model->Export();
	return FReply::Handled();
}

FReply SDreamTextureReflowPanel::OnOpenFolderClicked()
{
	if (Model->HasExported())
	{
		FPlatformProcess::ExploreFolder(*Model->GetExportDir());
	}
	return FReply::Handled();
}

//////////////////////////////////////////////////////////////////////////
// 取值

int32 SDreamTextureReflowPanel::GetViewIndex() const
{
	return Model->GetViewState() == FDreamTextureReflowModel::EViewState::Ready ? 1 : 0;
}

FText SDreamTextureReflowPanel::GetStatusText() const
{
	return Model->GetStatusText();
}

EVisibility SDreamTextureReflowPanel::GetCandidateListVisibility() const
{
	return Model->GetCandidates().Num() > 1 ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SDreamTextureReflowPanel::GetHeaderVisibility() const
{
	return Model->GetViewState() == FDreamTextureReflowModel::EViewState::Ready ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SDreamTextureReflowPanel::GetHeaderText() const
{
	const FDreamTextureMaterialAnalysis& Analysis = Model->GetAnalysis();
	if (const FDreamTextureReflowCandidate* Candidate = Model->GetSelectedCandidate())
	{
		if (Candidate->Kind == FDreamTextureReflowCandidate::EKind::ActorSlot)
		{
			return FText::FromString(Candidate->Label);
		}
	}
	return FText::FromString(Analysis.DisplayName);
}

FText SDreamTextureReflowPanel::GetHeaderDetailText() const
{
	const FDreamTextureMaterialAnalysis& Analysis = Model->GetAnalysis();
	const FText Kind = Analysis.bIsInstance ? LOCTEXT("KindInstance", "Material instance") : LOCTEXT("KindMaterial", "Master material");
	const FText Count = FText::Format(LOCTEXT("HeaderCount", "Uses {0} texture(s), {1} can be exported"), FText::AsNumber(Analysis.Textures.Num()), FText::AsNumber(Model->GetExportableCount()));
	if (Analysis.bIsInstance && !Analysis.BaseMaterialPath.IsEmpty())
	{
		return FText::Format(LOCTEXT("HeaderInstance", "{0}, master material {1}. {2}."), Kind, FText::FromString(FPackageName::ObjectPathToObjectName(Analysis.BaseMaterialPath)), Count);
	}
	return FText::Format(LOCTEXT("HeaderMaterial", "{0}. {1}."), Kind, Count);
}

FText SDreamTextureReflowPanel::GetWarningsText() const
{
	return FText::FromString(FString::Join(Model->GetAnalysis().Warnings, TEXT("\n")));
}

EVisibility SDreamTextureReflowPanel::GetWarningsVisibility() const
{
	return Model->GetAnalysis().Warnings.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed;
}

bool SDreamTextureReflowPanel::IsExportEnabled() const
{
	return Model->CanExport();
}

FText SDreamTextureReflowPanel::GetExportButtonText() const
{
	return FText::Format(LOCTEXT("ExportButton", "Export {0} PNG(s)"), FText::AsNumber(Model->GetExportableCount()));
}

EVisibility SDreamTextureReflowPanel::GetOpenFolderVisibility() const
{
	return Model->HasExported() ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SDreamTextureReflowPanel::GetMaxEdgeText() const
{
	return MaxEdgeLabel(Model->GetExportMaxEdge());
}

FText SDreamTextureReflowPanel::GetExportSummaryText() const
{
	if (Model->HasExported())
	{
		return FText::Format(LOCTEXT("SummaryExported", "Exported to {0}"), FText::FromString(Model->GetExportDir()));
	}
	if (Model->GetViewState() == FDreamTextureReflowModel::EViewState::Ready && Model->GetExportableCount() == 0)
	{
		return LOCTEXT("SummaryNoneExportable", "None of the textures in this material can be exported. See the table for the reasons.");
	}
	return FText::GetEmpty();
}

//////////////////////////////////////////////////////////////////////////
// 测试钩子

void SDreamTextureReflowPanel::BuildWidgetsForTest()
{
	// 先让列表控件吃一遍模型的数据，再手动把每一行生成出来（构造时就会生成各列）。
	HandleChanged();
	if (RowList.IsValid())
	{
		for (const FDreamTextureReflowRowItemPtr& Item : Model->GetRows())
		{
			OnGenerateRow(Item, RowList.ToSharedRef());
		}
	}
	for (int32 Index = 0; Index < CandidateItems.Num(); ++Index)
	{
		if (CandidateList.IsValid())
		{
			OnGenerateCandidate(CandidateItems[Index], CandidateList.ToSharedRef());
		}
	}
	GetViewIndex();
	GetStatusText();
	GetHeaderText();
	GetHeaderDetailText();
	GetWarningsText();
	GetExportButtonText();
	GetExportSummaryText();
	GetMaxEdgeText();
	if (ActionsWidget.IsValid())
	{
		ActionsWidget->BuildWidgetsForTest();
	}
	// 整页布局也走一遍（结果表 + 动作区 + 各块的可见性）。
	SlatePrepass(1.0f);
}

#undef LOCTEXT_NAMESPACE
