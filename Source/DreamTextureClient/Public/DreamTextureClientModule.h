// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FDreamTextureConnection;
class FDreamTextureJobTracker;
class FDreamTexturePushSession;
class FDreamTextureProcess;

/** 客户端模块。持有进程内唯一那条后端连接。 */
class DREAMTEXTURECLIENT_API FDreamTextureClientModule : public IModuleInterface
{
public:
	static FDreamTextureClientModule& Get();
	static bool IsAvailable();

	//~ IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** 模块未启动或已关闭时返回空指针，调用方必须判空。 */
	TSharedPtr<FDreamTextureConnection> GetConnection() const { return Connection; }

	/**
	 * 后端进程托管（「确保后端在」）。模块未启动或已关闭时返回空指针，调用方必须判空。
	 * 和连接是两件事：连接只管「连上没有」，进程管理器管「没有的话能不能把它拉起来」。
	 */
	TSharedPtr<FDreamTextureProcess> GetProcess() const { return Process; }

	/**
	 * 生成任务的提交与跟踪（M4 生成页、M8 回流共用）。进程内一个，订阅着上面那条连接的 job.* 事件。
	 * 放在模块上而不是页面上：一次生成动辄几分钟，用户常常提交完就把面板关了，
	 * 任务的进度、完成后的自动导入都不该跟着 tab 一起没。
	 * 模块未启动或已关闭时返回空指针，调用方必须判空。
	 */
	TSharedPtr<FDreamTextureJobTracker> GetJobTracker() const { return JobTracker; }

	/**
	 * 推送会话（M6）：连上后端就注册，让 Web 端的「发送到 UE」找得到这个编辑器，并收下它推来的材质。
	 * commandlet 里不绑连接（没有人在拨号），此时它存在但永远是未注册。
	 * 模块未启动或已关闭时返回空指针，调用方必须判空。
	 */
	TSharedPtr<FDreamTexturePushSession> GetPushSession() const { return PushSession; }

private:
	TSharedPtr<FDreamTextureConnection> Connection;
	TSharedPtr<FDreamTextureProcess> Process;
	TSharedPtr<FDreamTextureJobTracker> JobTracker;
	TSharedPtr<FDreamTexturePushSession> PushSession;
};
