// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DreamTexturePushSession.h"

/**
 * 收下 Web 端推来的材质：排进导入队列，再弹一条「Web 端发来…」的通知。
 *
 * ## 为什么挂在模块上，不挂在面板上
 *
 * 和导入通知、自动导入同一个理由：用户把面板关了，Web 端照样会推过来，材质不能因为没开面板就丢。
 * 所以订阅挂在 FDreamTextureEditorModule 上，整个编辑器生命周期内都在。
 *
 * ## 推送通知和导入完成通知怎么分工
 *
 * 导入完成的通知（FDreamTextureImportNotifier）本来就有，而且是订阅导入队列的，推送排进去的导入走的
 * 是同一条路径，所以完成 / 失败时它自然会弹——这里**不重复弹结果**。
 * 这里只说「收到了」：用户此刻在 Web 页面上点了按钮，转头看编辑器，得有个反应，
 * 否则要等下载 + 落资产十几秒后才有动静，会以为没发成功。两条通知一个说开始、一个说结果，内容不重叠；
 * 开始这条做得短（几秒就收）、不带图标和按钮，免得和结果那条叠在一起显得吵。
 *
 * 同一套材质已经在导入队列里时（用户在 Web 上连点了两下），不重复排——导入本来就在进行，通知照说「正在导入」就是实话。
 * 只有真的排不进去（队列拒了）才改口说失败。
 */
class FDreamTexturePushReceiver : public TSharedFromThis<FDreamTexturePushReceiver>
{
public:
	struct FHooks
	{
		/** 这套材质现在排着队或正在导（FDreamTextureImportQueue::IsActive）。 */
		TFunction<bool(const FString& /*MaterialId*/)> IsActive;

		/** 排一个导入（FDreamTextureImportQueue::Enqueue）。返回 false 表示没排进去。 */
		TFunction<bool(const FString& /*MaterialId*/)> Enqueue;

		/** 弹一条通知。没有 Slate 时什么都不做。 */
		TFunction<void(const FText&)> ShowNotice;

		static FHooks MakeDefault();
	};

	explicit FDreamTexturePushReceiver(FHooks InHooks = FHooks::MakeDefault());
	~FDreamTexturePushReceiver();

	/** 订阅会话的 OnImportPushed。必须已被共享指针持有（AddSP）。重复调用先解旧的。 */
	void Start(const TSharedPtr<FDreamTexturePushSession>& InSession);
	void Stop();

	/** 收到一套材质。公开是为了自动化测试直接喂数据。 */
	void HandlePushed(const FString& MaterialId);

	/** 通知文案。拆出来是为了测试能不依赖 Slate 断言。bImporting：true=导入正在进行（这次新排的，或本来就在队里）；false=没能排进去。 */
	static FText Describe(const FString& MaterialId, bool bImporting);

	/** 收到过多少次推送（测试与诊断用）。 */
	int32 GetNumReceived() const { return NumReceived; }

	/** 真的弹一条这类通知：短、无图标、无按钮。没有 Slate 时什么都不做。 */
	static void Show(const FText& Message);

private:
	FHooks Hooks;
	TWeakPtr<FDreamTexturePushSession> Session;
	FDelegateHandle Handle;
	int32 NumReceived = 0;
};
