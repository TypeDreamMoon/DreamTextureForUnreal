// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureImportQueue.h"
#include "DreamTextureSettings.h"
#include "DreamTextureTestUtils.h"

using namespace DreamTextureTestUtils;

namespace
{
	/**
	 * 一次队列测试的现场：订阅事件、临时改后端地址，离开时还原。
	 * 只改内存里的 CDO，不写任何 ini。
	 */
	struct FQueueTestSession
	{
		TArray<FDreamTextureImportStatus> Events;
		FDelegateHandle Handle;
		FString SavedURL;
		bool bOverridden = false;

		void Begin(const FString& BackendURL)
		{
			FDreamTextureImportQueue& Queue = FDreamTextureImportQueue::Get();
			// 上一个测试可能留下了没跑完的任务和还在路上的回调。
			Queue.Shutdown();

			UDreamTextureSettings* Settings = GetMutableDefault<UDreamTextureSettings>();
			SavedURL = Settings->BackendURL;
			Settings->BackendURL = BackendURL;
			bOverridden = true;

			Handle = Queue.OnStatusChanged.AddLambda([this](const FDreamTextureImportStatus& Status)
			{
				Events.Add(Status);
			});
		}

		void End()
		{
			if (Handle.IsValid())
			{
				FDreamTextureImportQueue::Get().OnStatusChanged.Remove(Handle);
				Handle.Reset();
			}
			if (bOverridden)
			{
				GetMutableDefault<UDreamTextureSettings>()->BackendURL = SavedURL;
				bOverridden = false;
			}
			FDreamTextureImportQueue::Get().Shutdown();
		}

		~FQueueTestSession()
		{
			End();
		}

		int32 IndexOf(const FString& MaterialId, EDreamTextureImportStage Stage, int32 StartFrom = 0) const
		{
			for (int32 Index = StartFrom; Index < Events.Num(); ++Index)
			{
				if (Events[Index].MaterialId.Equals(MaterialId, ESearchCase::CaseSensitive) && Events[Index].Stage == Stage)
				{
					return Index;
				}
			}
			return INDEX_NONE;
		}
	};

	bool IsFinished(const FString& MaterialId)
	{
		const FDreamTextureImportStatus* Status = FDreamTextureImportQueue::Get().FindStatus(MaterialId);
		return Status != nullptr && Status->IsFinished();
	}

	/** 一个不会有人监听的本机端口：连接会被立刻拒绝，不用等超时。 */
	const TCHAR* const DeadBackendURL = TEXT("http://127.0.0.1:1");
}

// ---------------------------------------------------------------------------
// 后端不可达：排队、一次只跑一个、失败有中文原因、状态指针稳定
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAssetsQueueBackendDownTest, "DreamTexture.Assets.Queue.BackendDown", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAssetsQueueBackendDownTest::RunTest(const FString& Parameters)
{
	FDreamTextureImportQueue& Queue = FDreamTextureImportQueue::Get();
	const TSharedRef<FQueueTestSession> Session = MakeShared<FQueueTestSession>();
	Session->Begin(DeadBackendURL);

	const FString IdA = TEXT("mat_test_queue_a");
	const FString IdB = TEXT("mat_test_queue_b");

	TestFalse(TEXT("空 id 不排"), Queue.Enqueue(FString()));
	TestFalse(TEXT("没排过的 id 不算 active"), Queue.IsActive(IdA));
	TestNull(TEXT("没排过的 id 没有状态"), Queue.FindStatus(IdA));

	TestTrue(TEXT("排 A"), Queue.Enqueue(IdA));
	TestTrue(TEXT("A 排进去之后是 active"), Queue.IsActive(IdA));
	TestFalse(TEXT("同一个 id 不重复排"), Queue.Enqueue(IdA));
	TestTrue(TEXT("排 B"), Queue.Enqueue(IdB));
	TestFalse(TEXT("B 也不重复排"), Queue.Enqueue(IdB));

	// FindStatus 给出去的指针，之后同一个 id 的所有状态变化都写在这块内存上。
	const FDreamTextureImportStatus* StatusA = Queue.FindStatus(IdA);
	const FDreamTextureImportStatus* StatusB = Queue.FindStatus(IdB);
	TestNotNull(TEXT("A 有状态"), StatusA);
	TestNotNull(TEXT("B 有状态"), StatusB);

	const TSharedRef<bool> bAborted = MakeShared<bool>(false);
	AddLatentWait(this, TEXT("两个任务都走到终态"), 60.0, [IdA, IdB]() { return IsFinished(IdA) && IsFinished(IdB); }, bAborted);

	AddLatentStep([this, Session, bAborted, IdA, IdB, StatusA, StatusB]()
	{
		FDreamTextureImportQueue& Queue = FDreamTextureImportQueue::Get();
		if (*bAborted)
		{
			return;
		}

		TestTrue(TEXT("A 的状态指针没变"), Queue.FindStatus(IdA) == StatusA);
		TestTrue(TEXT("B 的状态指针没变"), Queue.FindStatus(IdB) == StatusB);
		TestFalse(TEXT("终态之后 A 不再 active"), Queue.IsActive(IdA));
		TestFalse(TEXT("终态之后 B 不再 active"), Queue.IsActive(IdB));

		for (const FDreamTextureImportStatus* Status : { StatusA, StatusB })
		{
			TestTrue(FString::Printf(TEXT("%s 以 Failed 收场"), *Status->MaterialId), Status->Stage == EDreamTextureImportStage::Failed);
			TestTrue(FString::Printf(TEXT("%s 的失败原因是中文"), *Status->MaterialId), Status->Message.StartsWith(TEXT("Could not get the material info")));
			TestFalse(TEXT("失败时 Result.Error 也带着原因"), Status->Result.Error.IsEmpty());
		}

		// 事件顺序：先 Queued，再 FetchingManifest，最后 Failed。
		const int32 AQueued = Session->IndexOf(IdA, EDreamTextureImportStage::Queued);
		const int32 AFetching = Session->IndexOf(IdA, EDreamTextureImportStage::FetchingManifest);
		const int32 AFailed = Session->IndexOf(IdA, EDreamTextureImportStage::Failed);
		const int32 BQueued = Session->IndexOf(IdB, EDreamTextureImportStage::Queued);
		const int32 BFetching = Session->IndexOf(IdB, EDreamTextureImportStage::FetchingManifest);
		const int32 BFailed = Session->IndexOf(IdB, EDreamTextureImportStage::Failed);
		TestTrue(TEXT("A 的事件齐全且有序"), AQueued != INDEX_NONE && AQueued < AFetching && AFetching < AFailed);
		TestTrue(TEXT("B 的事件齐全且有序"), BQueued != INDEX_NONE && BQueued < BFetching && BFetching < BFailed);

		// 一次只跑一个：A 收场之后 B 才开始取 manifest。
		TestTrue(TEXT("A 失败在 B 开跑之前"), AFailed < BFetching);

		// 每个终态只广播一次。
		int32 FailedEvents = 0;
		for (const FDreamTextureImportStatus& Event : Session->Events)
		{
			FailedEvents += (Event.Stage == EDreamTextureImportStage::Failed) ? 1 : 0;
		}
		TestEqual(TEXT("每个任务的终态各广播一次"), FailedEvents, 2);

		// 终态之后可以再排：这是「失败了点重试」的路径，而且状态被整块重置。
		TestTrue(TEXT("失败后可以重新排"), Queue.Enqueue(IdA));
		TestTrue(TEXT("重排之后仍是同一块状态内存"), Queue.FindStatus(IdA) == StatusA);
		TestFalse(TEXT("重排后状态已清掉上一轮的终态"), StatusA->IsFinished());
		Queue.Shutdown();
	});

	return true;
}

// ---------------------------------------------------------------------------
// Shutdown：丢掉排队的，之后到达的回调一律忽略
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureAssetsQueueShutdownTest, "DreamTexture.Assets.Queue.Shutdown", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureAssetsQueueShutdownTest::RunTest(const FString& Parameters)
{
	FDreamTextureImportQueue& Queue = FDreamTextureImportQueue::Get();
	const TSharedRef<FQueueTestSession> Session = MakeShared<FQueueTestSession>();
	Session->Begin(DeadBackendURL);

	const FString IdA = TEXT("mat_test_shutdown_a");
	const FString IdB = TEXT("mat_test_shutdown_b");
	TestTrue(TEXT("排 A"), Queue.Enqueue(IdA));
	TestTrue(TEXT("排 B"), Queue.Enqueue(IdB));

	// A 的请求已经发出去了，B 还在排队。此刻关掉。
	Queue.Shutdown();
	const int32 EventsAtShutdown = Session->Events.Num();

	TestFalse(TEXT("关闭后 A 不再 active"), Queue.IsActive(IdA));
	TestFalse(TEXT("关闭后排队的 B 被丢掉"), Queue.IsActive(IdB));
	const FDreamTextureImportStatus* StatusB = Queue.FindStatus(IdB);
	if (TestNotNull(TEXT("B 仍查得到状态"), StatusB))
	{
		TestTrue(TEXT("被丢掉的任务标成 Failed 而不是永远停在 Queued"), StatusB->Stage == EDreamTextureImportStage::Failed);
	}

	// 等足够久，让 A 那个在飞的请求（连接被拒）回来：它的回调必须被忽略，不能再广播任何事件，
	// 更不能把 B 拉起来继续跑。
	const TSharedRef<double> Until = MakeShared<double>(-1.0);
	AddLatent([Until]()
	{
		if (*Until < 0.0)
		{
			*Until = FPlatformTime::Seconds() + 6.0;
		}
		return FPlatformTime::Seconds() > *Until;
	});

	AddLatentStep([this, Session, EventsAtShutdown, IdA, IdB]()
	{
		FDreamTextureImportQueue& Queue = FDreamTextureImportQueue::Get();
		TestEqual(TEXT("关闭之后没有再收到任何状态事件"), Session->Events.Num(), EventsAtShutdown);
		TestFalse(TEXT("过期回调没有把 B 拉起来"), Queue.IsActive(IdB));

		// 关闭之后队列还能正常工作（模块热重载、或测试之间复用）。
		TestTrue(TEXT("关闭后可以重新排"), Queue.Enqueue(IdA));
		Queue.Shutdown();
	});

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
