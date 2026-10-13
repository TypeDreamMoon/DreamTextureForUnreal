// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureHttpUtil.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HttpModule.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	/** 把 HTTP 层的失败翻成人话。措辞与 DreamTextureConnection.cpp 里同名函数保持一致。 */
	FString DescribeTransportFailure(const FHttpRequestPtr& Request, const FHttpResponsePtr& Response)
	{
		// GetFailureReason() 比只看 bSucceeded 能说清楚得多：
		// 「连不上」和「连上了但超时」对用户来说是两件要做不同处理的事。
		const EHttpFailureReason Reason = Request.IsValid() ? Request->GetFailureReason() : EHttpFailureReason::Other;
		switch (Reason)
		{
		case EHttpFailureReason::ConnectionError:
			return NSLOCTEXT("DreamTextureHttp", "ConnectionError", "cannot connect; the backend process may not be running").ToString();
		case EHttpFailureReason::TimedOut:
			return NSLOCTEXT("DreamTextureHttp", "TimedOut", "no response before the timeout").ToString();
		case EHttpFailureReason::Cancelled:
			return NSLOCTEXT("DreamTextureHttp", "Cancelled", "the request was cancelled").ToString();
		case EHttpFailureReason::ResponseTooLarge:
			return NSLOCTEXT("DreamTextureHttp", "ResponseTooLarge", "the response is too large").ToString();
		default:
			break;
		}

		if (Response.IsValid())
		{
			return FText::Format(NSLOCTEXT("DreamTextureHttp", "ReturnedStatus", "returned HTTP {0}"), FText::AsNumber(Response->GetResponseCode())).ToString();
		}
		return NSLOCTEXT("DreamTextureHttp", "RequestFailed", "the request failed").ToString();
	}
}

void DreamTexture::RunNextTick(TFunction<void()> Fn)
{
	// 延迟 0 秒 = 下一次核心 ticker。回调返回 false 表示只跑一次，系统会自己摘掉它——
	// 不要在回调里 RemoveTicker 自己，那是死锁（见 DreamTextureConnection.cpp 里记的坑）。
	FTSTicker::GetCoreTicker().AddTicker(
		TEXT("DreamTexture.RunNextTick"), 0.0f,
		[Fn = MoveTemp(Fn)](float) -> bool
		{
			if (Fn)
			{
				Fn();
			}
			return false;
		});
}

namespace
{
	/** 一次 SendRequest 的投递状态，让请求的委托和「发不出去」的兜底共用同一道单次投递的闸。 */
	struct FSendState
	{
		DreamTexture::FOnHttpOutcome Done;

		/** 结果已经交给 Done（或已经排期交给）了。 */
		bool bDelivered = false;

		/** 还在 ProcessRequest() 的调用栈里。这期间到来的结果必须推迟到下一帧再投递。 */
		bool bInsideProcessRequest = true;
	};

	void Deliver(const TSharedRef<FSendState>& State, DreamTexture::FHttpOutcome Outcome)
	{
		if (State->bDelivered)
		{
			return;
		}
		State->bDelivered = true;

		if (State->bInsideProcessRequest)
		{
			DreamTexture::RunNextTick([State, Outcome]()
			{
				if (State->Done)
				{
					State->Done(Outcome);
				}
			});
		}
		else if (State->Done)
		{
			State->Done(Outcome);
		}
	}
}

namespace
{
	/** JSON 与原始字节两个入口共用的发送与投递逻辑；Configure 负责往请求上放请求体和头。 */
	void SendConfigured(const TCHAR* Verb, const FString& URL, float TimeoutSecs, TFunction<void(IHttpRequest&)> Configure, DreamTexture::FOnHttpOutcome Done);
}

void DreamTexture::SendRequest(const TCHAR* Verb, const FString& URL, float TimeoutSecs, const FString& JsonBody, FOnHttpOutcome Done)
{
	SendConfigured(Verb, URL, TimeoutSecs, [&JsonBody](IHttpRequest& Request)
	{
		if (!JsonBody.IsEmpty())
		{
			Request.SetHeader(TEXT("Content-Type"), TEXT("application/json; charset=utf-8"));
			Request.SetContentAsString(JsonBody);
		}
	}, MoveTemp(Done));
}

void DreamTexture::SendRequestBytes(const TCHAR* Verb, const FString& URL, float TimeoutSecs, const FString& ContentType, TArray<uint8>&& Body, FOnHttpOutcome Done)
{
	SendConfigured(Verb, URL, TimeoutSecs, [&ContentType, &Body](IHttpRequest& Request)
	{
		Request.SetHeader(TEXT("Content-Type"), ContentType);
		Request.SetContent(MoveTemp(Body));
	}, MoveTemp(Done));
}

namespace
{
void SendConfigured(const TCHAR* Verb, const FString& URL, float TimeoutSecs, TFunction<void(IHttpRequest&)> Configure, DreamTexture::FOnHttpOutcome Done)
{
	using namespace DreamTexture;

	const TSharedRef<FSendState> State = MakeShared<FSendState>();
	State->Done = MoveTemp(Done);

	const FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(URL);
	Request->SetVerb(Verb);
	Request->SetTimeout(TimeoutSecs);
	if (Configure)
	{
		Configure(*Request);
	}

	Request->OnProcessRequestComplete().BindLambda(
		[State](FHttpRequestPtr InRequest, FHttpResponsePtr InResponse, bool bSucceeded)
		{
			FHttpOutcome Outcome;
			Outcome.Response = InResponse;

			if (!bSucceeded || !InResponse.IsValid())
			{
				Outcome.Error = DescribeTransportFailure(InRequest, InResponse);
			}
			else
			{
				Outcome.StatusCode = InResponse->GetResponseCode();
				if (EHttpResponseCodes::IsOk(Outcome.StatusCode))
				{
					Outcome.bOK = true;
				}
				else
				{
					// 后端的错误都是 {"error":"中文原因"}，那句话比一个状态码有用得多。
					const FString BackendMessage = ExtractBackendError(InResponse);
					Outcome.Error = BackendMessage.IsEmpty()
						? FText::Format(NSLOCTEXT("DreamTextureHttp", "BackendStatus", "The backend returned HTTP {0}."), FText::AsNumber(Outcome.StatusCode)).ToString()
						: FText::Format(NSLOCTEXT("DreamTextureHttp", "BackendStatusWithMessage", "The backend returned HTTP {0}: {1}"), FText::AsNumber(Outcome.StatusCode), FText::FromString(BackendMessage)).ToString();
				}
			}

			Deliver(State, MoveTemp(Outcome));
		});

	/*
	 * ProcessRequest() 返回 false 时，委托**也可能**已经被同步调用过了。
	 *
	 * 5.8 的 FHttpRequestCommon::PreProcess() 在 PreCheck / SetupRequest 失败时，
	 * 先 FinishRequestNotInHttpManager()（游戏线程上直接 FinishRequest()，也就是当场触发完成委托），
	 * 再返回 false。所以「返回 false 就自己补一次失败回调」会让调用方收到两次。
	 * 这里的做法：两条路径共用 Deliver() 的单次闸；而且在 ProcessRequest() 栈内到来的结果
	 * 一律推迟一帧，让「回调永远异步」这条承诺在失败路径上也成立。
	 */
	const bool bStarted = Request->ProcessRequest();
	if (!bStarted)
	{
		// 此时仍标着「在 ProcessRequest 栈内」，所以这次投递会被推迟到下一帧。
		FHttpOutcome Outcome;
		Outcome.Error = FText::Format(NSLOCTEXT("DreamTextureHttp", "CannotSend", "The request could not be sent; check that the address is correct: {0}"), FText::FromString(URL)).ToString();
		Deliver(State, MoveTemp(Outcome));
	}
	State->bInsideProcessRequest = false;
}
}

FString DreamTexture::PercentEncode(const FString& Text)
{
	// FTCHARToUTF8 整串转换，代理对会被正确合成一个 4 字节序列。
	const FTCHARToUTF8 Utf8(*Text);

	FString Encoded;
	Encoded.Reserve(Utf8.Length());
	for (int32 Index = 0; Index < Utf8.Length(); ++Index)
	{
		const uint8 Byte = static_cast<uint8>(Utf8.Get()[Index]);
		const bool bUnreserved = (Byte >= 'A' && Byte <= 'Z') || (Byte >= 'a' && Byte <= 'z')
			|| (Byte >= '0' && Byte <= '9') || Byte == '-' || Byte == '.' || Byte == '_' || Byte == '~';
		if (bUnreserved)
		{
			Encoded.AppendChar(static_cast<TCHAR>(Byte));
		}
		else
		{
			Encoded.Appendf(TEXT("%%%02X"), Byte);
		}
	}
	return Encoded;
}

TSharedPtr<FJsonObject> DreamTexture::ParseJsonObject(const FString& Text)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Root))
	{
		return nullptr;
	}
	return Root;
}

FString DreamTexture::ExtractBackendError(const FHttpResponsePtr& Response)
{
	if (!Response.IsValid())
	{
		return FString();
	}

	const TSharedPtr<FJsonObject> Root = ParseJsonObject(Response->GetContentAsString());
	FString Message;
	if (Root.IsValid())
	{
		Root->TryGetStringField(TEXT("error"), Message);
	}
	return Message;
}
