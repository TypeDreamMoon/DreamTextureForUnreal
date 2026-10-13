// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DreamTextureGenerate.h"
#include "DreamTextureReflow.h"
#include "DreamTextureReflowModel.h"
#include "DreamTextureReflowRun.h"
#include "DreamTextureTypes.h"

class FDreamTextureConnection;

/** 通道勾选区的一行：界面画什么、能不能勾、为什么。纯数据，每次现算。 */
struct FDreamTextureReflowChannelView
{
	/** manifest 通道名。 */
	FString Channel;

	/** 「底色」「法线」… */
	FText Label;

	/** 材质里已经有这个通道了吗。 */
	bool bPresent = false;

	/** 现状一句话：「缺失」「已有：T_Rock_N（参数 Normal）」「在 T_ORM 里（与 AO、金属度同一张）」。 */
	FText Status;

	bool bChecked = false;

	/** false = 勾不了（补通道时的底色）。Tooltip 里说明。 */
	bool bEnabled = true;

	/** 勾上了、但选了「原地覆盖」而这个通道原地改不了。界面画个警示，运行时会跳过它。 */
	bool bInPlaceBlocked = false;

	/** 悬停说明：为什么锁、为什么原地改不了、多张候选只处理第一张……没话说时为空。 */
	FText Tooltip;
};

/** 选中的分解模型有什么要提醒的。 */
struct FDreamTextureReflowDecomposeHint
{
	/** 许可：不可商用（CHORD）。界面标「仅限研究用途」。 */
	bool bNonCommercial = false;
	FString LicenseReason;

	/** 后端给的「两段搭不上」的提醒（手绘图喂给 CHORD…）。只提醒不拦。 */
	FString Mismatch;

	/**
	 * UV 展开的贴图配上依赖「首尾相接」假设的分解模型（CHORD）：它在平铺素材上训练，UV 展开图对它是分布外输入。
	 * 只提醒不拦——要不要试是用户的事。
	 */
	bool bOutOfDistribution = false;
	FString OutOfDistributionText;
};

/** 结果预览画什么。平铺看后端出的 3×3 平铺预览；UV 展开改成「原图 | 结果」并排。 */
struct FDreamTextureReflowPreviewPlan
{
	enum class EKind : uint8
	{
		/** 还没有结果可看（没成功、或还没拿到 manifest）。 */
		None,

		/** 一张：后端的 3×3 平铺预览。 */
		Tiled,

		/** 两张并排：原图 | 结果。 */
		SideBySide,
	};

	EKind Kind = EKind::None;

	/** 缩略图缓存的键（见 MakeFileKey / MakeMaterialFileKey）。Tiled 只有 LeftKey。 */
	FString LeftKey;
	FString RightKey;

	FText LeftLabel;
	FText RightLabel;
};

/** 回流页动作区依赖的外部能力。默认转发到真实实现；自动化测试换成假的。 */
struct FDreamTextureReflowActionsBackend
{
	TFunction<void(FDreamTextureGenerateApi::FOnCatalog)> ListWorkflows;
	TFunction<TSharedPtr<FDreamTextureConnection>()> GetConnection;

	/** 引擎版本，写进 derived_from.engine，如 5.8.2。 */
	TFunction<FString()> GetEngineVersion;

	/** 造一次运行。默认用真实钩子并让它在面板关了以后也活着；测试里换成带假钩子的。 */
	TFunction<TSharedRef<FDreamTextureReflowRun>(FDreamTextureReflowRunSpec&&)> MakeRun;

	static FDreamTextureReflowActionsBackend MakeDefault();
};

/**
 * 回流页动作区的状态（M8 平铺路径；UV 展开在 M9 接上）：
 * 两个模式、要覆盖的通道、分解模型、提示词与重绘幅度、新建还是原地覆盖、能不能开始、一次运行。
 *
 * 为什么单拎成不含 Slate 的类：这里的分支比页面本身还多（模式 × 材质种类 × 勾选 × 后端能力 × 连接三级），
 * 全塞在控件里没法测。拎出来之后注入假后端和假运行钩子，逐个分支验证。
 *
 * 它挂在 FDreamTextureReflowModel 上：选区、解析结果、导出设置都问模型，自己只管「在这个材质上要做什么」。
 * 模型一变（换了材质、用户指认了通道、重新取了选择），这里就重新算通道现状；
 * 换了材质则表单回到默认（勾选、模式的默认值、落回方式）。
 *
 * 所有委托都在游戏线程上广播。
 */
class FDreamTextureReflowActions : public TSharedFromThis<FDreamTextureReflowActions>
{
public:
	enum class ECatalogState : uint8
	{
		NotLoaded,
		Loading,
		Ready,
		Failed,
	};

	explicit FDreamTextureReflowActions(TSharedRef<FDreamTextureReflowModel> InModel, FDreamTextureReflowActionsBackend InBackend = FDreamTextureReflowActionsBackend::MakeDefault());
	~FDreamTextureReflowActions();

	/**
	 * 订阅模型与长连接、拉目录。不放构造函数：AddSP 要求对象已被共享指针持有。
	 * bBindExternal 为假时不订阅长连接（测试直接调 Handle* 喂数据）；目录照拉。
	 */
	void Initialize(bool bBindExternal = true);

	DECLARE_MULTICAST_DELEGATE(FOnChanged);
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnNotice, const FText& /*Message*/, bool /*bIsError*/);

	/** 表单任何一处变化（含运行状态变化）。 */
	FOnChanged OnChanged;

	/** 要提示用户的一句话。 */
	FOnNotice OnNotice;

	TSharedRef<FDreamTextureReflowModel> GetModel() const { return Model; }

	//~ 目录
	void LoadCatalog();
	ECatalogState GetCatalogState() const { return CatalogState; }
	const FString& GetCatalogError() const { return CatalogError; }
	const FDreamTextureWorkflowCatalog& GetCatalog() const { return Catalog; }

	/** 「已有底图」出图段：声明里有 source_image（image 类型）参数的出图段。没有（旧后端）返回 nullptr。 */
	const FDreamTextureWorkflowMeta* FindUploadSource() const;

	/** 能做「改写外观」的出图段：本地的（非云端）、有 reference 与 denoise 参数。按 id 排序。 */
	TArray<const FDreamTextureWorkflowMeta*> GetRestyleSources() const;

	/** 分解段，按 id 排序。读 Segments 而不是生成页那个过滤过的列表。 */
	TArray<const FDreamTextureWorkflowMeta*> GetDecomposeOptions() const;

	//~ 模式
	EDreamTextureReflowMode GetMode() const { return Mode; }
	void SetMode(EDreamTextureReflowMode InMode);

	//~ 平铺 / UV 展开（M9）
	EDreamTextureUVKind GetUVKind() const { return UVKind; }

	/**
	 * 选平铺还是 UV 展开，按材质资产记住（Saved/DreamTexture 下，和用户指认同一个文件，不写进用户的包）。
	 * 记不下来（不是工程里的资产、写盘失败）时发通知说明；这次编辑器会话里的选择仍然有效。
	 */
	void SetUVKind(EDreamTextureUVKind Kind);

	/** 这个分解段在当前平铺方式下是不是「分布外」。 */
	bool IsOutOfDistribution(const FDreamTextureWorkflowMeta& Segment) const;

	//~ 底图（送上去的那张贴图）
	/** 解析结果里能当底图的贴图的路径。 */
	TArray<FString> GetSourceCandidates() const;
	void SetSourceTexture(const FString& TexturePath);

	/** 实际会送上去的那张：选过且仍在候选里就用它，否则第一张。没有时为空。 */
	FString GetSourceTexturePath() const;
	const FDreamTextureMaterialTexture* GetSourceTexture() const;

	//~ 分解模型 / 改写用的出图段
	void SetDecomposeId(const FString& Id);
	FString GetDecomposeId() const;
	void SetRestyleSourceId(const FString& Id);
	FString GetRestyleSourceId() const;

	/** 当前选择拼出来的工作流；目录没到、或没有这个组合时为 nullptr。 */
	const FDreamTextureWorkflowMeta* GetCurrentWorkflow() const;

	FDreamTextureReflowDecomposeHint GetDecomposeHint() const;

	/**
	 * 下拉里一项的文字：「CHORD 分解 · 仅限研究用途」；UV 展开时依赖平铺假设的再加「· 分布外，质量不保证」。
	 */
	static FText DescribeSegment(const FDreamTextureWorkflowMeta& Segment, EDreamTextureUVKind Kind = EDreamTextureUVKind::Tiled);

	/**
	 * 这个分解段的结果依赖「图是首尾相接的」这个假设：声明里带 tileable 参数（关掉要靠包装节点，
	 * 见后端 docs/comfyui-notes.md「不要平铺时」）。传统派生没有任何依赖无缝的步骤，不带这个参数。
	 * 按声明判断而不是认名字：以后加的分解模型照样适用。
	 */
	static bool IsTileDependent(const FDreamTextureWorkflowMeta& Segment);

	//~ 通道
	const TArray<FDreamTextureReflowChannelState>& GetChannelStates() const { return States; }
	const TSet<FString>& GetSelection() const { return Selection; }
	bool IsChannelChecked(const FString& Channel) const { return Selection.Contains(Channel); }

	/** 勾上或取消一个通道；同一张打包图里的跟着动，被锁住的不动。 */
	void SetChannelChecked(const FString& Channel, bool bChecked);

	/** 回到默认勾选（缺的勾上）。 */
	void ResetSelection();

	TArray<FDreamTextureReflowChannelView> GetChannelViews() const;

	//~ 提示词与重绘幅度（改写外观）
	void SetPrompt(const FString& InPrompt);
	const FString& GetPrompt() const { return Prompt; }

	void SetDenoise(double InDenoise);

	/**
	 * 实际会提交的重绘幅度：用户动过就是他的值（夹在后端声明的范围内），否则取默认——
	 * 平铺取 0.55，UV 展开取 0.30（UV 展开有 0.35 的硬上限，默认值得在它下面）。
	 * 注意这里不按 UV 的上限去夹：超限是要拦住并说明的，悄悄改小用户给的值是替他做主。
	 */
	double GetDenoise() const;

	/** UV 展开的改写外观，重绘幅度超过硬上限。超了 OutReason 是人话说明（重绘会破坏 UV 对应关系）。 */
	bool IsDenoiseBlocked(FString* OutReason = nullptr) const;

	//~ 分辨率
	TArray<int32> GetResolutionOptions() const;
	void SetResolution(int32 InResolution);
	int32 GetResolution() const;

	//~ 落回
	void SetInPlace(bool bInInPlace);
	bool IsInPlace() const { return bInPlace; }

	/** 原地覆盖对选中的材质成不成立。不成立时 OutReason 是原因（界面据此置灰并说明）。 */
	bool CanOverwriteInPlace(FString* OutReason = nullptr) const;

	//~ 运行
	/** 现在能不能开始。不能时 OutReason 是中文原因。 */
	bool CanRun(FText* OutReason = nullptr) const;

	/** 能开始、但有话要说的（后端没起，点开始时会先启动它）。 */
	FText GetRunNote() const;

	/** 按当前表单拼一次运行的全部输入。拼不出时 OutError 是原因。 */
	bool BuildSpec(FDreamTextureReflowRunSpec& OutSpec, FText* OutError = nullptr) const;

	void Run();
	void CancelRun();

	/** 最近一次运行（属于当前这个材质的）；没有为空。 */
	TSharedPtr<FDreamTextureReflowRun> GetRun() const { return CurrentRun; }
	bool IsRunActive() const { return CurrentRun.IsValid() && CurrentRun->IsRunning(); }

	/** 结果预览画什么（纯数据，每次现算）。 */
	FDreamTextureReflowPreviewPlan GetPreviewPlan() const;

	/** 缩略图缓存的键：本地文件 / 后端材质里的某个文件。预览的缓存按键认出去哪取。 */
	static FString MakeFileKey(const FString& FilePath);
	static FString MakeMaterialFileKey(const FString& MaterialId, const FString& FileName);

	/** 链路变为 Live 时目录没拉到就再拉（后端刚起来、或断线重连后）。公开给测试直接调。 */
	void HandleLinkChanged(EDreamTextureLink NewLink);

	/** 模型变了（换了材质、重新解析…）。公开给测试直接调。 */
	void HandleModelChanged();

private:
	void ApplyCatalog(uint64 Generation, bool bOK, FDreamTextureWorkflowCatalog&& InCatalog, const FString& Error);
	void HandleRunChanged();

	/** 重新算通道现状、底图候选、勾选。 */
	void RebuildStates(bool bMaterialChanged);

	/** 选中材质的资产路径（解析失败 / 没选时为空）。 */
	FString GetCurrentAssetPath() const;

	TSharedRef<FDreamTextureReflowModel> Model;
	FDreamTextureReflowActionsBackend Backend;

	ECatalogState CatalogState = ECatalogState::NotLoaded;
	FDreamTextureWorkflowCatalog Catalog;
	FString CatalogError;
	uint64 CatalogGeneration = 0;

	EDreamTextureReflowMode Mode = EDreamTextureReflowMode::FillChannels;
	FString SourceTextureChoice;
	FString DecomposeChoice;
	FString RestyleSourceChoice;
	EDreamTextureUVKind UVKind = EDreamTextureUVKind::Tiled;
	FString Prompt;
	double DenoiseChoice = 0.0;
	bool bDenoiseTouched = false;
	int32 ResolutionChoice = 0;
	bool bInPlace = false;

	/** 当前材质的通道现状与勾选。 */
	FString CurrentAssetPath;
	TArray<FDreamTextureReflowChannelState> States;
	TSet<FString> Selection;
	bool bSelectionTouched = false;

	TSharedPtr<FDreamTextureReflowRun> CurrentRun;
	FDelegateHandle RunHandle;
	bool bRunNotified = false;

	FDelegateHandle ModelHandle;
	TWeakPtr<FDreamTextureConnection> BoundConnection;
	FDelegateHandle LinkHandle;
	bool bBoundExternal = false;
};
