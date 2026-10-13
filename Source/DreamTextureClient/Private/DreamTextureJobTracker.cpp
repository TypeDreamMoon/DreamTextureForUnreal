// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureJobTracker.h"

#include "Containers/Ticker.h"
#include "DreamTextureConnection.h"
#include "DreamTextureLog.h"

struct FDreamTextureJobTracker::FTimers
{
	/** 兜底补状态的定时器。无效 = 没在跑。 */
	FTSTicker::FDelegateHandle Reconcile;
};

FDreamTextureJobTracker::FHooks FDreamTextureJobTracker::FHooks::MakeDefault()
{
	FHooks Out;
	Out.GetJob = [](const FString& JobId, FDreamTextureGenerateApi::FOnJob Done)
	{
		FDreamTextureGenerateApi::GetJob(JobId, MoveTemp(Done));
	};
	Out.Submit = [](const FDreamTextureGenerateRequest& Request, FDreamTextureGenerateApi::FOnSubmitted Done)
	{
		FDreamTextureGenerateApi::Submit(Request, MoveTemp(Done));
	};
	Out.Cancel = [](const FString& JobId, FDreamTextureGenerateApi::FOnDone Done)
	{
		FDreamTextureGenerateApi::CancelJob(JobId, MoveTemp(Done));
	};
	return Out;
}

FDreamTextureJobTracker::FDreamTextureJobTracker(FHooks InHooks)
	: Hooks(MoveTemp(InHooks))
	, Timers(MakeUnique<FTimers>())
{
}

FDreamTextureJobTracker::~FDreamTextureJobTracker()
{
	Unbind();

	// 定时器的代码在本 DLL 里，模块卸载后再被叫到就是崩溃，所以析构时一定摘掉。
	// 这里不会是在定时器自己的回调里：回调里发现自己是最后一个持有者时，先把句柄 Reset 掉再返回 false（见 EnsureReconcileTicker），
	// 句柄无效就不会走到 RemoveTicker——FTSTicker::RemoveTicker 在该 ticker 自己的回调里调用是死锁。
	if (Timers.IsValid() && Timers->Reconcile.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(Timers->Reconcile);
	}
}

//////////////////////////////////////////////////////////////////////////
// 订阅

void FDreamTextureJobTracker::Bind(const TSharedPtr<FDreamTextureConnection>& Connection)
{
	Unbind();
	if (!Connection.IsValid())
	{
		return;
	}

	BoundConnection = Connection;
	EventHandle = Connection->OnEvent.AddSP(this, &FDreamTextureJobTracker::HandleEvent);
	LinkHandle = Connection->OnLinkChanged.AddSP(this, &FDreamTextureJobTracker::HandleLinkChanged);
}

void FDreamTextureJobTracker::Unbind()
{
	// AddSP 绑的是弱引用，漏摘不会悬空，但死条目要等到下一次广播才清掉。
	// 连接可能先于我们销毁（模块关闭的顺序），所以先 Pin。
	if (const TSharedPtr<FDreamTextureConnection> Connection = BoundConnection.Pin())
	{
		Connection->OnEvent.Remove(EventHandle);
		Connection->OnLinkChanged.Remove(LinkHandle);
	}
	BoundConnection.Reset();
	EventHandle.Reset();
	LinkHandle.Reset();
}

void FDreamTextureJobTracker::HandleLinkChanged(EDreamTextureLink NewLink)
{
	// 断线期间的事件是收不到的。重新连上就把没结束的任务各问一次，补上错过的状态。
	// 没有在跑的任务时 Reconcile 什么都不做，所以每次变 Live 都调是便宜的。
	if (NewLink == EDreamTextureLink::Live)
	{
		Reconcile();
	}
}

void FDreamTextureJobTracker::HandleEvent(const FDreamTextureEvent& Event)
{
	FDreamTextureJob Incoming;
	if (!FDreamTextureJob::FromEvent(Event, Incoming))
	{
		return;
	}
	ApplyUpdate(Incoming);
}

//////////////////////////////////////////////////////////////////////////
// 合并

bool FDreamTextureJobTracker::Merge(FEntry& Entry, const FDreamTextureJob& Incoming) const
{
	FDreamTextureJob& Current = Entry.Job;

	// 终态就是终态；比当前更靠前的状态（排队事件迟到、旧的 REST 应答）一律是过期的。
	if (Current.IsTerminal() || Incoming.GetRank() < Current.GetRank())
	{
		return false;
	}

	FDreamTextureJob Merged = Incoming;

	// 事件里偶尔缺字段（别的客户端的、更老的后端的），缺的沿用已知的，别被空值盖掉。
	if (Merged.MaterialId.IsEmpty())
	{
		Merged.MaterialId = Current.MaterialId;
	}
	if (Merged.WorkflowId.IsEmpty())
	{
		Merged.WorkflowId = Current.WorkflowId;
	}
	if (Merged.BatchId.IsEmpty())
	{
		Merged.BatchId = Current.BatchId;
	}
	if (Merged.PromptId.IsEmpty())
	{
		Merged.PromptId = Current.PromptId;
	}
	if (!Merged.bHasSeed)
	{
		Merged.bHasSeed = Current.bHasSeed;
		Merged.Seed = Current.Seed;
	}
	if (Merged.Resolution == 0)
	{
		Merged.Resolution = Current.Resolution;
	}
	if (Merged.Name.IsEmpty())
	{
		Merged.Name = Current.Name;
	}
	if (Merged.CreatedAt.GetTicks() == 0)
	{
		Merged.CreatedAt = Current.CreatedAt;
	}

	// 同一状态内进度不倒退：采样进度、分解、落盘各发各的事件，晚到的旧事件不该把进度条拉回去。
	if (Incoming.Status == Current.Status && !Incoming.IsTerminal())
	{
		Merged.Progress = FMath::Max(Current.Progress, Incoming.Progress);
	}
	if (Merged.Status == EDreamTextureJobStatus::Succeeded)
	{
		Merged.Progress = 1.0f;
	}

	const bool bChanged = Merged.Status != Current.Status
		|| !FMath::IsNearlyEqual(Merged.Progress, Current.Progress)
		|| Merged.Stage != Current.Stage
		|| Merged.Error != Current.Error
		|| Merged.MaterialId != Current.MaterialId
		|| Merged.PromptId != Current.PromptId;

	Current = MoveTemp(Merged);
	return bChanged;
}

void FDreamTextureJobTracker::ApplyUpdate(const FDreamTextureJob& Incoming)
{
	const TSharedPtr<FEntry>* Found = Entries.Find(Incoming.Id);
	if (Found == nullptr)
	{
		// 还没登记的任务：提交的应答可能还在路上，而后端已经开始推事件了。
		// 暂存最近一次的样子，登记时并进去，否则界面会先显示「排队」再等下一个事件才纠正。
		StashEarly(Incoming);
		return;
	}

	FEntry& Entry = **Found;
	if (Merge(Entry, Incoming))
	{
		OnJobChanged.Broadcast(Entry.Job);
	}
}

void FDreamTextureJobTracker::StashEarly(const FDreamTextureJob& Incoming)
{
	if (const FDreamTextureJob* Existing = Early.Find(Incoming.Id))
	{
		// 暂存区里也只往前走。
		if (Incoming.GetRank() < Existing->GetRank())
		{
			return;
		}
	}
	else
	{
		EarlyOrder.Add(Incoming.Id);
		while (EarlyOrder.Num() > MaxEarly)
		{
			Early.Remove(EarlyOrder[0]);
			EarlyOrder.RemoveAt(0);
		}
	}
	Early.Add(Incoming.Id, Incoming);
}

//////////////////////////////////////////////////////////////////////////
// 登记与查询

void FDreamTextureJobTracker::Track(const FDreamTextureJob& Job, const FString& Origin)
{
	if (Job.Id.IsEmpty())
	{
		return;
	}

	if (const TSharedPtr<FEntry>* Found = Entries.Find(Job.Id))
	{
		FEntry& Entry = **Found;
		if (Merge(Entry, Job))
		{
			OnJobChanged.Broadcast(Entry.Job);
		}
		return;
	}

	const TSharedRef<FEntry> Entry = MakeShared<FEntry>();
	Entry->Job = Job;
	Entry->Origin = Origin;

	// 并入登记前就到了的事件。先并再广播，订阅方只会看到最终状态那一次。
	if (const FDreamTextureJob* Stashed = Early.Find(Job.Id))
	{
		Merge(*Entry, *Stashed);
		Early.Remove(Job.Id);
		EarlyOrder.Remove(Job.Id);
	}

	Entries.Add(Job.Id, Entry);
	Order.Add(Job.Id);
	Trim();

	OnJobChanged.Broadcast(Entry->Job);

	// 有没结束的任务：开兜底补状态的定时器。事件流会静默丢事件，终态一丢这个任务就永远停在「生成中」。
	EnsureReconcileTicker();
}

void FDreamTextureJobTracker::Forget(const FString& JobId)
{
	Entries.Remove(JobId);
	Order.Remove(JobId);
	CancelRequested.Remove(JobId);
	FetchInFlight.Remove(JobId);
}

void FDreamTextureJobTracker::Trim()
{
	while (Order.Num() > MaxTracked)
	{
		// 只丢已结束的、最老的。全是在跑的任务时宁可超过上限，也不能丢掉还在等结果的。
		int32 Victim = INDEX_NONE;
		for (int32 i = 0; i < Order.Num(); ++i)
		{
			const TSharedPtr<FEntry>* Found = Entries.Find(Order[i]);
			if (Found == nullptr || (*Found)->Job.IsTerminal())
			{
				Victim = i;
				break;
			}
		}
		if (Victim == INDEX_NONE)
		{
			return;
		}
		// 拷一份再删：Forget 要从 Order 里移除这个 id，传进去的引用不能指向 Order 自己的元素。
		const FString VictimId = Order[Victim];
		Forget(VictimId);
	}
}

const FDreamTextureJob* FDreamTextureJobTracker::Find(const FString& JobId) const
{
	const TSharedPtr<FEntry>* Found = Entries.Find(JobId);
	return Found != nullptr ? &(*Found)->Job : nullptr;
}

FString FDreamTextureJobTracker::GetOrigin(const FString& JobId) const
{
	const TSharedPtr<FEntry>* Found = Entries.Find(JobId);
	return Found != nullptr ? (*Found)->Origin : FString();
}

TArray<FDreamTextureJob> FDreamTextureJobTracker::GetBatch(const FString& BatchId) const
{
	TArray<FDreamTextureJob> Out;
	if (BatchId.IsEmpty())
	{
		return Out;
	}
	for (const FString& Id : Order)
	{
		if (const TSharedPtr<FEntry>* Found = Entries.Find(Id))
		{
			if ((*Found)->Job.BatchId == BatchId)
			{
				Out.Add((*Found)->Job);
			}
		}
	}
	return Out;
}

FString FDreamTextureJobTracker::GetLatestBatchId(const FString& Origin) const
{
	for (int32 i = Order.Num() - 1; i >= 0; --i)
	{
		if (const TSharedPtr<FEntry>* Found = Entries.Find(Order[i]))
		{
			if ((*Found)->Origin == Origin && !(*Found)->Job.BatchId.IsEmpty())
			{
				return (*Found)->Job.BatchId;
			}
		}
	}
	return FString();
}

TArray<FDreamTextureJob> FDreamTextureJobTracker::GetActiveJobs() const
{
	TArray<FDreamTextureJob> Out;
	for (const FString& Id : Order)
	{
		if (const TSharedPtr<FEntry>* Found = Entries.Find(Id))
		{
			if (!(*Found)->Job.IsTerminal())
			{
				Out.Add((*Found)->Job);
			}
		}
	}
	return Out;
}

//////////////////////////////////////////////////////////////////////////
// 提交与取消

void FDreamTextureJobTracker::Submit(const FDreamTextureGenerateRequest& Request, FDreamTextureGenerateApi::FOnSubmitted Done, const FString& Origin)
{
	TWeakPtr<FDreamTextureJobTracker> WeakSelf = AsShared();
	Hooks.Submit(Request,
		[WeakSelf, Done = MoveTemp(Done), Origin](bool bOK, TArray<FDreamTextureJob>&& Jobs, const FString& Error) mutable
		{
			if (bOK)
			{
				// 追踪器已经没了（模块关闭中）就只把结果交出去，不登记。
				if (const TSharedPtr<FDreamTextureJobTracker> Self = WeakSelf.Pin())
				{
					for (const FDreamTextureJob& Job : Jobs)
					{
						Self->Track(Job, Origin);
					}
				}
			}
			if (Done)
			{
				Done(bOK, MoveTemp(Jobs), Error);
			}
		});
}

void FDreamTextureJobTracker::Cancel(const FString& JobId, FDreamTextureGenerateApi::FOnDone Done)
{
	// 先记下「已请求取消」再发请求：正在跑的任务要等 ComfyUI 被打断才结束，这中间界面得有个说法。
	CancelRequested.Add(JobId);

	TWeakPtr<FDreamTextureJobTracker> WeakSelf = AsShared();
	Hooks.Cancel(JobId,
		[WeakSelf, JobId, Done = MoveTemp(Done)](bool bOK, const FString& Error)
		{
			if (const TSharedPtr<FDreamTextureJobTracker> Self = WeakSelf.Pin())
			{
				if (bOK)
				{
					// 排队中的任务取消是立刻生效的，问一次就能拿到 canceled，不必干等事件。
					Self->FetchOne(JobId);
				}
				else
				{
					Self->CancelRequested.Remove(JobId);

					// 取消失败多半是因为任务刚好已经结束了（后端回 400「任务已结束」）。如果终态事件在这之前就丢了，
					// 这一行会回到「生成中」，再没人去校正它——点取消得到的恰恰是「它已经不在跑了」这条最有用的信息。
					// 所以失败时也问一次，让追踪器拿到真实状态。网络失败的话这一问也会失败，什么都不改，下次重连再补。
					Self->FetchOne(JobId);
				}
			}
			if (Done)
			{
				Done(bOK, Error);
			}
		});
}

bool FDreamTextureJobTracker::IsCancelRequested(const FString& JobId) const
{
	return CancelRequested.Contains(JobId);
}

//////////////////////////////////////////////////////////////////////////
// 补状态

void FDreamTextureJobTracker::Reconcile()
{
	// 先取一份 id 列表再发请求：请求回调是异步的，但 Hooks 是可替换的，
	// 假实现完全可能同步回调，那样会在遍历 Entries 的途中改它。
	TArray<FString> Ids;
	for (const FString& Id : Order)
	{
		if (const TSharedPtr<FEntry>* Found = Entries.Find(Id))
		{
			if (!(*Found)->Job.IsTerminal())
			{
				Ids.Add(Id);
			}
		}
	}
	for (const FString& Id : Ids)
	{
		FetchOne(Id);
	}
}

bool FDreamTextureJobTracker::HasActiveJobs() const
{
	for (const TPair<FString, TSharedPtr<FEntry>>& Pair : Entries)
	{
		if (Pair.Value.IsValid() && !Pair.Value->Job.IsTerminal())
		{
			return true;
		}
	}
	return false;
}

void FDreamTextureJobTracker::EnsureReconcileTicker()
{
	if (!Timers.IsValid() || Timers->Reconcile.IsValid() || !HasActiveJobs())
	{
		return;
	}

	TWeakPtr<FDreamTextureJobTracker> WeakThis = AsWeak();
	Timers->Reconcile = FTSTicker::GetCoreTicker().AddTicker(TEXT("DreamTexture.JobReconcile"), ReconcileIntervalSecs,
		[WeakThis](float) -> bool
		{
			const TSharedPtr<FDreamTextureJobTracker> Self = WeakThis.Pin();
			if (!Self.IsValid())
			{
				return false;
			}

			bool bKeep = Self->OnReconcileTick();

			// 本回调里持有的这一份是最后一个引用：返回后追踪器就在这个定时器的回调里被析构了，
			// 而析构会 RemoveTicker——在该 ticker 自己的回调里调用是死锁。所以这种情形主动收工。
			if (Self.IsUnique())
			{
				bKeep = false;
			}

			if (!bKeep)
			{
				// 先把句柄 Reset 掉再返回 false（由系统摘掉这个 ticker）。不要在这里 RemoveTicker：死锁。
				Self->Timers->Reconcile.Reset();
			}
			return bKeep;
		});
}

bool FDreamTextureJobTracker::OnReconcileTick()
{
	// 没有活动任务：不跑，定时器收工；下次有任务登记时 Track 会重新开。
	if (!HasActiveJobs())
	{
		return false;
	}

	// 后端已知不通：问了也是白问。连接重新变 Live 时 HandleLinkChanged 会补，这里不重复。
	if (const TSharedPtr<FDreamTextureConnection> Connection = BoundConnection.Pin())
	{
		if (!Connection->IsBackendReachable())
		{
			return true;
		}
	}

	Reconcile();
	return true;
}

void FDreamTextureJobTracker::FetchOne(const FString& JobId)
{
	if (!Entries.Contains(JobId) || FetchInFlight.Contains(JobId))
	{
		return;
	}
	FetchInFlight.Add(JobId);

	TWeakPtr<FDreamTextureJobTracker> WeakSelf = AsShared();
	Hooks.GetJob(JobId,
		[WeakSelf, JobId](bool bOK, FDreamTextureJob&& Job, const FString& Error, bool bNotFound)
		{
			const TSharedPtr<FDreamTextureJobTracker> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			Self->FetchInFlight.Remove(JobId);

			const TSharedPtr<FEntry>* Found = Self->Entries.Find(JobId);
			if (Found == nullptr)
			{
				return;
			}

			if (bOK)
			{
				if (Self->Merge(**Found, Job))
				{
					Self->OnJobChanged.Broadcast((*Found)->Job);
				}
				return;
			}

			if (bNotFound)
			{
				// 后端明确说没有这个任务（换了数据目录、库被清了）：它再也不会有结果了，
				// 留着「进行中」只会让界面永远转圈。判失败，把原因写清楚。
				FDreamTextureJob Gone = (*Found)->Job;
				Gone.Status = EDreamTextureJobStatus::Failed;
				Gone.Stage = FString();
				Gone.Error = NSLOCTEXT("DreamTextureJobTracker", "JobGone", "The backend no longer knows this job (it may have restarted or switched data folders), so it will never produce a result.").ToString();
				if (Self->Merge(**Found, Gone))
				{
					Self->OnJobChanged.Broadcast((*Found)->Job);
				}
				return;
			}

			// 连不上之类：什么都不改。任务大概率还在后端跑着，下次链路变 Live 会再问。
			UE_LOG(LogDreamTexture, Verbose, TEXT("Could not refresh the status of job %s; will retry on the next reconnect: %s"), *JobId, *Error);
		});
}
