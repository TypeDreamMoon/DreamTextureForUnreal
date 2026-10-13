// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DreamTextureReflowModel.h"
#include "Input/Reply.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

class FAssetThumbnailPool;
class FDreamTextureReflowActions;
class SBox;
class SHeaderRow;
template <typename OptionType> class SComboBox;

/**
 * 回流页：选一个工程里已有的材质，看它用了哪些贴图、每张是什么通道，把能导的导成 PNG。
 *
 *   ┌ 工具条 ──────────────────────────────────────────────────────┐
 *   │ [取当前选择]   内容浏览器里选材质/实例，或关卡里选 Actor           │
 *   ├ 候选（选区里有多个时才出现）─────────────────────────────────────┤
 *   │ ▸ MI_Rock          ▸ SM_Wall / 槽 0（Brick）· MI_Brick            │
 *   ├ 材质标题 ────────────────────────────────────────────────────┤
 *   │ MI_Rock（材质实例，母材质 M_Master）                              │
 *   ├ 结果表 ──────────────────────────────────────────────────────┤
 *   │ 缩略图 │ 贴图（参数/尺寸/用法）│ 通道（下拉；打包图再有 R G B A）│ 依据 │ 导出 │
 *   ├ 动作区 ──────────────────────────────────────────────────────┤
 *   │ [长边 ▾] [导出 PNG]  [打开文件夹]   已导出 3 张到 …                │
 *   │ ┌ GetActionsHost()：回流动作区（SDreamTextureReflowActions）┐       │
 *   │ │ 补齐通道 / 改写外观、要覆盖的通道、分解模型、落回方式、开始与进度 │       │
 *   │ └ 里面自己带滚动，最高 ~380px，不把上面的结果表挤没 ┘                │
 *   └──────────────────────────────────────────────────────────────┘
 *
 * 所有状态在 FDreamTextureReflowModel 里，控件只负责画与转发操作。
 */
class SDreamTextureReflowPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamTextureReflowPanel)
		: _bThumbnails(true)
		, _bAutoRefresh(true)
		, _bActions(true)
	{}
		/** 注入模型。默认新建一个连真实选区的。自动化测试注入带假后端的。 */
		SLATE_ARGUMENT(TSharedPtr<FDreamTextureReflowModel>, Model)

		/** false 时不建资产缩略图（无渲染环境的自动化测试里没法画）。 */
		SLATE_ARGUMENT(bool, bThumbnails)

		/** false 时构造时不去取选区（测试用，自己喂数据）。 */
		SLATE_ARGUMENT(bool, bAutoRefresh)

		/** 注入动作区的模型。默认在 Model 上新建一个连真实后端的。自动化测试注入带假后端的。 */
		SLATE_ARGUMENT(TSharedPtr<FDreamTextureReflowActions>, Actions)

		/** false 时不建动作区（它构造时会去拉工作流目录，要网络；只想测结果表的测试关掉它）。 */
		SLATE_ARGUMENT(bool, bActions)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SDreamTextureReflowPanel() override;

	TSharedPtr<FDreamTextureReflowModel> GetModel() const { return Model; }

	/**
	 * 动作区的容器：回流的「补齐通道 / 改写外观」挂在这里（M8）。
	 * bActions 为假时里面是空的。
	 */
	TSharedRef<SBox> GetActionsHost() const;

	/** 动作区的模型；没建动作区时为空。 */
	TSharedPtr<FDreamTextureReflowActions> GetActions() const { return Actions; }

	/**
	 * 测试钩子：把结果表每一行、每一列的控件都生成一遍，并过一遍各取值函数。
	 * 无渲染环境的自动化里没人驱动 Slate 的 Tick，列表控件不会自己生成行，
	 * 不调这个的话各列的构造代码就永远没被执行过。
	 */
	void BuildWidgetsForTest();

private:
	TSharedRef<SWidget> MakeToolbar();
	TSharedRef<SWidget> MakeCandidateList();
	TSharedRef<SWidget> MakeHeader();
	TSharedRef<SWidget> MakeTable();
	TSharedRef<SWidget> MakeFooter();

	TSharedRef<ITableRow> OnGenerateRow(FDreamTextureReflowRowItemPtr Item, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<ITableRow> OnGenerateCandidate(TSharedPtr<int32> Item, const TSharedRef<STableViewBase>& Owner);
	void OnCandidateSelected(TSharedPtr<int32> Item, ESelectInfo::Type SelectInfo);

	void HandleChanged();
	void HandleNotice(const FText& Message, bool bIsError);
	void SyncCandidateSelection();

	FReply OnRefreshClicked();
	FReply OnExportClicked();
	FReply OnOpenFolderClicked();

	int32 GetViewIndex() const;
	FText GetStatusText() const;
	FText GetHeaderText() const;
	FText GetHeaderDetailText() const;
	FText GetWarningsText() const;
	EVisibility GetWarningsVisibility() const;
	EVisibility GetCandidateListVisibility() const;
	EVisibility GetHeaderVisibility() const;
	bool IsExportEnabled() const;
	FText GetExportButtonText() const;
	FText GetExportSummaryText() const;
	EVisibility GetOpenFolderVisibility() const;
	FText GetMaxEdgeText() const;

	TSharedPtr<FDreamTextureReflowModel> Model;
	TSharedPtr<FDreamTextureReflowActions> Actions;
	TSharedPtr<class SDreamTextureReflowActions> ActionsWidget;
	TSharedPtr<FAssetThumbnailPool> ThumbnailPool;

	TSharedPtr<SListView<FDreamTextureReflowRowItemPtr>> RowList;
	TSharedPtr<SHeaderRow> HeaderRow;
	TSharedPtr<SListView<TSharedPtr<int32>>> CandidateList;
	TArray<TSharedPtr<int32>> CandidateItems;
	TSharedPtr<SBox> ActionsHost;
	TSharedPtr<SComboBox<TSharedPtr<int32>>> MaxEdgeCombo;
	TArray<TSharedPtr<int32>> MaxEdgeChoices;

	FDelegateHandle ChangedHandle;
	FDelegateHandle NoticeHandle;
	bool bSyncingSelection = false;
	bool bThumbnails = true;
};
