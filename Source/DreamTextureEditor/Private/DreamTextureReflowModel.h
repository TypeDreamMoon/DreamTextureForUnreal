// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DreamTextureMaterialAnalysis.h"
#include "DreamTextureTextureExporter.h"

class UMaterialInterface;

/** 选区里的一个候选：内容浏览器里的材质，或关卡里某个 Actor 的一个材质槽。 */
struct FDreamTextureReflowCandidate
{
	enum class EKind : uint8
	{
		Asset,
		ActorSlot,
	};

	EKind Kind = EKind::Asset;

	/** 给人看的名字。 */
	FString Label;

	/** 同一个候选在两次取选择之间保持不变的键，用来在刷新后认出「还是刚才选的那个」。 */
	FString Key;

	TWeakObjectPtr<UMaterialInterface> Material;
	FString MaterialPath;

	/** 槽位是空的（没有材质）。 */
	bool IsEmptySlot() const { return Kind == EKind::ActorSlot && MaterialPath.IsEmpty(); }
};

/** 结果表里的一行。只记贴图路径，内容每次从模型现取，这样重新解析之后列表控件的行不用重建。 */
struct FDreamTextureReflowRowItem
{
	FString TexturePath;
};

using FDreamTextureReflowRowItemPtr = TSharedPtr<FDreamTextureReflowRowItem>;

/**
 * 回流页依赖的外部能力。默认转发到真实实现；自动化测试换成假的，
 * 这样不用真的选中东西、不用造材质、不用写磁盘。
 */
struct FDreamTextureReflowBackend
{
	/** 内容浏览器里选中的材质/材质实例。 */
	TFunction<TArray<UMaterialInterface*>()> GetSelectedMaterials;

	/** 关卡里选中的 Actor 的材质槽（已展平，标签里带 Actor 名）。 */
	TFunction<TArray<FDreamTextureMaterialSlot>()> GetSelectedActorSlots;

	TFunction<FDreamTextureMaterialAnalysis(UMaterialInterface*, const FDreamTextureAnalyzeOptions&)> Analyze;
	TFunction<TArray<FDreamTextureExportedImage>(const FDreamTextureMaterialAnalysis&, const FDreamTextureExportOptions&)> Export;
	TFunction<FString()> MakeTempDir;

	/** 用户指认的库。nullptr = 默认库（工程 Saved 下）。 */
	FDreamTextureOverrideStore* Overrides = nullptr;

	static FDreamTextureReflowBackend MakeDefault();
};

/**
 * 回流页的状态：当前选区里的候选、选中的那个的解析结果、用户的指认、导出结果。
 *
 * 为什么单拎成不含 Slate 的类：这页的分支很多（没选东西、空槽、解析失败、没有贴图、有些贴图导不出…），
 * 全塞在控件里没法测。拎出来之后自动化测试注入假数据逐个分支验证。
 *
 * ## M8 在这上面接着做
 *
 * 补齐通道、改写外观都是「对当前解析结果里的某几张贴图做事」：读 GetAnalysis() 拿每张贴图的角色与排布，
 * 读 FindExport() 拿它们导出的 PNG 与哈希（写 manifest 的 derived_from），做完后调 Reanalyze() 刷新。
 * 界面上给它们留了位置，见 SDreamTextureReflowPanel::GetActionsHost()。
 *
 * 所有委托都在游戏线程上广播。
 */
class FDreamTextureReflowModel : public TSharedFromThis<FDreamTextureReflowModel>
{
public:
	explicit FDreamTextureReflowModel(FDreamTextureReflowBackend InBackend = FDreamTextureReflowBackend::MakeDefault());

	DECLARE_MULTICAST_DELEGATE(FOnChanged);
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnNotice, const FText& /*Message*/, bool /*bIsError*/);

	/** 候选、选中项、解析结果、导出结果任一变化。 */
	FOnChanged OnChanged;

	/** 要提示用户的一句话。模型自己不碰 Slate，由面板弹通知。 */
	FOnNotice OnNotice;

	/** 页面此刻该显示什么。公开给自动化测试断言分支。 */
	enum class EViewState : uint8
	{
		/** 没取到任何东西（或还没取过）。 */
		NoSelection,
		/** 选区里有多个候选，还没挑一个。 */
		PickOne,
		/** 挑中的是个空的材质槽。 */
		EmptySlot,
		/** 解析失败（材质被卸载、父链断了…）。 */
		AnalysisFailed,
		/** 解析成功但材质没用到任何贴图。 */
		NoTextures,
		/** 有结果可看。 */
		Ready,
	};

	//~ 选区
	/** 从内容浏览器与关卡选区重新取候选。只有一个候选时自动选中并解析；之前选的还在就保持。 */
	void RefreshSelection();
	const TArray<FDreamTextureReflowCandidate>& GetCandidates() const { return Candidates; }
	int32 GetSelectedIndex() const { return SelectedIndex; }
	const FDreamTextureReflowCandidate* GetSelectedCandidate() const;
	void SelectCandidate(int32 Index);

	//~ 解析
	/** 重新解析当前选中的候选。用户指认、材质图改了之后都调它。导出结果保留（贴图没变，文件还在）。 */
	void Reanalyze();
	EViewState GetViewState() const;
	/** 非 Ready 状态下给人看的说明（中文）。 */
	FText GetStatusText() const;
	const FDreamTextureMaterialAnalysis& GetAnalysis() const { return Analysis; }
	const TArray<FDreamTextureReflowRowItemPtr>& GetRows() const { return Rows; }
	const FDreamTextureMaterialTexture* FindTexture(const FString& TexturePath) const;

	/**
	 * 用户指认与「平铺 / UV 展开」记忆共用的库：注入的（测试）或默认库（工程 Saved 下）。
	 * 动作区按它记每个材质是平铺还是 UV 展开。
	 */
	FDreamTextureOverrideStore& GetOverrideStore() const;

	//~ 用户指认
	/** 把一张贴图指认为某个角色并记住。Packed 保留当前排布。 */
	void SetRole(const FString& TexturePath, EDreamTextureRole Role);
	/** 改打包图某个通道（0..3 = R G B A）的含义；贴图不是 Packed 时先把它指认为 Packed。 */
	void SetLayoutChannel(const FString& TexturePath, int32 ChannelIndex, EDreamTextureRole Role);
	/** 撤销指认，回到自动推断。 */
	void ClearOverride(const FString& TexturePath);

	//~ 导出
	/** 长边上限；0 = 不缩。 */
	void SetExportMaxEdge(int32 InMaxEdge);
	int32 GetExportMaxEdge() const { return ExportMaxEdge; }
	/** 解析成功且至少一张贴图能导。 */
	bool CanExport() const;
	int32 GetExportableCount() const;
	/** 把能导的导到一个新的临时目录。 */
	void Export();
	const FDreamTextureExportedImage* FindExport(const FString& TexturePath) const;
	const FString& GetExportDir() const { return ExportDir; }
	bool HasExported() const { return !ExportDir.IsEmpty(); }

	//~ 文案（纯函数，界面与测试共用）
	static FText RoleDisplayName(EDreamTextureRole Role);
	/** 打包图单个通道里的内容：Unknown 显示「？」，Other 显示「未用」。 */
	static FText ChannelContentDisplayName(EDreamTextureRole Role);
	static FText EvidenceDisplayName(EDreamTextureEvidence Evidence);
	static FText SinkDisplayName(EDreamTextureSink Sink);
	/** "BaseColor ← RGB；Roughness ← G"：这张贴图在材质里的用法。 */
	static FText DescribeUses(const FDreamTextureMaterialTexture& Row);

private:
	void SetOverride(const FString& TexturePath, const FDreamTextureRoleOverride& Override);
	void RebuildRows();
	void Notify(const FText& Message, bool bIsError);

	FDreamTextureReflowBackend Backend;

	TArray<FDreamTextureReflowCandidate> Candidates;
	int32 SelectedIndex = INDEX_NONE;

	FDreamTextureMaterialAnalysis Analysis;
	bool bHasAnalysis = false;
	TArray<FDreamTextureReflowRowItemPtr> Rows;

	TMap<FString, FDreamTextureExportedImage> Exports;
	FString ExportDir;
	int32 ExportMaxEdge = 0;
};
