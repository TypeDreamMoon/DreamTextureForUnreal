// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DreamTextureImportQueue.h"

/**
 * 导入完成后的编辑器通知。
 *
 * ## 为什么挂在模块上，不挂在面板上
 *
 * 导入队列在 DreamTextureAssets 里，按设计「只发事件，不弹通知」。通知归界面模块，
 * 但如果由面板订阅，用户点了「导入」就把 tab 关了，导入还在后台跑，跑完却悄无声息，
 * 还以为没成功。所以订阅挂在 FDreamTextureEditorModule 上，整个编辑器生命周期内都在，
 * 面板开不开都有结果提示——后面 M2 等别处排的导入也自动有通知。
 *
 * 成功：几张新建 / 更新 / 未变，外加一个「在内容浏览器中显示」链接，跳到资产目录。
 * 失败：中文原因。
 */
class FDreamTextureImportNotifier : public TSharedFromThis<FDreamTextureImportNotifier>
{
public:
	~FDreamTextureImportNotifier();

	/** 订阅导入队列。必须已被共享指针持有（AddSP）。 */
	void Start();
	void Stop();

	/** 材质实例这一项在界面上该怎么说。通知和详情栏共用，两边的说法才对得上。 */
	struct FMaterialInstanceSummary
	{
		enum class EKind : uint8
		{
			/** 新建 / 更新 / 未变：实例存在，可以选中。 */
			Present,
			/** 设置里关了自动创建。不是故障。 */
			DisabledBySetting,
			/** 开着但没建出来：Skipped / Failed，原因在 Reason。 */
			NotCreated,
		};

		EKind Kind = EKind::NotCreated;
		EDreamTextureImportAction Action = EDreamTextureImportAction::Skipped;

		/** MI_<Slug>，Present 时有。 */
		FString AssetName;
		/** Present 时有：要在内容浏览器里选中的对象路径。 */
		FString ObjectPath;
		/** NotCreated 时的中文原因。 */
		FString Reason;

		/** 一句话，通知和详情栏直接用。如 "材质实例 MI_X_1f53 已新建"。 */
		FText Sentence;
	};

	/**
	 * 把导入结果里的材质实例翻成界面说法。bCreateEnabled 是设置里的 bCreateMaterialInstance。
	 *
	 * 关着开关时实现方返回的是 Skipped + 一条 Note；界面不能把这个说成「失败」或「没建出来」，
	 * 因为那是用户自己的选择，所以先看开关再看 Action。
	 */
	static FMaterialInstanceSummary DescribeMaterialInstance(const FDreamTextureImportResult& Result, bool bCreateEnabled);

	/** 一条要弹的通知。拆成纯数据，是为了自动化测试能不依赖 Slate 直接断言文案。 */
	struct FNoticeContent
	{
		FText Message;
		bool bIsError = false;

		/** 成功且有资产目录时非空：没有可选中的材质实例时，链接跳去的内容浏览器路径。 */
		FString FolderToShow;

		/** 有材质实例时非空：链接优先选中它（不只是跳到目录）。 */
		FString AssetToSelect;

		/** 导入成功且这套材质的产出不可商用（manifest.license_flags.commercial_use=false）：消息末尾已带一行说明。 */
		bool bNonCommercial = false;
	};

	/**
	 * 许可这一行在界面上怎么说。只有 manifest 明说了不可商用才返回非空
	 * （「没写」不等于「不能商用」，也不等于「可以商用」，通知里不替它下结论，所以也返回空）。
	 * 如 "授权：不可商用（CHORD: Ubisoft ML License (research-only)）"。
	 */
	static FText DescribeLicense(const FDreamTextureLicense& License);

	/** 把一个已结束的状态翻成通知内容。未结束的状态不该传进来。 */
	static FNoticeContent Describe(const FDreamTextureImportStatus& Status);
	/** 同上，但开关由调用方给（自动化测试用，不依赖当前工程设置）。 */
	static FNoticeContent Describe(const FDreamTextureImportStatus& Status, bool bCreateMaterialInstance);

	/** 在内容浏览器里选中这个资产（对象路径）。找不到时退回跳到 FallbackFolder。 */
	static void ShowAssetInContentBrowser(const FString& ObjectPath, const FString& FallbackFolder);

	/** 内容浏览器里显示这个目录（如 /Game/DreamTexture/MossyRiverRocks_1f53）。 */
	static void ShowFolderInContentBrowser(const FString& Folder);

	/** 弹出通知。没有 Slate（命令行/测试）时什么都不做。 */
	static void Show(const FNoticeContent& Content);

private:
	void HandleStatus(const FDreamTextureImportStatus& Status);

	FDelegateHandle Handle;

	/** 已经通知过的 id。状态一回到未结束就清掉，所以同一个 id 再导一次会再通知。 */
	TSet<FString> Notified;
};
