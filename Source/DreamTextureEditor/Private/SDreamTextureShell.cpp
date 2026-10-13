// Copyright TypeDreamMoon. All Rights Reserved.

#include "SDreamTextureShell.h"

#include "SDreamTextureConnectionStrip.h"
#include "SDreamTextureGeneratePanel.h"
#include "SDreamTextureLibraryPanel.h"
#include "SDreamTextureReflowPanel.h"
#include "SDreamTextureStatusPanel.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWidget.h"
#include "Widgets/Layout/SWidgetSwitcher.h"

#define LOCTEXT_NAMESPACE "DreamTextureShell"

void SDreamTextureShell::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
			.Padding(FMargin(8.0f, 4.0f))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					// 点任一标记都去状态页：标记只是摘要，原因和修法在那边。
					SNew(SDreamTextureConnectionStrip)
					.OnClicked(FSimpleDelegate::CreateSP(this, &SDreamTextureShell::OnStripClicked))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SSegmentedControl<EPage>)
					.Value(this, &SDreamTextureShell::GetPage)
					.OnValueChanged(this, &SDreamTextureShell::SetPage)

					+ SSegmentedControl<EPage>::Slot(EPage::Library)
					.Text(LOCTEXT("PageLibrary", "Library"))
					.ToolTip(LOCTEXT("PageLibraryTip", "Browse the backend's library and import a material set into the project as textures with one click."))

					+ SSegmentedControl<EPage>::Slot(EPage::Generate)
					.Text(LOCTEXT("PageGenerate", "Generate"))
					.ToolTip(LOCTEXT("PageGenerateTip", "Stay in the editor: write a prompt, pick an image model and a decompose model, then generate a PBR material set and import it into the project."))

					+ SSegmentedControl<EPage>::Slot(EPage::Reflow)
					.Text(LOCTEXT("PageReflow", "Reflow"))
					.ToolTip(LOCTEXT("PageReflowTip", "Pick a material that is already in the project to see which textures it uses and which channel each one is, and export them as PNG."))

					+ SSegmentedControl<EPage>::Slot(EPage::Status)
					.Text(LOCTEXT("PageStatus", "Connection Status"))
					.ToolTip(LOCTEXT("PageStatusTip", "Detailed status of the backend, the event stream and ComfyUI, plus the Environment check."))
				]
			]
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SWidgetSwitcher)
			.WidgetIndex(this, &SDreamTextureShell::GetPageIndex)

			+ SWidgetSwitcher::Slot()
			[
				SNew(SDreamTextureLibraryPanel)
			]

			+ SWidgetSwitcher::Slot()
			[
				SNew(SDreamTextureStatusPanel)
			]

			// 槽位下标 = EPage 的枚举值，所以新页接在最后（见 EPage::Generate 的注释）。
			+ SWidgetSwitcher::Slot()
			[
				SNew(SDreamTextureGeneratePanel)
			]

			+ SWidgetSwitcher::Slot()
			[
				SNew(SDreamTextureReflowPanel)
			]
		]
	];
}

void SDreamTextureShell::OnStripClicked()
{
	Page = EPage::Status;
}

#undef LOCTEXT_NAMESPACE
