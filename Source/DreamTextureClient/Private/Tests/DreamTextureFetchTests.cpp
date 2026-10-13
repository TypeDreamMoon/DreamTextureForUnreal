// Copyright TypeDreamMoon. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureApi.h"
#include "DreamTextureDownloadStore.h"
#include "DreamTextureHttpUtil.h"
#include "DreamTextureManifest.h"
#include "DreamTextureSettings.h"
#include "DreamTextureTestUtil.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"

/*
 * 下载与落盘中**不需要后端**的那部分。
 *
 * 这里的 DownloadMaps 用例都被设计成「根本不会发出 HTTP 请求」（路径非法、全部已是最新……）：
 * 默认的后端地址 127.0.0.1:8777 上说不定正跑着用户自己的真实后端，离线测试不该去敲它。
 * 真的走网络的下载由 DreamTexture.Live.Fetch 覆盖。
 */

namespace
{
	/** 取固定素材的 manifest。读不到就让调用的测试失败。 */
	bool LoadFixtureManifest(FAutomationTestBase& Test, FDreamTextureManifest& Out)
	{
		FString Text;
		FString Error;
		if (!DreamTextureTest::LoadFixtureText(TEXT("manifest.json"), Text))
		{
			Test.AddError(TEXT("读不到固定素材的 manifest.json"));
			return false;
		}
		if (!FDreamTextureManifest::FromJsonString(Text, Out, Error))
		{
			Test.AddError(FString::Printf(TEXT("固定素材的 manifest 解析失败：%s"), *Error));
			return false;
		}
		return true;
	}

	/** 每个测试一个独立的临时目录，放在工程 Saved 下，用完整个删掉。 */
	FString MakeScratchDir()
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::ProjectSavedDir() / TEXT("DreamTexture") / TEXT("Tests") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}

	int32 CountFilesWithExtension(const FString& Dir, const TCHAR* Extension)
	{
		TArray<FString> Found;
		IFileManager::Get().FindFiles(Found, *FPaths::Combine(Dir, FString::Printf(TEXT("*.%s"), Extension)), true, false);
		return Found.Num();
	}

	/** 一次 DownloadMaps 的观察记录：回调了几次、收到了什么。放在共享引用里让 lambda 共用。 */
	struct FObservedFetch
	{
		int32 DoneCount = 0;
		FDreamTextureFetchResult Result;
		TArray<TPair<int32, int32>> Progress;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureSafeSegmentTest, "DreamTexture.Client.Fetch.SafePathSegment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureSafeSegmentTest::RunTest(const FString& Parameters)
{
	const TCHAR* const Safe[] = {
		TEXT("mat_20260827_092650_1f53"), TEXT("mat_20260827_777001_ref"), TEXT("basecolor.png"),
		TEXT("a-b_c.d"), TEXT("X"), TEXT(".hidden"),
	};
	for (const TCHAR* Text : Safe)
	{
		TestTrue(FString::Printf(TEXT("允许：%s"), Text), FDreamTextureFetcher::IsSafePathSegment(Text));
	}

	// ".." 的每个字符都在白名单里，正好是穿越，所以单独列出来。
	const TCHAR* const Unsafe[] = {
		TEXT(""), TEXT("."), TEXT(".."), TEXT("../x"), TEXT("..\\x"), TEXT("a/b"), TEXT("a\\b"), TEXT("a..b"),
		TEXT("C:\\x"), TEXT("C:x"), TEXT("a b"), TEXT("a%2e%2e"), TEXT("a\tb"), TEXT("名字"), TEXT("a:b"), TEXT("a*b"),
		TEXT("a?b"), TEXT("a\"b"), TEXT("a<b"), TEXT("a|b"), TEXT("~"),
	};
	for (const TCHAR* Text : Unsafe)
	{
		TestFalse(FString::Printf(TEXT("拒绝：「%s」"), Text), FDreamTextureFetcher::IsSafePathSegment(Text));
	}

	// 不干净的 ID 拿不到下载目录，不会被拼成一个穿出去的路径。
	TestTrue(TEXT("非法 ID 的下载目录为空"), FDreamTextureFetcher::GetDownloadDir(TEXT("../evil")).IsEmpty());
	TestTrue(TEXT("空 ID 的下载目录为空"), FDreamTextureFetcher::GetDownloadDir(FString()).IsEmpty());

	// 正常 ID：落在 <工程 Saved>/DreamTexture/Downloads/<id>。
	const FString Dir = FDreamTextureFetcher::GetDownloadDir(DreamTextureTest::FixtureId());
	const FString Expected = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamTexture/Downloads") / DreamTextureTest::FixtureId());
	TestEqual(TEXT("下载目录"), Dir, Expected);
	TestTrue(TEXT("下载目录是绝对路径"), !FPaths::IsRelative(Dir));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureDefaultChannelsTest, "DreamTexture.Client.Fetch.DefaultChannels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureDefaultChannelsTest::RunTest(const FString& Parameters)
{
	UDreamTextureSettings* Settings = GetMutableDefault<UDreamTextureSettings>();
	const bool bPrevious = Settings->bImportSeparateChannels;

	Settings->bImportSeparateChannels = false;
	const TArray<FString> Basic = FDreamTextureFetcher::DefaultChannels();
	TestEqual(TEXT("默认 4 个通道"), Basic.Num(), 4);
	TestTrue(TEXT("含 basecolor / normal / orm / height"),
		Basic.Contains(DreamTextureChannel::BaseColor) && Basic.Contains(DreamTextureChannel::Normal)
		&& Basic.Contains(DreamTextureChannel::ORM) && Basic.Contains(DreamTextureChannel::Height));
	TestFalse(TEXT("默认不含 roughness"), Basic.Contains(DreamTextureChannel::Roughness));

	Settings->bImportSeparateChannels = true;
	const TArray<FString> Full = FDreamTextureFetcher::DefaultChannels();
	TestEqual(TEXT("开了分通道之后 7 个"), Full.Num(), 7);
	TestTrue(TEXT("多出 roughness / metallic / ao"),
		Full.Contains(DreamTextureChannel::Roughness) && Full.Contains(DreamTextureChannel::Metallic) && Full.Contains(DreamTextureChannel::AO));

	Settings->bImportSeparateChannels = bPrevious;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureStoreVerifiedTest, "DreamTexture.Client.Fetch.StoreVerified",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureStoreVerifiedTest::RunTest(const FString& Parameters)
{
	FDreamTextureManifest Manifest;
	if (!LoadFixtureManifest(*this, Manifest))
	{
		return false;
	}
	const FDreamTextureMapInfo* Info = Manifest.FindMap(DreamTextureChannel::BaseColor);
	TArray<uint8> Good;
	if (!TestNotNull(TEXT("有 basecolor"), Info) || !TestTrue(TEXT("读得到 basecolor.png"), DreamTextureTest::LoadFixtureBytes(Info->File, Good)))
	{
		return false;
	}

	const FString Scratch = MakeScratchDir();
	const FString Final = FPaths::Combine(Scratch, TEXT("basecolor.png"));
	IFileManager& FileManager = IFileManager::Get();
	FileManager.MakeDirectory(*Scratch, true);

	// 1. 内容对：落盘成功，字节一致，目录里不留临时文件。
	{
		FString Error;
		const bool bOK = DreamTexture::StoreVerifiedFile(Good, Info->Bytes, Info->Sha256, Final, Error);
		TestTrue(FString::Printf(TEXT("正确的内容落盘成功：%s"), *Error), bOK);

		TArray<uint8> OnDisk;
		TestTrue(TEXT("正式文件存在"), FFileHelper::LoadFileToArray(OnDisk, *Final));
		TestTrue(TEXT("落盘字节与来源一致"), OnDisk == Good);
		TestEqual(TEXT("没有残留的 .part"), CountFilesWithExtension(Scratch, TEXT("part")), 0);
	}

	// 2. sha 不符（哪怕只差一位）：拒绝，且不动已有的好文件。
	{
		FString WrongSha = Info->Sha256;
		WrongSha[WrongSha.Len() - 1] = (WrongSha[WrongSha.Len() - 1] == TEXT('0')) ? TEXT('1') : TEXT('0');

		FString Error;
		TestFalse(TEXT("sha256 不符被拒绝"), DreamTexture::StoreVerifiedFile(Good, Info->Bytes, WrongSha, Final, Error));
		TestTrue(FString::Printf(TEXT("原因里提到 sha256（实际：%s）"), *Error), Error.Contains(TEXT("sha256")));

		TArray<uint8> OnDisk;
		FFileHelper::LoadFileToArray(OnDisk, *Final);
		TestTrue(TEXT("失败时已有的好文件原封不动"), OnDisk == Good);
		TestEqual(TEXT("失败后没有残留的 .part"), CountFilesWithExtension(Scratch, TEXT("part")), 0);
	}

	// 3. 被截断：manifest 写的字节数对不上，直接拒绝（不必等哈希）。
	{
		TArray<uint8> Truncated = Good;
		Truncated.SetNum(Truncated.Num() - 10);

		FString Error;
		TestFalse(TEXT("截断的内容被拒绝（按字节数）"), DreamTexture::StoreVerifiedFile(Truncated, Info->Bytes, Info->Sha256, Final, Error));
		TestTrue(FString::Printf(TEXT("原因里提到大小（实际：%s）"), *Error), Error.Contains(TEXT("Size mismatch")));

		// manifest 没给字节数（<=0）时，截断仍然会被哈希抓住。
		FString Error2;
		TestFalse(TEXT("截断的内容被拒绝（按 sha256）"), DreamTexture::StoreVerifiedFile(Truncated, 0, Info->Sha256, Final, Error2));
		TestTrue(FString::Printf(TEXT("原因里提到 sha256（实际：%s）"), *Error2), Error2.Contains(TEXT("sha256")));
	}

	// 4. 全新的目标名 + 错误内容：正式名上不能凭空出现文件。
	{
		const FString Other = FPaths::Combine(Scratch, TEXT("never.png"));
		TArray<uint8> Garbage;
		Garbage.Init(0x5A, 256);

		FString Error;
		TestFalse(TEXT("乱码内容被拒绝"), DreamTexture::StoreVerifiedFile(Garbage, 0, Info->Sha256, Other, Error));
		TestFalse(TEXT("正式名上没有出现文件"), FileManager.FileExists(*Other));
		TestEqual(TEXT("也没有临时文件（校验在落盘前做）"), CountFilesWithExtension(Scratch, TEXT("part")), 0);
	}

	// 5. 重新下载：正式名已存在（内容不同）时要能盖掉。
	{
		FFileHelper::SaveStringToFile(TEXT("stale"), *Final);

		FString Error;
		TestTrue(FString::Printf(TEXT("覆盖旧文件成功：%s"), *Error), DreamTexture::StoreVerifiedFile(Good, Info->Bytes, Info->Sha256, Final, Error));

		TArray<uint8> OnDisk;
		FFileHelper::LoadFileToArray(OnDisk, *Final);
		TestTrue(TEXT("旧内容已被换成正确内容"), OnDisk == Good);
	}

	// 6. sha 大小写不敏感（manifest 将来要是写成大写也不该误判）。
	{
		FString Error;
		TestTrue(FString::Printf(TEXT("大写 sha 也认：%s"), *Error), DreamTexture::StoreVerifiedFile(Good, Info->Bytes, Info->Sha256.ToUpper(), Final, Error));
	}

	FileManager.DeleteDirectory(*Scratch, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureFetchOfflineTest, "DreamTexture.Client.Fetch.Offline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureFetchOfflineTest::RunTest(const FString& Parameters)
{
	FDreamTextureManifest Fixture;
	if (!LoadFixtureManifest(*this, Fixture))
	{
		return false;
	}

	// 默认通道取决于「导入分通道图」这个设置。下面的计划（要下哪些通道）是在 DownloadMaps 调用时同步决定的，
	// 只有收尾回调是异步的，所以只需要在这个函数的范围内把设置钉死，不会漏到后面的 latent 命令里。
	struct FSeparateChannelsGuard
	{
		FSeparateChannelsGuard()
		{
			UDreamTextureSettings* Settings = GetMutableDefault<UDreamTextureSettings>();
			Previous = Settings->bImportSeparateChannels;
			Settings->bImportSeparateChannels = false;
		}
		~FSeparateChannelsGuard()
		{
			GetMutableDefault<UDreamTextureSettings>()->bImportSeparateChannels = Previous;
		}
		bool Previous = false;
	} SeparateChannelsGuard;

	FAutomationTestBase* Test = this;
	auto MakeObserver = []() { return MakeShared<FObservedFetch>(); };
	auto Launch = [](const FDreamTextureManifest& Manifest, const FDreamTextureFetchOptions& Options, const TSharedRef<FObservedFetch>& Observed)
	{
		FDreamTextureFetcher::DownloadMaps(Manifest, Options,
			[Observed](FDreamTextureFetchResult&& Result)
			{
				++Observed->DoneCount;
				Observed->Result = MoveTemp(Result);
			},
			[Observed](int32 Done, int32 Total)
			{
				Observed->Progress.Emplace(Done, Total);
			});
	};

	// ---- 1. 材质 ID 是路径穿越：必须拒绝，且回调是异步的 ----
	{
		FDreamTextureManifest Evil = Fixture;
		Evil.Id = TEXT("../../evil");

		const TSharedRef<FObservedFetch> Observed = MakeObserver();
		Launch(Evil, FDreamTextureFetchOptions(), Observed);
		TestEqual(TEXT("[非法 ID] DownloadMaps 返回时回调还没跑（回调永远异步）"), Observed->DoneCount, 0);

		ADD_LATENT_AUTOMATION_COMMAND(FDreamTextureWaitCommand(Test, TEXT("非法 ID 的 Done 回调"), 5.0,
			[Observed]() { return Observed->DoneCount > 0; },
			[Test, Observed]()
			{
				Test->TestEqual(TEXT("[非法 ID] Done 只回调一次"), Observed->DoneCount, 1);
				Test->TestFalse(TEXT("[非法 ID] bOK 为假"), Observed->Result.bOK);
				Test->TestTrue(TEXT("[非法 ID] 给出中文原因"), Observed->Result.Error.Contains(TEXT("ID")));
				Test->TestEqual(TEXT("[非法 ID] 什么都没拿到"), Observed->Result.LocalFiles.Num(), 0);
				Test->TestTrue(TEXT("[非法 ID] 没有给出落盘目录"), Observed->Result.LocalDir.IsEmpty());
			}));
	}

	// ---- 2. 文件名是路径穿越：该通道进 Skipped，bOK 为假；别的通道不受牵连 ----
	{
		FDreamTextureManifest Evil = Fixture;
		Evil.Maps.FindChecked(DreamTextureChannel::BaseColor).File = TEXT("../../evil.png");

		// 只要 basecolor + 一个已知哈希的 normal：整个过程不会发出任何请求。
		FDreamTextureFetchOptions Options;
		Options.Channels = { DreamTextureChannel::BaseColor, DreamTextureChannel::Normal };
		Options.KnownHashes.Add(DreamTextureChannel::Normal, Fixture.FindMap(DreamTextureChannel::Normal)->Sha256);

		const TSharedRef<FObservedFetch> Observed = MakeObserver();
		Launch(Evil, Options, Observed);

		ADD_LATENT_AUTOMATION_COMMAND(FDreamTextureWaitCommand(Test, TEXT("非法文件名的 Done 回调"), 5.0,
			[Observed]() { return Observed->DoneCount > 0; },
			[Test, Observed]()
			{
				const FDreamTextureFetchResult& Result = Observed->Result;
				Test->TestEqual(TEXT("[非法文件名] Done 只回调一次"), Observed->DoneCount, 1);
				Test->TestFalse(TEXT("[非法文件名] bOK 为假"), Result.bOK);
				Test->TestTrue(TEXT("[非法文件名] basecolor 记进了 Skipped"), Result.Skipped.Contains(DreamTextureChannel::BaseColor));
				Test->TestEqual(TEXT("[非法文件名] 什么文件都没有落盘"), Result.LocalFiles.Num(), 0);
				Test->TestTrue(TEXT("[非法文件名] normal 仍然被识别为已是最新"), Result.UpToDate.Contains(DreamTextureChannel::Normal));
				Test->TestTrue(TEXT("[非法文件名] 原因里点名了 basecolor"), Result.Error.Contains(TEXT("basecolor")));
			}));
	}

	// ---- 3. 全部已是最新：什么都不下，bOK 为真 ----
	{
		FDreamTextureFetchOptions Options;
		for (const TPair<FString, FDreamTextureMapInfo>& Pair : Fixture.Maps)
		{
			Options.KnownHashes.Add(Pair.Key, Pair.Value.Sha256);
		}

		const TSharedRef<FObservedFetch> Observed = MakeObserver();
		Launch(Fixture, Options, Observed);

		ADD_LATENT_AUTOMATION_COMMAND(FDreamTextureWaitCommand(Test, TEXT("全部已是最新的 Done 回调"), 5.0,
			[Observed]() { return Observed->DoneCount > 0; },
			[Test, Observed]()
			{
				const FDreamTextureFetchResult& Result = Observed->Result;
				Test->TestTrue(FString::Printf(TEXT("[全部已是最新] bOK 为真（%s）"), *Result.Error), Result.bOK);
				Test->TestEqual(TEXT("[全部已是最新] 默认 4 个通道都是 UpToDate"), Result.UpToDate.Num(), 4);
				Test->TestEqual(TEXT("[全部已是最新] 一个文件都没下"), Result.LocalFiles.Num(), 0);
				Test->TestEqual(TEXT("[全部已是最新] 没有 Skipped"), Result.Skipped.Num(), 0);
				Test->TestEqual(TEXT("[全部已是最新] 没有进度回调（没有文件要下）"), Observed->Progress.Num(), 0);
				Test->TestEqual(TEXT("[全部已是最新] 落盘目录"), Result.LocalDir, FDreamTextureFetcher::GetDownloadDir(DreamTextureTest::FixtureId()));
			}));

		// 再多等一会儿：确认 Done 真的只有一次，而不是「第一次回调完又补一次」。
		// 计时从这条命令真正开始执行时算起，而不是从排队时算起。
		const TSharedRef<double> WindowStart = MakeShared<double>(0.0);
		ADD_LATENT_AUTOMATION_COMMAND(FDreamTextureWaitCommand(Test, TEXT("观察窗口"), 5.0,
			[WindowStart]()
			{
				if (*WindowStart == 0.0)
				{
					*WindowStart = FPlatformTime::Seconds();
				}
				return FPlatformTime::Seconds() - *WindowStart > 0.5;
			},
			[Test, Observed]() { Test->TestEqual(TEXT("[全部已是最新] 过了半秒 Done 仍然只有一次"), Observed->DoneCount, 1); }));
	}

	// ---- 4. 要的通道 manifest 里一个都没有：不是失败，但也不能报成功 ----
	{
		FDreamTextureManifest Empty = Fixture;
		Empty.Maps.Empty();

		const TSharedRef<FObservedFetch> Observed = MakeObserver();
		Launch(Empty, FDreamTextureFetchOptions(), Observed);

		ADD_LATENT_AUTOMATION_COMMAND(FDreamTextureWaitCommand(Test, TEXT("无通道的 Done 回调"), 5.0,
			[Observed]() { return Observed->DoneCount > 0; },
			[Test, Observed]()
			{
				const FDreamTextureFetchResult& Result = Observed->Result;
				Test->TestEqual(TEXT("[无通道] Done 只回调一次"), Observed->DoneCount, 1);
				Test->TestFalse(TEXT("[无通道] 没有东西可导入，不能报成功"), Result.bOK);
				Test->TestEqual(TEXT("[无通道] 4 个默认通道都记进 Skipped"), Result.Skipped.Num(), 4);
				Test->TestFalse(TEXT("[无通道] 有原因"), Result.Error.IsEmpty());
			}));
	}

	// ---- 5. manifest 没给 sha256：没法校验就不收 ----
	{
		FDreamTextureManifest NoHash = Fixture;
		NoHash.Maps.FindChecked(DreamTextureChannel::Height).Sha256.Empty();

		FDreamTextureFetchOptions Options;
		Options.Channels = { DreamTextureChannel::Height };

		const TSharedRef<FObservedFetch> Observed = MakeObserver();
		Launch(NoHash, Options, Observed);

		ADD_LATENT_AUTOMATION_COMMAND(FDreamTextureWaitCommand(Test, TEXT("缺 sha256 的 Done 回调"), 5.0,
			[Observed]() { return Observed->DoneCount > 0; },
			[Test, Observed]()
			{
				const FDreamTextureFetchResult& Result = Observed->Result;
				Test->TestFalse(TEXT("[缺 sha256] bOK 为假"), Result.bOK);
				Test->TestTrue(TEXT("[缺 sha256] height 记进 Skipped"), Result.Skipped.Contains(DreamTextureChannel::Height));
				Test->TestEqual(TEXT("[缺 sha256] 什么都没落盘"), Result.LocalFiles.Num(), 0);
			}));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureSendFailureTest, "DreamTexture.Client.Http.SendFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureSendFailureTest::RunTest(const FString& Parameters)
{
	// 空 URL：ProcessRequest() 会直接返回 false。5.8 在这条路径上会先同步触发完成委托再返回 false
	// （FHttpRequestCommon::PreProcess），所以朴素的「返回 false 就补一次回调」会让调用方收到两次。
	// 这里证明 SendRequest 只回调一次，并且是异步的。不会发出任何网络请求。

	struct FObserved
	{
		int32 Calls = 0;
		DreamTexture::FHttpOutcome Outcome;
	};
	const TSharedRef<FObserved> Observed = MakeShared<FObserved>();

	DreamTexture::SendRequest(TEXT("GET"), FString(), 5.0f, FString(),
		[Observed](const DreamTexture::FHttpOutcome& Outcome)
		{
			++Observed->Calls;
			Observed->Outcome = Outcome;
		});
	TestEqual(TEXT("SendRequest 返回时回调还没跑（回调永远异步）"), Observed->Calls, 0);

	FAutomationTestBase* Test = this;
	ADD_LATENT_AUTOMATION_COMMAND(FDreamTextureWaitCommand(Test, TEXT("发不出去的请求的回调"), 5.0,
		[Observed]() { return Observed->Calls > 0; }));

	const TSharedRef<double> WindowStart = MakeShared<double>(0.0);
	ADD_LATENT_AUTOMATION_COMMAND(FDreamTextureWaitCommand(Test, TEXT("观察窗口"), 5.0,
		[WindowStart]()
		{
			if (*WindowStart == 0.0)
			{
				*WindowStart = FPlatformTime::Seconds();
			}
			return FPlatformTime::Seconds() - *WindowStart > 0.5;
		},
		[Test, Observed]()
		{
			Test->TestEqual(TEXT("只回调一次（半秒内没有第二次）"), Observed->Calls, 1);
			Test->TestFalse(TEXT("bOK 为假"), Observed->Outcome.bOK);
			Test->TestFalse(TEXT("给出了原因"), Observed->Outcome.Error.IsEmpty());
		}));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
