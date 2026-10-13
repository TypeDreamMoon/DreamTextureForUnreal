// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FDreamTextureAutoImporter;
class FDreamTextureImportNotifier;
class FDreamTexturePushReceiver;
class FSpawnTabArgs;
class SDockTab;

/** 编辑器模块：面板、菜单项、资产落地。 */
class DREAMTEXTUREEDITOR_API FDreamTextureEditorModule : public IModuleInterface
{
public:
	/** 面板那个 nomad tab 的 id。 */
	static const FName PanelTabId;

	static FDreamTextureEditorModule& Get();
	static bool IsAvailable();

	//~ IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** 打开（或聚焦）DreamTexture 面板。 */
	void OpenPanel();

private:
	/** nomad tab 的内容工厂。每次 tab 被打开都会新建一个面板控件。 */
	TSharedRef<SDockTab> SpawnPanelTab(const FSpawnTabArgs& Args);

	/**
	 * 往「工具」菜单里挂入口。
	 *
	 * 不在 StartupModule 里直接调：那会儿 UToolMenus 不一定就位。
	 * 走 UToolMenus::RegisterStartupCallback，就位了它自己会回调回来。
	 */
	void RegisterMenus();

	/**
	 * 导入完成的通知。挂在模块上而不是面板上：用户点了「导入」就关面板，
	 * 导入还会在后台跑完，跑完要有提示。
	 */
	TSharedPtr<FDreamTextureImportNotifier> ImportNotifier;

	/**
	 * 生成完成后的自动导入。同样挂在模块上：一次生成动辄几分钟，用户常常提交完就把面板关了，
	 * 回来发现材质生成好了却没进工程，会以为没成功。
	 */
	TSharedPtr<FDreamTextureAutoImporter> AutoImporter;

	/**
	 * Web 端「发送到 UE」推来的材质：排导入、弹通知。同样挂在模块上，面板开不开都要收得到。
	 */
	TSharedPtr<FDreamTexturePushReceiver> PushReceiver;
};
