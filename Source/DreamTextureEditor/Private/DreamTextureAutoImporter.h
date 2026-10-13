// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DreamTextureJobTracker.h"

/**
 * 生成完成后自动导入。
 *
 * ## 为什么挂在模块上，不挂在生成页上
 *
 * 一次生成动辄几分钟，用户提交完常常就把面板关了去干别的。导入的触发如果由页面订阅，
 * tab 一关就没人管了，回来发现材质生成好了却没进工程，还以为没成功。
 * 所以和导入通知（FDreamTextureImportNotifier）一样挂在 FDreamTextureEditorModule 上，
 * 整个编辑器生命周期内都在，页面开不开都一样。
 *
 * ## 规矩
 *
 * - 只认生成页提交的任务（追踪器里的来源标签）。回流等别的提交方有自己的去处，不归这里管。
 * - **完成的那一刻**看一次开关，之后不追认：开关关着时完成的任务，用户事后把开关打开，
 *   不会把之前攒下的一批全导进工程。
 * - 每个任务最多触发一次。追踪器可能为同一个终态广播不止一次（登记时并入暂存事件、补状态…）。
 */
class FDreamTextureAutoImporter : public TSharedFromThis<FDreamTextureAutoImporter>
{
public:
	struct FHooks
	{
		/** 现在开关开着吗。 */
		TFunction<bool()> IsEnabled;

		/** 排一个导入（FDreamTextureImportQueue::Enqueue）。 */
		TFunction<bool(const FString& /*MaterialId*/)> Enqueue;

		static FHooks MakeDefault();
	};

	explicit FDreamTextureAutoImporter(FHooks InHooks = FHooks::MakeDefault());
	~FDreamTextureAutoImporter();

	/** 订阅追踪器。必须已被共享指针持有（AddSP）。重复调用先解旧的。 */
	void Start(const TSharedPtr<FDreamTextureJobTracker>& InTracker);
	void Stop();

	/** 追踪器报来的任务变化。公开是为了自动化测试直接喂数据。 */
	void HandleJobChanged(const FDreamTextureJob& Job);

	/** 已经自动排过导入的任务数（测试与诊断用）。 */
	int32 GetNumTriggered() const { return NumTriggered; }

private:
	FHooks Hooks;
	TWeakPtr<FDreamTextureJobTracker> Tracker;
	FDelegateHandle Handle;

	/** 已经处理过的任务 id（无论当时开关开没开），防止重复触发。 */
	TSet<FString> Handled;
	int32 NumTriggered = 0;
};
