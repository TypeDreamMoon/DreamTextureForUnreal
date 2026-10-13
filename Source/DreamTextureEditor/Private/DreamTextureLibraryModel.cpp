// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureLibraryModel.h"

#include "DreamTextureClientModule.h"
#include "DreamTextureConnection.h"

#define LOCTEXT_NAMESPACE "DreamTextureLibraryModel"

FDreamTextureLibraryBackend FDreamTextureLibraryBackend::MakeDefault()
{
	FDreamTextureLibraryBackend Out;
	Out.List = [](const FDreamTextureMaterialQuery& Q, FDreamTextureApi::FOnList Done)
	{
		FDreamTextureApi::ListMaterials(Q, MoveTemp(Done));
	};
	Out.GetManifest = [](const FString& Id, FDreamTextureApi::FOnManifest Done)
	{
		FDreamTextureApi::GetManifest(Id, MoveTemp(Done));
	};
	Out.SetFavorite = [](const FString& Id, bool bFavorite, FDreamTextureApi::FOnDone Done)
	{
		FDreamTextureApi::SetFavorite(Id, bFavorite, MoveTemp(Done));
	};
	Out.Enqueue = [](const FString& Id)
	{
		return FDreamTextureImportQueue::Get().Enqueue(Id);
	};
	return Out;
}

FDreamTextureLibraryModel::FDreamTextureLibraryModel(FDreamTextureLibraryBackend InBackend)
	: Backend(MoveTemp(InBackend))
{
	Query.Limit = PageSize;
}

FDreamTextureLibraryModel::~FDreamTextureLibraryModel()
{
	if (bBoundExternal)
	{
		// AddSP 绑的是弱引用，漏摘不会悬空，但死条目要等到下一次广播才被清掉。
		// 面板是 nomad tab，开开关关频繁，不攒着。
		if (TSharedPtr<FDreamTextureConnection> Conn = BoundConnection.Pin())
		{
			Conn->OnLinkChanged.Remove(LinkHandle);
			Conn->OnEvent.Remove(EventHandle);
		}
		FDreamTextureImportQueue::Get().OnStatusChanged.Remove(ImportHandle);
	}
}

void FDreamTextureLibraryModel::Initialize(bool bBindExternal)
{
	if (!bBindExternal || bBoundExternal)
	{
		return;
	}
	bBoundExternal = true;

	// 导入队列是进程内单例，永远在。
	ImportHandle = FDreamTextureImportQueue::Get().OnStatusChanged.AddSP(this, &FDreamTextureLibraryModel::HandleImportStatus);

	// 客户端模块可能没加载，或者连接是空的（头文件明说了要判空）。
	// 这时只是没有自动刷新，手动「刷新」照常能用。
	if (FDreamTextureClientModule::IsAvailable())
	{
		if (TSharedPtr<FDreamTextureConnection> Conn = FDreamTextureClientModule::Get().GetConnection())
		{
			BoundConnection = Conn;
			LinkHandle = Conn->OnLinkChanged.AddSP(this, &FDreamTextureLibraryModel::HandleLinkChanged);
			EventHandle = Conn->OnEvent.AddSP(this, &FDreamTextureLibraryModel::HandleEvent);
		}
	}
}

//////////////////////////////////////////////////////////////////////////
// 查询条件

bool FDreamTextureLibraryModel::HasActiveFilter() const
{
	return !Query.Text.IsEmpty() || !Query.Style.IsEmpty() || Query.bFavoriteOnly;
}

void FDreamTextureLibraryModel::SetSearchText(const FString& InText)
{
	const FString Trimmed = InText.TrimStartAndEnd();
	if (Query.Text == Trimmed)
	{
		return;
	}
	Query.Text = Trimmed;
	OnFiltersChanged();
}

void FDreamTextureLibraryModel::SetStyleFilter(const FString& InStyle)
{
	if (Query.Style == InStyle)
	{
		return;
	}
	Query.Style = InStyle;
	OnFiltersChanged();
}

void FDreamTextureLibraryModel::SetFavoriteOnly(bool bInFavoriteOnly)
{
	if (Query.bFavoriteOnly == bInFavoriteOnly)
	{
		return;
	}
	Query.bFavoriteOnly = bInFavoriteOnly;
	OnFiltersChanged();
}

void FDreamTextureLibraryModel::OnFiltersChanged()
{
	// 条件变了，旧列表和「还有更多」都作废，只拉第一页。
	// 不保留已加载条数——那是自动刷新的待遇，换条件应该从头看。
	// 条目先清空：留着旧结果等新结果回来，用户会把搜索前的内容当成搜索结果。
	Items.Reset();
	bHasMore = false;
	bLoadedOnce = false;
	ListError.Reset();
	Refresh();
}

FDreamTextureMaterialQuery FDreamTextureLibraryModel::BuildQuery(int32 Offset, int32 Limit) const
{
	FDreamTextureMaterialQuery Out = Query;
	Out.Offset = Offset;
	Out.Limit = Limit;
	return Out;
}

//////////////////////////////////////////////////////////////////////////
// 列表

FDreamTextureLibraryItemPtr FDreamTextureLibraryModel::FindItem(const FString& Id) const
{
	for (const FDreamTextureLibraryItemPtr& Item : Items)
	{
		if (Item.IsValid() && Item->Summary.Id == Id)
		{
			return Item;
		}
	}
	return nullptr;
}

void FDreamTextureLibraryModel::Refresh()
{
	// 保留已经翻出来的条数：job.done 触发的自动刷新不该把用户翻过的页收回去。
	const int32 Limit = FMath::Clamp(Items.Num(), PageSize, MaxLimit);

	const uint64 Gen = ++ListGeneration;
	bLoading = true;
	// 刷新会作废在飞的「加载更多」，它回来也会因为代数对不上被丢。
	bLoadingMore = false;
	OnListChanged.Broadcast();

	TWeakPtr<FDreamTextureLibraryModel> WeakSelf = AsShared();
	Backend.List(BuildQuery(0, Limit),
		[WeakSelf, Gen, Limit](bool bOK, TArray<FDreamTextureMaterialSummary>&& Materials, const FString& Error)
		{
			if (TSharedPtr<FDreamTextureLibraryModel> Self = WeakSelf.Pin())
			{
				Self->ApplyListResult(Gen, Limit, bOK, MoveTemp(Materials), Error);
			}
		});
}

void FDreamTextureLibraryModel::ApplyListResult(uint64 Generation, int32 RequestedLimit, bool bOK, TArray<FDreamTextureMaterialSummary>&& Materials, const FString& Error)
{
	if (Generation != ListGeneration)
	{
		return;
	}

	bLoading = false;

	if (!bOK)
	{
		// 失败时保留已有条目：自动刷新失败不该把好端端的网格清空。
		// 错误原样记下，界面据此决定是整页空状态还是网格上方一条提示。
		ListError = Error.IsEmpty() ? LOCTEXT("ListReadFailed", "The library could not be read.").ToString() : Error;
		OnListChanged.Broadcast();
		return;
	}

	ListError.Reset();
	bLoadedOnce = true;
	bHasMore = Materials.Num() >= RequestedLimit;

	// 按 id 复用旧对象，只更新内容：STileView 靠指针认选中项，
	// 重新 new 一批的话每次自动刷新都会把用户的选中态冲掉。
	TMap<FString, FDreamTextureLibraryItemPtr> Old;
	for (const FDreamTextureLibraryItemPtr& Item : Items)
	{
		Old.Add(Item->Summary.Id, Item);
	}

	TArray<FDreamTextureLibraryItemPtr> NewItems;
	NewItems.Reserve(Materials.Num());
	TSet<FString> Seen;
	for (FDreamTextureMaterialSummary& Row : Materials)
	{
		if (Row.Id.IsEmpty() || Seen.Contains(Row.Id))
		{
			continue;
		}
		Seen.Add(Row.Id);

		FDreamTextureLibraryItemPtr Item = Old.FindRef(Row.Id);
		if (Item.IsValid())
		{
			// 收藏请求在路上的话，界面上的星标是乐观值，别被这次回来的旧值盖回去。
			const bool bKeepFav = Item->bFavoritePending;
			const bool bFav = Item->Summary.bFavorite;
			Item->Summary = MoveTemp(Row);
			if (bKeepFav)
			{
				Item->Summary.bFavorite = bFav;
			}
		}
		else
		{
			Item = MakeShared<FDreamTextureLibraryItem>();
			Item->Summary = MoveTemp(Row);
		}
		NewItems.Add(Item);
	}
	Items = MoveTemp(NewItems);

	// 选中的那张已经不在了（被删、被筛掉）：详情区该收起，别留着一个悬空的 id。
	if (!SelectedId.IsEmpty() && !FindItem(SelectedId).IsValid())
	{
		Select(FString());
	}

	OnListChanged.Broadcast();
}

void FDreamTextureLibraryModel::LoadMore()
{
	if (bLoading || bLoadingMore || !bHasMore)
	{
		return;
	}

	const uint64 Gen = ListGeneration;
	bLoadingMore = true;
	OnListChanged.Broadcast();

	TWeakPtr<FDreamTextureLibraryModel> WeakSelf = AsShared();
	Backend.List(BuildQuery(Items.Num(), PageSize),
		[WeakSelf, Gen](bool bOK, TArray<FDreamTextureMaterialSummary>&& Materials, const FString& Error)
		{
			if (TSharedPtr<FDreamTextureLibraryModel> Self = WeakSelf.Pin())
			{
				Self->ApplyMoreResult(Gen, bOK, MoveTemp(Materials), Error);
			}
		});
}

void FDreamTextureLibraryModel::ApplyMoreResult(uint64 Generation, bool bOK, TArray<FDreamTextureMaterialSummary>&& Materials, const FString& Error)
{
	// 期间被刷新或换了筛选条件：这一页属于旧的列表，丢掉。bLoadingMore 已经被 Refresh 复位。
	if (Generation != ListGeneration)
	{
		return;
	}

	bLoadingMore = false;

	if (!bOK)
	{
		// 加载更多失败不动 ListError：那个字段会让整页变成错误态，而前面的条目是好的。
		OnNotice.Broadcast(FText::Format(LOCTEXT("LoadMoreFailed", "Could not load more material sets: {0}"),
			Error.IsEmpty() ? LOCTEXT("UnknownReason", "Unknown reason") : FText::FromString(Error)), true);
		OnListChanged.Broadcast();
		return;
	}

	bHasMore = Materials.Num() >= PageSize;

	// 翻页期间如果有新素材生成，后一页的开头会和前一页的末尾重叠，按 id 去重。
	TSet<FString> Existing;
	for (const FDreamTextureLibraryItemPtr& Item : Items)
	{
		Existing.Add(Item->Summary.Id);
	}
	for (FDreamTextureMaterialSummary& Row : Materials)
	{
		if (Row.Id.IsEmpty() || Existing.Contains(Row.Id))
		{
			continue;
		}
		Existing.Add(Row.Id);
		FDreamTextureLibraryItemPtr Item = MakeShared<FDreamTextureLibraryItem>();
		Item->Summary = MoveTemp(Row);
		Items.Add(Item);
	}

	OnListChanged.Broadcast();
}

//////////////////////////////////////////////////////////////////////////
// 选中与详情

void FDreamTextureLibraryModel::Select(const FString& Id)
{
	if (Id == SelectedId)
	{
		return;
	}

	SelectedId = Id;
	DetailManifest = FDreamTextureManifest();
	DetailError.Reset();
	++DetailGeneration;

	if (Id.IsEmpty())
	{
		DetailState = EDreamTextureDetailState::None;
		OnDetailChanged.Broadcast();
		return;
	}

	if (const FDreamTextureManifest* Cached = ManifestCache.Find(Id))
	{
		DetailManifest = *Cached;
		DetailState = EDreamTextureDetailState::Ready;
		OnDetailChanged.Broadcast();
		return;
	}

	StartDetailFetch();
}

void FDreamTextureLibraryModel::RetryDetail()
{
	if (SelectedId.IsEmpty() || DetailState == EDreamTextureDetailState::Loading)
	{
		return;
	}
	++DetailGeneration;
	StartDetailFetch();
}

void FDreamTextureLibraryModel::StartDetailFetch()
{
	DetailState = EDreamTextureDetailState::Loading;
	DetailError.Reset();
	OnDetailChanged.Broadcast();

	const uint64 Gen = DetailGeneration;
	const FString Id = SelectedId;

	TWeakPtr<FDreamTextureLibraryModel> WeakSelf = AsShared();
	Backend.GetManifest(Id,
		[WeakSelf, Gen, Id](bool bOK, FDreamTextureManifest&& Manifest, const FString& Error)
		{
			TSharedPtr<FDreamTextureLibraryModel> Self = WeakSelf.Pin();
			// 用户已经点了别的卡片：这份 manifest 属于上一张，丢掉。
			if (!Self.IsValid() || Gen != Self->DetailGeneration)
			{
				return;
			}

			if (bOK)
			{
				Self->RememberManifest(Id, Manifest);
				Self->DetailManifest = MoveTemp(Manifest);
				Self->DetailState = EDreamTextureDetailState::Ready;
			}
			else
			{
				Self->DetailState = EDreamTextureDetailState::Failed;
				Self->DetailError = Error.IsEmpty() ? LOCTEXT("ManifestReadFailed", "The material manifest could not be read.").ToString() : Error;
			}
			Self->OnDetailChanged.Broadcast();
		});
}

void FDreamTextureLibraryModel::RememberManifest(const FString& Id, const FDreamTextureManifest& Manifest)
{
	if (Id.IsEmpty())
	{
		return;
	}

	// 键用请求时的 id，不用 manifest 里自带的：查的时候用的是选中项的 id，两边必须同一个口径。
	ManifestCacheOrder.Remove(Id);
	ManifestCacheOrder.Add(Id);
	ManifestCache.Add(Id, Manifest);

	// 封顶：每份 manifest 带着整份 JSON，无限攒下去等于内存泄漏。
	while (ManifestCacheOrder.Num() > MaxCachedManifests)
	{
		ManifestCache.Remove(ManifestCacheOrder[0]);
		ManifestCacheOrder.RemoveAt(0);
	}
}

//////////////////////////////////////////////////////////////////////////
// 收藏

void FDreamTextureLibraryModel::ToggleFavorite(const FString& Id)
{
	FDreamTextureLibraryItemPtr Item = FindItem(Id);
	if (!Item.IsValid() || Item->bFavoritePending)
	{
		return;
	}

	const bool bOld = Item->Summary.bFavorite;
	const bool bNew = !bOld;
	Item->Summary.bFavorite = bNew;
	Item->bFavoritePending = true;
	OnListChanged.Broadcast();

	TWeakPtr<FDreamTextureLibraryModel> WeakSelf = AsShared();
	Backend.SetFavorite(Id, bNew,
		[WeakSelf, Id, bOld](bool bOK, const FString& Error)
		{
			TSharedPtr<FDreamTextureLibraryModel> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}

			// 期间列表可能被刷新过：按 id 重新找，不用捕获的指针。找不到就是这张已经不在了。
			if (FDreamTextureLibraryItemPtr Found = Self->FindItem(Id))
			{
				Found->bFavoritePending = false;
				if (!bOK)
				{
					// 回滚星标。不回滚的话界面说「已收藏」而后端没有，下次刷新才会露馅。
					Found->Summary.bFavorite = bOld;
				}
			}

			if (!bOK)
			{
				Self->OnNotice.Broadcast(FText::Format(LOCTEXT("FavoriteFailed", "The favorite change did not go through and has been reverted: {0}"),
					Error.IsEmpty() ? LOCTEXT("UnknownReason", "Unknown reason") : FText::FromString(Error)), true);
			}
			Self->OnListChanged.Broadcast();
		});
}

//////////////////////////////////////////////////////////////////////////
// 导入

void FDreamTextureLibraryModel::RequestImport(const FString& Id)
{
	if (Id.IsEmpty())
	{
		return;
	}

	if (!Backend.Enqueue(Id))
	{
		// 契约：同一个 id 已在队列里或正在跑时返回 false。不是错误，说一声就行。
		OnNotice.Broadcast(LOCTEXT("AlreadyQueued", "This material set is already in the import queue."), false);
	}
}

void FDreamTextureLibraryModel::HandleImportStatus(const FDreamTextureImportStatus& Status)
{
	if (Status.MaterialId.IsEmpty())
	{
		return;
	}
	ImportStates.Add(Status.MaterialId, Status);
	// 卡片的阶段文字和进度条读的是 FindImportStatus，Slate 每帧重画就会取到，
	// 不需要重建网格，所以这里不广播 OnListChanged。
}

const FDreamTextureImportStatus* FDreamTextureLibraryModel::FindImportStatus(const FString& Id) const
{
	if (const FDreamTextureImportStatus* Found = ImportStates.Find(Id))
	{
		return Found;
	}

	// 面板关掉再打开：模型是新的，但队列还记着上一次的状态，补上它。
	// 只在真的订阅了外部事件时才问队列，自动化测试用的假后端不该被真队列污染。
	if (bBoundExternal)
	{
		return FDreamTextureImportQueue::Get().FindStatus(Id);
	}
	return nullptr;
}

//////////////////////////////////////////////////////////////////////////
// 自动刷新

void FDreamTextureLibraryModel::HandleLinkChanged(EDreamTextureLink NewLink)
{
	if (NewLink == EDreamTextureLink::Live)
	{
		Refresh();
	}
}

void FDreamTextureLibraryModel::HandleEvent(const FDreamTextureEvent& Event)
{
	if (Event.Type == TEXT("job.done"))
	{
		Refresh();
	}
}

#undef LOCTEXT_NAMESPACE
