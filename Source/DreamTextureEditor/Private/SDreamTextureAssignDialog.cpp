// Copyright TypeDreamMoon. All Rights Reserved.

#include "SDreamTextureAssignDialog.h"

#include "DreamTextureAssignModel.h"
#include "DreamTextureLog.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Materials/MaterialInterface.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "DreamTextureAssignDialog"

void SDreamTextureAssignDialog::Construct(const FArguments& InArgs)
{
	Model = InArgs._Model;
	OnConfirm = InArgs._OnConfirm;
	OnCancel = InArgs._OnCancel;
	check(Model.IsValid());

	const TWeakPtr<FDreamTextureAssignModel> WeakModel = Model;

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
		.Padding(FMargin(16.0f))
		[
			SNew(SVerticalBox)

			// 材质名
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text_Lambda([WeakModel]()
				{
					const TSharedPtr<FDreamTextureAssignModel> Pinned = WeakModel.Pin();
					return Pinned.IsValid()
						? FText::Format(LOCTEXT("MaterialLine", "Material: {0}"), Pinned->GetMaterialName())
						: FText::GetEmpty();
				})
				.ColorAndOpacity(FStyleColors::Foreground)
			]

			// 「将替换 N 个材质槽」：这句话是这个框存在的理由，放大加粗。
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 8.0f, 0.0f, 8.0f))
			[
				SNew(STextBlock)
				.Text(this, &SDreamTextureAssignDialog::GetHeadline)
				.Font(FAppStyle::Get().GetFontStyle("HeadingExtraSmall"))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text_Lambda([WeakModel]()
				{
					const TSharedPtr<FDreamTextureAssignModel> Pinned = WeakModel.Pin();
					return Pinned.IsValid() ? Pinned->GetSourceText() : FText::GetEmpty();
				})
				.AutoWrapText(true)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
			[
				SNew(STextBlock)
				.Text_Lambda([WeakModel]()
				{
					const TSharedPtr<FDreamTextureAssignModel> Pinned = WeakModel.Pin();
					return Pinned.IsValid() ? Pinned->GetTargetListText() : FText::GetEmpty();
				})
				.ColorAndOpacity(FStyleColors::Foreground)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
			[
				SNew(STextBlock)
				.Visibility(this, &SDreamTextureAssignDialog::GetLockedVisibility)
				.Text_Lambda([WeakModel]()
				{
					const TSharedPtr<FDreamTextureAssignModel> Pinned = WeakModel.Pin();
					return Pinned.IsValid() ? Pinned->GetLockedText() : FText::GetEmpty();
				})
				.ColorAndOpacity(FStyleColors::Warning)
				.AutoWrapText(true)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
			[
				SNew(STextBlock)
				.Visibility(this, &SDreamTextureAssignDialog::GetAssetWarningVisibility)
				.Text_Lambda([WeakModel]()
				{
					const TSharedPtr<FDreamTextureAssignModel> Pinned = WeakModel.Pin();
					return Pinned.IsValid() ? Pinned->GetAssetWarningText() : FText::GetEmpty();
				})
				.ColorAndOpacity(FStyleColors::Warning)
				.AutoWrapText(true)
			]

			// 槽位选择：只有目标里有多槽时才出现。
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 10.0f, 0.0f, 0.0f))
			[
				SNew(SBox)
				.Visibility(this, &SDreamTextureAssignDialog::GetSlotPickerVisibility)
				[
					MakeSlotPicker()
				]
			]

			// 不能赋时的原因（比如指定槽位却一个都没勾）。
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 8.0f, 0.0f, 0.0f))
			[
				SNew(STextBlock)
				.Visibility(this, &SDreamTextureAssignDialog::GetHintVisibility)
				.Text(this, &SDreamTextureAssignDialog::GetCannotApplyHint)
				.ColorAndOpacity(FStyleColors::Error)
				.AutoWrapText(true)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Right)
			.Padding(FMargin(0.0f, 14.0f, 0.0f, 0.0f))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.ContentPadding(FMargin(16.0f, 4.0f))
					.IsEnabled(this, &SDreamTextureAssignDialog::CanConfirm)
					.OnClicked(this, &SDreamTextureAssignDialog::HandleConfirm)
					[
						SNew(STextBlock).Text(LOCTEXT("Confirm", "Assign"))
					]
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.ContentPadding(FMargin(16.0f, 4.0f))
					.OnClicked(this, &SDreamTextureAssignDialog::HandleCancel)
					[
						SNew(STextBlock).Text(LOCTEXT("Cancel", "Cancel"))
					]
				]
			]
		]
	];
}

TSharedRef<SWidget> SDreamTextureAssignDialog::MakeSlotPicker()
{
	const TWeakPtr<FDreamTextureAssignModel> WeakModel = Model;

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	Box->AddSlot()
	.AutoHeight()
	[
		SNew(SCheckBox)
		.IsChecked_Lambda([WeakModel]()
		{
			const TSharedPtr<FDreamTextureAssignModel> Pinned = WeakModel.Pin();
			return Pinned.IsValid() && Pinned->IsAllSlots() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([WeakModel](ECheckBoxState State)
		{
			if (const TSharedPtr<FDreamTextureAssignModel> Pinned = WeakModel.Pin())
			{
				Pinned->SetAllSlots(State == ECheckBoxState::Checked);
			}
		})
		[
			SNew(STextBlock)
			.Text(LOCTEXT("AllSlots", "All material slots"))
			.Font(FAppStyle::Get().GetFontStyle("NormalFontBold"))
		]
	];

	// 逐槽的勾选。「全部」勾着时灰掉；取消「全部」后一个都不预选，逼用户明确选，免得又变成闷头全覆盖。
	const int32 MaxSlots = Model->GetMaxSlots();
	for (int32 SlotIndex = 0; SlotIndex < MaxSlots; ++SlotIndex)
	{
		const FText Name = Model->GetSlotName(SlotIndex);
		const FText Label = Name.IsEmpty()
			? FText::Format(LOCTEXT("SlotLabel", "Slot {0} ({1} {1}|plural(one=target has,other=targets have) it)"), FText::AsNumber(SlotIndex), Model->GetNumTargetsWithSlot(SlotIndex))
			: FText::Format(LOCTEXT("SlotLabelNamed", "Slot {0}: {1} ({2} {2}|plural(one=target has,other=targets have) it)"), FText::AsNumber(SlotIndex), Name, Model->GetNumTargetsWithSlot(SlotIndex));

		Box->AddSlot()
		.AutoHeight()
		.Padding(FMargin(20.0f, 2.0f, 0.0f, 0.0f))
		[
			SNew(SCheckBox)
			.IsEnabled_Lambda([WeakModel]()
			{
				const TSharedPtr<FDreamTextureAssignModel> Pinned = WeakModel.Pin();
				return Pinned.IsValid() && !Pinned->IsAllSlots();
			})
			.IsChecked_Lambda([WeakModel, SlotIndex]()
			{
				const TSharedPtr<FDreamTextureAssignModel> Pinned = WeakModel.Pin();
				return Pinned.IsValid() && Pinned->IsSlotChosen(SlotIndex) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([WeakModel, SlotIndex](ECheckBoxState State)
			{
				if (const TSharedPtr<FDreamTextureAssignModel> Pinned = WeakModel.Pin())
				{
					Pinned->SetSlotChosen(SlotIndex, State == ECheckBoxState::Checked);
				}
			})
			[
				SNew(STextBlock).Text(Label)
			]
		];
	}

	// 槽很多时（比如 12 个）别把框撑出屏幕。
	return SNew(SBox)
		.MaxDesiredHeight(220.0f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				Box
			]
		];
}

FText SDreamTextureAssignDialog::GetHeadline() const
{
	return Model->GetHeadline();
}

FText SDreamTextureAssignDialog::GetCannotApplyHint() const
{
	FText Why;
	Model->CanApply(&Why);
	return Why;
}

bool SDreamTextureAssignDialog::CanConfirm() const
{
	return Model->CanApply();
}

EVisibility SDreamTextureAssignDialog::GetSlotPickerVisibility() const
{
	return Model->GetMaxSlots() > 1 ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SDreamTextureAssignDialog::GetLockedVisibility() const
{
	return Model->GetLockedText().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility SDreamTextureAssignDialog::GetAssetWarningVisibility() const
{
	return Model->GetAssetWarningText().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility SDreamTextureAssignDialog::GetHintVisibility() const
{
	return Model->CanApply() ? EVisibility::Collapsed : EVisibility::Visible;
}

FReply SDreamTextureAssignDialog::HandleConfirm()
{
	OnConfirm.ExecuteIfBound();
	return FReply::Handled();
}

FReply SDreamTextureAssignDialog::HandleCancel()
{
	OnCancel.ExecuteIfBound();
	return FReply::Handled();
}

//////////////////////////////////////////////////////////////////////////
// 流程

namespace
{
	void Toast(const FText& Text, bool bSuccess)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}
		FNotificationInfo Info(Text);
		Info.bFireAndForget = true;
		Info.bUseSuccessFailIcons = true;
		Info.ExpireDuration = bSuccess ? 6.0f : 10.0f;
		if (TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
		{
			Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
		}
	}
}

void DreamTextureAssignFlow::Run(const FString& MaterialObjectPath)
{
	UMaterialInterface* Material = LoadMaterial(MaterialObjectPath);
	if (Material == nullptr)
	{
		Toast(FText::Format(LOCTEXT("LoadFailed", "Could not find the material to assign: {0}. It may have been deleted or moved."), FText::FromString(MaterialObjectPath)), false);
		return;
	}
	Run(Material);
}

void DreamTextureAssignFlow::Run(UMaterialInterface* Material)
{
	// 没有 Slate（命令行 / 无头自动化）就没有对话框可弹，什么都不做——静默赋值正是我们要避免的。
	if (Material == nullptr || !FSlateApplication::IsInitialized())
	{
		return;
	}

	const FDreamTextureAssignSelection Selection = FDreamTextureAssignSelection::Query();
	const TSharedRef<FDreamTextureAssignModel> Model = MakeShared<FDreamTextureAssignModel>(Material, Selection);

	if (Selection.Source == EDreamTextureAssignSource::None || !Selection.HasUsableSlots())
	{
		FText Why;
		Model->CanApply(&Why);
		Toast(Why.IsEmpty() ? LOCTEXT("NothingSelected", "Nothing assignable is selected.") : Why, false);
		return;
	}

	const TSharedRef<bool> bConfirmed = MakeShared<bool>(false);
	const TSharedRef<TWeakPtr<SWindow>> WeakWindow = MakeShared<TWeakPtr<SWindow>>();

	auto Close = [WeakWindow]()
	{
		if (const TSharedPtr<SWindow> Window = WeakWindow->Pin())
		{
			Window->RequestDestroyWindow();
		}
	};

	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("DialogTitle", "Assign to Selection"))
		.SizingRule(ESizingRule::Autosized)
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		[
			SNew(SBox)
			.MinDesiredWidth(440.0f)
			.MaxDesiredWidth(560.0f)
			[
				SNew(SDreamTextureAssignDialog)
				.Model(Model)
				.OnConfirm(FSimpleDelegate::CreateLambda([bConfirmed, Close]() { *bConfirmed = true; Close(); }))
				.OnCancel(FSimpleDelegate::CreateLambda([Close]() { Close(); }))
			]
		];
	*WeakWindow = Window;

	// 模态：用户答完之前不让他再去改选中，否则「将替换 N 个」说的就不是最后改的那批了。
	FSlateApplication::Get().AddModalWindow(Window, FGlobalTabmanager::Get()->GetRootWindow());

	if (!*bConfirmed)
	{
		return;
	}

	const FDreamTextureAssignResult Result = Model->Apply();
	Toast(FDreamTextureAssignModel::DescribeResult(Result, Model->GetMaterialName()), Result.bOK);
}

#undef LOCTEXT_NAMESPACE
