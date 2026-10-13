// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTexturePushSession.h"

#include "DreamTextureConnection.h"
#include "DreamTextureHttpUtil.h"
#include "DreamTextureLog.h"
#include "DreamTextureSettings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	/** 注册 / 心跳的超时。localhost 上正常是毫秒级，给长了只会让「后端卡死」拖着状态不动。 */
	constexpr float RequestTimeoutSecs = 5.0f;

	/** 注销的超时。退出时发的，短一点：宁可没注销（后端 45 秒后自己清）也别拖住退出。 */
	constexpr float UnregisterTimeoutSecs = 2.0f;

	/**
	 * 这几个状态码表示「后端根本没有这套接口」。
	 *
	 * 404 是旧后端的老实回答；405 是路径在、方法不对；501 是协议里约定的「不支持」。
	 * 再加上 200 却没有 session_id 的情形——旧后端对不认识的路径一律回落到前端的 index.html，
	 * 状态码是 200，这一条在 HandleRegisterReply 里单独判。
	 */
	bool IsNotSupportedStatus(int32 Code)
	{
		return Code == 404 || Code == 405 || Code == 501;
	}

	/** material_id 会被拼进磁盘路径与 URL，挡住明显不是 id 的东西。 */
	bool LooksLikeMaterialId(const FString& Id)
	{
		return !Id.IsEmpty() && Id != TEXT(".") && Id != TEXT("..")
			&& !Id.Contains(TEXT("..")) && !Id.Contains(TEXT("/")) && !Id.Contains(TEXT("\\"));
	}
}

const TCHAR* LexToString(EDreamTexturePushState State)
{
	switch (State)
	{
	case EDreamTexturePushState::Unregistered: return TEXT("Unregistered");
	case EDreamTexturePushState::Registering:  return TEXT("Registering");
	case EDreamTexturePushState::Registered:   return TEXT("Registered");
	case EDreamTexturePushState::Unsupported:  return TEXT("Unsupported");
	}
	return TEXT("Unknown");
}

//////////////////////////////////////////////////////////////////////////
// 注册信息与默认的网络层

FDreamTextureSessionRegistration FDreamTextureSessionRegistration::FromCurrentProcess()
{
	FDreamTextureSessionRegistration Out;
	Out.Name = FApp::GetProjectName();
	if (Out.Name.IsEmpty())
	{
		Out.Name = TEXT("UnrealEditor");
	}
	Out.Project = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
	Out.Engine = FEngineVersion::Current().ToString(EVersionComponent::Patch);
	Out.PID = static_cast<int32>(FPlatformProcess::GetCurrentProcessId());
	return Out;
}

FString FDreamTextureSessionRegistration::ToJsonString() const
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("name"), Name);
	Object->SetStringField(TEXT("project"), Project);
	Object->SetStringField(TEXT("engine"), Engine);
	Object->SetNumberField(TEXT("pid"), PID);

	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Object, Writer);
	return Out;
}

FDreamTexturePushSession::FHooks FDreamTexturePushSession::FHooks::MakeDefault()
{
	FHooks Out;
	Out.Send = [](const FString& Verb, const FString& Path, const FString& JsonBody, float TimeoutSecs, FOnReply Done)
	{
		// 编辑器退出时模块是一个个关的。注销请求发出那一刻 HTTP 模块还在：DreamTextureClient 启动时先加载了它，
		// 所以它比我们晚卸载（Build.cs 里的依赖不管卸载顺序）。万一不在，别在关闭途中把它重新加载起来。
		if (!FModuleManager::Get().IsModuleLoaded(TEXT("HTTP")))
		{
			FDreamTextureSessionReply Reply;
			// 这条只会出现在编辑器退出的路上，那时本地化管理器可能已经先关了，所以不走 NSLOCTEXT。
			Reply.Error = TEXT("The HTTP module is not loaded.");
			Done(Reply);
			return;
		}

		// 完整地址（注销时用的缓存地址）原样用，不碰设置对象：模块关闭时它可能已经不在了。
		const bool bAbsolute = Path.StartsWith(TEXT("http://")) || Path.StartsWith(TEXT("https://"));
		const FString Url = bAbsolute ? Path : UDreamTextureSettings::Get().GetEndpoint(Path);
		DreamTexture::SendRequest(*Verb, Url, TimeoutSecs, JsonBody,
			[Done = MoveTemp(Done)](const DreamTexture::FHttpOutcome& Outcome)
			{
				FDreamTextureSessionReply Reply;
				Reply.bGotResponse = Outcome.Response.IsValid();
				Reply.StatusCode = Outcome.StatusCode;
				Reply.Body = Outcome.Response.IsValid() ? Outcome.Response->GetContentAsString() : FString();
				Reply.Error = Outcome.Error;
				Done(Reply);
			});
	};
	Out.GetRegistration = []() { return FDreamTextureSessionRegistration::FromCurrentProcess(); };
	Out.GetBaseURL = []() { return UDreamTextureSettings::Get().GetNormalizedBackendURL(); };
	return Out;
}

//////////////////////////////////////////////////////////////////////////
// 生命周期

FDreamTexturePushSession::FDreamTexturePushSession(FHooks InHooks)
	: Hooks(MoveTemp(InHooks))
{
}

FDreamTexturePushSession::~FDreamTexturePushSession()
{
	// 析构里不发请求（拿不到 AsShared，回调也没有可捕获的东西），只把订阅与定时器摘干净。
	// 要注销请在持有者那里先调 Shutdown()。
	Unbind();
}

void FDreamTexturePushSession::Bind(const TSharedPtr<FDreamTextureConnection>& Connection)
{
	Unbind();
	if (!Connection.IsValid())
	{
		return;
	}

	BoundConnection = Connection;
	EventHandle = Connection->OnEvent.AddSP(this, &FDreamTexturePushSession::HandleEvent);
	LinkHandle = Connection->OnLinkChanged.AddSP(this, &FDreamTexturePushSession::HandleLinkChanged);

	// 回调里只持弱引用：会话对象先没了，下一次 tick 自己返回 false 把自己摘掉。
	// 回调自己从不 RemoveTicker——在 ticker 自己的回调里摘自己是死锁（见 Docs/contributing.md 的 UE 5.8 pitfalls）。
	PulseHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([Weak = AsWeak()](float) -> bool
		{
			if (const TSharedPtr<FDreamTexturePushSession> Self = Weak.Pin())
			{
				Self->Pulse();
				return true;
			}
			return false;
		}),
		Hooks.PulseIntervalSecs);

	// 绑定时连接可能已经是 Live（面板晚于连接建立）：不会再有一次状态变化通知我们。
	if (Connection->GetLink() == EDreamTextureLink::Live)
	{
		HandleLinkChanged(EDreamTextureLink::Live);
	}
}

void FDreamTexturePushSession::Unbind()
{
	if (PulseHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PulseHandle);
		PulseHandle.Reset();
	}

	// 连接可能先于我们销毁（模块关闭的顺序），所以先 Pin。
	if (const TSharedPtr<FDreamTextureConnection> Connection = BoundConnection.Pin())
	{
		Connection->OnEvent.Remove(EventHandle);
		Connection->OnLinkChanged.Remove(LinkHandle);
	}
	BoundConnection.Reset();
	EventHandle.Reset();
	LinkHandle.Reset();

	// 作废所有在途请求，并且不再认为自己在线。
	++Epoch;
	bLive = false;
	bHeartbeatInFlight = false;
	if (State != EDreamTexturePushState::Unsupported)
	{
		State = EDreamTexturePushState::Unregistered;
	}
}

void FDreamTexturePushSession::Shutdown()
{
	const FString Id = SessionId;
	const bool bShouldUnregister = !Id.IsEmpty() && State != EDreamTexturePushState::Unsupported;

	Unbind();
	SessionId.Reset();
	SessionName.Reset();
	Publish();

	if (bShouldUnregister)
	{
		SendUnregister(Id);
	}
}

void FDreamTexturePushSession::HandleLinkChanged(EDreamTextureLink NewLink)
{
	const bool bNowLive = NewLink == EDreamTextureLink::Live;
	if (bNowLive == bLive)
	{
		// Connecting ↔ Stale ↔ Offline 之间的来回对我们没有区别。
		return;
	}

	bLive = bNowLive;
	++Epoch;
	bHeartbeatInFlight = false;

	if (bLive)
	{
		// 后端可能在断线期间被换成了新版本，也可能还是那个旧的：Unsupported 不是永久的，每次重连试一次。
		if (State == EDreamTexturePushState::Unsupported)
		{
			State = EDreamTexturePushState::Unregistered;
			LastError.Reset();
			LastLoggedFailure.Reset();
		}
		Reconcile();
		return;
	}

	// 链路没了：停心跳（Pulse 看 bLive）。session_id 留着，等下一次 Live 去确认它还在不在。
	// 状态退回「未注册」而不是继续说「已注册」——没在续期的会话 45 秒后后端就不认了。
	if (State != EDreamTexturePushState::Unsupported)
	{
		State = EDreamTexturePushState::Unregistered;
	}
	Publish();
}

void FDreamTexturePushSession::Reconcile()
{
	if (!bLive || State == EDreamTexturePushState::Unsupported)
	{
		return;
	}

	if (SessionId.IsEmpty())
	{
		Register();
	}
	else
	{
		SendHeartbeat(/*bResuming*/ true);
	}
}

void FDreamTexturePushSession::Pulse()
{
	if (!bLive)
	{
		return;
	}

	switch (State)
	{
	case EDreamTexturePushState::Registered:
		SendHeartbeat(/*bResuming*/ false);
		break;
	case EDreamTexturePushState::Unregistered:
		// 上一次注册 / 确认失败了（后端抖了一下），这一轮再试。
		Reconcile();
		break;
	case EDreamTexturePushState::Registering:
	case EDreamTexturePushState::Unsupported:
		break;
	}
}

//////////////////////////////////////////////////////////////////////////
// 请求

void FDreamTexturePushSession::Register()
{
	State = EDreamTexturePushState::Registering;
	Publish();

	const FDreamTextureSessionRegistration Registration = Hooks.GetRegistration
		? Hooks.GetRegistration() : FDreamTextureSessionRegistration::FromCurrentProcess();
	const uint32 SentEpoch = Epoch;

	// 趁现在（编辑器正常运行、设置对象一定在）记下后端地址，注销时只用它。
	if (Hooks.GetBaseURL)
	{
		UnregisterBaseURL = Hooks.GetBaseURL();
	}

	// 以下是最后一步：Send 的实现可以同步回调，回来时状态已经被改过，这里之后不能再碰状态。
	Hooks.Send(TEXT("POST"), TEXT("/api/ue/sessions"), Registration.ToJsonString(), RequestTimeoutSecs,
		[Weak = AsWeak(), SentEpoch, Name = Registration.Name](const FDreamTextureSessionReply& Reply)
		{
			if (const TSharedPtr<FDreamTexturePushSession> Self = Weak.Pin())
			{
				Self->HandleRegisterReply(SentEpoch, Name, Reply);
			}
		});
}

void FDreamTexturePushSession::HandleRegisterReply(uint32 InEpoch, const FString& ReportedName, const FDreamTextureSessionReply& Reply)
{
	FString NewId;
	if (Reply.IsSuccess())
	{
		if (const TSharedPtr<FJsonObject> Root = DreamTexture::ParseJsonObject(Reply.Body))
		{
			Root->TryGetStringField(TEXT("session_id"), NewId);
		}
	}

	if (InEpoch != Epoch)
	{
		// 请求在途时链路换了一轮（或已 Shutdown）。这个会话没人认领了：能注销就注销，免得 Web 端多一个 45 秒的幽灵。
		if (!NewId.IsEmpty())
		{
			SendUnregister(NewId);
		}
		return;
	}

	if (Reply.IsSuccess())
	{
		if (NewId.IsEmpty())
		{
			MarkUnsupported(NSLOCTEXT("DreamTexturePushSession", "NoSessionId", "The backend's reply to /api/ue/sessions has no session_id (most likely an older version without push support).").ToString());
			return;
		}

		SessionId = NewId;
		SessionName = ReportedName;
		State = EDreamTexturePushState::Registered;
		LastError.Reset();
		LastLoggedFailure.Reset();
		UE_LOG(LogDreamTexture, Log, TEXT("Registered the editor session with the backend: %s (%s)"), *SessionId, *SessionName);
		Publish();

		// 立刻心跳一次取 pending：后端可能把被替换的旧会话没确认的投递带给了这个新会话（见类注释）。
		SendHeartbeat(/*bResuming*/ false);
		return;
	}

	if (Reply.bGotResponse && IsNotSupportedStatus(Reply.StatusCode))
	{
		MarkUnsupported(FText::Format(NSLOCTEXT("DreamTexturePushSession", "RegisterNotSupported", "The backend returned HTTP {0} and has no push session API (an older version)."), FText::AsNumber(Reply.StatusCode)).ToString());
		return;
	}

	State = EDreamTexturePushState::Unregistered;
	NoteFailure(Reply.Error.IsEmpty() ? NSLOCTEXT("DreamTexturePushSession", "RegisterFailed", "Could not register the session.").ToString() : Reply.Error);
}

void FDreamTexturePushSession::SendHeartbeat(bool bResuming)
{
	if (bHeartbeatInFlight || SessionId.IsEmpty())
	{
		return;
	}
	bHeartbeatInFlight = true;

	if (bResuming)
	{
		// 重连后确认会话还在：界面上不能继续说「已注册」，也还不能说「未注册」。
		State = EDreamTexturePushState::Registering;
		Publish();
	}

	const uint32 SentEpoch = Epoch;
	const FString Path = FString::Printf(TEXT("/api/ue/sessions/%s/heartbeat"), *DreamTexture::PercentEncode(SessionId));

	// 请求体：有待确认的就带 {"ack":[…]}，没有就不发体（旧后端第一版只认无体的心跳，也能续期）。
	// 请求成功才把它们从待确认里划掉：失败了下一次还要带。
	const TArray<FString> SentAcks = PendingAcks;
	FString Body;
	if (SentAcks.Num() > 0)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Ack : SentAcks)
		{
			Values.Add(MakeShared<FJsonValueString>(Ack));
		}
		Object->SetArrayField(TEXT("ack"), Values);
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
		FJsonSerializer::Serialize(Object, Writer);
	}

	Hooks.Send(TEXT("POST"), Path, Body, RequestTimeoutSecs,
		[Weak = AsWeak(), SentEpoch, bResuming, SentAcks](const FDreamTextureSessionReply& Reply)
		{
			if (const TSharedPtr<FDreamTexturePushSession> Self = Weak.Pin())
			{
				Self->HandleHeartbeatReply(SentEpoch, bResuming, SentAcks, Reply);
			}
		});
}

void FDreamTexturePushSession::HandleHeartbeatReply(uint32 InEpoch, bool bResuming, const TArray<FString>& SentAcks, const FDreamTextureSessionReply& Reply)
{
	if (InEpoch != Epoch)
	{
		// 过期的：epoch 自增时 bHeartbeatInFlight 已经复位了，这里什么都不用动。
		return;
	}
	bHeartbeatInFlight = false;

	if (Reply.IsSuccess())
	{
		State = EDreamTexturePushState::Registered;
		LastError.Reset();
		LastLoggedFailure.Reset();

		// 后端收到了确认。这次请求在途期间新处理的不在 SentAcks 里，原样留给下一次。
		if (SentAcks.Num() > 0)
		{
			PendingAcks.RemoveAll([&SentAcks](const FString& Id) { return SentAcks.Contains(Id); });
		}

		// 先把状态公布出去再处理投递：投递处理会触发导入等一串动作，界面此刻应该已经是「已注册」。
		Publish();

		// 200 带 pending；204（旧后端第一版）没有应答体，什么都不用做。
		if (Reply.StatusCode == 200)
		{
			ProcessPendingBody(Reply.Body);
		}
		return;
	}

	if (Reply.bGotResponse && Reply.StatusCode == 404)
	{
		// 后端不认这个会话了：重启过，或者断得比 45 秒久被清掉了。这是协议约定的信号，不是错误。
		UE_LOG(LogDreamTexture, Log, TEXT("The backend no longer recognizes session %s; registering again"), *SessionId);
		SessionId.Reset();
		SessionName.Reset();
		// 待确认的是旧会话的投递，新会话里没有这些 id；留着只会白白占请求体。
		PendingAcks.Reset();
		Register();
		return;
	}

	if (Reply.bGotResponse && (Reply.StatusCode == 405 || Reply.StatusCode == 501))
	{
		MarkUnsupported(FText::Format(NSLOCTEXT("DreamTexturePushSession", "HeartbeatNotSupported", "The backend returned HTTP {0} for the heartbeat, so push sessions are unavailable."), FText::AsNumber(Reply.StatusCode)).ToString());
		return;
	}

	// 其它失败（超时、5xx）：会话还在不在不知道。已注册的就保持，下一轮再发——真断了的话链路会先变 Stale，
	// 那边会停掉心跳；确认中的退回未注册，下一轮 Pulse 再确认一次。
	if (bResuming)
	{
		State = EDreamTexturePushState::Unregistered;
	}
	NoteFailure(Reply.Error.IsEmpty() ? NSLOCTEXT("DreamTexturePushSession", "HeartbeatFailed", "The heartbeat failed.").ToString() : Reply.Error);
}

void FDreamTexturePushSession::SendUnregister(const FString& Id)
{
	if (Id.IsEmpty() || !Hooks.Send)
	{
		return;
	}

	// 不等应答，也不捕获 this：Shutdown 之后对象随时会没，应答来时什么都不用做。
	// 用注册时缓存的完整地址：这里多半是在模块关闭途中，不能再去取设置对象。
	const FString Path = FString::Printf(TEXT("/api/ue/sessions/%s"), *DreamTexture::PercentEncode(Id));
	const FString Target = UnregisterBaseURL.IsEmpty() ? Path : UnregisterBaseURL + Path;
	Hooks.Send(TEXT("DELETE"), Target, FString(), UnregisterTimeoutSecs, [](const FDreamTextureSessionReply&) {});
}

//////////////////////////////////////////////////////////////////////////
// 状态

void FDreamTexturePushSession::MarkUnsupported(const FString& Why)
{
	State = EDreamTexturePushState::Unsupported;
	SessionId.Reset();
	SessionName.Reset();
	PendingAcks.Reset();
	LastError = Why;

	// 一次连接只写一条，而且只是 Log 级：旧后端不是故障，不该在日志里红一片。
	UE_LOG(LogDreamTexture, Log, TEXT("Editor push is disabled: %s. Reconnect after upgrading the backend."), *Why);
	Publish();
}

void FDreamTexturePushSession::NoteFailure(const FString& Why)
{
	LastError = Why;
	if (Why != LastLoggedFailure)
	{
		// 同一句原因只写一次：后端半死不活的时候，每 15 秒一条会把日志刷满。
		LastLoggedFailure = Why;
		UE_LOG(LogDreamTexture, Warning, TEXT("Renewing or registering the editor session failed: %s (retrying in 15 seconds)"), *Why);
	}
	Publish();
}

void FDreamTexturePushSession::Publish()
{
	if (PublishedState == State && PublishedId == SessionId && PublishedError == LastError)
	{
		return;
	}
	PublishedState = State;
	PublishedId = SessionId;
	PublishedError = LastError;
	OnStateChanged.Broadcast();
}

//////////////////////////////////////////////////////////////////////////
// 事件

void FDreamTexturePushSession::HandleEvent(const FDreamTextureEvent& Event)
{
	if (!Event.Type.Equals(TEXT("ue.import"), ESearchCase::CaseSensitive) || !Event.Raw.IsValid())
	{
		return;
	}

	const TSharedPtr<FJsonObject>* Data = nullptr;
	if (!Event.Raw->TryGetObjectField(TEXT("data"), Data) || Data == nullptr || !Data->IsValid())
	{
		return;
	}

	FString TargetSession;
	FString MaterialId;
	FString DeliveryId;
	(*Data)->TryGetStringField(TEXT("session_id"), TargetSession);
	(*Data)->TryGetStringField(TEXT("material_id"), MaterialId);
	// 第二版才有；旧后端的事件没有，此时无从去重也无从确认，按第一版的方式处理一次。
	(*Data)->TryGetStringField(TEXT("delivery_id"), DeliveryId);

	// 这条事件所有订阅者（包括别的编辑器、Web 页面）都收得到，只认发给自己的。
	// 区分大小写：id 是后端生成的不透明串，FString 的 == 默认不区分，会把两个只差大小写的 id 当成同一个。
	if (SessionId.IsEmpty() || !TargetSession.Equals(SessionId, ESearchCase::CaseSensitive))
	{
		return;
	}

	ProcessDelivery(DeliveryId, MaterialId, /*bFromHeartbeat*/ false);
}

bool FDreamTexturePushSession::ProcessDelivery(const FString& DeliveryId, const FString& MaterialId, bool bFromHeartbeat)
{
	if (!DeliveryId.IsEmpty())
	{
		if (Handled.Contains(DeliveryId))
		{
			// WS 与心跳各送了一遍：只处理一次。心跳还在列它说明上次的确认没送到（或还在路上），
			// 补进待确认，下一次再带；WS 重复的不用管。
			if (bFromHeartbeat && !PendingAcks.Contains(DeliveryId))
			{
				PendingAcks.Add(DeliveryId);
			}
			return false;
		}

		Handled.Add(DeliveryId);
		HandledOrder.Add(DeliveryId);
		while (HandledOrder.Num() > MaxTrackedDeliveries)
		{
			Handled.Remove(HandledOrder[0]);
			HandledOrder.RemoveAt(0);
		}

		// 先确认再处理：就算这个 id 后面被判成无效，也得让后端别再补发它。
		PendingAcks.Add(DeliveryId);
		while (PendingAcks.Num() > MaxTrackedDeliveries)
		{
			PendingAcks.RemoveAt(0);
		}
	}

	if (!LooksLikeMaterialId(MaterialId))
	{
		UE_LOG(LogDreamTexture, Warning, TEXT("Received a push addressed to this editor, but material_id does not look like an id (%s); ignored"), *MaterialId);
		return false;
	}

	UE_LOG(LogDreamTexture, Log, TEXT("The web UI pushed material %s (%s, delivery %s)"), *MaterialId,
		bFromHeartbeat ? TEXT("redelivered via heartbeat") : TEXT("WS event"), DeliveryId.IsEmpty() ? TEXT("no id") : *DeliveryId);
	OnImportPushed.Broadcast(MaterialId);
	return true;
}

void FDreamTexturePushSession::ProcessPendingBody(const FString& Body)
{
	if (Body.IsEmpty())
	{
		return;
	}
	const TSharedPtr<FJsonObject> Root = DreamTexture::ParseJsonObject(Body);
	const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
	// pending 可能是 null（Go 的 nil 切片）或缺失：都当没有。
	if (!Root.IsValid() || !Root->TryGetArrayField(TEXT("pending"), List) || List == nullptr)
	{
		return;
	}

	// 逐条处理时委托的接收方可能做任何事，先把要处理的拷出来再动手。
	TArray<TPair<FString, FString>> Items;
	for (const TSharedPtr<FJsonValue>& Value : *List)
	{
		// 先看类型再 AsObject：对不是对象的值调 AsObject 会往 LogJson 写 Error，自动化里算失败。
		const TSharedPtr<FJsonObject> Entry = (Value.IsValid() && Value->Type == EJson::Object) ? Value->AsObject() : nullptr;
		if (!Entry.IsValid())
		{
			continue;
		}
		FString DeliveryId;
		FString MaterialId;
		Entry->TryGetStringField(TEXT("delivery_id"), DeliveryId);
		Entry->TryGetStringField(TEXT("material_id"), MaterialId);
		// 没有 delivery_id 的 pending 项没法去重也没法确认，每次心跳都会重复处理——后端不该这么发，宁可不要。
		if (DeliveryId.IsEmpty())
		{
			continue;
		}
		Items.Emplace(MoveTemp(DeliveryId), MoveTemp(MaterialId));
	}

	for (const TPair<FString, FString>& Item : Items)
	{
		ProcessDelivery(Item.Key, Item.Value, /*bFromHeartbeat*/ true);
	}
}
