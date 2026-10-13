// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DreamTextureGenerate.h"
#include "DreamTextureTypes.h"

class FDreamTextureConnection;

/**
 * 生成任务的提交与跟踪。进程内一个，由 FDreamTextureClientModule 持有。
 *
 * 生成页（M4）和回流（M8）都经它提交任务、读任务进度，所以它不认识「提示词」「材质」这些页面概念，
 * 只认「请求 → 一批任务 → 每个任务的状态」。
 *
 * ## 进度从哪来
 *
 * 全部来自连接上的 `job.*` 事件，**不轮询**。只有两处主动问一次 `GET /api/jobs/{id}`：
 *
 *   1. 链路变成 Live 时（断线重连、后端重启之后）。断线期间的事件是收不到的，不补就会永远停在旧状态；
 *   2. 取消请求成功之后。排队中的任务被取消是立刻生效的，不必干等事件。
 *
 * 事件流本身也不保证不丢：后端给每个订阅者一条 64 格的缓冲，满了就**静默**丢事件（job/events.go 的 Publish）。
 * 终态事件丢了，这个任务就永远停在「生成中」。所以补状态有三道：
 *
 *   - 重连、取消请求（成不成功都问）之后各问一次；
 *   - 有活动任务时每 DefaultReconcileIntervalSecs 秒自动 Reconcile 一次——这是**兜底**，不是进度来源：
 *     进度仍然只来自事件，这里一次一个任务一个 GET，频率低到不会给后端添负担；没有活动任务时不跑；
 *   - Reconcile() 是公开的，界面在页面打开、用户点「同步」的时候也可以调。
 *
 * ## 状态只往前走
 *
 * 事件和 REST 应答谁先到都有可能（提交的应答回来之前，job.progress 事件可能已经到了）。
 * 每次更新都按 FDreamTextureJob::GetRank() 比先后：排队 < 运行 < 终态，旧的不能盖掉新的，
 * 终态一旦落定就是终态。还没来得及登记的任务事件会暂存一小会儿，登记时并进去。
 *
 * 所有委托都在游戏线程上广播。
 */
class DREAMTEXTURECLIENT_API FDreamTextureJobTracker : public TSharedFromThis<FDreamTextureJobTracker>
{
public:
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnJobChanged, const FDreamTextureJob& /*Job*/);

	/**
	 * 网络那一层，默认全部转发到 FDreamTextureGenerateApi。
	 * 自动化测试换成假的，把回调攥在手里自己决定什么时候回。
	 *
	 * 要带 API 导出：构造函数的默认实参 FHooks::MakeDefault() 是在调用方的翻译单元里求值的，
	 * 别的模块（界面、测试）直接 MakeShared<FDreamTextureJobTracker>() 时，没导出就是链接错误。
	 */
	struct DREAMTEXTURECLIENT_API FHooks
	{
		TFunction<void(const FString& /*JobId*/, FDreamTextureGenerateApi::FOnJob)> GetJob;
		TFunction<void(const FDreamTextureGenerateRequest&, FDreamTextureGenerateApi::FOnSubmitted)> Submit;
		TFunction<void(const FString& /*JobId*/, FDreamTextureGenerateApi::FOnDone)> Cancel;

		static FHooks MakeDefault();
	};

	/** 默认的来源标签：生成页提交的任务。 */
	static const TCHAR* GetDefaultOrigin() { return TEXT("generate"); }

	/** 最多记住多少个任务。超了先丢最老的**已结束**的；没结束的任务绝不丢。 */
	static constexpr int32 MaxTracked = 200;

	/**
	 * 有活动任务时兜底补状态的间隔（秒）。30 秒：丢了终态事件的任务最多晚 30 秒被校正，
	 * 8 个变体也不过每 30 秒 8 个 GET。
	 */
	static constexpr float DefaultReconcileIntervalSecs = 30.0f;

	explicit FDreamTextureJobTracker(FHooks InHooks = FHooks::MakeDefault());
	~FDreamTextureJobTracker();

	/** 改兜底补状态的间隔。只对之后新起的定时器生效；自动化测试用，别处不要动它。 */
	void SetReconcileInterval(float Secs) { ReconcileIntervalSecs = FMath::Max(0.05f, Secs); }

	/**
	 * 订阅这条连接的事件与链路变化。必须已被共享指针持有（AddSP）。
	 * 重复调用会先解绑旧的。
	 */
	void Bind(const TSharedPtr<FDreamTextureConnection>& Connection);
	void Unbind();

	/**
	 * 提交并开始跟踪。成功时 Done 收到本批全部任务（已登记）；失败时 Jobs 为空，Error 是中文原因。
	 *
	 * @param Origin 谁提交的。自动导入只认生成页的任务，回流等别的提交方用自己的标签，互不干扰。
	 */
	void Submit(const FDreamTextureGenerateRequest& Request, FDreamTextureGenerateApi::FOnSubmitted Done,
		const FString& Origin = GetDefaultOrigin());

	/**
	 * 请求取消。排队中的立刻变成 canceled，正在跑的要等 ComfyUI 被打断后才结束，
	 * 期间 IsCancelRequested() 为真，界面可以显示「取消中」。
	 */
	void Cancel(const FString& JobId, FDreamTextureGenerateApi::FOnDone Done = nullptr);
	bool IsCancelRequested(const FString& JobId) const;

	/** 开始跟踪一个已有的任务（提交的应答、恢复上一批…）。已经在跟踪的，按「只往前走」并进去。 */
	void Track(const FDreamTextureJob& Job, const FString& Origin = GetDefaultOrigin());

	void Forget(const FString& JobId);

	/** 没有返回 nullptr。指针只在下一次 Track/Forget/事件处理之前有效，别存。 */
	const FDreamTextureJob* Find(const FString& JobId) const;
	FString GetOrigin(const FString& JobId) const;

	/** 一批里的全部任务，按登记先后（也就是变体顺序）。 */
	TArray<FDreamTextureJob> GetBatch(const FString& BatchId) const;

	/** 某个提交方最近一批的 id。没有返回空串。页面重新打开时据此恢复上一批。 */
	FString GetLatestBatchId(const FString& Origin) const;

	/** 还没结束的任务。 */
	TArray<FDreamTextureJob> GetActiveJobs() const;

	int32 GetNumTracked() const { return Entries.Num(); }

	/**
	 * 把没结束的任务逐个问一遍后端。公开是因为界面在「页面打开」「用户点同步」时也要用，
	 * 不只是重连之后。同一个任务有请求在途时不重复问。
	 */
	void Reconcile();

	//~ 以下两个是订阅的处理函数，公开是为了自动化测试直接喂数据
	void HandleEvent(const FDreamTextureEvent& Event);
	void HandleLinkChanged(EDreamTextureLink NewLink);

	/** 任务第一次登记、或任一字段（状态、进度、阶段、错误、材质 id）变化时广播。 */
	FOnJobChanged OnJobChanged;

private:
	struct FEntry
	{
		FDreamTextureJob Job;
		FString Origin;
	};

	/** 把一次更新并进条目。真的变了返回 true。规则见类注释的「状态只往前走」。 */
	bool Merge(FEntry& Entry, const FDreamTextureJob& Incoming) const;

	void ApplyUpdate(const FDreamTextureJob& Incoming);
	void StashEarly(const FDreamTextureJob& Incoming);
	void Trim();

	/** 问后端一个任务的现状并并进来。 */
	void FetchOne(const FString& JobId);

	bool HasActiveJobs() const;

	/** 有活动任务就确保兜底定时器在跑。 */
	void EnsureReconcileTicker();

	/** 定时器每到点来一次。返回 false 表示不再需要（没有活动任务了）。 */
	bool OnReconcileTick();

	FHooks Hooks;

	/** 条目用共享指针存：Find() 给出去的指针不会因为 TMap 扩容而挪位置。 */
	TMap<FString, TSharedPtr<FEntry>> Entries;

	/** 登记先后。 */
	TArray<FString> Order;

	/** 事件先于登记到达的任务，最近一次的样子。 */
	TMap<FString, FDreamTextureJob> Early;
	TArray<FString> EarlyOrder;
	static constexpr int32 MaxEarly = 64;

	TSet<FString> FetchInFlight;
	TSet<FString> CancelRequested;

	TWeakPtr<FDreamTextureConnection> BoundConnection;
	FDelegateHandle EventHandle;
	FDelegateHandle LinkHandle;

	float ReconcileIntervalSecs = DefaultReconcileIntervalSecs;

	/**
	 * 定时器句柄放进一个只在 .cpp 里定义的结构体，不把 Containers/Ticker.h 拖进这个公共头
	 * ——编辑器模块里每个包含它的翻译单元都得跟着吃一遍（FDreamTextureConnection 同理）。
	 */
	struct FTimers;
	TUniquePtr<FTimers> Timers;
};
