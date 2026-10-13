// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureClientModule.h"
#include "DreamTexturePushReceiver.h"
#include "DreamTexturePushSession.h"
#include "Framework/Application/SlateApplication.h"
#include "SDreamTextureStatusPanel.h"

/*
 * DreamTexture.Editor.Push.* —— 编辑器收到 Web 端推送之后的行为。
 *
 * 导入队列与通知都换成假的：断言「排了什么、弹了什么」，不真的下载、不真的弹窗。
 * 真正连后端的整条通路在 DreamTexture.Live.Push。
 */

namespace
{
	struct FPushReceiverRig
	{
		TArray<FString> Enqueued;
		TArray<FText> Notices;
		TSet<FString> Active;
		bool bEnqueueResult = true;

		FDreamTexturePushReceiver::FHooks MakeHooks()
		{
			FDreamTexturePushReceiver::FHooks Hooks;
			Hooks.IsActive = [this](const FString& Id) { return Active.Contains(Id); };
			Hooks.Enqueue = [this](const FString& Id)
			{
				Enqueued.Add(Id);
				return bEnqueueResult;
			};
			Hooks.ShowNotice = [this](const FText& Message) { Notices.Add(Message); };
			return Hooks;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTexturePushReceiveTest, "DreamTexture.Editor.Push.Receive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamTexturePushReceiveTest::RunTest(const FString& Parameters)
{
	// ---- 正常：排进队列，弹一条「Web 端发来一套材质」----
	{
		FPushReceiverRig Rig;
		const TSharedRef<FDreamTexturePushReceiver> Receiver = MakeShared<FDreamTexturePushReceiver>(Rig.MakeHooks());
		Receiver->HandlePushed(TEXT("mat_a"));

		if (TestEqual(TEXT("排了一次导入"), Rig.Enqueued.Num(), 1))
		{
			TestEqual(TEXT("排的是推来的那套"), Rig.Enqueued[0], FString(TEXT("mat_a")));
		}
		if (TestEqual(TEXT("弹了一条通知"), Rig.Notices.Num(), 1))
		{
			const FString Text = Rig.Notices[0].ToString();
			TestTrue(TEXT("通知说明是 Web 端发来的"), Text.Contains(TEXT("The web UI sent a material set")));
			TestTrue(TEXT("通知带材质 id"), Text.Contains(TEXT("mat_a")));
			TestTrue(TEXT("通知说在导入，结果另有通知"), Text.Contains(TEXT("is being imported")));
		}
		TestEqual(TEXT("计数"), Receiver->GetNumReceived(), 1);
	}

	// ---- 同一套已在导入：不重复排，通知照说在导入 ----
	{
		FPushReceiverRig Rig;
		Rig.Active.Add(TEXT("mat_a"));
		const TSharedRef<FDreamTexturePushReceiver> Receiver = MakeShared<FDreamTexturePushReceiver>(Rig.MakeHooks());
		Receiver->HandlePushed(TEXT("mat_a"));
		TestEqual(TEXT("已在队列里不重复排"), Rig.Enqueued.Num(), 0);
		if (TestEqual(TEXT("仍弹一条通知"), Rig.Notices.Num(), 1))
		{
			TestTrue(TEXT("说的是正在导入"), Rig.Notices[0].ToString().Contains(TEXT("is being imported")));
		}
	}

	// ---- 队列拒了：如实说没排进去 ----
	{
		FPushReceiverRig Rig;
		Rig.bEnqueueResult = false;
		const TSharedRef<FDreamTexturePushReceiver> Receiver = MakeShared<FDreamTexturePushReceiver>(Rig.MakeHooks());
		Receiver->HandlePushed(TEXT("mat_a"));
		if (TestEqual(TEXT("弹了通知"), Rig.Notices.Num(), 1))
		{
			TestTrue(TEXT("说没能排入"), Rig.Notices[0].ToString().Contains(TEXT("could not be queued")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTexturePushWireTest, "DreamTexture.Editor.Push.Wire",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamTexturePushWireTest::RunTest(const FString& Parameters)
{
	// 会话不绑连接、不发请求；直接广播它的委托，等价于它过滤出了一条发给自己的 ue.import。
	const TSharedRef<FDreamTexturePushSession> Session = MakeShared<FDreamTexturePushSession>();
	FPushReceiverRig Rig;
	const TSharedRef<FDreamTexturePushReceiver> Receiver = MakeShared<FDreamTexturePushReceiver>(Rig.MakeHooks());

	Receiver->Start(Session);
	Session->OnImportPushed.Broadcast(TEXT("mat_x"));
	TestEqual(TEXT("订阅后收到推送就排导入"), Rig.Enqueued.Num(), 1);

	Receiver->Stop();
	Session->OnImportPushed.Broadcast(TEXT("mat_y"));
	TestEqual(TEXT("Stop 之后不再响应"), Rig.Enqueued.Num(), 1);

	// 重复 Start 先解旧的：只会收一次，不会收两次。
	Receiver->Start(Session);
	Receiver->Start(Session);
	Session->OnImportPushed.Broadcast(TEXT("mat_z"));
	TestEqual(TEXT("重复 Start 不重复订阅"), Rig.Enqueued.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTexturePushPanelTest, "DreamTexture.Editor.Push.Panel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamTexturePushPanelTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("已跳过：没有 Slate（命令行 / -nullrhi），无法构造状态页控件。"));
		return true;
	}

	// 状态页带「推送」那一行：在会话状态变化的广播里重绘不崩。
	const TSharedRef<SDreamTextureStatusPanel> Panel = SNew(SDreamTextureStatusPanel);
	Panel->SlatePrepass(1.0f);

	if (FDreamTextureClientModule::IsAvailable())
	{
		if (const TSharedPtr<FDreamTexturePushSession> Push = FDreamTextureClientModule::Get().GetPushSession())
		{
			Push->OnStateChanged.Broadcast();
			Panel->SlatePrepass(1.0f);
		}
	}
	TestTrue(TEXT("状态页构造并重算布局后仍有效"), Panel->GetDesiredSize().X >= 0.0f);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
