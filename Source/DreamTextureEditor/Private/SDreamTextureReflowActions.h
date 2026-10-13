// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DreamTextureReflowActions.h"
#include "Input/Reply.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"

class FDreamTextureThumbCache;
class SMultiLineEditableTextBox;
template <typename OptionType> class SComboBox;

/**
 * 回流页的动作区：补齐缺失通道 / 改写外观。放在 SDreamTextureReflowPanel::GetActionsHost() 里。
 *
 *   ┌ 做什么 ────────────────────────────────────────────────────────────┐
 *   │ (●) 补齐缺失通道（画面不变）    ( ) 改写外观（按提示词重画，再整套分解）  │
 *   │ 贴图类型 (●) 平铺   ( ) UV 展开（角色、道具、trim sheet）  ← 按材质记住    │
 *   │ 底色贴图 [T_Rock_BC ▾]（认出多张底色时才出现）                          │
 *   ├ 要覆盖的通道（已有的默认不动）─────────────────────────────────────────┤
 *   │ ☐ 底色    已有：T_Rock_BC（参数 BaseColor）  ← 补齐时锁定，沿用原图       │
 *   │ ☑ 法线    缺失                                                       │
 *   │ ☐ 粗糙度  已有：在 T_Rock_ORM 里（与 AO、金属度同一张）                  │
 *   │ …                                                                   │
 *   ├ 分解模型 [CHORD 分解 · 仅限研究用途 ▾]   ⚠ 仅限研究用途……                │
 *   │   （UV 展开默认选传统派生；选 CHORD 标「分布外，质量不保证」）            │
 *   ├ （改写外观）出图模型 / 提示词 / 重绘幅度 + 档位说明                      │
 *   │   （UV 展开重绘幅度 ≤ 0.35，超了拦住并说明为什么）                       │
 *   ├ 落回：(●) 新建（新贴图 + 新 MI，原材质不动）  ( ) 原地覆盖（可撤销）      │
 *   ├ [开始]  [取消]     提示 / 开始不了的原因                                │
 *   └ 进度：导出 → 上传 → 提交 → 生成 → 下载 → 导入    ▓▓▓░░   当前一步的说明   │
 *        结果（新实例名 / 改了哪些参数）  [在内容浏览器中显示]                    │
 *        预览：平铺 = 3×3 平铺图；UV 展开 = 「原图 | 结果」并排                   │
 *
 * 所有状态在 FDreamTextureReflowActions 里，控件只负责画与转发操作。
 */
class SDreamTextureReflowActions : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamTextureReflowActions)
		: _bThumbnails(true)
		, _bBindExternal(true)
	{}
		/** 注入模型。默认新建一个连真实后端的（需要同时给 ReflowModel）。 */
		SLATE_ARGUMENT(TSharedPtr<FDreamTextureReflowActions>, Actions)

		/** false 时不建结果预览的缩略图缓存（无渲染环境的自动化测试里没法画）。 */
		SLATE_ARGUMENT(bool, bThumbnails)

		/** false 时模型不订阅长连接（测试用）。 */
		SLATE_ARGUMENT(bool, bBindExternal)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SDreamTextureReflowActions() override;

	TSharedPtr<FDreamTextureReflowActions> GetActions() const { return Actions; }

	/**
	 * 测试钩子：把各区的取值函数都过一遍。-nullrhi 的自动化里没人驱动 Slate 的 Tick，
	 * 属性回调永远不会被调用，不调这个的话控件的取值代码就永远没被执行过。
	 */
	void BuildWidgetsForTest();

private:
	using FOptionPtr = TSharedPtr<FString>;

	TSharedRef<SWidget> MakeModeBlock();
	TSharedRef<SWidget> MakeChannelBlock();
	TSharedRef<SWidget> MakeUVKindBlock();
	TSharedRef<SWidget> MakeDecomposeBlock();
	TSharedRef<SWidget> MakeRestyleBlock();
	TSharedRef<SWidget> MakeLandingBlock();
	TSharedRef<SWidget> MakeRunBlock();
	TSharedRef<SWidget> MakeResultBlock();

	void RefreshOptions();
	void HandleActionsChanged();

	//~ 取值
	FText GetDecomposeLabel() const;
	FText GetDecomposeWarning() const;
	EVisibility GetDecomposeWarningVisibility() const;
	FText GetRestyleSourceLabel() const;
	EVisibility GetRestyleVisibility() const;
	EVisibility GetSourceRowVisibility() const;
	FText GetSourceLabel() const;
	EVisibility GetResolutionVisibility() const;
	FText GetResolutionLabel() const;
	FText GetDenoiseTierText() const;
	FText GetDenoiseWarning() const;
	EVisibility GetDenoiseWarningVisibility() const;
	EVisibility GetUVKindVisibility() const;
	FText GetInPlaceHint() const;
	EVisibility GetInPlaceHintVisibility() const;
	bool IsRunEnabled() const;
	FText GetRunReason() const;
	EVisibility GetRunReasonVisibility() const;
	FText GetRunNote() const;
	EVisibility GetRunNoteVisibility() const;
	EVisibility GetRunStatusVisibility() const;
	FText GetStepsText() const;
	FText GetRunMessage() const;
	FSlateColor GetRunMessageColor() const;
	TOptional<float> GetRunProgress() const;
	EVisibility GetProgressVisibility() const;
	EVisibility GetCancelVisibility() const;
	EVisibility GetResultVisibility() const;
	EVisibility GetShowButtonVisibility() const;
	const FSlateBrush* GetLeftPreviewBrush() const;
	const FSlateBrush* GetRightPreviewBrush() const;
	FText GetLeftPreviewLabel() const;
	FText GetRightPreviewLabel() const;
	EVisibility GetPreviewVisibility() const;
	EVisibility GetRightPreviewVisibility() const;

	FReply OnRunClicked();
	FReply OnCancelClicked();
	FReply OnShowClicked();

	TSharedPtr<FDreamTextureReflowActions> Actions;
	TSharedPtr<FDreamTextureThumbCache> PreviewCache;

	/** 通道行的缓存：模型一变就重新取一遍，每帧不重算。 */
	TArray<FDreamTextureReflowChannelView> Views;

	TArray<FOptionPtr> DecomposeOptions;
	TArray<FOptionPtr> RestyleSourceOptions;
	TArray<FOptionPtr> SourceOptions;
	TArray<FOptionPtr> ResolutionOptions;

	TSharedPtr<SComboBox<FOptionPtr>> DecomposeCombo;
	TSharedPtr<SComboBox<FOptionPtr>> RestyleSourceCombo;
	TSharedPtr<SComboBox<FOptionPtr>> SourceCombo;
	TSharedPtr<SComboBox<FOptionPtr>> ResolutionCombo;
	TSharedPtr<SMultiLineEditableTextBox> PromptBox;
	TSharedPtr<SVerticalBox> ChannelRows;

	FDelegateHandle ChangedHandle;
	bool bThumbnails = true;
};
