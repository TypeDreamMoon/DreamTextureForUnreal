// Copyright TypeDreamMoon. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureGenerate.h"
#include "DreamTextureHttpUtil.h"
#include "DreamTextureTestUtil.h"
#include "DreamTextureUpload.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"

/*
 * 回流（M8）在 Client 层加的两样东西：POST /api/uploads 的封装，和生成请求里的 derived_from。
 *
 * 全部不连后端：multipart 请求体、应答解析、本地就能判定的失败（扩展名、大小、读不到文件）、
 * 连不上时的失败，以及 derived_from 的写出、本地校验与从任务里读回。
 * 真的连后端的部分见 DreamTexture.Live.Reflow。
 */

namespace
{
	/** 在请求体里找一段字节，找不到返回 INDEX_NONE。 */
	int32 FindBytes(const TArray<uint8>& Haystack, const TArray<uint8>& Needle, int32 Start = 0)
	{
		if (Needle.Num() == 0 || Haystack.Num() < Needle.Num())
		{
			return INDEX_NONE;
		}
		for (int32 Index = Start; Index + Needle.Num() <= Haystack.Num(); ++Index)
		{
			if (FMemory::Memcmp(Haystack.GetData() + Index, Needle.GetData(), Needle.Num()) == 0)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	TArray<uint8> Ascii(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		TArray<uint8> Out;
		Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		return Out;
	}

	FString AsText(const TArray<uint8>& Bytes)
	{
		// 只用来检查头部：把非 ASCII 的字节换成问号，别让二进制把字符串截断。
		FString Out;
		for (const uint8 Byte : Bytes)
		{
			Out.AppendChar(Byte >= 0x20 && Byte < 0x7f ? static_cast<TCHAR>(Byte) : (Byte == '\r' ? TEXT('\r') : (Byte == '\n' ? TEXT('\n') : TEXT('?'))));
		}
		return Out;
	}

	FString TempFile(const TCHAR* Extension)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamTextureTest") / TEXT("Upload")
			/ (FGuid::NewGuid().ToString(EGuidFormats::Digits) + Extension));
	}
}

// ---------------------------------------------------------------------------
// multipart 请求体
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureUploadMultipartTest, "DreamTexture.Client.Upload.Multipart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureUploadMultipartTest::RunTest(const FString& Parameters)
{
	// 载荷里故意放进后端解析最容易出事的字节：CRLF、NUL、像边界的文本。它们必须原样、连续地出现在请求体里。
	TArray<uint8> Payload = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0x00, 0xff, '-', '-', 'x', '\r', '\n', 0x00 };

	TArray<uint8> Body;
	FDreamTextureUploadApi::BuildMultipartBody(TEXT("XBOUNDARYX"), TEXT("image"), TEXT("Base.png"), TEXT("image/png"), Payload, Body);

	const FString Text = AsText(Body);
	TestTrue(TEXT("以边界开头"), Text.StartsWith(TEXT("--XBOUNDARYX\r\n")));
	TestTrue(TEXT("字段名是 image（后端 FormFile 只认这个名字）"), Text.Contains(TEXT("Content-Disposition: form-data; name=\"image\"; filename=\"Base.png\"\r\n")));
	TestTrue(TEXT("带 Content-Type"), Text.Contains(TEXT("Content-Type: image/png\r\n\r\n")));
	TestTrue(TEXT("以收尾边界结束"), Text.EndsWith(TEXT("\r\n--XBOUNDARYX--\r\n")));

	const TArray<uint8> HeaderEnd = Ascii(TEXT("Content-Type: image/png\r\n\r\n"));
	const int32 HeaderAt = FindBytes(Body, HeaderEnd);
	if (TestTrue(TEXT("找得到头部结尾"), HeaderAt != INDEX_NONE))
	{
		const int32 PayloadAt = HeaderAt + HeaderEnd.Num();
		TestTrue(TEXT("载荷原样、连续地跟在头部后面"),
			PayloadAt + Payload.Num() <= Body.Num() && FMemory::Memcmp(Body.GetData() + PayloadAt, Payload.GetData(), Payload.Num()) == 0);
		const TArray<uint8> Tail = Ascii(TEXT("\r\n--XBOUNDARYX--\r\n"));
		TestEqual(TEXT("载荷之后紧跟收尾边界，中间没有多余字节"), Body.Num(), PayloadAt + Payload.Num() + Tail.Num());
	}

	// 文件名里的引号与换行会把一个头切成两个：换掉。
	TArray<uint8> Sanitized;
	FDreamTextureUploadApi::BuildMultipartBody(TEXT("B"), TEXT("image"), TEXT("a\"b\r\nX-Evil: 1.png"), TEXT("image/png"), Payload, Sanitized);
	const FString SanitizedText = AsText(Sanitized);
	TestFalse(TEXT("文件名里的换行没能注入新的头"), SanitizedText.Contains(TEXT("\r\nX-Evil")));
	TestTrue(TEXT("文件名里的引号换成了下划线"), SanitizedText.Contains(TEXT("filename=\"a_b__X-Evil: 1.png\"")));

	// 空文件也要拼得出合法的请求体（后端会拒绝它，但不该是我们这边崩）。
	TArray<uint8> Empty;
	FDreamTextureUploadApi::BuildMultipartBody(TEXT("B"), TEXT("image"), TEXT("e.png"), TEXT("image/png"), TConstArrayView<uint8>(), Empty);
	TestTrue(TEXT("空文件的请求体仍以收尾边界结束"), AsText(Empty).EndsWith(TEXT("\r\n--B--\r\n")));
	return true;
}

// ---------------------------------------------------------------------------
// 应答、扩展名、Content-Type
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureUploadResponseTest, "DreamTexture.Client.Upload.Response",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureUploadResponseTest::RunTest(const FString& Parameters)
{
	FString Name;
	TestTrue(TEXT("后端的应答"), FDreamTextureUploadApi::ParseResponse(TEXT("{\"name\":\"dt_ref_ab12.png\",\"bytes\":1234,\"original\":\"Base.png\"}"), Name));
	TestEqual(TEXT("文件名原样取出"), Name, FString(TEXT("dt_ref_ab12.png")));

	// ComfyUI 把图放进子目录时，后端返回「子目录/文件名」，原样用。
	TestTrue(TEXT("带子目录"), FDreamTextureUploadApi::ParseResponse(TEXT("{\"name\":\"sub/dt_ref_x.png\"}"), Name));
	TestEqual(TEXT("子目录不丢"), Name, FString(TEXT("sub/dt_ref_x.png")));

	Name.Reset();
	TestFalse(TEXT("没有 name"), FDreamTextureUploadApi::ParseResponse(TEXT("{\"bytes\":1}"), Name));
	TestFalse(TEXT("name 是 null"), FDreamTextureUploadApi::ParseResponse(TEXT("{\"name\":null}"), Name));
	TestFalse(TEXT("name 是空白"), FDreamTextureUploadApi::ParseResponse(TEXT("{\"name\":\"  \"}"), Name));
	TestFalse(TEXT("不是 JSON"), FDreamTextureUploadApi::ParseResponse(TEXT("<html>"), Name));
	TestFalse(TEXT("JSON 数组"), FDreamTextureUploadApi::ParseResponse(TEXT("[1]"), Name));
	TestTrue(TEXT("失败时没有改写输出"), Name.IsEmpty());

	for (const TCHAR* Good : { TEXT("a.png"), TEXT("A.PNG"), TEXT("a.jpg"), TEXT("a.jpeg"), TEXT("a.webp"), TEXT("a.bmp"), TEXT("C:/x/y.Png") })
	{
		TestTrue(FString::Printf(TEXT("后端收 %s"), Good), FDreamTextureUploadApi::IsSupportedExtension(Good));
	}
	for (const TCHAR* Bad : { TEXT("a.tga"), TEXT("a.exr"), TEXT("a"), TEXT("a.png.exe"), TEXT("") })
	{
		TestFalse(FString::Printf(TEXT("后端不收「%s」"), Bad), FDreamTextureUploadApi::IsSupportedExtension(Bad));
	}

	TestEqual(TEXT("png"), FDreamTextureUploadApi::GuessContentType(TEXT("a.png")), FString(TEXT("image/png")));
	TestEqual(TEXT("jpeg"), FDreamTextureUploadApi::GuessContentType(TEXT("a.JPEG")), FString(TEXT("image/jpeg")));
	TestEqual(TEXT("webp"), FDreamTextureUploadApi::GuessContentType(TEXT("a.webp")), FString(TEXT("image/webp")));
	TestEqual(TEXT("bmp"), FDreamTextureUploadApi::GuessContentType(TEXT("a.bmp")), FString(TEXT("image/bmp")));
	TestEqual(TEXT("未知"), FDreamTextureUploadApi::GuessContentType(TEXT("a.xyz")), FString(TEXT("application/octet-stream")));
	TestEqual(TEXT("上限是 32 MB（后端 maxUploadBytes）"), FDreamTextureUploadApi::MaxUploadBytes, static_cast<int64>(32 * 1024 * 1024));
	return true;
}

// ---------------------------------------------------------------------------
// 不连后端也能判定的失败，以及连不上
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureUploadOfflineTest, "DreamTexture.Client.Upload.Offline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureUploadOfflineTest::RunTest(const FString& Parameters)
{
	struct FObserved
	{
		int32 MissingCalls = 0;
		FString MissingError;
		int32 ExtensionCalls = 0;
		FString ExtensionError;
		int32 TooBigCalls = 0;
		FString TooBigError;
		int32 RefusedCalls = 0;
		bool bRefusedOK = true;
		FString RefusedError;
		FString BigFile;
		FString SmallFile;
	};
	const TSharedRef<FObserved> Observed = MakeShared<FObserved>();

	// 1. 文件不存在。回调是异步的：返回时还没跑。
	FDreamTextureUploadApi::UploadImage(TempFile(TEXT(".png")), [Observed](bool bOK, FString&&, const FString& Error)
	{
		++Observed->MissingCalls;
		Observed->MissingError = bOK ? FString() : Error;
	});
	TestEqual(TEXT("[文件不存在] 返回时回调还没跑"), Observed->MissingCalls, 0);

	// 2. 扩展名后端不收：不读文件、不发请求。
	FDreamTextureUploadApi::UploadImage(TEXT("C:/nowhere/base.tga"), [Observed](bool bOK, FString&&, const FString& Error)
	{
		++Observed->ExtensionCalls;
		Observed->ExtensionError = bOK ? FString() : Error;
	});

	// 3. 超过 32 MB：在本地拦下，不把几十 MB 白白送过去。用一个刚好多一个字节的全零文件。
	Observed->BigFile = TempFile(TEXT(".png"));
	{
		TArray<uint8> Big;
		Big.SetNumZeroed(static_cast<int32>(FDreamTextureUploadApi::MaxUploadBytes) + 1);
		TestTrue(TEXT("写出超限的测试文件"), FFileHelper::SaveArrayToFile(Big, *Observed->BigFile));
	}
	FDreamTextureUploadApi::UploadImage(Observed->BigFile, [Observed](bool bOK, FString&&, const FString& Error)
	{
		++Observed->TooBigCalls;
		Observed->TooBigError = bOK ? FString() : Error;
	});

	// 4. 合法的小文件发到没人监听的地址：连接被拒，回调一次，原因带「上传失败」。
	Observed->SmallFile = TempFile(TEXT(".png"));
	TestTrue(TEXT("写出小文件"), FFileHelper::SaveArrayToFile(TArray<uint8>({ 1, 2, 3 }), *Observed->SmallFile));
	TSharedPtr<DreamTextureTest::FBackendUrlOverride> Override = MakeShared<DreamTextureTest::FBackendUrlOverride>(TEXT("http://127.0.0.1:18779"));
	FDreamTextureUploadApi::UploadImage(Observed->SmallFile, [Observed](bool bOK, FString&&, const FString& Error)
	{
		++Observed->RefusedCalls;
		Observed->bRefusedOK = bOK;
		Observed->RefusedError = Error;
	});

	FAutomationTestBase* Test = this;
	ADD_LATENT_AUTOMATION_COMMAND(FDreamTextureWaitCommand(Test, TEXT("四个上传都回调了"), 20.0,
		[Observed]()
		{
			return Observed->MissingCalls > 0 && Observed->ExtensionCalls > 0 && Observed->TooBigCalls > 0 && Observed->RefusedCalls > 0;
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
		[Test, Observed, Override]()
		{
			Test->TestEqual(TEXT("[文件不存在] 只回调一次"), Observed->MissingCalls, 1);
			Test->TestTrue(TEXT("[文件不存在] 原因点明找不到文件"), Observed->MissingError.Contains(TEXT("was not found")));

			Test->TestEqual(TEXT("[扩展名] 只回调一次"), Observed->ExtensionCalls, 1);
			Test->TestTrue(TEXT("[扩展名] 原因点明格式"), Observed->ExtensionError.Contains(TEXT("PNG")) && Observed->ExtensionError.Contains(TEXT("base.tga")));

			Test->TestEqual(TEXT("[超限] 只回调一次"), Observed->TooBigCalls, 1);
			Test->TestTrue(TEXT("[超限] 原因点明超过上限"), Observed->TooBigError.Contains(TEXT("exceeds")) && Observed->TooBigError.Contains(TEXT("32")));

			Test->TestEqual(TEXT("[连不上] 只回调一次"), Observed->RefusedCalls, 1);
			Test->TestFalse(TEXT("[连不上] 失败"), Observed->bRefusedOK);
			Test->TestTrue(TEXT("[连不上] 原因以「上传失败」打头，让人知道是哪一步"), Observed->RefusedError.StartsWith(TEXT("Upload failed")));

			IFileManager::Get().Delete(*Observed->BigFile, false, true, true);
			IFileManager::Get().Delete(*Observed->SmallFile, false, true, true);
		}));
	return true;
}

// ---------------------------------------------------------------------------
// derived_from
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureGenerateDerivedFromTest, "DreamTexture.Client.Generate.DerivedFrom",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureGenerateDerivedFromTest::RunTest(const FString& Parameters)
{
	// ---- 没设就不写 ----
	{
		FDreamTextureGenerateRequest Request;
		Request.WorkflowId = TEXT("a.b");
		FString Json;
		FString Error;
		TestTrue(TEXT("普通请求"), Request.ToJsonString(Json, Error));
		TestFalse(TEXT("没设 derived_from 就不出现在请求里（后端据此判断是不是回流）"), Json.Contains(TEXT("derived_from")));
	}

	// ---- 写出：字段名与嵌套结构和后端 material.DerivedFrom 的 json 标签一致 ----
	{
		FDreamTextureGenerateRequest Request;
		Request.WorkflowId = TEXT("src-upload-v1.dec-derive-v1");
		Request.StringParams.Add(TEXT("source_image"), TEXT("dt_ref_ab12.png"));
		Request.BoolParams.Add(TEXT("tileable"), false);
		Request.DerivedFrom.Kind = FDreamTextureDerivedFrom::KindUEMaterial();
		Request.DerivedFrom.AssetPath = TEXT("/Game/Materials/MI_Rock.MI_Rock");
		Request.DerivedFrom.Engine = TEXT("5.8.2");
		Request.DerivedFrom.Mode = FDreamTextureDerivedFrom::ModeFillChannels();
		Request.DerivedFrom.MapsIn.Add(TEXT("basecolor"), TEXT("0123456789abcdef"));

		FString Json;
		FString Error;
		if (TestTrue(*FString::Printf(TEXT("合法的 derived_from 能序列化（%s）"), *Error), Request.ToJsonString(Json, Error)))
		{
			const TSharedPtr<FJsonObject> Root = DreamTexture::ParseJsonObject(Json);
			const TSharedPtr<FJsonObject>* Derived = nullptr;
			if (TestTrue(TEXT("根上有 derived_from 对象"), Root.IsValid() && Root->TryGetObjectField(TEXT("derived_from"), Derived)))
			{
				FString Text;
				TestTrue(TEXT("kind"), (*Derived)->TryGetStringField(TEXT("kind"), Text) && Text == TEXT("ue_material"));
				TestTrue(TEXT("mode"), (*Derived)->TryGetStringField(TEXT("mode"), Text) && Text == TEXT("fill_channels"));
				TestTrue(TEXT("asset_path"), (*Derived)->TryGetStringField(TEXT("asset_path"), Text) && Text == TEXT("/Game/Materials/MI_Rock.MI_Rock"));
				TestTrue(TEXT("engine"), (*Derived)->TryGetStringField(TEXT("engine"), Text) && Text == TEXT("5.8.2"));
				const TSharedPtr<FJsonObject>* Maps = nullptr;
				if (TestTrue(TEXT("maps_in 是对象"), (*Derived)->TryGetObjectField(TEXT("maps_in"), Maps)))
				{
					TestTrue(TEXT("maps_in.basecolor"), (*Maps)->TryGetStringField(TEXT("basecolor"), Text) && Text == TEXT("0123456789abcdef"));
				}
			}
			// 与参数的位置：derived_from 在根上，不在 params 里。
			const TSharedPtr<FJsonObject>* Params = nullptr;
			if (Root.IsValid() && Root->TryGetObjectField(TEXT("params"), Params))
			{
				TestFalse(TEXT("derived_from 不混进 params"), (*Params)->HasField(TEXT("derived_from")));
			}
		}
	}

	// ---- 可选字段缺省时不写空串 ----
	{
		FDreamTextureGenerateRequest Request;
		Request.WorkflowId = TEXT("a.b");
		Request.DerivedFrom.Kind = FDreamTextureDerivedFrom::KindUEMaterial();
		Request.DerivedFrom.Mode = FDreamTextureDerivedFrom::ModeRestyle();
		FString Json;
		FString Error;
		TestTrue(TEXT("只有 kind 和 mode 也合法"), Request.ToJsonString(Json, Error));
		TestFalse(TEXT("没填 asset_path 就不写"), Json.Contains(TEXT("asset_path")));
		TestFalse(TEXT("没填 engine 就不写"), Json.Contains(TEXT("engine")));
		TestFalse(TEXT("没有指纹就不写 maps_in"), Json.Contains(TEXT("maps_in")));
	}

	// ---- 本地校验：与后端 Validate 同一套规矩 ----
	{
		auto Make = []()
		{
			FDreamTextureGenerateRequest Request;
			Request.WorkflowId = TEXT("a.b");
			Request.DerivedFrom.Kind = FDreamTextureDerivedFrom::KindUEMaterial();
			Request.DerivedFrom.Mode = FDreamTextureDerivedFrom::ModeFillChannels();
			Request.DerivedFrom.MapsIn.Add(TEXT("basecolor"), TEXT("0123456789abcdef"));
			return Request;
		};

		FString Error;
		TestTrue(TEXT("基线合法"), Make().Validate(Error));

		FDreamTextureGenerateRequest BadKind = Make();
		BadKind.DerivedFrom.Kind = TEXT("blueprint");
		TestFalse(TEXT("kind 不是 ue_material"), BadKind.Validate(Error));
		TestTrue(TEXT("原因点名 kind"), Error.Contains(TEXT("kind")));

		FDreamTextureGenerateRequest BadMode = Make();
		BadMode.DerivedFrom.Mode = TEXT("fill");
		TestFalse(TEXT("mode 拼错"), BadMode.Validate(Error));
		TestTrue(TEXT("原因点名 mode 与两个合法值"), Error.Contains(TEXT("mode")) && Error.Contains(TEXT("fill_channels")) && Error.Contains(TEXT("restyle")));

		FDreamTextureGenerateRequest NoMode = Make();
		NoMode.DerivedFrom.Mode.Empty();
		TestFalse(TEXT("设了出身却没有 mode"), NoMode.Validate(Error));

		for (const TCHAR* Bad : { TEXT("0123456789abcde"), TEXT("0123456789abcdef0"), TEXT("0123456789abcdeg"), TEXT("") })
		{
			FDreamTextureGenerateRequest BadSha = Make();
			BadSha.DerivedFrom.MapsIn.Add(TEXT("basecolor"), Bad);
			TestFalse(FString::Printf(TEXT("指纹「%s」不合法"), Bad), BadSha.Validate(Error));
			TestTrue(TEXT("原因点名 maps_in 与通道"), Error.Contains(TEXT("maps_in[basecolor]")));
		}

		FDreamTextureGenerateRequest Upper = Make();
		Upper.DerivedFrom.MapsIn.Add(TEXT("basecolor"), TEXT("0123456789ABCDEF"));
		TestTrue(TEXT("大写十六进制放行（后端会规整成小写）"), Upper.Validate(Error));

		FDreamTextureGenerateRequest EmptyChannel = Make();
		EmptyChannel.DerivedFrom.MapsIn.Add(TEXT("  "), TEXT("0123456789abcdef"));
		TestFalse(TEXT("空的通道名"), EmptyChannel.Validate(Error));

		FDreamTextureGenerateRequest LongPath = Make();
		LongPath.DerivedFrom.AssetPath = FString::ChrN(513, TEXT('a'));
		TestFalse(TEXT("asset_path 超长"), LongPath.Validate(Error));

		FDreamTextureGenerateRequest LongEngine = Make();
		LongEngine.DerivedFrom.Engine = FString::ChrN(41, TEXT('a'));
		TestFalse(TEXT("engine 超长"), LongEngine.Validate(Error));

		// Submit 路径上也走这道校验：非法的 derived_from 不会发出去。
		FString Json;
		TestFalse(TEXT("ToJsonString 也拦下非法的出身"), BadMode.ToJsonString(Json, Error));
	}

	// ---- 从任务里读回（后端把它存进 params.__derived_from）----
	{
		FDreamTextureJob Job;
		const FString Text = TEXT("{\"id\":\"job_1\",\"status\":\"running\",\"params\":{\"seed\":7,\"__derived_from\":"
			"{\"kind\":\"ue_material\",\"asset_path\":\"/Game/M.M\",\"engine\":\"5.8.2\",\"mode\":\"restyle\",\"maps_in\":{\"basecolor\":\"fedcba9876543210\"}}}}");
		if (TestTrue(TEXT("解析任务"), FDreamTextureJob::FromJson(DreamTexture::ParseJsonObject(Text), Job)))
		{
			TestEqual(TEXT("kind"), Job.DerivedFrom.Kind, FString(TEXT("ue_material")));
			TestEqual(TEXT("mode"), Job.DerivedFrom.Mode, FString(TEXT("restyle")));
			TestEqual(TEXT("asset_path"), Job.DerivedFrom.AssetPath, FString(TEXT("/Game/M.M")));
			TestEqual(TEXT("engine"), Job.DerivedFrom.Engine, FString(TEXT("5.8.2")));
			TestEqual(TEXT("maps_in 一项"), Job.DerivedFrom.MapsIn.Num(), 1);
			TestEqual(TEXT("指纹原样"), Job.DerivedFrom.MapsIn.FindRef(TEXT("basecolor")), FString(TEXT("fedcba9876543210")));
		}

		FDreamTextureJob Plain;
		TestTrue(TEXT("没有 __derived_from 的普通任务"), FDreamTextureJob::FromJson(DreamTexture::ParseJsonObject(TEXT("{\"id\":\"job_2\",\"params\":{\"seed\":1}}")), Plain));
		TestFalse(TEXT("读出来是空的"), Plain.DerivedFrom.IsSet());

		FDreamTextureJob Nulled;
		TestTrue(TEXT("__derived_from 是 null 也不崩"), FDreamTextureJob::FromJson(DreamTexture::ParseJsonObject(TEXT("{\"id\":\"job_3\",\"params\":{\"__derived_from\":null}}")), Nulled));
		TestFalse(TEXT("读出来是空的"), Nulled.DerivedFrom.IsSet());
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
