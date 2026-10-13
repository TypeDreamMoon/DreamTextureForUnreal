// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateColor.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class FDreamTextureConnection;
struct FSlateBrush;

/**
 * 面板顶部常驻的一条紧凑连接状态：后端 / 长连接 / ComfyUI 三个小标记。
 *
 * 为什么常驻：素材库页是默认页，用户多数时间不会切到「连接状态」。可只要后端或
 * ComfyUI 出了问题，列表为空、导入失败这些现象都得回头对照连接状态才能看懂，
 * 所以三级状态必须一直在眼前，而且仍然分三个标记，不混成一个灯——ComfyUI 起不来
 * 的时候后端照常应答，混在一起会让人去重启错误的东西。
 *
 * 每个标记 = 形状不同的图标 + 一个中文状态词，颜色只是第三重冗余。
 *
 * 不缓存、不订阅：三个标记的取值函数直接读连接上的只读 getter（都是返回成员的
 * 廉价调用），每帧读一次。这样没有委托句柄要摘，也不会出现缓存和连接对不上。
 * 连接只持弱引用，面板不该延长它的寿命。
 */
class SDreamTextureConnectionStrip : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamTextureConnectionStrip) {}
		/** 点任一标记。外壳据此切到「连接状态」页。 */
		SLATE_EVENT(FSimpleDelegate, OnClicked)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** 和 M0 状态页同一套分档，但那边的枚举是私有的，这里独立一份。 */
	enum class ETone : uint8
	{
		Ok,
		Busy,
		Warn,
		Bad,
		Unknown,
	};

	/** 一个标记此刻要显示的内容。 */
	struct FMarker
	{
		ETone Tone = ETone::Unknown;
		FText Value;
		FText Tooltip;
	};

	/** 三个标记的取值。公开成静态函数是为了自动化测试能直接喂假连接状态验证分档。 */
	static FMarker EvaluateBackend(const TSharedPtr<FDreamTextureConnection>& Conn);
	static FMarker EvaluateLink(const TSharedPtr<FDreamTextureConnection>& Conn);
	static FMarker EvaluateComfy(const TSharedPtr<FDreamTextureConnection>& Conn);

	static const FSlateBrush* ToneBrush(ETone InTone);
	static FSlateColor ToneColor(ETone InTone);

private:
	enum class EWhich : uint8 { Backend, Link, Comfy };

	TSharedRef<SWidget> MakeMarker(const FText& InTitle, EWhich InWhich);
	FMarker Evaluate(EWhich InWhich) const;
	TSharedPtr<FDreamTextureConnection> GetConnection() const;

	FSimpleDelegate OnClickedDelegate;
	TWeakPtr<FDreamTextureConnection> ConnectionWeak;
};
