// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DreamTextureApi.h"
#include "DreamTextureImporter.h"

enum class EDreamTextureImportStage : uint8
{
	Queued,
	FetchingManifest,
	Downloading,
	Importing,
	Succeeded,
	Failed,
};

/** 一次导入的进度，界面直接据此画。 */
struct DREAMTEXTUREASSETS_API FDreamTextureImportStatus
{
	FString MaterialId;
	EDreamTextureImportStage Stage = EDreamTextureImportStage::Queued;

	/** Downloading 阶段的文件计数。 */
	int32 FilesDone = 0;
	int32 FilesTotal = 0;

	/** 当前阶段的中文说明，或失败原因。直接显示。 */
	FString Message;

	/** Succeeded / Failed 时有效。 */
	FDreamTextureImportResult Result;

	bool IsFinished() const { return Stage == EDreamTextureImportStage::Succeeded || Stage == EDreamTextureImportStage::Failed; }
};

/**
 * 一次导入的可选项。默认（全空）就是 M1/M2 的行为：设置里默认的通道，全部落成资产，再建 MI_<Slug>。
 *
 * 回流（M8）靠它做三件普通导入不做的事，见 DreamTextureReflow.h：
 *   - 只导入要的那几个通道（补通道模式下底色不导入，沿用用户原来那张）；
 *   - 新材质实例的某些贴图参数指向工程里**已有**的贴图，而不是刚导进来的；
 *   - 原地覆盖时不新建材质实例（贴图导入后由回流自己去改用户的那个实例）。
 */
struct DREAMTEXTUREASSETS_API FDreamTextureImportOptions
{
	/** true 时只导入 Channels 里的通道（哪怕它为空），而不是设置里默认的那几个。 */
	bool bExplicitChannels = false;
	TArray<FString> Channels;

	/**
	 * manifest 通道（basecolor / normal / orm / height）→ 工程里已有的贴图对象路径。
	 * 建材质实例时这些通道用这张贴图，不用后端生成的。对应的通道应当同时不在 Channels 里（否则白导入了一张）。
	 */
	TMap<FString, FString> ReuseTextures;

	/** false 时这一次不建材质实例（设置里关了的话本来也不建）。 */
	bool bCreateMaterialInstance = true;
};

/**
 * 导入的编排：拉 manifest → 查工程里已导入的哈希 → 只下载变了的 → 落资产。
 *
 * 一次只跑一个，其余排队——落资产必须在游戏线程，并发跑没有好处，还会让进度乱跳。
 * 进程内单例，由 DreamTextureAssets 模块在关闭时 Shutdown。
 *
 * 只发事件，不弹通知：通知和「在内容浏览器中显示」是界面模块订阅 OnStatusChanged 后自己做的。
 */
class DREAMTEXTUREASSETS_API FDreamTextureImportQueue
{
public:
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnStatusChanged, const FDreamTextureImportStatus& /*Status*/);

	FDreamTextureImportQueue() = default;

	// 进程内单例，不许复制。显式删掉不只是风格：类带 DLL 导出时编译器会把隐式的拷贝函数一并实例化，
	// 而成员里的 TUniquePtr 没法拷贝，会编不过。
	FDreamTextureImportQueue(const FDreamTextureImportQueue&) = delete;
	FDreamTextureImportQueue& operator=(const FDreamTextureImportQueue&) = delete;

	static FDreamTextureImportQueue& Get();

	/** 排一个导入。同一个 id 已在队列里或正在跑时什么都不做，返回 false。 */
	bool Enqueue(const FString& MaterialId);

	/** 同上，带可选项（见 FDreamTextureImportOptions）。选项只对这一轮有效，跑完即丢。 */
	bool Enqueue(const FString& MaterialId, const FDreamTextureImportOptions& Options);

	/** 排队中或正在跑。 */
	bool IsActive(const FString& MaterialId) const;

	/** 这个 id 最近一次的状态；从没排过返回 nullptr。 */
	const FDreamTextureImportStatus* FindStatus(const FString& MaterialId) const;

	/** 每次状态变化都在游戏线程上广播，包括进入 Succeeded / Failed。 */
	FOnStatusChanged OnStatusChanged;

	/** 丢掉排队中的，之后到达的回调一律忽略。模块关闭时调。 */
	void Shutdown();

private:
	// ---- 以下是实现细节，界面代码不要碰（不属于契约） ----

	/** 空闲时取队首开跑。 */
	void Pump();
	void Begin(const FString& MaterialId);

	/** 这个回调是不是还属于当前这一轮：Shutdown 过、或这个 id 已不是正在跑的那个，都算过期。 */
	bool IsCurrent(const FString& MaterialId, uint32 InGeneration) const;

	void OnManifest(const FString& MaterialId, uint32 InGeneration, bool bOK, FDreamTextureManifest&& Manifest, const FString& Error);
	void OnProgress(const FString& MaterialId, uint32 InGeneration, int32 FilesDone, int32 FilesTotal);
	void OnFetched(const FString& MaterialId, uint32 InGeneration, TSharedRef<FDreamTextureManifest> Manifest, TArray<FString> Channels, FDreamTextureFetchResult&& Fetch);
	void RunImport(const FString& MaterialId, uint32 InGeneration, TSharedRef<FDreamTextureManifest> Manifest, TArray<FString> Channels, FDreamTextureFetchResult Fetch);

	/** 取（没有就建）这个 id 的状态并重置成 Queued。同一个 id 永远是同一块内存，FindStatus 给出去的指针不会悬空。 */
	FDreamTextureImportStatus& ResetStatus(const FString& MaterialId);
	void SetStage(const FString& MaterialId, EDreamTextureImportStage Stage, const FString& Message);
	void Fail(const FString& MaterialId, const FString& Reason);
	void Finish(const FString& MaterialId);

	/** 排队中的 id，先进先出。 */
	TArray<FString> Pending;

	/** 排队中的 id 各自的选项。没登记的就是默认选项。 */
	TMap<FString, FDreamTextureImportOptions> PendingOptions;

	/** 正在跑的那一轮的选项；Begin 时从 PendingOptions 取出，Finish 时清掉。 */
	FDreamTextureImportOptions RunningOptions;

	/** 正在跑的 id；空 = 空闲。 */
	FString Running;

	/** Shutdown 一次加一。异步回调捕获排队那一刻的代数，不一致就说明队列已经被关过，直接丢弃。 */
	uint32 Generation = 0;

	/** 每个 id 最近一次的状态。用 TUniquePtr 存是为了条目不随 TMap 扩容而挪位置。 */
	TMap<FString, TUniquePtr<FDreamTextureImportStatus>> Statuses;
};
