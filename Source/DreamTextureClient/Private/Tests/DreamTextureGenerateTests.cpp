// Copyright TypeDreamMoon. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureConnection.h"
#include "DreamTextureGenerate.h"
#include "DreamTextureHttpUtil.h"
#include "DreamTextureJobTracker.h"
#include "DreamTextureTestUtil.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"

/*
 * 「在 UE 里生成」的 Client 层：目录解析、请求体、种子上限、任务与事件的解析、任务追踪的状态推进。
 *
 * 全部不连后端：目录用 Tests/Data/Generate/workflows.json（从真实 /api/workflows 的应答里截出来的，
 * 去掉了 outputs 和模型清单），其余用内联的固定 JSON；追踪器的网络层换成假的，回调攥在手里，
 * 由测试决定什么时候、以什么顺序回。真的连后端的部分见 DreamTexture.Live.Generate。
 */

namespace
{
	TSharedPtr<FJsonObject> Parse(const FString& Json)
	{
		return DreamTexture::ParseJsonObject(Json);
	}

	/** workflows.json 是旧后端截的；workflows.upload.json 是新后端（带「已有底图」出图段）截的。 */
	bool LoadCatalogFixture(FAutomationTestBase& Test, FDreamTextureWorkflowCatalog& Out, const TCHAR* FileName = TEXT("workflows.json"))
	{
		const FString Path = FPaths::Combine(DreamTextureTest::GetPluginDir(), TEXT("Tests"), TEXT("Data"), TEXT("Generate"), FileName);
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			Test.AddError(FString::Printf(TEXT("读不到目录固定数据：%s"), *Path));
			return false;
		}
		if (!FDreamTextureWorkflowCatalog::FromJson(Parse(Text), Out))
		{
			Test.AddError(TEXT("目录固定数据解析失败"));
			return false;
		}
		return true;
	}

	/** 一个任务事件。字段按需给，省略的不写进 JSON（模拟后端 omitempty）。 */
	FDreamTextureEvent MakeJobEvent(const TCHAR* Type, const FString& JobId, const TCHAR* Status, double Progress = 0.0,
		const TCHAR* Stage = TEXT(""), const TCHAR* Batch = TEXT("b1"), const TCHAR* MaterialId = TEXT("mat_1"), const TCHAR* Error = TEXT(""))
	{
		FString Json = FString::Printf(TEXT("{\"type\":\"%s\",\"job\":{\"id\":\"%s\",\"material_id\":\"%s\",\"workflow_id\":\"w\",\"batch_id\":\"%s\","
			"\"params\":{\"seed\":42,\"resolution\":1024},\"status\":\"%s\",\"progress\":%f"),
			Type, *JobId, MaterialId, Batch, Status, Progress);
		if (FCString::Strlen(Stage) > 0)
		{
			Json += FString::Printf(TEXT(",\"stage\":\"%s\""), Stage);
		}
		if (FCString::Strlen(Error) > 0)
		{
			Json += FString::Printf(TEXT(",\"error\":\"%s\""), Error);
		}
		Json += TEXT("}}");

		FDreamTextureEvent Event;
		Event.Type = Type;
		Event.Raw = Parse(Json);
		return Event;
	}

	FDreamTextureJob MakeJob(const FString& Id, EDreamTextureJobStatus Status, float Progress = 0.0f, const TCHAR* Batch = TEXT("b1"))
	{
		FDreamTextureJob Job;
		Job.Id = Id;
		Job.MaterialId = TEXT("mat_") + Id;
		Job.WorkflowId = TEXT("w");
		Job.BatchId = Batch;
		Job.Status = Status;
		Job.Progress = Progress;
		return Job;
	}

	/** 追踪器的假网络层：回调攥在手里。 */
	struct FFakeHooks
	{
		struct FGet
		{
			FString Id;
			FDreamTextureGenerateApi::FOnJob Done;
		};

		TArray<FGet> Gets;
		TArray<FDreamTextureGenerateRequest> SubmitRequests;
		TArray<FDreamTextureGenerateApi::FOnSubmitted> SubmitDones;
		TArray<FString> CancelIds;
		TArray<FDreamTextureGenerateApi::FOnDone> CancelDones;

		FDreamTextureJobTracker::FHooks Make()
		{
			FDreamTextureJobTracker::FHooks Out;
			Out.GetJob = [this](const FString& Id, FDreamTextureGenerateApi::FOnJob Done)
			{
				Gets.Add({ Id, MoveTemp(Done) });
			};
			Out.Submit = [this](const FDreamTextureGenerateRequest& Request, FDreamTextureGenerateApi::FOnSubmitted Done)
			{
				SubmitRequests.Add(Request);
				SubmitDones.Add(MoveTemp(Done));
			};
			Out.Cancel = [this](const FString& Id, FDreamTextureGenerateApi::FOnDone Done)
			{
				CancelIds.Add(Id);
				CancelDones.Add(MoveTemp(Done));
			};
			return Out;
		}

		void CompleteGet(int32 Index, const FDreamTextureJob& Job)
		{
			FDreamTextureJob Copy = Job;
			Gets[Index].Done(true, MoveTemp(Copy), FString(), false);
		}
		void FailGet(int32 Index, bool bNotFound)
		{
			Gets[Index].Done(false, FDreamTextureJob(), TEXT("查询失败"), bNotFound);
		}
	};

	TSharedRef<FDreamTextureJobTracker> MakeTracker(FFakeHooks& Fake)
	{
		return MakeShared<FDreamTextureJobTracker>(Fake.Make());
	}
}

// ---------------------------------------------------------------------------
// 目录
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateCatalogTest, "DreamTexture.Client.Generate.Catalog.Parse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateCatalogTest::RunTest(const FString& Parameters)
{
	FDreamTextureWorkflowCatalog Catalog;
	if (!LoadCatalogFixture(*this, Catalog))
	{
		return false;
	}

	TestEqual(TEXT("四条工作流"), Catalog.Workflows.Num(), 4);
	TestEqual(TEXT("六个段"), Catalog.Segments.Num(), 6);

	// 出图段 / 分解段按 id 排序，图片类不在其中。
	const TArray<const FDreamTextureWorkflowMeta*> Sources = Catalog.GetSourceSegments();
	if (TestEqual(TEXT("四个出图段"), Sources.Num(), 4))
	{
		TestEqual(TEXT("出图段排序 0"), Sources[0]->Id, FString(TEXT("src-api-realistic-v1")));
		TestEqual(TEXT("出图段排序 3"), Sources[3]->Id, FString(TEXT("src-local-stylized-v1")));
	}
	const TArray<const FDreamTextureWorkflowMeta*> Decomposes = Catalog.GetDecomposeSegments();
	if (TestEqual(TEXT("两个分解段"), Decomposes.Num(), 2))
	{
		TestEqual(TEXT("分解段 0"), Decomposes[0]->Id, FString(TEXT("dec-chord-v1")));
		TestEqual(TEXT("分解段 1"), Decomposes[1]->Id, FString(TEXT("dec-derive-v1")));
	}

	// 组合 id 是 <出图段>.<分解段>。
	TestEqual(TEXT("组合 id 的拼法"), FDreamTextureWorkflowCatalog::MakeComposedId(TEXT("a"), TEXT("b")), FString(TEXT("a.b")));
	const FDreamTextureWorkflowMeta* Chord = Catalog.FindComposed(TEXT("src-local-realistic-v1"), TEXT("dec-chord-v1"));
	const FDreamTextureWorkflowMeta* Derive = Catalog.FindComposed(TEXT("src-local-realistic-v1"), TEXT("dec-derive-v1"));
	if (!TestNotNull(TEXT("找得到本地写实 × CHORD"), Chord) || !TestNotNull(TEXT("找得到本地写实 × 传统派生"), Derive))
	{
		return false;
	}
	TestTrue(TEXT("组合出来的带两个来源字段"), Chord->IsComposed());
	TestEqual(TEXT("来源段"), Chord->SourceSegment, FString(TEXT("src-local-realistic-v1")));
	TestEqual(TEXT("分解段"), Chord->DecomposeSegment, FString(TEXT("dec-chord-v1")));
	TestEqual(TEXT("组合 id 就是工作流 id"), Chord->Id, FString(TEXT("src-local-realistic-v1.dec-chord-v1")));
	TestTrue(TEXT("目录里没有的组合找不到"), Catalog.FindComposed(TEXT("src-api-realistic-v1"), TEXT("dec-chord-v1")) == nullptr);
	TestTrue(TEXT("空 id 找不到"), Catalog.FindComposed(FString(), TEXT("dec-chord-v1")) == nullptr);

	// 许可看并集：CHORD 是 research-only，传统派生没有许可限制。
	TestFalse(TEXT("CHORD 组合不可商用"), Chord->IsCommercial());
	TestEqual(TEXT("许可条数"), Chord->Licenses.Num(), 1);
	TestTrue(TEXT("原因格式与后端 Commercial() 一致：组件: 许可"),
		Chord->GetNonCommercialReason().StartsWith(TEXT("CHORD (dt.pbr_estimate / dt.chord_model): Ubisoft Machine Learning License")));
	TestTrue(TEXT("传统派生可商用"), Derive->IsCommercial());
	TestTrue(TEXT("可商用时原因为空"), Derive->GetNonCommercialReason().IsEmpty());
	TestFalse(TEXT("云端底图 × CHORD 同样不可商用（许可来自分解段）"),
		Catalog.FindWorkflow(TEXT("src-api-stylized-v1.dec-chord-v1"))->IsCommercial());

	// 云端底图：看段上的 source 字段。
	const FDreamTextureWorkflowMeta* CloudSeg = Catalog.FindSegment(TEXT("src-api-stylized-v1"));
	const FDreamTextureWorkflowMeta* LocalSeg = Catalog.FindSegment(TEXT("src-local-stylized-v1"));
	if (TestNotNull(TEXT("云端段"), CloudSeg) && TestNotNull(TEXT("本地段"), LocalSeg))
	{
		TestTrue(TEXT("云端段标着云端底图"), CloudSeg->bCloudSource);
		TestEqual(TEXT("云端来源"), CloudSeg->CloudProvider, FString(TEXT("openai")));
		TestFalse(TEXT("本地段不是"), LocalSeg->bCloudSource);
		TestTrue(TEXT("段的类别"), CloudSeg->Segment == EDreamTextureSegmentKind::Source);
		TestEqual(TEXT("画面类型"), CloudSeg->Domain, FString(TEXT("stylized")));
		const FDreamTextureParamSpec* Hidden = CloudSeg->FindParam(TEXT("source_image"));
		if (TestNotNull(TEXT("云端段有 source_image 参数"), Hidden))
		{
			TestTrue(TEXT("它是后端自己填的隐藏参数"), Hidden->bHidden);
		}
	}

	// 搭不上的提醒：手绘 × CHORD。
	const FDreamTextureWorkflowMeta* Mismatch = Catalog.FindWorkflow(TEXT("src-api-stylized-v1.dec-chord-v1"));
	if (TestNotNull(TEXT("手绘 × CHORD"), Mismatch))
	{
		TestFalse(TEXT("带提醒"), Mismatch->Mismatch.IsEmpty());
	}
	TestTrue(TEXT("写实 × CHORD 没有提醒"), Chord->Mismatch.IsEmpty());

	// 分解段声明吃得下哪些画面。
	const FDreamTextureWorkflowMeta* ChordSeg = Catalog.FindSegment(TEXT("dec-chord-v1"));
	if (TestNotNull(TEXT("CHORD 段"), ChordSeg))
	{
		TestTrue(TEXT("分解段"), ChordSeg->Segment == EDreamTextureSegmentKind::Decompose);
		if (TestEqual(TEXT("只吃一种画面"), ChordSeg->ExpectsDomain.Num(), 1))
		{
			TestEqual(TEXT("只吃写实"), ChordSeg->ExpectsDomain[0], FString(TEXT("realistic")));
		}
		TestFalse(TEXT("段自己的许可"), ChordSeg->IsCommercial());
	}

	// 参数：分辨率是 enum，候选和默认值都是整数；种子默认 -1；提示词是多行。
	const FDreamTextureParamSpec* Resolution = Chord->FindParam(TEXT("resolution"));
	if (TestNotNull(TEXT("分辨率参数"), Resolution))
	{
		TArray<int32> Options;
		TestTrue(TEXT("候选全是整数"), Resolution->TryGetIntOptions(Options));
		if (TestEqual(TEXT("两个候选"), Options.Num(), 2))
		{
			TestEqual(TEXT("候选 0"), Options[0], 768);
			TestEqual(TEXT("候选 1"), Options[1], 1024);
		}
		TestEqual(TEXT("默认值"), Resolution->GetDefaultInt(0), 1024);
		TestEqual(TEXT("候选的文本不带小数点"), Resolution->Options[1], FString(TEXT("1024")));
	}
	const FDreamTextureParamSpec* Seed = Chord->FindParam(TEXT("seed"));
	if (TestNotNull(TEXT("种子参数"), Seed))
	{
		TestEqual(TEXT("种子默认 -1"), Seed->GetDefaultInt(0), -1);
		TestEqual(TEXT("种子是 int"), Seed->Type, FString(TEXT("int")));
	}
	const FDreamTextureParamSpec* Prompt = Chord->FindParam(TEXT("prompt"));
	if (TestNotNull(TEXT("提示词参数"), Prompt))
	{
		TestTrue(TEXT("多行"), Prompt->bMultiline);
	}
	TestTrue(TEXT("没有的参数找不到"), Chord->FindParam(TEXT("nope")) == nullptr);

	// 云端出图的分辨率在高级参数里：FindParam 两处都找。
	const FDreamTextureWorkflowMeta* Cloud = Catalog.FindWorkflow(TEXT("src-api-stylized-v1.dec-chord-v1"));
	TestTrue(TEXT("云端组合的分辨率在高级参数里也找得到"), Cloud != nullptr && Cloud->FindParam(TEXT("resolution")) != nullptr);

	// 图片类管线：不分段、没有组合关系。
	const FDreamTextureWorkflowMeta* Image = Catalog.FindWorkflow(TEXT("local-image-v1"));
	if (TestNotNull(TEXT("图片管线"), Image))
	{
		TestEqual(TEXT("kind"), Image->Kind, FString(TEXT("image")));
		TestFalse(TEXT("不是组合"), Image->IsComposed());
		TestTrue(TEXT("没有许可提示就是可商用"), Image->IsCommercial());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateSegmentsTest, "DreamTexture.Client.Generate.Segments",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateSegmentsTest::RunTest(const FString& Parameters)
{
	// 新后端的目录：多了「已有底图」出图段 src-upload-v1（segment=source，没有 prompt，唯一的输入是必填的 source_image）。
	FDreamTextureWorkflowCatalog Catalog;
	if (!LoadCatalogFixture(*this, Catalog, TEXT("workflows.upload.json")))
	{
		return false;
	}

	const FDreamTextureWorkflowMeta* Upload = Catalog.FindSegment(TEXT("src-upload-v1"));
	if (!TestNotNull(TEXT("目录里有 src-upload-v1"), Upload))
	{
		return false;
	}
	TestEqual(TEXT("七个段（含已有底图）"), Catalog.Segments.Num(), 7);
	TestTrue(TEXT("它是出图段"), Upload->Segment == EDreamTextureSegmentKind::Source);
	TestTrue(TEXT("它没有 prompt 参数"), Upload->FindParam(TEXT("prompt")) == nullptr);
	if (const FDreamTextureParamSpec* Source = Upload->FindParam(TEXT("source_image")))
	{
		TestTrue(TEXT("source_image 是必填"), Source->bRequired);
		TestEqual(TEXT("类型是 image"), Source->Type, FString(TEXT("image")));
	}
	else
	{
		AddError(TEXT("已有底图段里没有 source_image 参数"));
	}
	{
		const FDreamTextureParamSpec* Resolution = Upload->FindParam(TEXT("resolution"));
		TestTrue(TEXT("别的参数不是必填"), Resolution != nullptr && !Resolution->bRequired);
	}

	// 它拼出来的组合仍在 workflows 里（别的页面，比如回流，要提交它们）。
	TestTrue(TEXT("组合 src-upload-v1.dec-chord-v1 仍在目录里"), Catalog.FindComposed(TEXT("src-upload-v1"), TEXT("dec-chord-v1")) != nullptr);
	TestTrue(TEXT("组合 src-upload-v1.dec-derive-v1 仍在目录里"), Catalog.FindComposed(TEXT("src-upload-v1"), TEXT("dec-derive-v1")) != nullptr);

	// 但生成页的出图模型下拉里不能有它：选了它提交必然 400。
	const TArray<const FDreamTextureWorkflowMeta*> Sources = Catalog.GetSourceSegments();
	TestEqual(TEXT("生成页能用的出图段只有四个"), Sources.Num(), 4);
	for (const FDreamTextureWorkflowMeta* Meta : Sources)
	{
		TestTrue(*FString::Printf(TEXT("%s 有提示词可填"), *Meta->Id), Meta->FindParam(TEXT("prompt")) != nullptr);
		TestFalse(*FString::Printf(TEXT("%s 不是已有底图"), *Meta->Id), Meta->Id == TEXT("src-upload-v1"));
	}
	TestEqual(TEXT("分解段不受影响"), Catalog.GetDecomposeSegments().Num(), 2);

	// 判据与 Web 端一致：只看**基础**参数里有没有 prompt。提示词挪到高级参数里的段不列。
	{
		FDreamTextureWorkflowCatalog Moved = Catalog;
		for (FDreamTextureWorkflowMeta& Meta : Moved.Segments)
		{
			if (Meta.Id == TEXT("src-local-stylized-v1"))
			{
				for (int32 i = Meta.Params.Num() - 1; i >= 0; --i)
				{
					if (Meta.Params[i].Key == TEXT("prompt"))
					{
						Meta.Advanced.Add(Meta.Params[i]);
						Meta.Params.RemoveAt(i);
					}
				}
			}
		}
		TestEqual(TEXT("提示词只在高级参数里的出图段不列"), Moved.GetSourceSegments().Num(), 3);
	}

	// 分解段带必填参数：生成页没地方让人填，不列（基础参数和高级参数里的都算）。
	{
		FDreamTextureWorkflowCatalog Required = Catalog;
		for (FDreamTextureWorkflowMeta& Meta : Required.Segments)
		{
			if (Meta.Id == TEXT("dec-derive-v1") && Meta.Params.Num() > 0)
			{
				Meta.Params[0].bRequired = true;
			}
		}
		const TArray<const FDreamTextureWorkflowMeta*> Decomposes = Required.GetDecomposeSegments();
		if (TestEqual(TEXT("带必填基础参数的分解段不列"), Decomposes.Num(), 1))
		{
			TestEqual(TEXT("剩下的是 CHORD"), Decomposes[0]->Id, FString(TEXT("dec-chord-v1")));
		}

		FDreamTextureWorkflowCatalog RequiredAdvanced = Catalog;
		for (FDreamTextureWorkflowMeta& Meta : RequiredAdvanced.Segments)
		{
			if (Meta.Id == TEXT("dec-chord-v1") && Meta.Advanced.Num() > 0)
			{
				Meta.Advanced[0].bRequired = true;
			}
		}
		TestEqual(TEXT("带必填高级参数的分解段也不列"), RequiredAdvanced.GetDecomposeSegments().Num(), 1);
	}

	// 旧后端的目录里没有 required 字段：解析成 false，所有段照旧都列。
	{
		FDreamTextureWorkflowCatalog Old;
		if (LoadCatalogFixture(*this, Old))
		{
			TestEqual(TEXT("旧目录：四个出图段"), Old.GetSourceSegments().Num(), 4);
			TestEqual(TEXT("旧目录：两个分解段"), Old.GetDecomposeSegments().Num(), 2);
			for (const FDreamTextureWorkflowMeta& Meta : Old.Segments)
			{
				for (const FDreamTextureParamSpec& Spec : Meta.Params)
				{
					TestFalse(*FString::Printf(TEXT("%s.%s 没有 required 字段，按非必填"), *Meta.Id, *Spec.Key), Spec.bRequired);
				}
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateCatalogNullTest, "DreamTexture.Client.Generate.Catalog.Null",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateCatalogNullTest::RunTest(const FString& Parameters)
{
	// Go 的 nil 切片序列化成 null 而不是 []。每个数组字段都要扛得住。
	{
		FDreamTextureWorkflowCatalog Catalog;
		TestTrue(TEXT("两个数组都是 null"), FDreamTextureWorkflowCatalog::FromJson(Parse(TEXT("{\"workflows\":null,\"segments\":null}")), Catalog));
		TestEqual(TEXT("没有工作流"), Catalog.Workflows.Num(), 0);
		TestEqual(TEXT("没有段"), Catalog.Segments.Num(), 0);
		TestEqual(TEXT("没有出图段"), Catalog.GetSourceSegments().Num(), 0);
	}
	{
		FDreamTextureWorkflowCatalog Catalog;
		TestTrue(TEXT("两个字段都不存在"), FDreamTextureWorkflowCatalog::FromJson(Parse(TEXT("{}")), Catalog));
		TestEqual(TEXT("没有工作流"), Catalog.Workflows.Num(), 0);
	}
	{
		FDreamTextureWorkflowCatalog Catalog;
		TestFalse(TEXT("根对象无效才算失败"), FDreamTextureWorkflowCatalog::FromJson(nullptr, Catalog));
	}

	// 一项里所有的数组、对象字段都是 null；以及夹着坏条目。
	{
		const FString Json = TEXT("{\"workflows\":["
			"{\"id\":\"w1\",\"name\":null,\"params\":null,\"advanced\":null,\"licenses\":null,\"license_notice\":null,"
			"\"expects_domain\":null,\"source\":null,\"model_requirements\":null,\"node_packs\":null},"
			"{\"name\":\"没有 id\"},"
			"42,null,"
			"{\"id\":\"w2\",\"params\":[null,{\"label\":\"没有 key\"},{\"key\":\"k\",\"type\":\"enum\",\"options\":null,\"default\":null,\"min\":null}]}"
			"],\"segments\":[{\"id\":\"s1\",\"segment\":\"source\"}]}");
		FDreamTextureWorkflowCatalog Catalog;
		TestTrue(TEXT("解析成功"), FDreamTextureWorkflowCatalog::FromJson(Parse(Json), Catalog));
		if (TestEqual(TEXT("没有 id 的和非对象的被丢掉，只剩两个"), Catalog.Workflows.Num(), 2))
		{
			const FDreamTextureWorkflowMeta& W1 = Catalog.Workflows[0];
			TestEqual(TEXT("w1"), W1.Id, FString(TEXT("w1")));
			TestTrue(TEXT("名字 null → 空串"), W1.Name.IsEmpty());
			TestEqual(TEXT("params null → 空"), W1.Params.Num(), 0);
			TestEqual(TEXT("licenses null → 没有许可，按可商用"), W1.Licenses.Num(), 0);
			TestTrue(TEXT("可商用"), W1.IsCommercial());
			TestFalse(TEXT("source null → 不是云端"), W1.bCloudSource);

			const FDreamTextureWorkflowMeta& W2 = Catalog.Workflows[1];
			if (TestEqual(TEXT("w2 只剩有 key 的那个参数"), W2.Params.Num(), 1))
			{
				TestFalse(TEXT("default null → 没有默认值"), W2.Params[0].bHasDefault);
				TestEqual(TEXT("options null → 没有候选"), W2.Params[0].Options.Num(), 0);
				TestFalse(TEXT("min null → 没有下限"), W2.Params[0].Min.IsSet());
			}
		}
		TestEqual(TEXT("一个段"), Catalog.Segments.Num(), 1);
	}

	// 单数的 license_notice 只作兜底：复数缺失时用它，不能把 research-only 丢了。
	{
		const FString Json = TEXT("{\"workflows\":[{\"id\":\"w\",\"license_notice\":{\"component\":\"X\",\"license\":\"research-only\",\"commercial\":false}}]}");
		FDreamTextureWorkflowCatalog Catalog;
		FDreamTextureWorkflowCatalog::FromJson(Parse(Json), Catalog);
		if (TestEqual(TEXT("一条"), Catalog.Workflows.Num(), 1))
		{
			TestFalse(TEXT("兜底后仍是不可商用"), Catalog.Workflows[0].IsCommercial());
		}
	}

	// commercial 字段缺失按不可商用：少发字段不能把研究用途说成可商用。
	{
		FDreamTextureLicenseNotice Notice;
		TestTrue(TEXT("解析"), FDreamTextureLicenseNotice::FromJson(Parse(TEXT("{\"component\":\"X\",\"license\":\"?\"}")), Notice));
		TestFalse(TEXT("缺失按不可商用"), Notice.bCommercial);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateImagenTest, "DreamTexture.Client.Generate.Imagen",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateImagenTest::RunTest(const FString& Parameters)
{
	// 声明里的控件提示：云端模型填到哪个参数里，看 widget，不认识具体参数名。
	{
		FDreamTextureWorkflowCatalog Catalog;
		if (!LoadCatalogFixture(*this, Catalog))
		{
			return false;
		}
		const FDreamTextureWorkflowMeta* Cloud = Catalog.FindWorkflow(TEXT("src-api-stylized-v1.dec-chord-v1"));
		const FDreamTextureWorkflowMeta* Local = Catalog.FindWorkflow(TEXT("src-local-realistic-v1.dec-chord-v1"));
		if (TestNotNull(TEXT("云端组合"), Cloud) && TestNotNull(TEXT("本地组合"), Local))
		{
			const FDreamTextureParamSpec* ModelParam = Cloud->FindParamByWidget(TEXT("imagen-model"));
			if (TestNotNull(TEXT("云端组合有云端模型参数"), ModelParam))
			{
				TestEqual(TEXT("参数名"), ModelParam->Key, FString(TEXT("api_model")));
				TestEqual(TEXT("控件提示"), ModelParam->Widget, FString(TEXT("imagen-model")));
			}
			TestTrue(TEXT("本地组合没有"), Local->FindParamByWidget(TEXT("imagen-model")) == nullptr);
			TestTrue(TEXT("空提示找不到任何参数"), Cloud->FindParamByWidget(FString()) == nullptr);

			const FDreamTextureParamSpec* Quality = Cloud->FindParam(TEXT("api_quality"));
			if (TestNotNull(TEXT("画质参数"), Quality))
			{
				TestEqual(TEXT("三档"), Quality->Options.Num(), 3);
				TestEqual(TEXT("默认 medium"), Quality->Default, FString(TEXT("medium")));
			}
		}
	}

	// 后端的真实应答：没填令牌时 models 是 []。
	{
		FDreamTextureImagenProviders Providers;
		TestTrue(TEXT("没填令牌的应答"), FDreamTextureImagenProviders::FromJson(
			Parse(TEXT("{\"providers\":[{\"id\":\"openai\",\"label\":\"OpenAI\",\"configured\":false,\"models\":[]}]}")), Providers));
		if (TestEqual(TEXT("一个来源"), Providers.Providers.Num(), 1))
		{
			TestFalse(TEXT("没填令牌"), Providers.Providers[0].bConfigured);
			TestEqual(TEXT("没有模型"), Providers.Providers[0].Models.Num(), 0);
			TestTrue(TEXT("按 id 找得到"), Providers.Find(TEXT("openai")) != nullptr);
			TestTrue(TEXT("找不到别的"), Providers.Find(TEXT("other")) == nullptr);
		}
	}

	// 有令牌：模型带说明和 known。夹着没有 id 的坏条目、null 的 note。
	{
		FDreamTextureImagenProviders Providers;
		const FString Json = TEXT("{\"providers\":[{\"id\":\"openai\",\"label\":\"OpenAI\",\"configured\":true,\"models\":["
			"{\"id\":\"gpt-image-2\",\"label\":\"GPT Image 2\",\"provider\":\"openai\",\"edits\":true,\"note\":\"最新\",\"known\":true,\"sizes\":null},"
			"{\"label\":\"没有 id\"},"
			"{\"id\":\"gpt-image-9\",\"note\":null,\"known\":false},null]}]}");
		TestTrue(TEXT("解析成功"), FDreamTextureImagenProviders::FromJson(Parse(Json), Providers));
		if (TestEqual(TEXT("一个来源"), Providers.Providers.Num(), 1) && TestEqual(TEXT("没有 id 的被丢掉，剩两个"), Providers.Providers[0].Models.Num(), 2))
		{
			const FDreamTextureImagenModel& First = Providers.Providers[0].Models[0];
			TestEqual(TEXT("id"), First.Id, FString(TEXT("gpt-image-2")));
			TestEqual(TEXT("说明"), First.Note, FString(TEXT("最新")));
			TestTrue(TEXT("认得的"), First.bKnown);
			const FDreamTextureImagenModel& Second = Providers.Providers[0].Models[1];
			TestFalse(TEXT("新发现的不是认得的"), Second.bKnown);
			TestTrue(TEXT("note null → 空串"), Second.Note.IsEmpty());
		}
	}

	// 有令牌但列不出来：error 带原因，models 可能是 null。
	{
		FDreamTextureImagenProviders Providers;
		TestTrue(TEXT("解析成功"), FDreamTextureImagenProviders::FromJson(
			Parse(TEXT("{\"providers\":[{\"id\":\"openai\",\"configured\":true,\"models\":null,\"error\":\"组织未完成身份验证\"}]}")), Providers));
		if (TestEqual(TEXT("一个来源"), Providers.Providers.Num(), 1))
		{
			TestEqual(TEXT("models null → 空"), Providers.Providers[0].Models.Num(), 0);
			TestEqual(TEXT("原因"), Providers.Providers[0].Error, FString(TEXT("组织未完成身份验证")));
		}
	}

	// providers 本身是 null / 缺失：没有来源，不是错误。
	{
		FDreamTextureImagenProviders Providers;
		TestTrue(TEXT("providers null"), FDreamTextureImagenProviders::FromJson(Parse(TEXT("{\"providers\":null}")), Providers));
		TestEqual(TEXT("没有来源"), Providers.Providers.Num(), 0);
		TestFalse(TEXT("根对象无效才算失败"), FDreamTextureImagenProviders::FromJson(nullptr, Providers));
	}

	return true;
}

// ---------------------------------------------------------------------------
// 请求体
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateRequestTest, "DreamTexture.Client.Generate.Request",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateRequestTest::RunTest(const FString& Parameters)
{
	FDreamTextureGenerateRequest Request;
	Request.WorkflowId = FDreamTextureWorkflowCatalog::MakeComposedId(TEXT("src-local-realistic-v1"), TEXT("dec-chord-v1"));
	Request.Variants = 4;
	Request.Name = TEXT("苔藓岩石");
	// 引号、换行、中文都要正确转义，原样回得来。
	Request.StringParams.Add(TEXT("prompt"), TEXT("mossy \"rocks\"\n苔藓和岩石"));
	Request.IntParams.Add(TEXT("resolution"), 1024);
	Request.SetSeed(DreamTextureGenerateLimits::RandomSeed);
	Request.FloatParams.Add(TEXT("tile_fix"), 0.45);
	Request.BoolParams.Add(TEXT("height_invert"), true);

	FString Json;
	FString Error;
	if (!TestTrue(TEXT("请求合法"), Request.ToJsonString(Json, Error)) || !TestTrue(TEXT("没有错误信息"), Error.IsEmpty()))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("请求体：%s"), *Json));

	// 整数不能写成 1024.0 / -1.0：后端 coerce 对 enum 按数值比，但这种写法在日志和 manifest 里难看，也容易被别的消费者当浮点。
	TestTrue(TEXT("整数写成整数"), Json.Contains(TEXT("\"resolution\":1024,")) || Json.Contains(TEXT("\"resolution\":1024}")));
	TestTrue(TEXT("随机种子写成 -1"), Json.Contains(TEXT("\"seed\":-1")));

	const TSharedPtr<FJsonObject> Root = Parse(Json);
	if (!TestTrue(TEXT("是合法 JSON"), Root.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("workflow_id 是组合 id"), Root->GetStringField(TEXT("workflow_id")), FString(TEXT("src-local-realistic-v1.dec-chord-v1")));
	TestEqual(TEXT("variants"), Root->GetIntegerField(TEXT("variants")), 4);
	TestEqual(TEXT("name"), Root->GetStringField(TEXT("name")), FString(TEXT("苔藓岩石")));

	const TSharedPtr<FJsonObject>* Params = nullptr;
	if (TestTrue(TEXT("有 params"), Root->TryGetObjectField(TEXT("params"), Params)))
	{
		TestEqual(TEXT("提示词转义后原样回得来"), (*Params)->GetStringField(TEXT("prompt")), FString(TEXT("mossy \"rocks\"\n苔藓和岩石")));
		TestEqual(TEXT("分辨率"), (*Params)->GetIntegerField(TEXT("resolution")), 1024);
		TestEqual(TEXT("种子"), static_cast<int64>((*Params)->GetNumberField(TEXT("seed"))), static_cast<int64>(-1));
		TestEqual(TEXT("浮点参数"), (*Params)->GetNumberField(TEXT("tile_fix")), 0.45);
		TestTrue(TEXT("布尔参数"), (*Params)->GetBoolField(TEXT("height_invert")));
	}

	// 展示名为空就不写：后端 `req.Name != ""` 才会用，空串和不写等价，但不发多余的字段。
	Request.Name.Reset();
	TestTrue(TEXT("仍然合法"), Request.ToJsonString(Json, Error));
	TestFalse(TEXT("没有名字时不写 name"), Parse(Json)->HasField(TEXT("name")));

	// 没设种子 = 随机；后端缺省也按随机处理。
	FDreamTextureGenerateRequest NoSeed;
	NoSeed.WorkflowId = TEXT("w");
	TestTrue(TEXT("没设种子算随机"), NoSeed.IsRandomSeed());
	NoSeed.SetSeed(7);
	TestFalse(TEXT("设了具体值就不是随机"), NoSeed.IsRandomSeed());
	NoSeed.SetSeed(DreamTextureGenerateLimits::RandomSeed);
	TestTrue(TEXT("-1 是随机"), NoSeed.IsRandomSeed());

	// 展示名：取提示词前 60 个字符，换行折成空格。
	TestEqual(TEXT("前后空白去掉，换行折成空格"),
		FDreamTextureGenerateRequest::MakeNameFromPrompt(TEXT("  line1\nline2\r\nline3\t ")), FString(TEXT("line1 line2 line3")));
	TestEqual(TEXT("正好 60 个不动"), FDreamTextureGenerateRequest::MakeNameFromPrompt(FString::ChrN(60, TEXT('a'))).Len(), 60);
	TestEqual(TEXT("超过 60 个截到 60"), FDreamTextureGenerateRequest::MakeNameFromPrompt(FString::ChrN(100, TEXT('a'))).Len(), 60);
	{
		// 第 60 个单元是代理对的前半个：不能留一个孤立的半个字符。
		FString Prompt = FString::ChrN(59, TEXT('a'));
		Prompt.AppendChar(static_cast<TCHAR>(0xD83E));
		Prompt.AppendChar(static_cast<TCHAR>(0xDEA8));
		Prompt += TEXT("bbb");
		const FString Name = FDreamTextureGenerateRequest::MakeNameFromPrompt(Prompt);
		TestEqual(TEXT("劈开代理对时舍掉前半个"), Name.Len(), 59);
	}
	{
		// 代理对完整地落在 60 以内：保留。
		FString Prompt = FString::ChrN(58, TEXT('a'));
		Prompt.AppendChar(static_cast<TCHAR>(0xD83E));
		Prompt.AppendChar(static_cast<TCHAR>(0xDEA8));
		Prompt += TEXT("bbb");
		TestEqual(TEXT("完整的代理对保留"), FDreamTextureGenerateRequest::MakeNameFromPrompt(Prompt).Len(), 60);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateSeedTest, "DreamTexture.Client.Generate.Seed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateSeedTest::RunTest(const FString& Parameters)
{
	using namespace DreamTextureGenerateLimits;

	auto Make = [](int64 Seed, int32 Variants)
	{
		FDreamTextureGenerateRequest Request;
		Request.WorkflowId = TEXT("w");
		Request.Variants = Variants;
		Request.SetSeed(Seed);
		return Request;
	};
	FString Error;

	TestEqual(TEXT("安全上界是 2^53-1"), MaxSafeSeed, static_cast<int64>(9007199254740991LL));

	// 随机与固定的合法范围。
	TestTrue(TEXT("-1 随机"), Make(RandomSeed, 4).Validate(Error));
	TestTrue(TEXT("0"), Make(0, 4).Validate(Error));
	TestTrue(TEXT("普通固定种子"), Make(123456789, 8).Validate(Error));

	// 负数（除 -1）拒绝。后端 resolveSeed 把任何 <0 当随机，用户填 -5 多半是想要某个确定的种子，静默变随机会骗他。
	TestFalse(TEXT("-2 拒绝"), Make(-2, 1).Validate(Error));
	TestTrue(TEXT("给出原因"), Error.Contains(TEXT("seed")));

	// 上限：变体 i 的种子是 Seed+i，所以上限随变体数缩小。
	TestEqual(TEXT("1 个变体的上限"), FDreamTextureGenerateRequest::MaxFixedSeed(1), MaxSafeSeed);
	TestEqual(TEXT("2 个变体的上限"), FDreamTextureGenerateRequest::MaxFixedSeed(2), MaxSafeSeed - 1);
	TestEqual(TEXT("8 个变体的上限"), FDreamTextureGenerateRequest::MaxFixedSeed(8), MaxSafeSeed - 7);
	TestEqual(TEXT("变体数越界时夹住"), FDreamTextureGenerateRequest::MaxFixedSeed(100), MaxSafeSeed - 7);
	TestEqual(TEXT("变体数为 0 时按 1"), FDreamTextureGenerateRequest::MaxFixedSeed(0), MaxSafeSeed);

	TestTrue(TEXT("1 个变体可以填满上限"), Make(MaxSafeSeed, 1).Validate(Error));
	TestFalse(TEXT("2 个变体填满上限，第二个变体的种子就越界"), Make(MaxSafeSeed, 2).Validate(Error));
	TestTrue(TEXT("2 个变体的最大值"), Make(MaxSafeSeed - 1, 2).Validate(Error));
	TestFalse(TEXT("8 个变体填 MaxSafeSeed-6 越界"), Make(MaxSafeSeed - 6, 8).Validate(Error));
	TestTrue(TEXT("8 个变体的最大值"), Make(MaxSafeSeed - 7, 8).Validate(Error));
	TestFalse(TEXT("2^53 拒绝"), Make(MaxSafeSeed + 1, 1).Validate(Error));
	TestFalse(TEXT("int64 最大值拒绝"), Make(MAX_int64, 1).Validate(Error));
	TestTrue(TEXT("原因里写出了上限"), Error.Contains(TEXT("maximum")));

	// 上限值要能无损写进 JSON：2^53-1 原样出现，不是 9.007199254740992e+15 之类。
	{
		FDreamTextureGenerateRequest Request = Make(MaxSafeSeed, 1);
		FString Json;
		TestTrue(TEXT("可序列化"), Request.ToJsonString(Json, Error));
		TestTrue(TEXT("2^53-1 原样写出"), Json.Contains(TEXT("\"seed\":9007199254740991")));
		const TSharedPtr<FJsonObject> Root = Parse(Json);
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (Root.IsValid() && Root->TryGetObjectField(TEXT("params"), Params))
		{
			int64 Back = 0;
			TestTrue(TEXT("读回来"), (*Params)->TryGetNumberField(TEXT("seed"), Back));
			TestEqual(TEXT("无损往返"), Back, MaxSafeSeed);
		}
	}

	// 非法的请求不会产出请求体。
	{
		FString Json = TEXT("untouched");
		TestFalse(TEXT("越界种子序列化失败"), Make(MaxSafeSeed + 1, 1).ToJsonString(Json, Error));
		TestTrue(TEXT("失败时不动输出"), Json == TEXT("untouched"));
	}

	// 其它整数参数也不能越过 JSON 的安全区间。
	{
		FDreamTextureGenerateRequest Request = Make(1, 1);
		Request.IntParams.Add(TEXT("big"), MaxSafeSeed + 1);
		TestFalse(TEXT("别的整数参数越界也拒绝"), Request.Validate(Error));
		TestTrue(TEXT("原因里点名了参数"), Error.Contains(TEXT("big")));
	}

	// 变体数 1~8。
	TestFalse(TEXT("0 个变体"), Make(1, 0).Validate(Error));
	TestFalse(TEXT("9 个变体"), Make(1, 9).Validate(Error));
	TestFalse(TEXT("负数个变体"), Make(1, -1).Validate(Error));
	TestTrue(TEXT("1 个变体"), Make(1, 1).Validate(Error));
	TestTrue(TEXT("8 个变体"), Make(1, 8).Validate(Error));

	// 没选工作流。
	{
		FDreamTextureGenerateRequest Request = Make(1, 1);
		Request.WorkflowId = TEXT("   ");
		TestFalse(TEXT("空白的工作流 id"), Request.Validate(Error));
	}

	return true;
}

// ---------------------------------------------------------------------------
// 任务与事件
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateJobTest, "DreamTexture.Client.Generate.Job.Parse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateJobTest::RunTest(const FString& Parameters)
{
	const FString Json = TEXT("{\"id\":\"job_20260827_092650_ab12\",\"material_id\":\"mat_20260827_092650_1f53\","
		"\"workflow_id\":\"src-local-realistic-v1.dec-chord-v1\",\"batch_id\":\"b7c9d1\","
		"\"params\":{\"prompt\":\"x\",\"seed\":4503599627370495,\"resolution\":1024,\"__name\":\"苔藓\"},"
		"\"status\":\"running\",\"prompt_id\":\"pid-1\",\"progress\":0.45,\"stage\":\"生成中\","
		"\"created_at\":\"2026-08-27T09:26:50.123+08:00\",\"started_at\":\"2026-08-27T09:26:51.000+08:00\"}");

	FDreamTextureJob Job;
	if (!TestTrue(TEXT("解析成功"), FDreamTextureJob::FromJson(Parse(Json), Job)))
	{
		return false;
	}
	TestEqual(TEXT("id"), Job.Id, FString(TEXT("job_20260827_092650_ab12")));
	TestEqual(TEXT("材质 id"), Job.MaterialId, FString(TEXT("mat_20260827_092650_1f53")));
	TestEqual(TEXT("批次"), Job.BatchId, FString(TEXT("b7c9d1")));
	TestTrue(TEXT("状态"), Job.Status == EDreamTextureJobStatus::Running);
	TestFalse(TEXT("进行中不是终态"), Job.IsTerminal());
	TestEqual(TEXT("阶段"), Job.Stage, FString(TEXT("生成中")));
	TestTrue(TEXT("进度"), FMath::IsNearlyEqual(Job.Progress, 0.45f));
	TestTrue(TEXT("读到了种子"), Job.bHasSeed);
	TestEqual(TEXT("种子（2^52 量级也精确）"), Job.Seed, static_cast<int64>(4503599627370495LL));
	TestEqual(TEXT("分辨率"), Job.Resolution, 1024);
	TestEqual(TEXT("展示名"), Job.Name, FString(TEXT("苔藓")));
	TestEqual(TEXT("创建时间（带时区偏移，换算成 UTC）"), Job.CreatedAt.GetHour(), 1);

	// 全 null / 全缺失：不炸，落到默认值。
	{
		const FString Sparse = TEXT("{\"id\":\"j\",\"params\":null,\"stage\":null,\"error\":null,\"started_at\":null,\"finished_at\":null,"
			"\"status\":\"queued\",\"progress\":null,\"batch_id\":null,\"prompt_id\":null}");
		FDreamTextureJob SparseJob;
		TestTrue(TEXT("稀疏的任务能解析"), FDreamTextureJob::FromJson(Parse(Sparse), SparseJob));
		TestTrue(TEXT("排队"), SparseJob.Status == EDreamTextureJobStatus::Queued);
		TestFalse(TEXT("没有种子"), SparseJob.bHasSeed);
		TestTrue(TEXT("阶段空"), SparseJob.Stage.IsEmpty());
		TestTrue(TEXT("进度 0"), SparseJob.Progress == 0.0f);
	}

	TestFalse(TEXT("没有 id 的不算任务"), FDreamTextureJob::FromJson(Parse(TEXT("{\"status\":\"queued\"}")), Job));
	TestFalse(TEXT("空对象"), FDreamTextureJob::FromJson(nullptr, Job));

	// 状态映射。
	TestTrue(TEXT("queued"), ParseJobStatus(TEXT("queued")) == EDreamTextureJobStatus::Queued);
	TestTrue(TEXT("running"), ParseJobStatus(TEXT("running")) == EDreamTextureJobStatus::Running);
	TestTrue(TEXT("succeeded"), ParseJobStatus(TEXT("succeeded")) == EDreamTextureJobStatus::Succeeded);
	TestTrue(TEXT("failed"), ParseJobStatus(TEXT("failed")) == EDreamTextureJobStatus::Failed);
	TestTrue(TEXT("canceled"), ParseJobStatus(TEXT("canceled")) == EDreamTextureJobStatus::Canceled);
	TestTrue(TEXT("不认识的"), ParseJobStatus(TEXT("weird")) == EDreamTextureJobStatus::Unknown);
	TestTrue(TEXT("空串"), ParseJobStatus(FString()) == EDreamTextureJobStatus::Unknown);
	TestEqual(TEXT("英文标识"), FString(LexToString(EDreamTextureJobStatus::Canceled)), FString(TEXT("canceled")));

	// 终态与先后。
	FDreamTextureJob Probe;
	const EDreamTextureJobStatus Terminal[] = { EDreamTextureJobStatus::Succeeded, EDreamTextureJobStatus::Failed, EDreamTextureJobStatus::Canceled };
	for (const EDreamTextureJobStatus Status : Terminal)
	{
		Probe.Status = Status;
		TestTrue(FString::Printf(TEXT("%s 是终态"), LexToString(Status)), Probe.IsTerminal());
		TestEqual(FString::Printf(TEXT("%s 的先后"), LexToString(Status)), Probe.GetRank(), 3);
	}
	Probe.Status = EDreamTextureJobStatus::Unknown;
	TestFalse(TEXT("不认识的状态不是终态：别把没见过的状态当成完成"), Probe.IsTerminal());
	TestEqual(TEXT("Unknown 排最前"), Probe.GetRank(), 0);

	// 进度夹在 0~1。
	{
		FDreamTextureJob Clamped;
		FDreamTextureJob::FromJson(Parse(TEXT("{\"id\":\"j\",\"progress\":1.7}")), Clamped);
		TestTrue(TEXT("上夹"), Clamped.Progress == 1.0f);
		FDreamTextureJob::FromJson(Parse(TEXT("{\"id\":\"j\",\"progress\":-0.2}")), Clamped);
		TestTrue(TEXT("下夹"), Clamped.Progress == 0.0f);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateJobStageTest, "DreamTexture.Client.Generate.Job.Stage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateJobStageTest::RunTest(const FString& Parameters)
{
	// 后端写死的六个阶段名都要换成英文源文（测试编辑器钉在 en）；认不得的原样显示；空串是空。
	TestEqual(TEXT("提交中"), FDreamTextureJob::LocalizeStage(TEXT("提交中")).ToString(), FString(TEXT("Submitting")));
	TestEqual(TEXT("等待 ComfyUI"), FDreamTextureJob::LocalizeStage(TEXT("等待 ComfyUI")).ToString(), FString(TEXT("Waiting for ComfyUI")));
	TestEqual(TEXT("生成中"), FDreamTextureJob::LocalizeStage(TEXT("生成中")).ToString(), FString(TEXT("Generating")));
	TestEqual(TEXT("云端出图"), FDreamTextureJob::LocalizeStage(TEXT("云端出图")).ToString(), FString(TEXT("Generating cloud image")));
	TestEqual(TEXT("落盘中"), FDreamTextureJob::LocalizeStage(TEXT("落盘中")).ToString(), FString(TEXT("Saving")));
	TestEqual(TEXT("完成"), FDreamTextureJob::LocalizeStage(TEXT("完成")).ToString(), FString(TEXT("Done")));
	TestEqual(TEXT("认不得的原样"), FDreamTextureJob::LocalizeStage(TEXT("分解中")).ToString(), FString(TEXT("分解中")));
	TestEqual(TEXT("差一个空格就不认"), FDreamTextureJob::LocalizeStage(TEXT("等待ComfyUI")).ToString(), FString(TEXT("等待ComfyUI")));
	TestTrue(TEXT("空串"), FDreamTextureJob::LocalizeStage(FString()).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateJobCancelReasonTest, "DreamTexture.Client.Generate.Job.CancelReason",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateJobCancelReasonTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("没有原因"), FDreamTextureJob::IsPlainCancelReason(FString()));
	TestTrue(TEXT("后端的通用原文"), FDreamTextureJob::IsPlainCancelReason(TEXT("已取消")));
	TestFalse(TEXT("别的原因要显示"), FDreamTextureJob::IsPlainCancelReason(TEXT("已取消：用户在 Web 端取消")));
	TestFalse(TEXT("英文原因要显示"), FDreamTextureJob::IsPlainCancelReason(TEXT("canceled by admin")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateJobEventTest, "DreamTexture.Client.Generate.Job.Event",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateJobEventTest::RunTest(const FString& Parameters)
{
	FDreamTextureJob Job;

	// 四种任务事件都能解。
	TestTrue(TEXT("job.queued"), FDreamTextureJob::FromEvent(MakeJobEvent(TEXT("job.queued"), TEXT("j1"), TEXT("queued")), Job));
	TestTrue(TEXT("排队"), Job.Status == EDreamTextureJobStatus::Queued);

	TestTrue(TEXT("job.progress"), FDreamTextureJob::FromEvent(MakeJobEvent(TEXT("job.progress"), TEXT("j1"), TEXT("running"), 0.5, TEXT("生成中")), Job));
	TestTrue(TEXT("进行中"), Job.Status == EDreamTextureJobStatus::Running);
	TestEqual(TEXT("阶段"), Job.Stage, FString(TEXT("生成中")));

	TestTrue(TEXT("job.done"), FDreamTextureJob::FromEvent(MakeJobEvent(TEXT("job.done"), TEXT("j1"), TEXT("succeeded"), 1.0, TEXT("完成")), Job));
	TestTrue(TEXT("完成"), Job.Status == EDreamTextureJobStatus::Succeeded);
	TestEqual(TEXT("材质 id"), Job.MaterialId, FString(TEXT("mat_1")));

	// job.failed 里的状态可能是 canceled：以任务自己说的为准，界面上「失败」和「已取消」是两种说法。
	TestTrue(TEXT("job.failed 带 canceled"), FDreamTextureJob::FromEvent(
		MakeJobEvent(TEXT("job.failed"), TEXT("j1"), TEXT("canceled"), 0.2, TEXT(""), TEXT("b1"), TEXT("mat_1"), TEXT("已取消")), Job));
	TestTrue(TEXT("保持 canceled"), Job.Status == EDreamTextureJobStatus::Canceled);
	TestEqual(TEXT("原因"), Job.Error, FString(TEXT("已取消")));

	TestTrue(TEXT("job.failed 带 failed"), FDreamTextureJob::FromEvent(
		MakeJobEvent(TEXT("job.failed"), TEXT("j1"), TEXT("failed"), 0.2, TEXT(""), TEXT("b1"), TEXT("mat_1"), TEXT("ComfyUI 报错")), Job));
	TestTrue(TEXT("失败"), Job.Status == EDreamTextureJobStatus::Failed);

	// 事件没带状态时按事件类型补。
	{
		FDreamTextureEvent NoStatus;
		NoStatus.Type = TEXT("job.done");
		NoStatus.Raw = Parse(TEXT("{\"type\":\"job.done\",\"job\":{\"id\":\"j2\"}}"));
		TestTrue(TEXT("没带状态"), FDreamTextureJob::FromEvent(NoStatus, Job));
		TestTrue(TEXT("按 job.done 补成完成"), Job.Status == EDreamTextureJobStatus::Succeeded);

		NoStatus.Type = TEXT("job.failed");
		TestTrue(TEXT("job.failed 没带状态"), FDreamTextureJob::FromEvent(NoStatus, Job));
		TestTrue(TEXT("按 job.failed 补成失败"), Job.Status == EDreamTextureJobStatus::Failed);
	}

	// 不是任务事件的、或没带 job 的、或 job 是 null 的，不解。
	{
		FDreamTextureEvent Other;
		Other.Type = TEXT("comfy.status");
		Other.Raw = Parse(TEXT("{\"type\":\"comfy.status\",\"data\":{\"alive\":true}}"));
		TestFalse(TEXT("comfy.status"), FDreamTextureJob::FromEvent(Other, Job));

		Other.Type = TEXT("job.progress");
		Other.Raw = Parse(TEXT("{\"type\":\"job.progress\"}"));
		TestFalse(TEXT("没带 job"), FDreamTextureJob::FromEvent(Other, Job));

		Other.Raw = Parse(TEXT("{\"type\":\"job.progress\",\"job\":null}"));
		TestFalse(TEXT("job 是 null"), FDreamTextureJob::FromEvent(Other, Job));

		Other.Raw = Parse(TEXT("{\"type\":\"job.progress\",\"job\":{\"status\":\"running\"}}"));
		TestFalse(TEXT("job 没有 id"), FDreamTextureJob::FromEvent(Other, Job));

		Other.Raw.Reset();
		TestFalse(TEXT("没有原始 JSON"), FDreamTextureJob::FromEvent(Other, Job));

		Other.Type = TEXT("model.download");
		Other.Raw = Parse(TEXT("{\"type\":\"model.download\",\"data\":{}}"));
		TestFalse(TEXT("model.download"), FDreamTextureJob::FromEvent(Other, Job));
	}

	return true;
}

// ---------------------------------------------------------------------------
// 扩写
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateRefineTest, "DreamTexture.Client.Generate.Refine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateRefineTest::RunTest(const FString& Parameters)
{
	{
		FDreamTextureRefineModels Models;
		TestTrue(TEXT("正常应答"), FDreamTextureRefineModels::FromJson(
			Parse(TEXT("{\"models\":[\"gpt-5-mini\",\"gpt-5\"],\"default\":\"gpt-5-mini\"}")), Models));
		TestEqual(TEXT("两个模型"), Models.Models.Num(), 2);
		TestEqual(TEXT("默认"), Models.Default, FString(TEXT("gpt-5-mini")));
		TestTrue(TEXT("没有错误"), Models.Error.IsEmpty());
	}
	{
		// 列不出来不算错：models 为空（甚至 null），error 说明原因。
		FDreamTextureRefineModels Models;
		TestTrue(TEXT("列不出来的应答"), FDreamTextureRefineModels::FromJson(
			Parse(TEXT("{\"models\":null,\"default\":\"gpt-5-mini\",\"error\":\"网关不提供 /models\"}")), Models));
		TestEqual(TEXT("没有模型"), Models.Models.Num(), 0);
		TestEqual(TEXT("默认仍在"), Models.Default, FString(TEXT("gpt-5-mini")));
		TestEqual(TEXT("原因"), Models.Error, FString(TEXT("网关不提供 /models")));
		TestFalse(TEXT("无效根对象"), FDreamTextureRefineModels::FromJson(nullptr, Models));
	}
	{
		FDreamTextureRefineResult Result;
		TestTrue(TEXT("正常结果"), FDreamTextureRefineResult::FromJson(
			Parse(TEXT("{\"prompt\":\"mossy rocks, top down\",\"model\":\"gpt-5-mini\",\"usage\":{\"input_tokens\":10},\"elapsed_ms\":1234}")), Result));
		TestEqual(TEXT("提示词"), Result.Prompt, FString(TEXT("mossy rocks, top down")));
		TestEqual(TEXT("模型"), Result.Model, FString(TEXT("gpt-5-mini")));
		TestEqual(TEXT("耗时"), Result.ElapsedMs, static_cast<int64>(1234));

		// 空结果不是结果：拿它去覆盖用户写的提示词就丢数据了。
		TestFalse(TEXT("空串"), FDreamTextureRefineResult::FromJson(Parse(TEXT("{\"prompt\":\"  \",\"model\":\"m\"}")), Result));
		TestFalse(TEXT("缺 prompt"), FDreamTextureRefineResult::FromJson(Parse(TEXT("{\"model\":\"m\"}")), Result));
		TestFalse(TEXT("prompt 是 null"), FDreamTextureRefineResult::FromJson(Parse(TEXT("{\"prompt\":null}")), Result));
	}
	return true;
}

// ---------------------------------------------------------------------------
// 追踪器
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureTrackerProgressTest, "DreamTexture.Client.Generate.Tracker.Progress",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureTrackerProgressTest::RunTest(const FString& Parameters)
{
	FFakeHooks Fake;
	const TSharedRef<FDreamTextureJobTracker> Tracker = MakeTracker(Fake);

	TArray<FDreamTextureJob> Seen;
	Tracker->OnJobChanged.AddLambda([&Seen](const FDreamTextureJob& Job) { Seen.Add(Job); });

	Tracker->Track(MakeJob(TEXT("j1"), EDreamTextureJobStatus::Queued));
	TestEqual(TEXT("登记时广播一次"), Seen.Num(), 1);
	TestEqual(TEXT("跟踪着一个"), Tracker->GetNumTracked(), 1);
	TestEqual(TEXT("来源标签默认是生成页"), Tracker->GetOrigin(TEXT("j1")), FString(TEXT("generate")));

	Tracker->HandleEvent(MakeJobEvent(TEXT("job.progress"), TEXT("j1"), TEXT("running"), 0.3, TEXT("生成中")));
	const FDreamTextureJob* Job = Tracker->Find(TEXT("j1"));
	if (!TestNotNull(TEXT("找得到"), Job))
	{
		return false;
	}
	TestTrue(TEXT("进入运行"), Job->Status == EDreamTextureJobStatus::Running);
	TestTrue(TEXT("进度 0.3"), FMath::IsNearlyEqual(Job->Progress, 0.3f));
	TestEqual(TEXT("阶段"), Job->Stage, FString(TEXT("生成中")));
	TestEqual(TEXT("广播两次"), Seen.Num(), 2);

	// 同一状态内进度不倒退，也不重复广播（没有任何字段变化）。
	Tracker->HandleEvent(MakeJobEvent(TEXT("job.progress"), TEXT("j1"), TEXT("running"), 0.2, TEXT("生成中")));
	TestTrue(TEXT("进度没有倒退"), FMath::IsNearlyEqual(Tracker->Find(TEXT("j1"))->Progress, 0.3f));
	TestEqual(TEXT("没变化不广播"), Seen.Num(), 2);

	Tracker->HandleEvent(MakeJobEvent(TEXT("job.progress"), TEXT("j1"), TEXT("running"), 0.95, TEXT("落盘中")));
	TestEqual(TEXT("阶段推进"), Tracker->Find(TEXT("j1"))->Stage, FString(TEXT("落盘中")));
	TestEqual(TEXT("广播三次"), Seen.Num(), 3);

	Tracker->HandleEvent(MakeJobEvent(TEXT("job.done"), TEXT("j1"), TEXT("succeeded"), 1.0, TEXT("完成")));
	TestTrue(TEXT("完成"), Tracker->Find(TEXT("j1"))->Status == EDreamTextureJobStatus::Succeeded);
	TestTrue(TEXT("完成时进度是 1"), Tracker->Find(TEXT("j1"))->Progress == 1.0f);
	TestEqual(TEXT("材质 id 在"), Tracker->Find(TEXT("j1"))->MaterialId, FString(TEXT("mat_1")));
	TestEqual(TEXT("完成广播一次"), Seen.Num(), 4);
	TestEqual(TEXT("没有进行中的任务了"), Tracker->GetActiveJobs().Num(), 0);

	// 终态落定后，迟到的进度事件不能把它拉回去。
	Tracker->HandleEvent(MakeJobEvent(TEXT("job.progress"), TEXT("j1"), TEXT("running"), 0.5, TEXT("生成中")));
	TestTrue(TEXT("仍是完成"), Tracker->Find(TEXT("j1"))->Status == EDreamTextureJobStatus::Succeeded);
	TestEqual(TEXT("不再广播"), Seen.Num(), 4);

	// 终态之间也不互相覆盖：先到的为准。
	Tracker->HandleEvent(MakeJobEvent(TEXT("job.failed"), TEXT("j1"), TEXT("failed"), 0.5, TEXT(""), TEXT("b1"), TEXT("mat_1"), TEXT("x")));
	TestTrue(TEXT("完成不被失败覆盖"), Tracker->Find(TEXT("j1"))->Status == EDreamTextureJobStatus::Succeeded);

	// 没登记过的任务（别的客户端提交的）不进追踪表。
	Tracker->HandleEvent(MakeJobEvent(TEXT("job.progress"), TEXT("other"), TEXT("running"), 0.5));
	TestEqual(TEXT("陌生任务不被追踪"), Tracker->GetNumTracked(), 1);
	TestTrue(TEXT("查不到"), Tracker->Find(TEXT("other")) == nullptr);

	// 不是任务事件的忽略。
	FDreamTextureEvent Hb;
	Hb.Type = TEXT("comfy.status");
	Hb.Raw = Parse(TEXT("{\"type\":\"comfy.status\",\"data\":{}}"));
	Tracker->HandleEvent(Hb);
	TestEqual(TEXT("无关事件不广播"), Seen.Num(), 4);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureTrackerOrderTest, "DreamTexture.Client.Generate.Tracker.Order",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureTrackerOrderTest::RunTest(const FString& Parameters)
{
	FFakeHooks Fake;
	const TSharedRef<FDreamTextureJobTracker> Tracker = MakeTracker(Fake);

	// 事件先于提交的应答到达：暂存，登记时并进去，订阅方只看到最终状态那一次。
	Tracker->HandleEvent(MakeJobEvent(TEXT("job.progress"), TEXT("early"), TEXT("running"), 0.4, TEXT("生成中")));
	TestEqual(TEXT("暂存的不算追踪"), Tracker->GetNumTracked(), 0);

	TArray<FDreamTextureJob> Seen;
	Tracker->OnJobChanged.AddLambda([&Seen](const FDreamTextureJob& Job) { Seen.Add(Job); });

	// 提交应答里它还是 queued。
	Tracker->Track(MakeJob(TEXT("early"), EDreamTextureJobStatus::Queued));
	if (TestEqual(TEXT("只广播一次"), Seen.Num(), 1))
	{
		TestTrue(TEXT("广播的就是并入后的状态"), Seen[0].Status == EDreamTextureJobStatus::Running);
	}
	const FDreamTextureJob* Early = Tracker->Find(TEXT("early"));
	if (TestNotNull(TEXT("已登记"), Early))
	{
		TestTrue(TEXT("没有被 queued 拉回去"), Early->Status == EDreamTextureJobStatus::Running);
		TestTrue(TEXT("进度也并进来了"), FMath::IsNearlyEqual(Early->Progress, 0.4f));
		TestEqual(TEXT("阶段"), Early->Stage, FString(TEXT("生成中")));
	}

	// 暂存区也只往前走，且已用掉的不会重复并入。
	Tracker->HandleEvent(MakeJobEvent(TEXT("job.done"), TEXT("early2"), TEXT("succeeded"), 1.0));
	Tracker->HandleEvent(MakeJobEvent(TEXT("job.progress"), TEXT("early2"), TEXT("running"), 0.5));
	Tracker->Track(MakeJob(TEXT("early2"), EDreamTextureJobStatus::Queued));
	TestTrue(TEXT("暂存区里终态不被迟到的进度盖掉"), Tracker->Find(TEXT("early2"))->Status == EDreamTextureJobStatus::Succeeded);

	// 登记之后收到更旧的状态（排队事件迟到、过期的 REST 应答）一律忽略。
	Tracker->Track(MakeJob(TEXT("late"), EDreamTextureJobStatus::Running, 0.6f));
	Tracker->HandleEvent(MakeJobEvent(TEXT("job.queued"), TEXT("late"), TEXT("queued")));
	TestTrue(TEXT("排队事件迟到被忽略"), Tracker->Find(TEXT("late"))->Status == EDreamTextureJobStatus::Running);
	Tracker->Track(MakeJob(TEXT("late"), EDreamTextureJobStatus::Queued));
	TestTrue(TEXT("重复登记不倒退"), Tracker->Find(TEXT("late"))->Status == EDreamTextureJobStatus::Running);

	// 暂存区封顶。
	for (int32 i = 0; i < 200; ++i)
	{
		Tracker->HandleEvent(MakeJobEvent(TEXT("job.progress"), FString::Printf(TEXT("stranger_%d"), i), TEXT("running"), 0.5));
	}
	TestEqual(TEXT("陌生任务不进追踪表"), Tracker->GetNumTracked(), 3);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureTrackerReconcileTest, "DreamTexture.Client.Generate.Tracker.Reconcile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureTrackerReconcileTest::RunTest(const FString& Parameters)
{
	FFakeHooks Fake;
	const TSharedRef<FDreamTextureJobTracker> Tracker = MakeTracker(Fake);

	Tracker->Track(MakeJob(TEXT("a"), EDreamTextureJobStatus::Running, 0.4f));
	Tracker->Track(MakeJob(TEXT("b"), EDreamTextureJobStatus::Queued));
	Tracker->Track(MakeJob(TEXT("c"), EDreamTextureJobStatus::Succeeded, 1.0f));

	// 非 Live 的链路变化不补。
	Tracker->HandleLinkChanged(EDreamTextureLink::Connecting);
	Tracker->HandleLinkChanged(EDreamTextureLink::Stale);
	Tracker->HandleLinkChanged(EDreamTextureLink::Offline);
	TestEqual(TEXT("断线期间不问"), Fake.Gets.Num(), 0);

	// 重新连上：只问没结束的。
	Tracker->HandleLinkChanged(EDreamTextureLink::Live);
	if (!TestEqual(TEXT("问了两个没结束的"), Fake.Gets.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("先问 a"), Fake.Gets[0].Id, FString(TEXT("a")));
	TestEqual(TEXT("再问 b"), Fake.Gets[1].Id, FString(TEXT("b")));

	// 在途时不重复问。
	Tracker->Reconcile();
	TestEqual(TEXT("在途的不重复问"), Fake.Gets.Num(), 2);

	// 断线期间 a 已经跑完了：补上之后变成功。
	FDreamTextureJob Done = MakeJob(TEXT("a"), EDreamTextureJobStatus::Succeeded, 1.0f);
	Done.Stage = TEXT("完成");
	Fake.CompleteGet(0, Done);
	TestTrue(TEXT("a 补成了完成"), Tracker->Find(TEXT("a"))->Status == EDreamTextureJobStatus::Succeeded);
	TestEqual(TEXT("材质 id 在"), Tracker->Find(TEXT("a"))->MaterialId, FString(TEXT("mat_a")));

	// 连不上之类的失败：不改任何东西，任务大概率还在后端跑着。
	Fake.FailGet(1, /*bNotFound*/ false);
	TestTrue(TEXT("b 保持排队"), Tracker->Find(TEXT("b"))->Status == EDreamTextureJobStatus::Queued);
	TestTrue(TEXT("b 还算进行中"), !Tracker->Find(TEXT("b"))->IsTerminal());

	// 下一次变 Live 再问（上一次的在途标记已经清掉了）。
	Tracker->HandleLinkChanged(EDreamTextureLink::Live);
	if (!TestEqual(TEXT("只剩 b 要问"), Fake.Gets.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("问的是 b"), Fake.Gets[2].Id, FString(TEXT("b")));

	// 后端明确说没有这个任务：它再也不会有结果了，判失败并写清楚原因。
	Fake.FailGet(2, /*bNotFound*/ true);
	TestTrue(TEXT("b 判失败"), Tracker->Find(TEXT("b"))->Status == EDreamTextureJobStatus::Failed);
	TestFalse(TEXT("有中文原因"), Tracker->Find(TEXT("b"))->Error.IsEmpty());
	TestEqual(TEXT("没有进行中的了"), Tracker->GetActiveJobs().Num(), 0);

	// 补状态的应答比事件旧：不能倒退。
	Tracker->Track(MakeJob(TEXT("d"), EDreamTextureJobStatus::Running, 0.2f));
	Tracker->Reconcile();
	const int32 DIndex = Fake.Gets.Num() - 1;
	Tracker->HandleEvent(MakeJobEvent(TEXT("job.done"), TEXT("d"), TEXT("succeeded"), 1.0));
	Fake.CompleteGet(DIndex, MakeJob(TEXT("d"), EDreamTextureJobStatus::Running, 0.2f));
	TestTrue(TEXT("迟到的旧应答不覆盖已完成"), Tracker->Find(TEXT("d"))->Status == EDreamTextureJobStatus::Succeeded);

	// 追踪器先于应答销毁：回调到达不崩。
	Tracker->Track(MakeJob(TEXT("e"), EDreamTextureJobStatus::Running));
	Tracker->Reconcile();
	const int32 EIndex = Fake.Gets.Num() - 1;
	{
		FFakeHooks Local;
		TSharedPtr<FDreamTextureJobTracker> Short = MakeTracker(Local);
		Short->Track(MakeJob(TEXT("z"), EDreamTextureJobStatus::Running));
		Short->Reconcile();
		Short.Reset();
		Local.CompleteGet(0, MakeJob(TEXT("z"), EDreamTextureJobStatus::Succeeded, 1.0f));
	}
	Fake.CompleteGet(EIndex, MakeJob(TEXT("e"), EDreamTextureJobStatus::Succeeded, 1.0f));
	TestTrue(TEXT("追踪器销毁后回调到达没有崩溃"), true);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureTrackerSubmitTest, "DreamTexture.Client.Generate.Tracker.Submit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureTrackerSubmitTest::RunTest(const FString& Parameters)
{
	FFakeHooks Fake;
	const TSharedRef<FDreamTextureJobTracker> Tracker = MakeTracker(Fake);

	FDreamTextureGenerateRequest Request;
	Request.WorkflowId = TEXT("w");
	Request.Variants = 3;
	Request.StringParams.Add(TEXT("prompt"), TEXT("p"));

	// 成功：本批全部登记，顺序是变体顺序。
	bool bDone = false;
	bool bOK = false;
	TArray<FDreamTextureJob> Got;
	Tracker->Submit(Request, [&](bool bInOK, TArray<FDreamTextureJob>&& Jobs, const FString&)
	{
		bDone = true;
		bOK = bInOK;
		Got = MoveTemp(Jobs);
	});
	TestEqual(TEXT("请求发给了网络层"), Fake.SubmitRequests.Num(), 1);
	TestEqual(TEXT("请求原样传下去"), Fake.SubmitRequests[0].Variants, 3);
	TestFalse(TEXT("应答前还没回调"), bDone);

	TArray<FDreamTextureJob> Created;
	Created.Add(MakeJob(TEXT("j1"), EDreamTextureJobStatus::Queued, 0.0f, TEXT("batchA")));
	Created.Add(MakeJob(TEXT("j2"), EDreamTextureJobStatus::Queued, 0.0f, TEXT("batchA")));
	Created.Add(MakeJob(TEXT("j3"), EDreamTextureJobStatus::Queued, 0.0f, TEXT("batchA")));
	Fake.SubmitDones[0](true, MoveTemp(Created), FString());

	TestTrue(TEXT("回调了"), bDone && bOK);
	TestEqual(TEXT("回调里是三个任务"), Got.Num(), 3);
	TestEqual(TEXT("三个都登记了"), Tracker->GetNumTracked(), 3);
	const TArray<FDreamTextureJob> Batch = Tracker->GetBatch(TEXT("batchA"));
	if (TestEqual(TEXT("批里三个"), Batch.Num(), 3))
	{
		TestEqual(TEXT("按变体顺序"), Batch[0].Id, FString(TEXT("j1")));
		TestEqual(TEXT("按变体顺序"), Batch[2].Id, FString(TEXT("j3")));
	}
	TestEqual(TEXT("最近一批"), Tracker->GetLatestBatchId(TEXT("generate")), FString(TEXT("batchA")));
	TestEqual(TEXT("三个都没结束"), Tracker->GetActiveJobs().Num(), 3);

	// 别的提交方用自己的标签：不影响生成页的「最近一批」。
	Tracker->Submit(Request, [](bool, TArray<FDreamTextureJob>&&, const FString&) {}, TEXT("reflow"));
	TArray<FDreamTextureJob> Other;
	Other.Add(MakeJob(TEXT("r1"), EDreamTextureJobStatus::Queued, 0.0f, TEXT("batchR")));
	Fake.SubmitDones[1](true, MoveTemp(Other), FString());
	TestEqual(TEXT("回流的批"), Tracker->GetLatestBatchId(TEXT("reflow")), FString(TEXT("batchR")));
	TestEqual(TEXT("生成页的最近一批不变"), Tracker->GetLatestBatchId(TEXT("generate")), FString(TEXT("batchA")));
	TestEqual(TEXT("标签记下了"), Tracker->GetOrigin(TEXT("r1")), FString(TEXT("reflow")));
	TestEqual(TEXT("没有的来源没有批"), Tracker->GetLatestBatchId(TEXT("nobody")), FString());

	// 失败：什么都不登记，原因原样交回。
	FString FailError;
	bool bFailOK = true;
	Tracker->Submit(Request, [&](bool bInOK, TArray<FDreamTextureJob>&&, const FString& Error)
	{
		bFailOK = bInOK;
		FailError = Error;
	});
	Fake.SubmitDones[2](false, TArray<FDreamTextureJob>(), TEXT("提交失败：任务队列已满"));
	TestFalse(TEXT("失败"), bFailOK);
	TestEqual(TEXT("原因原样交回"), FailError, FString(TEXT("提交失败：任务队列已满")));
	TestEqual(TEXT("没有多登记"), Tracker->GetNumTracked(), 4);

	// 追踪器先于应答销毁：Done 仍然会被调用（调用方在等），但不登记。
	{
		FFakeHooks Local;
		TSharedPtr<FDreamTextureJobTracker> Short = MakeTracker(Local);
		bool bLocalDone = false;
		Short->Submit(Request, [&](bool, TArray<FDreamTextureJob>&&, const FString&) { bLocalDone = true; });
		Short.Reset();
		TArray<FDreamTextureJob> Orphans;
		Orphans.Add(MakeJob(TEXT("o1"), EDreamTextureJobStatus::Queued));
		Local.SubmitDones[0](true, MoveTemp(Orphans), FString());
		TestTrue(TEXT("追踪器没了回调仍到达"), bLocalDone);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureTrackerCancelTest, "DreamTexture.Client.Generate.Tracker.Cancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureTrackerCancelTest::RunTest(const FString& Parameters)
{
	FFakeHooks Fake;
	const TSharedRef<FDreamTextureJobTracker> Tracker = MakeTracker(Fake);

	Tracker->Track(MakeJob(TEXT("q"), EDreamTextureJobStatus::Queued));
	Tracker->Track(MakeJob(TEXT("r"), EDreamTextureJobStatus::Running, 0.3f));
	TestFalse(TEXT("还没请求取消"), Tracker->IsCancelRequested(TEXT("q")));

	// 排队中的：请求成功后问一次，立刻拿到 canceled，不必干等事件。
	bool bCanceled = false;
	Tracker->Cancel(TEXT("q"), [&](bool bOK, const FString&) { bCanceled = bOK; });
	TestEqual(TEXT("发出了取消请求"), Fake.CancelIds.Num(), 1);
	TestTrue(TEXT("应答前就记下了「已请求取消」，界面可以显示取消中"), Tracker->IsCancelRequested(TEXT("q")));
	TestEqual(TEXT("应答前还没补状态"), Fake.Gets.Num(), 0);

	Fake.CancelDones[0](true, FString());
	TestTrue(TEXT("Done 回调了"), bCanceled);
	if (TestEqual(TEXT("请求成功后补一次状态"), Fake.Gets.Num(), 1))
	{
		FDreamTextureJob Canceled = MakeJob(TEXT("q"), EDreamTextureJobStatus::Canceled);
		Canceled.Error = TEXT("已取消");
		Fake.CompleteGet(0, Canceled);
		TestTrue(TEXT("补到了 canceled"), Tracker->Find(TEXT("q"))->Status == EDreamTextureJobStatus::Canceled);
		TestEqual(TEXT("原因"), Tracker->Find(TEXT("q"))->Error, FString(TEXT("已取消")));
	}

	// 正在跑的：取消请求成功，但状态要等 ComfyUI 被打断才变。期间仍是 running，但记着取消请求。
	Tracker->Cancel(TEXT("r"));
	Fake.CancelDones[1](true, FString());
	TestTrue(TEXT("仍在取消中"), Tracker->IsCancelRequested(TEXT("r")));
	Fake.CompleteGet(1, MakeJob(TEXT("r"), EDreamTextureJobStatus::Running, 0.3f));
	TestTrue(TEXT("后端还没打断完，仍是进行中"), Tracker->Find(TEXT("r"))->Status == EDreamTextureJobStatus::Running);
	// 随后事件到达。
	Tracker->HandleEvent(MakeJobEvent(TEXT("job.failed"), TEXT("r"), TEXT("canceled"), 0.3, TEXT(""), TEXT("b1"), TEXT("mat_r"), TEXT("已取消")));
	TestTrue(TEXT("事件把它推到 canceled"), Tracker->Find(TEXT("r"))->Status == EDreamTextureJobStatus::Canceled);

	// 取消失败（比如任务刚好结束了，后端回 400）：清掉「已请求取消」，原因交回。
	Tracker->Track(MakeJob(TEXT("s"), EDreamTextureJobStatus::Running));
	FString CancelError;
	Tracker->Cancel(TEXT("s"), [&](bool, const FString& Error) { CancelError = Error; });
	TestTrue(TEXT("先记下"), Tracker->IsCancelRequested(TEXT("s")));
	Fake.CancelDones[2](false, TEXT("取消失败：任务已结束"));
	TestFalse(TEXT("失败后清掉"), Tracker->IsCancelRequested(TEXT("s")));
	TestEqual(TEXT("原因交回"), CancelError, FString(TEXT("取消失败：任务已结束")));

	// 取消失败也要重查一次状态：任务其实已经结束、终态事件丢了，不重查的话这一行会永远停在「生成中」。
	if (TestEqual(TEXT("取消失败后补问了一次状态"), Fake.Gets.Num(), 3))
	{
		TestEqual(TEXT("问的是这个任务"), Fake.Gets[2].Id, FString(TEXT("s")));
		TestTrue(TEXT("补问之前仍是旧状态"), Tracker->Find(TEXT("s"))->Status == EDreamTextureJobStatus::Running);
		Fake.CompleteGet(2, MakeJob(TEXT("s"), EDreamTextureJobStatus::Succeeded, 1.0f));
		TestTrue(TEXT("补到了真实的终态"), Tracker->Find(TEXT("s"))->Status == EDreamTextureJobStatus::Succeeded);
		TestTrue(TEXT("已经是终态"), Tracker->Find(TEXT("s"))->IsTerminal());
	}

	// 取消失败是因为连不上后端：补问也失败，什么都不改（任务大概率还在跑，下次重连再补）。
	{
		Tracker->Track(MakeJob(TEXT("t"), EDreamTextureJobStatus::Running, 0.2f));
		Tracker->Cancel(TEXT("t"));
		Fake.CancelDones[3](false, TEXT("取消失败：连不上"));
		const int32 Last = Fake.Gets.Num() - 1;
		TestEqual(TEXT("补问的是 t"), Fake.Gets[Last].Id, FString(TEXT("t")));
		Fake.FailGet(Last, /*bNotFound*/ false);
		TestTrue(TEXT("网络失败不改状态"), Tracker->Find(TEXT("t"))->Status == EDreamTextureJobStatus::Running);
		TestFalse(TEXT("也不再是取消中"), Tracker->IsCancelRequested(TEXT("t")));
	}

	return true;
}

namespace
{
	void EnqueueCmd(const TSharedPtr<IAutomationLatentCommand>& Command)
	{
		FAutomationTestFramework::Get().EnqueueLatentCommand(Command);
	}

	void GenEnqueueDo(TFunction<void()> Action)
	{
		EnqueueCmd(MakeShareable(new FDreamTextureDoCommand(MoveTemp(Action))));
	}

	void GenEnqueueWait(FAutomationTestBase* Test, const FString& What, double TimeoutSecs, TFunction<bool()> IsDone, TFunction<void()> OnDone = nullptr)
	{
		EnqueueCmd(MakeShareable(new FDreamTextureWaitCommand(Test, What, TimeoutSecs, MoveTemp(IsDone), MoveTemp(OnDone))));
	}

	/** 什么都不等地观察一段时间，然后调 OnDone。用来证明「这段时间里没有发生某件事」。 */
	void EnqueueWindow(FAutomationTestBase* Test, double Secs, TFunction<void()> OnDone)
	{
		const TSharedRef<double> Start = MakeShared<double>(0.0);
		GenEnqueueWait(Test, TEXT("观察窗口"), Secs + 5.0,
			[Start, Secs]()
			{
				if (*Start == 0.0)
				{
					*Start = FPlatformTime::Seconds();
				}
				return FPlatformTime::Seconds() - *Start > Secs;
			},
			MoveTemp(OnDone));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureTrackerPeriodicTest, "DreamTexture.Client.Generate.Tracker.Periodic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureTrackerPeriodicTest::RunTest(const FString& Parameters)
{
	// 事件流会静默丢事件（后端给每个订阅者 64 格缓冲，满了就丢）。终态一丢，任务就永远停在「生成中」，
	// 所以有活动任务时追踪器自己每隔一阵子 Reconcile 一次，没有活动任务时不跑。
	// 间隔压到 0.2 秒好在测试里等得起；真实默认是 30 秒。
	struct FState
	{
		FFakeHooks Fake;
		TSharedPtr<FDreamTextureJobTracker> Tracker;
		int32 Mark = 0;

		FFakeHooks Fake2;
		TSharedPtr<FDreamTextureJobTracker> Tracker2;
		TSharedPtr<FDreamTextureConnection> Conn;
	};
	const TSharedRef<FState> S = MakeShared<FState>();
	FAutomationTestBase* Test = this;

	TestEqual(TEXT("真实默认间隔是 30 秒"), FDreamTextureJobTracker::DefaultReconcileIntervalSecs, 30.0f);

	// 1. 有活动任务：定时来问；上一次的问还没回时不重复问。
	GenEnqueueDo([S]()
	{
		S->Tracker = MakeTracker(S->Fake);
		S->Tracker->SetReconcileInterval(0.2f);
		S->Tracker->Track(MakeJob(TEXT("p1"), EDreamTextureJobStatus::Running, 0.1f));
	});
	GenEnqueueWait(Test, TEXT("定时补状态：第一次问"), 5.0,
		[S]() { return S->Fake.Gets.Num() >= 1; },
		[S, Test]()
		{
			Test->TestEqual(TEXT("问的是 p1"), S->Fake.Gets[0].Id, FString(TEXT("p1")));
			S->Mark = S->Fake.Gets.Num();
		});
	EnqueueWindow(Test, 0.8, [S, Test]()
	{
		Test->TestEqual(TEXT("上一次的问还没回，不重复问"), S->Fake.Gets.Num(), S->Mark);
		// 应答：仍在运行（进度前进了）。
		S->Fake.CompleteGet(0, MakeJob(TEXT("p1"), EDreamTextureJobStatus::Running, 0.3f));
	});
	GenEnqueueWait(Test, TEXT("仍在运行：下一轮又问"), 5.0,
		[S]() { return S->Fake.Gets.Num() >= 2; },
		[S, Test]()
		{
			Test->TestEqual(TEXT("问的还是 p1"), S->Fake.Gets[1].Id, FString(TEXT("p1")));
			// 这一次的应答是终态：断线期间丢掉的 job.done 就这样被补回来。
			S->Fake.CompleteGet(1, MakeJob(TEXT("p1"), EDreamTextureJobStatus::Succeeded, 1.0f));
			Test->TestTrue(TEXT("丢了终态事件的任务被校正成完成"), S->Tracker->Find(TEXT("p1"))->Status == EDreamTextureJobStatus::Succeeded);
			S->Mark = S->Fake.Gets.Num();
		});

	// 2. 没有活动任务了：不跑。
	EnqueueWindow(Test, 1.2, [S, Test]()
	{
		Test->TestEqual(TEXT("没有活动任务时一个请求都不发"), S->Fake.Gets.Num(), S->Mark);
	});

	// 3. 又来了新任务：定时器重新开。
	GenEnqueueDo([S]()
	{
		S->Tracker->Track(MakeJob(TEXT("p2"), EDreamTextureJobStatus::Queued));
	});
	GenEnqueueWait(Test, TEXT("新任务登记后定时器重新开：问了 p2"), 5.0,
		[S]() { return S->Fake.Gets.Num() > S->Mark; },
		[S, Test]()
		{
			Test->TestEqual(TEXT("问的是 p2"), S->Fake.Gets.Last().Id, FString(TEXT("p2")));
			S->Fake.CompleteGet(S->Fake.Gets.Num() - 1, MakeJob(TEXT("p2"), EDreamTextureJobStatus::Failed));
		});

	// 4. 后端已知不通：不发请求（重连变 Live 时 HandleLinkChanged 会补）。
	GenEnqueueDo([S]()
	{
		// 连接没 Start()：IsBackendReachable() 为假，也不会有任何网络请求。
		S->Conn = MakeShared<FDreamTextureConnection>();
		S->Tracker2 = MakeTracker(S->Fake2);
		S->Tracker2->SetReconcileInterval(0.2f);
		S->Tracker2->Bind(S->Conn);
		S->Tracker2->Track(MakeJob(TEXT("q1"), EDreamTextureJobStatus::Running, 0.1f));
	});
	EnqueueWindow(Test, 1.0, [S, Test]()
	{
		Test->TestEqual(TEXT("后端已知不通时不发请求"), S->Fake2.Gets.Num(), 0);
	});

	// 5. 追踪器带着活动任务被销毁：定时器要一起摘掉（它的代码在本 DLL 里，模块卸载后再被叫到就是崩溃）。
	GenEnqueueDo([S]()
	{
		S->Tracker2.Reset();

		S->Tracker->Track(MakeJob(TEXT("p3"), EDreamTextureJobStatus::Running, 0.1f));
		S->Tracker.Reset();
	});
	EnqueueWindow(Test, 0.8, [Test]()
	{
		Test->TestTrue(TEXT("追踪器销毁后定时器没有再来（没有崩溃）"), true);
	});

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureTrackerTrimTest, "DreamTexture.Client.Generate.Tracker.Trim",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureTrackerTrimTest::RunTest(const FString& Parameters)
{
	FFakeHooks Fake;
	const TSharedRef<FDreamTextureJobTracker> Tracker = MakeTracker(Fake);

	// 先登记几个进行中的，再灌一堆已结束的：封顶时丢的是已结束的，进行中的一个都不能少。
	for (int32 i = 0; i < 5; ++i)
	{
		Tracker->Track(MakeJob(FString::Printf(TEXT("run_%d"), i), EDreamTextureJobStatus::Running));
	}
	for (int32 i = 0; i < FDreamTextureJobTracker::MaxTracked + 50; ++i)
	{
		Tracker->Track(MakeJob(FString::Printf(TEXT("done_%d"), i), EDreamTextureJobStatus::Succeeded, 1.0f));
	}

	TestTrue(TEXT("总数封顶"), Tracker->GetNumTracked() <= FDreamTextureJobTracker::MaxTracked);
	TestEqual(TEXT("进行中的一个没丢"), Tracker->GetActiveJobs().Num(), 5);
	TestTrue(TEXT("丢的是最老的已结束任务"), Tracker->Find(TEXT("done_0")) == nullptr);
	TestTrue(TEXT("最新的还在"), Tracker->Find(FString::Printf(TEXT("done_%d"), FDreamTextureJobTracker::MaxTracked + 49)) != nullptr);

	// 全是进行中的：宁可超过上限，也不丢还在等结果的。
	FFakeHooks Fake2;
	const TSharedRef<FDreamTextureJobTracker> Busy = MakeTracker(Fake2);
	for (int32 i = 0; i < FDreamTextureJobTracker::MaxTracked + 10; ++i)
	{
		Busy->Track(MakeJob(FString::Printf(TEXT("run_%d"), i), EDreamTextureJobStatus::Running));
	}
	TestEqual(TEXT("进行中的全留着"), Busy->GetNumTracked(), FDreamTextureJobTracker::MaxTracked + 10);

	Tracker->Forget(TEXT("run_0"));
	TestTrue(TEXT("Forget 之后查不到"), Tracker->Find(TEXT("run_0")) == nullptr);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureTrackerBindTest, "DreamTexture.Client.Generate.Tracker.Bind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureTrackerBindTest::RunTest(const FString& Parameters)
{
	// 经过真正的连接对象广播：验证订阅接线，而不只是直接调 Handle*。
	// 连接不 Start()，不会发任何网络请求；OnEvent / OnLinkChanged 是公开的多播委托，直接广播。
	const TSharedRef<FDreamTextureConnection> Conn = MakeShared<FDreamTextureConnection>();

	FFakeHooks Fake;
	TSharedPtr<FDreamTextureJobTracker> Tracker = MakeTracker(Fake);
	Tracker->Bind(Conn);
	Tracker->Track(MakeJob(TEXT("j"), EDreamTextureJobStatus::Queued));

	Conn->OnEvent.Broadcast(MakeJobEvent(TEXT("job.progress"), TEXT("j"), TEXT("running"), 0.5, TEXT("生成中")));
	TestTrue(TEXT("连接上的事件到了追踪器"), Tracker->Find(TEXT("j"))->Status == EDreamTextureJobStatus::Running);

	Conn->OnLinkChanged.Broadcast(EDreamTextureLink::Live);
	TestEqual(TEXT("链路变 Live 触发补状态"), Fake.Gets.Num(), 1);

	// 重复 Bind：先解旧的再绑，重新绑上之后照常收。
	int32 Broadcasts = 0;
	Tracker->OnJobChanged.AddLambda([&Broadcasts](const FDreamTextureJob&) { ++Broadcasts; });
	Tracker->Bind(Conn);
	Conn->OnEvent.Broadcast(MakeJobEvent(TEXT("job.progress"), TEXT("j"), TEXT("running"), 0.8, TEXT("生成中")));
	TestEqual(TEXT("重复 Bind 之后仍然收得到"), Broadcasts, 1);

	// Unbind 之后不再收。
	Tracker->Unbind();
	Conn->OnEvent.Broadcast(MakeJobEvent(TEXT("job.done"), TEXT("j"), TEXT("succeeded"), 1.0));
	TestTrue(TEXT("解绑后不处理"), Tracker->Find(TEXT("j"))->Status == EDreamTextureJobStatus::Running);

	// 追踪器先于连接销毁、连接再广播：不能崩（AddSP 是弱引用，析构里也摘了）。
	Tracker->Bind(Conn);
	Tracker.Reset();
	Conn->OnEvent.Broadcast(MakeJobEvent(TEXT("job.progress"), TEXT("j"), TEXT("running"), 0.9));
	Conn->OnLinkChanged.Broadcast(EDreamTextureLink::Live);
	TestTrue(TEXT("追踪器销毁后连接广播没有崩溃"), true);

	return true;
}

// ---------------------------------------------------------------------------
// 网络层：不连后端也能验证的失败路径
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateApiOfflineTest, "DreamTexture.Client.Generate.Offline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateApiOfflineTest::RunTest(const FString& Parameters)
{
	struct FObserved
	{
		int32 SubmitCalls = 0;
		bool bSubmitOK = true;
		FString SubmitError;

		int32 GetCalls = 0;
		bool bGetOK = true;
		bool bGetNotFound = true;
		FString GetError;

		int32 CancelCalls = 0;
		bool bCancelOK = true;

		int32 CatalogCalls = 0;
		bool bCatalogOK = true;

		int32 RefineCalls = 0;
		bool bRefineOK = true;
	};
	const TSharedRef<FObserved> Observed = MakeShared<FObserved>();

	// 1. 不合法的请求：根本不发，但回调仍然异步（返回时还没跑）。
	{
		FDreamTextureGenerateRequest Bad;
		Bad.WorkflowId = TEXT("w");
		Bad.Variants = 9;
		FDreamTextureGenerateApi::Submit(Bad, [Observed](bool bOK, TArray<FDreamTextureJob>&& Jobs, const FString& Error)
		{
			++Observed->SubmitCalls;
			Observed->bSubmitOK = bOK;
			Observed->SubmitError = Error;
		});
		TestEqual(TEXT("[非法请求] 返回时回调还没跑"), Observed->SubmitCalls, 0);
	}

	// 2. 其余请求发到一个没人监听的地址：连接被拒，回调一次，且不是「没有这个任务」。
	{
		DreamTextureTest::FBackendUrlOverride Override(TEXT("http://127.0.0.1:18779"));

		FDreamTextureGenerateApi::GetJob(TEXT("job_x"), [Observed](bool bOK, FDreamTextureJob&&, const FString& Error, bool bNotFound)
		{
			++Observed->GetCalls;
			Observed->bGetOK = bOK;
			Observed->bGetNotFound = bNotFound;
			Observed->GetError = Error;
		});
		FDreamTextureGenerateApi::CancelJob(TEXT("job_x"), [Observed](bool bOK, const FString&)
		{
			++Observed->CancelCalls;
			Observed->bCancelOK = bOK;
		});
		FDreamTextureGenerateApi::ListWorkflows([Observed](bool bOK, FDreamTextureWorkflowCatalog&&, const FString&)
		{
			++Observed->CatalogCalls;
			Observed->bCatalogOK = bOK;
		});
		FDreamTextureGenerateApi::RefinePrompt(TEXT("x"), EDreamTextureRefinePurpose::Texture, FString(),
			[Observed](bool bOK, FDreamTextureRefineResult&&, const FString&)
			{
				++Observed->RefineCalls;
				Observed->bRefineOK = bOK;
			});
	}

	FAutomationTestBase* Test = this;
	ADD_LATENT_AUTOMATION_COMMAND(FDreamTextureWaitCommand(Test, TEXT("四个请求与非法提交都回调了"), 20.0,
		[Observed]()
		{
			return Observed->SubmitCalls > 0 && Observed->GetCalls > 0 && Observed->CancelCalls > 0
				&& Observed->CatalogCalls > 0 && Observed->RefineCalls > 0;
		}));

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
			Test->TestEqual(TEXT("[非法请求] 只回调一次"), Observed->SubmitCalls, 1);
			Test->TestFalse(TEXT("[非法请求] 失败"), Observed->bSubmitOK);
			Test->TestTrue(TEXT("[非法请求] 原因点明变体数"), Observed->SubmitError.Contains(TEXT("variants")));

			Test->TestEqual(TEXT("[查任务] 只回调一次"), Observed->GetCalls, 1);
			Test->TestFalse(TEXT("[查任务] 连不上是失败"), Observed->bGetOK);
			Test->TestFalse(TEXT("[查任务] 连不上不是「任务不存在」：只有后端明说 404 才算"), Observed->bGetNotFound);
			Test->TestTrue(TEXT("[查任务] 原因里有任务 id"), Observed->GetError.Contains(TEXT("job_x")));

			Test->TestEqual(TEXT("[取消] 只回调一次"), Observed->CancelCalls, 1);
			Test->TestFalse(TEXT("[取消] 失败"), Observed->bCancelOK);
			Test->TestEqual(TEXT("[目录] 只回调一次"), Observed->CatalogCalls, 1);
			Test->TestFalse(TEXT("[目录] 失败"), Observed->bCatalogOK);
			Test->TestEqual(TEXT("[扩写] 只回调一次"), Observed->RefineCalls, 1);
			Test->TestFalse(TEXT("[扩写] 失败"), Observed->bRefineOK);
		}));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
