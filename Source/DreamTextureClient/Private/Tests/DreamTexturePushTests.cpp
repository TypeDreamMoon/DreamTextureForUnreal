// Copyright TypeDreamMoon. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureConnection.h"
#include "DreamTexturePushSession.h"
#include "DreamTextureTestUtil.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

/*
 * DreamTexture.Client.Push.* —— 推送会话（M6 插件侧）。
 *
 * 网络层换成假后端（FFakeSessionBackend）：它按协议答注册 / 心跳 / 注销，会话表在它自己手里，
 * 能「重启」（清空会话表）、能装成旧后端（404 / 501 / 200 回落页面）、能把应答攥住不放。
 * 定时器的间隔设得很大，测试自己调 Pulse() 驱动，所以不依赖时钟；只有 Ticker 那一条真等定时器。
 */

namespace
{
	using FOnReply = FDreamTexturePushSession::FOnReply;

	/** 按 Docs/contributing.md 里「Push protocol」答话的假后端。 */
	struct FFakeSessionBackend
	{
		struct FCall
		{
			FString Verb;
			FString Path;
			FString Body;
		};

		TArray<FCall> Calls;
		TSet<FString> Sessions;
		int32 NextId = 1;

		/** 后端为这个会话攒着的未确认投递（delivery_id → material_id），按发送先后。 */
		TArray<TPair<FString, FString>> PendingItems;

		/** 心跳回 204、不带 pending：第一版后端。 */
		bool bLegacyHeartbeat204 = false;

		/** 收到过的全部 ack（按到达先后）。 */
		TArray<FString> AckLog;

		/** 记下 ack 但不删投递：模拟「确认送到了、后端那边却没删成」。 */
		bool bIgnoreAcks = false;

		//~ 把后端装成各种样子
		int32 RegisterStatus = 200;      // 非 200：注册直接回这个状态码
		bool bRegisterHtml = false;      // 200，但应答体是 index.html（旧后端的 SPA 回落）
		bool bRegisterTransportFail = false;
		int32 HeartbeatStatus = 0;       // 非 0：心跳直接回这个状态码
		bool bHeartbeatTransportFail = false;

		/** 为 true 时应答攥在手里，Release() 才回。 */
		bool bHold = false;
		TArray<TFunction<void()>> Held;

		FDreamTextureSessionReply Respond(const FCall& Call)
		{
			FDreamTextureSessionReply Reply;
			Reply.bGotResponse = true;

			if (Call.Verb == TEXT("POST") && Call.Path == TEXT("/api/ue/sessions"))
			{
				if (bRegisterTransportFail)
				{
					Reply.bGotResponse = false;
					Reply.Error = TEXT("连不上，后端进程可能没在跑");
				}
				else if (RegisterStatus != 200)
				{
					Reply.StatusCode = RegisterStatus;
					Reply.Error = FString::Printf(TEXT("后端返回 HTTP %d"), RegisterStatus);
				}
				else if (bRegisterHtml)
				{
					Reply.StatusCode = 200;
					Reply.Body = TEXT("<!doctype html><html><body>dt-placeholder</body></html>");
				}
				else
				{
					const FString Id = FString::Printf(TEXT("ue_%d"), NextId++);
					Sessions.Add(Id);
					Reply.StatusCode = 200;
					Reply.Body = FString::Printf(TEXT("{\"session_id\":\"%s\"}"), *Id);
				}
				return Reply;
			}

			if (Call.Verb == TEXT("POST") && Call.Path.EndsWith(TEXT("/heartbeat")))
			{
				if (bHeartbeatTransportFail)
				{
					Reply.bGotResponse = false;
					Reply.Error = TEXT("超时没有应答");
					return Reply;
				}
				if (HeartbeatStatus != 0)
				{
					Reply.StatusCode = HeartbeatStatus;
					Reply.Error = FString::Printf(TEXT("后端返回 HTTP %d"), HeartbeatStatus);
					return Reply;
				}
				const FString Id = IdOf(Call.Path);
				const bool bKnown = Sessions.Contains(Id);
				if (!bKnown)
				{
					Reply.StatusCode = 404;
					Reply.Error = TEXT("后端返回 HTTP 404：会话不存在或已超时，请重新注册");
					return Reply;
				}
				if (bLegacyHeartbeat204)
				{
					Reply.StatusCode = 204;
					return Reply;
				}

				// 第二版：先按 ack 删，再把剩下的全部作为 pending 回给它。
				TSharedPtr<FJsonObject> Parsed;
				if (!Call.Body.IsEmpty() && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Call.Body), Parsed) && Parsed.IsValid())
				{
					const TArray<TSharedPtr<FJsonValue>>* Acks = nullptr;
					if (Parsed->TryGetArrayField(TEXT("ack"), Acks) && Acks != nullptr)
					{
						for (const TSharedPtr<FJsonValue>& Ack : *Acks)
						{
							const FString AckId = Ack->AsString();
							AckLog.Add(AckId);
							if (!bIgnoreAcks)
							{
								PendingItems.RemoveAll([&AckId](const TPair<FString, FString>& Item) { return Item.Key == AckId; });
							}
						}
					}
				}
				FString Pending;
				for (const TPair<FString, FString>& Item : PendingItems)
				{
					Pending += FString::Printf(TEXT("%s{\"delivery_id\":\"%s\",\"material_id\":\"%s\",\"sent_at\":\"2026-10-12T00:00:00Z\"}"),
						Pending.IsEmpty() ? TEXT("") : TEXT(","), *Item.Key, *Item.Value);
				}
				Reply.StatusCode = 200;
				Reply.Body = FString::Printf(TEXT("{\"pending\":[%s]}"), *Pending);
				return Reply;
			}

			if (Call.Verb == TEXT("DELETE"))
			{
				Sessions.Remove(IdOf(Call.Path));
				Reply.StatusCode = 204;
				return Reply;
			}

			Reply.StatusCode = 500;
			Reply.Error = TEXT("假后端不认识这个请求");
			return Reply;
		}

		/** /api/ue/sessions/{id}[/heartbeat] 里取 id。 */
		static FString IdOf(const FString& Path)
		{
			FString Rest = Path;
			Rest.RemoveFromStart(TEXT("/api/ue/sessions/"));
			int32 Slash = INDEX_NONE;
			if (Rest.FindChar(TEXT('/'), Slash))
			{
				Rest.LeftInline(Slash);
			}
			return Rest;
		}

		/** 模拟后端重启：会话表没了，id 计数不回头（新进程发的 id 不会和旧的重复）。 */
		void Restart()
		{
			Sessions.Reset();
		}

		void Release()
		{
			TArray<TFunction<void()>> Pending = MoveTemp(Held);
			Held.Reset();
			for (const TFunction<void()>& Fn : Pending)
			{
				Fn();
			}
		}

		int32 Count(const TCHAR* Verb, const TCHAR* PathSuffix = nullptr) const
		{
			int32 N = 0;
			for (const FCall& Call : Calls)
			{
				if (Call.Verb == Verb && (PathSuffix == nullptr || Call.Path.EndsWith(PathSuffix)))
				{
					++N;
				}
			}
			return N;
		}

		int32 CountRegisters() const { return Count(TEXT("POST"), TEXT("/api/ue/sessions")); }
		int32 CountHeartbeats() const { return Count(TEXT("POST"), TEXT("/heartbeat")); }
		int32 CountDeletes() const { return Count(TEXT("DELETE")); }

		/** 最近一次心跳的请求体。 */
		FString LastHeartbeatBody() const
		{
			for (int32 i = Calls.Num() - 1; i >= 0; --i)
			{
				if (Calls[i].Verb == TEXT("POST") && Calls[i].Path.EndsWith(TEXT("/heartbeat")))
				{
					return Calls[i].Body;
				}
			}
			return FString();
		}

		FDreamTexturePushSession::FHooks MakeHooks(TSharedRef<FFakeSessionBackend> Self, float PulseSecs = 1000.0f)
		{
			FDreamTexturePushSession::FHooks Hooks;
			Hooks.PulseIntervalSecs = PulseSecs;
			Hooks.GetRegistration = []()
			{
				FDreamTextureSessionRegistration R;
				R.Name = TEXT("TestProj");
				R.Project = TEXT("C:/Projects/TestProj/TestProj.uproject");
				R.Engine = TEXT("5.8.2");
				R.PID = 4242;
				return R;
			};
			Hooks.Send = [Self](const FString& Verb, const FString& Path, const FString& Body, float /*Timeout*/, FOnReply Done)
			{
				FCall Call;
				Call.Verb = Verb;
				Call.Path = Path;
				Call.Body = Body;
				Self->Calls.Add(Call);

				// 应答在发出这一刻就按当时的后端状态算好：攥住的应答放出来时，内容是它被请求时的样子。
				const FDreamTextureSessionReply Reply = Self->Respond(Call);
				if (Self->bHold)
				{
					Self->Held.Add([Done, Reply]() { Done(Reply); });
				}
				else
				{
					Done(Reply);
				}
			};
			return Hooks;
		}
	};

	/** 单纯睡一会儿，让真的定时器有机会跳。 */
	class FDreamTexturePushSleepCommand : public IAutomationLatentCommand
	{
	public:
		explicit FDreamTexturePushSleepCommand(double InSecs) : Secs(InSecs) {}
		virtual bool Update() override { return GetCurrentRunTime() >= Secs; }

	private:
		double Secs;
	};

	struct FRig
	{
		TSharedRef<FFakeSessionBackend> Backend = MakeShared<FFakeSessionBackend>();
		TSharedRef<FDreamTexturePushSession> Session;
		int32 StateChanges = 0;
		TArray<FString> Pushed;

		FRig()
			: Session(MakeShared<FDreamTexturePushSession>(Backend->MakeHooks(Backend)))
		{
			Session->OnStateChanged.AddLambda([this]() { ++StateChanges; });
			Session->OnImportPushed.AddLambda([this](const FString& Id) { Pushed.Add(Id); });
		}

		void Live() { Session->HandleLinkChanged(EDreamTextureLink::Live); }
		void Stale() { Session->HandleLinkChanged(EDreamTextureLink::Stale); }
	};

	FDreamTextureEvent MakeEvent(const FString& Json)
	{
		FDreamTextureEvent Event;
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
		{
			Root->TryGetStringField(TEXT("type"), Event.Type);
			Event.Raw = Root;
		}
		return Event;
	}

	FDreamTextureEvent MakeImportEvent(const FString& SessionId, const FString& MaterialId, const TCHAR* Type = TEXT("ue.import"))
	{
		return MakeEvent(FString::Printf(TEXT("{\"type\":\"%s\",\"data\":{\"session_id\":\"%s\",\"material_id\":\"%s\"}}"), Type, *SessionId, *MaterialId));
	}
}

//////////////////////////////////////////////////////////////////////////
// 注册

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTexturePushRegisterTest, "DreamTexture.Client.Push.Register",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamTexturePushRegisterTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	TestEqual(TEXT("起始是未注册"), Rig.Session->GetState(), EDreamTexturePushState::Unregistered);
	TestTrue(TEXT("起始没有会话 id"), Rig.Session->GetSessionId().IsEmpty());

	// 链路没通之前一个请求都不发。
	Rig.Session->Pulse();
	TestEqual(TEXT("没 Live 时 Pulse 不发请求"), Rig.Backend->Calls.Num(), 0);

	Rig.Live();
	if (!TestEqual(TEXT("变 Live 发一次注册"), Rig.Backend->CountRegisters(), 1))
	{
		return false;
	}
	const FFakeSessionBackend::FCall& Call = Rig.Backend->Calls[0];
	TestEqual(TEXT("注册是 POST"), Call.Verb, FString(TEXT("POST")));
	TestEqual(TEXT("注册的路径"), Call.Path, FString(TEXT("/api/ue/sessions")));

	TSharedPtr<FJsonObject> Body;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Call.Body);
	if (TestTrue(TEXT("注册体是合法 JSON"), FJsonSerializer::Deserialize(Reader, Body) && Body.IsValid()))
	{
		TestEqual(TEXT("name 用工程名"), Body->GetStringField(TEXT("name")), FString(TEXT("TestProj")));
		TestEqual(TEXT("project 用工程路径"), Body->GetStringField(TEXT("project")), FString(TEXT("C:/Projects/TestProj/TestProj.uproject")));
		TestEqual(TEXT("engine"), Body->GetStringField(TEXT("engine")), FString(TEXT("5.8.2")));
		TestEqual(TEXT("pid 是数字"), static_cast<int32>(Body->GetNumberField(TEXT("pid"))), 4242);
	}

	TestEqual(TEXT("注册成功后是已注册"), Rig.Session->GetState(), EDreamTexturePushState::Registered);
	TestEqual(TEXT("拿到后端发的会话 id"), Rig.Session->GetSessionId(), FString(TEXT("ue_1")));
	TestEqual(TEXT("会话名是注册时报的工程名"), Rig.Session->GetSessionName(), FString(TEXT("TestProj")));
	TestTrue(TEXT("没有错误"), Rig.Session->GetLastError().IsEmpty());
	TestTrue(TEXT("状态变化广播过"), Rig.StateChanges >= 1);

	// 链路没变就不重复注册。
	Rig.Live();
	TestEqual(TEXT("重复的 Live 不再注册"), Rig.Backend->CountRegisters(), 1);

	// 默认注册信息取自当前进程：工程名、绝对路径、pid 都得有。
	const FDreamTextureSessionRegistration Real = FDreamTextureSessionRegistration::FromCurrentProcess();
	TestFalse(TEXT("真实注册信息有工程名"), Real.Name.IsEmpty());
	TestFalse(TEXT("真实注册信息有引擎版本"), Real.Engine.IsEmpty());
	TestTrue(TEXT("真实注册信息有 pid"), Real.PID > 0);
	TestFalse(TEXT("真实工程路径是绝对路径"), FPaths::IsRelative(Real.Project));
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 心跳

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTexturePushHeartbeatTest, "DreamTexture.Client.Push.Heartbeat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamTexturePushHeartbeatTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Live();
	TestEqual(TEXT("先注册"), Rig.Session->GetState(), EDreamTexturePushState::Registered);

	// 注册成功后立刻补了一次心跳（取被替换旧会话带过来的 pending），之后才是定时的。
	const int32 Base = Rig.Backend->CountHeartbeats();
	TestEqual(TEXT("注册后立刻心跳一次"), Base, 1);

	Rig.Session->Pulse();
	Rig.Session->Pulse();
	TestEqual(TEXT("每次 Pulse 发一次心跳"), Rig.Backend->CountHeartbeats(), Base + 2);
	TestEqual(TEXT("心跳路径带会话 id"), Rig.Backend->Calls.Last().Path, FString(TEXT("/api/ue/sessions/ue_1/heartbeat")));
	TestEqual(TEXT("心跳是 POST"), Rig.Backend->Calls.Last().Verb, FString(TEXT("POST")));
	TestEqual(TEXT("心跳不重新注册"), Rig.Backend->CountRegisters(), 1);
	TestEqual(TEXT("续期成功仍是已注册"), Rig.Session->GetState(), EDreamTexturePushState::Registered);

	// 心跳在途时不叠发：后端卡住的时候不能一轮轮往里堆请求。
	Rig.Backend->bHold = true;
	Rig.Session->Pulse();
	Rig.Session->Pulse();
	Rig.Session->Pulse();
	TestEqual(TEXT("在途的心跳没回来之前不再发"), Rig.Backend->CountHeartbeats(), Base + 3);
	Rig.Backend->bHold = false;
	Rig.Backend->Release();
	Rig.Session->Pulse();
	TestEqual(TEXT("应答回来之后又能发"), Rig.Backend->CountHeartbeats(), Base + 4);

	// 传输失败（超时）：会话不丢，状态仍是已注册，错误记下来，下一轮照常发。
	Rig.Backend->bHeartbeatTransportFail = true;
	Rig.Session->Pulse();
	TestEqual(TEXT("心跳超时不丢会话"), Rig.Session->GetState(), EDreamTexturePushState::Registered);
	TestEqual(TEXT("会话 id 还在"), Rig.Session->GetSessionId(), FString(TEXT("ue_1")));
	TestFalse(TEXT("失败原因记下来了"), Rig.Session->GetLastError().IsEmpty());
	Rig.Backend->bHeartbeatTransportFail = false;
	Rig.Session->Pulse();
	TestTrue(TEXT("恢复后错误清掉"), Rig.Session->GetLastError().IsEmpty());
	TestEqual(TEXT("恢复后没有重新注册"), Rig.Backend->CountRegisters(), 1);
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 心跳 404 → 重新注册

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTexturePushRejoinTest, "DreamTexture.Client.Push.Rejoin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamTexturePushRejoinTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Live();
	TestEqual(TEXT("先注册"), Rig.Session->GetSessionId(), FString(TEXT("ue_1")));

	// 后端重启：会话表没了。下一次心跳得 404，插件当场重新注册，不等下一个间隔。
	Rig.Backend->Restart();
	Rig.Session->Pulse();
	TestEqual(TEXT("注册后一次、404 的一次，再加重新注册后立刻补的一次"), Rig.Backend->CountHeartbeats(), 3);
	TestEqual(TEXT("心跳 404 后立刻重新注册"), Rig.Backend->CountRegisters(), 2);
	TestEqual(TEXT("拿到新的会话 id"), Rig.Session->GetSessionId(), FString(TEXT("ue_2")));
	TestEqual(TEXT("重新注册后是已注册"), Rig.Session->GetState(), EDreamTexturePushState::Registered);
	TestTrue(TEXT("404 不算错误"), Rig.Session->GetLastError().IsEmpty());

	// 旧 id 的推送不再认，新 id 的才认。
	Rig.Session->HandleEvent(MakeImportEvent(TEXT("ue_1"), TEXT("mat_a")));
	TestEqual(TEXT("旧会话的推送被忽略"), Rig.Pushed.Num(), 0);
	Rig.Session->HandleEvent(MakeImportEvent(TEXT("ue_2"), TEXT("mat_a")));
	TestEqual(TEXT("新会话的推送被接收"), Rig.Pushed.Num(), 1);

	// 之后的心跳用新 id。
	Rig.Session->Pulse();
	TestEqual(TEXT("新 id 的心跳"), Rig.Backend->Calls.Last().Path, FString(TEXT("/api/ue/sessions/ue_2/heartbeat")));
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 断线重连

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTexturePushReconnectTest, "DreamTexture.Client.Push.Reconnect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamTexturePushReconnectTest::RunTest(const FString& Parameters)
{
	// ---- 后端没丢会话（只是 WS 抖了一下）：重连先确认，id 不变，不产生第二个会话 ----
	{
		FRig Rig;
		Rig.Live();
		Rig.Stale();
		TestEqual(TEXT("链路断了不再说已注册"), Rig.Session->GetState(), EDreamTexturePushState::Unregistered);
		TestEqual(TEXT("会话 id 留着等确认"), Rig.Session->GetSessionId(), FString(TEXT("ue_1")));

		const int32 Before = Rig.Backend->Calls.Num();
		Rig.Session->Pulse();
		Rig.Session->Pulse();
		TestEqual(TEXT("链路断着时不发任何请求"), Rig.Backend->Calls.Num(), Before);

		Rig.Live();
		TestEqual(TEXT("重新 Live 先发心跳确认（加上注册后的那次共 2）"), Rig.Backend->CountHeartbeats(), 2);
		TestEqual(TEXT("没有多注册"), Rig.Backend->CountRegisters(), 1);
		TestEqual(TEXT("确认后是已注册"), Rig.Session->GetState(), EDreamTexturePushState::Registered);
		TestEqual(TEXT("仍是原来的会话"), Rig.Session->GetSessionId(), FString(TEXT("ue_1")));
	}

	// ---- 后端丢了会话（重启过）：重连后重新注册 ----
	{
		FRig Rig;
		Rig.Live();
		Rig.Session->HandleLinkChanged(EDreamTextureLink::Offline);
		Rig.Backend->Restart();
		Rig.Live();
		TestEqual(TEXT("注册后一次、确认 404 一次、重新注册后一次"), Rig.Backend->CountHeartbeats(), 3);
		TestEqual(TEXT("随后重新注册"), Rig.Backend->CountRegisters(), 2);
		TestEqual(TEXT("拿到新会话"), Rig.Session->GetSessionId(), FString(TEXT("ue_2")));
		TestEqual(TEXT("重新注册后是已注册"), Rig.Session->GetState(), EDreamTexturePushState::Registered);
	}

	// ---- 重连时确认心跳超时：退回未注册，下一轮 Pulse 再确认；不会凭空多出注册 ----
	{
		FRig Rig;
		Rig.Live();
		Rig.Stale();
		Rig.Backend->bHeartbeatTransportFail = true;
		Rig.Live();
		TestEqual(TEXT("确认失败是未注册"), Rig.Session->GetState(), EDreamTexturePushState::Unregistered);
		TestFalse(TEXT("原因记下来了"), Rig.Session->GetLastError().IsEmpty());
		Rig.Backend->bHeartbeatTransportFail = false;
		Rig.Session->Pulse();
		TestEqual(TEXT("下一轮确认成功"), Rig.Session->GetState(), EDreamTexturePushState::Registered);
		TestEqual(TEXT("仍是原来的会话"), Rig.Session->GetSessionId(), FString(TEXT("ue_1")));
		TestEqual(TEXT("一直只注册过一次"), Rig.Backend->CountRegisters(), 1);
	}
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 事件过滤

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTexturePushFilterTest, "DreamTexture.Client.Push.Filter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamTexturePushFilterTest::RunTest(const FString& Parameters)
{
	FRig Rig;

	// 还没注册：谁发的都不认（我们还没有「自己」）。
	Rig.Session->HandleEvent(MakeImportEvent(TEXT(""), TEXT("mat_a")));
	TestEqual(TEXT("没会话时空 session_id 也不认"), Rig.Pushed.Num(), 0);

	Rig.Live();
	TestEqual(TEXT("先注册"), Rig.Session->GetSessionId(), FString(TEXT("ue_1")));

	Rig.Session->HandleEvent(MakeImportEvent(TEXT("ue_1"), TEXT("mat_20260827_777001_ref")));
	if (TestEqual(TEXT("发给自己的 ue.import 转成委托"), Rig.Pushed.Num(), 1))
	{
		TestEqual(TEXT("委托带 material_id"), Rig.Pushed[0], FString(TEXT("mat_20260827_777001_ref")));
	}

	Rig.Session->HandleEvent(MakeImportEvent(TEXT("ue_2"), TEXT("mat_b")));
	TestEqual(TEXT("发给别的会话的忽略"), Rig.Pushed.Num(), 1);

	Rig.Session->HandleEvent(MakeImportEvent(TEXT("UE_1"), TEXT("mat_b")));
	TestEqual(TEXT("会话 id 区分大小写"), Rig.Pushed.Num(), 1);

	Rig.Session->HandleEvent(MakeImportEvent(TEXT("ue_1"), TEXT("mat_b"), TEXT("job.done")));
	TestEqual(TEXT("别的事件类型不管"), Rig.Pushed.Num(), 1);

	Rig.Session->HandleEvent(MakeImportEvent(TEXT("ue_1"), TEXT("mat_b"), TEXT("UE.IMPORT")));
	TestEqual(TEXT("事件类型区分大小写"), Rig.Pushed.Num(), 1);

	Rig.Session->HandleEvent(MakeImportEvent(TEXT("ue_1"), TEXT("")));
	TestEqual(TEXT("material_id 为空的忽略"), Rig.Pushed.Num(), 1);

	Rig.Session->HandleEvent(MakeImportEvent(TEXT("ue_1"), TEXT("../etc")));
	Rig.Session->HandleEvent(MakeImportEvent(TEXT("ue_1"), TEXT("a/b")));
	Rig.Session->HandleEvent(MakeImportEvent(TEXT("ue_1"), TEXT("a\\\\b")));
	TestEqual(TEXT("带路径分隔符 / .. 的 material_id 忽略"), Rig.Pushed.Num(), 1);

	Rig.Session->HandleEvent(MakeEvent(TEXT("{\"type\":\"ue.import\"}")));
	Rig.Session->HandleEvent(MakeEvent(TEXT("{\"type\":\"ue.import\",\"data\":null}")));
	Rig.Session->HandleEvent(MakeEvent(TEXT("{\"type\":\"ue.import\",\"data\":{\"material_id\":\"mat_c\"}}")));
	Rig.Session->HandleEvent(FDreamTextureEvent());
	TestEqual(TEXT("缺 data / 缺 session_id / 空事件都不炸、不触发"), Rig.Pushed.Num(), 1);

	// 同一个 id 连推两次：每次都转，去重是导入队列的事（它对在途的同一个 id 返回 false）。
	Rig.Session->HandleEvent(MakeImportEvent(TEXT("ue_1"), TEXT("mat_20260827_777001_ref")));
	TestEqual(TEXT("再次推同一套照样转发"), Rig.Pushed.Num(), 2);
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 后端不支持

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTexturePushUnsupportedTest, "DreamTexture.Client.Push.Unsupported",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamTexturePushUnsupportedTest::RunTest(const FString& Parameters)
{
	// 404 / 405 / 501：路径不存在、方法不对、明说不支持。
	for (const int32 Status : { 404, 405, 501 })
	{
		FRig Rig;
		Rig.Backend->RegisterStatus = Status;
		Rig.Live();

		TestEqual(*FString::Printf(TEXT("%d：置为不支持"), Status), Rig.Session->GetState(), EDreamTexturePushState::Unsupported);
		TestFalse(*FString::Printf(TEXT("%d：说明原因"), Status), Rig.Session->GetLastError().IsEmpty());
		TestTrue(*FString::Printf(TEXT("%d：没有会话"), Status), Rig.Session->GetSessionId().IsEmpty());

		// 停用就是停用：之后的定时动作一个请求都不发。
		Rig.Session->Pulse();
		Rig.Session->Pulse();
		TestEqual(*FString::Printf(TEXT("%d：只请求过一次"), Status), Rig.Backend->Calls.Num(), 1);

		// 事件也不转：没有会话，没有「自己」。
		Rig.Session->HandleEvent(MakeImportEvent(TEXT(""), TEXT("mat_a")));
		TestEqual(*FString::Printf(TEXT("%d：不转事件"), Status), Rig.Pushed.Num(), 0);

		// Shutdown 也不会去注销一个不存在的会话。
		Rig.Session->Shutdown();
		TestEqual(*FString::Printf(TEXT("%d：不发注销"), Status), Rig.Backend->CountDeletes(), 0);
	}

	// 旧后端对不认识的路径回落到前端 index.html，状态码是 200——不能当成注册成功。
	{
		FRig Rig;
		Rig.Backend->bRegisterHtml = true;
		Rig.Live();
		TestEqual(TEXT("200 却没有 session_id：置为不支持"), Rig.Session->GetState(), EDreamTexturePushState::Unsupported);
		TestTrue(TEXT("不把 HTML 当会话 id"), Rig.Session->GetSessionId().IsEmpty());
	}

	// 心跳返回 501：中途被换成了旧后端。
	{
		FRig Rig;
		Rig.Live();
		Rig.Backend->HeartbeatStatus = 501;
		Rig.Session->Pulse();
		TestEqual(TEXT("心跳 501：置为不支持"), Rig.Session->GetState(), EDreamTexturePushState::Unsupported);
	}

	// 不支持不是永久的：后端换成新版本、链路重新连上，再试一次。
	{
		FRig Rig;
		Rig.Backend->RegisterStatus = 404;
		Rig.Live();
		TestEqual(TEXT("先是不支持"), Rig.Session->GetState(), EDreamTexturePushState::Unsupported);

		Rig.Backend->RegisterStatus = 200;
		Rig.Session->Pulse();
		TestEqual(TEXT("同一次连接内不重试"), Rig.Session->GetState(), EDreamTexturePushState::Unsupported);

		Rig.Stale();
		TestEqual(TEXT("链路断开时仍显示不支持"), Rig.Session->GetState(), EDreamTexturePushState::Unsupported);
		Rig.Live();
		TestEqual(TEXT("重新连上后再试一次，这回成了"), Rig.Session->GetState(), EDreamTexturePushState::Registered);
		TestTrue(TEXT("不支持的说明清掉了"), Rig.Session->GetLastError().IsEmpty());
	}
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 注册失败

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTexturePushFailureTest, "DreamTexture.Client.Push.Failure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamTexturePushFailureTest::RunTest(const FString& Parameters)
{
	// 传输失败、5xx：暂时的，不是不支持。下一轮 Pulse 再试。
	{
		FRig Rig;
		Rig.Backend->bRegisterTransportFail = true;
		Rig.Live();
		TestEqual(TEXT("传输失败：未注册"), Rig.Session->GetState(), EDreamTexturePushState::Unregistered);
		TestFalse(TEXT("传输失败：有原因"), Rig.Session->GetLastError().IsEmpty());

		Rig.Session->Pulse();
		TestEqual(TEXT("每个间隔重试一次"), Rig.Backend->CountRegisters(), 2);

		Rig.Backend->bRegisterTransportFail = false;
		Rig.Session->Pulse();
		TestEqual(TEXT("后端恢复后注册成功"), Rig.Session->GetState(), EDreamTexturePushState::Registered);
		TestTrue(TEXT("成功后错误清掉"), Rig.Session->GetLastError().IsEmpty());
		TestEqual(TEXT("拿到会话"), Rig.Session->GetSessionId(), FString(TEXT("ue_1")));
	}

	{
		FRig Rig;
		Rig.Backend->RegisterStatus = 500;
		Rig.Live();
		TestEqual(TEXT("500：未注册而不是不支持"), Rig.Session->GetState(), EDreamTexturePushState::Unregistered);
		Rig.Backend->RegisterStatus = 200;
		Rig.Session->Pulse();
		TestEqual(TEXT("500 过后恢复"), Rig.Session->GetState(), EDreamTexturePushState::Registered);
	}
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 注销

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTexturePushShutdownTest, "DreamTexture.Client.Push.Shutdown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamTexturePushShutdownTest::RunTest(const FString& Parameters)
{
	{
		FRig Rig;
		Rig.Live();
		Rig.Session->Shutdown();

		if (TestEqual(TEXT("关闭时发一次注销"), Rig.Backend->CountDeletes(), 1))
		{
			TestEqual(TEXT("注销路径带会话 id"), Rig.Backend->Calls.Last().Path, FString(TEXT("/api/ue/sessions/ue_1")));
		}
		TestFalse(TEXT("后端那边会话没了"), Rig.Backend->Sessions.Contains(TEXT("ue_1")));
		TestEqual(TEXT("关闭后是未注册"), Rig.Session->GetState(), EDreamTexturePushState::Unregistered);
		TestTrue(TEXT("会话 id 清了"), Rig.Session->GetSessionId().IsEmpty());

		const int32 Before = Rig.Backend->Calls.Num();
		Rig.Session->Pulse();
		TestEqual(TEXT("关闭后不再发任何请求"), Rig.Backend->Calls.Num(), Before);
	}

	// 应答攥着不放（后端已经没了）：关闭本身不能等它。
	{
		FRig Rig;
		Rig.Live();
		Rig.Backend->bHold = true;
		Rig.Session->Shutdown();
		TestEqual(TEXT("不等应答就返回，注销已发出"), Rig.Backend->CountDeletes(), 1);
		TestEqual(TEXT("应答还没回"), Rig.Backend->Held.Num(), 1);
		Rig.Backend->Release();
	}

	// 没注册过：没得注销。
	{
		FRig Rig;
		Rig.Session->Shutdown();
		TestEqual(TEXT("没注册过就不发注销"), Rig.Backend->Calls.Num(), 0);
	}

	// 对象先于 Shutdown 被释放时，攥着的应答回来不能崩。
	{
		TSharedRef<FFakeSessionBackend> Backend = MakeShared<FFakeSessionBackend>();
		{
			Backend->bHold = true;
			const TSharedRef<FDreamTexturePushSession> Session = MakeShared<FDreamTexturePushSession>(Backend->MakeHooks(Backend));
			Session->HandleLinkChanged(EDreamTextureLink::Live);
			TestEqual(TEXT("注册请求在途"), Backend->Held.Num(), 1);
		}
		Backend->Release();
		TestTrue(TEXT("会话对象没了之后应答回来没有副作用"), true);
	}

	// 注销用注册时缓存的完整地址，关闭途中不再取后端地址。
	// 默认实现取地址要碰设置对象（CDO），而注销发生在模块关闭时、UObject 可能已经拆了——
	// GUI 编辑器带着已注册的会话退出时，UDreamTextureSettings::Get 里访问冲突，实测崩过。
	{
		TSharedRef<FFakeSessionBackend> Backend = MakeShared<FFakeSessionBackend>();
		FDreamTexturePushSession::FHooks Hooks = Backend->MakeHooks(Backend);
		int32 BaseCalls = 0;
		Hooks.GetBaseURL = [&BaseCalls]() { ++BaseCalls; return FString(TEXT("http://cached.example:1")); };
		const TSharedRef<FDreamTexturePushSession> Session = MakeShared<FDreamTexturePushSession>(MoveTemp(Hooks));
		Session->HandleLinkChanged(EDreamTextureLink::Live);
		const int32 CallsBeforeShutdown = BaseCalls;
		TestTrue(TEXT("注册时取过后端地址"), CallsBeforeShutdown >= 1);

		Session->Shutdown();
		TestEqual(TEXT("关闭途中没有再取后端地址"), BaseCalls, CallsBeforeShutdown);
		if (TestEqual(TEXT("发了一次注销"), Backend->CountDeletes(), 1))
		{
			TestEqual(TEXT("注销用的是注册时缓存的完整地址"), Backend->Calls.Last().Path,
				FString(TEXT("http://cached.example:1/api/ue/sessions/ue_1")));
		}
	}
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 过期应答

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTexturePushStaleTest, "DreamTexture.Client.Push.Stale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamTexturePushStaleTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Backend->bHold = true;

	// 注册请求在途时链路抖了一下又回来：第二轮发了第二个注册。
	Rig.Live();
	Rig.Stale();
	TestEqual(TEXT("链路断开时在途的注册不再算数，退回未注册"), Rig.Session->GetState(), EDreamTexturePushState::Unregistered);
	Rig.Live();
	if (!TestEqual(TEXT("两轮各一个注册请求"), Rig.Backend->Held.Num(), 2))
	{
		return false;
	}

	// 第一轮的应答姗姗来迟。它发的会话没人认领：忽略它，并把它注销，免得后端多一个幽灵会话。
	TFunction<void()> First = Rig.Backend->Held[0];
	TFunction<void()> Second = Rig.Backend->Held[1];
	Rig.Backend->Held.Reset();
	First();
	TestEqual(TEXT("过期的注册应答不改状态"), Rig.Session->GetState(), EDreamTexturePushState::Registering);
	TestTrue(TEXT("也不采用它的会话 id"), Rig.Session->GetSessionId().IsEmpty());
	TestEqual(TEXT("把过期的会话注销掉"), Rig.Backend->CountDeletes(), 1);
	TestEqual(TEXT("注销的是第一个会话"), Rig.Backend->Calls.Last().Path, FString(TEXT("/api/ue/sessions/ue_1")));

	// 第二轮的应答才算数。
	Rig.Backend->Held.Reset();
	Second();
	TestEqual(TEXT("有效的应答生效"), Rig.Session->GetState(), EDreamTexturePushState::Registered);
	TestEqual(TEXT("采用第二个会话"), Rig.Session->GetSessionId(), FString(TEXT("ue_2")));
	return true;
}


//////////////////////////////////////////////////////////////////////////
// 第二版：心跳兜底投递

namespace
{
	/** 把 ack 数组从心跳请求体里取出来。 */
	TArray<FString> AcksIn(const FString& Body)
	{
		TArray<FString> Out;
		TSharedPtr<FJsonObject> Parsed;
		if (!Body.IsEmpty() && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body), Parsed) && Parsed.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Acks = nullptr;
			if (Parsed->TryGetArrayField(TEXT("ack"), Acks) && Acks != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& Ack : *Acks)
				{
					Out.Add(Ack->AsString());
				}
			}
		}
		return Out;
	}

	FDreamTextureEvent MakeDeliveryEvent(const FString& SessionId, const FString& MaterialId, const FString& DeliveryId)
	{
		return MakeEvent(FString::Printf(TEXT("{\"type\":\"ue.import\",\"data\":{\"session_id\":\"%s\",\"material_id\":\"%s\",\"delivery_id\":\"%s\"}}"),
			*SessionId, *MaterialId, *DeliveryId));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTexturePushPendingTest, "DreamTexture.Client.Push.Pending",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamTexturePushPendingTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Live();
	TestTrue(TEXT("没有投递时请求体为空"), Rig.Backend->LastHeartbeatBody().IsEmpty());

	// 后端攒了两条（WS 没收到的）：下一次心跳应答带回，当场处理，按先后。
	Rig.Backend->PendingItems.Add({ TEXT("d1"), TEXT("mat_a") });
	Rig.Backend->PendingItems.Add({ TEXT("d2"), TEXT("mat_b") });
	Rig.Session->Pulse();
	if (TestEqual(TEXT("心跳带回的 pending 当场处理，不等下一轮"), Rig.Pushed.Num(), 2))
	{
		TestEqual(TEXT("先来的先处理"), Rig.Pushed[0], FString(TEXT("mat_a")));
		TestEqual(TEXT("后来的后处理"), Rig.Pushed[1], FString(TEXT("mat_b")));
	}
	TestEqual(TEXT("两条记进待确认"), Rig.Session->GetNumPendingAcks(), 2);
	TestEqual(TEXT("这一次心跳本身没带 ack"), AcksIn(Rig.Backend->LastHeartbeatBody()).Num(), 0);

	// 下一次心跳确认它们，后端据此删掉。
	Rig.Session->Pulse();
	const TArray<FString> Acks = AcksIn(Rig.Backend->LastHeartbeatBody());
	if (TestEqual(TEXT("下一次心跳带两个 ack"), Acks.Num(), 2))
	{
		TestEqual(TEXT("ack 的是 d1"), Acks[0], FString(TEXT("d1")));
		TestEqual(TEXT("ack 的是 d2"), Acks[1], FString(TEXT("d2")));
	}
	TestEqual(TEXT("后端那边清空了"), Rig.Backend->PendingItems.Num(), 0);
	TestEqual(TEXT("确认成功后待确认清空"), Rig.Session->GetNumPendingAcks(), 0);
	TestEqual(TEXT("没有重复处理"), Rig.Pushed.Num(), 2);

	// 再一次心跳：没有要确认的，也就不带请求体。
	Rig.Session->Pulse();
	TestTrue(TEXT("没有待确认时请求体为空"), Rig.Backend->LastHeartbeatBody().IsEmpty());

	// 确认的请求没送到（超时）：ack 留着，下一次照带，直到成功。
	Rig.Backend->PendingItems.Add({ TEXT("d3"), TEXT("mat_c") });
	Rig.Session->Pulse();
	TestEqual(TEXT("d3 处理了"), Rig.Pushed.Num(), 3);
	Rig.Backend->bHeartbeatTransportFail = true;
	Rig.Session->Pulse();
	TestEqual(TEXT("确认没送到，ack 还留着"), Rig.Session->GetNumPendingAcks(), 1);
	Rig.Backend->bHeartbeatTransportFail = false;
	Rig.Session->Pulse();
	TestEqual(TEXT("补发的确认带着 d3"), AcksIn(Rig.Backend->LastHeartbeatBody()).Num(), 1);
	TestEqual(TEXT("这回成功，后端清掉了 d3"), Rig.Backend->PendingItems.Num(), 0);
	TestEqual(TEXT("待确认清空"), Rig.Session->GetNumPendingAcks(), 0);

	// 无效的 material_id 也要确认掉（否则后端 10 分钟里每次心跳都补一遍），但不触发导入。
	Rig.Backend->PendingItems.Add({ TEXT("d4"), TEXT("../evil") });
	Rig.Session->Pulse();
	TestEqual(TEXT("无效 id 不触发导入"), Rig.Pushed.Num(), 3);
	Rig.Session->Pulse();
	TestEqual(TEXT("但被确认了"), Rig.Backend->PendingItems.Num(), 0);

	// 重连时的确认心跳同样会带回 pending：断线期间发的就是这样补上的。
	Rig.Stale();
	Rig.Backend->PendingItems.Add({ TEXT("d5"), TEXT("mat_during_outage") });
	TestEqual(TEXT("断线期间没处理"), Rig.Pushed.Num(), 3);
	Rig.Live();
	if (TestEqual(TEXT("重连的确认心跳把断线期间的推送补上"), Rig.Pushed.Num(), 4))
	{
		TestEqual(TEXT("补的是断线期间发的那套"), Rig.Pushed[3], FString(TEXT("mat_during_outage")));
	}

	// 注册成功后立刻心跳：被替换的旧会话带过来的投递靠它补上。
	{
		FRig Fresh;
		Fresh.Backend->PendingItems.Add({ TEXT("carried"), TEXT("mat_carried") });
		Fresh.Live();
		TestEqual(TEXT("注册后的那次心跳带回携带过来的投递"), Fresh.Pushed.Num(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTexturePushDedupeTest, "DreamTexture.Client.Push.Dedupe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamTexturePushDedupeTest::RunTest(const FString& Parameters)
{
	// ---- WS 先到，心跳随后又给一遍：只处理一次，确认只带一次 ----
	{
		FRig Rig;
		Rig.Live();
		Rig.Session->HandleEvent(MakeDeliveryEvent(TEXT("ue_1"), TEXT("mat_a"), TEXT("d1")));
		TestEqual(TEXT("WS 事件处理一次"), Rig.Pushed.Num(), 1);
		TestEqual(TEXT("记进待确认"), Rig.Session->GetNumPendingAcks(), 1);

		// 后端攒着 d1 且这次不删它（确认送到了但后端没删成）：心跳应答里 d1 又出现了。
		Rig.Backend->PendingItems.Add({ TEXT("d1"), TEXT("mat_a") });
		Rig.Backend->bIgnoreAcks = true;
		Rig.Session->Pulse();
		TestEqual(TEXT("心跳再给一遍不重复处理"), Rig.Pushed.Num(), 1);
		TestEqual(TEXT("这次心跳带了 d1 的 ack"), AcksIn(Rig.Backend->LastHeartbeatBody()).Num(), 1);
		TestEqual(TEXT("后端还列着它，所以重新进了待确认"), Rig.Session->GetNumPendingAcks(), 1);

		Rig.Backend->bIgnoreAcks = false;
		Rig.Session->Pulse();
		TestEqual(TEXT("这回后端删了"), Rig.Backend->PendingItems.Num(), 0);
		TestEqual(TEXT("待确认清空"), Rig.Session->GetNumPendingAcks(), 0);
		TestEqual(TEXT("全程只处理过一次"), Rig.Pushed.Num(), 1);
	}

	// ---- 心跳先到，WS 事件随后又来：同样只处理一次 ----
	{
		FRig Rig;
		Rig.Live();
		Rig.Backend->PendingItems.Add({ TEXT("d2"), TEXT("mat_b") });
		Rig.Session->Pulse();
		TestEqual(TEXT("心跳先处理"), Rig.Pushed.Num(), 1);
		Rig.Session->HandleEvent(MakeDeliveryEvent(TEXT("ue_1"), TEXT("mat_b"), TEXT("d2")));
		TestEqual(TEXT("WS 事件迟到不重复处理"), Rig.Pushed.Num(), 1);
		TestEqual(TEXT("待确认里只有一份"), Rig.Session->GetNumPendingAcks(), 1);
	}

	// ---- 不同 delivery_id 的同一套材质：各处理一次（用户确实发了两次）----
	{
		FRig Rig;
		Rig.Live();
		Rig.Session->HandleEvent(MakeDeliveryEvent(TEXT("ue_1"), TEXT("mat_a"), TEXT("d1")));
		Rig.Session->HandleEvent(MakeDeliveryEvent(TEXT("ue_1"), TEXT("mat_a"), TEXT("d2")));
		TestEqual(TEXT("两次投递各一次"), Rig.Pushed.Num(), 2);
	}

	// ---- 别的会话的投递不记 ----
	{
		FRig Rig;
		Rig.Live();
		Rig.Session->HandleEvent(MakeDeliveryEvent(TEXT("ue_other"), TEXT("mat_a"), TEXT("d1")));
		TestEqual(TEXT("别的会话的不处理"), Rig.Pushed.Num(), 0);
		TestEqual(TEXT("也不进待确认"), Rig.Session->GetNumPendingAcks(), 0);
		TestEqual(TEXT("也不进去重集合"), Rig.Session->GetNumTrackedDeliveries(), 0);
	}

	// ---- 去重集合有上限：最近 256 个 ----
	{
		FRig Rig;
		Rig.Live();
		const int32 Cap = FDreamTexturePushSession::MaxTrackedDeliveries;
		for (int32 i = 0; i < Cap + 50; ++i)
		{
			Rig.Session->HandleEvent(MakeDeliveryEvent(TEXT("ue_1"), TEXT("mat_a"), FString::Printf(TEXT("d%d"), i)));
		}
		TestEqual(TEXT("都处理了"), Rig.Pushed.Num(), Cap + 50);
		TestEqual(TEXT("去重集合不超过上限"), Rig.Session->GetNumTrackedDeliveries(), Cap);
		TestTrue(TEXT("待确认也不超过上限"), Rig.Session->GetNumPendingAcks() <= Cap);

		// 最近的仍在去重范围内；最老的已被挤出（再来就当新的）。
		Rig.Session->HandleEvent(MakeDeliveryEvent(TEXT("ue_1"), TEXT("mat_a"), FString::Printf(TEXT("d%d"), Cap + 49)));
		TestEqual(TEXT("最近的重复被去重"), Rig.Pushed.Num(), Cap + 50);
		Rig.Session->HandleEvent(MakeDeliveryEvent(TEXT("ue_1"), TEXT("mat_a"), TEXT("d0")));
		TestEqual(TEXT("早已挤出的最老记录不再去重"), Rig.Pushed.Num(), Cap + 51);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTexturePushLegacyTest, "DreamTexture.Client.Push.Legacy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamTexturePushLegacyTest::RunTest(const FString& Parameters)
{
	// 第一版后端：心跳回 204，事件没有 delivery_id。
	FRig Rig;
	Rig.Backend->bLegacyHeartbeat204 = true;
	Rig.Live();
	TestEqual(TEXT("已注册"), Rig.Session->GetState(), EDreamTexturePushState::Registered);

	Rig.Session->Pulse();
	TestEqual(TEXT("204 照常算成功"), Rig.Session->GetState(), EDreamTexturePushState::Registered);
	TestTrue(TEXT("204 没有错误"), Rig.Session->GetLastError().IsEmpty());
	TestEqual(TEXT("204 没有 pending 可处理"), Rig.Pushed.Num(), 0);

	// 旧事件：没有 delivery_id，每条都处理一次，无从去重也无从确认。
	Rig.Session->HandleEvent(MakeImportEvent(TEXT("ue_1"), TEXT("mat_a")));
	Rig.Session->HandleEvent(MakeImportEvent(TEXT("ue_1"), TEXT("mat_a")));
	TestEqual(TEXT("旧事件各处理一次"), Rig.Pushed.Num(), 2);
	TestEqual(TEXT("没有 delivery_id 就没有待确认"), Rig.Session->GetNumPendingAcks(), 0);
	TestEqual(TEXT("也不进去重集合"), Rig.Session->GetNumTrackedDeliveries(), 0);

	Rig.Session->Pulse();
	TestTrue(TEXT("心跳仍不带请求体（旧后端第一版只认无体的）"), Rig.Backend->LastHeartbeatBody().IsEmpty());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTexturePushOddReplyTest, "DreamTexture.Client.Push.OddReply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamTexturePushOddReplyTest::RunTest(const FString& Parameters)
{
	// 心跳 200 的应答体各种不规矩的样子：都当没有 pending，不炸、不丢会话。
	for (const TCHAR* Body : { TEXT(""), TEXT("{}"), TEXT("{\"pending\":null}"), TEXT("{\"pending\":[]}"), TEXT("not json"),
		TEXT("{\"pending\":[{\"material_id\":\"mat_a\"}]}"), TEXT("{\"pending\":[null,5,\"x\"]}") })
	{
		TSharedRef<FFakeSessionBackend> Backend = MakeShared<FFakeSessionBackend>();
		FDreamTexturePushSession::FHooks Hooks = Backend->MakeHooks(Backend);
		const FString OddBody = Body;
		const auto BaseSend = Hooks.Send;
		Hooks.Send = [BaseSend, OddBody](const FString& Verb, const FString& Path, const FString& JsonBody, float Timeout, FOnReply Done)
		{
			BaseSend(Verb, Path, JsonBody, Timeout, [Done, Path, OddBody](const FDreamTextureSessionReply& Reply)
			{
				FDreamTextureSessionReply Out = Reply;
				if (Path.EndsWith(TEXT("/heartbeat")) && Reply.StatusCode == 200)
				{
					Out.Body = OddBody;
				}
				Done(Out);
			});
		};

		const TSharedRef<FDreamTexturePushSession> Session = MakeShared<FDreamTexturePushSession>(Hooks);
		int32 Pushed = 0;
		Session->OnImportPushed.AddLambda([&Pushed](const FString&) { ++Pushed; });
		Session->HandleLinkChanged(EDreamTextureLink::Live);
		Session->Pulse();
		TestEqual(*FString::Printf(TEXT("应答体 %s：仍是已注册"), Body), Session->GetState(), EDreamTexturePushState::Registered);
		TestEqual(*FString::Printf(TEXT("应答体 %s：没有触发导入"), Body), Pushed, 0);
	}
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 订阅真实的连接

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTexturePushBindTest, "DreamTexture.Client.Push.Bind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamTexturePushBindTest::RunTest(const FString& Parameters)
{
	// 连接没有 Start()，不会真的拨号；委托直接广播，等价于它收到了事件 / 链路变了。
	const TSharedRef<FDreamTextureConnection> Connection = MakeShared<FDreamTextureConnection>();
	FRig Rig;
	Rig.Session->Bind(Connection);

	Connection->OnLinkChanged.Broadcast(EDreamTextureLink::Live);
	TestEqual(TEXT("连接变 Live 触发注册"), Rig.Session->GetState(), EDreamTexturePushState::Registered);

	Connection->OnEvent.Broadcast(MakeImportEvent(TEXT("ue_1"), TEXT("mat_a")));
	Connection->OnEvent.Broadcast(MakeImportEvent(TEXT("ue_9"), TEXT("mat_b")));
	if (TestEqual(TEXT("连接的 OnEvent 经过滤只转发给自己的"), Rig.Pushed.Num(), 1))
	{
		TestEqual(TEXT("转发的是自己的那条"), Rig.Pushed[0], FString(TEXT("mat_a")));
	}

	Connection->OnLinkChanged.Broadcast(EDreamTextureLink::Stale);
	TestEqual(TEXT("连接失联后不再是已注册"), Rig.Session->GetState(), EDreamTexturePushState::Unregistered);

	// 解绑之后连接上的任何变化都与它无关。
	Rig.Session->Unbind();
	const int32 Before = Rig.Backend->Calls.Num();
	Connection->OnLinkChanged.Broadcast(EDreamTextureLink::Live);
	Connection->OnEvent.Broadcast(MakeImportEvent(TEXT("ue_1"), TEXT("mat_c")));
	TestEqual(TEXT("解绑后不再响应链路"), Rig.Backend->Calls.Num(), Before);
	TestEqual(TEXT("解绑后不再转事件"), Rig.Pushed.Num(), 1);
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 真的定时器

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTexturePushTickerTest, "DreamTexture.Client.Push.Ticker",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamTexturePushTickerTest::RunTest(const FString& Parameters)
{
	// 把间隔缩到 50 毫秒，等定时器自己心跳几次。Pulse() 之外要证明的是：Bind 真的把定时器挂上了，
	// 而且 Unbind 之后它不再跳（也就不会在回调里摘自己）。
	struct FState
	{
		TSharedRef<FFakeSessionBackend> Backend = MakeShared<FFakeSessionBackend>();
		TSharedPtr<FDreamTexturePushSession> Session;
		TSharedPtr<FDreamTextureConnection> Connection;
		int32 AfterUnbind = 0;
	};
	const TSharedRef<FState> State = MakeShared<FState>();
	State->Connection = MakeShared<FDreamTextureConnection>();
	State->Session = MakeShared<FDreamTexturePushSession>(State->Backend->MakeHooks(State->Backend, 0.05f));
	State->Session->Bind(State->Connection);
	State->Connection->OnLinkChanged.Broadcast(EDreamTextureLink::Live);
	TestEqual(TEXT("先注册"), State->Session->GetState(), EDreamTexturePushState::Registered);

	ADD_LATENT_AUTOMATION_COMMAND(FDreamTextureWaitCommand(this, TEXT("定时器自己发出至少 3 次心跳"), 10.0,
		[State]() { return State->Backend->CountHeartbeats() >= 3; }));

	ADD_LATENT_AUTOMATION_COMMAND(FDreamTextureDoCommand([this, State]()
	{
		TestEqual(TEXT("定时器心跳没有重新注册"), State->Backend->CountRegisters(), 1);
		State->Session->Unbind();
		State->AfterUnbind = State->Backend->Calls.Num();
	}));

	// 解绑后再等一会儿（足够定时器跳好几次），不该再有请求。
	ADD_LATENT_AUTOMATION_COMMAND(FDreamTexturePushSleepCommand(0.4));
	ADD_LATENT_AUTOMATION_COMMAND(FDreamTextureDoCommand([this, State]()
	{
		TestEqual(TEXT("解绑后定时器不再发请求"), State->Backend->Calls.Num(), State->AfterUnbind);
	}));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
