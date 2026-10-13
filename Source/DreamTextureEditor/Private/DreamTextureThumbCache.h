// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DreamTextureApi.h"

struct FSlateDynamicImageBrush;
class IImageWrapperModule;
struct FSlateBrush;

/**
 * 素材缩略图：磁盘缓存 + 有上限的内存画刷缓存，解码全在后台线程。
 *
 * ## 为什么解码不能在游戏线程
 *
 * preview.png 是 3×3 平铺预览，1K 材质的话是 3072² 量级。一屏十几张卡片同时
 * 解码，游戏线程会卡上好几秒，编辑器整个假死。所以：下载回来的字节交给线程池，
 * 后台用 ImageWrapper 解码、缩到缩略图尺寸，只有最后「用 BGRA 字节建画刷」这一步
 * （Slate 渲染器资源，必须在游戏线程）回来做。
 *
 * ## 三层
 *
 * 1. 内存：FSlateDynamicImageBrush，LRU，封顶 MaxEntries。画刷持有渲染器资源，
 *    不封顶的话滚几千张素材就是显存泄漏。
 * 2. 磁盘：<工程 Saved>/DreamTexture/Thumbs/<id>.png，存的是**已经缩小**的缩略图
 *    （不是原图），下次开面板解码很快，也不用再碰后端。
 * 3. 后端：FDreamTextureApi::FetchFile(id, "preview.png")。材质产物在后端是不可变的，
 *    所以磁盘缓存永不过期。
 *
 * ## 生命周期
 *
 * 所有跨线程回调只捕获弱指针。面板关掉、缓存析构之后迟到的结果 Pin 不到，直接丢弃，
 * 不会去碰一个已经销毁的渲染器资源表。
 */
class FDreamTextureThumbCache : public TSharedFromThis<FDreamTextureThumbCache>
{
public:
	/** 缩略图长边像素。卡片显示 ~160px，留出高 DPI 的余量。 */
	static constexpr int32 ThumbMaxEdge = 256;

	/** 取图函数，默认是 FDreamTextureApi::FetchFile(id, "preview.png")。测试时替换。 */
	using FFetchFn = TFunction<void(const FString& /*Id*/, FDreamTextureApi::FOnBytes)>;

	/**
	 * @param InThumbMaxEdge 解码后缩到的长边像素。素材库卡片用默认的 256；回流的结果预览要看清细节，给更大的值。
	 *        键里要体现差别（磁盘缓存存的是缩后的图，同一个键不能被两种尺寸共用）——回流用自己的键前缀，不与素材库的撞。
	 */
	explicit FDreamTextureThumbCache(int32 InMaxEntries = 200, FFetchFn InFetch = nullptr, int32 InThumbMaxEdge = ThumbMaxEdge);
	~FDreamTextureThumbCache();

	/**
	 * 取这张的画刷。没有就返回 nullptr，并（在没失败过、没在加载的前提下）顺手排一次加载。
	 * 在游戏线程、可以在 Slate 的属性回调里调。
	 */
	const FSlateBrush* GetBrush(const FString& Id);

	/** 已经有画刷了吗（不触发加载）。 */
	bool HasBrush(const FString& Id) const;

	/** 在加载中。卡片据此显示「加载中」而不是「无预览」。 */
	bool IsPending(const FString& Id) const { return InFlight.Contains(Id); }

	/** 最近失败过且还没到自动重试的时候。 */
	bool IsFailed(const FString& Id) const { return Failed.Contains(Id); }

	/** 清掉失败记录，让下一次 GetBrush 重试。用户点「刷新」时调。 */
	void ResetFailures();

	int32 GetNumBrushes() const { return Brushes.Num(); }
	int32 GetMaxEntries() const { return MaxEntries; }

	/**
	 * 解码 + 缩小。纯函数，不碰 Slate，任意线程可调——后台任务用它，测试也直接用它。
	 *
	 * 成功返回 true：OutBGRA 是 OutWidth×OutHeight 的 8 位 BGRA，长边不超过 MaxEdge。
	 * OutPng 非空时附带把缩小后的图编码成 PNG（给磁盘缓存用）。
	 */
	static bool DecodeAndShrink(IImageWrapperModule& Wrapper, TConstArrayView<uint8> Source, int32 MaxEdge,
		int32& OutWidth, int32& OutHeight, TArray<uint8>& OutBGRA, TArray64<uint8>* OutPng);

	/** <工程 Saved>/DreamTexture/Thumbs。 */
	static FString GetDiskDir();
	/** id 里不该有路径分隔符，但这是从网络来的字符串，落盘前先过一遍。 */
	static FString GetDiskPath(const FString& Id);

private:
	struct FDecoded
	{
		int32 Width = 0;
		int32 Height = 0;
		TArray<uint8> BGRA;
	};

	void StartLoad(const FString& Id);
	void BeginNetworkFetch(const FString& Id);
	void HandleDecoded(const FString& Id, TSharedPtr<FDecoded> Decoded, const FString& FailureReason);
	void MarkFailed(const FString& Id, const FString& Reason);
	void Touch(const FString& Id);
	void Trim();

	int32 MaxEntries;
	int32 ThumbEdge;
	FFetchFn Fetch;

	/** ImageWrapper 模块要在游戏线程加载，这里取好指针，后台线程直接用。 */
	IImageWrapperModule* Wrapper = nullptr;

	TMap<FString, TSharedPtr<FSlateDynamicImageBrush>> Brushes;
	/** 最久没用的在前。 */
	TArray<FString> LruOrder;
	TSet<FString> InFlight;
	/** id → 失败时间（秒）。超过 RetryAfterSeconds 后允许自动重试。 */
	TMap<FString, double> Failed;

	static constexpr double RetryAfterSeconds = 30.0;
};
