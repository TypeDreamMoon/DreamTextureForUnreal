// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureMaterialAnalysis.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

/*
 * 用户指认的存盘格式：
 *
 *   {
 *     "version": 1,
 *     "materials": {
 *       "/Game/Foo/MI_Rock.MI_Rock": {
 *         "/Game/Foo/T_Rock_ORM.T_Rock_ORM": { "role": "packed", "layout": ["ao","roughness","metallic","unknown"] }
 *       }
 *     }
 *     },
 *     "uv_kinds": { "/Game/Foo/MI_Hero.MI_Hero": "unwrapped" }
 *   }
 *
 * uv_kinds 是回流（M9）记的「这个材质的贴图是 UV 展开的」，只存非默认值（默认是平铺）。
 * 旧版本的插件读到这个文件会忽略它不认识的键，所以不需要升 version。
 *
 * layout 只在 role 为 packed 时写。读的时候对缺字段、未知角色名、长度不对的数组一律容忍——
 * 这个文件会被人手改、会被以后更新的版本改，读不懂的条目丢掉，不能让整个文件作废。
 */

namespace
{
	constexpr int32 FormatVersion = 1;

	const TCHAR* const VersionField = TEXT("version");
	const TCHAR* const MaterialsField = TEXT("materials");
	const TCHAR* const RoleField = TEXT("role");
	const TCHAR* const LayoutField = TEXT("layout");
	const TCHAR* const UVKindsField = TEXT("uv_kinds");
}

const TCHAR* LexToString(EDreamTextureUVKind Kind)
{
	return Kind == EDreamTextureUVKind::Unwrapped ? TEXT("unwrapped") : TEXT("tiled");
}

bool LexTryParseString(EDreamTextureUVKind& OutKind, const FString& Text)
{
	if (Text.Equals(TEXT("tiled"), ESearchCase::IgnoreCase))
	{
		OutKind = EDreamTextureUVKind::Tiled;
		return true;
	}
	if (Text.Equals(TEXT("unwrapped"), ESearchCase::IgnoreCase))
	{
		OutKind = EDreamTextureUVKind::Unwrapped;
		return true;
	}
	return false;
}

FString FDreamTextureOverrideStore::GetDefaultFilePath()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamTexture") / TEXT("ReflowOverrides.json"));
}

FDreamTextureOverrideStore& FDreamTextureOverrideStore::Get()
{
	static FDreamTextureOverrideStore Instance(GetDefaultFilePath());
	return Instance;
}

FDreamTextureOverrideStore::FDreamTextureOverrideStore(const FString& InFilePath)
	: FilePath(InFilePath)
{
}

void FDreamTextureOverrideStore::Reload()
{
	bLoaded = false;
	Data.Reset();
	UVKinds.Reset();
	LastError.Reset();
}

void FDreamTextureOverrideStore::EnsureLoaded() const
{
	if (bLoaded)
	{
		return;
	}
	bLoaded = true;
	Data.Reset();
	UVKinds.Reset();

	if (!IFileManager::Get().FileExists(*FilePath))
	{
		return;
	}

	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *FilePath))
	{
		// 文件在、但读不出来（被别的进程独占、权限、磁盘错误……）。这时**不能**当成「没有记录」继续用：
		// 内存里是空的，下一次 Set 就会把一份空的（外加这一条）写回去，用户攒下的全部指认就没了。
		// 所以不置「已加载」——下一次访问重新读盘（文件可能只是暂时被占着）；写操作在 Save 里一律拒绝。
		bLoaded = false;
		LastError = FText::Format(NSLOCTEXT("DreamTextureOverrideStore", "ReadFailed", "Could not read the assignment record: {0}"), FText::FromString(FilePath)).ToString();
		return;
	}

	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		// 文件坏了：当空的用，下次保存会覆盖它。不删文件，留给人看。
		LastError = FText::Format(NSLOCTEXT("DreamTextureOverrideStore", "BadJson", "The assignment record is not valid JSON and was ignored: {0}"), FText::FromString(FilePath)).ToString();
		return;
	}

	// 平铺 / UV 展开：读不懂的条目丢掉，别让一条坏记录拖垮整个文件。
	const TSharedPtr<FJsonObject>* Kinds = nullptr;
	if (Root->TryGetObjectField(UVKindsField, Kinds) && Kinds != nullptr && Kinds->IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Kinds)->Values)
		{
			FString KindText;
			EDreamTextureUVKind Kind = EDreamTextureUVKind::Tiled;
			if (Pair.Value.IsValid() && Pair.Value->TryGetString(KindText) && LexTryParseString(Kind, KindText) && Kind != EDreamTextureUVKind::Tiled)
			{
				UVKinds.Add(Pair.Key, Kind);
			}
		}
	}

	const TSharedPtr<FJsonObject>* Materials = nullptr;
	if (!Root->TryGetObjectField(MaterialsField, Materials) || Materials == nullptr || !Materials->IsValid())
	{
		return;
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& MaterialPair : (*Materials)->Values)
	{
		const TSharedPtr<FJsonObject>* Textures = nullptr;
		if (!MaterialPair.Value.IsValid() || !MaterialPair.Value->TryGetObject(Textures) || Textures == nullptr || !Textures->IsValid())
		{
			continue;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& TexturePair : (*Textures)->Values)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!TexturePair.Value.IsValid() || !TexturePair.Value->TryGetObject(Entry) || Entry == nullptr || !Entry->IsValid())
			{
				continue;
			}

			FString RoleText;
			FDreamTextureRoleOverride Override;
			if (!(*Entry)->TryGetStringField(RoleField, RoleText) || !LexTryParseString(Override.Role, RoleText))
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Layout = nullptr;
			if ((*Entry)->TryGetArrayField(LayoutField, Layout) && Layout != nullptr)
			{
				for (int32 Index = 0; Index < FMath::Min(4, Layout->Num()); ++Index)
				{
					FString ChannelText;
					EDreamTextureRole ChannelRole = EDreamTextureRole::Unknown;
					if ((*Layout)[Index].IsValid() && (*Layout)[Index]->TryGetString(ChannelText) && LexTryParseString(ChannelRole, ChannelText))
					{
						Override.Layout.Channels[Index] = ChannelRole;
					}
				}
			}
			Data.FindOrAdd(MaterialPair.Key).Add(TexturePair.Key, Override);
		}
	}
}

bool FDreamTextureOverrideStore::Find(const FString& MaterialAssetPath, const FString& TexturePath, FDreamTextureRoleOverride& OutOverride) const
{
	EnsureLoaded();
	const TMap<FString, FDreamTextureRoleOverride>* Textures = Data.Find(MaterialAssetPath);
	if (Textures == nullptr)
	{
		return false;
	}
	const FDreamTextureRoleOverride* Found = Textures->Find(TexturePath);
	if (Found == nullptr)
	{
		return false;
	}
	OutOverride = *Found;
	return true;
}

bool FDreamTextureOverrideStore::Set(const FString& MaterialAssetPath, const FString& TexturePath, const FDreamTextureRoleOverride& Override)
{
	EnsureLoaded();
	if (MaterialAssetPath.IsEmpty() || TexturePath.IsEmpty())
	{
		LastError = NSLOCTEXT("DreamTextureOverrideStore", "EmptyPath", "The material or texture path is empty, so this manual assignment cannot be remembered.").ToString();
		return false;
	}
	Data.FindOrAdd(MaterialAssetPath).Add(TexturePath, Override);
	return Save();
}

bool FDreamTextureOverrideStore::Remove(const FString& MaterialAssetPath, const FString& TexturePath)
{
	EnsureLoaded();
	TMap<FString, FDreamTextureRoleOverride>* Textures = Data.Find(MaterialAssetPath);
	if (Textures == nullptr || Textures->Remove(TexturePath) == 0)
	{
		return false;
	}
	if (Textures->Num() == 0)
	{
		Data.Remove(MaterialAssetPath);
	}
	Save();
	return true;
}

int32 FDreamTextureOverrideStore::NumFor(const FString& MaterialAssetPath) const
{
	EnsureLoaded();
	const TMap<FString, FDreamTextureRoleOverride>* Textures = Data.Find(MaterialAssetPath);
	return Textures != nullptr ? Textures->Num() : 0;
}

bool FDreamTextureOverrideStore::FindUVKind(const FString& MaterialAssetPath, EDreamTextureUVKind& OutKind) const
{
	EnsureLoaded();
	const EDreamTextureUVKind* Found = UVKinds.Find(MaterialAssetPath);
	if (Found == nullptr)
	{
		return false;
	}
	OutKind = *Found;
	return true;
}

bool FDreamTextureOverrideStore::SetUVKind(const FString& MaterialAssetPath, EDreamTextureUVKind Kind)
{
	EnsureLoaded();
	if (MaterialAssetPath.IsEmpty())
	{
		LastError = NSLOCTEXT("DreamTextureOverrideStore", "EmptyMaterialPath", "The material path is empty, so whether it is Tileable or UV-unwrapped cannot be remembered.").ToString();
		return false;
	}
	if (Kind == EDreamTextureUVKind::Tiled)
	{
		if (UVKinds.Remove(MaterialAssetPath) == 0)
		{
			return true;
		}
	}
	else
	{
		UVKinds.Add(MaterialAssetPath, Kind);
	}
	return Save();
}

bool FDreamTextureOverrideStore::Save()
{
	// 读失败（见 EnsureLoaded）时不写：这时内存里的状态不是文件里的状态，写回去会覆盖全部已有的记录。
	// Set / SetUVKind 在调用 Save 之前已经改了内存，这里拒绝之后那点改动留不下来——bLoaded 为假，
	// 下一次访问会先 Data.Reset() 再重新读盘。
	if (!bLoaded)
	{
		LastError = FText::Format(NSLOCTEXT("DreamTextureOverrideStore", "WriteRefused", "The assignment record cannot be read right now ({0}); nothing was written this time, to avoid overwriting the existing record."), FText::FromString(FilePath)).ToString();
		return false;
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(VersionField, FormatVersion);

	TSharedRef<FJsonObject> Materials = MakeShared<FJsonObject>();

	// 键排个序：文件进不进版本控制都不该因为 TMap 的遍历顺序而每次存盘都抖一遍。
	TArray<FString> MaterialKeys;
	Data.GetKeys(MaterialKeys);
	MaterialKeys.Sort();
	for (const FString& MaterialKey : MaterialKeys)
	{
		const TMap<FString, FDreamTextureRoleOverride>& Textures = Data.FindChecked(MaterialKey);
		TSharedRef<FJsonObject> TextureObject = MakeShared<FJsonObject>();

		TArray<FString> TextureKeys;
		Textures.GetKeys(TextureKeys);
		TextureKeys.Sort();
		for (const FString& TextureKey : TextureKeys)
		{
			const FDreamTextureRoleOverride& Override = Textures.FindChecked(TextureKey);
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(RoleField, LexToString(Override.Role));
			if (Override.Role == EDreamTextureRole::Packed)
			{
				TArray<TSharedPtr<FJsonValue>> Layout;
				for (int32 Index = 0; Index < 4; ++Index)
				{
					Layout.Add(MakeShared<FJsonValueString>(LexToString(Override.Layout.Channels[Index])));
				}
				Entry->SetArrayField(LayoutField, Layout);
			}
			TextureObject->SetObjectField(TextureKey, Entry);
		}
		Materials->SetObjectField(MaterialKey, TextureObject);
	}
	Root->SetObjectField(MaterialsField, Materials);

	if (UVKinds.Num() > 0)
	{
		TSharedRef<FJsonObject> Kinds = MakeShared<FJsonObject>();
		TArray<FString> KindKeys;
		UVKinds.GetKeys(KindKeys);
		KindKeys.Sort();
		for (const FString& Key : KindKeys)
		{
			Kinds->SetStringField(Key, LexToString(UVKinds.FindChecked(Key)));
		}
		Root->SetObjectField(UVKindsField, Kinds);
	}

	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		LastError = NSLOCTEXT("DreamTextureOverrideStore", "SerializeFailed", "Could not serialize the assignment record.").ToString();
		return false;
	}

	// 先写临时文件再改名：写到一半被杀掉，不会留下一个读不了的半截文件把以前的指认全冲掉。
	const FString TempPath = FilePath + TEXT(".tmp");
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), /*Tree*/ true);
	if (!FFileHelper::SaveStringToFile(Text, *TempPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		LastError = FText::Format(NSLOCTEXT("DreamTextureOverrideStore", "WriteFailed", "Could not write the assignment record: {0}"), FText::FromString(TempPath)).ToString();
		return false;
	}
	if (!IFileManager::Get().Move(*FilePath, *TempPath, /*Replace*/ true))
	{
		LastError = FText::Format(NSLOCTEXT("DreamTextureOverrideStore", "RenameFailed", "The assignment record was written but could not be renamed into place: {0}"), FText::FromString(FilePath)).ToString();
		IFileManager::Get().Delete(*TempPath);
		return false;
	}
	LastError.Reset();
	return true;
}
