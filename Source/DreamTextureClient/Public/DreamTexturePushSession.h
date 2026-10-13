// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "DreamTextureTypes.h"

class FDreamTextureConnection;

/** 一次会话请求的结论，网络那一层交给会话管理器的就是这个。 */
struct DREAMTEXTURECLIENT_API FDreamTextureSessionReply
{
	/** 拿到了 HTTP 应答（不论状态码）。传输失败时为 false，StatusCode 是 0。 */
	bool bGotResponse = false;

	int32 StatusCode = 0;

	/** 应答体原文。 */
	FString Body;

	/** 传输失败或非 2xx 时的中文原因，可直接写日志 / 显示。 */
	FString Error;

	bool IsSuccess() const { return bGotResponse && StatusCode >= 200 && StatusCode < 300; }
};

/** 注册时报给后端的信息，对应 POST /api/ue/sessions 的请求体。 */
struct DREAMTEXTURECLIENT_API FDreamTextureSessionRegistration
{
	/** 给 Web 端下拉框看的，用工程名。 */
	FString Name;
	/** 工程文件的绝对路径。 */
	FString Project;
	/** 引擎版本，如 5.8.2。 */
	FString Engine;
	int32 PID = 0;

	/** 按当前进程填：工程名、工程路径、引擎版本、本进程 pid。 */
	static FDreamTextureSessionRegistration FromCurrentProcess();

	/** 序列化成请求体。 */
	FString ToJsonString() const;
};

enum class EDreamTexturePushState : uint8
{
	/** 没有会话：长连接没通、或注册没成功（原因见 GetLastError）、或刚失去会话在等下一轮。 */
	Unregistered,

	/** 注册请求（或重连后确认会话还在的那一次心跳）在途。 */
	Registering,

	/** 已有会话，每 15 秒续期。 */
	Registered,

	/** 后端不认这套接口（旧版本）。安静地停用，下一次重新连上时再试一次。 */
	Unsupported,
};

DREAMTEXTURECLIENT_API const TCHAR* LexToString(EDreamTexturePushState State);

/**
 * 推送会话（M6，插件这一侧）：让 Web 端看得见这个编辑器，并收下它推过来的材质。
 *
 * ## 为什么要有「会话」
 *
 * 连接那条 WS 只收不发，后端无从知道有没有编辑器在线、是哪一个。所以在线这件事靠 REST 报：
 * 连上（Live）就 POST /api/ue/sessions 注册，拿到 session_id；之后每 15 秒 POST .../heartbeat 续期，
 * 后端 45 秒没收到就当它下线。Web 端「发送到 UE」时后端发一条 `ue.import` 事件，全部 WS 订阅者都收得到，
 * 本类按自己的 session_id 过滤，只认发给自己的，转成 OnImportPushed 交给界面层去导入。
 *
 * ## 生命周期
 *
 * - **链路变 Live**：有上一次的 session_id 就先发一次心跳确认它还在（204 就接着用，404 才重新注册）；
 *   没有就注册。这样 WS 抖一下不会让 Web 端的下拉框里同时出现新旧两个会话 45 秒——
 *   那会让「不指定会话的发送」变成 409 歧义。
 * - **每 15 秒**：已注册则心跳；没注册（上一次失败了）则再试。心跳 404（后端重启过、或断得太久被清掉）
 *   立刻重新注册。心跳传输失败不丢会话，下一轮再发——真断了的话链路会先变 Stale。
 * - **链路离开 Live**：停心跳（不再发请求），但记着 session_id 留给下一次 Live 去确认。
 * - **Shutdown**（模块关闭）：尽力发 DELETE，不等应答，不会卡住编辑器退出。
 * - 注册返回 404 / 405 / 501，或者 200 但应答体里没有 session_id（旧后端对不认识的路径回落到前端的
 *   index.html，状态码是 200），都算后端不支持：置 Unsupported，不再请求、不刷错误日志。
 *
 * ## 第二版：心跳兜底投递
 *
 * 只靠 WS 事件会丢（长连接在重连退避里、或后端总线缓冲满了，事件没人收，会话却还没过期）。
 * 所以每次推送后端还记一笔 delivery，随心跳应答的 `pending` 再给一遍；插件处理完在下一次心跳的 `ack` 里确认。
 * WS 事件管及时，心跳管不丢。两条路来的投递**按 delivery_id 去重**，各处理一次（一次 OnImportPushed）；
 * 去重集合只记最近 MaxTrackedDeliveries 个，别无限长。
 *
 * 兼容旧后端：心跳回 204（第一版）照常算成功、没有 pending；事件没有 delivery_id 就按原来的方式处理一次，
 * 也无从确认。
 *
 * 注册成功后立刻补一次心跳：后端会把被替换的旧会话（比如丢了注册应答的那次）没确认的投递带给新会话，
 * 而事件带的是旧 session_id，新会话的 WS 会把它过滤掉——只有这次心跳的 pending 能把它补上。
 *
 * 一个 ticker 管心跳与补注册，从 Bind 活到 Unbind；它的回调里不移除自己（那是死锁，见 Docs/contributing.md 的 UE 5.8 pitfalls）。
 *
 * 所有委托都在游戏线程上广播。**WS 仍然只收不发。**
 */
class DREAMTEXTURECLIENT_API FDreamTexturePushSession : public TSharedFromThis<FDreamTexturePushSession>
{
public:
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnImportPushed, const FString& /*MaterialId*/);
	DECLARE_MULTICAST_DELEGATE(FOnStateChanged);

	using FOnReply = TFunction<void(const FDreamTextureSessionReply&)>;

	/**
	 * 网络与环境那一层，默认走真实 HTTP 与当前进程信息。
	 * 自动化测试换成假的，自己决定应答什么、什么时候应答。
	 *
	 * 要带 API 导出：构造函数的默认实参 FHooks::MakeDefault() 是在调用方的翻译单元里求值的，
	 * 别的模块（界面、测试）直接 MakeShared 时，没导出就是链接错误。
	 */
	struct DREAMTEXTURECLIENT_API FHooks
	{
		/**
		 * 发一个会话请求。Path 形如 "/api/ue/sessions"，基地址由实现方决定；
		 * Path 若已是 http:// 或 https:// 开头的完整地址就原样用。回调保证只调一次。
		 */
		TFunction<void(const FString& /*Verb*/, const FString& /*Path*/, const FString& /*JsonBody*/, float /*TimeoutSecs*/, FOnReply)> Send;

		TFunction<FDreamTextureSessionRegistration()> GetRegistration;

		/**
		 * 当前后端的基地址（如 http://127.0.0.1:8777）。每次注册时取一次缓存下来，注销只用缓存。
		 *
		 * 注销发生在模块关闭时，那时 UObject 系统可能已经拆了，再去取设置对象（CDO）就是野指针——
		 * GUI 编辑器带着已注册的会话退出时实测崩过（UDreamTextureSettings::Get 里访问冲突）。
		 * 为空（测试里的假钩子）时注销照旧走 Send 的相对路径。
		 */
		TFunction<FString()> GetBaseURL;

		/** 心跳与补注册的间隔（秒）。协议规定 15；测试可以缩短。 */
		float PulseIntervalSecs = 15.0f;

		static FHooks MakeDefault();
	};

	/** 协议规定的心跳间隔。后端 45 秒没收到就清掉会话，所以这里留够三次机会。 */
	static constexpr float DefaultPulseIntervalSecs = 15.0f;

	/** 记住最近处理过的多少个 delivery_id（去重）。后端每会话最多留 100 条未确认，这里留得比它宽。 */
	static constexpr int32 MaxTrackedDeliveries = 256;

	explicit FDreamTexturePushSession(FHooks InHooks = FHooks::MakeDefault());
	~FDreamTexturePushSession();

	/** 订阅这条连接。必须已被共享指针持有（AddSP）。重复调用先解旧的。连接此刻已是 Live 就立刻开始。 */
	void Bind(const TSharedPtr<FDreamTextureConnection>& Connection);
	void Unbind();

	/** 模块关闭：解绑并尽力注销。不等应答。 */
	void Shutdown();

	EDreamTexturePushState GetState() const { return State; }

	/** 当前会话 id；没有为空。 */
	const FString& GetSessionId() const { return SessionId; }

	/** 注册时报给后端的名字（工程名）；没注册过为空。 */
	const FString& GetSessionName() const { return SessionName; }

	/** 最近一次注册 / 续期失败的原因，成功后清空。Unsupported 时是一句说明。 */
	const FString& GetLastError() const { return LastError; }

	/** 处理过、还没被后端确认的投递数（下一次心跳会带上）。测试与诊断用。 */
	int32 GetNumPendingAcks() const { return PendingAcks.Num(); }

	/** 去重集合现在记着多少个 delivery_id。测试用。 */
	int32 GetNumTrackedDeliveries() const { return Handled.Num(); }

	//~ 以下是订阅的处理函数与定时动作，公开是为了自动化测试直接驱动
	void HandleLinkChanged(EDreamTextureLink NewLink);
	void HandleEvent(const FDreamTextureEvent& Event);

	/** 定时动作：已注册就心跳，没注册就再试一次。ticker 每个间隔调一次。 */
	void Pulse();

	/** 收到发给自己的 ue.import 时广播。别的会话的事件不会走到这里。 */
	FOnImportPushed OnImportPushed;

	/** 状态、会话 id 或最近错误变了就广播。 */
	FOnStateChanged OnStateChanged;

private:
	/** 有上一次的会话就先确认它，没有就注册。 */
	void Reconcile();
	void Register();
	void SendHeartbeat(bool bResuming);

	/**
	 * 处理一次投递（WS 事件或心跳 pending 带来的）。有 DeliveryId 的按它去重，处理过的记进待确认；
	 * 没有的（旧后端）每次都处理。返回是否真的处理了（广播了 OnImportPushed）。
	 */
	bool ProcessDelivery(const FString& DeliveryId, const FString& MaterialId, bool bFromHeartbeat);

	/** 解析心跳 200 应答里的 pending 并逐条处理。 */
	void ProcessPendingBody(const FString& Body);
	void SendUnregister(const FString& Id);

	void HandleRegisterReply(uint32 InEpoch, const FString& ReportedName, const FDreamTextureSessionReply& Reply);
	void HandleHeartbeatReply(uint32 InEpoch, bool bResuming, const TArray<FString>& SentAcks, const FDreamTextureSessionReply& Reply);

	/** 置成 Unsupported：停止一切请求。 */
	void MarkUnsupported(const FString& Why);

	/** 记一次失败。同一句原因只写一次日志，别每 15 秒刷一条。 */
	void NoteFailure(const FString& Why);

	/** 状态 / id / 错误真的变了才广播。 */
	void Publish();

	FHooks Hooks;
	TWeakPtr<FDreamTextureConnection> BoundConnection;
	FDelegateHandle EventHandle;
	FDelegateHandle LinkHandle;
	FTSTicker::FDelegateHandle PulseHandle;

	EDreamTexturePushState State = EDreamTexturePushState::Unregistered;
	FString SessionId;
	FString SessionName;
	FString LastError;

	/** 注册时的后端基地址。注销只用它，不在关闭途中碰设置对象（见 FHooks::GetBaseURL）。 */
	FString UnregisterBaseURL;

	/** 上一次 Publish 时的样子，用来判断有没有变。 */
	EDreamTexturePushState PublishedState = EDreamTexturePushState::Unregistered;
	FString PublishedId;
	FString PublishedError;

	/** 处理过的 delivery_id（去重），与记录先后的数组配合，超过上限丢最老的。 */
	TSet<FString> Handled;
	TArray<FString> HandledOrder;

	/** 处理过、还没被后端确认的 delivery_id，按处理先后。心跳请求成功之前一直留着。 */
	TArray<FString> PendingAcks;

	/** 链路此刻是不是 Live。 */
	bool bLive = false;

	/** 有心跳在途，不叠发。 */
	bool bHeartbeatInFlight = false;

	/** 最近一次写进日志的失败原因，同一句不重复写。 */
	FString LastLoggedFailure;

	/**
	 * 每次链路进出 Live、每次 Shutdown / Unbind 自增。在途请求的回调捕获发出时的值，对不上就是过期的：
	 * 它回来时我们已经换了一轮，不能再改状态。
	 */
	uint32 Epoch = 0;
};
