// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DreamTexturePushSession.h"
#include "DreamTextureTypes.h"
#include "Input/Reply.h"
#include "Styling/SlateColor.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class FDreamTextureConnection;
class SBox;
struct FSlateBrush;

/**
 * DreamTexture 面板（M0）：只做「一眼看清连接状态」这一件事。
 *
 * ## 为什么把状态拆成三行，而不是一个灯
 *
 * 后端进程、WS 长连接、ComfyUI 是三件互相独立的事，尤其是最后一件：
 * ComfyUI 起不来的时候后端仍然能启动、能应答 REST。混成一个灯的话，
 * 用户看见「连不上」的第一反应是去重启后端——而该重启的是 ComfyUI，
 * 重启后端只会把问题原样留在那儿。所以三行分开，各自带各自的原因。
 *
 * 「启动中」还得从「未就绪」里再单拎出来：ComfyUI 冷启动几十秒到几分钟是
 * 常态（自定义节点首次运行还要现装依赖）。显示成「未连接」会让人去点重启，
 * 而重启只是把这几十秒重来一遍。
 *
 * ## 不轮询
 *
 * 状态全由 FDreamTextureConnection 的 OnLinkChanged / OnHealthChanged 推过来，
 * 面板只在回调里刷新几个缓存字段，Slate 的 TAttribute 读缓存，不去碰连接对象。
 * 委托句柄存成员里，析构必须摘掉——这是个 nomad tab，会被反复开关，
 * 漏摘一次留下的就是个悬空 this。
 */
class SDreamTextureStatusPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamTextureStatusPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SDreamTextureStatusPanel() override;

private:
	/**
	 * 一行状态的视觉分档。
	 *
	 * 每一档绑一个**形状不同**的图标（圆形对勾 / 三角警告 / 转圈 / 圆形信息）
	 * 外加一句中文，颜色只是第三重冗余——只靠颜色区分对色觉障碍不友好，
	 * 而且用户发过来的截图被压过之后，红绿经常就分不出来了。
	 */
	enum class EStatusTone : uint8
	{
		/** 好了。圆形对勾。 */
		Ok,
		/** 在路上，等着就行，别动手。转圈。 */
		Busy,
		/** 能用但不对劲，或者需要人来处理。三角。 */
		Warn,
		/** 坏了。红三角。 */
		Bad,
		/** 没有依据可以判断，别瞎猜。圆形信息。 */
		Unknown,
	};

	//~ 连接的订阅与解绑
	/** 取连接并订阅两个委托。连接为空（客户端模块未就绪）时安静返回，界面自己会说明。 */
	void BindConnection();
	void UnbindConnection();
	void HandleLinkChanged(EDreamTextureLink NewLink);
	void HandleHealthChanged(const FDreamTextureHealth& InHealth);
	/** 把连接上的四个只读字段抄进缓存。回调里和刚打开面板时各调一次。 */
	void RefreshFromConnection();

	//~ 推送会话（M6）的订阅与缓存。和连接分开订阅：它是另一个对象，状态变化的时机也不同。
	void BindPush();
	void UnbindPush();
	void HandlePushChanged();
	void RefreshFromPush();

	//~ 构件
	TSharedRef<SWidget> MakeToneIcon(TAttribute<EStatusTone> InTone);
	TSharedRef<SWidget> MakeStatusRow(const FText& InTitle, TAttribute<EStatusTone> InTone,
		TAttribute<FText> InValue, TAttribute<FText> InNote);
	TSharedRef<SWidget> MakeToolbar();
	TSharedRef<SWidget> BuildChecksContent() const;
	void RebuildChecks();

	//~ 三行状态的取值（都是 const，TAttribute::CreateSP 只接受 const 成员函数）
	EStatusTone GetBackendTone() const;
	FText GetBackendText() const;
	FText GetBackendNote() const;

	EStatusTone GetLinkTone() const;
	FText GetLinkText() const;
	FText GetLinkNote() const;

	EStatusTone GetComfyTone() const;
	FText GetComfyText() const;
	FText GetComfyNote() const;

	EStatusTone GetPushTone() const;
	FText GetPushText() const;
	FText GetPushNote() const;
	/** 不管新鲜不新鲜，把 Health 翻成一句人话。失联时拿它填「最后一次是……」。 */
	FText GetComfyStateText() const;
	/**
	 * 手上这份 Health 还算不算数。
	 *
	 * Health 是 WS 的 comfy.status 推过来的，链路一 Stale 它就是过期数据。
	 * Web 端实测把后端进程杀干净，页面照旧显示「ComfyUI 就绪、显存 14.8G」——
	 * 一份看起来很确定的过期数字比空白危险得多，所以这里宁可退回「未知」。
	 */
	bool IsHealthFresh() const;

	//~ 其它显示项
	FText GetBackendUrlText() const;
	FText GetDetailText() const;
	EVisibility GetDetailVisibility() const;
	FText GetErrorText() const;
	/** 要显示的错误：插件启动后端的失败优先，其次是连接的 LastError。 */
	FString GetShownError() const;
	EVisibility GetErrorVisibility() const;
	FText GetChecksButtonText() const;
	bool IsChecksButtonEnabled() const;
	bool HasConnection() const;

	//~ 按钮
	FReply OnReconnectClicked();
	FReply OnOpenWebClicked();
	FReply OnStartBackendClicked();
	FReply OnStopBackendClicked();
	bool IsStartEnabled() const;
	FText GetStartTooltip() const;
	FText GetStartButtonText() const;
	EVisibility GetStopVisibility() const;
	FReply OnSettingsClicked();
	FReply OnRunChecksClicked();

	//~ 分档 → 画笔 / 颜色
	static const FSlateBrush* ToneBrush(EStatusTone InTone);
	static FSlateColor ToneColor(EStatusTone InTone);
	/** /api/checks 的 "ok|warn|fail" 映射到分档。认不出来的一律 Unknown，不猜。 */
	static EStatusTone ToneFromCheckStatus(const FString& InStatus);

	/** 弱引用：连接归客户端模块所有，面板只是个观察者，不该延长它的寿命。 */
	TWeakPtr<FDreamTextureConnection> ConnectionWeak;
	FDelegateHandle LinkChangedHandle;
	FDelegateHandle HealthChangedHandle;

	/** 推送会话归客户端模块所有，面板只观察。 */
	TWeakPtr<FDreamTexturePushSession> PushWeak;
	FDelegateHandle PushChangedHandle;
	EDreamTexturePushState CachedPushState = EDreamTexturePushState::Unregistered;
	FString CachedPushName;
	FString CachedPushId;
	FString CachedPushError;

	//~ 缓存，全部由回调更新
	EDreamTextureLink CachedLink = EDreamTextureLink::Offline;
	FDreamTextureHealth CachedHealth;
	bool bCachedBackendReachable = false;
	FString CachedLastError;

	/**
	 * 有没有拿到过任何一次状态。
	 *
	 * 面板刚打开、连接还什么都没发生过的时候 Link 就是 Offline，
	 * 直接显示「未连接」会让人以为坏了——其实只是还没开始。没拿到过就一律说
	 * 「连接中」。
	 */
	bool bStateKnown = false;

	//~ 环境自检
	/** 结果区的宿主。结果是异步来的，直接换掉 SBox 的内容比增删 slot 干净。 */
	TSharedPtr<SBox> ChecksHost;
	FDreamTextureChecksResult ChecksResult;
	FString ChecksError;
	bool bChecksInFlight = false;
	bool bHasChecks = false;
};
