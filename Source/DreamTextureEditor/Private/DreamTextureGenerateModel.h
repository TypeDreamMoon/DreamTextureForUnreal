// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DreamTextureGenerate.h"
#include "DreamTextureImportQueue.h"
#include "DreamTextureJobTracker.h"
#include "DreamTextureProcess.h"
#include "DreamTextureTypes.h"

class FDreamTextureConnection;

/** 提交按钮被什么挡住时，是三级状态里的哪一级。三级不要混成一个灯。 */
enum class EDreamTextureGenerateTier : uint8
{
	None,
	/** 第 1 级：后端进程。 */
	Backend,
	/** 第 2 级：WS 长连接。只收不发，挡不住提交，只影响看进度。 */
	Link,
	/** 第 3 级：ComfyUI。 */
	Comfy,
};

/** 现在能不能提交，以及（不能时）是哪一级的问题。 */
struct FDreamTextureGenerateReadiness
{
	bool bCanSubmit = false;
	EDreamTextureGenerateTier Tier = EDreamTextureGenerateTier::None;

	/** bCanSubmit 为假时的原因，中文，直接显示。 */
	FText Reason;

	/**
	 * 第 1 级不通，但只是「还没开始连 / 正在连」，不是「连过了确实没应答」。
	 * 前者几秒内多半自己就好，界面不该为它让出「启动后端」的入口。
	 */
	bool bConnecting = false;

	/** bCanSubmit 为真但有值得说一声的事（长连接断了、ComfyUI 积压…）。 */
	FText Note;
};

/** 许可提示：选的组合产出能不能商用。 */
struct FDreamTextureGenerateLicenseHint
{
	bool bNonCommercial = false;

	/** 后端 Commercial() 同口径的原因："组件: 许可; 组件: 许可"。 */
	FString Reason;

	/** 涉及的组件名（CHORD (…)），用于逐条列出。 */
	TArray<FString> Components;

	/** 两段搭不上的提醒（手绘图喂给 CHORD…）。只提醒不拦。 */
	FString Mismatch;

	/** 目录里没有许可限制的分解模型的名字。不可商用时用来告诉人「换成它就行」。 */
	TArray<FString> CommercialDecomposeNames;
};

/** 一张变体卡片此刻该怎么画。每帧由 EvaluateRow 现算，不缓存。 */
struct FDreamTextureGenerateRowView
{
	enum class EKind : uint8
	{
		/** 追踪器里没有这个任务，或后端给了不认识的状态。 */
		Unknown,
		Waiting,
		Running,
		/** 已请求取消，等 ComfyUI 被打断。 */
		Canceling,
		Done,
		Failed,
		Canceled,
	};

	enum class EImport : uint8
	{
		/** 没导入过，或任务还没完成。 */
		None,
		Active,
		Succeeded,
		Failed,
	};

	EKind Kind = EKind::Unknown;

	/** "变体 2"。 */
	FText Title;

	/** "种子 123456 · 1024²"。 */
	FText Subtitle;

	/** 阶段文字（排队中、生成中、落盘中、取消中、完成、失败、已取消…）。 */
	FText Stage;

	/** 失败/取消的原因。 */
	FText Detail;

	bool bShowProgress = false;

	/** 进度 0~1。bShowProgress 为真而这里没设值，表示不知道进度（等 ComfyUI、取消中），进度条走跑马灯。 */
	TOptional<float> Percent;

	bool bCanCancel = false;
	bool bCanImport = false;

	EImport Import = EImport::None;
	FText ImportButton;
	FText ImportText;
	FText ImportTooltip;
};

/**
 * 模型依赖的外部能力。默认全部转发到真实实现；自动化测试替换成假的，
 * 这样不用起后端、也不会真的去排导入队列或碰项目设置。
 */
struct FDreamTextureGenerateBackend
{
	TFunction<void(FDreamTextureGenerateApi::FOnCatalog)> ListWorkflows;
	TFunction<void(FDreamTextureGenerateApi::FOnRefineModels)> ListRefineModels;
	TFunction<void(FDreamTextureGenerateApi::FOnImagenProviders)> ListImagenProviders;

	/**
	 * 确保后端在（FDreamTextureProcess::EnsureBackend，按项目设置里的地址与可执行文件）。
	 * 回调一定是异步的。默认转发到客户端模块的进程管理器；它不在时异步失败。
	 */
	TFunction<void(FDreamTextureProcess::FEnsureDone)> EnsureBackend;

	/** 设置里配了「后端可执行文件」。没配就没有东西可拉起。 */
	TFunction<bool()> HasBackendExecutable;
	TFunction<void(const FString& /*Prompt*/, EDreamTextureRefinePurpose, const FString& /*Model*/, FDreamTextureGenerateApi::FOnRefined)> Refine;

	/** 排一个导入；同一个 id 已在队列里时返回 false。 */
	TFunction<bool(const FString& /*MaterialId*/)> Enqueue;

	/** 这个材质最近一次的导入状态；没排过返回 nullptr。 */
	TFunction<const FDreamTextureImportStatus*(const FString& /*MaterialId*/)> FindImportStatus;

	TFunction<TSharedPtr<FDreamTextureJobTracker>()> GetTracker;
	TFunction<TSharedPtr<FDreamTextureConnection>()> GetConnection;

	/** 「完成后自动导入」这个设置。读写都走它，测试里不碰真实的项目设置。 */
	TFunction<bool()> GetAutoImport;
	TFunction<void(bool)> SetAutoImport;

	static FDreamTextureGenerateBackend MakeDefault();
};

/**
 * 生成页的状态：出图模型 × 分解模型的选择、提示词与参数、提交、本批变体、扩写。
 *
 * 为什么单拎成一个不含 Slate 的类：界面要的东西都是「异步结果 + 过期丢弃」，
 * 这些逻辑一旦混进控件里就没法测。拎出来之后自动化测试可以注入假后端和假追踪器，逐个分支验证。
 *
 * ## 回调与生命周期
 *
 * 所有异步回调只捕获弱指针。模型归面板所有，面板关掉之后迟到的回调 Pin 不到就直接丢弃。
 * **任务本身不归模型管**：它们在追踪器里（进程内一个），面板关掉再打开，Initialize 会从追踪器里
 * 把上一批找回来，进度条继续走。
 *
 * ## 组合管线怎么提交
 *
 * 出图模型、分解模型各选一个，提交时 workflow_id 是 `<出图段 id>.<分解段 id>`，后端不知道也不关心「段」。
 * 目录里没有这个组合（段是新加的、后端还没重载）就不能提交，并且说清楚。
 *
 * 所有委托都在游戏线程上广播。
 */
class FDreamTextureGenerateModel : public TSharedFromThis<FDreamTextureGenerateModel>
{
public:
	enum class ECatalogState : uint8
	{
		NotLoaded,
		Loading,
		Ready,
		Failed,
	};

	enum class ERefineState : uint8
	{
		Idle,
		Working,
		HasResult,
	};

	enum class ECloudState : uint8
	{
		NotLoaded,
		Loading,
		Ready,
		Failed,
	};

	/** 「启动后端并生成」走到哪一步了。Idle = 没在走。 */
	enum class EAutoStartState : uint8
	{
		Idle,
		/** 在等进程管理器把后端拉起来（或确认它已经在了）。 */
		StartingBackend,
		/** 后端应答了，目录还没拉到，等它。 */
		LoadingCatalog,
		/** 目录到了，所选的是云端出图模型而云端清单还没到，等它。 */
		LoadingCloud,
	};

	/** 主按钮此刻该做什么。 */
	enum class EPrimaryAction : uint8
	{
		Submit,
		/** 后端进程没应答、设置里配了可执行文件：先把后端拉起来，再提交。 */
		StartAndSubmit,
	};

	/** 本批里的一行。只记任务 id 和变体序号，任务的实时状态到追踪器里取。 */
	struct FRow
	{
		FString JobId;

		/** 从 1 开始的变体序号。 */
		int32 Number = 0;
	};
	using FRowPtr = TSharedPtr<FRow>;

	explicit FDreamTextureGenerateModel(FDreamTextureGenerateBackend InBackend = FDreamTextureGenerateBackend::MakeDefault());
	~FDreamTextureGenerateModel();

	/**
	 * 订阅外部事件（长连接）、拉目录、找回上一批。不放构造函数：AddSP 要求对象已被共享指针持有。
	 * bBindExternal 为 false 时不订阅长连接，自动化测试直接调 Handle* 喂数据；目录与扩写模型仍会通过后端函数去拉。
	 */
	void Initialize(bool bBindExternal = true);

	DECLARE_MULTICAST_DELEGATE(FOnChanged);
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnNotice, const FText& /*Message*/, bool /*bIsError*/);

	/** 目录、选择、可选项（分辨率）、许可、扩写模型列表任一变化。 */
	FOnChanged OnFormChanged;

	/** 本批的行列表变了（提交了新的一批、找回了上一批）。 */
	FOnChanged OnBatchChanged;

	/** 扩写的状态或结果变化。 */
	FOnChanged OnRefineChanged;

	/** 提示词被程序改了（接受扩写结果）。用户自己打字不触发，控件已经是最新的。 */
	FOnChanged OnPromptReplaced;

	/** 要提示用户的一句话。模型自己不碰 Slate，由面板弹通知。 */
	FOnNotice OnNotice;

	//~ 目录
	void LoadCatalog();
	ECatalogState GetCatalogState() const { return CatalogState; }
	const FString& GetCatalogError() const { return CatalogError; }
	const FDreamTextureWorkflowCatalog& GetCatalog() const { return Catalog; }

	//~ 出图模型 × 分解模型
	TArray<const FDreamTextureWorkflowMeta*> GetSourceOptions() const { return Catalog.GetSourceSegments(); }
	TArray<const FDreamTextureWorkflowMeta*> GetDecomposeOptions() const { return Catalog.GetDecomposeSegments(); }
	void SetSourceId(const FString& Id);
	void SetDecomposeId(const FString& Id);
	const FString& GetSourceId() const { return SourceId; }
	const FString& GetDecomposeId() const { return DecomposeId; }

	/** 当前选择拼出来的工作流；目录没加载、或没有这个组合时为 nullptr。 */
	const FDreamTextureWorkflowMeta* GetCurrentWorkflow() const;

	//~ 表单
	void SetPrompt(const FString& InPrompt) { Prompt = InPrompt; }
	const FString& GetPrompt() const { return Prompt; }

	/** 当前工作流的分辨率候选；它没有这个参数、或候选不是整数时为空（界面隐藏下拉）。 */
	TArray<int32> GetResolutionOptions() const;
	void SetResolution(int32 InResolution);

	/** 实际会提交的分辨率：选过且仍在候选里就用它，否则取参数默认值。0 = 不提交这个参数。 */
	int32 GetResolution() const;

	void SetVariants(int32 InVariants);
	int32 GetVariants() const { return Variants; }

	void SetRandomSeed(bool bInRandom) { bRandomSeed = bInRandom; }
	bool IsRandomSeed() const { return bRandomSeed; }

	/** 固定种子。夹在 [0, GetMaxSeedForUI()]：界面上限和 Client 校验是同一个口径。 */
	void SetFixedSeed(int64 InSeed);
	int64 GetFixedSeed() const { return FixedSeed; }

	/**
	 * 界面上种子输入框的上限：按最大变体数留余量，所以不管变体数怎么改都不会让已填的种子变得非法。
	 * 提交时 Client 的 Validate 按当前变体数卡得更精确，两个口径指向同一个函数。
	 */
	static int64 GetMaxSeedForUI() { return FDreamTextureGenerateRequest::MaxFixedSeed(DreamTextureGenerateLimits::MaxVariants); }

	//~ 云端底图
	/**
	 * 当前选的出图模型是不是云端底图。云端出图要先选一个云端模型（后端 checkSource 会在提交时拦下没选的），
	 * 而且要有令牌、按量计费，所以界面上多出「云端模型」「画质」两项，提交前还要查这一路通不通。
	 */
	bool IsCloudSource() const;

	void LoadCloudModels();
	ECloudState GetCloudState() const { return CloudState; }
	const FString& GetCloudError() const { return CloudError; }

	/** 当前云端来源的清单；没拉到、或后端没有这个来源时为 nullptr。 */
	const FDreamTextureImagenProvider* GetCloudProvider() const;
	TArray<const FDreamTextureImagenModel*> GetCloudModels() const;

	/** 选了云端模型。不在清单里的 id 被忽略。 */
	void SetCloudModel(const FString& Id);

	/** 实际会提交的云端模型：选的还在清单里就用它，否则清单第一个（后端把推荐的排在前面）。清单没到时为空。 */
	FString GetCloudModelId() const;

	/** 画质候选，取自所选管线的 api_quality 参数。没有这个参数时为空（界面隐藏）。 */
	TArray<FString> GetCloudQualityOptions() const;
	void SetCloudQuality(const FString& Quality) { CloudQuality = Quality; }
	FString GetCloudQuality() const;

	/** 云端这一路现在走不走得通。走不通时 OutReason 是中文原因（没填令牌、清单没到、没选模型…）。 */
	bool CheckCloudReady(FText* OutReason = nullptr) const;

	//~ 许可
	FDreamTextureGenerateLicenseHint GetLicenseHint() const;

	//~ 能不能提交
	/** 只看连接这三级，不看表单。公开成静态函数是为了自动化测试能直接喂假连接验证分档。 */
	static FDreamTextureGenerateReadiness EvaluateReadiness(const TSharedPtr<FDreamTextureConnection>& Conn);

	FDreamTextureGenerateReadiness GetReadiness() const;

	/** 连接和表单都过关才返回 true；否则 OutReason 是中文原因。 */
	bool CanSubmit(FText* OutReason = nullptr) const;

	/** 按当前表单拼请求。拼不出（没选工作流）返回 false。 */
	bool BuildRequest(FDreamTextureGenerateRequest& OutRequest, FText* OutError = nullptr) const;

	void Submit();
	bool IsSubmitting() const { return bSubmitting; }

	//~ 启动后端并生成
	/**
	 * 后端进程没应答（第 1 级，且是连过了确实没应答，不是正在连），而设置里配了可执行文件时，
	 * 主按钮就是「启动后端并生成」：不用先跑去状态页点「启动后端」，一步到位。
	 * 其余情形（包括没配可执行文件）主按钮就是普通的提交。
	 */
	EPrimaryAction GetPrimaryAction() const;

	/** 主按钮此刻能不能点；不能点时 OutReason 是中文原因。 */
	bool CanActivatePrimary(FText* OutReason = nullptr) const;

	/** 点主按钮：按 GetPrimaryAction() 提交，或启动后端并生成。正在启动时什么都不做。 */
	void ActivatePrimary();

	/**
	 * 启动后端并生成。流程：
	 *   1. EnsureBackend：端口上有后端就附着，没有才按设置拉起来（并发调用只会起一个进程）；
	 *   2. 后端应答后，目录还没拉到就补拉；所选是云端出图模型，云端清单没到也等；
	 *   3. 按用户**当时**的表单提交。这一次提交不看连接的三级状态——后端刚应答，连接要过几秒才追上；
	 *      ComfyUI 此刻多半还在启动，任务会在后端排队、停在「等待 ComfyUI」，卡片上看得出来。
	 * 启动失败：EnsureBackend 给的中文原因原样放进 GetSubmitError()。
	 */
	void StartBackendAndSubmit();

	/** 能不能「启动后端并生成」；不能时 OutReason 是原因（没配可执行文件、没写提示词…）。 */
	bool CanAutoStart(FText* OutReason = nullptr) const;

	/**
	 * 取消。只是不再提交：已经发出去的启动请求不撤（后端起来了就让它活着），也绝不去停后端。
	 * 没在启动时什么都不做。
	 */
	void CancelAutoStart();

	EAutoStartState GetAutoStartState() const { return AutoStartState; }
	bool IsAutoStarting() const { return AutoStartState != EAutoStartState::Idle; }

	/** 设置里配了后端可执行文件吗。 */
	bool HasBackendExecutable() const;

	/**
	 * 后端进程不通、又没配可执行文件：界面该给一个跳到项目设置的入口。
	 * 配了的情形不需要——「启动后端并生成」本身就是出路。
	 */
	bool NeedsExecutableSetting() const;

	/**
	 * 后端进程不通，且设置里配了可执行文件：左栏即使没拉到目录也显示表单（写提示词、点「启动后端并生成」），
	 * 而不是只给一个「读不到工作流列表」。
	 */
	bool ShouldShowOfflineForm() const;

	/** 最近一次提交失败的原因；成功或再次提交后清空。 */
	const FText& GetSubmitError() const { return SubmitError; }

	//~ 本批
	const FString& GetBatchId() const { return BatchId; }
	const TArray<FRowPtr>& GetRows() const { return Rows; }
	const FDreamTextureJob* FindJob(const FString& JobId) const;
	FDreamTextureGenerateRowView GetRowView(const FRowPtr& Row) const;

	/** 本批里有任务没结束。 */
	bool HasActiveJobs() const;
	int32 GetNumFinished() const;

	void CancelJob(const FString& JobId);
	void CancelBatch();

	/** 把没结束的任务逐个问一遍后端。事件流可能丢事件，用户点「同步」时用。 */
	void SyncJobs();

	/** 把一个已完成的变体导入工程。 */
	void RequestImport(const FString& JobId);

	bool GetAutoImport() const;
	void SetAutoImport(bool bInAuto);

	/** 纯函数，不碰 Slate 控件；自动化测试直接验证各状态的说法。 */
	static FDreamTextureGenerateRowView EvaluateRow(int32 Number, const FDreamTextureJob* Job, bool bCancelRequested,
		const FDreamTextureImportStatus* ImportStatus);

	//~ 扩写
	void LoadRefineModels();
	const TArray<FString>& GetRefineModels() const { return RefineModels; }
	const FString& GetRefineDefaultModel() const { return RefineDefault; }
	const FString& GetRefineModelsError() const { return RefineModelsError; }

	/** 空串 = 用后端的默认模型。 */
	void SetRefineModel(const FString& Model) { RefineModel = Model; }
	const FString& GetRefineModel() const { return RefineModel; }

	void Refine();
	ERefineState GetRefineState() const { return RefineState; }
	const FString& GetRefinedPrompt() const { return RefinedPrompt; }
	const FString& GetRefinedModel() const { return RefinedModel; }
	const FString& GetRefineError() const { return RefineError; }

	/** 用扩写结果替换提示词。 */
	void AcceptRefine();
	void DiscardRefine();

	//~ 自动订阅的两个触发点（公开是为了测试直接调用）
	/** 链路变为 Live 时目录、扩写模型没拉到就再拉——后端刚起来、或断线重连后。 */
	void HandleLinkChanged(EDreamTextureLink NewLink);

private:
	void ApplyCatalog(uint64 Generation, bool bOK, FDreamTextureWorkflowCatalog&& InCatalog, const FString& Error);
	void ApplySubmitResult(uint64 Generation, bool bOK, TArray<FDreamTextureJob>&& Jobs, const FString& Error);

	/**
	 * CanSubmit 的实体。bAfterAutoStart 为真时跳过连接三级的检查——只有「启动后端并生成」
	 * 在后端刚应答后的那一次提交走这条路，理由见 StartBackendAndSubmit()。
	 */
	bool CheckSubmittable(FText* OutReason, bool bAfterAutoStart) const;
	void SubmitInternal(bool bAfterAutoStart);

	void HandleBackendEnsured(uint64 Generation, bool bOK, const FString& Error);
	void FinishAutoStart();
	void FailAutoStart(const FText& Why);
	void ApplyCloudModels(uint64 Generation, bool bOK, FDreamTextureImagenProviders&& Providers, const FString& Error);

	/** 选中的出图模型是云端的、而清单还没拉到（或上次失败了）就去拉。 */
	void MaybeLoadCloudModels();

	/** 目录到位后把选择落在存在的段上：之前选的还在就保留，不在了取默认。 */
	void EnsureSelection();

	void RestoreLatestBatch();
	void SetBatch(const FString& InBatchId, const TArray<FDreamTextureJob>& Jobs);

	FDreamTextureGenerateBackend Backend;

	ECatalogState CatalogState = ECatalogState::NotLoaded;
	FDreamTextureWorkflowCatalog Catalog;
	FString CatalogError;
	uint64 CatalogGeneration = 0;

	FString SourceId;
	FString DecomposeId;

	FString Prompt;
	int32 ResolutionChoice = 0;
	int32 Variants = 4;
	bool bRandomSeed = true;
	int64 FixedSeed = 12345;

	bool bSubmitting = false;
	FText SubmitError;
	uint64 SubmitGeneration = 0;

	EAutoStartState AutoStartState = EAutoStartState::Idle;
	uint64 AutoStartGeneration = 0;

	FString BatchId;
	TArray<FRowPtr> Rows;

	ECloudState CloudState = ECloudState::NotLoaded;
	FDreamTextureImagenProviders CloudProviders;
	FString CloudError;
	FString CloudModelChoice;
	FString CloudQuality;
	uint64 CloudGeneration = 0;

	TArray<FString> RefineModels;
	FString RefineDefault;
	FString RefineModelsError;
	bool bRefineModelsLoaded = false;
	bool bRefineModelsLoading = false;
	FString RefineModel;

	ERefineState RefineState = ERefineState::Idle;
	FString RefinedPrompt;
	FString RefinedModel;
	FString RefineError;
	uint64 RefineGeneration = 0;

	TWeakPtr<FDreamTextureConnection> BoundConnection;
	FDelegateHandle LinkHandle;
	bool bBoundExternal = false;
};
