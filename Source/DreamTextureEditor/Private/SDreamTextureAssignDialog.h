// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Input/Reply.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class FDreamTextureAssignModel;
class SWindow;

/**
 * 「赋给选中物体」的确认框内容。
 *
 * 先说「将替换 N 个材质槽」（随槽位选择实时变），说明改的是谁（关卡里的 Actor 还是资产默认材质），
 * 多槽时让用户选槽位——**不闷头全覆盖**。确认才赋值，取消什么都不动。
 *
 * 内容和窗口分开：这个控件只管内容，窗口（模态）由 DreamTextureAssignFlow::Run 建，
 * 这样自动化测试可以不弹窗地构造它。
 */
class SDreamTextureAssignDialog : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamTextureAssignDialog) {}
		SLATE_ARGUMENT(TSharedPtr<FDreamTextureAssignModel>, Model)
		/** 点了「赋值」。 */
		SLATE_EVENT(FSimpleDelegate, OnConfirm)
		/** 点了「取消」。 */
		SLATE_EVENT(FSimpleDelegate, OnCancel)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TSharedRef<SWidget> MakeSlotPicker();

	FText GetHeadline() const;
	FText GetCannotApplyHint() const;
	bool CanConfirm() const;
	EVisibility GetSlotPickerVisibility() const;
	EVisibility GetLockedVisibility() const;
	EVisibility GetAssetWarningVisibility() const;
	EVisibility GetHintVisibility() const;

	FReply HandleConfirm();
	FReply HandleCancel();

	TSharedPtr<FDreamTextureAssignModel> Model;
	FSimpleDelegate OnConfirm;
	FSimpleDelegate OnCancel;
};
