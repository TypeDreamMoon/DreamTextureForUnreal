// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureImporter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "DreamTextureLog.h"
#include "DreamTextureSettings.h"
#include "Engine/Texture2D.h"
#include "FileHelpers.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "TextureImportSettings.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"

namespace
{
	/**
	 * 通道规则表。数据照 manifest-v1.md 的「UE 导入映射」：
	 *
	 *   basecolor            Default (TC_Default)   sRGB 开
	 *   normal               Normalmap              关
	 *   orm                  Masks                  关
	 *   roughness/metallic/ao/height   Grayscale    关
	 *
	 * 灰度图选 TC_Grayscale 而不是 TC_Masks：它们是单通道数据，G8/G16 一份就够，
	 * 用 Masks 会白占 BC1 的三个通道，16 位高度图还会被压成 8 位。
	 */
	const TArray<FDreamTextureChannelRule>& GetRules()
	{
		static const TArray<FDreamTextureChannelRule> Rules = []()
		{
			auto Make = [](const FString& Channel, const TCHAR* Suffix, TextureCompressionSettings Compression, TextureGroup LODGroup, bool bSRGB)
			{
				FDreamTextureChannelRule Rule;
				Rule.Channel = Channel;
				Rule.Suffix = Suffix;
				Rule.Compression = Compression;
				Rule.LODGroup = LODGroup;
				Rule.bDefaultSRGB = bSRGB;
				return Rule;
			};

			TArray<FDreamTextureChannelRule> Out;
			Out.Add(Make(DreamTextureChannel::BaseColor, TEXT("BC"), TC_Default, TEXTUREGROUP_World, true));
			Out.Add(Make(DreamTextureChannel::Normal, TEXT("N"), TC_Normalmap, TEXTUREGROUP_WorldNormalMap, false));
			Out.Add(Make(DreamTextureChannel::ORM, TEXT("ORM"), TC_Masks, TEXTUREGROUP_World, false));
			Out.Add(Make(DreamTextureChannel::Height, TEXT("H"), TC_Grayscale, TEXTUREGROUP_World, false));
			Out.Add(Make(DreamTextureChannel::Roughness, TEXT("R"), TC_Grayscale, TEXTUREGROUP_World, false));
			Out.Add(Make(DreamTextureChannel::Metallic, TEXT("M"), TC_Grayscale, TEXTUREGROUP_World, false));
			Out.Add(Make(DreamTextureChannel::AO, TEXT("AO"), TC_Grayscale, TEXTUREGROUP_World, false));
			return Out;
		}();
		return Rules;
	}

	/** 资产名里的名字部分最长多少个字符。包路径在 Windows 上有 260 的总长限制，名字不能无限长。 */
	constexpr int32 MaxNameChars = 32;

	/** 名字一个 ASCII 字母数字都剩不下时，整段 id 当 slug 用，最长保留多少个字符（只是防病态的超长 id 撑爆路径）。 */
	constexpr int32 MaxIdChars = 48;

	/** id 尾段最长多少个字符。正常的尾段只有 4~8 位；防的是没有下划线的整段 id 被原样拼进路径。 */
	constexpr int32 MaxIdTailChars = 32;

	/**
	 * 只认 ASCII 字母数字。不能用 FChar::IsAlnum：它走 C 运行时的 iswalnum，
	 * 在中文代码页下会把汉字也判成字母——资产名里混进汉字，换台机器就是乱码。
	 */
	bool IsAsciiAlnum(TCHAR C)
	{
		return (C >= TEXT('0') && C <= TEXT('9')) || (C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('A') && C <= TEXT('Z'));
	}

	FString ToAsciiAlnum(const FString& In)
	{
		FString Out;
		for (const TCHAR C : In)
		{
			if (IsAsciiAlnum(C))
			{
				Out.AppendChar(C);
			}
		}
		return Out;
	}

	/** 内容根目录归一：去空白、反斜杠换正斜杠、去尾斜杠、补前导斜杠。空串不处理，由调用方回退。 */
	FString NormalizeRoot(FString Root)
	{
		Root.TrimStartAndEndInline();
		Root.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Root.RemoveFromEnd(TEXT("/")))
		{
		}
		if (!Root.IsEmpty() && !Root.StartsWith(TEXT("/")))
		{
			Root.InsertAt(0, TEXT('/'));
		}
		return Root;
	}

	/** 传进来的根为空就取设置里的 ImportRoot；设置也是空（被人清掉了）再退回出厂默认。 */
	FString ResolveContentRoot(const FString& ContentRoot)
	{
		FString Root = NormalizeRoot(ContentRoot);
		if (Root.IsEmpty())
		{
			Root = NormalizeRoot(UDreamTextureSettings::Get().ImportRoot);
		}
		if (Root.IsEmpty())
		{
			Root = TEXT("/Game/DreamTexture");
		}
		return Root;
	}

	/**
	 * 来源记录的键。记在包的元数据（UPackage::GetMetaData）里，不在资产的任何 UPROPERTY 里。
	 *
	 * 为什么选它：cook 出去的资产不能引用只存在于编辑器模块里的类。包元数据是
	 * FMetaData（CoreUObject 里的普通 TMap<FName, FString>），WITH_METADATA 等于
	 * WITH_EDITORONLY_DATA——不带编辑器数据的构建里这套 API 连声明都没有；
	 * 而且存包时 cook 路径根本不收割它（SavePackage2 里 `if (!SaveContext.IsCooking())`）。
	 * 换成自定义的 UAssetUserData 子类，类定义就得放进本模块，cook 出去的贴图会引用到它。
	 */
	namespace SourceKey
	{
		const TCHAR* const MaterialId = TEXT("DreamTexture.MaterialId");
		const TCHAR* const Channel = TEXT("DreamTexture.Channel");
		const TCHAR* const Sha256 = TEXT("DreamTexture.Sha256");
	}

	/** 通道的固定处理顺序：先主要的几张，让结果列表读起来稳定。其余未知通道排在后面按字母序。 */
	const TArray<FString>& GetChannelOrder()
	{
		static const TArray<FString> Order = {
			DreamTextureChannel::BaseColor, DreamTextureChannel::Normal, DreamTextureChannel::ORM,
			DreamTextureChannel::Height, DreamTextureChannel::Roughness, DreamTextureChannel::Metallic, DreamTextureChannel::AO,
		};
		return Order;
	}

	/** 哈希比较忽略大小写（后端写小写，但人手改 manifest 时不该因此重导）。两边都得非空才算一致。 */
	bool HashesMatch(const FString& A, const FString& B)
	{
		return !A.IsEmpty() && !B.IsEmpty() && A.Equals(B, ESearchCase::IgnoreCase);
	}

	/**
	 * 找已有的资产：内存里有就直接用，否则磁盘上有这个包就加载进来，都没有返回 nullptr。
	 * 来源记录在包元数据里，不加载包就读不到，所以去重必须走加载。
	 */
	UObject* FindExistingAsset(const FString& PackageName, const FString& AssetName)
	{
		const FString ObjectPath = PackageName + TEXT(".") + AssetName;
		if (UObject* InMemory = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return InMemory;
		}
		if (FPackageName::DoesPackageExist(PackageName))
		{
			return LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		return nullptr;
	}

	void WriteSourceRecord(UObject* Texture, const FString& MaterialId, const FString& Channel, const FString& Sha256)
	{
#if WITH_METADATA
		FMetaData& MetaData = Texture->GetOutermost()->GetMetaData();
		MetaData.SetValue(Texture, SourceKey::MaterialId, *MaterialId);
		MetaData.SetValue(Texture, SourceKey::Channel, *Channel);
		MetaData.SetValue(Texture, SourceKey::Sha256, *Sha256);
#endif
	}

	/** 读 PNG。保留文件原本的位深和通道数：灰度 8 位是 G8，灰度 16 位是 G16，不替调用方降级。 */
	bool DecodeImageFile(const FString& Path, FImage& OutImage, FString& OutError)
	{
		TArray64<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path))
		{
			OutError = FText::Format(NSLOCTEXT("DreamTextureImporter", "ReadFailed", "Could not read the file: {0}"), FText::FromString(Path)).ToString();
			return false;
		}
		if (!FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), OutImage) || OutImage.SizeX <= 0 || OutImage.SizeY <= 0)
		{
			OutError = FText::Format(NSLOCTEXT("DreamTextureImporter", "DecodeFailed", "Could not decode the file; it is not a valid image: {0}"), FText::FromString(Path)).ToString();
			return false;
		}
		return true;
	}

	/**
	 * 把通道规则落到贴图上。新建与就地更新共用这一处，两条路径不会各改各的。
	 *
	 * 更新时也重新套一遍这几项（压缩、sRGB、LODGroup、翻 G）：它们决定这张图能不能被正确采样，
	 * 比如法线必须是 TC_Normalmap；用户随手改过的其它设置（过滤、LOD 偏移…）不碰。
	 */
	void ApplyChannelSettings(UTexture2D* Texture, const FDreamTextureChannelRule& Rule, const FDreamTextureMapInfo& Map)
	{
		Texture->CompressionSettings = Rule.Compression;
		Texture->LODGroup = Rule.LODGroup;

		switch (Map.ColorSpace)
		{
		case EDreamTextureColorSpace::SRGB:
			Texture->SRGB = true;
			break;
		case EDreamTextureColorSpace::Linear:
			Texture->SRGB = false;
			break;
		default:
			Texture->SRGB = Rule.bDefaultSRGB;
			break;
		}
	}

	/**
	 * 法线的 G 通道：**不翻**，除非 manifest 明说是 opengl。
	 *
	 * manifest-v1 里两条管线统一输出 directx（Y 向下，UE 原生），再翻一次就成了反向法线——
	 * 凹凸整个颠倒，而贴图本身看着毫无异常，排查起来极费劲。迟早有人觉得「法线导进 UE 都要翻 G」
	 * 然后顺手加一行，所以这里**显式赋值**而不是依赖默认值，并且有测试守着。
	 * 非法线通道一律为假。
	 */
	void ApplyFlipGreenChannel(UTexture2D* Texture, const FDreamTextureChannelRule& Rule, const FDreamTextureMapInfo& Map)
	{
		const bool bIsNormal = Rule.Channel.Equals(DreamTextureChannel::Normal, ESearchCase::CaseSensitive);
		const bool bOpenGL = Map.NormalY.Equals(TEXT("opengl"), ESearchCase::IgnoreCase);
		Texture->bFlipGreenChannel = (bIsNormal && bOpenGL);
	}

	/** 一次 Import 里的共享上下文。 */
	struct FImportContext
	{
		const FDreamTextureImportRequest& Request;
		FString Slug;
		FString Folder;
		/** 需要存盘的包（新建与更新过的）。 */
		TArray<UPackage*> PackagesToSave;
		/** PackagesToSave 里每个包对应的结果下标，保存失败时回头改状态用。 */
		TArray<int32> ResultIndexForPackage;
	};

	/** 处理一个通道。返回的结果里 Action 一定被设置过。 */
	FDreamTextureImportedTexture ImportChannel(FImportContext& Context, const FString& Channel, int32 ResultIndex)
	{
		const FDreamTextureManifest& Manifest = Context.Request.Manifest;

		FDreamTextureImportedTexture Out;
		Out.Channel = Channel;
		Out.Action = EDreamTextureImportAction::Skipped;

		const FDreamTextureChannelRule* Rule = FDreamTextureChannelRule::Find(Channel);
		if (Rule == nullptr)
		{
			Out.Note = FText::Format(NSLOCTEXT("DreamTextureImporter", "UnknownChannel", "Unknown channel '{0}'; skipped."), FText::FromString(Channel)).ToString();
			return Out;
		}

		const FDreamTextureMapInfo* Map = Manifest.FindMap(Channel);
		if (Map == nullptr)
		{
			Out.Note = FText::Format(NSLOCTEXT("DreamTextureImporter", "ChannelNotInManifest", "The manifest has no channel '{0}'; skipped."), FText::FromString(Channel)).ToString();
			return Out;
		}

		const FString AssetName = FDreamTextureImporter::GetTextureAssetName(Context.Slug, Channel);
		const FString PackageName = Context.Folder / AssetName;
		Out.ObjectPath = PackageName + TEXT(".") + AssetName;

		// 先看工程里有没有。有，且记录的哈希与 manifest 一致，那就什么都不用做——
		// 连本地文件都不必读，Unchanged 完全由 manifest 里的哈希决定。
		// 本地文件的完整性是 FDreamTextureFetcher 下载时校验的，这里信任它。
		UObject* ExistingObject = FindExistingAsset(PackageName, AssetName);
		UTexture2D* ExistingTexture = Cast<UTexture2D>(ExistingObject);
		if (ExistingObject != nullptr && ExistingTexture == nullptr)
		{
			Out.Action = EDreamTextureImportAction::Failed;
			Out.Note = FText::Format(NSLOCTEXT("DreamTextureImporter", "PathTaken", "The path {0} is already taken by a {1} asset, so the texture cannot be placed there."), FText::FromString(Out.ObjectPath), FText::FromString(ExistingObject->GetClass()->GetName())).ToString();
			return Out;
		}

		if (ExistingTexture != nullptr)
		{
			FDreamTextureSourceRecord Record;
			if (FDreamTextureImporter::ReadSourceRecord(ExistingTexture, Record))
			{
				if (!Record.MaterialId.Equals(Manifest.Id, ESearchCase::CaseSensitive))
				{
					// slug 撞车：另一套材质恰好得到了同一个名字+尾段。覆盖它会弄坏那套材质，
					// 所以宁可让这个通道失败，也不要悄悄抢走人家的资产。
					Out.Action = EDreamTextureImportAction::Failed;
					Out.Note = FText::Format(NSLOCTEXT("DreamTextureImporter", "OwnedByOtherSet", "The asset {0} belongs to another material set ({1}) and cannot be overwritten."), FText::FromString(Out.ObjectPath), FText::FromString(Record.MaterialId)).ToString();
					return Out;
				}
				if (HashesMatch(Record.Sha256, Map->Sha256))
				{
					Out.Action = EDreamTextureImportAction::Unchanged;
					return Out;
				}
			}
		}

		const FString* LocalFile = Context.Request.LocalFiles.Find(Channel);
		if (LocalFile == nullptr || LocalFile->IsEmpty())
		{
			// 没给文件，工程里的资产又和 manifest 对不上（或根本没有）：没法导。
			// 可能是设置里没打开分通道导入，调用方知道具体原因的话会覆盖这条说明。
			Out.Note = ExistingTexture != nullptr
				? NSLOCTEXT("DreamTextureImporter", "NoLocalFileStale", "No local file was provided for this channel, and the texture in the project does not match the manifest; it was not updated.").ToString()
				: NSLOCTEXT("DreamTextureImporter", "NoLocalFile", "No local file was provided for this channel; it was not imported.").ToString();
			return Out;
		}

		FImage Image;
		FString DecodeError;
		if (!DecodeImageFile(*LocalFile, Image, DecodeError))
		{
			Out.Action = EDreamTextureImportAction::Failed;
			Out.Note = DecodeError;
			return Out;
		}
		if (Map->Width > 0 && Map->Height > 0 && (Image.SizeX != Map->Width || Image.SizeY != Map->Height))
		{
			// 不拦：贴图本身是好的，尺寸以文件为准。但这说明 manifest 与文件对不上，留条日志。
			UE_LOG(LogDreamTexture, Warning, TEXT("Channel %s is actually %dx%d, but the manifest says %dx%d; importing the actual file."),
				*Channel, Image.SizeX, Image.SizeY, Map->Width, Map->Height);
		}

		UTexture2D* Texture = ExistingTexture;
		const bool bCreate = (Texture == nullptr);

		if (bCreate)
		{
			UPackage* Package = CreatePackage(*PackageName);
			if (Package == nullptr)
			{
				Out.Action = EDreamTextureImportAction::Failed;
				Out.Note = FText::Format(NSLOCTEXT("DreamTextureImporter", "CreatePackageFailed", "Could not create the package: {0}"), FText::FromString(PackageName)).ToString();
				return Out;
			}
			Texture = NewObject<UTexture2D>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
			if (Texture == nullptr)
			{
				Out.Action = EDreamTextureImportAction::Failed;
				Out.Note = FText::Format(NSLOCTEXT("DreamTextureImporter", "CreateTextureFailed", "Could not create the texture object: {0}"), FText::FromString(Out.ObjectPath)).ToString();
				return Out;
			}
		}
		else
		{
			// 就地更新的关键：同一个 UObject，只换源数据。引用它的材质持有的是这个对象的指针，
			// 删了重建就全断了。PreEditChange 会等异步编译结束并释放渲染资源，
			// 必须与最后的 PostEditChange 成对出现，Source.Init 也要求夹在这一对之间。
			Texture->PreEditChange(nullptr);
		}

		// 直接用 FTextureSource::Init(FImageView) 而不是走 UTextureFactory / Interchange：
		// 工厂会按「用户上次的选择」（FlipNormalMapGreenChannel 存在编辑器配置里）、自动识别法线、
		// 自动转虚拟纹理等去改设置，结果取决于谁的机器、谁点过什么；Interchange 还要过管线资产。
		// 我们的规则表是确定的，这里只做「解码 → 灌进 Source → 套规则」三步。
		// Image 保留 PNG 的位深：G8→TSF_G8，G16→TSF_G16，BGRA8→TSF_BGRA8，RGBA16→TSF_RGBA16。
		Texture->Source.Init(Image);

		ApplyChannelSettings(Texture, *Rule, *Map);

		// 新式 mip 滤波、非 2 的幂的处理等「新导入贴图该有的默认值」，引擎自己的导入流程都过这一道。
		UE::TextureUtilitiesCommon::ApplyDefaultsForNewlyImportedTextures(Texture, /*bIsReimport*/ !bCreate);

		// 放在套完默认值之后：保证最终值只由 manifest 的 y 决定。
		ApplyFlipGreenChannel(Texture, *Rule, *Map);

		WriteSourceRecord(Texture, Manifest.Id, Channel, Map->Sha256);

		if (bCreate)
		{
			FAssetRegistryModule::AssetCreated(Texture);
		}
		Texture->PostEditChange();
		Texture->MarkPackageDirty();

		Context.PackagesToSave.Add(Texture->GetOutermost());
		Context.ResultIndexForPackage.Add(ResultIndex);

		Out.Action = bCreate ? EDreamTextureImportAction::Created : EDreamTextureImportAction::Updated;
		return Out;
	}
}

const FDreamTextureChannelRule* FDreamTextureChannelRule::Find(const FString& Channel)
{
	// 精确匹配、区分大小写：manifest 的键是后端写死的小写，
	// 放宽匹配只会让一个拼错的键悄悄落成资产，而不是被记成 Skipped 让人看见。
	// 注意不能写 Rule.Channel == Channel：FString 的 == 默认忽略大小写。
	return GetRules().FindByPredicate([&Channel](const FDreamTextureChannelRule& Rule)
	{
		return Rule.Channel.Equals(Channel, ESearchCase::CaseSensitive);
	});
}

FDreamTextureImportResult FDreamTextureImporter::Import(const FDreamTextureImportRequest& Request)
{
	check(IsInGameThread());

	FDreamTextureImportResult Result;
	const FDreamTextureManifest& Manifest = Request.Manifest;
	Result.License = Manifest.License;

	if (Manifest.Id.IsEmpty())
	{
		Result.Error = NSLOCTEXT("DreamTextureImporter", "NoMaterialId", "The manifest has no material id, so the asset path cannot be determined.").ToString();
		return Result;
	}

	FImportContext Context{ Request };
	Context.Slug = MakeSlug(Manifest);
	Context.Folder = GetAssetFolder(Manifest, Request.ContentRoot);
	Result.AssetFolder = Context.Folder;

	// 路径不合法（比如内容根里有空格或点号）要在建任何东西之前就拦下。
	{
		FText Reason;
		const FString Probe = Context.Folder / GetTextureAssetName(Context.Slug, DreamTextureChannel::BaseColor);
		if (!FPackageName::IsValidLongPackageName(Probe, /*bIncludeReadOnlyRoots*/ false, &Reason))
		{
			Result.Error = FText::Format(NSLOCTEXT("DreamTextureImporter", "InvalidFolder", "The import folder is not valid: {0} ({1})"), FText::FromString(Context.Folder), Reason).ToString();
			return Result;
		}
	}

	// 要处理的通道 = manifest 里有的 ∪ 调用方给了文件的。主要几张按固定顺序，未知的按字母序排后面。
	TSet<FString> Wanted;
	for (const TPair<FString, FDreamTextureMapInfo>& Pair : Manifest.Maps)
	{
		Wanted.Add(Pair.Key);
	}
	for (const TPair<FString, FString>& Pair : Request.LocalFiles)
	{
		Wanted.Add(Pair.Key);
	}

	TArray<FString> Channels;
	for (const FString& Known : GetChannelOrder())
	{
		if (Wanted.Remove(Known) > 0)
		{
			Channels.Add(Known);
		}
	}
	TArray<FString> Rest = Wanted.Array();
	Rest.Sort();
	Channels.Append(Rest);

	for (const FString& Channel : Channels)
	{
		const int32 ResultIndex = Result.Textures.Num();
		Result.Textures.Add(ImportChannel(Context, Channel, ResultIndex));
	}

	// 一次存完所有动过的包。用编辑器自己的存盘入口：版本控制开着时会签出，不弹模态框。
	if (Request.bSave && Context.PackagesToSave.Num() > 0)
	{
		if (!UEditorLoadingAndSavingUtils::SavePackages(Context.PackagesToSave, /*bOnlyDirty*/ false))
		{
			for (const int32 Index : Context.ResultIndexForPackage)
			{
				FDreamTextureImportedTexture& Texture = Result.Textures[Index];
				Texture.Action = EDreamTextureImportAction::Failed;
				Texture.Note = NSLOCTEXT("DreamTextureImporter", "SaveFailed", "The asset was created, but saving it to disk failed (see the log).").ToString();
			}
		}
	}

	int32 NumFailed = 0;
	int32 NumDone = 0;
	for (const FDreamTextureImportedTexture& Texture : Result.Textures)
	{
		switch (Texture.Action)
		{
		case EDreamTextureImportAction::Failed:
			++NumFailed;
			if (!Result.Error.IsEmpty())
			{
				Result.Error += NSLOCTEXT("DreamTextureImporter", "ErrorSeparator", "; ").ToString();
			}
			Result.Error += FText::Format(NSLOCTEXT("DreamTextureImporter", "ChannelNote", "{0}: {1}"), FText::FromString(Texture.Channel), FText::FromString(Texture.Note)).ToString();
			break;
		case EDreamTextureImportAction::Skipped:
			break;
		default:
			++NumDone;
			break;
		}
	}

	Result.bOK = (NumFailed == 0 && NumDone > 0);
	if (!Result.bOK && Result.Error.IsEmpty())
	{
		Result.Error = Manifest.Maps.Num() == 0 ? NSLOCTEXT("DreamTextureImporter", "NoMaps", "The manifest has no texture channels.").ToString() : NSLOCTEXT("DreamTextureImporter", "NothingImported", "No channel was imported.").ToString();
	}
	return Result;
}

FString FDreamTextureImporter::MakeSlug(const FDreamTextureManifest& Manifest)
{
	// 名字：非 ASCII 字母数字的字符都当分隔符，每个词首字母大写，其余原样。
	// 只动首字母而不是把其余部分压成小写：用户本来写了 "PBR" 或 "MossyRock"，不该被改走样。
	FString Name;
	bool bWordStart = true;
	for (const TCHAR C : Manifest.Name)
	{
		if (!IsAsciiAlnum(C))
		{
			bWordStart = true;
			continue;
		}
		Name.AppendChar(bWordStart ? FChar::ToUpper(C) : C);
		bWordStart = false;
	}
	if (Name.Len() > MaxNameChars)
	{
		Name.LeftInline(MaxNameChars);
	}
	if (Name.IsEmpty())
	{
		// 全中文名（或空名、全标点）：名字部分一个字符都没有，只剩 id 区分。
		// 这时**不能**只取 id 的最后一段——那只有 4 位十六进制（16 位熵），而一批中文提示词的变体
		// 恰恰是同一天同一分钟跑出来的、名字又都不含 ASCII，撞同名的概率不低；撞了的那一个导入器会拒绝
		// （不会静默覆盖别人的资产），但它就导不进来了。所以改用 id 去掉 mat_ 前缀后的整段：
		//   mat_20260827_092650_1f53 → Mat_20260827_092650_1f53
		// 日期时间对人没用，但这里要的是唯一，不是好读。有英文名的材质规则不变（名字 + 尾段）。
		FString Rest = Manifest.Id;
		if (Rest.StartsWith(TEXT("mat_"), ESearchCase::IgnoreCase))
		{
			Rest.RightChopInline(4);
		}
		// 只留 ASCII 字母数字和下划线（资产名里不能有别的），多个下划线并成一个，去掉首尾的下划线。
		FString Clean;
		for (const TCHAR C : Rest)
		{
			if (IsAsciiAlnum(C))
			{
				Clean.AppendChar(C);
			}
			else if (C == TEXT('_') && !Clean.IsEmpty() && !Clean.EndsWith(TEXT("_")))
			{
				Clean.AppendChar(C);
			}
		}
		while (Clean.RemoveFromEnd(TEXT("_")))
		{
		}
		if (Clean.Len() > MaxIdChars)
		{
			Clean.LeftInline(MaxIdChars);
			while (Clean.RemoveFromEnd(TEXT("_")))
			{
			}
		}
		if (Clean.IsEmpty())
		{
			Clean = TEXT("unknown");
		}
		return FString::Printf(TEXT("Mat_%s"), *Clean);
	}

	// 尾段：id 按下划线切，取最后一个非空的。没有下划线就是整段 id。
	// 取尾段而不是整个 id，是因为 id 里的日期时间对人没用，路径里留着只会更长。
	TArray<FString> Parts;
	Manifest.Id.ParseIntoArray(Parts, TEXT("_"), /*CullEmpty*/ true);
	FString Tail;
	for (int32 Index = Parts.Num() - 1; Index >= 0 && Tail.IsEmpty(); --Index)
	{
		Tail = ToAsciiAlnum(Parts[Index]);
	}
	if (Tail.Len() > MaxIdTailChars)
	{
		Tail.LeftInline(MaxIdTailChars);
	}
	if (Tail.IsEmpty())
	{
		Tail = TEXT("unknown");
	}

	return FString::Printf(TEXT("%s_%s"), *Name, *Tail);
}

FString FDreamTextureImporter::ResolveContentRoot(const FString& ContentRoot)
{
	return ::ResolveContentRoot(ContentRoot);
}

FString FDreamTextureImporter::GetAssetFolder(const FDreamTextureManifest& Manifest, const FString& ContentRoot)
{
	return ResolveContentRoot(ContentRoot) / MakeSlug(Manifest);
}

FString FDreamTextureImporter::GetTextureAssetName(const FString& Slug, const FString& Channel)
{
	const FDreamTextureChannelRule* Rule = FDreamTextureChannelRule::Find(Channel);
	if (Rule == nullptr)
	{
		return FString();
	}
	return FString::Printf(TEXT("T_%s_%s"), *Slug, *Rule->Suffix);
}

TMap<FString, FString> FDreamTextureImporter::FindImportedHashes(const FDreamTextureManifest& Manifest, const FString& ContentRoot)
{
	check(IsInGameThread());

	TMap<FString, FString> Out;
	if (Manifest.Id.IsEmpty())
	{
		return Out;
	}

	const FString Slug = MakeSlug(Manifest);
	const FString Folder = GetAssetFolder(Manifest, ContentRoot);

	for (const TPair<FString, FDreamTextureMapInfo>& Pair : Manifest.Maps)
	{
		const FString AssetName = GetTextureAssetName(Slug, Pair.Key);
		if (AssetName.IsEmpty())
		{
			continue;
		}

		const UObject* Asset = FindExistingAsset(Folder / AssetName, AssetName);
		FDreamTextureSourceRecord Record;
		if (Cast<UTexture2D>(Asset) == nullptr || !ReadSourceRecord(Asset, Record))
		{
			continue;
		}

		// 材质 id 和通道都得对得上：路径一样但记的是别的材质，那不是这套的贴图。
		if (Record.MaterialId.Equals(Manifest.Id, ESearchCase::CaseSensitive) &&
			Record.Channel.Equals(Pair.Key, ESearchCase::CaseSensitive) &&
			!Record.Sha256.IsEmpty())
		{
			Out.Add(Pair.Key, Record.Sha256);
		}
	}
	return Out;
}

bool FDreamTextureImporter::ReadSourceRecord(const UObject* Texture, FDreamTextureSourceRecord& OutRecord)
{
#if WITH_METADATA
	if (Texture == nullptr)
	{
		return false;
	}
	UPackage* Package = Texture->GetOutermost();
	if (Package == nullptr)
	{
		return false;
	}

	FMetaData& MetaData = Package->GetMetaData();
	const FString* MaterialId = MetaData.FindValue(Texture, SourceKey::MaterialId);
	const FString* Channel = MetaData.FindValue(Texture, SourceKey::Channel);
	const FString* Sha256 = MetaData.FindValue(Texture, SourceKey::Sha256);
	if (MaterialId == nullptr || Channel == nullptr || Sha256 == nullptr || MaterialId->IsEmpty() || Channel->IsEmpty())
	{
		return false;
	}

	OutRecord.MaterialId = *MaterialId;
	OutRecord.Channel = *Channel;
	OutRecord.Sha256 = *Sha256;
	return true;
#else
	return false;
#endif
}
