// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DreamTextureTypes.h"
#include "Templates/SharedPointer.h"

class IWebSocket;

/**
 * 与 DreamTexture 后端的长连接。
 *
 * 进程内一条，由 FDreamTextureClientModule 持有；界面通过委托订阅，不轮询。
 *
 * ## 判活的判据是「有没有消息进来」，不是 close 事件
 *
 * close 不保证会来：机器休眠再唤醒、半开连接都会让它丢掉。丢掉之后界面会一直
 * 显示「已连接」外加一份过期状态——Web 端实测把后端进程杀干净，页面照旧显示
 * ComfyUI 就绪、显存 14.8G。协议层的 ping/pong 也帮不上忙。所以这里跟 Web 端
 * 一样用看门狗盯消息，超过 StaleAfterMs 就判 Stale 并主动重连。
 *
 * ## 只收不发
 *
 * 所有操作走 REST，这条连接单向。这样状态变更只有一条路径，不会出现
 * 「有的操作走 WS 有的走 HTTP」导致的时序问题。
 *
 * 所有委托都在游戏线程上广播。
 */
class DREAMTEXTURECLIENT_API FDreamTextureConnection : public TSharedFromThis<FDreamTextureConnection>
{
public:
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnLinkChanged, EDreamTextureLink /*NewLink*/);
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnHealthChanged, const FDreamTextureHealth& /*Health*/);
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnEvent, const FDreamTextureEvent& /*Event*/);

	FDreamTextureConnection();
	~FDreamTextureConnection();

	/** 开始连接并保持自动重连。重复调用无副作用。 */
	void Start();

	/** 主动断开，不再重连。模块关闭时调。 */
	void Stop();

	/** 立刻重试一次，不等退避。设置里改了地址、或用户点了「重连」时调。 */
	void Reconnect();

	EDreamTextureLink GetLink() const { return Link; }

	/** 最近一次 comfy.status 带来的 ComfyUI 状态。没连上时是上一次的值。 */
	const FDreamTextureHealth& GetHealth() const { return Health; }

	/**
	 * 后端进程本身是否应答过 /api/health。
	 *
	 * 和 GetLink() 分开：ComfyUI 起不来的时候后端仍然能启动、能应答，
	 * 面板要如实分成两级显示，别把「ComfyUI 没就绪」说成「连不上」。
	 */
	bool IsBackendReachable() const { return bBackendReachable; }

	/** 最近一次失败的原因，可直接显示。连上后清空。 */
	const FString& GetLastError() const { return LastError; }

	/** 拉一次 /api/checks。回调保证在游戏线程上跑。 */
	void FetchChecks(TFunction<void(bool /*bOK*/, FDreamTextureChecksResult&& /*Result*/, const FString& /*Error*/)> Done);

	FOnLinkChanged OnLinkChanged;
	FOnHealthChanged OnHealthChanged;
	FOnEvent OnEvent;

protected:
	/** 状态字段对界面只读；私有实现细节由 DreamTextureConnection.cpp 拥有，可自行增补。 */
	EDreamTextureLink Link = EDreamTextureLink::Offline;
	FDreamTextureHealth Health;
	bool bBackendReachable = false;
	FString LastError;

private:
	/** 一次连接尝试：先 GET /api/health，通了才开 WS。理由见 .cpp。 */
	void BeginAttempt();

	/**
	 * health 探针的结论。
	 *
	 * 故意只收裸类型不收 FHttpResponsePtr：那样这个公共头就得包含 HTTP 模块的头，
	 * 而它现在只允许在 protected 以下动，也没必要让编辑器模块跟着吃这份依赖。
	 */
	void HandleHealthResult(uint64 InEpoch, bool bOK, const FString& FailureText);

	/** health 通了才走到这里。 */
	void OpenWebSocket();

	/** 只在真的变了的时候广播 OnLinkChanged。 */
	void SetLink(EDreamTextureLink NewLink);

	/** 解析并分发一条 WS 文本帧。 */
	void HandleMessage(const FString& Message);

	/** 收到任何消息就重挂一次看门狗。判活看的是「有没有消息」，不是 close 事件。 */
	void KickWatchdog();
	void ClearWatchdog();
	void ClearRetryTimer();

	/** 看门狗超时：置 Stale，主动关掉，再排重连。 */
	void HandleStale(uint64 InEpoch);

	/** 解绑委托 + Close + 延迟释放。顺序有讲究，理由见 .cpp。 */
	void CloseSocket();

	/** 收尾当前这条连接并按退避排下一次。settled 守卫在这里，理由见 .cpp。 */
	void SettleAndScheduleReconnect();

	/** 把项目设置里那几个数缓存下来，省得每个回调都去取一次 CDO。 */
	void ReloadSettings();

	/** Start() 过且还没被 Stop()。false 时任何路径都不再排重连。 */
	bool bRunning = false;

	TSharedPtr<IWebSocket> Socket;

	/** 当前这次尝试是否已经「了结」（已经排过重连）。见 .cpp 里那段坑的说明。 */
	bool bSettled = false;

	/**
	 * 每次 BeginAttempt() 自增。所有异步回调都把当时的值捕进去，回来对不上就丢弃。
	 *
	 * 它挡的是「上一次尝试迟到的东西」：还在飞的 health 响应、旧 socket 的关闭
	 * 事件、已经作废的定时器。bSettled 只管一条连接内部不重复排重连，跨尝试
	 * 那一层要靠这个计数。
	 */
	uint64 Epoch = 0;

	/** 下一次重连要等多久（毫秒）。连上后复位成 ReconnectBaseMs。 */
	int32 BackoffMs = 1000;

	//~ 下面三个是 UDreamTextureSettings 的副本，Start() / Reconnect() 时刷新。
	int32 ReconnectBaseMs = 1000;
	int32 ReconnectMaxMs = 15000;
	int32 StaleAfterMs = 40000;

	/**
	 * 定时器句柄放进一个只在 .cpp 里定义的结构体。
	 *
	 * 单纯是为了不把 Containers/Ticker.h 拖进这个公共头——编辑器模块里每个
	 * 包含它的翻译单元都得跟着吃一遍。
	 */
	struct FTimers;
	TUniquePtr<FTimers> Timers;
};
