// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureClientModule.h"
#include "DreamTextureConnection.h"
#include "DreamTextureImportQueue.h"
#include "DreamTexturePushReceiver.h"
#include "DreamTexturePushSession.h"
#include "DreamTextureSettings.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "TextureCompiler.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

/*
 * DreamTexture.Live.Push：Web 端「发送到 UE」的整条通路，对着真后端走一遍。
 *
 *   会话注册成功 → GET /api/ue/sessions 能看到自己 → 自己调 POST /api/materials/{id}/send-to-ue
 *   → 收到 ue.import（会话的 OnImportPushed）→ 导入队列走到 Succeeded，资产落在唯一的测试目录。
 *
 * 然后是第二版的兜底：把长连接停掉（会话留着，心跳随之暂停），断线期间再 send-to-ue，
 * 事件发出去时没人收；恢复连接后，重连的确认心跳应答里的 pending 把它补上，
 * 在一个心跳周期内导入、且只导入一次（之后的 ack 让后端把这笔投递清掉）。
 *
 * 用的是编辑器模块里那个全局会话，它订阅的是测试编辑器的全局连接（Tools/Test.ps1 把它指到固定数据后端）。
 *
 * 后端要带推送接口（M6）。旧后端上会话会停在 Unsupported，此时跳过并说明，不算失败。
 * 要测新后端：DREAMTEXTURE_BACKEND_EXE=<新后端的 dreamtexture.exe> 再跑 Tools/Test.ps1 -Live。
 */

namespace
{
	/** 一次异步 HTTP 往返。放在共享指针里：请求可能在测试收尾之后才回来。 */
	struct FPushHttpResult
	{
		bool bDone = false;
		bool bGotResponse = false;
		int32 Code = 0;
		FString Body;
	};

	void FirePushRequest(const FString& LiveURL, const TCHAR* Verb, const FString& Path, const FString& JsonBody, const TSharedRef<FPushHttpResult>& Out)
	{
		const FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(LiveURL + Path);
		Request->SetVerb(Verb);
		Request->SetTimeout(10.0f);
		if (!JsonBody.IsEmpty())
		{
			Request->SetHeader(TEXT("Content-Type"), TEXT("application/json; charset=utf-8"));
			Request->SetContentAsString(JsonBody);
		}
		Request->OnProcessRequestComplete().BindLambda([Out](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
		{
			Out->bGotResponse = bSucceeded && Response.IsValid();
			Out->Code = Out->bGotResponse ? Response->GetResponseCode() : 0;
			Out->Body = Out->bGotResponse ? Response->GetContentAsString() : FString();
			Out->bDone = true;
		});
		if (!Request->ProcessRequest())
		{
			// 5.8 里发不出去时委托可能已被同步调过，bDone 已置位；没置位就补上。
			if (!Out->bDone)
			{
				Out->bDone = true;
			}
		}
	}

	TSharedPtr<FJsonObject> ParsePushJson(const FString& Text)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, Root) ? Root : nullptr;
	}

	class FPushDoCommand : public IAutomationLatentCommand
	{
	public:
		explicit FPushDoCommand(TFunction<void()> InAction) : Action(MoveTemp(InAction)) {}

		virtual bool Update() override
		{
			if (Action)
			{
				Action();
			}
			return true;
		}

	private:
		TFunction<void()> Action;
	};

	class FPushWaitCommand : public IAutomationLatentCommand
	{
	public:
		FPushWaitCommand(FAutomationTestBase* InTest, FString InWhat, double InTimeoutSecs, TFunction<bool()> InIsDone, TFunction<bool()> InSkip)
			: Test(InTest), What(MoveTemp(InWhat)), TimeoutSecs(InTimeoutSecs), IsDone(MoveTemp(InIsDone)), Skip(MoveTemp(InSkip))
		{
		}

		virtual bool Update() override
		{
			// 前面已经中止（跳过或失败）：不用再等了。
			if (Skip && Skip())
			{
				return true;
			}
			if (IsDone())
			{
				return true;
			}
			if (GetCurrentRunTime() > TimeoutSecs)
			{
				Test->AddError(FString::Printf(TEXT("等待超时（%.0f 秒）：%s"), TimeoutSecs, *What));
				return true;
			}
			return false;
		}

	private:
		FAutomationTestBase* Test;
		FString What;
		double TimeoutSecs;
		TFunction<bool()> IsDone;
		TFunction<bool()> Skip;
	};

	struct FPushLiveState
	{
		FString LiveURL;
		FString MaterialId = TEXT("mat_20260827_777001_ref");

		// 唯一的测试导入目录，测完连包带文件清掉。
		FString Root = TEXT("/Game/DreamTextureTests/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		FString SavedRoot;
		bool bRootOverridden = false;

		TSharedPtr<FDreamTexturePushSession> Session;
		TSharedPtr<FDreamTexturePushReceiver> Receiver;
		FDelegateHandle PushedHandle;
		TArray<FString> Pushed;
		TArray<FText> Notices;

		bool bAbort = false;

		// 断线补投递那一段
		TSharedPtr<FDreamTextureConnection> Connection;
		bool bConnectionStopped = false;
		double RestartedAt = 0.0;
		double PushedAfterSecs = -1.0;
		TSharedRef<FPushHttpResult> SendOutage = MakeShared<FPushHttpResult>();
		TSharedRef<FPushHttpResult> RawHeartbeat = MakeShared<FPushHttpResult>();

		TSharedRef<FPushHttpResult> Sessions = MakeShared<FPushHttpResult>();
		TSharedRef<FPushHttpResult> Send = MakeShared<FPushHttpResult>();
		TSharedRef<FPushHttpResult> SendGone = MakeShared<FPushHttpResult>();
		TSharedRef<FPushHttpResult> SendNoMaterial = MakeShared<FPushHttpResult>();

		bool IsSettled() const
		{
			return Session.IsValid() && (Session->GetState() == EDreamTexturePushState::Registered || Session->GetState() == EDreamTexturePushState::Unsupported);
		}

		bool IsImportFinished() const
		{
			const FDreamTextureImportStatus* Status = FDreamTextureImportQueue::Get().FindStatus(MaterialId);
			return Status != nullptr && Status->IsFinished();
		}

		/** 摘订阅、还原设置、删测试资产。可重复调。 */
		void End()
		{
			// 中途失败时别把测试编辑器的全局连接留在停掉的状态。Start() 对已在跑的是空操作。
			if (bConnectionStopped && Connection.IsValid())
			{
				Connection->Start();
				bConnectionStopped = false;
			}
			if (Receiver.IsValid())
			{
				Receiver->Stop();
				Receiver.Reset();
			}
			if (Session.IsValid() && PushedHandle.IsValid())
			{
				Session->OnImportPushed.Remove(PushedHandle);
				PushedHandle.Reset();
			}
			if (bRootOverridden)
			{
				GetMutableDefault<UDreamTextureSettings>()->ImportRoot = SavedRoot;
				bRootOverridden = false;
			}
			FDreamTextureImportQueue::Get().Shutdown();
			Cleanup();
		}

		void Cleanup()
		{
			// 贴图的异步编译还在读它们的源数据，删之前必须等完。
			FTextureCompilingManager::Get().FinishAllCompilation();

			TArray<UObject*> ToDelete;
			const FString Prefix = Root + TEXT("/");
			for (TObjectIterator<UPackage> It; It; ++It)
			{
				UPackage* Package = *It;
				if (Package->GetName().StartsWith(Prefix))
				{
					ForEachObjectWithPackage(Package, [&ToDelete](UObject* Object)
					{
						if (Object->IsAsset())
						{
							ToDelete.Add(Object);
						}
						return true;
					}, EGetObjectsFlags::None);
				}
			}
			if (ToDelete.Num() > 0)
			{
				ObjectTools::DeleteObjectsUnchecked(ToDelete);
			}

			const FString DiskDir = FPackageName::LongPackageNameToFilename(Root);
			IFileManager::Get().DeleteDirectory(*DiskDir, /*RequireExists*/ false, /*Tree*/ true);
			IFileManager::Get().DeleteDirectory(*FPaths::GetPath(DiskDir), /*RequireExists*/ false, /*Tree*/ false);
			if (FAssetRegistryModule* Module = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry")))
			{
				Module->Get().ScanPathsSynchronous({ Root }, /*bForceRescan*/ true);
			}
		}

		~FPushLiveState()
		{
			End();
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTexturePushLiveTest, "DreamTexture.Live.Push",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTexturePushLiveTest::RunTest(const FString& Parameters)
{
	const FString LiveURL = FPlatformMisc::GetEnvironmentVariable(TEXT("DREAMTEXTURE_LIVE_URL")).TrimStartAndEnd();
	if (LiveURL.IsEmpty())
	{
		AddInfo(TEXT("已跳过：没有设置环境变量 DREAMTEXTURE_LIVE_URL。需要先起固定数据后端，Tools/Test.ps1 -Live 会替你做。"
			"推送接口要新后端：另设 DREAMTEXTURE_BACKEND_EXE=<新后端的 dreamtexture.exe>（或写进 Tools/LocalSettings.psd1 的 BackendExe）。"));
		return true;
	}
	if (!FDreamTextureClientModule::IsAvailable() || !FDreamTextureClientModule::Get().GetPushSession().IsValid())
	{
		AddError(TEXT("客户端模块没有推送会话"));
		return false;
	}

	const TSharedRef<FPushLiveState> State = MakeShared<FPushLiveState>();
	State->LiveURL = LiveURL;
	State->Session = FDreamTextureClientModule::Get().GetPushSession();

	// ---- 1. 会话注册成功（或后端不支持 → 跳过）----
	ADD_LATENT_AUTOMATION_COMMAND(FPushWaitCommand(this, TEXT("会话注册成功或判定后端不支持"), 40.0,
		[State]() { return State->IsSettled(); }, nullptr));

	ADD_LATENT_AUTOMATION_COMMAND(FPushDoCommand([this, State]()
	{
		if (State->Session->GetState() == EDreamTexturePushState::Unsupported)
		{
			AddInfo(FString::Printf(TEXT("已跳过：后端不支持推送会话（%s）。设 DREAMTEXTURE_BACKEND_EXE 指向带 M6 的新后端再跑。"), *State->Session->GetLastError()));
			State->bAbort = true;
			return;
		}
		if (!TestEqual(TEXT("会话已注册"), State->Session->GetState(), EDreamTexturePushState::Registered))
		{
			State->bAbort = true;
			return;
		}
		TestEqual(TEXT("会话名是工程名"), State->Session->GetSessionName(), FString(FApp::GetProjectName()));
		TestFalse(TEXT("有会话 id"), State->Session->GetSessionId().IsEmpty());

		// ---- 2. GET /api/ue/sessions 看到自己 ----
		FirePushRequest(State->LiveURL, TEXT("GET"), TEXT("/api/ue/sessions"), FString(), State->Sessions);
	}));

	ADD_LATENT_AUTOMATION_COMMAND(FPushWaitCommand(this, TEXT("GET /api/ue/sessions 应答"), 15.0,
		[State]() { return State->Sessions->bDone; }, [State]() { return State->bAbort; }));

	ADD_LATENT_AUTOMATION_COMMAND(FPushDoCommand([this, State]()
	{
		if (State->bAbort)
		{
			return;
		}
		TestEqual(TEXT("GET /api/ue/sessions 返回 200"), State->Sessions->Code, 200);
		bool bFound = false;
		const TSharedPtr<FJsonObject> Root = ParsePushJson(State->Sessions->Body);
		const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
		if (TestTrue(TEXT("应答是带 sessions 数组的 JSON"), Root.IsValid() && Root->TryGetArrayField(TEXT("sessions"), List) && List != nullptr))
		{
			for (const TSharedPtr<FJsonValue>& Value : *List)
			{
				const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
				FString Id;
				if (Entry.IsValid() && Entry->TryGetStringField(TEXT("session_id"), Id) && Id.Equals(State->Session->GetSessionId(), ESearchCase::CaseSensitive))
				{
					bFound = true;
					TestEqual(TEXT("列表里的 name 是工程名"), Entry->GetStringField(TEXT("name")), FString(FApp::GetProjectName()));
					TestFalse(TEXT("列表里有 engine"), Entry->GetStringField(TEXT("engine")).IsEmpty());
					TestFalse(TEXT("列表里有 project"), Entry->GetStringField(TEXT("project")).IsEmpty());
				}
			}
		}
		TestTrue(TEXT("在线会话列表里有自己"), bFound);

		// ---- 3. 准备导入：唯一目录，订阅推送 ----
		FDreamTextureImportQueue::Get().Shutdown();
		UDreamTextureSettings* Settings = GetMutableDefault<UDreamTextureSettings>();
		State->SavedRoot = Settings->ImportRoot;
		Settings->ImportRoot = State->Root;
		State->bRootOverridden = true;

		State->PushedHandle = State->Session->OnImportPushed.AddLambda([State](const FString& Id) { State->Pushed.Add(Id); });

		// 通知换成记录：测试里不弹窗；排队走真的导入队列。
		FDreamTexturePushReceiver::FHooks Hooks = FDreamTexturePushReceiver::FHooks::MakeDefault();
		Hooks.ShowNotice = [State](const FText& Message) { State->Notices.Add(Message); };
		State->Receiver = MakeShared<FDreamTexturePushReceiver>(Hooks);
		State->Receiver->Start(State->Session);

		// ---- 4. 反例先走：发给不存在的会话 → 409；不存在的材质 → 404。都不该推出任何东西 ----
		FirePushRequest(State->LiveURL, TEXT("POST"), FString::Printf(TEXT("/api/materials/%s/send-to-ue"), *State->MaterialId),
			TEXT("{\"session_id\":\"ue_nope\"}"), State->SendGone);
		FirePushRequest(State->LiveURL, TEXT("POST"), TEXT("/api/materials/mat_does_not_exist/send-to-ue"),
			FString::Printf(TEXT("{\"session_id\":\"%s\"}"), *State->Session->GetSessionId()), State->SendNoMaterial);
	}));

	ADD_LATENT_AUTOMATION_COMMAND(FPushWaitCommand(this, TEXT("两个反例应答"), 15.0,
		[State]() { return State->SendGone->bDone && State->SendNoMaterial->bDone; }, [State]() { return State->bAbort; }));

	ADD_LATENT_AUTOMATION_COMMAND(FPushDoCommand([this, State]()
	{
		if (State->bAbort)
		{
			return;
		}
		TestEqual(TEXT("发给不在线的会话 → 409"), State->SendGone->Code, 409);
		TestTrue(TEXT("409 带 session_gone"), State->SendGone->Body.Contains(TEXT("session_gone")));
		TestEqual(TEXT("材质不存在 → 404"), State->SendNoMaterial->Code, 404);

		// ---- 5. 正例：发给自己 ----
		FirePushRequest(State->LiveURL, TEXT("POST"), FString::Printf(TEXT("/api/materials/%s/send-to-ue"), *State->MaterialId),
			FString::Printf(TEXT("{\"session_id\":\"%s\"}"), *State->Session->GetSessionId()), State->Send);
	}));

	ADD_LATENT_AUTOMATION_COMMAND(FPushWaitCommand(this, TEXT("send-to-ue 应答"), 15.0,
		[State]() { return State->Send->bDone; }, [State]() { return State->bAbort; }));

	ADD_LATENT_AUTOMATION_COMMAND(FPushWaitCommand(this, TEXT("收到 ue.import"), 15.0,
		[State]() { return State->Pushed.Num() > 0; }, [State]() { return State->bAbort; }));

	ADD_LATENT_AUTOMATION_COMMAND(FPushDoCommand([this, State]()
	{
		if (State->bAbort)
		{
			return;
		}
		TestEqual(TEXT("send-to-ue 返回 202"), State->Send->Code, 202);
		if (TestEqual(TEXT("只收到一条推送（反例没有推出东西）"), State->Pushed.Num(), 1))
		{
			TestEqual(TEXT("推来的是这套材质"), State->Pushed[0], State->MaterialId);
		}
		if (TestTrue(TEXT("弹过「Web 端发来」通知"), State->Notices.Num() >= 1))
		{
			TestTrue(TEXT("通知带材质 id"), State->Notices[0].ToString().Contains(State->MaterialId));
		}
		TestTrue(TEXT("已排入导入队列"), FDreamTextureImportQueue::Get().FindStatus(State->MaterialId) != nullptr);
	}));

	// ---- 6. 导入走到终态，成功 ----
	ADD_LATENT_AUTOMATION_COMMAND(FPushWaitCommand(this, TEXT("推送触发的导入走到终态"), 120.0,
		[State]() { return State->IsImportFinished(); }, [State]() { return State->bAbort; }));

	ADD_LATENT_AUTOMATION_COMMAND(FPushDoCommand([this, State]()
	{
		if (State->bAbort)
		{
			return;
		}
		const FDreamTextureImportStatus* Status = FDreamTextureImportQueue::Get().FindStatus(State->MaterialId);
		if (TestNotNull(TEXT("导入有状态"), Status))
		{
			TestTrue(FString::Printf(TEXT("推送触发的导入成功（%s / %s）"), *Status->Message, *Status->Result.Error),
				Status->Stage == EDreamTextureImportStage::Succeeded && Status->Result.bOK);
			TestTrue(TEXT("资产落在唯一的测试目录下"), Status->Result.AssetFolder.StartsWith(State->Root));
			TestTrue(TEXT("有导入的贴图"), Status->Result.Textures.Num() > 0);
		}
	}));

	// ---- 7. 断线期间的推送在重连后补到 ----
	ADD_LATENT_AUTOMATION_COMMAND(FPushDoCommand([this, State]()
	{
		if (State->bAbort)
		{
			return;
		}
		State->Connection = FDreamTextureClientModule::Get().GetConnection();
		if (!TestTrue(TEXT("有全局连接"), State->Connection.IsValid()))
		{
			State->bAbort = true;
			return;
		}

		// 停掉长连接：WS 关了，会话保留（只暂停心跳），后端那边 45 秒内还认它。
		State->Pushed.Reset();
		State->Notices.Reset();
		State->Connection->Stop();
		State->bConnectionStopped = true;
		TestEqual(TEXT("连接停掉后会话不再算已注册（心跳暂停）"), State->Session->GetState(), EDreamTexturePushState::Unregistered);
		TestFalse(TEXT("会话 id 留着"), State->Session->GetSessionId().IsEmpty());

		// 断线期间 Web 端点了发送：后端照样 202，事件发出去时没有 WS 订阅者收。
		FirePushRequest(State->LiveURL, TEXT("POST"), FString::Printf(TEXT("/api/materials/%s/send-to-ue"), *State->MaterialId),
			FString::Printf(TEXT("{\"session_id\":\"%s\"}"), *State->Session->GetSessionId()), State->SendOutage);
	}));

	ADD_LATENT_AUTOMATION_COMMAND(FPushWaitCommand(this, TEXT("断线期间 send-to-ue 应答"), 15.0,
		[State]() { return State->SendOutage->bDone; }, [State]() { return State->bAbort; }));

	ADD_LATENT_AUTOMATION_COMMAND(FPushDoCommand([this, State]()
	{
		if (State->bAbort)
		{
			return;
		}
		TestEqual(TEXT("断线期间发送照样 202"), State->SendOutage->Code, 202);
		const TSharedPtr<FJsonObject> Root = ParsePushJson(State->SendOutage->Body);
		FString DeliveryId;
		TestTrue(TEXT("应答里有 delivery_id"), Root.IsValid() && Root->TryGetStringField(TEXT("delivery_id"), DeliveryId) && !DeliveryId.IsEmpty());
		TestEqual(TEXT("断线期间什么都没收到"), State->Pushed.Num(), 0);

		// 恢复连接。计时从这一刻起：重连成功（变 Live）就发确认心跳，应答里带着 pending。
		State->RestartedAt = FPlatformTime::Seconds();
		State->Connection->Start();
		State->bConnectionStopped = false;
	}));

	ADD_LATENT_AUTOMATION_COMMAND(FPushWaitCommand(this, TEXT("重连后补到断线期间的推送"), 30.0,
		[State]()
		{
			if (State->Pushed.Num() > 0 && State->PushedAfterSecs < 0.0)
			{
				State->PushedAfterSecs = FPlatformTime::Seconds() - State->RestartedAt;
			}
			return State->Pushed.Num() > 0;
		}, [State]() { return State->bAbort; }));

	ADD_LATENT_AUTOMATION_COMMAND(FPushWaitCommand(this, TEXT("补投递触发的导入走到终态"), 120.0,
		[State]() { return State->IsImportFinished(); }, [State]() { return State->bAbort; }));

	ADD_LATENT_AUTOMATION_COMMAND(FPushDoCommand([this, State]()
	{
		if (State->bAbort)
		{
			return;
		}
		TestEqual(TEXT("补到一次"), State->Pushed.Num(), 1);
		if (State->Pushed.Num() > 0)
		{
			TestEqual(TEXT("补的是断线期间发的那套"), State->Pushed[0], State->MaterialId);
		}
		TestTrue(FString::Printf(TEXT("在一个心跳周期内补上（实际 %.1f 秒）"), State->PushedAfterSecs),
			State->PushedAfterSecs >= 0.0 && State->PushedAfterSecs <= FDreamTexturePushSession::DefaultPulseIntervalSecs);
		TestTrue(TEXT("补投递弹过通知"), State->Notices.Num() >= 1);
		const FDreamTextureImportStatus* Status = FDreamTextureImportQueue::Get().FindStatus(State->MaterialId);
		if (TestNotNull(TEXT("补投递触发的导入有状态"), Status))
		{
			TestTrue(FString::Printf(TEXT("补投递触发的导入成功（%s / %s）"), *Status->Message, *Status->Result.Error),
				Status->Stage == EDreamTextureImportStage::Succeeded && Status->Result.bOK);
		}

		// 让下一次心跳把 ack 送出去（不等 15 秒的定时器，手动催一次）。
		State->Session->Pulse();
	}));

	ADD_LATENT_AUTOMATION_COMMAND(FPushWaitCommand(this, TEXT("确认（ack）被后端收下"), 15.0,
		[State]() { return State->Session->GetNumPendingAcks() == 0; }, [State]() { return State->bAbort; }));

	ADD_LATENT_AUTOMATION_COMMAND(FPushDoCommand([this, State]()
	{
		if (State->bAbort)
		{
			return;
		}
		// 自己再发一次不带 ack 的心跳，看后端的 pending 里是不是已经没有这笔：它被确认掉了，不会再补第二次。
		FirePushRequest(State->LiveURL, TEXT("POST"), FString::Printf(TEXT("/api/ue/sessions/%s/heartbeat"), *State->Session->GetSessionId()),
			FString(), State->RawHeartbeat);
	}));

	ADD_LATENT_AUTOMATION_COMMAND(FPushWaitCommand(this, TEXT("原始心跳应答"), 15.0,
		[State]() { return State->RawHeartbeat->bDone; }, [State]() { return State->bAbort; }));

	ADD_LATENT_AUTOMATION_COMMAND(FPushDoCommand([this, State]()
	{
		if (State->bAbort)
		{
			return;
		}
		TestEqual(TEXT("心跳 200（第二版）"), State->RawHeartbeat->Code, 200);
		const TSharedPtr<FJsonObject> Root = ParsePushJson(State->RawHeartbeat->Body);
		const TArray<TSharedPtr<FJsonValue>>* Pending = nullptr;
		if (TestTrue(TEXT("应答是带 pending 的 JSON"), Root.IsValid() && Root->TryGetArrayField(TEXT("pending"), Pending) && Pending != nullptr))
		{
			TestEqual(TEXT("确认之后后端的 pending 是空的"), Pending->Num(), 0);
		}
		TestEqual(TEXT("全程只导入触发过一次"), State->Pushed.Num(), 1);
	}));

	// 收尾放在最后一步：即使前面超时 / 跳过也会走到这里。
	ADD_LATENT_AUTOMATION_COMMAND(FPushDoCommand([State]()
	{
		State->End();
	}));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
