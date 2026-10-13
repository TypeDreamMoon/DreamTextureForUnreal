// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * 后端连接的状态。界面只认这四个值。
 *
 * 注意它说的只是 WebSocket 那条长连接。「后端进程在不在」是另一回事，
 * 看 FDreamTextureConnection::IsBackendReachable()；「ComfyUI 在不在」是
 * 第三回事，看 FDreamTextureHealth。这三级不要混成一个灯。
 */
enum class EDreamTextureLink : uint8
{
	/** 没连，也没在尝试：还没 Start()，或者被 Stop() 了。 */
	Offline,

	/** 正在连，或正在退避等下一次重试。 */
	Connecting,

	/** 连上了，心跳正常。 */
	Live,

	/**
	 * 连接还开着，但超过 StaleAfterMs 没收到任何消息。
	 *
	 * 单独一个状态是必要的：这时候界面上显示的一切都是过期的，
	 * 但它和「连不上」不一样——马上就会自动重连，不需要用户做任何事。
	 */
	Stale,
};

DREAMTEXTURECLIENT_API const TCHAR* LexToString(EDreamTextureLink Link);

/**
 * 对应后端 internal/comfy.Health 的 JSON。只取界面和判定用得上的字段。
 *
 * 后端保证这个结构在 /api/health 之外，也会通过 WS 的 comfy.status 事件推过来，
 * 且连上的第一条消息就是它——所以不必额外拉一次。
 */
struct DREAMTEXTURECLIENT_API FDreamTextureHealth
{
	/** managed | attach。attach 模式下后端不负责启动 ComfyUI。 */
	FString Mode;

	/** ComfyUI 进程能应答。 */
	bool bAlive = false;

	/** ComfyUI 能接活了。Alive 但不 Ready 是常态，别当故障报。 */
	bool bReady = false;

	/** 不就绪的原因，中文，可直接显示。 */
	FString Reason;

	FString ComfyUIVersion;
	FString Device;
	int64 VRAMTotalMB = 0;
	int64 VRAMFreeMB = 0;
	int32 QueueDepth = 0;
	int32 Restarts = 0;

	/**
	 * 进程拉起来了但还没能应答。
	 *
	 * 冷启动动辄几十秒到几分钟（自定义节点首次运行还要现装依赖）。这段时间
	 * 既不是「就绪」也不是「未连接」——混进未连接里会让人以为坏了去点重启，
	 * 而重启只会把这几十秒重来一遍。
	 */
	bool bStarting = false;
	int32 StartingSecs = 0;

	/** 区分「用户停的」和「它自己挂了」。别去比对 Reason 里的中文。 */
	bool bUserStopped = false;

	/**
	 * 从 comfy.status 的 data 或 /api/health 的响应体解析。
	 *
	 * 字段缺失或为 null 一律走默认值不报错：Go 的 nil 切片会序列化成 null
	 * 而不是 []，后端仓库里这个坑踩过两次。
	 */
	static bool FromJson(const TSharedPtr<FJsonObject>& Obj, FDreamTextureHealth& Out);
};

/** /api/checks 里的一项环境自检结果。 */
struct DREAMTEXTURECLIENT_API FDreamTextureCheck
{
	/** comfy | vram | nodes | models | output | imagen */
	FString Key;

	/** 中文标签，直接显示。 */
	FString Label;

	/** ok | warn | fail */
	FString Status;

	/** 中文说明，直接显示。 */
	FString Detail;

	/** 可执行的修复动作标识（restart-comfy / open-models …），空表示只能人工处理。 */
	FString Fix;

	/** 造成不通过的具体条目：缺失的节点类、模型名。 */
	TArray<FString> Items;

	static bool FromJson(const TSharedPtr<FJsonObject>& Obj, FDreamTextureCheck& Out);
};

/** /api/checks 的整个响应。 */
struct DREAMTEXTURECLIENT_API FDreamTextureChecksResult
{
	/** 所有项里最差的那个：ok | warn | fail */
	FString Status;
	TArray<FDreamTextureCheck> Checks;
};

/**
 * 一条从 /api/ws 收到的事件。
 *
 * 这条连接**只收不发**：所有操作走 REST，这样状态变更只有一条路径。
 * 不要因为「顺手」就往它上面写命令。
 */
struct DREAMTEXTURECLIENT_API FDreamTextureEvent
{
	/**
	 * hb | comfy.status | job.queued | job.progress | job.done | job.failed | model.download
	 *
	 * hb 是心跳，收到就只更新看门狗，不用往上抛。
	 */
	FString Type;

	/** 整条消息的 JSON。按 Type 自取 job / data 字段。M0 只用到 comfy.status。 */
	TSharedPtr<FJsonObject> Raw;
};
