// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureManifest.h"

#include "DreamTextureLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

/*
 * 解析的总原则：除了 schema / schema_version 这两道门，其余字段缺了、是 null、类型不对，
 * 一律落回默认值，不报错。
 *
 * 原因有两个。一是 Go 的 nil 切片和 nil 指针会序列化成 null 而不是省略，后端仓库里这个坑踩过两次；
 * 二是 manifest v1 约定「新增字段不算破坏性变更、消费端必须忽略未知字段」——解析端越宽容，
 * 后端往后加字段越不会把老插件搞坏。FJsonObject 的 TryGet*Field 对 null 值本身就返回 false，
 * 所以下面直接用它们、不用自己再判 null。
 */

namespace
{
	/** manifest.schema 的固定值，见 docs/manifest-v1.md。 */
	const TCHAR* const ExpectedSchema = TEXT("dreamtexture/material-manifest");

	FString ReadManifestString(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		FString Value;
		if (Obj.IsValid())
		{
			Obj->TryGetStringField(Field, Value);
		}
		return Value;
	}

	EDreamTextureColorSpace ParseColorSpace(const FString& Text)
	{
		// 不靠文件名猜色彩空间，只认 manifest 给的字面值。
		// 未知值保持 Unknown——让导入端决定怎么降级，而不是在这里替它猜一个。
		if (Text.Equals(TEXT("srgb"), ESearchCase::IgnoreCase))
		{
			return EDreamTextureColorSpace::SRGB;
		}
		if (Text.Equals(TEXT("linear"), ESearchCase::IgnoreCase))
		{
			return EDreamTextureColorSpace::Linear;
		}
		return EDreamTextureColorSpace::Unknown;
	}

	/** 解 maps 里的一项。File 为空的条目没有任何用处，返回 false 由调用方丢掉。 */
	bool ParseMapInfo(const FString& Channel, const TSharedPtr<FJsonObject>& Obj, FDreamTextureMapInfo& Out)
	{
		Out.Channel = Channel;
		Out.File = ReadManifestString(Obj, TEXT("file"));
		Out.ColorSpace = ParseColorSpace(ReadManifestString(Obj, TEXT("colorspace")));
		Out.NormalY = ReadManifestString(Obj, TEXT("y"));
		Out.Packing = ReadManifestString(Obj, TEXT("packing"));
		Out.Sha256 = ReadManifestString(Obj, TEXT("sha256"));

		Obj->TryGetNumberField(TEXT("width"), Out.Width);
		Obj->TryGetNumberField(TEXT("height"), Out.Height);
		Obj->TryGetNumberField(TEXT("bytes"), Out.Bytes);

		return !Out.File.IsEmpty();
	}
}

bool FDreamTextureManifest::FromJson(const TSharedPtr<FJsonObject>& Obj, FDreamTextureManifest& Out, FString& OutError)
{
	if (!Obj.IsValid())
	{
		OutError = NSLOCTEXT("DreamTextureManifest", "Empty", "The manifest is empty.").ToString();
		return false;
	}

	// 先过两道门再读别的：schema 对不上说明根本不是我们的文件，
	// 版本高于我们认识的说明字段语义可能已经变了（v1 的约定是：改语义必须升版本）。
	// 两种情况下往下读都只会读出一堆看似合理的错数据。
	FString Schema;
	if (!Obj->TryGetStringField(TEXT("schema"), Schema))
	{
		OutError = NSLOCTEXT("DreamTextureManifest", "NoSchema", "This is not a DreamTexture material manifest: the schema field is missing.").ToString();
		return false;
	}
	if (Schema != ExpectedSchema)
	{
		OutError = FText::Format(NSLOCTEXT("DreamTextureManifest", "WrongSchema", "This is not a DreamTexture material manifest: its schema is '{0}', but it should be '{1}'."), FText::FromString(Schema), FText::FromString(ExpectedSchema)).ToString();
		return false;
	}

	int32 SchemaVersion = 0;
	if (!Obj->TryGetNumberField(TEXT("schema_version"), SchemaVersion) || SchemaVersion < 1)
	{
		// 缺版本号的 manifest 后端不会产出。宁可拒收，也不要对着一份不知道语义的数据硬读。
		OutError = NSLOCTEXT("DreamTextureManifest", "NoSchemaVersion", "The manifest has no valid schema_version, so its format version cannot be confirmed.").ToString();
		return false;
	}
	if (SchemaVersion > SupportedSchemaVersion)
	{
		OutError = FText::Format(
			NSLOCTEXT("DreamTextureManifest", "NewerSchemaVersion", "This manifest uses format version v{0}, but this plugin only understands up to v{1}. Upgrade the plugin (or have the backend output an older version)."),
			FText::AsNumber(SchemaVersion), FText::AsNumber(SupportedSchemaVersion)).ToString();
		return false;
	}

	// 通过校验之后才动 Out：失败时调用方手里的对象保持原样，不会是个半填的东西。
	FDreamTextureManifest Parsed;

	Parsed.Id = ReadManifestString(Obj, TEXT("id"));
	Parsed.Name = ReadManifestString(Obj, TEXT("name"));
	Parsed.Style = ReadManifestString(Obj, TEXT("style"));
	Parsed.Prompt = ReadManifestString(Obj, TEXT("prompt"));
	Parsed.Negative = ReadManifestString(Obj, TEXT("negative"));
	Parsed.Preview = ReadManifestString(Obj, TEXT("preview"));
	Parsed.CreatedAt = ReadManifestString(Obj, TEXT("created_at"));

	// 种子后端保证 < 2^53（JSON 数字是 double，再大会丢精度），所以按 int64 读是安全的。
	Obj->TryGetNumberField(TEXT("seed"), Parsed.Seed);
	Obj->TryGetNumberField(TEXT("resolution"), Parsed.Resolution);
	Obj->TryGetBoolField(TEXT("tileable"), Parsed.bTileable);

	const TSharedPtr<FJsonObject>* Workflow = nullptr;
	if (Obj->TryGetObjectField(TEXT("workflow"), Workflow) && Workflow != nullptr)
	{
		Parsed.WorkflowId = ReadManifestString(*Workflow, TEXT("id"));
		(*Workflow)->TryGetNumberField(TEXT("version"), Parsed.WorkflowVersion);
	}

	// maps 为 null 是合法的（Go 的 nil map）。未知通道原样保留——消费端按键取用，
	// 不认识的键不会碍事，但丢掉就等于把后端新增的产物吞了。
	const TSharedPtr<FJsonObject>* Maps = nullptr;
	if (Obj->TryGetObjectField(TEXT("maps"), Maps) && Maps != nullptr)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Maps)->Values)
		{
			const TSharedPtr<FJsonObject>* MapObject = nullptr;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetObject(MapObject) || MapObject == nullptr)
			{
				continue;
			}

			FDreamTextureMapInfo Info;
			if (ParseMapInfo(Pair.Key, *MapObject, Info))
			{
				Parsed.Maps.Add(Pair.Key, MoveTemp(Info));
			}
			else
			{
				UE_LOG(LogDreamTexture, Verbose, TEXT("Manifest %s: channel %s has no file name; ignored"), *Parsed.Id, *Pair.Key);
			}
		}
	}

	// license_flags 没有就是「不知道」，不是「可以商用」：bKnown 留 false，让界面按保守口径显示。
	const TSharedPtr<FJsonObject>* License = nullptr;
	if (Obj->TryGetObjectField(TEXT("license_flags"), License) && License != nullptr)
	{
		Parsed.License.bKnown = true;
		(*License)->TryGetBoolField(TEXT("commercial_use"), Parsed.License.bCommercialUse);
		Parsed.License.Reason = ReadManifestString(*License, TEXT("reason"));
	}

	// source 的存在本身就是信息（云端底图 = 复现不了）。null 不携带任何信息，当作没有。
	const TSharedPtr<FJsonValue> Source = Obj->TryGetField(TEXT("source"));
	Parsed.bHasCloudSource = Source.IsValid() && Source->Type != EJson::Null;

	Parsed.Raw = Obj;

	Out = MoveTemp(Parsed);
	OutError.Reset();
	return true;
}

bool FDreamTextureManifest::FromJsonString(const FString& Json, FDreamTextureManifest& Out, FString& OutError)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = FText::Format(NSLOCTEXT("DreamTextureManifest", "BadJson", "The manifest is not valid JSON: {0}"), FText::FromString(Reader->GetErrorMessage())).ToString();
		return false;
	}
	return FromJson(Root, Out, OutError);
}

const FDreamTextureMapInfo* FDreamTextureManifest::FindMap(const FString& Channel) const
{
	return Maps.Find(Channel);
}

bool FDreamTextureMaterialSummary::FromJson(const TSharedPtr<FJsonObject>& Obj, FDreamTextureMaterialSummary& Out)
{
	if (!Obj.IsValid())
	{
		return false;
	}

	// 列表项没有 id 就没法点开、没法下载，留着只会让界面出现一张点不动的卡片。
	FString Id;
	if (!Obj->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		return false;
	}

	FDreamTextureMaterialSummary Parsed;
	Parsed.Id = MoveTemp(Id);
	Parsed.Name = ReadManifestString(Obj, TEXT("name"));
	Parsed.Style = ReadManifestString(Obj, TEXT("style"));
	Parsed.WorkflowId = ReadManifestString(Obj, TEXT("workflow_id"));
	Parsed.Prompt = ReadManifestString(Obj, TEXT("prompt"));

	Obj->TryGetNumberField(TEXT("seed"), Parsed.Seed);
	Obj->TryGetNumberField(TEXT("resolution"), Parsed.Resolution);
	Obj->TryGetBoolField(TEXT("favorite"), Parsed.bFavorite);

	// tags 带 omitempty，没标签时整个字段不存在；万一哪天变成 null 也一样处理。
	Obj->TryGetStringArrayField(TEXT("tags"), Parsed.Tags);

	// 后端的 created_at 是 Go 的 time.Time：RFC3339，毫秒精度，带本地时区偏移。
	// ParseIso8601 认小数秒和 +hh:mm，并且已经换算成 UTC 了。
	const FString CreatedAt = ReadManifestString(Obj, TEXT("created_at"));
	if (!CreatedAt.IsEmpty())
	{
		FDateTime::ParseIso8601(*CreatedAt, Parsed.CreatedAt);
	}

	Out = MoveTemp(Parsed);
	return true;
}
