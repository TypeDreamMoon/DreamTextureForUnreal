// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureConnection.h"

#include "DreamTextureLog.h"
#include "DreamTextureSettings.h"

#include "Containers/Ticker.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HttpModule.h"
#include "IWebSocket.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "WebSocketsModule.h"

/*
 * 线程假设：这一整个文件都跑在游戏线程上。
 *
 * FHttpModule 的完成委托默认是 EHttpRequestDelegateThreadPolicy::CompleteOnGameThread
 * （见 HttpRequestCommon.h 里 DelegateThreadPolicy 的初值），我们没有改过它；
 * IWebSocket 的几个事件由各平台的 WebSockets 管理器在游戏线程上派发；
 * FTSTicker::GetCoreTicker() 也在游戏线程上跑。
 *
 * 所以这里的成员变量不加锁，OnLinkChanged / OnHealthChanged / OnEvent 也就
 * 天然满足了「所有委托都在游戏线程上广播」这条约定。谁要是给某个请求改成
 * CompleteOnHttpThread，这套假设就塌了，记得连着改。
 */

namespace
{
	/**
	 * health 探针的超时（秒）。
	 *
	 * 后端在本机，正常是毫秒级。给 5 秒是留给「后端刚起来还在初始化」这种情况，
	 * 再长就没意义了——反正连不上会自动重试，让界面早点显示出真相更重要。
	 */
	constexpr float HealthTimeoutSecs = 5.0f;

	/**
	 * /api/checks 的超时（秒）。
	 *
	 * 它比 health 慢一个量级：要去问 ComfyUI 的节点清单、扫模型目录、看显存。
	 * 15 秒是给冷启动留的余量。
	 */
	constexpr float ChecksTimeoutSecs = 15.0f;

	/** 把 HTTP 的失败翻成人话。这些字串会原样出现在面板上，别写英文缩写。 */
	FString DescribeHttpFailure(const FHttpRequestPtr& Request, const FHttpResponsePtr& Response)
	{
		// GetFailureReason() 比只看 bSucceeded 能说清楚得多：
		// 「连不上」和「连上了但超时」对用户来说是两件要做不同处理的事。
		const EHttpFailureReason Reason = Request.IsValid() ? Request->GetFailureReason() : EHttpFailureReason::Other;
		switch (Reason)
		{
		case EHttpFailureReason::ConnectionError:
			return NSLOCTEXT("DreamTextureConnection", "ConnectionError", "cannot connect; the backend process may not be running").ToString();
		case EHttpFailureReason::TimedOut:
			return NSLOCTEXT("DreamTextureConnection", "TimedOut", "no response before the timeout").ToString();
		case EHttpFailureReason::Cancelled:
			return NSLOCTEXT("DreamTextureConnection", "Cancelled", "the request was cancelled").ToString();
		case EHttpFailureReason::ResponseTooLarge:
			return NSLOCTEXT("DreamTextureConnection", "ResponseTooLarge", "the response is too large").ToString();
		default:
			break;
		}

		if (Response.IsValid())
		{
			return FText::Format(NSLOCTEXT("DreamTextureConnection", "ReturnedStatus", "returned HTTP {0}"), FText::AsNumber(Response->GetResponseCode())).ToString();
		}
		return NSLOCTEXT("DreamTextureConnection", "RequestFailed", "the request failed").ToString();
	}

	/** ok < warn < fail，用来在后端没给 status 时自己推一个总状态出来。 */
	int32 CheckStatusSeverity(const FString& Status)
	{
		if (Status.Equals(TEXT("fail"), ESearchCase::IgnoreCase))
		{
			return 2;
		}
		if (Status.Equals(TEXT("warn"), ESearchCase::IgnoreCase))
		{
			return 1;
		}
		return 0;
	}

	/**
	 * 解 /api/checks 的整个响应。
	 *
	 * 放在这里而不是 FDreamTextureChecksResult::FromJson，是因为那个结构体的头
	 * （DreamTextureTypes.h）已经冻结，没给它留静态方法。
	 */
	bool ParseChecksResponse(const FString& Body, FDreamTextureChecksResult& Out)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return false;
		}

		Root->TryGetStringField(TEXT("status"), Out.Status);

		// checks 可能是 null——Go 的 nil 切片就序列化成 null 而不是 []。
		// 这不是错误，当空数组处理。
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (Root->TryGetArrayField(TEXT("checks"), Items) && Items != nullptr)
		{
			Out.Checks.Reserve(Items->Num());
			for (const TSharedPtr<FJsonValue>& Value : *Items)
			{
				const TSharedPtr<FJsonObject>* ItemObject = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(ItemObject) || ItemObject == nullptr)
				{
					continue;
				}

				FDreamTextureCheck Check;
				if (FDreamTextureCheck::FromJson(*ItemObject, Check))
				{
					Out.Checks.Add(MoveTemp(Check));
				}
			}
		}

		// status 缺了就从各项里推最差的那个。界面上那盏总灯不能是空的，
		// 空白比「有问题」更让人不知所措。
		if (Out.Status.IsEmpty())
		{
			int32 Worst = 0;
			for (const FDreamTextureCheck& Check : Out.Checks)
			{
				Worst = FMath::Max(Worst, CheckStatusSeverity(Check.Status));
			}
			Out.Status = (Worst == 2) ? TEXT("fail") : (Worst == 1 ? TEXT("warn") : TEXT("ok"));
		}

		return true;
	}
}

/**
 * 定时器句柄。只在这个 .cpp 里可见，避免把 Containers/Ticker.h 塞进公共头。
 */
struct FDreamTextureConnection::FTimers
{
	/** 退避重连用的一次性定时器。 */
	FTSTicker::FDelegateHandle Retry;

	/** 失联看门狗。每收到一条消息就重挂一次。 */
	FTSTicker::FDelegateHandle Watchdog;
};

FDreamTextureConnection::FDreamTextureConnection()
	: Timers(MakeUnique<FTimers>())
{
	// 这里刻意不去读项目设置：构造点可能早于 CDO 就绪，而且用户随时会改地址，
	// 读了也白读。设置统一在 Start() / Reconnect() 里刷新。
}

FDreamTextureConnection::~FDreamTextureConnection()
{
	// 走到这里说明持有者没先 Stop()。不能直接复用 Stop()：它会广播 OnLinkChanged，
	// 而订阅方手里多半正拿着一个指向「我」的指针——在析构过程里回调回去是自找麻烦。
	// 下面这几步都不碰 AsShared()/AsWeak()（析构里调那两个是非法的）。
	bRunning = false;
	++Epoch;
	ClearRetryTimer();
	ClearWatchdog();
	CloseSocket();
}

void FDreamTextureConnection::Start()
{
	if (bRunning)
	{
		// 「重复调用无副作用」：已经在跑就别打断当前的尝试，也别把退避复位——
		// 面板每次打开都调一次 Start() 的话，那样会变成绕过退避的重试风暴。
		return;
	}

	bRunning = true;
	ReloadSettings();
	BackoffMs = ReconnectBaseMs;
	BeginAttempt();
}

void FDreamTextureConnection::Stop()
{
	// bRunning 先落地：后面每条路径都靠它短路，保证 Stop() 之后一次都不再重连。
	bRunning = false;

	// 让所有还在飞的回调作废。它们对不上 Epoch 就会自己走掉。
	++Epoch;

	ClearRetryTimer();
	ClearWatchdog();
	CloseSocket();

	bBackendReachable = false;
	LastError.Reset();
	SetLink(EDreamTextureLink::Offline);
}

void FDreamTextureConnection::Reconnect()
{
	// 立刻重试，不等退避。用户点「重连」或者刚改完地址时用。
	bRunning = true;

	// 地址、超时、退避都可能刚被改过，重新取一遍。
	ReloadSettings();

	// 复位退避 + 取消待定的那次定时重连，然后马上试。少了任何一步，
	// 「立刻」都会变成「等最多 15 秒」。
	BackoffMs = ReconnectBaseMs;
	ClearRetryTimer();

	BeginAttempt();
}

void FDreamTextureConnection::ReloadSettings()
{
	const UDreamTextureSettings& Settings = UDreamTextureSettings::Get();

	// UPROPERTY 上的 ClampMin 只管得住编辑器里的输入框，ini 里手写的值不受约束。
	// 退避是 0 的话这里会退化成一个疯狂重连的死循环，必须自己再夹一次。
	ReconnectBaseMs = FMath::Max(200, Settings.ReconnectBaseMs);
	ReconnectMaxMs = FMath::Max(ReconnectBaseMs, Settings.ReconnectMaxMs);
	StaleAfterMs = FMath::Max(5000, Settings.StaleAfterMs);
}

void FDreamTextureConnection::BeginAttempt()
{
	if (!bRunning)
	{
		return;
	}

	++Epoch;
	bSettled = false;

	ClearWatchdog();
	CloseSocket();

	SetLink(EDreamTextureLink::Connecting);

	const FString HealthURL = UDreamTextureSettings::Get().GetEndpoint(TEXT("/api/health"));

	/*
	 * 一次连接尝试 = 先 GET /api/health，通了才开 WS。
	 *
	 * 多这一步不是保守，是 IsBackendReachable() 有意义的前提：
	 * 「后端在、ComfyUI 不在」是完全正常的状态（attach 模式下更是常态），
	 * 直接开 WS 的话这两种情况会糊成同一个失败，界面只能显示「连不上」，
	 * 然后用户就去重启错的那个东西了。
	 *
	 * 顺带还省掉了一件事：后端没起来的时候不用反复去建 WebSocket，
	 * 那玩意每次失败都会在 LogWebSockets 里刷一串告警。
	 */
	const uint64 MyEpoch = Epoch;
	TWeakPtr<FDreamTextureConnection> WeakThis = AsWeak();

	const FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(HealthURL);
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(HealthTimeoutSecs);
	Request->OnProcessRequestComplete().BindLambda(
		[WeakThis, MyEpoch, HealthURL](FHttpRequestPtr InRequest, FHttpResponsePtr InResponse, bool bSucceeded)
		{
			// 不裸捕 this：这个请求飞在半空的时候连接可能已经被销毁了
			//（模块卸载、用户关工程）。Pin 不上就当没发生过。
			const TSharedPtr<FDreamTextureConnection> Self = WeakThis.Pin();
			if (!Self.IsValid())
			{
				return;
			}

			if (!bSucceeded || !InResponse.IsValid())
			{
				Self->HandleHealthResult(MyEpoch, false,
					FText::Format(NSLOCTEXT("DreamTextureConnection", "HealthUnreachable", "Cannot reach the backend at {0}: {1}"), FText::FromString(HealthURL), FText::FromString(DescribeHttpFailure(InRequest, InResponse))).ToString());
				return;
			}

			const int32 ResponseCode = InResponse->GetResponseCode();
			if (!EHttpResponseCodes::IsOk(ResponseCode))
			{
				Self->HandleHealthResult(MyEpoch, false,
					FText::Format(NSLOCTEXT("DreamTextureConnection", "HealthBadStatus", "The backend at {0} returned HTTP {1}."), FText::FromString(HealthURL), FText::AsNumber(ResponseCode)).ToString());
				return;
			}

			// 只看「有没有应答」，不看 body 里的 ok 字段。这条探针的职责就一件事：
			// 后端进程在不在。ComfyUI 的死活是另一级，走 WS 的 comfy.status。
			Self->HandleHealthResult(MyEpoch, true, FString());
		});

	if (!Request->ProcessRequest())
	{
		// 同步就发不出去（地址非法之类）。委托不会再回来了，得在这里自己收尾，
		// 否则这条尝试会永远挂着，看门狗也不会启动（它是开 WS 之后才挂的）。
		HandleHealthResult(MyEpoch, false, FText::Format(NSLOCTEXT("DreamTextureConnection", "CannotSend", "The request could not be sent; check that the address is correct: {0}"), FText::FromString(HealthURL)).ToString());
	}
}

void FDreamTextureConnection::HandleHealthResult(uint64 InEpoch, bool bOK, const FString& FailureText)
{
	if (InEpoch != Epoch || !bRunning)
	{
		// 上一次尝试迟到的响应。它描述的是一条已经被放弃的连接，照它改状态
		// 只会把刚建好的这条搅乱。
		return;
	}

	if (!bOK)
	{
		bBackendReachable = false;
		LastError = FailureText;
		UE_LOG(LogDreamTexture, Verbose, TEXT("%s"), *LastError);

		// 仍然是 Connecting 不是 Offline：Offline 的含义是「没在尝试」，
		// 而我们马上就会重试，用户什么都不用做。
		SetLink(EDreamTextureLink::Connecting);
		SettleAndScheduleReconnect();
		return;
	}

	bBackendReachable = true;
	OpenWebSocket();
}

void FDreamTextureConnection::OpenWebSocket()
{
	// URL 在 #if 里面声明：WITH_WEBSOCKETS=0 的构建下它会变成没人用的局部变量，
	// 那是个会被当成错误的告警。
#if WITH_WEBSOCKETS
	const FString URL = UDreamTextureSettings::Get().GetWebSocketURL();
	UE_LOG(LogDreamTexture, Verbose, TEXT("Connecting to the event stream at %s"), *URL);

	// 第二个参数是子协议，后端 /api/ws 不挑，给空的。
	Socket = FWebSocketsModule::Get().CreateWebSocket(URL, FString());
#endif

	if (!Socket.IsValid())
	{
		// 只有在 WITH_WEBSOCKETS=0 的构建里才会走到这儿（Win64 编辑器不会）。
		// 不重连也没用，但保持退避重试比直接躺平好排查。
		LastError = NSLOCTEXT("DreamTextureConnection", "NoWebSockets", "This build has no WebSocket support, so backend events cannot be received.").ToString();
		SetLink(EDreamTextureLink::Connecting);
		SettleAndScheduleReconnect();
		return;
	}

	const uint64 MyEpoch = Epoch;
	TWeakPtr<FDreamTextureConnection> WeakThis = AsWeak();

	Socket->OnConnected().AddLambda(
		[WeakThis, MyEpoch]()
		{
			const TSharedPtr<FDreamTextureConnection> Self = WeakThis.Pin();
			if (!Self.IsValid() || Self->Epoch != MyEpoch)
			{
				return;
			}

			Self->LastError.Reset();
			Self->BackoffMs = Self->ReconnectBaseMs; // 连上了就复位退避
			Self->SetLink(EDreamTextureLink::Live);

			// 握手完成也算一次「有动静」，看门狗从这里开始计时。
			Self->KickWatchdog();

			UE_LOG(LogDreamTexture, Log, TEXT("Event stream connected"));
		});

	Socket->OnConnectionError().AddLambda(
		[WeakThis, MyEpoch](const FString& Error)
		{
			const TSharedPtr<FDreamTextureConnection> Self = WeakThis.Pin();
			if (!Self.IsValid() || Self->Epoch != MyEpoch)
			{
				return;
			}

			Self->LastError = FText::Format(NSLOCTEXT("DreamTextureConnection", "StreamConnectFailed", "The event stream could not connect: {0}"), FText::FromString(Error)).ToString();
			UE_LOG(LogDreamTexture, Verbose, TEXT("%s"), *Self->LastError);
			Self->SetLink(EDreamTextureLink::Connecting);
			Self->SettleAndScheduleReconnect();
		});

	Socket->OnClosed().AddLambda(
		[WeakThis, MyEpoch](int32 StatusCode, const FString& Reason, bool bWasClean)
		{
			const TSharedPtr<FDreamTextureConnection> Self = WeakThis.Pin();
			if (!Self.IsValid() || Self->Epoch != MyEpoch)
			{
				// 旧 socket 迟到的关闭事件。丢掉——它要是能排上一次重连，
				// 就会跟已经建好的新连接叠在一起，越滚越多。
				return;
			}

			Self->LastError = bWasClean
				? FText::Format(NSLOCTEXT("DreamTextureConnection", "StreamClosedByBackend", "The backend closed the event stream ({0} {1}); reconnecting."), FText::AsNumber(StatusCode), FText::FromString(Reason)).ToString()
				: FText::Format(NSLOCTEXT("DreamTextureConnection", "StreamDropped", "The event stream dropped unexpectedly ({0} {1}); reconnecting."), FText::AsNumber(StatusCode), FText::FromString(Reason)).ToString();
			UE_LOG(LogDreamTexture, Verbose, TEXT("%s"), *Self->LastError);
			Self->SetLink(EDreamTextureLink::Connecting);
			Self->SettleAndScheduleReconnect();
		});

	Socket->OnMessage().AddLambda(
		[WeakThis, MyEpoch](const FString& Message)
		{
			const TSharedPtr<FDreamTextureConnection> Self = WeakThis.Pin();
			if (!Self.IsValid() || Self->Epoch != MyEpoch)
			{
				return;
			}

			// 先喂看门狗再解析：一条解不动的消息也照样证明对端还活着。
			Self->KickWatchdog();
			Self->HandleMessage(Message);
		});

	// 看门狗在 Connect() 之前就挂上，两个理由：
	// 一是握手阶段也要有兜底——Connect() 卡在那里不回的时候（半开连接、对端
	// accept 了却不升级协议）OnConnectionError 不一定会来；
	// 二是万一哪个平台的实现会同步回调错误，收尾逻辑能顺手把它清掉，
	// 不会留下一个 40 秒后凭空把状态打成 Stale 的孤儿定时器。
	KickWatchdog();

	// 先取一份本地强引用再 Connect()。同理：同步回调进来的话 CloseSocket()
	// 会把 Socket 成员清空，那时候再用成员就是空指针解引用了。
	const TSharedPtr<IWebSocket> LocalSocket = Socket;
	LocalSocket->Connect();
}

void FDreamTextureConnection::HandleMessage(const FString& Message)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		// 解不动就丢掉，不断连。这条连接只收不发，没有需要回话的握手，
		// 一条坏消息没有理由掀掉整条流。
		UE_LOG(LogDreamTexture, Verbose, TEXT("Dropped a WebSocket message that could not be parsed: %s"), *Message.Left(200));
		return;
	}

	FDreamTextureEvent Event;
	Root->TryGetStringField(TEXT("type"), Event.Type);
	Event.Raw = Root;

	// hb 是心跳，上面已经喂过看门狗了，不用往上抛——每 15 秒吵订阅方一次没意义。
	if (Event.Type.Equals(TEXT("hb")))
	{
		return;
	}

	if (Event.Type.Equals(TEXT("comfy.status")))
	{
		// 连上后的第一条消息必定是它，所以不用额外再拉一次 /api/health 拿状态。
		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (Root->TryGetObjectField(TEXT("data"), Data) && Data != nullptr)
		{
			FDreamTextureHealth NewHealth;
			if (FDreamTextureHealth::FromJson(*Data, NewHealth))
			{
				Health = MoveTemp(NewHealth);
				OnHealthChanged.Broadcast(Health);
			}
		}
		// data 缺了就只当条普通事件抛出去。后端不该这么发，但不值得为它断连。
	}

	OnEvent.Broadcast(Event);
}

void FDreamTextureConnection::SetLink(EDreamTextureLink NewLink)
{
	if (Link == NewLink)
	{
		// 变了才广播。每次 tick 都发的话，订阅方就得自己去重，
		// 而界面上的重绘是有代价的。
		return;
	}

	Link = NewLink;
	UE_LOG(LogDreamTexture, Verbose, TEXT("Link state -> %s"), LexToString(Link));
	OnLinkChanged.Broadcast(Link);
}

void FDreamTextureConnection::KickWatchdog()
{
	ClearWatchdog();

	if (!bRunning)
	{
		return;
	}

	const float DelaySecs = FMath::Max(1.0f, static_cast<float>(StaleAfterMs) / 1000.0f);
	const uint64 MyEpoch = Epoch;
	TWeakPtr<FDreamTextureConnection> WeakThis = AsWeak();

	Timers->Watchdog = FTSTicker::GetCoreTicker().AddTicker(
		TEXT("DreamTexture.Watchdog"), DelaySecs,
		[WeakThis, MyEpoch](float) -> bool
		{
			if (const TSharedPtr<FDreamTextureConnection> Self = WeakThis.Pin())
			{
				// 先把句柄清掉再往下走，这一步不是洁癖：FTSTicker::RemoveTicker
				// 「在委托执行途中被调用时会阻塞到执行结束」，同一个线程上自己
				// 移除自己就是死锁。下面 HandleStale 会一路调到 ClearWatchdog()，
				// 句柄空了它才会跳过 RemoveTicker。
				Self->Timers->Watchdog.Reset();
				Self->HandleStale(MyEpoch);
			}
			return false; // 一次性
		});
}

void FDreamTextureConnection::ClearWatchdog()
{
	if (Timers->Watchdog.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(Timers->Watchdog);
		Timers->Watchdog.Reset();
	}
}

void FDreamTextureConnection::ClearRetryTimer()
{
	if (Timers->Retry.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(Timers->Retry);
		Timers->Retry.Reset();
	}
}

void FDreamTextureConnection::HandleStale(uint64 InEpoch)
{
	if (InEpoch != Epoch || !bRunning)
	{
		return;
	}

	/*
	 * 超过 StaleAfterMs 没收到任何东西，就当这条连接死了。
	 *
	 * 判据是「有没有消息进来」而不是 close 事件：close 不保证会来，机器休眠
	 * 再唤醒、半开连接都会把它弄丢。Web 端实测把后端进程杀干净，页面照旧显示
	 * ComfyUI 就绪、显存 14.8G——因为 close 从来没到过。
	 *
	 * Stale 是独立的一档，不是 Connecting：界面上那份数据已经过期了要说清楚，
	 * 但它跟「连不上」不一样，用户什么都不用做。
	 */
	LastError = FText::Format(NSLOCTEXT("DreamTextureConnection", "StreamStale", "No message from the backend for {0} seconds; reconnecting."),
		FText::AsNumber(FMath::RoundToInt(static_cast<float>(StaleAfterMs) / 1000.0f))).ToString();
	UE_LOG(LogDreamTexture, Warning, TEXT("%s"), *LastError);

	SetLink(EDreamTextureLink::Stale);

	// SettleAndScheduleReconnect() 里会主动 Close()。Stale 会一直显示到
	// 下一次尝试真正开始（BeginAttempt 把它改成 Connecting）。
	SettleAndScheduleReconnect();
}

void FDreamTextureConnection::SettleAndScheduleReconnect()
{
	/*
	 * settled 守卫：一条连接只能安排一次重连。
	 *
	 * 没有它的话会这样：看门狗把一条僵死的连接 Close 掉、排了重连、新连接也
	 * 已经建好，此时旧连接迟到的 OnClosed 才到，于是又排一次——同时挂两条，
	 * 之后每轮翻倍，越滚越多。Web 端 store.ts 的注释里专门记了这个坑。
	 *
	 * 跨尝试那一层由 Epoch 挡（旧 socket 的回调根本走不到这里），
	 * 这个标志管的是同一次尝试里 OnConnectionError 和 OnClosed 双双触发。
	 */
	if (bSettled)
	{
		return;
	}
	bSettled = true;

	ClearWatchdog();
	CloseSocket();

	if (!bRunning)
	{
		return;
	}

	const int32 DelayMs = FMath::Clamp(BackoffMs, ReconnectBaseMs, ReconnectMaxMs);

	// 退避翻倍，上限 ReconnectMaxMs。复位在 OnConnected 里做。
	BackoffMs = FMath::Min(DelayMs * 2, ReconnectMaxMs);

	ClearRetryTimer();

	TWeakPtr<FDreamTextureConnection> WeakThis = AsWeak();
	Timers->Retry = FTSTicker::GetCoreTicker().AddTicker(
		TEXT("DreamTexture.Reconnect"), static_cast<float>(DelayMs) / 1000.0f,
		[WeakThis](float) -> bool
		{
			if (const TSharedPtr<FDreamTextureConnection> Self = WeakThis.Pin())
			{
				// 同上：先清句柄，免得后面某条路径走到 ClearRetryTimer()，
				// 在自己的委托里 RemoveTicker 自己而死锁。
				Self->Timers->Retry.Reset();
				Self->BeginAttempt();
			}
			return false; // 一次性
		});
}

void FDreamTextureConnection::CloseSocket()
{
	if (!Socket.IsValid())
	{
		return;
	}

	// 先把成员置空，后面拿的是本地这份强引用。
	TSharedPtr<IWebSocket> Old = MoveTemp(Socket);
	Socket.Reset();

	// 解绑在 Close() 之前：否则关闭过程里回调进来，会在一条我们已经放弃的
	// 连接上再走一遍状态机。
	Old->OnConnected().Clear();
	Old->OnConnectionError().Clear();
	Old->OnClosed().Clear();
	Old->OnMessage().Clear();

	// WebSockets 模块已经卸载（只会发生在编辑器退出途中）：它卸载时已经把所有连接收掉了。
	// 这时再 Close()，引擎会去取 FWebSocketsModule，把已卸载的模块重新加载，断言崩溃。
	// 正常情况下本模块启动时先加载了 WebSockets，它比我们晚卸载，走不到这里；这是兜底。
	// 最后这份引用也不放：析构函数要求连接已经关掉，而管理器已经不在了，没人能保证这一点。
	// 进程马上就退出，故意漏掉这一个对象比在退出途中赌一次析构稳妥。
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("WebSockets")))
	{
		new TSharedPtr<IWebSocket>(MoveTemp(Old));
		return;
	}

	// 不判 IsConnected()：握手还没完成的时候它是 false，而卡在握手上的连接
	// 恰恰是最需要主动掐掉的那种。各平台的实现对重复 Close 都是幂等的。
	Old->Close();

	// 编辑器退出途中不会再有下一帧：挂到 ticker 上的那份引用要等到静态析构才放，
	// 那时 WebSockets 模块早关了。这里不在 socket 自己的广播里（是模块关闭调进来的），当场放掉；
	// 管理器自己还持有一份，关闭流程照常在它那边走完。
	if (IsEngineExitRequested())
	{
		return;
	}

	/*
	 * 最后一份强引用挂到下一帧再放。
	 *
	 * CloseSocket() 有可能是从这个 socket 自己的 OnClosed 广播里调进来的，
	 * 当场让它析构等于在广播过程中销毁广播者。现在的实现里管理器还另外持有
	 * 一份引用，未必真的会当场释放——但这种「靠别人多拿了一份引用才不崩」的
	 * 依赖不该留在代码里。
	 */
	FTSTicker::GetCoreTicker().AddTicker(
		TEXT("DreamTexture.ReleaseSocket"), 0.0f,
		[Old](float) mutable -> bool
		{
			Old.Reset();
			return false;
		});
}

void FDreamTextureConnection::FetchChecks(
	TFunction<void(bool /*bOK*/, FDreamTextureChecksResult&& /*Result*/, const FString& /*Error*/)> Done)
{
	const FString URL = UDreamTextureSettings::Get().GetEndpoint(TEXT("/api/checks"));
	TWeakPtr<FDreamTextureConnection> WeakThis = AsWeak();

	const FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(URL);
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(ChecksTimeoutSecs);

	// 「已经回调过」的标志，委托和下面的同步失败分支共用。
	// 5.8 的 FHttpRequestCommon::PreProcess() 在请求发不出去时，会先当场触发完成委托、
	// 再让 ProcessRequest() 返回 false——只靠返回值补一次回调，调用方就会收到两次。
	const TSharedRef<bool> bDelivered = MakeShared<bool>(false);

	// Done 按值传进来，这里拷一份给委托、原件留给下面的同步失败分支。
	// TFunction 是可拷贝的（TUniqueFunction 才是只能移动的那个）。
	Request->OnProcessRequestComplete().BindLambda(
		[WeakThis, Done, bDelivered](FHttpRequestPtr InRequest, FHttpResponsePtr InResponse, bool bSucceeded)
		{
			// 连接都没了就别回调了：调用方（面板）跟连接是同生共死的，
			// 这时候回过去多半是往一块已经析构的内存上写。
			if (!WeakThis.IsValid() || !Done || *bDelivered)
			{
				return;
			}
			*bDelivered = true;

			FDreamTextureChecksResult Result;

			if (!bSucceeded || !InResponse.IsValid())
			{
				Done(false, MoveTemp(Result),
					FText::Format(NSLOCTEXT("DreamTextureConnection", "ChecksFailed", "Could not load the Environment check: {0}"), FText::FromString(DescribeHttpFailure(InRequest, InResponse))).ToString());
				return;
			}

			const int32 ResponseCode = InResponse->GetResponseCode();
			if (!EHttpResponseCodes::IsOk(ResponseCode))
			{
				Done(false, MoveTemp(Result), FText::Format(NSLOCTEXT("DreamTextureConnection", "ChecksBadStatus", "The Environment check endpoint returned HTTP {0}."), FText::AsNumber(ResponseCode)).ToString());
				return;
			}

			if (!ParseChecksResponse(InResponse->GetContentAsString(), Result))
			{
				Done(false, MoveTemp(Result), NSLOCTEXT("DreamTextureConnection", "ChecksBadJson", "The Environment check endpoint did not return valid JSON.").ToString());
				return;
			}

			Done(true, MoveTemp(Result), FString());
		});

	if (!Request->ProcessRequest() && Done && !*bDelivered)
	{
		*bDelivered = true;
		// 同步失败也得给调用方一个交代，否则面板上那个转圈会一直转下去。
		FDreamTextureChecksResult Empty;
		Done(false, MoveTemp(Empty), FText::Format(NSLOCTEXT("DreamTextureConnection", "CannotSend", "The request could not be sent; check that the address is correct: {0}"), FText::FromString(URL)).ToString());
	}
}
