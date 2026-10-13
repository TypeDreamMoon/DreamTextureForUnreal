// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "DreamTextureSettings.generated.h"

/**
 * 项目设置 → 插件 → DreamTexture。写进工程的 Config/DefaultEditor.ini。
 *
 * 面板上看到的提示取自每个 UPROPERTY 的 meta=(ToolTip="…")，写的是**英文**，下面的中文注释只给读代码的人看——
 * UHT 在没有显式 ToolTip 时才会拿注释当提示，那样英文界面就会出现中文。中文提示在
 * Tools/Localization/translations/Settings.json，与 ToolTip 一一对应；改了英文要同步改它（sync_loctext.py 会报）。
 * 显示名没写 DisplayName 的由属性名推出（bCreateMaterialInstance → "Create Material Instance"），也在那个文件里翻译。
 */
UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "DreamTexture", ToolTip = "Project Settings > Plugins > DreamTexture. Saved to the project's Config/DefaultEditor.ini."))
class DREAMTEXTURECLIENT_API UDreamTextureSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UDreamTextureSettings();

	/** 后端地址。默认端口与 configs/dreamtexture.json 一致。 */
	UPROPERTY(EditAnywhere, config, Category = "Connection", meta = (ToolTip = "Backend address. The default port matches configs/dreamtexture.json in the backend."))
	FString BackendURL;

	/**
	 * dreamtexture.exe 的路径。
	 *
	 * M5 用它把后端拉起来。留空则只连不管——连不上时面板给提示，不自作主张启动。
	 */
	UPROPERTY(EditAnywhere, config, Category = "Connection", meta = (FilePathFilter = "exe", RelativeToGameDir, ToolTip = "Path to dreamtexture.exe. The plugin uses it to start the backend. If left empty, the plugin only connects and does not manage the backend: when it cannot connect, the panel shows a hint instead of starting one on its own."))
	FFilePath BackendExecutable;

	/**
	 * 传给 dreamtexture.exe 的启动参数，原样拼在命令行上，例如 -config "D:\DreamTexture\configs\my.json"。
	 *
	 * 留空则后端用自己的默认配置（程序旁边 configs\dreamtexture.json）。注意后端的 addr 要和上面的后端地址对得上，
	 * 否则进程起来了、地址上却没人应答，插件会等到超时才报错。
	 */
	UPROPERTY(EditAnywhere, config, Category = "Connection", meta = (ToolTip = "Launch arguments passed to dreamtexture.exe, appended to the command line as they are, for example -config followed by the path of a configuration file. If left empty, the backend uses its own default configuration (configs/dreamtexture.json next to the program). Note that the backend's addr must match the Backend URL above; otherwise the process starts but nothing answers at that address, and the plugin only reports an error after it times out."))
	FString BackendArguments;

	/**
	 * 编辑器启动时，若上面配了后端路径且地址上还没有后端在应答，就自动把它拉起来。
	 *
	 * 默认关：后端的 ComfyUI 会占显存，而编辑器本身也吃显存，用户没打算出图时不该替他开。
	 * 关着时后端也会在需要它的时候（点「启动后端」、生成页出图）按需拉起。
	 */
	UPROPERTY(EditAnywhere, config, Category = "Connection", meta = (ToolTip = "When the editor starts, if a backend executable is set above and nothing is answering at the backend address yet, start the backend automatically. Off by default: the backend's ComfyUI takes VRAM and the editor needs VRAM too, so it should not be started for someone who has no plans to generate images. When off, the backend is still started on demand, when it is needed (clicking Start Backend, or generating from the Generate page)."))
	bool bAutoStartBackend;

	/** 断线重连的起始退避（毫秒）。每次翻倍，上限 ReconnectMaxMs，连上后复位。 */
	UPROPERTY(EditAnywhere, config, AdvancedDisplay, Category = "Connection", meta = (DisplayName = "Reconnect Base (ms)", ClampMin = "200", UIMax = "5000", ToolTip = "Initial backoff before reconnecting after a disconnect, in milliseconds. It doubles on every attempt up to Reconnect Max (ms), and resets once connected."))
	int32 ReconnectBaseMs;

	/** 重连退避的上限（毫秒）。 */
	UPROPERTY(EditAnywhere, config, AdvancedDisplay, Category = "Connection", meta = (DisplayName = "Reconnect Max (ms)", ClampMin = "1000", ToolTip = "Upper limit of the reconnect backoff, in milliseconds."))
	int32 ReconnectMaxMs;

	/**
	 * 多久没收到任何消息就判定这条连接死了（毫秒）。
	 *
	 * 后端每 15 秒发一个 {"type":"hb"} 文本帧（internal/api/websocket.go 的
	 * HeartbeatInterval）。默认 40000 与 Web 端的 STALE_AFTER 是同一个值：
	 * 两拍多一点的余量，短暂的调度停顿不该掐掉一条好连接。
	 *
	 * 改之前先去看 web/src/store.ts 上那段注释，两边要一起改。
	 */
	UPROPERTY(EditAnywhere, config, AdvancedDisplay, Category = "Connection", meta = (DisplayName = "Stale After (ms)", ClampMin = "5000", ToolTip = "How long without receiving any message before the connection is considered dead, in milliseconds. The backend sends a heartbeat text frame every 15 seconds (HeartbeatInterval in internal/api/websocket.go). The default of 40000 is the same value as STALE_AFTER in the web UI: a little more than two beats of margin, so a short scheduling stall does not cut a good connection. Before changing it, read the comment in web/src/store.ts: both sides must be changed together."))
	int32 StaleAfterMs;

	/** 导入落地的内容根目录。 */
	UPROPERTY(EditAnywhere, config, Category = "Import", meta = (ToolTip = "Content root folder that imported assets are placed under."))
	FString ImportRoot;

	/**
	 * 连 ORM 之外的分通道图（roughness / metallic / ao）也一起导入。
	 *
	 * 默认关：ORM 已经覆盖了这三张，母材质也只采样 ORM。打开只是为了导给别的 DCC。
	 */
	UPROPERTY(EditAnywhere, config, Category = "Import", meta = (ToolTip = "Also import the separate channel textures (Roughness, Metallic, Ambient Occlusion) in addition to the packed texture (ORM). Off by default: the ORM texture already covers these three, and the master material only samples the ORM. Turn it on only if you want to hand the textures to another DCC tool."))
	bool bImportSeparateChannels;

	/**
	 * 导入后自动建（或更新）材质实例 MI_<名字>，父材质是插件生成在 ImportRoot 下的 M_DreamTexture_Master。
	 * 关掉就只落贴图。
	 */
	UPROPERTY(EditAnywhere, config, Category = "Import", meta = (ToolTip = "After import, automatically create (or update) the material instance MI_<name>, whose parent is the master material M_DreamTexture_Master that the plugin generates under the Import Root folder. When off, only the textures are imported."))
	bool bCreateMaterialInstance;

	/**
	 * 生成页提交的变体完成后自动导入工程（走和素材库「导入」按钮同一条路径）。
	 *
	 * 默认关：一次出几个变体是为了挑，全导进工程会留下一堆不要的资产。
	 * 只在**完成的那一刻**看这个开关，不追认：关着时完成的变体，事后打开开关不会被补导。
	 */
	UPROPERTY(EditAnywhere, config, Category = "Generate", meta = (ToolTip = "Automatically import the variants submitted from the Generate page into the project once they finish (the same path as the Import button in the Library). Off by default: you generate several variants to choose from, and importing all of them leaves a pile of assets you do not want. The setting is only checked at the moment a variant finishes and is not applied retroactively: variants that finished while it was off are not imported later if you turn it on."))
	bool bAutoImportGenerated;

	static const UDreamTextureSettings& Get();

	/** 去掉尾部斜杠的后端地址；为空或非法时回退到默认值。 */
	FString GetNormalizedBackendURL() const;

	/** 拼一个 REST 端点，Path 形如 "/api/health"。 */
	FString GetEndpoint(const FString& Path) const;

	/** 把 http(s):// 换成 ws(s):// 并接上 /api/ws。 */
	FString GetWebSocketURL() const;

	//~ UDeveloperSettings
	virtual FName GetCategoryName() const override;
};
