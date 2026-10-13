// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureEditorModule.h"

#include "DreamTextureAutoImporter.h"
#include "DreamTextureClientModule.h"
#include "DreamTextureImportNotifier.h"
#include "DreamTextureLog.h"
#include "DreamTexturePushReceiver.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/Docking/TabManager.h"
#include "SDreamTextureShell.h"
#include "Styling/AppStyle.h"
#include "Textures/SlateIcon.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "DreamTextureEditor"

/**
 * tab id 一旦发出去就不要改：编辑器会把它写进用户的布局文件（EditorLayout.ini），
 * 改名等于让所有人的既有布局里凭空多出一个打不开的死 tab。
 */
const FName FDreamTextureEditorModule::PanelTabId(TEXT("DreamTexturePanel"));

FDreamTextureEditorModule& FDreamTextureEditorModule::Get()
{
	return FModuleManager::LoadModuleChecked<FDreamTextureEditorModule>(TEXT("DreamTextureEditor"));
}

bool FDreamTextureEditorModule::IsAvailable()
{
	return FModuleManager::Get().IsModuleLoaded(TEXT("DreamTextureEditor"));
}

void FDreamTextureEditorModule::StartupModule()
{
	// commandlet 或者没有 Slate 的进程里既没有 tab 也没有菜单，注册只会炸。
	// 客户端模块不受影响——它本来就不依赖 UI，自动化路径照走。
	if (IsRunningCommandlet() || !FSlateApplication::IsInitialized())
	{
		return;
	}

	ImportNotifier = MakeShared<FDreamTextureImportNotifier>();
	ImportNotifier->Start();

	// 追踪器归客户端模块持有；它没起来（理论上不会，Editor 依赖 Client）就只是没有自动导入。
	AutoImporter = MakeShared<FDreamTextureAutoImporter>();
	if (FDreamTextureClientModule::IsAvailable())
	{
		AutoImporter->Start(FDreamTextureClientModule::Get().GetJobTracker());
	}

	// 推送会话同样归客户端模块持有；它没起来就只是收不到 Web 端的推送。
	PushReceiver = MakeShared<FDreamTexturePushReceiver>();
	if (FDreamTextureClientModule::IsAvailable())
	{
		PushReceiver->Start(FDreamTextureClientModule::Get().GetPushSession());
	}

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(PanelTabId,
			FOnSpawnTab::CreateRaw(this, &FDreamTextureEditorModule::SpawnPanelTab))
		.SetDisplayName(LOCTEXT("PanelTabTitle", "DreamTexture"))
		.SetTooltipText(LOCTEXT("PanelTabTooltip", "Browse the DreamTexture library, import material sets with one click, and check the status of the backend, the event stream and ComfyUI."))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Server")));

	// UToolMenus 在模块启动这一刻不一定就位。RegisterStartupCallback 会在它就位
	// 之后回调；已经就位的话立刻执行。两种情况都不用调用方自己判断。
	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FDreamTextureEditorModule::RegisterMenus));
}

void FDreamTextureEditorModule::ShutdownModule()
{
	// 顺序有讲究：先摘掉还没跑的启动回调，再撤已经加进去的菜单项。
	// 反过来的话，卸载途中回调万一触发，会把菜单项又加回一个已经死掉的 this 上。
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);

	// 导入队列是 Assets 模块的单例，比我们晚关；摘掉订阅，别让它广播到已卸载的代码上。
	if (ImportNotifier.IsValid())
	{
		ImportNotifier->Stop();
		ImportNotifier.Reset();
	}

	if (AutoImporter.IsValid())
	{
		AutoImporter->Stop();
		AutoImporter.Reset();
	}

	if (PushReceiver.IsValid())
	{
		PushReceiver->Stop();
		PushReceiver.Reset();
	}

	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(PanelTabId);
	}
}

void FDreamTextureEditorModule::OpenPanel()
{
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}

	// 已经开着就聚焦过去，不会开出第二个。
	FGlobalTabmanager::Get()->TryInvokeTab(FTabId(PanelTabId));
}

TSharedRef<SDockTab> FDreamTextureEditorModule::SpawnPanelTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SDreamTextureShell)
		];
}

void FDreamTextureEditorModule::RegisterMenus()
{
	// Owner 记成 this，ShutdownModule 里一句 UnregisterOwner(this) 就能全撤干净，
	// 不用记住自己往哪几个菜单里塞过东西。
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
	if (!Menu)
	{
		// 关卡编辑器没起来（比如只开了资产编辑器窗口）。不是错误，面板照样能从
		// 「窗口 → 工具」里打开。
		UE_LOG(LogDreamTexture, Verbose, TEXT("Could not get LevelEditor.MainMenu.Tools; the menu entry was not added this time."));
		return;
	}

	FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("DreamTexture"), LOCTEXT("MenuSection", "DreamTexture"));
	Section.AddMenuEntry(
		TEXT("OpenDreamTexturePanel"),
		LOCTEXT("MenuOpenPanel", "DreamTexture Panel"),
		LOCTEXT("MenuOpenPanelTooltip", "Open the DreamTexture panel to browse the library, import material sets with one click, and check the status of the backend and ComfyUI."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Server")),
		FUIAction(FExecuteAction::CreateRaw(this, &FDreamTextureEditorModule::OpenPanel)));
}

IMPLEMENT_MODULE(FDreamTextureEditorModule, DreamTextureEditor)

#undef LOCTEXT_NAMESPACE
