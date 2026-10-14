// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DreamTextureTypes.h"

class FJsonObject;

/**
 * 「在 UE 里生成」要用到的后端契约：工作流与段、生成请求、任务、提示词扩写。
 *
 * 全是纯数据 + 静态解析函数，不碰网络——网络那层是文件末尾的 FDreamTextureGenerateApi。
 * 这样请求体怎么拼、任务事件怎么解，都能拿固定 JSON 直接测，不用起后端。
 *
 * 出处（后端仓库 https://github.com/TypeDreamMoon/DreamTexture）：
 *   工作流与段   internal/api/server.go listWorkflows、internal/workflow/{template,segment,compose}.go
 *   生成请求     internal/job/runner.go 的 Request / Submit / resolveSeed
 *   任务         internal/store/store.go 的 Job、internal/job/events.go
 *   扩写         internal/api/refine.go
 * Web 端的参照实现是 DreamTextureDesktop 仓库（https://github.com/TypeDreamMoon/DreamTextureDesktop）的 web/src/views/GenerateView.vue。
 *
 * 解析一律容忍 null 与缺字段：Go 的 nil 切片序列化成 null 而不是 []，后端仓库里这个坑踩过两次。
 */

namespace DreamTextureGenerateLimits
{
	/** 一次最多几个变体。后端 Submit 会把更大的数静默夹成 8，这里直接拒绝，免得界面和后端各说各话。 */
	constexpr int32 MaxVariants = 8;

	/**
	 * JSON 数字能无损往返的上界（2^53-1），与后端 maxSafeSeed 同值。
	 *
	 * 种子要穿过好几层 JSON（入库、出库、manifest、发给前端），任何一环把它解成 float64，
	 * 超过 2^53 的部分都会被悄悄改写，于是 manifest 里记的种子复现不出原图。
	 */
	constexpr int64 MaxSafeSeed = (static_cast<int64>(1) << 53) - 1;

	/** 种子填它表示「每个变体各取一个随机种子」。 */
	constexpr int64 RandomSeed = -1;
}

//////////////////////////////////////////////////////////////////////////
// 工作流与段

/** 参数声明里的 type 是个开放字符串（string | int | float | bool | enum | image），界面据此选控件。 */
struct DREAMTEXTURECLIENT_API FDreamTextureParamSpec
{
	FString Key;
	FString Label;
	FString Type;
	FString Note;

	/** 由后端自己填、界面上不显示（例如云端底图落盘后的文件名）。 */
	bool bHidden = false;
	bool bMultiline = false;

	/**
	 * 提交时必须给（后端会在提交就拦下，而不是等 ComfyUI 去校验）。目前是「已有底图」出图段的 source_image：
	 * 它不生成，唯一的输入就是上传的那张图。
	 */
	bool bRequired = false;

	/**
	 * 给界面的控件提示，用于选项运行时才知道的参数。目前只有 "imagen-model"：云端模型清单要现问服务端，
	 * 写不进静态的参数声明。界面据此找出「该把云端模型填到哪个参数里」，不认识任何具体参数名。
	 */
	FString Widget;

	/**
	 * enum 的候选。JSON 里是数字或字符串，这里统一成文本：整数不带小数点（1024 而不是 1024.0），
	 * 否则界面上会出现「1024.0」。
	 */
	TArray<FString> Options;

	FString Default;
	bool bHasDefault = false;

	TOptional<double> Min;
	TOptional<double> Max;

	/** 全部候选都是整数时取出来；有一个不是就返回 false。 */
	bool TryGetIntOptions(TArray<int32>& Out) const;

	/** 默认值按整数读；没有或读不出来返回 Fallback。 */
	int32 GetDefaultInt(int32 Fallback) const;

	static bool FromJson(const TSharedPtr<FJsonObject>& Obj, FDreamTextureParamSpec& Out);
};

/** 工作流声明里的许可提示（internal/workflow.LicenseNotice）。 */
struct DREAMTEXTURECLIENT_API FDreamTextureLicenseNotice
{
	FString Component;
	FString License;
	bool bCommercial = false;

	/** 想换掉这个组件时该换哪一段。 */
	FString ReplaceableSegment;
	TArray<FString> Alternatives;

	static bool FromJson(const TSharedPtr<FJsonObject>& Obj, FDreamTextureLicenseNotice& Out);
};

enum class EDreamTextureSegmentKind : uint8
{
	/** 完整模板（组合出来的，或用户自己导入的）：自己从头跑到尾，能直接提交。 */
	None,
	/** 出图段：产出一张底图。 */
	Source,
	/** 分解段：把底图拆成 PBR 通道。 */
	Decompose,
};

/**
 * 一份工作流或一段的元信息（internal/workflow.Meta）。
 *
 * ## 组合管线怎么表示
 *
 * 材质管线分成「出图段 × 分解段」，后端在**加载期**把两两拼成一份普通模板，id 是
 * `<出图段 id>.<分解段 id>`（compose.go 的 ComposedID，分隔符用 '.' 因为这个 id 会进接口路径和查询串）。
 * 提交时只需要这个组合 id，后端不知道也不关心「段」。组合出来的工作流在 `workflows` 里，
 * 带 source_segment / decompose_segment 两个字段；单独的段在 `segments` 里，自己提交不了。
 */
struct DREAMTEXTURECLIENT_API FDreamTextureWorkflowMeta
{
	FString Id;

	/** material | image。 */
	FString Kind;

	int32 Version = 0;
	FString Name;
	FString Style;
	FString Description;
	int32 Resolution = 0;
	bool bTileable = false;

	EDreamTextureSegmentKind Segment = EDreamTextureSegmentKind::None;

	/** 出图段产出的画面类型（realistic | stylized）。分解段用 ExpectsDomain 声明吃得下哪些。 */
	FString Domain;
	TArray<FString> ExpectsDomain;

	/** 只有组合出来的管线才有：由哪两段拼的，以及两段搭不上时后端给的提醒。 */
	FString SourceSegment;
	FString DecomposeSegment;
	FString Mismatch;

	/** 底图来自云端（声明里有 source 字段）。云端出图要先选一个云端模型，按量计费。 */
	bool bCloudSource = false;
	FString CloudProvider;

	/** 云端拿回来就是成品，不经过 ComfyUI。material 管线里没有这种。 */
	bool bDirectOutput = false;

	/**
	 * 这条管线涉及的**全部**许可提示。组合出来的是两段的并集。
	 *
	 * 判断能不能商用必须看这个并集：只看一边，「云端底图 + CHORD」会把 CHORD 的 research-only 丢掉，
	 * 那是会让人拿去商用的错误（后端为此栽过，见 docs/workflow-segments.md「许可要取并集」）。
	 */
	TArray<FDreamTextureLicenseNotice> Licenses;

	TArray<FDreamTextureParamSpec> Params;
	TArray<FDreamTextureParamSpec> Advanced;

	bool IsComposed() const { return !SourceSegment.IsEmpty() && !DecomposeSegment.IsEmpty(); }

	/** 与后端 Meta.Commercial() 同口径：没有任何一条许可标着不可商用才算可商用。没有许可提示也是可商用。 */
	bool IsCommercial() const;

	/** 不可商用的原因，格式与后端一致（"组件: 许可; 组件: 许可"）。可商用时为空。 */
	FString GetNonCommercialReason() const;

	/** 先找基础参数再找高级参数。没有返回 nullptr。 */
	const FDreamTextureParamSpec* FindParam(const FString& Key) const;

	/** 按控件提示找参数（见 FDreamTextureParamSpec::Widget）。没有返回 nullptr。 */
	const FDreamTextureParamSpec* FindParamByWidget(const FString& Widget) const;

	static bool FromJson(const TSharedPtr<FJsonObject>& Obj, FDreamTextureWorkflowMeta& Out);
};

/** GET /api/workflows 的整个响应：能直接提交的 workflows，和只供界面渲染下拉的 segments。 */
struct DREAMTEXTURECLIENT_API FDreamTextureWorkflowCatalog
{
	TArray<FDreamTextureWorkflowMeta> Workflows;
	TArray<FDreamTextureWorkflowMeta> Segments;

	/** 拼组合 id。与后端 compose.go 的 ComposedID 一致，不要另发明分隔符。 */
	static FString MakeComposedId(const FString& SourceId, const FString& DecomposeId);

	const FDreamTextureWorkflowMeta* FindWorkflow(const FString& Id) const;
	const FDreamTextureWorkflowMeta* FindSegment(const FString& Id) const;

	/** 两段拼得起来就返回组合出来的工作流；后端没有拼出这个组合（比如段是新加的、还没重载）返回 nullptr。 */
	const FDreamTextureWorkflowMeta* FindComposed(const FString& SourceId, const FString& DecomposeId) const;

	/**
	 * **生成页能用的**出图段 / 分解段，按 id 排好序——不是目录里全部的段（要全部的读 Segments）：
	 *
	 *   - 出图段只列「靠提示词出图」的，即基础参数里有 prompt 的。「已有底图」(src-upload-v1) 不生成，
	 *     唯一的输入是必填的上传图，列进来的话这个页面没有提示词可填，选了提交必然 400。
	 *     与 Web 端 GenerateView.vue 的 sourceSegs 同一个判据。
	 *   - 分解段不列带必填参数的：生成页没有地方让人填它。
	 *
	 * 图片类（kind=image）没有段，不在其中。
	 */
	TArray<const FDreamTextureWorkflowMeta*> GetSourceSegments() const;
	TArray<const FDreamTextureWorkflowMeta*> GetDecomposeSegments() const;

	/** 响应体根对象 → 目录。workflows 与 segments 缺失、为 null、为空都是合法的。 */
	static bool FromJson(const TSharedPtr<FJsonObject>& Root, FDreamTextureWorkflowCatalog& Out);

	/** 同上，直接吃 JSON 文本。不是合法 JSON 对象时返回 false。界面模块不依赖 Json 模块，测试要拿固定数据时用它。 */
	static bool FromJsonString(const FString& Json, FDreamTextureWorkflowCatalog& Out);
};

//////////////////////////////////////////////////////////////////////////
// 生成请求

/**
 * 请求里的 derived_from：这次生成是从工程里已有的一份材质派生的（回流，M8/M9）。
 * 后端原样写进 manifest（docs/manifest-v1.md「derived_from 条目」），不升 schema_version。
 *
 * 后端在提交时校验：kind、mode 是枚举，拼错直接拒绝；maps_in 的指纹必须是 16 位十六进制。
 * 这里在发请求之前先校验一遍——同一套规矩，错了在本地就能说清楚是哪个字段。
 */
struct DREAMTEXTURECLIENT_API FDreamTextureDerivedFrom
{
	static const TCHAR* KindUEMaterial() { return TEXT("ue_material"); }

	/** 原图原样当底图进分解段（出图段 src-upload-v1）。 */
	static const TCHAR* ModeFillChannels() { return TEXT("fill_channels"); }

	/** 原图当参考图走 img2img（本地出图段的 reference）。 */
	static const TCHAR* ModeRestyle() { return TEXT("restyle"); }

	/** 空 = 请求不带 derived_from。 */
	FString Kind;

	/** 源资产在工程里的路径，如 /Game/Materials/MI_Rock。后端只原样记下。 */
	FString AssetPath;

	/** 源编辑器的引擎版本，如 5.8.2。 */
	FString Engine;

	FString Mode;

	/** 通道名 → 送上去的那份文件的 SHA-256 前 16 位（小写十六进制），不是产物的指纹。 */
	TMap<FString, FString> MapsIn;

	bool IsSet() const { return !Kind.IsEmpty() || !Mode.IsEmpty() || !AssetPath.IsEmpty() || MapsIn.Num() > 0; }

	/** 16 位十六进制（大小写都认，后端会规整成小写）。 */
	static bool IsValidFingerprint(const FString& Text);

	bool Validate(FString& OutError) const;
};

/**
 * POST /api/generate 的请求（job.Request）。
 *
 * 参数是开放的 map：生成页放 prompt / resolution / seed，别的页面（回流）放别的键，
 * 这一层不认识任何具体参数名，只管种子这一条危险的规矩。
 */
struct DREAMTEXTURECLIENT_API FDreamTextureGenerateRequest
{
	/** 完整模板或组合 id（见 FDreamTextureWorkflowMeta 的说明）。 */
	FString WorkflowId;

	/** 变体数，1~MaxVariants。同一批的变体种子互不相同：固定种子时是 Seed+0、Seed+1…，随机时各取各的。 */
	int32 Variants = 1;

	/** 材质展示名，留空则后端取提示词。 */
	FString Name;

	TMap<FString, FString> StringParams;

	/** 整数参数。种子在这里，键名 "seed"。 */
	TMap<FString, int64> IntParams;

	TMap<FString, double> FloatParams;
	TMap<FString, bool> BoolParams;

	/** 回流的出身声明。没设（IsSet() 为假）就不写进请求。只有材质管线能带。 */
	FDreamTextureDerivedFrom DerivedFrom;

	/** 种子参数在 params 里的键名，后端按它找。 */
	static const TCHAR* SeedKey() { return TEXT("seed"); }

	void SetSeed(int64 Seed) { IntParams.Add(SeedKey(), Seed); }

	/** 没设种子（后端按随机处理）或设成 RandomSeed 都算随机。 */
	bool IsRandomSeed() const;

	/**
	 * 固定种子最大能填多少。
	 *
	 * 不是 MaxSafeSeed 本身：变体 i 的种子是 Seed+i，种子填满上限时第二个变体就越界了。
	 * 界面上的上限和 Validate() 都用这个函数，两边才对得上。
	 */
	static int64 MaxFixedSeed(int32 Variants);

	/**
	 * 把提示词的前 60 个字符作成展示名，与 Web 端 `prompt.slice(0, 60)` 一致。
	 * 换行折成空格；60 的位置正好劈开一个代理对（emoji 之类）时舍掉那半个，免得名字里混进乱码。
	 */
	static FString MakeNameFromPrompt(const FString& Prompt);

	/** 检查能不能发。不通过时 OutError 是中文原因，可直接显示。 */
	bool Validate(FString& OutError) const;

	/** 校验并序列化。种子等整数走 int64 写出，不经过 float 的 %g，所以 2^53-1 原样写成 9007199254740991。 */
	bool ToJsonString(FString& OutJson, FString& OutError) const;
};

//////////////////////////////////////////////////////////////////////////
// 任务

enum class EDreamTextureJobStatus : uint8
{
	/** 后端给了个不认识的状态。按未结束处理，别当成完成。 */
	Unknown,
	Queued,
	Running,
	Succeeded,
	Failed,
	Canceled,
};

/** 英文标识（日志与断言用）。界面文案自己映射。 */
DREAMTEXTURECLIENT_API const TCHAR* LexToString(EDreamTextureJobStatus Status);

/** queued | running | succeeded | failed | canceled；别的值返回 Unknown。 */
DREAMTEXTURECLIENT_API EDreamTextureJobStatus ParseJobStatus(const FString& Text);

/** 一个生成任务（store.Job）。一次提交 N 个变体就是 N 个任务，同一个 BatchId。 */
struct DREAMTEXTURECLIENT_API FDreamTextureJob
{
	FString Id;

	/** 成功后素材库里的材质 ID。任务一创建就有，但材质要到 Succeeded 才真的存在。 */
	FString MaterialId;
	FString WorkflowId;
	FString BatchId;
	FString PromptId;

	EDreamTextureJobStatus Status = EDreamTextureJobStatus::Unknown;

	/** 0~1。后端给的是采样进度 × 0.9，再留出分解与落盘；已完成是 1。 */
	float Progress = 0.0f;

	/**
	 * 阶段说明，后端原文（中文），如「提交中」「生成中」「落盘中」「等待 ComfyUI」。
	 * 代码里拿它逐字比较；显示用 LocalizeStage。
	 */
	FString Stage;

	/** 失败或取消的原因，中文。 */
	FString Error;

	/** 取自 params：种子（后端已经把「随机」解成具体数）、分辨率、展示名。 */
	bool bHasSeed = false;
	int64 Seed = 0;
	int32 Resolution = 0;
	FString Name;

	/** 取自 params.__derived_from：后端记下的回流出身（没有就是空的）。 */
	FDreamTextureDerivedFrom DerivedFrom;

	FDateTime CreatedAt;

	bool IsTerminal() const;

	/**
	 * 界面上显示的阶段文字。后端的阶段名写死成中文（后端 internal/job 里的 runner.go、image.go、source.go），
	 * 认得的几种换成可本地化的英文源文；认不得的（后端以后新加的）原样显示，不猜。空串返回空 FText。
	 */
	static FText LocalizeStage(const FString& Stage);

	/**
	 * 取消的原因是不是后端那句通用的「已取消」（或干脆没有）。是的话界面上只显示「已取消」这个状态，
	 * 不再把后端原文重复一遍；别的原因（比如以后后端写明是谁取消的）照常显示。
	 */
	static bool IsPlainCancelReason(const FString& Error);

	/**
	 * 生命周期的先后：Unknown 0 < Queued 1 < Running 2 < 终态 3。
	 * 追踪器用它保证状态只往前走——事件和 REST 的应答谁先到都有可能。
	 */
	int32 GetRank() const;

	/** 任务对象 → 结构体。缺 id 的不算任务。 */
	static bool FromJson(const TSharedPtr<FJsonObject>& Obj, FDreamTextureJob& Out);

	/**
	 * job.queued / job.progress / job.done / job.failed 事件 → 任务（事件根对象的 job 字段）。
	 * 别的事件类型、或没带 job 的，返回 false。
	 */
	static bool FromEvent(const FDreamTextureEvent& Event, FDreamTextureJob& Out);
};

//////////////////////////////////////////////////////////////////////////
// 提示词扩写

/** purpose 决定后端用哪套系统提示：材质要带上正交平光可平铺那套硬约束，普通图片不带。 */
enum class EDreamTextureRefinePurpose : uint8
{
	Texture,
	Image,
};

/** GET /api/prompts/models。列不出来不算错：error 非空、models 为空，界面上仍可用默认模型。 */
struct DREAMTEXTURECLIENT_API FDreamTextureRefineModels
{
	TArray<FString> Models;

	/** 后端默认的扩写模型。 */
	FString Default;

	/** 列不出模型的原因（网关不提供 /models、令牌没有列举权限…）。空 = 列出来了。 */
	FString Error;

	static bool FromJson(const TSharedPtr<FJsonObject>& Root, FDreamTextureRefineModels& Out);
};

/** POST /api/prompts/refine 的结果。只是结果，不替用户改——用不用由用户决定。 */
struct DREAMTEXTURECLIENT_API FDreamTextureRefineResult
{
	FString Prompt;
	FString Model;
	int64 ElapsedMs = 0;

	static bool FromJson(const TSharedPtr<FJsonObject>& Root, FDreamTextureRefineResult& Out);
};

//////////////////////////////////////////////////////////////////////////
// 云端底图的模型清单

/** 云端来源上的一个模型（imagen.Model）。 */
struct DREAMTEXTURECLIENT_API FDreamTextureImagenModel
{
	FString Id;
	FString Label;

	/** 一句话说明，界面直接显示（各模型的速度与花费差着一个量级，光看名字选不出来）。 */
	FString Note;

	/** 认得的模型。false = 从服务端列表里发现的新模型，参数表只能靠推定。 */
	bool bKnown = false;

	static bool FromJson(const TSharedPtr<FJsonObject>& Obj, FDreamTextureImagenModel& Out);
};

/** 一个云端来源（imagen provider）及它现在能用的模型。 */
struct DREAMTEXTURECLIENT_API FDreamTextureImagenProvider
{
	FString Id;
	FString Label;

	/** 填了令牌。没填时 Models 一定是空的，不是错误——界面要指引去设置。 */
	bool bConfigured = false;

	TArray<FDreamTextureImagenModel> Models;

	/** 有令牌但列不出模型的原因（网关不通、组织没验证…）。 */
	FString Error;

	static bool FromJson(const TSharedPtr<FJsonObject>& Obj, FDreamTextureImagenProvider& Out);
};

/** GET /api/imagen/providers 的整个响应。 */
struct DREAMTEXTURECLIENT_API FDreamTextureImagenProviders
{
	TArray<FDreamTextureImagenProvider> Providers;

	/** 没有返回 nullptr。 */
	const FDreamTextureImagenProvider* Find(const FString& Id) const;

	static bool FromJson(const TSharedPtr<FJsonObject>& Root, FDreamTextureImagenProviders& Out);
};

//////////////////////////////////////////////////////////////////////////
// 网络

/**
 * 生成相关的 REST 接口。全是静态函数，基地址每次现取 UDreamTextureSettings。
 * 回调保证在游戏线程、且一定是异步的（走 DreamTexture::SendRequest）。
 * 这层不管调用方的生命周期：回调里要碰 UI 对象，调用方自己捕获弱指针。
 */
class DREAMTEXTURECLIENT_API FDreamTextureGenerateApi
{
public:
	using FOnCatalog = TFunction<void(bool bOK, FDreamTextureWorkflowCatalog&& Catalog, const FString& Error)>;
	using FOnSubmitted = TFunction<void(bool bOK, TArray<FDreamTextureJob>&& Jobs, const FString& Error)>;

	/** bNotFound：后端明确说这个任务不存在（404）。和「没连上」是两回事——前者该判这个任务没了，后者只是下次再问。 */
	using FOnJob = TFunction<void(bool bOK, FDreamTextureJob&& Job, const FString& Error, bool bNotFound)>;
	using FOnDone = TFunction<void(bool bOK, const FString& Error)>;
	using FOnRefineModels = TFunction<void(bool bOK, FDreamTextureRefineModels&& Models, const FString& Error)>;
	using FOnRefined = TFunction<void(bool bOK, FDreamTextureRefineResult&& Result, const FString& Error)>;
	using FOnImagenProviders = TFunction<void(bool bOK, FDreamTextureImagenProviders&& Providers, const FString& Error)>;

	/** GET /api/workflows。 */
	static void ListWorkflows(FOnCatalog Done);

	/** POST /api/generate，成功是 202 + {"jobs":[…]}。请求不合法时不发请求，直接异步回调失败。 */
	static void Submit(const FDreamTextureGenerateRequest& Request, FOnSubmitted Done);

	/** GET /api/jobs/{id}。断线重连后补状态用。 */
	static void GetJob(const FString& JobId, FOnJob Done);

	/** POST /api/jobs/{id}/cancel。排队中的立刻判负，正在跑的会打断 ComfyUI；已结束的后端回 400。 */
	static void CancelJob(const FString& JobId, FOnDone Done);

	/** GET /api/prompts/models。 */
	static void ListRefineModels(FOnRefineModels Done);

	/** POST /api/prompts/refine。Model 留空用后端的默认。后端最长等 90 秒。 */
	static void RefinePrompt(const FString& Prompt, EDreamTextureRefinePurpose Purpose, const FString& Model, FOnRefined Done);

	/** GET /api/imagen/providers。选了云端底图的出图模型时用它列可选的云端模型。 */
	static void ListImagenProviders(FOnImagenProviders Done);
};
