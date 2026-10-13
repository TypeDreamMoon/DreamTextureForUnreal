// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

// 前置声明必须在全局作用域：写在 DreamTexture 命名空间里的 `class FJsonObject` 会声明出另一个
// DreamTexture::FJsonObject，与引擎的 ::FJsonObject 不是同一个类型（不带 PCH 编译时才会暴露）。
class FJsonObject;

/*
 * DreamTextureApi.cpp 和 DreamTextureFetcher.cpp 共用的 HTTP 小工具。
 *
 * 线程假设和 DreamTextureConnection.cpp 顶部写的一样：FHttpModule 的完成委托默认在游戏线程上跑
 * （EHttpRequestDelegateThreadPolicy::CompleteOnGameThread），我们没改过它。
 * 所以这里 SendRequest 的回调、RunNextTick 的回调都在游戏线程，调用方的状态不用加锁。
 */

namespace DreamTexture
{
	/** 一次 HTTP 往返的结论。bOK 为 true 当且仅当传输成功且状态码是 2xx。 */
	struct FHttpOutcome
	{
		bool bOK = false;

		/** 没拿到应答时是 0。 */
		int32 StatusCode = 0;

		FHttpResponsePtr Response;

		/** bOK 为 false 时的中文原因，可直接拼进界面文案。 */
		FString Error;
	};

	using FOnHttpOutcome = TFunction<void(const FHttpOutcome&)>;

	/**
	 * 发一个请求。传输失败、非 2xx、同步发不出去，都折成同一种 bOK=false + 中文 Error 交给回调。
	 *
	 * 回调**一定是异步的**：连「同步就发不出去」也会推迟到下一帧再回调。
	 * 调用方于是不用担心「回调在我设置好状态之前就跑完了」。
	 *
	 * @param JsonBody 非空时作为 application/json 的请求体。
	 */
	void SendRequest(const TCHAR* Verb, const FString& URL, float TimeoutSecs, const FString& JsonBody, FOnHttpOutcome Done);

	/**
	 * 同 SendRequest，但请求体是原始字节、Content-Type 由调用方给（multipart 上传用）。
	 * 回调的保证、失败的折叠方式与 SendRequest 完全一样——两者共用同一套投递逻辑。
	 */
	void SendRequestBytes(const TCHAR* Verb, const FString& URL, float TimeoutSecs, const FString& ContentType, TArray<uint8>&& Body, FOnHttpOutcome Done);

	/** 把一个函数推迟到下一次核心 ticker（游戏线程）上执行，只执行一次。 */
	void RunNextTick(TFunction<void()> Fn);

	/**
	 * 百分号编码，按 UTF-8 字节。只放过 RFC 3986 的 unreserved：A-Z a-z 0-9 - . _ ~
	 *
	 * 不用 FGenericPlatformHttp::UrlEncode：它逐个 TCHAR 转 UTF-8，遇到代理对（emoji 之类
	 * 的增补平面字符）会拆成两个孤立的半个字符，编出来的是乱码。搜索框里什么都可能输进来。
	 */
	FString PercentEncode(const FString& Text);

	/** 从应答体里取后端写的 {"error":"…"}。没有、或者不是那种格式，返回空串。 */
	FString ExtractBackendError(const FHttpResponsePtr& Response);

	/** 把应答体当 JSON 对象解析。失败返回空指针。 */
	TSharedPtr<FJsonObject> ParseJsonObject(const FString& Text);
}
