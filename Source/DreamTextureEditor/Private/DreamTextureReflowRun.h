// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DreamTextureApi.h"
#include "DreamTextureGenerate.h"
#include "DreamTextureImportQueue.h"
#include "DreamTextureJobTracker.h"
#include "DreamTextureManifest.h"
#include "DreamTextureReflow.h"
#include "DreamTextureTextureExporter.h"
#include "DreamTextureUpload.h"
#include "UObject/WeakObjectPtr.h"

class UMaterialInstanceConstant;

/**
 * 回流一次运行的流程。从「点开始」到「新材质落进工程 / 选中的实例被改写」的七步，失败了要说清楚是哪一步。
 */
enum class EDreamTextureReflowStep : uint8
{
	None,

	/** 确保后端在：没起就拉起来。 */
	Backend,

	/** 把底色贴图从源数据导成 PNG。 */
	Export,

	/** 上传给后端（POST /api/uploads）。 */
	Upload,

	/** 提交生成请求（POST /api/generate），请求被后端校验。 */
	Submit,

	/** 后端在跑：ComfyUI 出图 / 分解。 */
	Generate,

	/** 把结果贴图拉到本地。 */
	Download,

	/** 落成资产、建材质实例。 */
	Import,

	/** 原地覆盖时，把新贴图换到选中的实例上。 */
	Apply,
};

/** 步骤的中文名，也是失败说明里「在「X」这一步失败」的 X。 */
FText DescribeReflowStep(EDreamTextureReflowStep Step);

/**
 * 一次运行要的全部输入。开始那一刻定死，之后用户在页面上再动表单也不影响这次运行。
 * 请求里还缺两样要运行中才知道的东西——上传后后端给的文件名、导出文件的指纹——由运行自己填。
 */
struct FDreamTextureReflowRunSpec
{
	EDreamTextureReflowMode Mode = EDreamTextureReflowMode::FillChannels;

	/**
	 * 这个材质的贴图是平铺的还是 UV 展开的。请求里的 tileable 已经按它填好；这里留着是因为
	 * 改写外观时 UV 展开的重绘幅度有硬上限，运行开始前要在最低一层再拦一次（页面上拦过了，这是兜底）。
	 */
	EDreamTextureUVKind UVKind = EDreamTextureUVKind::Tiled;

	bool bInPlace = false;

	/** 给人看的标题，如「补齐缺失通道 · MI_Rock」。 */
	FString Title;

	/** 源材质的资产路径与显示名。 */
	FString MaterialAssetPath;
	FString MaterialName;

	/** 只含「底图」那一行的解析结果，导出只导它。 */
	FDreamTextureMaterialAnalysis ExportAnalysis;
	FString SourceTexturePath;
	FString SourceTextureName;

	/** 导出时长边的上限。 */
	int32 ExportMaxEdge = DreamTextureReflow::DefaultExportMaxEdge;

	/** 除上传文件名和 maps_in 之外都填好了的请求。 */
	FDreamTextureGenerateRequest Request;

	/** 上传得到的文件名填进请求的哪个参数：补通道是 source_image，改写外观是 reference。 */
	FString UploadParamKey;

	/** 导入队列的选项（新建 / 原地覆盖不同）。 */
	FDreamTextureImportOptions ImportOptions;

	/** 原地覆盖：要改的目标实例与要改的几处。 */
	TWeakObjectPtr<UMaterialInstanceConstant> InPlaceTarget;
	TArray<FDreamTextureReflowOverwrite> Overwrites;

	/** 计划里带出来的说明（新建时没带过去的用户原有贴图等），成功后一并给用户看。 */
	TArray<FString> PlanNotes;
};

/**
 * 运行依赖的外部能力。默认转发到真实实现；自动化测试换成假的，
 * 这样不用起后端、不用写磁盘、也不用真的去排导入队列。
 */
struct FDreamTextureReflowRunHooks
{
	/** 确保后端在。回调一定异步。 */
	TFunction<void(TFunction<void(bool /*bOK*/, const FString& /*Error*/)>)> EnsureBackend;

	TFunction<TArray<FDreamTextureExportedImage>(const FDreamTextureMaterialAnalysis&, const FDreamTextureExportOptions&)> Export;
	TFunction<FString()> MakeTempDir;

	TFunction<void(const FString& /*FilePath*/, FDreamTextureUploadApi::FOnUploaded)> Upload;

	TFunction<TSharedPtr<FDreamTextureJobTracker>()> GetTracker;

	/** 排一个导入；同一个 id 已在队列里时返回 false。 */
	TFunction<bool(const FString& /*MaterialId*/, const FDreamTextureImportOptions&)> Enqueue;

	/** 拉结果的 manifest（给结果预览用，失败不影响运行）。 */
	TFunction<void(const FString& /*MaterialId*/, FDreamTextureApi::FOnManifest)> GetManifest;

	TFunction<FDreamTextureReflowApplyResult(UMaterialInstanceConstant*, const TArray<FDreamTextureReflowOverwrite>&, const FDreamTextureImportResult&)> ApplyInPlace;

	/**
	 * 订阅任务变化 / 导入状态变化，返回「退订」函数。拆成钩子是因为测试要自己决定什么时候、
	 * 以什么顺序把事件喂进来（真的追踪器与导入队列是进程内单例，测试不碰）。
	 */
	TFunction<TFunction<void()>(TFunction<void(const FDreamTextureJob&)>)> WatchJobs;
	TFunction<TFunction<void()>(TFunction<void(const FDreamTextureImportStatus&)>)> WatchImports;

	/** 当前的 FileSize（字节）等不用钩子；路径上的文件由 Upload 钩子自己读。 */

	static FDreamTextureReflowRunHooks MakeDefault();
};

/**
 * 回流的一次运行。
 *
 * ## 为什么是独立对象，不在页面模型里
 *
 * 一次生成动辄几十秒到几分钟，用户常常点完就把面板关了。运行归自己所有，并在跑的时候让自己活着
 * （KeepAliveWhileRunning），面板关掉以后任务照常推进、导入照常落、实例照常改——和生成页「面板关了也自动导入」
 * 是同一个理由。页面重新打开时从模型上找回最近的这次运行。
 *
 * ## 谁提交的任务
 *
 * 追踪器里的来源标签是 "reflow"。自动导入只认生成页（"generate"）的任务，回流的任务不会被它误导入成一套普通材质——
 * 回流的结果要按回流的规矩落（底色沿用原图、原地覆盖……），只能由本类导入。
 *
 * ## 取消
 *
 * 导出 / 上传 / 提交这几步是一瞬间的事，取消只是做个记号，下一步开始前发现了就停在当前这一步；
 * 生成这一步取消会打断后端的任务，等 job.failed(canceled) 回来。
 *
 * 所有委托都在游戏线程上广播。
 */
class FDreamTextureReflowRun : public TSharedFromThis<FDreamTextureReflowRun>
{
public:
	enum class EState : uint8
	{
		/** 还没开始。 */
		Idle,
		Running,
		Succeeded,
		Failed,
	};

	/** 后端任务的来源标签。 */
	static const TCHAR* GetOrigin() { return TEXT("reflow"); }

	FDreamTextureReflowRun(FDreamTextureReflowRunSpec InSpec, FDreamTextureReflowRunHooks InHooks = FDreamTextureReflowRunHooks::MakeDefault());
	~FDreamTextureReflowRun();

	DECLARE_MULTICAST_DELEGATE(FOnChanged);

	/** 状态、步骤、进度、结果任一变化。 */
	FOnChanged OnChanged;

	/** 开始。只能调一次。已经开始过再调什么都不做。 */
	void Start();

	/** 请求取消。已经结束的什么都不做。 */
	void Cancel();

	/**
	 * 运行期间让自己不被释放。结束（成功或失败）时放手。
	 * 默认不开：自动化测试里对象的生命周期归测试管；真实的页面在 Start 之前调它。
	 */
	void KeepAliveWhileRunning();

	/** 此刻靠 KeepAliveWhileRunning 活着的运行数。 */
	static int32 GetNumKeptAlive();

	EState GetState() const { return State; }
	bool IsRunning() const { return State == EState::Running; }
	bool IsFinished() const { return State == EState::Succeeded || State == EState::Failed; }

	/** 现在在哪一步；失败后是失败在哪一步。 */
	EDreamTextureReflowStep GetStep() const { return Step; }

	/** 当前一步的一句话说明（含「下载 2/4」「生成中 35%」这类）。失败后是一句完整的失败说明。 */
	const FText& GetMessage() const { return Message; }

	/** 0~1。不知道进度（导出、上传、等 ComfyUI…）时没有值。 */
	TOptional<float> GetProgress() const { return Progress; }

	const FDreamTextureReflowRunSpec& GetSpec() const { return Spec; }

	//~ 运行中陆续知道的东西
	const FString& GetExportedFile() const { return ExportedFile; }
	const FString& GetExportedSha() const { return ExportedSha; }
	const FString& GetUploadedName() const { return UploadedName; }
	const FString& GetJobId() const { return JobId; }
	const FString& GetMaterialId() const { return MaterialId; }

	/** 实际提交出去的请求（上传文件名与 maps_in 已填好）。提交之前为空。 */
	const FDreamTextureGenerateRequest& GetSubmittedRequest() const { return Submitted; }

	//~ 结果
	const FDreamTextureImportResult& GetImportResult() const { return ImportResult; }
	const FDreamTextureReflowApplyResult& GetApplyResult() const { return ApplyResult; }

	/** 新建时：新材质实例的对象路径；原地覆盖时：被改的那个实例的对象路径。 */
	const FString& GetResultInstancePath() const { return ResultInstancePath; }

	/** 后端结果的 manifest（给预览用）。拉到之前 bHasManifest 为假。 */
	bool HasManifest() const { return bHasManifest; }
	const FDreamTextureManifest& GetManifest() const { return Manifest; }

	//~ 以下两个是订阅的处理函数，公开是为了自动化测试直接喂数据
	void HandleJob(const FDreamTextureJob& Job);
	void HandleImport(const FDreamTextureImportStatus& Status);

private:
	void SetStep(EDreamTextureReflowStep NewStep, const FText& NewMessage, TOptional<float> NewProgress = TOptional<float>());
	void Fail(EDreamTextureReflowStep FailedStep, const FString& Reason);
	void Finish(const FText& Summary);
	void Release();

	/** 取消后在每一步的边界检查；返回 true 表示已经按取消收尾了，失败记在取消发生时正在做的那一步上。 */
	bool StopIfCancelRequested();

	void DoExport();
	void DoUpload();
	void DoSubmit();
	void DoImport();
	void DoApply();
	void FetchManifestForPreview();

	FDreamTextureReflowRunSpec Spec;
	FDreamTextureReflowRunHooks Hooks;

	EState State = EState::Idle;
	EDreamTextureReflowStep Step = EDreamTextureReflowStep::None;
	FText Message;
	TOptional<float> Progress;
	bool bCancelRequested = false;

	FString ExportedFile;
	FString ExportedSha;
	FString UploadedName;
	FDreamTextureGenerateRequest Submitted;
	FString JobId;
	FString MaterialId;

	/** 导入阶段最近一次看到的、没失败的阶段：失败时据此判断是「下载」还是「导入」那一步出的事。 */
	EDreamTextureImportStage LastImportStage = EDreamTextureImportStage::Queued;

	FDreamTextureImportResult ImportResult;
	FDreamTextureReflowApplyResult ApplyResult;
	FString ResultInstancePath;

	FDreamTextureManifest Manifest;
	bool bHasManifest = false;

	TFunction<void()> UnwatchJobs;
	TFunction<void()> UnwatchImports;

	TSharedPtr<FDreamTextureReflowRun> SelfKeepAlive;
	bool bKeepAlive = false;
};
