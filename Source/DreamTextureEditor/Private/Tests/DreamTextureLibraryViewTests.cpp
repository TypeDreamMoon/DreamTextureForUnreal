// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureConnection.h"
#include "DreamTextureImportNotifier.h"
#include "DreamTextureLibraryModel.h"
#include "DreamTextureThumbCache.h"
#include "Framework/Application/SlateApplication.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SDreamTextureConnectionStrip.h"
#include "SDreamTextureLibraryPanel.h"

/*
 * 素材库界面：模型的异步分支、面板在各状态下构造不崩、连接状态分档、通知文案、缩略图解码。
 *
 * 全部不连后端：模型的取数函数换成假的，把回调攥在手里，由测试决定什么时候、以什么顺序回。
 * 这样「慢的旧请求晚于新请求回来」「面板关了回调才到」这类真实世界里偶发的时序，
 * 在这里可以稳定复现。
 */

namespace
{
	FDreamTextureMaterialSummary LibMakeRow(const TCHAR* Id, const TCHAR* Name, bool bFavorite = false)
	{
		FDreamTextureMaterialSummary Row;
		Row.Id = Id;
		Row.Name = Name;
		Row.Style = TEXT("realistic");
		Row.Resolution = 1024;
		Row.bFavorite = bFavorite;
		return Row;
	}

	TArray<FDreamTextureMaterialSummary> MakeRows(int32 Count, int32 StartIndex = 0)
	{
		TArray<FDreamTextureMaterialSummary> Out;
		for (int32 i = 0; i < Count; ++i)
		{
			FDreamTextureMaterialSummary Row;
			Row.Id = FString::Printf(TEXT("mat_%04d"), StartIndex + i);
			Row.Name = FString::Printf(TEXT("材质 %d"), StartIndex + i);
			Row.Style = TEXT("stylized");
			Row.Resolution = 1024;
			Out.Add(MoveTemp(Row));
		}
		return Out;
	}

	/** 假后端：回调攥在手里，测试想什么时候回就什么时候回。 */
	struct FFakeLibraryBackend
	{
		struct FList
		{
			FDreamTextureMaterialQuery Query;
			FDreamTextureApi::FOnList Done;
		};
		struct FManifest
		{
			FString Id;
			FDreamTextureApi::FOnManifest Done;
		};
		struct FFavorite
		{
			FString Id;
			bool bFavorite = false;
			FDreamTextureApi::FOnDone Done;
		};

		TArray<FList> Lists;
		TArray<FManifest> Manifests;
		TArray<FFavorite> Favorites;
		TArray<FString> Enqueued;
		bool bEnqueueResult = true;

		FDreamTextureLibraryBackend Make()
		{
			FDreamTextureLibraryBackend Out;
			Out.List = [this](const FDreamTextureMaterialQuery& Q, FDreamTextureApi::FOnList Done)
			{
				Lists.Add({ Q, MoveTemp(Done) });
			};
			Out.GetManifest = [this](const FString& Id, FDreamTextureApi::FOnManifest Done)
			{
				Manifests.Add({ Id, MoveTemp(Done) });
			};
			Out.SetFavorite = [this](const FString& Id, bool bFav, FDreamTextureApi::FOnDone Done)
			{
				Favorites.Add({ Id, bFav, MoveTemp(Done) });
			};
			Out.Enqueue = [this](const FString& Id)
			{
				Enqueued.Add(Id);
				return bEnqueueResult;
			};
			return Out;
		}

		void CompleteList(int32 Index, TArray<FDreamTextureMaterialSummary> Rows)
		{
			Lists[Index].Done(true, MoveTemp(Rows), FString());
		}
		void FailList(int32 Index, const FString& Error)
		{
			Lists[Index].Done(false, TArray<FDreamTextureMaterialSummary>(), Error);
		}
		void CompleteManifest(int32 Index, FDreamTextureManifest Manifest)
		{
			Manifests[Index].Done(true, MoveTemp(Manifest), FString());
		}
	};

	TSharedRef<FDreamTextureLibraryModel> MakeModel(FFakeLibraryBackend& Fake)
	{
		TSharedRef<FDreamTextureLibraryModel> Model = MakeShared<FDreamTextureLibraryModel>(Fake.Make());
		Model->Initialize(/*bBindExternal*/ false);
		return Model;
	}

	/** 连接的假身：基类把状态字段留给子类可写，getter 都是非虚的，直接读成员。 */
	class FFakeLibraryConnection : public FDreamTextureConnection
	{
	public:
		void Set(EDreamTextureLink InLink, bool bReachable, const FString& InError = FString())
		{
			Link = InLink;
			bBackendReachable = bReachable;
			LastError = InError;
		}
		void SetHealth(const FDreamTextureHealth& InHealth) { Health = InHealth; }
	};
}

// ---------------------------------------------------------------------------
// 模型：列表、过期结果、翻页
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureLibraryModelListTest, "DreamTexture.Editor.LibraryView.Model.List",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureLibraryModelListTest::RunTest(const FString& Parameters)
{
	FFakeLibraryBackend Fake;
	TSharedRef<FDreamTextureLibraryModel> Model = MakeModel(Fake);

	TestFalse(TEXT("还没请求时不算加载过"), Model->HasLoadedOnce());

	Model->Refresh();
	TestEqual(TEXT("发出了一次列表请求"), Fake.Lists.Num(), 1);
	TestTrue(TEXT("请求在途中"), Model->IsLoading());
	TestEqual(TEXT("首页 Offset"), Fake.Lists[0].Query.Offset, 0);
	TestEqual(TEXT("首页 Limit"), Fake.Lists[0].Query.Limit, FDreamTextureLibraryModel::PageSize);

	Fake.CompleteList(0, MakeRows(3));
	TestFalse(TEXT("回来后不再加载"), Model->IsLoading());
	TestTrue(TEXT("标记为读取过"), Model->HasLoadedOnce());
	TestEqual(TEXT("三条"), Model->GetItems().Num(), 3);
	TestFalse(TEXT("不足一页，没有更多"), Model->HasMore());
	TestTrue(TEXT("没有错误"), Model->GetListError().IsEmpty());

	// 失败：保留旧条目，只记错误。
	Model->Refresh();
	Fake.FailList(1, TEXT("连不上后端"));
	TestEqual(TEXT("失败时旧条目保留"), Model->GetItems().Num(), 3);
	TestEqual(TEXT("错误原样记下"), Model->GetListError(), FString(TEXT("连不上后端")));

	// 后端的 null 数组在解析层是空数组；空列表是合法结果。
	Model->Refresh();
	Fake.CompleteList(2, {});
	TestEqual(TEXT("空列表清空条目"), Model->GetItems().Num(), 0);
	TestTrue(TEXT("成功后错误清空"), Model->GetListError().IsEmpty());
	TestTrue(TEXT("空但读取过"), Model->HasLoadedOnce());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureLibraryModelStaleTest, "DreamTexture.Editor.LibraryView.Model.StaleResults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureLibraryModelStaleTest::RunTest(const FString& Parameters)
{
	FFakeLibraryBackend Fake;
	TSharedRef<FDreamTextureLibraryModel> Model = MakeModel(Fake);

	// 连着改两次搜索词：两个请求同时在途。
	Model->SetSearchText(TEXT("a"));
	Model->SetSearchText(TEXT("ab"));
	TestEqual(TEXT("两个请求"), Fake.Lists.Num(), 2);
	TestEqual(TEXT("请求带着搜索词"), Fake.Lists[1].Query.Text, FString(TEXT("ab")));

	// 新的先回、旧的后回：旧的绝不能盖掉新的。
	Fake.CompleteList(1, MakeRows(2, 100));
	Fake.CompleteList(0, MakeRows(5, 0));
	TestEqual(TEXT("留下的是新请求的结果"), Model->GetItems().Num(), 2);
	TestEqual(TEXT("第一条是新结果"), Model->GetItems()[0]->Summary.Id, FString(TEXT("mat_0100")));

	// 同样的搜索词不重复请求。
	Model->SetSearchText(TEXT("  ab "));
	TestEqual(TEXT("去掉空白后没变，不发请求"), Fake.Lists.Num(), 2);

	// 风格与收藏筛选进入查询参数。
	Model->SetStyleFilter(TEXT("stylized"));
	Model->SetFavoriteOnly(true);
	TestEqual(TEXT("风格参数"), Fake.Lists.Last().Query.Style, FString(TEXT("stylized")));
	TestTrue(TEXT("收藏参数"), Fake.Lists.Last().Query.bFavoriteOnly);
	TestTrue(TEXT("有筛选在起作用"), Model->HasActiveFilter());
	TestEqual(TEXT("换条件后旧条目立刻清掉"), Model->GetItems().Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureLibraryModelPagingTest, "DreamTexture.Editor.LibraryView.Model.Paging",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureLibraryModelPagingTest::RunTest(const FString& Parameters)
{
	FFakeLibraryBackend Fake;
	TSharedRef<FDreamTextureLibraryModel> Model = MakeModel(Fake);

	Model->Refresh();
	Fake.CompleteList(0, MakeRows(FDreamTextureLibraryModel::PageSize));
	TestTrue(TEXT("满一页，可能还有更多"), Model->HasMore());

	Model->LoadMore();
	TestEqual(TEXT("加载更多的 Offset"), Fake.Lists[1].Query.Offset, FDreamTextureLibraryModel::PageSize);
	TestTrue(TEXT("加载更多在途"), Model->IsLoadingMore());

	// 在途时再点不重复发。
	Model->LoadMore();
	TestEqual(TEXT("不重复发请求"), Fake.Lists.Num(), 2);

	// 翻页期间有新素材生成：下一页开头与上一页末尾重叠 2 条，按 id 去重。
	TArray<FDreamTextureMaterialSummary> Next = MakeRows(10, FDreamTextureLibraryModel::PageSize - 2);
	Fake.CompleteList(1, MoveTemp(Next));
	TestEqual(TEXT("去重后的总数"), Model->GetItems().Num(), FDreamTextureLibraryModel::PageSize + 8);
	TestFalse(TEXT("不足一页，到底了"), Model->HasMore());

	// 自动刷新保留翻过的条数，不把用户翻过的页收回去。
	Model->Refresh();
	TestEqual(TEXT("刷新请求的 Limit 覆盖已加载条数"), Fake.Lists.Last().Query.Limit, FDreamTextureLibraryModel::PageSize + 8);

	// 加载更多失败：不动列表错误（否则整页变错误态），只发提示。
	Fake.CompleteList(2, MakeRows(FDreamTextureLibraryModel::PageSize + 8));
	int32 NoticeCount = 0;
	bool bNoticeWasError = false;
	Model->OnNotice.AddLambda([&](const FText&, bool bIsError)
	{
		++NoticeCount;
		bNoticeWasError = bIsError;
	});
	Model->LoadMore();
	Fake.FailList(Fake.Lists.Num() - 1, TEXT("超时"));
	TestEqual(TEXT("发了一条提示"), NoticeCount, 1);
	TestTrue(TEXT("是错误提示"), bNoticeWasError);
	TestTrue(TEXT("列表错误没被污染"), Model->GetListError().IsEmpty());
	TestEqual(TEXT("条目还在"), Model->GetItems().Num(), FDreamTextureLibraryModel::PageSize + 8);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureLibraryModelRefreshKeepsTest, "DreamTexture.Editor.LibraryView.Model.RefreshKeepsSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureLibraryModelRefreshKeepsTest::RunTest(const FString& Parameters)
{
	FFakeLibraryBackend Fake;
	TSharedRef<FDreamTextureLibraryModel> Model = MakeModel(Fake);

	Model->Refresh();
	Fake.CompleteList(0, MakeRows(3));

	const FDreamTextureLibraryItemPtr Before = Model->FindItem(TEXT("mat_0001"));
	TestTrue(TEXT("找得到 mat_0001"), Before.IsValid());
	Model->Select(TEXT("mat_0001"));

	// 刷新回来同一个 id：条目对象复用，STileView 的选中态才不会丢。
	Model->Refresh();
	Fake.CompleteList(1, MakeRows(4));
	TestTrue(TEXT("同 id 复用同一个对象"), Model->FindItem(TEXT("mat_0001")) == Before);
	TestEqual(TEXT("选中还在"), Model->GetSelectedId(), FString(TEXT("mat_0001")));

	// 选中的那张从结果里消失：详情收起。
	Model->Refresh();
	TArray<FDreamTextureMaterialSummary> WithoutIt;
	WithoutIt.Add(LibMakeRow(TEXT("mat_0000"), TEXT("A")));
	Fake.CompleteList(2, MoveTemp(WithoutIt));
	TestTrue(TEXT("选中项消失后清掉选择"), Model->GetSelectedId().IsEmpty());
	TestTrue(TEXT("详情回到 None"), Model->GetDetailState() == EDreamTextureDetailState::None);

	return true;
}

// ---------------------------------------------------------------------------
// 模型：收藏、详情、导入、自动刷新、生命周期
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureLibraryModelFavoriteTest, "DreamTexture.Editor.LibraryView.Model.Favorite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureLibraryModelFavoriteTest::RunTest(const FString& Parameters)
{
	FFakeLibraryBackend Fake;
	TSharedRef<FDreamTextureLibraryModel> Model = MakeModel(Fake);

	Model->Refresh();
	TArray<FDreamTextureMaterialSummary> Rows;
	Rows.Add(LibMakeRow(TEXT("mat_a"), TEXT("A"), false));
	Rows.Add(LibMakeRow(TEXT("mat_b"), TEXT("B"), true));
	Fake.CompleteList(0, MoveTemp(Rows));

	int32 ErrorNotices = 0;
	Model->OnNotice.AddLambda([&](const FText&, bool bIsError) { ErrorNotices += bIsError ? 1 : 0; });

	// 乐观更新：先翻星标。
	Model->ToggleFavorite(TEXT("mat_a"));
	TestTrue(TEXT("乐观更新立刻生效"), Model->FindItem(TEXT("mat_a"))->Summary.bFavorite);
	TestEqual(TEXT("发出一个收藏请求"), Fake.Favorites.Num(), 1);
	TestTrue(TEXT("请求的是收藏"), Fake.Favorites[0].bFavorite);

	// 在途时再点不重复发。
	Model->ToggleFavorite(TEXT("mat_a"));
	TestEqual(TEXT("在途时忽略再次点击"), Fake.Favorites.Num(), 1);

	// 失败：回滚并提示。
	Fake.Favorites[0].Done(false, TEXT("服务器 500"));
	TestFalse(TEXT("失败后星标回滚"), Model->FindItem(TEXT("mat_a"))->Summary.bFavorite);
	TestFalse(TEXT("不再是在途"), Model->FindItem(TEXT("mat_a"))->bFavoritePending);
	TestEqual(TEXT("提示了一次"), ErrorNotices, 1);

	// 成功：保持。取消收藏同理。
	Model->ToggleFavorite(TEXT("mat_b"));
	TestFalse(TEXT("取消收藏的乐观更新"), Model->FindItem(TEXT("mat_b"))->Summary.bFavorite);
	Fake.Favorites[1].Done(true, FString());
	TestFalse(TEXT("成功后保持取消"), Model->FindItem(TEXT("mat_b"))->Summary.bFavorite);
	TestEqual(TEXT("成功不提示"), ErrorNotices, 1);

	// 请求在途时列表被刷新：乐观值不能被回来的旧值盖掉。
	Model->ToggleFavorite(TEXT("mat_a"));
	Model->Refresh();
	TArray<FDreamTextureMaterialSummary> Refreshed;
	Refreshed.Add(LibMakeRow(TEXT("mat_a"), TEXT("A"), false));
	Refreshed.Add(LibMakeRow(TEXT("mat_b"), TEXT("B"), false));
	Fake.CompleteList(1, MoveTemp(Refreshed));
	TestTrue(TEXT("在途的收藏不被刷新盖回"), Model->FindItem(TEXT("mat_a"))->Summary.bFavorite);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureLibraryModelDetailTest, "DreamTexture.Editor.LibraryView.Model.Detail",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureLibraryModelDetailTest::RunTest(const FString& Parameters)
{
	FFakeLibraryBackend Fake;
	TSharedRef<FDreamTextureLibraryModel> Model = MakeModel(Fake);

	Model->Refresh();
	Fake.CompleteList(0, MakeRows(3));

	Model->Select(TEXT("mat_0000"));
	TestTrue(TEXT("读取中"), Model->GetDetailState() == EDreamTextureDetailState::Loading);
	TestEqual(TEXT("拉了 manifest"), Fake.Manifests.Num(), 1);

	// 用户马上点了别的：第一张的 manifest 后到，必须丢掉。
	Model->Select(TEXT("mat_0001"));
	TestEqual(TEXT("第二次拉取"), Fake.Manifests.Num(), 2);

	FDreamTextureManifest First;
	First.Id = TEXT("mat_0000");
	First.License.bKnown = true;
	First.License.bCommercialUse = false;
	First.License.Reason = TEXT("CHORD: Ubisoft ML License (research-only)");
	Fake.CompleteManifest(0, MoveTemp(First));
	TestTrue(TEXT("过期的 manifest 被丢弃，仍是读取中"), Model->GetDetailState() == EDreamTextureDetailState::Loading);

	FDreamTextureManifest Second;
	Second.Id = TEXT("mat_0001");
	Second.bHasCloudSource = true;
	Second.License.bKnown = true;
	Second.License.bCommercialUse = false;
	Second.License.Reason = TEXT("CHORD: Ubisoft ML License (research-only)");
	Fake.CompleteManifest(1, MoveTemp(Second));
	TestTrue(TEXT("就绪"), Model->GetDetailState() == EDreamTextureDetailState::Ready);
	TestFalse(TEXT("不可商用"), Model->GetDetailManifest().License.bCommercialUse);
	TestTrue(TEXT("云端底图"), Model->GetDetailManifest().bHasCloudSource);

	// 看过的不再重拉。
	Model->Select(TEXT("mat_0002"));
	Model->Select(TEXT("mat_0001"));
	TestEqual(TEXT("缓存命中，不重拉 mat_0001"), Fake.Manifests.Num(), 3);
	TestTrue(TEXT("直接就绪"), Model->GetDetailState() == EDreamTextureDetailState::Ready);

	// 失败：状态 Failed，可重试。mat_0002 的第一次请求被切走时作废了，这是它的第二次。
	Model->Select(TEXT("mat_0002"));
	TestEqual(TEXT("mat_0002 重新拉取"), Fake.Manifests.Num(), 4);
	Fake.Manifests.Last().Done(false, FDreamTextureManifest(), TEXT("读取超时"));
	TestTrue(TEXT("失败"), Model->GetDetailState() == EDreamTextureDetailState::Failed);
	TestEqual(TEXT("失败原因保留"), Model->GetDetailError(), FString(TEXT("读取超时")));
	Model->RetryDetail();
	TestTrue(TEXT("重试回到读取中"), Model->GetDetailState() == EDreamTextureDetailState::Loading);
	TestEqual(TEXT("重试发了新请求"), Fake.Manifests.Num(), 5);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureLibraryModelAutoRefreshTest, "DreamTexture.Editor.LibraryView.Model.AutoRefresh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureLibraryModelAutoRefreshTest::RunTest(const FString& Parameters)
{
	FFakeLibraryBackend Fake;
	TSharedRef<FDreamTextureLibraryModel> Model = MakeModel(Fake);

	Model->HandleLinkChanged(EDreamTextureLink::Connecting);
	Model->HandleLinkChanged(EDreamTextureLink::Stale);
	Model->HandleLinkChanged(EDreamTextureLink::Offline);
	TestEqual(TEXT("非 Live 的链路变化不刷新"), Fake.Lists.Num(), 0);

	Model->HandleLinkChanged(EDreamTextureLink::Live);
	TestEqual(TEXT("变为 Live 刷新一次"), Fake.Lists.Num(), 1);

	FDreamTextureEvent Heartbeat;
	Heartbeat.Type = TEXT("hb");
	Model->HandleEvent(Heartbeat);
	FDreamTextureEvent Progress;
	Progress.Type = TEXT("job.progress");
	Model->HandleEvent(Progress);
	TestEqual(TEXT("心跳与进度事件不刷新"), Fake.Lists.Num(), 1);

	FDreamTextureEvent Done;
	Done.Type = TEXT("job.done");
	Model->HandleEvent(Done);
	TestEqual(TEXT("job.done 刷新一次"), Fake.Lists.Num(), 2);

	// 连发两个 job.done：旧请求作废，只认最后一个的结果。
	Model->HandleEvent(Done);
	Fake.CompleteList(2, MakeRows(1, 50));
	Fake.CompleteList(1, MakeRows(4, 0));
	TestEqual(TEXT("最后一次请求的结果生效"), Model->GetItems().Num(), 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureLibraryModelImportTest, "DreamTexture.Editor.LibraryView.Model.Import",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureLibraryModelImportTest::RunTest(const FString& Parameters)
{
	FFakeLibraryBackend Fake;
	TSharedRef<FDreamTextureLibraryModel> Model = MakeModel(Fake);

	int32 Notices = 0;
	Model->OnNotice.AddLambda([&](const FText&, bool) { ++Notices; });

	Model->RequestImport(TEXT("mat_x"));
	TestEqual(TEXT("排了一个导入"), Fake.Enqueued.Num(), 1);
	TestEqual(TEXT("排的是 mat_x"), Fake.Enqueued[0], FString(TEXT("mat_x")));
	TestEqual(TEXT("成功排队不提示"), Notices, 0);

	Fake.bEnqueueResult = false;
	Model->RequestImport(TEXT("mat_x"));
	TestEqual(TEXT("已在队列里时提示一声"), Notices, 1);

	TestNull(TEXT("没状态时查不到"), Model->FindImportStatus(TEXT("mat_x")));

	FDreamTextureImportStatus Status;
	Status.MaterialId = TEXT("mat_x");
	Status.Stage = EDreamTextureImportStage::Downloading;
	Status.FilesDone = 2;
	Status.FilesTotal = 4;
	Model->HandleImportStatus(Status);

	const FDreamTextureImportStatus* Found = Model->FindImportStatus(TEXT("mat_x"));
	if (TestNotNull(TEXT("收到状态后查得到"), Found))
	{
		TestEqual(TEXT("进度"), Found->FilesDone, 2);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureLibraryModelLifetimeTest, "DreamTexture.Editor.LibraryView.Model.Lifetime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureLibraryModelLifetimeTest::RunTest(const FString& Parameters)
{
	FFakeLibraryBackend Fake;
	{
		TSharedPtr<FDreamTextureLibraryModel> Model = MakeModel(Fake);
		Model->Refresh();
		TArray<FDreamTextureMaterialSummary> Rows;
		Rows.Add(LibMakeRow(TEXT("mat_a"), TEXT("A")));
		Fake.CompleteList(0, MoveTemp(Rows));

		// 三类请求都在途时模型被销毁。
		Model->Select(TEXT("mat_a"));
		Model->ToggleFavorite(TEXT("mat_a"));
		Model->Refresh();
		Model.Reset();
	}

	TestEqual(TEXT("三个回调都还攥在手里"), Fake.Lists.Num() + Fake.Manifests.Num() + Fake.Favorites.Num(), 4);

	// 模型已经没了，迟到的回调才到：不能崩。
	Fake.CompleteList(1, MakeRows(2));
	Fake.CompleteManifest(0, FDreamTextureManifest());
	Fake.Favorites[0].Done(true, FString());

	TestTrue(TEXT("迟到的回调没有崩溃"), true);
	return true;
}

// ---------------------------------------------------------------------------
// 面板：各状态下构造不崩，空状态/错误状态分支正确
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureLibraryViewTest, "DreamTexture.Editor.LibraryView.Panel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureLibraryViewTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddWarning(TEXT("Slate 没有初始化（命令行环境），跳过面板构造部分；模型部分见 .Model.* 子测试。"));
		return true;
	}

	FFakeLibraryBackend Fake;
	TSharedRef<FDreamTextureLibraryModel> Model = MakeModel(Fake);

	TSharedPtr<SDreamTextureLibraryPanel> Panel = SNew(SDreamTextureLibraryPanel)
		.Model(Model)
		.bBindExternal(false)
		.bAutoRefresh(false);

	using EView = SDreamTextureLibraryPanel::EViewState;

	// 1) 还没请求过：读取中，不是空状态。
	TestTrue(TEXT("初始是读取中"), Panel->GetViewState() == EView::Loading);

	// 2) 请求在途：仍是读取中。
	Model->Refresh();
	TestTrue(TEXT("请求在途是读取中"), Panel->GetViewState() == EView::Loading);

	// 3) 后端连不上：错误态，不是空网格。
	Fake.FailList(0, TEXT("后端没应答"));
	TestTrue(TEXT("失败且无旧数据是错误态"), Panel->GetViewState() == EView::Error);
	Panel->BuildWidgetsForTest();

	// 4) 成功但为空：空状态；有筛选时区分文案的分支也走一遍。
	Model->Refresh();
	Fake.CompleteList(1, {});
	TestTrue(TEXT("成功但为空是空状态"), Panel->GetViewState() == EView::Empty);
	Panel->BuildWidgetsForTest();
	Model->SetSearchText(TEXT("不存在的东西"));
	Fake.CompleteList(2, {});
	TestTrue(TEXT("筛选后为空也是空状态"), Panel->GetViewState() == EView::Empty);
	TestTrue(TEXT("有筛选"), Model->HasActiveFilter());
	Panel->BuildWidgetsForTest();

	// 5) 喂几条假数据：网格态，卡片构造不崩（含不可商用 / 云端底图的详情）。
	Model->SetSearchText(FString());
	TArray<FDreamTextureMaterialSummary> Rows = MakeRows(5);
	Rows[1].bFavorite = true;
	Rows[2].Name = FString();
	Rows[3].Tags = { TEXT("石头"), TEXT("苔藓") };
	Rows[4].CreatedAt = FDateTime(2026, 8, 27, 9, 26, 50);
	Fake.CompleteList(Fake.Lists.Num() - 1, MoveTemp(Rows));
	TestTrue(TEXT("有数据是网格态"), Panel->GetViewState() == EView::Grid);
	Panel->BuildWidgetsForTest();

	Model->Select(TEXT("mat_0001"));
	Panel->BuildWidgetsForTest(); // 读取中的详情

	FDreamTextureManifest Manifest;
	Manifest.Id = TEXT("mat_0001");
	Manifest.Negative = TEXT("blurry");
	Manifest.WorkflowVersion = 3;
	Manifest.bTileable = true;
	Manifest.bHasCloudSource = true;
	Manifest.License.bKnown = true;
	Manifest.License.bCommercialUse = false;
	Manifest.License.Reason = TEXT("CHORD: Ubisoft ML License (research-only)");
	Fake.CompleteManifest(0, Manifest);
	Panel->BuildWidgetsForTest(); // 不可商用 + 云端

	Manifest.License.bCommercialUse = true;
	Manifest.Id = TEXT("mat_0002");
	Model->Select(TEXT("mat_0002"));
	Fake.CompleteManifest(1, Manifest);
	Panel->BuildWidgetsForTest(); // 可商用

	Model->Select(TEXT("mat_0003"));
	Manifest.Id = TEXT("mat_0003");
	Manifest.License.bKnown = false;
	Fake.CompleteManifest(2, Manifest);
	Panel->BuildWidgetsForTest(); // 许可未声明

	Model->Select(TEXT("mat_0004"));
	Fake.Manifests[3].Done(false, FDreamTextureManifest(), TEXT("超时"));
	Panel->BuildWidgetsForTest(); // manifest 失败

	// 各个导入阶段的卡片取值。
	FDreamTextureImportStatus Status;
	Status.MaterialId = TEXT("mat_0001");
	for (EDreamTextureImportStage Stage : { EDreamTextureImportStage::Queued, EDreamTextureImportStage::FetchingManifest,
		EDreamTextureImportStage::Downloading, EDreamTextureImportStage::Importing, EDreamTextureImportStage::Failed })
	{
		Status.Stage = Stage;
		Status.FilesTotal = 4;
		Status.FilesDone = 1;
		Status.Message = TEXT("原因");
		Model->HandleImportStatus(Status);
		Panel->BuildWidgetsForTest();
	}
	Status.Stage = EDreamTextureImportStage::Succeeded;
	Status.Result.bOK = true;
	Status.Result.AssetFolder = TEXT("/Game/DreamTexture/X_1");
	Model->HandleImportStatus(Status);
	Panel->BuildWidgetsForTest();

	// 选中项（mat_0004）导入成功后，详情栏的材质实例这一段：新建 / 未变 / 没建（桩的说法）/ 失败。
	Status.MaterialId = TEXT("mat_0004");
	Model->Select(TEXT("mat_0004"));
	for (EDreamTextureImportAction Action : { EDreamTextureImportAction::Created, EDreamTextureImportAction::Unchanged,
		EDreamTextureImportAction::Skipped, EDreamTextureImportAction::Failed })
	{
		Status.Result.MaterialInstance = FDreamTextureImportedTexture();
		Status.Result.MaterialInstance.Action = Action;
		Status.Result.MaterialInstance.Note = TEXT("材质实例尚未实现");
		if (Action == EDreamTextureImportAction::Created || Action == EDreamTextureImportAction::Unchanged)
		{
			Status.Result.MaterialInstance.ObjectPath = TEXT("/Game/DreamTexture/X_1/MI_X_1.MI_X_1");
		}
		Model->HandleImportStatus(Status);
		Panel->BuildWidgetsForTest();
	}

	// 取消选中：详情栏回到占位提示，不崩。
	Model->Select(FString());
	Panel->BuildWidgetsForTest();

	// 6) 有旧数据时刷新失败：仍是网格态（只挂提示条），不是错误态。
	Model->Refresh();
	Fake.FailList(Fake.Lists.Num() - 1, TEXT("断了"));
	TestTrue(TEXT("有旧数据时失败仍是网格态"), Panel->GetViewState() == EView::Grid);
	Panel->BuildWidgetsForTest();

	// 7) 面板先关，回调后到：不能崩。
	Model->Refresh();
	Panel.Reset();
	Fake.CompleteList(Fake.Lists.Num() - 1, MakeRows(2));
	TestTrue(TEXT("面板销毁后回调到达没有崩溃"), true);

	return true;
}

// ---------------------------------------------------------------------------
// 顶部三级状态标记的分档
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureConnectionStripTest, "DreamTexture.Editor.LibraryView.Strip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureConnectionStripTest::RunTest(const FString& Parameters)
{
	using ETone = SDreamTextureConnectionStrip::ETone;

	// 没有连接对象：三个都是未知，不崩。
	TestTrue(TEXT("无连接：后端未知"), SDreamTextureConnectionStrip::EvaluateBackend(nullptr).Tone == ETone::Unknown);
	TestTrue(TEXT("无连接：长连接未知"), SDreamTextureConnectionStrip::EvaluateLink(nullptr).Tone == ETone::Unknown);
	TestTrue(TEXT("无连接：ComfyUI 未知"), SDreamTextureConnectionStrip::EvaluateComfy(nullptr).Tone == ETone::Unknown);

	TSharedRef<FFakeLibraryConnection> Conn = MakeShared<FFakeLibraryConnection>();
	TSharedPtr<FDreamTextureConnection> Base = Conn;

	// 还没开始连：是「连接中」，不是「未连接」。
	Conn->Set(EDreamTextureLink::Offline, false);
	TestTrue(TEXT("没开始连：后端连接中"), SDreamTextureConnectionStrip::EvaluateBackend(Base).Tone == ETone::Busy);
	TestTrue(TEXT("没开始连：长连接连接中"), SDreamTextureConnectionStrip::EvaluateLink(Base).Tone == ETone::Busy);

	// 连过又失败：后端和长连接都坏了，但 ComfyUI 是「未知」而不是「未运行」。
	Conn->Set(EDreamTextureLink::Offline, false, TEXT("连接被拒绝"));
	TestTrue(TEXT("失败：后端 Bad"), SDreamTextureConnectionStrip::EvaluateBackend(Base).Tone == ETone::Bad);
	TestTrue(TEXT("失败：长连接 Bad"), SDreamTextureConnectionStrip::EvaluateLink(Base).Tone == ETone::Bad);
	TestTrue(TEXT("后端没应答时 ComfyUI 未知，不能报未运行"), SDreamTextureConnectionStrip::EvaluateComfy(Base).Tone == ETone::Unknown);

	// 后端通、WS 没连上、ComfyUI 起不来：三级各自独立。
	FDreamTextureHealth Down;
	Conn->SetHealth(Down);
	Conn->Set(EDreamTextureLink::Connecting, true);
	TestTrue(TEXT("后端通"), SDreamTextureConnectionStrip::EvaluateBackend(Base).Tone == ETone::Ok);
	TestTrue(TEXT("长连接连接中"), SDreamTextureConnectionStrip::EvaluateLink(Base).Tone == ETone::Busy);
	TestTrue(TEXT("ComfyUI 未运行是 Bad"), SDreamTextureConnectionStrip::EvaluateComfy(Base).Tone == ETone::Bad);

	// 启动中排在最前面：是 Busy，不是 Bad。
	FDreamTextureHealth Starting;
	Starting.bStarting = true;
	Starting.StartingSecs = 42;
	Conn->SetHealth(Starting);
	Conn->Set(EDreamTextureLink::Live, true);
	TestTrue(TEXT("启动中是 Busy"), SDreamTextureConnectionStrip::EvaluateComfy(Base).Tone == ETone::Busy);

	FDreamTextureHealth Ready;
	Ready.bAlive = true;
	Ready.bReady = true;
	Conn->SetHealth(Ready);
	TestTrue(TEXT("长连接 Live 是 Ok"), SDreamTextureConnectionStrip::EvaluateLink(Base).Tone == ETone::Ok);
	TestTrue(TEXT("ComfyUI 就绪是 Ok"), SDreamTextureConnectionStrip::EvaluateComfy(Base).Tone == ETone::Ok);

	// 失联：长连接 Warn，ComfyUI 的读数是残值，退回未知。
	Conn->Set(EDreamTextureLink::Stale, true);
	TestTrue(TEXT("失联：长连接 Warn"), SDreamTextureConnectionStrip::EvaluateLink(Base).Tone == ETone::Warn);
	TestTrue(TEXT("失联：过期的就绪读数不显示成 Ok"), SDreamTextureConnectionStrip::EvaluateComfy(Base).Tone == ETone::Unknown);

	// 用户停的 / 活着没就绪：Warn，不报红。
	FDreamTextureHealth Stopped;
	Stopped.bUserStopped = true;
	Conn->SetHealth(Stopped);
	Conn->Set(EDreamTextureLink::Live, true);
	TestTrue(TEXT("手动停止是 Warn"), SDreamTextureConnectionStrip::EvaluateComfy(Base).Tone == ETone::Warn);

	// 形状：五档图标两两不同（Warning 与 Error 是同一张图的坑）。
	if (FSlateApplication::IsInitialized())
	{
		TSet<const FSlateBrush*> Brushes;
		for (ETone Tone : { ETone::Ok, ETone::Busy, ETone::Warn, ETone::Bad, ETone::Unknown })
		{
			Brushes.Add(SDreamTextureConnectionStrip::ToneBrush(Tone));
		}
		TestEqual(TEXT("五档用五张不同的图标"), Brushes.Num(), 5);
	}

	return true;
}

// ---------------------------------------------------------------------------
// 导入完成通知的文案
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureImportNotifierTest, "DreamTexture.Editor.LibraryView.Notifier",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureImportNotifierTest::RunTest(const FString& Parameters)
{
	// 成功：几张新建/更新/未变，带资产目录。
	{
		FDreamTextureImportStatus Status;
		Status.MaterialId = TEXT("mat_1");
		Status.Stage = EDreamTextureImportStage::Succeeded;
		Status.Result.bOK = true;
		Status.Result.AssetFolder = TEXT("/Game/DreamTexture/MossyRiverRocks_1f53");

		auto AddTex = [&Status](EDreamTextureImportAction Action)
		{
			FDreamTextureImportedTexture Tex;
			Tex.Action = Action;
			Status.Result.Textures.Add(Tex);
		};
		AddTex(EDreamTextureImportAction::Created);
		AddTex(EDreamTextureImportAction::Created);
		AddTex(EDreamTextureImportAction::Updated);
		AddTex(EDreamTextureImportAction::Unchanged);

		const FDreamTextureImportNotifier::FNoticeContent Content = FDreamTextureImportNotifier::Describe(Status);
		const FString Text = Content.Message.ToString();
		TestFalse(TEXT("成功不是错误"), Content.bIsError);
		TestEqual(TEXT("跳转目录"), Content.FolderToShow, Status.Result.AssetFolder);
		TestTrue(TEXT("新建 2 张"), Text.Contains(TEXT("2 textures created")));
		TestTrue(TEXT("更新 1 张"), Text.Contains(TEXT("1 updated")));
		TestTrue(TEXT("未变 1 张"), Text.Contains(TEXT("1 unchanged")));
		TestTrue(TEXT("带名字"), Text.Contains(TEXT("MossyRiverRocks_1f53")));
	}

	// 失败：中文原因，没有跳转目录。
	{
		FDreamTextureImportStatus Status;
		Status.MaterialId = TEXT("mat_2");
		Status.Stage = EDreamTextureImportStage::Failed;
		Status.Message = TEXT("下载 basecolor 失败：校验不符");

		const FDreamTextureImportNotifier::FNoticeContent Content = FDreamTextureImportNotifier::Describe(Status);
		TestTrue(TEXT("失败是错误"), Content.bIsError);
		TestTrue(TEXT("带原因"), Content.Message.ToString().Contains(TEXT("校验不符")));
		TestTrue(TEXT("失败不跳转"), Content.FolderToShow.IsEmpty());
	}

	// 「完成」但 bOK 为假（全部跳过）：按失败说，原因取 Result.Error。
	{
		FDreamTextureImportStatus Status;
		Status.MaterialId = TEXT("mat_3");
		Status.Stage = EDreamTextureImportStage::Succeeded;
		Status.Message = TEXT("完成");
		Status.Result.bOK = false;
		Status.Result.Error = TEXT("没有任何通道被导入");

		const FDreamTextureImportNotifier::FNoticeContent Content = FDreamTextureImportNotifier::Describe(Status);
		TestTrue(TEXT("bOK 为假按失败"), Content.bIsError);
		TestTrue(TEXT("原因取 Result.Error"), Content.Message.ToString().Contains(TEXT("没有任何通道被导入")));
	}

	// 没有任何原因：给一个兜底说法而不是空串。
	{
		FDreamTextureImportStatus Status;
		Status.MaterialId = TEXT("mat_4");
		Status.Stage = EDreamTextureImportStage::Failed;
		const FDreamTextureImportNotifier::FNoticeContent Content = FDreamTextureImportNotifier::Describe(Status);
		TestTrue(TEXT("兜底原因"), Content.Message.ToString().Contains(TEXT("Unknown reason")));
	}

	return true;
}

// ---------------------------------------------------------------------------
// M2：材质实例在通知与详情栏里的说法
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureMaterialInstanceViewTest, "DreamTexture.Editor.LibraryView.MaterialInstance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureMaterialInstanceViewTest::RunTest(const FString& Parameters)
{
	using EKind = FDreamTextureImportNotifier::FMaterialInstanceSummary::EKind;

	const FString Folder = TEXT("/Game/DreamTexture/Mossy_1f53");
	const FString MIPath = TEXT("/Game/DreamTexture/Mossy_1f53/MI_Mossy_1f53.MI_Mossy_1f53");

	auto MakeStatus = [&](EDreamTextureImportAction MIAction, const TCHAR* MIPathOrEmpty, const TCHAR* Note)
	{
		FDreamTextureImportStatus Status;
		Status.MaterialId = TEXT("mat_1");
		Status.Stage = EDreamTextureImportStage::Succeeded;
		Status.Result.bOK = true;
		Status.Result.AssetFolder = Folder;
		FDreamTextureImportedTexture Tex;
		Tex.Action = EDreamTextureImportAction::Created;
		Status.Result.Textures.Add(Tex);
		Status.Result.MaterialInstance.Action = MIAction;
		Status.Result.MaterialInstance.ObjectPath = MIPathOrEmpty;
		Status.Result.MaterialInstance.Note = Note;
		return Status;
	};

	// 新建 / 更新 / 未变：说出实例名，链接选中实例。
	const struct { EDreamTextureImportAction Action; const TCHAR* Word; } Present[] = {
		{ EDreamTextureImportAction::Created, TEXT("created") },
		{ EDreamTextureImportAction::Updated, TEXT("updated") },
		{ EDreamTextureImportAction::Unchanged, TEXT("unchanged") },
	};
	for (const auto& Case : Present)
	{
		const FDreamTextureImportNotifier::FNoticeContent Content =
			FDreamTextureImportNotifier::Describe(MakeStatus(Case.Action, *MIPath, TEXT("")), /*bCreateMaterialInstance*/ true);
		const FString Text = Content.Message.ToString();
		TestFalse(TEXT("成功不是错误"), Content.bIsError);
		TestEqual(TEXT("链接选中材质实例"), Content.AssetToSelect, MIPath);
		TestEqual(TEXT("目录仍保留作退路"), Content.FolderToShow, Folder);
		TestTrue(FString::Printf(TEXT("带实例名与动作：%s"), Case.Word), Text.Contains(FString::Printf(TEXT("MI_Mossy_1f53 %s"), Case.Word)));
	}

	// 桩的说法：Skipped + 「尚未实现」。显示原因，链接退回跳目录。
	{
		const FDreamTextureImportNotifier::FNoticeContent Content =
			FDreamTextureImportNotifier::Describe(MakeStatus(EDreamTextureImportAction::Skipped, TEXT(""), TEXT("材质实例尚未实现")), true);
		TestFalse(TEXT("没建实例不影响贴图成功"), Content.bIsError);
		TestTrue(TEXT("没有可选中的实例"), Content.AssetToSelect.IsEmpty());
		TestEqual(TEXT("退回跳目录"), Content.FolderToShow, Folder);
		TestTrue(TEXT("带原因"), Content.Message.ToString().Contains(TEXT("材质实例尚未实现")));
		TestTrue(TEXT("同时仍说贴图结果"), Content.Message.ToString().Contains(TEXT("1 texture created")));
	}

	// 开关关着：说是设置关的，不说成「没建出来」或故障，也不带实现方的 Note。
	{
		const FDreamTextureImportNotifier::FNoticeContent Content =
			FDreamTextureImportNotifier::Describe(MakeStatus(EDreamTextureImportAction::Skipped, TEXT(""), TEXT("设置里关了自动创建材质实例")), /*bCreateMaterialInstance*/ false);
		const FString Text = Content.Message.ToString();
		TestTrue(TEXT("说明是设置关的"), Text.Contains(TEXT("turned off")));
		TestTrue(TEXT("退回跳目录"), Content.AssetToSelect.IsEmpty() && Content.FolderToShow == Folder);

		const auto Summary = FDreamTextureImportNotifier::DescribeMaterialInstance(
			MakeStatus(EDreamTextureImportAction::Skipped, TEXT(""), TEXT("")).Result, false);
		TestTrue(TEXT("开关关着归为 DisabledBySetting"), Summary.Kind == EKind::DisabledBySetting);
	}

	// 开着却建失败：照实说原因。
	{
		const auto Summary = FDreamTextureImportNotifier::DescribeMaterialInstance(
			MakeStatus(EDreamTextureImportAction::Failed, TEXT(""), TEXT("母材质生成失败")).Result, false);
		TestTrue(TEXT("失败即使开关关着也照实说"), Summary.Kind == EKind::NotCreated);
		TestTrue(TEXT("带原因"), Summary.Sentence.ToString().Contains(TEXT("母材质生成失败")));
	}

	// 没有任何信息（老的队列实现，MaterialInstance 是默认值）：给兜底说法而不是空串，也不崩。
	{
		FDreamTextureImportStatus Status = MakeStatus(EDreamTextureImportAction::Skipped, TEXT(""), TEXT(""));
		const auto Summary = FDreamTextureImportNotifier::DescribeMaterialInstance(Status.Result, true);
		TestTrue(TEXT("没原因也有说法"), !Summary.Sentence.IsEmpty());
		TestTrue(TEXT("归为没建"), Summary.Kind == EKind::NotCreated);
	}

	// 贴图导入失败时，不提材质实例，只说失败。
	{
		FDreamTextureImportStatus Status;
		Status.MaterialId = TEXT("mat_2");
		Status.Stage = EDreamTextureImportStage::Failed;
		Status.Message = TEXT("下载失败");
		const FDreamTextureImportNotifier::FNoticeContent Content = FDreamTextureImportNotifier::Describe(Status, true);
		TestTrue(TEXT("失败是错误"), Content.bIsError);
		TestTrue(TEXT("失败不提材质实例"), !Content.Message.ToString().Contains(TEXT("material instance")));
		TestTrue(TEXT("失败没有可选中的东西"), Content.AssetToSelect.IsEmpty());
	}

	return true;
}

// ---------------------------------------------------------------------------
// 缩略图：解码缩小与路径
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureThumbDecodeTest, "DreamTexture.Editor.LibraryView.Thumb",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureThumbDecodeTest::RunTest(const FString& Parameters)
{
	IImageWrapperModule& Wrapper = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

	// 造一张 600×300 的渐变 PNG（长边超过缩略图尺寸，会被缩小）。
	const int32 SrcW = 600;
	const int32 SrcH = 300;
	TArray<FColor> Pixels;
	Pixels.SetNumUninitialized(SrcW * SrcH);
	for (int32 y = 0; y < SrcH; ++y)
	{
		for (int32 x = 0; x < SrcW; ++x)
		{
			Pixels[y * SrcW + x] = FColor(static_cast<uint8>(x % 256), static_cast<uint8>(y % 256), 128, 255);
		}
	}

	TSharedPtr<IImageWrapper> Encoder = Wrapper.CreateImageWrapper(EImageFormat::PNG);
	if (!TestTrue(TEXT("造得出测试 PNG"), Encoder.IsValid() && Encoder->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), SrcW, SrcH, ERGBFormat::BGRA, 8)))
	{
		return false;
	}
	const TArray64<uint8> Png = Encoder->GetCompressed();
	TestTrue(TEXT("PNG 非空"), Png.Num() > 0);

	int32 W = 0, H = 0;
	TArray<uint8> BGRA;
	TArray64<uint8> OutPng;
	const bool bOK = FDreamTextureThumbCache::DecodeAndShrink(Wrapper, TConstArrayView<uint8>(Png.GetData(), static_cast<int32>(Png.Num())),
		FDreamTextureThumbCache::ThumbMaxEdge, W, H, BGRA, &OutPng);
	TestTrue(TEXT("解码成功"), bOK);
	TestEqual(TEXT("长边缩到上限"), W, FDreamTextureThumbCache::ThumbMaxEdge);
	TestEqual(TEXT("保持宽高比"), H, FDreamTextureThumbCache::ThumbMaxEdge / 2);
	TestEqual(TEXT("BGRA 字节数"), BGRA.Num(), W * H * 4);
	TestTrue(TEXT("附带编码了缩小后的 PNG"), OutPng.Num() > 0);

	// 缩小后的 PNG 能被再次解码（磁盘缓存读回来就是这条路径）。
	int32 W2 = 0, H2 = 0;
	TArray<uint8> BGRA2;
	const bool bRoundTrip = FDreamTextureThumbCache::DecodeAndShrink(Wrapper, TConstArrayView<uint8>(OutPng.GetData(), static_cast<int32>(OutPng.Num())),
		FDreamTextureThumbCache::ThumbMaxEdge, W2, H2, BGRA2, nullptr);
	TestTrue(TEXT("缓存的 PNG 能读回"), bRoundTrip);
	TestEqual(TEXT("读回宽度不变"), W2, W);

	// 比缩略图还小的图不放大。
	int32 SmallW = 0, SmallH = 0;
	TArray<uint8> SmallBGRA;
	FDreamTextureThumbCache::DecodeAndShrink(Wrapper, TConstArrayView<uint8>(OutPng.GetData(), static_cast<int32>(OutPng.Num())),
		64, SmallW, SmallH, SmallBGRA, nullptr);
	TestEqual(TEXT("按更小的上限缩"), SmallW, 64);
	FDreamTextureThumbCache::DecodeAndShrink(Wrapper, TConstArrayView<uint8>(OutPng.GetData(), static_cast<int32>(OutPng.Num())),
		4096, SmallW, SmallH, SmallBGRA, nullptr);
	TestEqual(TEXT("只缩不放"), SmallW, W);

	// 垃圾字节：失败而不是崩。
	const uint8 Garbage[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	int32 GW = 0, GH = 0;
	TArray<uint8> GBGRA;
	TestFalse(TEXT("垃圾字节解码失败"), FDreamTextureThumbCache::DecodeAndShrink(Wrapper, TConstArrayView<uint8>(Garbage, 8),
		FDreamTextureThumbCache::ThumbMaxEdge, GW, GH, GBGRA, nullptr));
	TestFalse(TEXT("空输入失败"), FDreamTextureThumbCache::DecodeAndShrink(Wrapper, TConstArrayView<uint8>(),
		FDreamTextureThumbCache::ThumbMaxEdge, GW, GH, GBGRA, nullptr));

	// 磁盘路径：来自网络的 id 不能写到缓存目录外面。
	const FString Dir = FDreamTextureThumbCache::GetDiskDir();
	const FString Evil = FDreamTextureThumbCache::GetDiskPath(TEXT("../../evil"));
	TestTrue(TEXT("路径仍在缓存目录里"), FPaths::IsUnderDirectory(Evil, Dir));
	TestTrue(TEXT("是 PNG"), Evil.EndsWith(TEXT(".png")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
