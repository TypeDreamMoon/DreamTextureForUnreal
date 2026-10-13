// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DreamTextureLibraryModel.h"
#include "Input/Reply.h"
#include "Styling/SlateColor.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Views/STileView.h"

class FDreamTextureThumbCache;
class SBox;
class SSearchBox;
class STableViewBase;
class ITableRow;
struct FSlateBrush;

/**
 * 素材库页：浏览后端的素材库、选中看详情、一键导入。
 *
 *   ┌ 工具条 ──────────────────────────────────────────────┐
 *   │ [搜索…………]  [全部｜写实｜手绘]  [☆ 收藏]  [刷新]            │
 *   ├──────────────────────────────────┬───────────────────┤
 *   │ 网格（STileView）                  │ 详情（选中后出现）   │
 *   │  ┌────┐ ┌────┐ ┌────┐             │  大图 / 名字 / 星标  │
 *   │  │缩略图│ │    │ │    │             │  导入 / 进度        │
 *   │  └────┘ └────┘ └────┘             │  许可 / 云端提示     │
 *   │  名字     名字    名字              │  提示词 种子 分辨率… │
 *   │  [导入]   [导入]  [导入]            │                    │
 *   │ 已加载 60 套            [加载更多]  │                    │
 *   └──────────────────────────────────┴───────────────────┘
 *
 * 网格区在四种状态间切换：读取中 / 读取失败（带原因和「重连」）/ 没有素材 / 网格。
 * 后端连不上时绝不显示一个空网格——那看起来像「素材库是空的」，会把人引到错的地方去查。
 *
 * 所有状态在 FDreamTextureLibraryModel 里，控件只负责画。
 */
class SDreamTextureLibraryPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamTextureLibraryPanel)
		: _bBindExternal(true)
		, _bAutoRefresh(true)
	{}
		/** 注入模型。默认新建一个连真后端的。自动化测试注入带假后端的。 */
		SLATE_ARGUMENT(TSharedPtr<FDreamTextureLibraryModel>, Model)

		/** false 时模型不订阅长连接与导入队列（测试用）。 */
		SLATE_ARGUMENT(bool, bBindExternal)

		/** false 时构造时不发起首次刷新（测试用，自己喂数据）。 */
		SLATE_ARGUMENT(bool, bAutoRefresh)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SDreamTextureLibraryPanel() override;

	/** 网格区此刻处于哪种状态。公开给自动化测试断言分支。 */
	enum class EViewState : uint8
	{
		/** 第一次读取还没回来。 */
		Loading,
		/** 读取失败且没有任何旧数据可显示。 */
		Error,
		/** 读取成功但一条都没有。 */
		Empty,
		/** 有东西可画。 */
		Grid,
	};

	EViewState GetViewState() const;
	TSharedPtr<FDreamTextureLibraryModel> GetModel() const { return Model; }

	/**
	 * 测试钩子：把当前每张卡片、详情区、各状态区的取值函数都过一遍。
	 * -nullrhi 的自动化里没人驱动 Slate 的 Tick，STileView 不会自己生成卡片，
	 * 不调这个的话卡片和详情的构造代码就永远没被执行过。
	 */
	void BuildWidgetsForTest();

private:
	/** 风格筛选。对应后端 style 参数的 "" / realistic / stylized。 */
	enum class EStyleFilter : uint8
	{
		All,
		Realistic,
		Stylized,
	};

	//~ 构件
	TSharedRef<SWidget> MakeToolbar();
	TSharedRef<SWidget> MakeGridArea();
	TSharedRef<SWidget> MakeLoadingState();
	TSharedRef<SWidget> MakeErrorState();
	TSharedRef<SWidget> MakeEmptyState();
	TSharedRef<SWidget> MakeTileContent(const FDreamTextureLibraryItemPtr& Item);
	/** 一张缩略图：加载中/无预览的占位 + 画刷。卡片和详情共用，所以按 id 取而不是按条目。 */
	TSharedRef<SWidget> MakeThumbnail(const FString& Id, float Size);

	/**
	 * 导入按钮 + 进度。卡片和详情共用：按钮、阶段文字、进度条、结果行都读模型里的导入状态，
	 * 每帧取一次，所以进度变化不需要重建任何控件。
	 */
	TSharedRef<SWidget> MakeImportControls(const FString& Id);

	/**
	 * 收藏星标。实心 = 已收藏，空心 = 未收藏：形状不同，颜色（实心偏黄）只是冗余。
	 * 点击走 FDreamTextureLibraryModel::ToggleFavorite（乐观更新，失败回滚并提示）。
	 */
	TSharedRef<SWidget> MakeFavoriteButton(const FDreamTextureLibraryItemPtr& Item);

	//~ 详情区
	TSharedRef<SWidget> MakeDetailArea();
	void RebuildDetail();
	TSharedRef<SWidget> BuildDetailContent();
	TSharedRef<SWidget> MakeLicenseBox(const FDreamTextureManifest& Manifest);
	TSharedRef<SWidget> MakeCloudSourceBox();
	/** 没选中任何东西时详情栏里的提示。栏一直在，所以空的时候也要说话。 */
	TSharedRef<SWidget> MakeDetailPlaceholder();
	/** 导入过之后显示材质实例：名字、新建/更新/未变（或没建的原因）、「在内容浏览器中显示」。 */
	TSharedRef<SWidget> MakeMaterialInstanceSection(const FString& Id);
	TSharedRef<SWidget> MakeFieldRow(const FText& Label, const FText& Value);
	static FText FormatCreatedAt(const FDateTime& UtcTime);

	TSharedRef<ITableRow> OnGenerateTile(FDreamTextureLibraryItemPtr Item, const TSharedRef<STableViewBase>& Owner);
	void OnSelectionChanged(FDreamTextureLibraryItemPtr Item, ESelectInfo::Type SelectInfo);

	//~ 模型事件
	void HandleListChanged();
	void HandleDetailChanged();
	void HandleNotice(const FText& Message, bool bIsError);

	//~ 工具条
	void OnSearchChanged(const FText& NewText);
	void OnStyleChanged(EStyleFilter NewStyle);
	EStyleFilter GetStyleFilter() const;
	ECheckBoxState GetFavoriteOnlyState() const;
	void OnFavoriteOnlyChanged(ECheckBoxState NewState);
	FReply OnRefreshClicked();
	FReply OnReconnectClicked();
	FReply OnClearFiltersClicked();
	FReply OnLoadMoreClicked();

	//~ 状态区取值
	int32 GetViewStateIndex() const;
	FText GetErrorHeadline() const;
	FText GetConnectionErrorText() const;
	EVisibility GetConnectionErrorVisibility() const;
	EVisibility GetStaleErrorBannerVisibility() const;
	FText GetStaleErrorBannerText() const;
	FText GetFooterCountText() const;
	EVisibility GetLoadMoreVisibility() const;
	bool IsLoadMoreEnabled() const;
	FText GetLoadMoreText() const;
	bool IsRefreshEnabled() const;

	//~ 文本
	static FText StyleLabel(const FString& Style);

	TSharedPtr<FDreamTextureLibraryModel> Model;
	TSharedPtr<FDreamTextureThumbCache> ThumbCache;

	TSharedPtr<STileView<FDreamTextureLibraryItemPtr>> TileView;
	TSharedPtr<SSearchBox> SearchBox;

	/** 详情内容的宿主。manifest 是异步来的，状态一变整块换掉比增删子控件干净。 */
	TSharedPtr<SBox> DetailHost;

	FDelegateHandle ListChangedHandle;
	FDelegateHandle DetailChangedHandle;
	FDelegateHandle NoticeHandle;

	/** 卡片尺寸。STileView 要的是含边距的整格大小。 */
	static constexpr float TileWidth = 176.0f;
	static constexpr float TileHeight = 252.0f;
	static constexpr float ThumbSize = 160.0f;
	static constexpr float DetailWidth = 320.0f;
	static constexpr float DetailThumbSize = 280.0f;
};
