// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DreamTextureApi.h"
#include "DreamTextureImportQueue.h"
#include "DreamTextureManifest.h"
#include "DreamTextureTypes.h"

class FDreamTextureConnection;

/** 网格里的一张卡片。用共享指针传递，刷新时按 id 复用同一个对象，STileView 的选中态才不会丢。 */
struct FDreamTextureLibraryItem
{
	FDreamTextureMaterialSummary Summary;

	/** 收藏请求在路上：期间不接受再次点击，避免两个请求乱序后界面和后端对不上。 */
	bool bFavoritePending = false;
};

using FDreamTextureLibraryItemPtr = TSharedPtr<FDreamTextureLibraryItem>;

/** 详情区里 manifest 的加载状态。列表行不含许可信息，要选中后再拉一次。 */
enum class EDreamTextureDetailState : uint8
{
	/** 没选中任何东西。 */
	None,
	Loading,
	Ready,
	Failed,
};

/**
 * 模型依赖的外部能力。默认全部转发到真实实现；自动化测试替换成假的，
 * 这样不用起后端、也不会真的去排导入队列。
 */
struct FDreamTextureLibraryBackend
{
	TFunction<void(const FDreamTextureMaterialQuery&, FDreamTextureApi::FOnList)> List;
	TFunction<void(const FString&, FDreamTextureApi::FOnManifest)> GetManifest;
	TFunction<void(const FString&, bool, FDreamTextureApi::FOnDone)> SetFavorite;
	TFunction<bool(const FString&)> Enqueue;

	static FDreamTextureLibraryBackend MakeDefault();
};

/**
 * 素材库页的状态：查询条件、分页列表、选中项与它的 manifest、收藏、导入进度。
 *
 * 为什么单拎成一个不含 Slate 的类：界面要的东西都是「异步结果 + 过期丢弃」，
 * 这些逻辑（尤其是收藏的乐观更新与回滚、翻页时被刷新打断）一旦混进控件里就没法测。
 * 拎出来之后自动化测试可以注入假后端，逐个分支验证。
 *
 * ## 回调与生命周期
 *
 * 所有异步回调都只捕获弱指针。模型归面板所有，面板关掉之后迟到的 HTTP 回调
 * Pin 不到就直接丢弃。
 *
 * ## 过期结果
 *
 * 每次发起列表请求都把 ListGeneration 加一并带进回调，回来对不上就丢——
 * 用户一边打字一边点筛选的时候，慢的旧请求绝不能盖掉新的结果。
 *
 * 所有委托都在游戏线程上广播。
 */
class FDreamTextureLibraryModel : public TSharedFromThis<FDreamTextureLibraryModel>
{
public:
	/** 翻页大小。后端对 ≤0 或 >500 的 limit 一律按 60 处理，所以刷新时的上限卡在 500。 */
	static constexpr int32 PageSize = 60;
	static constexpr int32 MaxLimit = 500;

	explicit FDreamTextureLibraryModel(FDreamTextureLibraryBackend InBackend = FDreamTextureLibraryBackend::MakeDefault());
	~FDreamTextureLibraryModel();

	/**
	 * 订阅外部事件（长连接、导入队列）。不放构造函数：AddSP 要求对象已被共享指针持有。
	 * bBindExternal 为 false 时一个都不订，自动化测试直接调 Handle* 喂数据。
	 */
	void Initialize(bool bBindExternal = true);

	DECLARE_MULTICAST_DELEGATE(FOnChanged);
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnNotice, const FText& /*Message*/, bool /*bIsError*/);

	/** 列表、加载状态、错误任一变化。 */
	FOnChanged OnListChanged;
	/** 选中项或它的 manifest 变化。 */
	FOnChanged OnDetailChanged;
	/** 要提示用户的一句话（收藏失败、加载更多失败…）。模型自己不碰 Slate，由面板弹通知。 */
	FOnNotice OnNotice;

	//~ 查询条件
	void SetSearchText(const FString& InText);
	void SetStyleFilter(const FString& InStyle);
	void SetFavoriteOnly(bool bInFavoriteOnly);
	const FString& GetSearchText() const { return Query.Text; }
	const FString& GetStyleFilter() const { return Query.Style; }
	bool IsFavoriteOnly() const { return Query.bFavoriteOnly; }
	/** 有没有任何筛选在起作用——空结果时据此区分「还没有素材」和「没有符合条件的」。 */
	bool HasActiveFilter() const;

	//~ 列表
	/** 从头重新拉。已加载的条数会保留（最多 MaxLimit），所以自动刷新不会把翻过的页收回去。 */
	void Refresh();
	void LoadMore();

	const TArray<FDreamTextureLibraryItemPtr>& GetItems() const { return Items; }
	FDreamTextureLibraryItemPtr FindItem(const FString& Id) const;
	bool IsLoading() const { return bLoading; }
	bool IsLoadingMore() const { return bLoadingMore; }
	bool HasMore() const { return bHasMore; }
	/** 至少成功拿到过一次列表。没有的话「空」可能只是还没回来。 */
	bool HasLoadedOnce() const { return bLoadedOnce; }
	/** 最近一次列表请求的失败原因；成功后清空。 */
	const FString& GetListError() const { return ListError; }

	//~ 选中与详情
	void Select(const FString& Id);
	const FString& GetSelectedId() const { return SelectedId; }
	EDreamTextureDetailState GetDetailState() const { return DetailState; }
	const FDreamTextureManifest& GetDetailManifest() const { return DetailManifest; }
	const FString& GetDetailError() const { return DetailError; }
	void RetryDetail();

	//~ 收藏
	/** 乐观更新：先翻星标，请求失败再翻回来并发 OnNotice。 */
	void ToggleFavorite(const FString& Id);

	//~ 导入
	/** 排一个导入。已经在队列里时发 OnNotice 说一声，不重复排。 */
	void RequestImport(const FString& Id);
	/** 导入队列报来的状态变化。 */
	void HandleImportStatus(const FDreamTextureImportStatus& Status);
	/** 没排过返回 nullptr。 */
	const FDreamTextureImportStatus* FindImportStatus(const FString& Id) const;

	//~ 自动刷新的两个触发点（公开是为了测试直接调用）
	/** 链路变为 Live 时刷新一次——后端刚起来、或断线重连后，列表可能已经过期。 */
	void HandleLinkChanged(EDreamTextureLink NewLink);
	/** job.done：Web 端刚生成完的材质自己出现。其它事件类型忽略。 */
	void HandleEvent(const FDreamTextureEvent& Event);

private:
	FDreamTextureMaterialQuery BuildQuery(int32 Offset, int32 Limit) const;
	void OnFiltersChanged();
	void ApplyListResult(uint64 Generation, int32 RequestedLimit, bool bOK, TArray<FDreamTextureMaterialSummary>&& Materials, const FString& Error);
	void ApplyMoreResult(uint64 Generation, bool bOK, TArray<FDreamTextureMaterialSummary>&& Materials, const FString& Error);
	void StartDetailFetch();
	void RememberManifest(const FString& Id, const FDreamTextureManifest& Manifest);

	FDreamTextureLibraryBackend Backend;
	FDreamTextureMaterialQuery Query;

	TArray<FDreamTextureLibraryItemPtr> Items;
	bool bLoading = false;
	bool bLoadingMore = false;
	bool bHasMore = false;
	bool bLoadedOnce = false;
	FString ListError;
	uint64 ListGeneration = 0;

	FString SelectedId;
	EDreamTextureDetailState DetailState = EDreamTextureDetailState::None;
	FDreamTextureManifest DetailManifest;
	FString DetailError;
	uint64 DetailGeneration = 0;

	/** 看过的 manifest。内容在后端是不可变的，来回点同一个不必重拉；条数封顶免得无限涨。 */
	TMap<FString, FDreamTextureManifest> ManifestCache;
	TArray<FString> ManifestCacheOrder;
	static constexpr int32 MaxCachedManifests = 24;

	TMap<FString, FDreamTextureImportStatus> ImportStates;

	TWeakPtr<FDreamTextureConnection> BoundConnection;
	FDelegateHandle LinkHandle;
	FDelegateHandle EventHandle;
	FDelegateHandle ImportHandle;
	bool bBoundExternal = false;
};
