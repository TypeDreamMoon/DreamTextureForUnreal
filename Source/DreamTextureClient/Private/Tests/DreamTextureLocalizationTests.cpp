// Copyright TypeDreamMoon. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureTestUtil.h"

#include "DreamTextureSettings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Text.h"
#include "Internationalization/TextLocalizationManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ObjectEditorUtils.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

/*
 * 本地化（M10）的引擎内验证。离线的工具链自测（Tools/Localization/selftest.py）只能证明「我们写的 .locres 与我们
 * 对引擎的理解一致」；这里让真正的 FTextLocalizationManager 去读，证明与引擎一致：
 *
 *   .Strings          loctext.json 里**每一条**源文：切到 zh-Hans 后，引擎实时表里取出的就是译文（语言中立的取回源文）；
 *                     切回 en 后又是英文源文。取不到、或取到的不对，说明键哈希或源文 CRC 对不上——引擎对这种情况不报错，
 *                     只是静默丢掉译文，界面回落成英文，别的任何地方都看不出来。
 *   .CompiledCallSites 本文件里用真正的 LOCTEXT / NSLOCTEXT 宏写了几条带 C 转义、相邻字面量、占位符、复数写法的文字。
 *                     编译器给出的字符串必须与扫描器解码出的 src 逐字相同，且切到 zh-Hans 后这些 FText 本身变成中文——
 *                     这是「扫描器解码」与「编译器」之间唯一的真对照。
 *   .Coverage         loctext.json 的每一条都有译文（或登记了语言中立），译文里没有指向不存在的 key。不碰引擎的文化。
 *   .SettingsPage     项目设置页（UDreamTextureSettings 的 UPROPERTY 元数据）：属性显示名、悬停提示、分类标题、分区标题与说明，
 *                     直接问引擎要，zh-Hans 下是中文、en 下是英文（提示不是头文件里的中文注释）。见本文件末尾的说明。
 *
 * 测试编辑器由 Tools/Test.ps1 钉在 -culture=en；这里切文化的测试在任何出口都切回原来的。
 */

namespace
{
	struct FLocTestEntry
	{
		FString Ns;
		FString Key;
		FString Src;

		FString Id() const { return Ns + TEXT("|") + Key; }
	};

	FString LocTestDataDir()
	{
		return FPaths::Combine(DreamTextureTest::GetPluginDir(), TEXT("Tools"), TEXT("Localization"));
	}

	bool LocTestParseJson(const FString& Path, TSharedPtr<FJsonValue>& OutValue, FString& OutError)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			OutError = FString::Printf(TEXT("cannot read %s"), *Path);
			return false;
		}
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, OutValue) || !OutValue.IsValid())
		{
			OutError = FString::Printf(TEXT("%s is not valid JSON"), *Path);
			return false;
		}
		return true;
	}

	/** loctext.json：[{ns, key, src, file}, …]。 */
	bool LocTestLoadEntries(TArray<FLocTestEntry>& OutEntries, FString& OutError)
	{
		TSharedPtr<FJsonValue> Root;
		if (!LocTestParseJson(FPaths::Combine(LocTestDataDir(), TEXT("loctext.json")), Root, OutError))
		{
			return false;
		}
		// 对不是数组 / 对象的 FJsonValue 取 AsArray / AsObject 会往 LogJson 写 Error（自动化里算失败），先判类型。
		if (Root->Type != EJson::Array)
		{
			OutError = TEXT("loctext.json is not an array");
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Item : Root->AsArray())
		{
			if (!Item.IsValid() || Item->Type != EJson::Object)
			{
				OutError = TEXT("loctext.json has an element that is not an object");
				return false;
			}
			const TSharedPtr<FJsonObject> Object = Item->AsObject();
			FLocTestEntry Entry;
			if (!Object->TryGetStringField(TEXT("ns"), Entry.Ns) || !Object->TryGetStringField(TEXT("key"), Entry.Key)
				|| !Object->TryGetStringField(TEXT("src"), Entry.Src))
			{
				OutError = TEXT("loctext.json has an element without ns / key / src");
				return false;
			}
			OutEntries.Add(MoveTemp(Entry));
		}
		return true;
	}

	/** translations/*.json 合并成 "ns|key" -> 译文。同一个键出现在两处就是错误（Python 侧的校验同样拦它）。 */
	bool LocTestLoadTranslations(TMap<FString, FString>& OutTranslations, TArray<FString>& OutErrors)
	{
		const FString Dir = FPaths::Combine(LocTestDataDir(), TEXT("translations"));
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *FPaths::Combine(Dir, TEXT("*.json")), true, false);
		Files.Sort();

		for (const FString& File : Files)
		{
			TSharedPtr<FJsonValue> Root;
			FString Error;
			if (!LocTestParseJson(FPaths::Combine(Dir, File), Root, Error) || Root->Type != EJson::Object)
			{
				OutErrors.Add(FString::Printf(TEXT("translations/%s: %s"), *File, Error.IsEmpty() ? TEXT("not an object") : *Error));
				continue;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Root->AsObject()->Values)
			{
				if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String)
				{
					OutErrors.Add(FString::Printf(TEXT("translations/%s: %s is not a string"), *File, *Pair.Key));
					continue;
				}
				if (OutTranslations.Contains(Pair.Key))
				{
					OutErrors.Add(FString::Printf(TEXT("%s is translated twice (second time in translations/%s)"), *Pair.Key, *File));
					continue;
				}
				OutTranslations.Add(Pair.Key, Pair.Value->AsString());
			}
		}
		return true;
	}

	/** language_neutral.json：翻不翻都一样的条目登记（键 -> 原因）。不存在当作没有。 */
	void LocTestLoadNeutral(TSet<FString>& OutNeutral, TArray<FString>& OutErrors)
	{
		const FString Path = FPaths::Combine(LocTestDataDir(), TEXT("language_neutral.json"));
		if (!FPaths::FileExists(Path))
		{
			return;
		}
		TSharedPtr<FJsonValue> Root;
		FString Error;
		if (!LocTestParseJson(Path, Root, Error) || Root->Type != EJson::Object)
		{
			OutErrors.Add(TEXT("language_neutral.json is unreadable or not an object"));
			return;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Root->AsObject()->Values)
		{
			OutNeutral.Add(Pair.Key);
		}
	}

	/**
	 * zh-Hans 下每一条该显示什么：登记了语言中立的是源文本身，其余是译文，没有译文的不在表里。
	 */
	TMap<FString, FString> LocTestExpectedZh(const TArray<FLocTestEntry>& Entries, const TMap<FString, FString>& Translations, const TSet<FString>& Neutral)
	{
		TMap<FString, FString> Expected;
		for (const FLocTestEntry& Entry : Entries)
		{
			if (Neutral.Contains(Entry.Id()))
			{
				Expected.Add(Entry.Id(), Entry.Src);
			}
			else if (const FString* Translation = Translations.Find(Entry.Id()))
			{
				Expected.Add(Entry.Id(), *Translation);
			}
		}
		return Expected;
	}

	/**
	 * 构造时记下当前文化，析构时（含测试中途 return、断言失败）一定切回去。
	 * 切文化会让 FTextLocalizationManager 重新加载本地化资源；默认是同步等的，这里再显式等一次，
	 * 免得后一个测试在资源只加载了一半时读到半新半旧的文字。
	 */
	class FLocTestCultureScope
	{
	public:
		FLocTestCultureScope()
		{
			FInternationalization::Get().BackupCultureState(Saved);
		}

		~FLocTestCultureScope()
		{
			FInternationalization::Get().RestoreCultureState(Saved);
			FTextLocalizationManager::Get().WaitForAsyncTasks();
		}

		static bool Switch(const FString& Culture)
		{
			const bool bOk = FInternationalization::Get().SetCurrentLanguageAndLocale(Culture);
			FTextLocalizationManager::Get().WaitForAsyncTasks();
			return bOk;
		}

	private:
		FInternationalization::FCultureStateSnapshot Saved;
	};

	/** 找不到数据文件时的收法：跳过并写明（Tools/Test.ps1 认「已跳过」打头的 Info）。源码 checkout 里它们必然存在。 */
	bool LocTestSkipIfNoData(FAutomationTestBase& Test)
	{
		if (!FPaths::FileExists(FPaths::Combine(LocTestDataDir(), TEXT("loctext.json"))))
		{
			Test.AddInfo(TEXT("已跳过：这份插件里没有 Tools/Localization/loctext.json（只有源码 checkout 才带这些数据文件）。"));
			return true;
		}
		return false;
	}

	/** 逐条核对实时表。最多报 limit 条细节，其余只计数，免得一次坏掉刷出九百行。 */
	int32 LocTestCheckLiveTable(FAutomationTestBase& Test, const TCHAR* Label, const TArray<FLocTestEntry>& Entries,
		const TMap<FString, FString>& Expected, bool bEnglish)
	{
		int32 Bad = 0;
		for (const FLocTestEntry& Entry : Entries)
		{
			const FString* Want = bEnglish ? &Entry.Src : Expected.Find(Entry.Id());
			if (!Want)
			{
				continue;   // 没有译文的条目由 .Coverage 报，这里不重复
			}
			FText Found;
			if (!FText::FindTextInLiveTable_Advanced(FTextKey(*Entry.Ns), FTextKey(*Entry.Key), Found, &Entry.Src))
			{
				if (++Bad <= 12)
				{
					Test.AddError(FString::Printf(TEXT("[%s] %s is not in the live table, or its source hash does not match the code (engine drops such a translation silently)"), Label, *Entry.Id()));
				}
				continue;
			}
			if (!Found.ToString().Equals(*Want, ESearchCase::CaseSensitive))
			{
				if (++Bad <= 12)
				{
					Test.AddError(FString::Printf(TEXT("[%s] %s: expected '%s', got '%s'"), Label, *Entry.Id(), **Want, *Found.ToString()));
				}
			}
		}
		if (Bad > 12)
		{
			Test.AddError(FString::Printf(TEXT("[%s] ...and %d more mismatches"), Label, Bad - 12));
		}
		return Bad;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureLocalizationStringsTest, "DreamTexture.Localization.ZhHans.Strings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureLocalizationStringsTest::RunTest(const FString& Parameters)
{
	if (LocTestSkipIfNoData(*this))
	{
		return true;
	}

	TArray<FLocTestEntry> Entries;
	FString Error;
	if (!LocTestLoadEntries(Entries, Error))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("loctext.json is not empty"), Entries.Num() > 0);

	TMap<FString, FString> Translations;
	TArray<FString> LoadErrors;
	LocTestLoadTranslations(Translations, LoadErrors);
	TSet<FString> Neutral;
	LocTestLoadNeutral(Neutral, LoadErrors);
	for (const FString& LoadError : LoadErrors)
	{
		AddError(LoadError);
	}
	const TMap<FString, FString> Expected = LocTestExpectedZh(Entries, Translations, Neutral);

	// 之后无论怎么出去，文化都切回开始时的样子。
	FLocTestCultureScope CultureScope;

	TestTrue(TEXT("switch to zh-Hans"), FLocTestCultureScope::Switch(TEXT("zh-Hans")));
	const int32 BadZh = LocTestCheckLiveTable(*this, TEXT("zh-Hans"), Entries, Expected, /*bEnglish=*/false);
	TestEqual(TEXT("entries that are not the zh-Hans translation"), BadZh, 0);

	// 带参数的一条走一遍 FText::Format：译文里的占位符真的能被引擎替换。
	for (const FLocTestEntry& Entry : Entries)
	{
		const FString* Translation = Translations.Find(Entry.Id());
		if (Translation && Entry.Src.Contains(TEXT("{0}")) && !Entry.Src.Contains(TEXT("{1}")) && Translation->Contains(TEXT("{0}")))
		{
			FText Pattern;
			if (FText::FindTextInLiveTable_Advanced(FTextKey(*Entry.Ns), FTextKey(*Entry.Key), Pattern, &Entry.Src))
			{
				const FString Formatted = FText::Format(Pattern, FText::FromString(TEXT("@@7@@"))).ToString();
				TestTrue(*FString::Printf(TEXT("%s formats with its argument"), *Entry.Id()), Formatted.Contains(TEXT("@@7@@")) && !Formatted.Contains(TEXT("{0}")));
			}
			break;
		}
	}

	// 切回英文：实时表里必须回到源文。引擎只在「该文化的 .locres 里有这一条」时才覆盖实时表，
	// 所以这一步同时验证了 en/ 下那份「原文 -> 原文」的原生文化 .locres 在起作用。
	TestTrue(TEXT("switch back to en"), FLocTestCultureScope::Switch(TEXT("en")));
	const int32 BadEn = LocTestCheckLiveTable(*this, TEXT("en"), Entries, Expected, /*bEnglish=*/true);
	TestEqual(TEXT("entries that are not the English source after switching back"), BadEn, 0);

	return true;
}

// ----------------------------------------------------------------------------------- 真宏

#define LOCTEXT_NAMESPACE "DreamTextureLocalizationTest"

namespace
{
	struct FLocTestFixture
	{
		const TCHAR* Key;
		FText Text;
	};

	/**
	 * 每次调用都重新求值（宏每次都去实时表里取），所以文化切换之后再调它拿到的是新文化的文字。
	 * 写法故意刁钻：这些就是扫描器最容易与编译器看法不一致的地方。key 要与 loctext.json 里的一一对应。
	 */
	TArray<FLocTestFixture> LocTestMakeFixtures()
	{
		TArray<FLocTestFixture> Fixtures;
		Fixtures.Add({ TEXT("Escapes"), LOCTEXT("Escapes", "tab\there \"quoted\" back\\slash · dot \x41\x42 \101 \U0001F600 emoji") });
		Fixtures.Add({ TEXT("Concat"), LOCTEXT("Concat", "first part of a long message, "
			"second part, " // 相邻字面量之间有注释也照样拼起来
			"third part.") });
		Fixtures.Add({ TEXT("Explicit"), NSLOCTEXT("DreamTextureLocalizationTest", "Explicit", "explicit namespace, argument {0}") });
		Fixtures.Add({ TEXT("Plural"), LOCTEXT("Plural", "{0} {0}|plural(one=file,other=files)") });
		return Fixtures;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureLocalizationCallSitesTest, "DreamTexture.Localization.ZhHans.CompiledCallSites",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureLocalizationCallSitesTest::RunTest(const FString& Parameters)
{
	if (LocTestSkipIfNoData(*this))
	{
		return true;
	}

	TArray<FLocTestEntry> Entries;
	FString Error;
	if (!LocTestLoadEntries(Entries, Error))
	{
		AddError(Error);
		return false;
	}
	TMap<FString, FString> Translations;
	TArray<FString> LoadErrors;
	LocTestLoadTranslations(Translations, LoadErrors);
	TSet<FString> Neutral;
	LocTestLoadNeutral(Neutral, LoadErrors);
	for (const FString& LoadError : LoadErrors)
	{
		AddError(LoadError);
	}

	const FString Namespace = TEXT("DreamTextureLocalizationTest");
	const TArray<FLocTestFixture> Probe = LocTestMakeFixtures();
	TestEqual(TEXT("fixture count"), Probe.Num(), 4);

	FLocTestCultureScope CultureScope;

	// 先在 en 下：编译器给出的字符串必须与扫描器解码出的 src 逐字相同。
	TestTrue(TEXT("switch to en"), FLocTestCultureScope::Switch(TEXT("en")));
	for (const FLocTestFixture& Fixture : LocTestMakeFixtures())
	{
		const FLocTestEntry* Entry = Entries.FindByPredicate([&](const FLocTestEntry& E) { return E.Ns.Equals(Namespace, ESearchCase::CaseSensitive) && E.Key.Equals(Fixture.Key, ESearchCase::CaseSensitive); });
		if (!TestNotNull(*FString::Printf(TEXT("loctext.json lists %s|%s (run Tools/Localization/sync_loctext.py)"), *Namespace, Fixture.Key), Entry))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("%s: compiler string equals the scanner's decoded src"), Fixture.Key), Fixture.Text.ToString(), Entry->Src);
	}

	// 再在 zh-Hans 下：这些 FText 本身（不是通过 FindText 查出来的）变成译文。
	TestTrue(TEXT("switch to zh-Hans"), FLocTestCultureScope::Switch(TEXT("zh-Hans")));
	for (const FLocTestFixture& Fixture : LocTestMakeFixtures())
	{
		const FString Id = Namespace + TEXT("|") + Fixture.Key;
		const FString* Translation = Translations.Find(Id);
		if (!TestNotNull(*FString::Printf(TEXT("%s has a translation"), *Id), Translation))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("%s in zh-Hans"), Fixture.Key), Fixture.Text.ToString(), *Translation);
	}

	// 复数写法：英文取 one / other，中文译文只写 {0}。
	{
		const FLocTestFixture* Plural = nullptr;
		const TArray<FLocTestFixture> Fixtures = LocTestMakeFixtures();
		for (const FLocTestFixture& Fixture : Fixtures)
		{
			if (FCString::Strcmp(Fixture.Key, TEXT("Plural")) == 0)
			{
				Plural = &Fixture;
			}
		}
		if (TestNotNull(TEXT("plural fixture"), Plural))
		{
			TestEqual(TEXT("zh-Hans plural with 1"), FText::Format(Plural->Text, FText::AsNumber(1)).ToString(), FString(TEXT("1 个文件")));
			TestEqual(TEXT("zh-Hans plural with 5"), FText::Format(Plural->Text, FText::AsNumber(5)).ToString(), FString(TEXT("5 个文件")));
		}
	}

	TestTrue(TEXT("switch back to en"), FLocTestCultureScope::Switch(TEXT("en")));
	{
		const TArray<FLocTestFixture> Fixtures = LocTestMakeFixtures();
		for (const FLocTestFixture& Fixture : Fixtures)
		{
			if (FCString::Strcmp(Fixture.Key, TEXT("Plural")) == 0)
			{
				TestEqual(TEXT("en plural with 1"), FText::Format(Fixture.Text, FText::AsNumber(1)).ToString(), FString(TEXT("1 file")));
				TestEqual(TEXT("en plural with 5"), FText::Format(Fixture.Text, FText::AsNumber(5)).ToString(), FString(TEXT("5 files")));
			}
		}
	}
	return true;
}

#undef LOCTEXT_NAMESPACE

// ----------------------------------------------------------------------------------- 覆盖率

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureLocalizationCoverageTest, "DreamTexture.Localization.ZhHans.Coverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureLocalizationCoverageTest::RunTest(const FString& Parameters)
{
	if (LocTestSkipIfNoData(*this))
	{
		return true;
	}

	TArray<FLocTestEntry> Entries;
	FString Error;
	if (!LocTestLoadEntries(Entries, Error))
	{
		AddError(Error);
		return false;
	}

	TMap<FString, FString> Translations;
	TArray<FString> LoadErrors;
	LocTestLoadTranslations(Translations, LoadErrors);
	TSet<FString> Neutral;
	LocTestLoadNeutral(Neutral, LoadErrors);
	for (const FString& LoadError : LoadErrors)
	{
		AddError(LoadError);
	}

	int32 Missing = 0;
	TSet<FString> Live;
	for (const FLocTestEntry& Entry : Entries)
	{
		Live.Add(Entry.Id());
		const FString* Translation = Translations.Find(Entry.Id());
		const bool bNeutral = Neutral.Contains(Entry.Id());
		if (!bNeutral && (!Translation || Translation->IsEmpty()))
		{
			if (++Missing <= 12)
			{
				AddError(FString::Printf(TEXT("no zh-Hans translation: %s  (source: '%s')"), *Entry.Id(), *Entry.Src));
			}
		}
		if (bNeutral && Translation)
		{
			AddError(FString::Printf(TEXT("%s is registered language-neutral AND translated; keep one"), *Entry.Id()));
		}
	}
	if (Missing > 12)
	{
		AddError(FString::Printf(TEXT("...and %d more untranslated entries"), Missing - 12));
	}
	TestEqual(TEXT("entries without a zh-Hans translation"), Missing, 0);

	// 译文不能指向不存在的 key（key 改名之后旧译文会悄悄变成孤儿，这条译文就再也不会被用到）。
	int32 Orphans = 0;
	for (const TPair<FString, FString>& Pair : Translations)
	{
		if (!Live.Contains(Pair.Key) && ++Orphans <= 12)
		{
			AddError(FString::Printf(TEXT("translation for a key that is not in loctext.json: %s"), *Pair.Key));
		}
	}
	for (const FString& Id : Neutral)
	{
		if (!Live.Contains(Id) && ++Orphans <= 12)
		{
			AddError(FString::Printf(TEXT("language-neutral registration for a key that is not in loctext.json: %s"), *Id));
		}
	}
	TestEqual(TEXT("translations pointing at missing keys"), Orphans, 0);

	AddInfo(FString::Printf(TEXT("%d entries, %d translations, %d language-neutral."), Entries.Num(), Translations.Num(), Neutral.Num()));
	return true;
}

// ----------------------------------------------------------------------------------- 项目设置页

#if WITH_EDITOR

namespace
{
	/** 英文提示 / 显示名里不该出现中日韩字符：U+3000 之后是 CJK 符号标点、假名、汉字和全角形式；英文里的 > … 都在它前面。 */
	bool LocTestContainsCjk(const FString& Text)
	{
		for (const TCHAR Char : Text)
		{
			if (Char >= 0x3000)
			{
				return true;
			}
		}
		return false;
	}
}

/*
 * 项目设置页（UPROPERTY 的元数据）走的是引擎另外三个命名空间，不是 LOCTEXT：
 *   UObjectDisplayNames  显示名，key = 类名:属性名（类本身是类名）
 *   UObjectToolTips      悬停提示，同上
 *   UObjectCategory      分类，key = 分类字符串
 * 这里直接问引擎（FField::GetDisplayNameText / GetToolTipText、UDeveloperSettings::GetSectionText、
 * FObjectEditorUtils::GetCategoryText），而不是去查实时表——查实时表只能证明 .locres 写对了，
 * 证明不了引擎真的会去问这个 key、拿这个源文去比 CRC。
 *
 * 数据驱动：期望值取自 loctext.json（扫描器从头文件里读出来的）和 translations/*.json，所以将来给设置类加属性，
 * 只要扫描器认得它，这里自动覆盖；反过来，类上每个面板可见的属性都必须在清单里有显示名和提示。
 * 另外夹着几条写死的字面量，免得数据自洽地错了（比如显示名的推导错了、中文提示不小心写成了英文）。
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureLocalizationSettingsPageTest, "DreamTexture.Localization.ZhHans.SettingsPage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureLocalizationSettingsPageTest::RunTest(const FString& Parameters)
{
	if (LocTestSkipIfNoData(*this))
	{
		return true;
	}

	TArray<FLocTestEntry> Entries;
	FString Error;
	if (!LocTestLoadEntries(Entries, Error))
	{
		AddError(Error);
		return false;
	}
	TMap<FString, FString> Translations;
	TArray<FString> LoadErrors;
	LocTestLoadTranslations(Translations, LoadErrors);
	TSet<FString> Neutral;
	LocTestLoadNeutral(Neutral, LoadErrors);
	for (const FString& LoadError : LoadErrors)
	{
		AddError(LoadError);
	}
	const TMap<FString, FString> ExpectedZh = LocTestExpectedZh(Entries, Translations, Neutral);

	UClass* const Class = UDreamTextureSettings::StaticClass();
	const FString ClassKey = Class->GetName();
	TestEqual(TEXT("the reflected class name is the key prefix without the U"), ClassKey, FString(TEXT("DreamTextureSettings")));

	// 清单里属于设置页的条目，按引擎的三个命名空间分开。
	TMap<FString, const FLocTestEntry*> Names;
	TMap<FString, const FLocTestEntry*> Tips;
	TMap<FString, const FLocTestEntry*> Categories;
	for (const FLocTestEntry& Entry : Entries)
	{
		const bool bOurs = Entry.Key == ClassKey || Entry.Key.StartsWith(ClassKey + TEXT(":"));
		if (Entry.Ns == TEXT("UObjectDisplayNames") && bOurs)
		{
			Names.Add(Entry.Key, &Entry);
		}
		else if (Entry.Ns == TEXT("UObjectToolTips") && bOurs)
		{
			Tips.Add(Entry.Key, &Entry);
		}
		else if (Entry.Ns == TEXT("UObjectCategory"))
		{
			Categories.Add(Entry.Key, &Entry);
		}
	}
	if (!TestTrue(TEXT("loctext.json lists the settings page (run Tools/Localization/sync_loctext.py)"), Names.Contains(ClassKey) && Names.Num() > 1))
	{
		return false;
	}

	// 类上每个面板可见（CPF_Edit）的属性，在清单里都要有显示名、提示和分类；清单里的属性在类上都要找得到。
	TArray<const FProperty*> Props;
	for (TFieldIterator<FProperty> It(Class, EFieldIterationFlags::None); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Edit))
		{
			Props.Add(*It);
		}
	}
	TestTrue(TEXT("the class has editable properties"), Props.Num() > 0);
	for (const FProperty* Prop : Props)
	{
		const FString Key = ClassKey + TEXT(":") + Prop->GetName();
		const FString Category = Prop->GetMetaData(TEXT("Category"));
		TestTrue(*FString::Printf(TEXT("%s has a display name in loctext.json"), *Key), Names.Contains(Key));
		TestTrue(*FString::Printf(TEXT("%s has a tooltip in loctext.json"), *Key), Tips.Contains(Key));
		TestTrue(*FString::Printf(TEXT("%s has category '%s' in loctext.json"), *Key, *Category), Categories.Contains(Category));
	}
	for (const TPair<FString, const FLocTestEntry*>& Pair : Names)
	{
		FString PropName;
		if (Pair.Key.Split(TEXT(":"), nullptr, &PropName))
		{
			TestNotNull(*FString::Printf(TEXT("%s names a real property"), *Pair.Key), FindFProperty<FProperty>(Class, *PropName));
		}
	}

	// 某个文化下该是什么：中文取译文（语言中立的取源文），英文取源文。
	auto CheckPage = [&](const TCHAR* Label, const bool bChinese)
	{
		auto Want = [&](const FLocTestEntry* Entry) -> FString
		{
			return bChinese ? ExpectedZh.FindRef(Entry->Id()) : Entry->Src;
		};
		auto Check = [&](const FString& What, const FText& Got, const FString& Expected)
		{
			TestEqual(*FString::Printf(TEXT("[%s] %s"), Label, *What), Got.ToString(), Expected);
		};

		// 类：项目设置里这一栏的标题与说明。
		Check(TEXT("class display name"), Class->GetDisplayNameText(), Want(Names[ClassKey]));
		Check(TEXT("section title"), GetDefault<UDreamTextureSettings>()->GetSectionText(), Want(Names[ClassKey]));
		if (const FLocTestEntry* const* ClassTip = Tips.Find(ClassKey))
		{
			Check(TEXT("class tooltip"), Class->GetToolTipText(), Want(*ClassTip));
			Check(TEXT("section description"), GetDefault<UDreamTextureSettings>()->GetSectionDescription(), Want(*ClassTip));
		}

		for (const FProperty* Prop : Props)
		{
			const FString Key = ClassKey + TEXT(":") + Prop->GetName();
			const FString Category = Prop->GetMetaData(TEXT("Category"));
			if (const FLocTestEntry* const* Name = Names.Find(Key))
			{
				Check(Key + TEXT(" display name"), Prop->GetDisplayNameText(), Want(*Name));
				if (!bChinese)
				{
					TestFalse(*FString::Printf(TEXT("[%s] %s display name has no CJK"), Label, *Key), LocTestContainsCjk(Prop->GetDisplayNameText().ToString()));
				}
			}
			if (const FLocTestEntry* const* Tip = Tips.Find(Key))
			{
				Check(Key + TEXT(" tooltip"), Prop->GetToolTipText(), Want(*Tip));
				// 没有显式 ToolTip 的话，引擎在英文界面下拿到的是头文件里的中文注释——这条就是防它的。
				const bool bHasCjk = LocTestContainsCjk(Prop->GetToolTipText().ToString());
				if (bChinese)
				{
					TestTrue(*FString::Printf(TEXT("[%s] %s tooltip is Chinese"), Label, *Key), bHasCjk);
				}
				else
				{
					TestFalse(*FString::Printf(TEXT("[%s] %s tooltip is English (not the Chinese doc comment)"), Label, *Key), bHasCjk);
				}
			}
			if (const FLocTestEntry* const* Cat = Categories.Find(Category))
			{
				// 属性自己的分类（FObjectEditorUtils::GetCategoryText(FField*)）：key 与源文都是分类字符串。
				Check(Key + TEXT(" category"), FObjectEditorUtils::GetCategoryText(Prop), Want(*Cat));
			}
		}

		// 细节面板里的分类标题走另一个重载（FName）：英文下会再做一次 NameToDisplayString 美化，中文直接取译文。
		for (const TPair<FString, const FLocTestEntry*>& Pair : Categories)
		{
			const FString ExpectedCategory = bChinese ? Want(Pair.Value) : FName::NameToDisplayString(Pair.Key, false);
			Check(TEXT("category title ") + Pair.Key, FObjectEditorUtils::GetCategoryText(FName(*Pair.Key)), ExpectedCategory);
		}
	};

	// 写死的几条：数据自洽地错了（推导错、译文写成英文）时，数据驱动的比对发现不了。
	auto CheckLiterals = [&](const TCHAR* Label, const bool bChinese)
	{
		struct FSpot
		{
			const TCHAR* Property;
			const TCHAR* English;
			const TCHAR* Chinese;
		};
		static const FSpot Spots[] =
		{
			{ TEXT("BackendURL"), TEXT("Backend URL"), TEXT("后端地址") },
			{ TEXT("BackendExecutable"), TEXT("Backend Executable"), TEXT("后端可执行文件") },
			{ TEXT("BackendArguments"), TEXT("Backend Arguments"), TEXT("启动参数") },
			{ TEXT("bAutoStartBackend"), TEXT("Auto Start Backend"), TEXT("自动启动后端") },
			{ TEXT("ReconnectBaseMs"), TEXT("Reconnect Base (ms)"), TEXT("重连起始退避（毫秒）") },
			{ TEXT("bCreateMaterialInstance"), TEXT("Create Material Instance"), TEXT("自动创建材质实例") },
		};
		for (const FSpot& Spot : Spots)
		{
			const FProperty* Prop = FindFProperty<FProperty>(Class, Spot.Property);
			if (TestNotNull(*FString::Printf(TEXT("[%s] property %s"), Label, Spot.Property), Prop))
			{
				TestEqual(*FString::Printf(TEXT("[%s] %s display name"), Label, Spot.Property), Prop->GetDisplayNameText().ToString(), FString(bChinese ? Spot.Chinese : Spot.English));
			}
		}
		const FProperty* Url = FindFProperty<FProperty>(Class, TEXT("BackendURL"));
		if (TestNotNull(TEXT("BackendURL"), Url))
		{
			TestTrue(*FString::Printf(TEXT("[%s] BackendURL tooltip starts with the right words"), Label),
				Url->GetToolTipText().ToString().StartsWith(bChinese ? TEXT("后端地址") : TEXT("Backend address")));
		}
		TestEqual(*FString::Printf(TEXT("[%s] category Connection"), Label), FObjectEditorUtils::GetCategoryText(FName(TEXT("Connection"))).ToString(), FString(bChinese ? TEXT("连接") : TEXT("Connection")));
		TestEqual(*FString::Printf(TEXT("[%s] category Import"), Label), FObjectEditorUtils::GetCategoryText(FName(TEXT("Import"))).ToString(), FString(bChinese ? TEXT("导入") : TEXT("Import")));
		TestEqual(*FString::Printf(TEXT("[%s] category Generate"), Label), FObjectEditorUtils::GetCategoryText(FName(TEXT("Generate"))).ToString(), FString(bChinese ? TEXT("生成") : TEXT("Generate")));
		TestEqual(*FString::Printf(TEXT("[%s] section title is the product name"), Label), GetDefault<UDreamTextureSettings>()->GetSectionText().ToString(), FString(TEXT("DreamTexture")));
	};

	// 之后无论怎么出去，文化都切回开始时的样子。
	FLocTestCultureScope CultureScope;

	TestTrue(TEXT("switch to en"), FLocTestCultureScope::Switch(TEXT("en")));
	CheckPage(TEXT("en"), /*bChinese=*/false);
	CheckLiterals(TEXT("en"), /*bChinese=*/false);

	TestTrue(TEXT("switch to zh-Hans"), FLocTestCultureScope::Switch(TEXT("zh-Hans")));
	CheckPage(TEXT("zh-Hans"), /*bChinese=*/true);
	CheckLiterals(TEXT("zh-Hans"), /*bChinese=*/true);

	// 切回英文：引擎只在「该文化的 .locres 里有这一条」时才覆盖实时表，所以这一步也验证了 en/ 那份原生文化的 .locres 管着这三个命名空间。
	TestTrue(TEXT("switch back to en"), FLocTestCultureScope::Switch(TEXT("en")));
	CheckPage(TEXT("en again"), /*bChinese=*/false);
	CheckLiterals(TEXT("en again"), /*bChinese=*/false);

	AddInfo(FString::Printf(TEXT("%d properties, %d display names, %d tooltips, %d categories."), Props.Num(), Names.Num(), Tips.Num(), Categories.Num()));
	return true;
}

#else

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureLocalizationSettingsPageTest, "DreamTexture.Localization.ZhHans.SettingsPage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureLocalizationSettingsPageTest::RunTest(const FString& Parameters)
{
	AddInfo(TEXT("已跳过：项目设置页的元数据只在带编辑器的构建里有。"));
	return true;
}

#endif // WITH_EDITOR

#endif // WITH_DEV_AUTOMATION_TESTS
