// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

/**
 * nomad tab 的外壳：顶部一条常驻连接状态 + 页签，下面是几页。
 *
 *   ┌────────────────────────────────────────────────────────┐
 *   │ 后端 ✓已应答  长连接 ✓已连接  ComfyUI ✓就绪   [素材库｜生成｜回流｜连接状态] │
 *   ├────────────────────────────────────────────────────────┤
 *   │ 当前页                                                  │
 *   └────────────────────────────────────────────────────────┘
 *
 * 各页都在 Construct 里建好、用 SWidgetSwitcher 切换，不是切页时再建：
 * 素材库页里有进行中的请求和缩略图缓存，生成页里有没写完的提示词和进行中的变体，
 * 切到别的页再切回来不该丢掉它们。
 */
class SDreamTextureShell : public SCompoundWidget
{
public:
	enum class EPage : uint8
	{
		/** 默认页。 */
		Library,
		/** M0 的 SDreamTextureStatusPanel，原样保留。 */
		Status,
		/**
		 * M4 的生成页。枚举值接在后面而不是插在中间：它同时是 SWidgetSwitcher 的槽位下标，
		 * 插在中间会让别人已经按下标写死的地方悄悄错位。页签的先后顺序由 SSegmentedControl 的槽位声明顺序决定，与枚举值无关。
		 */
		Generate,
		/**
		 * M7 的回流页：选一个工程里已有的材质，解析它的贴图、认出各是什么通道、导出 PNG。
		 * 同样接在最后（槽位下标 = 枚举值）。
		 */
		Reflow,
	};

	SLATE_BEGIN_ARGS(SDreamTextureShell) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	void SetPage(EPage InPage) { Page = InPage; }
	EPage GetPage() const { return Page; }

private:
	int32 GetPageIndex() const { return static_cast<int32>(Page); }
	void OnStripClicked();

	EPage Page = EPage::Library;
};
