// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DreamTextureGenerateModel.h"
#include "Input/Reply.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SComboBox.h"

class FDreamTextureThumbCache;
class SMultiLineEditableTextBox;
class SWrapBox;

/**
 * 生成页：不切窗口，从一句提示词到生成出 PBR 材质再导进工程。
 *
 *   ┌ 左栏（定宽）─────────────────┬ 右栏 ────────────────────────────────────┐
 *   │ 出图模型  [本地 SDXL · 写实 ▼] │ 本批 4 个变体 · 2 个已结束   [同步] [全部取消] │
 *   │ 分解模型  [CHORD 分解      ▼] │ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐             │
 *   │ ⚠ 搭不上的提醒（只提醒不拦）     │ │缩略图│ │缩略图│ │占位 │ │占位 │             │
 *   │ 提示词                       │ │     │ │     │ │     │ │     │             │
 *   │ ┌──────────────────────────┐ │ └─────┘ └─────┘ └─────┘ └─────┘             │
 *   │ │ 多行输入                  │ │ 变体 1   变体 2   变体 3   变体 4             │
 *   │ └──────────────────────────┘ │ 种子·1024² …                                │
 *   │ [扩写] [模型 ▼]               │ ✓ 完成    ✓ 完成    生成中    排队中…          │
 *   │ （扩写结果 [用它] [不用]）       │ [导入]    已导入    ▓▓▓░░    （无进度条）      │
 *   │ 分辨率 [1024▼]  变体数 [4]     │                    [取消]    [取消]          │
 *   │ 种子 (随机｜固定) [______]      │                                              │
 *   │ ☐ 完成后自动导入               │                                              │
 *   │ ⚠ 研究用途许可（不可商用）       │                                              │
 *   │ [        生成        ]        │                                              │
 *   │ 提交不了的原因（第几级）         │                                              │
 *   └──────────────────────────────┴──────────────────────────────────────────┘
 *
 * 左栏在四种状态间切换：读取中 / 读取失败（带原因与「重试」）/ 表单。后端连不上时不显示一个空表单——
 * 两个下拉是空的，会让人去查工作流有没有装，而真正的问题是更上一层的后端没应答。
 *
 * 所有状态在 FDreamTextureGenerateModel 里，控件只负责画；任务的实时状态来自追踪器，每帧现取。
 */
class SDreamTextureGeneratePanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamTextureGeneratePanel)
		: _bBindExternal(true)
		, _bAutoLoad(true)
	{}
		/** 注入模型。默认新建一个连真后端的。自动化测试注入带假后端的。 */
		SLATE_ARGUMENT(TSharedPtr<FDreamTextureGenerateModel>, Model)

		/** false 时模型不订阅长连接（测试用）。 */
		SLATE_ARGUMENT(bool, bBindExternal)

		/** false 时构造时不去拉目录与扩写模型列表（测试用，自己喂数据）。 */
		SLATE_ARGUMENT(bool, bAutoLoad)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SDreamTextureGeneratePanel() override;

	TSharedPtr<FDreamTextureGenerateModel> GetModel() const { return Model; }

	/**
	 * 左栏此刻处于哪种状态。公开给自动化测试断言分支。
	 */
	enum class EFormState : uint8
	{
		Loading,
		Failed,
		Form,
	};
	EFormState GetFormState() const;

	/**
	 * 测试钩子：把每张卡片、各状态区的取值函数都过一遍。
	 * -nullrhi 的自动化里没人驱动 Slate 的 Tick，属性回调永远不会被调用，
	 * 不调这个的话卡片和表单的取值代码就永远没被执行过。
	 */
	void BuildWidgetsForTest();

private:
	/** 下拉里的一项。 */
	struct FOption
	{
		FString Id;
		FText Label;

		/** 名字后面的小标签（写实/手绘/云端底图…）。 */
		FText Tag;

		/** 标签要不要用警告色（研究用途）。 */
		bool bWarn = false;
	};
	using FOptionPtr = TSharedPtr<FOption>;

	//~ 构件
	TSharedRef<SWidget> MakeLeftColumn();
	TSharedRef<SWidget> MakeForm();
	TSharedRef<SWidget> MakeLoadingState();
	TSharedRef<SWidget> MakeFailedState();
	TSharedRef<SWidget> MakeRefineArea();
	TSharedRef<SWidget> MakeCloudBlock();
	TSharedRef<SWidget> MakeSeedRow();
	TSharedRef<SWidget> MakeLicenseBox();
	TSharedRef<SWidget> MakeSubmitArea();
	TSharedRef<SWidget> MakeRightColumn();
	TSharedRef<SWidget> MakeRowCard(const FDreamTextureGenerateModel::FRowPtr& Row);
	TSharedRef<SWidget> MakeCardThumbnail(const FDreamTextureGenerateModel::FRowPtr& Row, float Size);
	TSharedRef<SWidget> MakeLabel(const FText& Text);
	TSharedRef<SWidget> MakeOptionWidget(FOptionPtr Item);

	//~ 下拉
	void RebuildOptions();
	FOptionPtr FindOption(const TArray<FOptionPtr>& List, const FString& Id) const;
	void OnSourcePicked(FOptionPtr Item, ESelectInfo::Type SelectInfo);
	void OnDecomposePicked(FOptionPtr Item, ESelectInfo::Type SelectInfo);
	void OnResolutionPicked(FOptionPtr Item, ESelectInfo::Type SelectInfo);
	void OnRefineModelPicked(FOptionPtr Item, ESelectInfo::Type SelectInfo);
	void OnCloudModelPicked(FOptionPtr Item, ESelectInfo::Type SelectInfo);
	void OnCloudQualityPicked(FOptionPtr Item, ESelectInfo::Type SelectInfo);
	FText GetCloudModelLabel() const;
	FText GetCloudQualityLabel() const;
	FText GetSourceLabel() const;
	FText GetDecomposeLabel() const;
	FText GetResolutionLabel() const;
	FText GetRefineModelLabel() const;

	//~ 模型事件
	void HandleFormChanged();
	void HandleBatchChanged();
	void HandleRefineChanged();
	void HandlePromptReplaced();
	void HandleNotice(const FText& Message, bool bIsError);
	void RebuildCards();

	//~ 状态区取值
	int32 GetFormStateIndex() const;
	FText GetCatalogErrorText() const;
	FText GetSourceNote() const;
	EVisibility GetSourceNoteVisibility() const;
	EVisibility GetCloudVisibility() const;
	EVisibility GetCloudQualityVisibility() const;
	FText GetCloudStatusText() const;
	EVisibility GetCloudStatusVisibility() const;
	FText GetCloudCostText() const;
	EVisibility GetCloudRetryVisibility() const;
	FText GetMismatchText() const;
	EVisibility GetMismatchVisibility() const;
	EVisibility GetResolutionVisibility() const;
	FText GetLicenseHeadline() const;
	FText GetLicenseReasonText() const;
	EVisibility GetLicenseVisibility() const;
	bool IsSubmitEnabled() const;
	FText GetSubmitErrorText() const;
	EVisibility GetSubmitErrorVisibility() const;
	EVisibility GetSettingsLinkVisibility() const;
	EVisibility GetCancelAutoStartVisibility() const;
	FText GetSubmitText() const;
	FText GetSubmitHintText() const;
	FSlateColor GetSubmitHintColor() const;
	EVisibility GetSubmitHintVisibility() const;
	FText GetBatchSummaryText() const;
	EVisibility GetEmptyHintVisibility() const;
	EVisibility GetCardsVisibility() const;
	bool IsCancelAllEnabled() const;

	//~ 动作
	FReply OnSubmitClicked();
	FReply OnCancelAutoStartClicked();
	FReply OnOpenSettingsClicked();
	FReply OnRefineClicked();
	FReply OnAcceptRefineClicked();
	FReply OnDiscardRefineClicked();
	FReply OnRetryCatalogClicked();
	FReply OnRetryCloudClicked();
	FReply OnSyncClicked();
	FReply OnCancelAllClicked();
	void OnPromptTextChanged(const FText& NewText);

	TSharedPtr<FDreamTextureGenerateModel> Model;
	TSharedPtr<FDreamTextureThumbCache> ThumbCache;

	TArray<FOptionPtr> SourceOptions;
	TArray<FOptionPtr> DecomposeOptions;
	TArray<FOptionPtr> ResolutionOptions;
	TArray<FOptionPtr> RefineModelOptions;
	TArray<FOptionPtr> CloudModelOptions;
	TArray<FOptionPtr> CloudQualityOptions;
	TSharedPtr<SComboBox<FOptionPtr>> SourceCombo;
	TSharedPtr<SComboBox<FOptionPtr>> DecomposeCombo;
	TSharedPtr<SComboBox<FOptionPtr>> ResolutionCombo;
	TSharedPtr<SComboBox<FOptionPtr>> RefineModelCombo;
	TSharedPtr<SComboBox<FOptionPtr>> CloudModelCombo;
	TSharedPtr<SComboBox<FOptionPtr>> CloudQualityCombo;

	TSharedPtr<SMultiLineEditableTextBox> PromptBox;

	/** 变体卡片的容器。批次一变整块重建，卡片内容靠属性回调每帧读追踪器，进度变化不需要重建。 */
	TSharedPtr<SWrapBox> CardBox;

	FDelegateHandle FormChangedHandle;
	FDelegateHandle BatchChangedHandle;
	FDelegateHandle RefineChangedHandle;
	FDelegateHandle PromptReplacedHandle;
	FDelegateHandle NoticeHandle;

	static constexpr float LeftColumnWidth = 344.0f;
	static constexpr float CardWidth = 196.0f;
	static constexpr float CardThumbSize = 176.0f;
};
