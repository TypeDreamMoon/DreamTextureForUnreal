// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FDreamTextureProcess;

/**
 * 状态页与顶部状态条共用的后端动作：启动 / 停止 / 打开 Web 界面。
 *
 * 放成自由函数而不是各写一份：两处按钮的可用条件、提示和失败说法必须一致，
 * 否则状态条上能点的按钮在状态页上点不动，用户只会觉得是坏了。
 */
namespace DreamTextureBackendActions
{
	/** 客户端模块的进程管理器；模块未就绪时为空。 */
	TSharedPtr<FDreamTextureProcess> GetProcess();

	/** 「启动后端」此刻能不能点；不能点时 OutWhy 是原因（可直接当提示显示）。 */
	bool CanStart(FText* OutWhy = nullptr);

	/** 正在拉起后端。 */
	bool IsStarting();

	/** 后端是插件拉起来的（Origin == Launched）。 */
	bool IsLaunchedByPlugin();

	/** 确保后端在，结果用一条通知告诉用户。 */
	void Start();

	/** 停掉插件自己起的后端；附着的后端不动。 */
	void Stop();

	/** 用系统默认浏览器打开后端自带的 Web 界面。 */
	void OpenWebUI();

	/** 后端行的补充说明：谁起的、pid、退出时会不会被停。没有可说的返回空。 */
	FText DescribeOrigin();
}
