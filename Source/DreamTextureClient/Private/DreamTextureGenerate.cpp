// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureGenerate.h"

#include "DreamTextureHttpUtil.h"
#include "DreamTextureSettings.h"

#include "Dom/JsonObject.h"
#include "Containers/StringConv.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

/*
 * 解析的总原则和 DreamTextureManifest.cpp 一样：缺了、是 null、类型不对，一律落回默认值，不报错。
 * FJsonObject 的 TryGet*Field 对 null 值本身就返回 false，所以下面直接用，不再自己判 null。
 */

namespace
{
	/** 列表类请求的超时（秒）。后端在本机，正常是毫秒级；给 15 秒留给冷启动。 */
	constexpr float GenerateApiTimeoutSecs = 15.0f;

	/**
	 * 提交的超时。后端提交时要把每个变体的参数验一遍、逐个落库，8 个变体也是毫秒级，
	 * 宽一点是因为失败时用户会重试，而「请求其实成功了、只是应答慢」重试就会提交两批。
	 */
	constexpr float SubmitTimeoutSecs = 30.0f;

	/** 扩写：后端自己最多等模型 90 秒（internal/imagen/refine.go 的 refineTimeout），这里要比它长。 */
	constexpr float RefineTimeoutSecs = 120.0f;

	/** 列扩写模型要现问外部网关，比本机接口慢。 */
	constexpr float ModelListTimeoutSecs = 30.0f;

	FString ReadString(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		FString Value;
		if (Obj.IsValid())
		{
			Obj->TryGetStringField(Field, Value);
		}
		return Value;
	}

	/** JSON 标量 → 文本。整数不带小数点；对象、数组、null 返回 false。 */
	bool ScalarToText(const TSharedPtr<FJsonValue>& Value, FString& Out)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		switch (Value->Type)
		{
		case EJson::String:
			Out = Value->AsString();
			return true;
		case EJson::Boolean:
			Out = Value->AsBool() ? TEXT("true") : TEXT("false");
			return true;
		case EJson::Number:
			{
				const double Number = Value->AsNumber();
				if (FMath::Abs(Number) < 1.0e15 && FMath::FloorToDouble(Number) == Number)
				{
					Out = FString::Printf(TEXT("%lld"), static_cast<int64>(Number));
				}
				else
				{
					Out = FString::SanitizeFloat(Number);
				}
				return true;
			}
		default:
			return false;
		}
	}

	bool ParseObjectArray(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, TArray<TSharedPtr<FJsonObject>>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Obj.IsValid() || !Obj->TryGetArrayField(Field, Items) || Items == nullptr)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Item : *Items)
		{
			const TSharedPtr<FJsonObject>* ItemObject = nullptr;
			if (Item.IsValid() && Item->TryGetObject(ItemObject) && ItemObject != nullptr)
			{
				Out.Add(*ItemObject);
			}
		}
		return true;
	}

	bool LooksLikeInteger(const FString& Text)
	{
		if (Text.IsEmpty())
		{
			return false;
		}
		int32 Start = Text[0] == TCHAR('-') ? 1 : 0;
		if (Start >= Text.Len())
		{
			return false;
		}
		for (int32 i = Start; i < Text.Len(); ++i)
		{
			if (!FChar::IsDigit(Text[i]))
			{
				return false;
			}
		}
		return true;
	}

	/** 字符串序列化成紧凑 JSON。 */
	FString SerializeCondensed(const TSharedRef<FJsonObject>& Root)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Root, Writer);
		Writer->Close();
		return Out;
	}
}

//////////////////////////////////////////////////////////////////////////
// 参数声明

bool FDreamTextureParamSpec::TryGetIntOptions(TArray<int32>& Out) const
{
	Out.Reset();
	if (Options.Num() == 0)
	{
		return false;
	}
	for (const FString& Option : Options)
	{
		if (!LooksLikeInteger(Option))
		{
			Out.Reset();
			return false;
		}
		Out.Add(FCString::Atoi(*Option));
	}
	return true;
}

int32 FDreamTextureParamSpec::GetDefaultInt(int32 Fallback) const
{
	return (bHasDefault && LooksLikeInteger(Default)) ? FCString::Atoi(*Default) : Fallback;
}

bool FDreamTextureParamSpec::FromJson(const TSharedPtr<FJsonObject>& Obj, FDreamTextureParamSpec& Out)
{
	if (!Obj.IsValid())
	{
		return false;
	}

	FDreamTextureParamSpec Parsed;
	Parsed.Key = ReadString(Obj, TEXT("key"));
	if (Parsed.Key.IsEmpty())
	{
		// 没有 key 的参数没法注入，留着只会让界面出现一个什么都不影响的控件。
		return false;
	}
	Parsed.Label = ReadString(Obj, TEXT("label"));
	Parsed.Type = ReadString(Obj, TEXT("type"));
	Parsed.Note = ReadString(Obj, TEXT("note"));
	Obj->TryGetBoolField(TEXT("hidden"), Parsed.bHidden);
	Obj->TryGetBoolField(TEXT("multiline"), Parsed.bMultiline);
	Obj->TryGetBoolField(TEXT("required"), Parsed.bRequired);
	Parsed.Widget = ReadString(Obj, TEXT("widget"));

	const TArray<TSharedPtr<FJsonValue>>* OptionItems = nullptr;
	if (Obj->TryGetArrayField(TEXT("options"), OptionItems) && OptionItems != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& Item : *OptionItems)
		{
			FString Text;
			if (ScalarToText(Item, Text))
			{
				Parsed.Options.Add(MoveTemp(Text));
			}
		}
	}

	const TSharedPtr<FJsonValue> DefaultValue = Obj->TryGetField(TEXT("default"));
	Parsed.bHasDefault = ScalarToText(DefaultValue, Parsed.Default);

	double Number = 0.0;
	if (Obj->TryGetNumberField(TEXT("min"), Number))
	{
		Parsed.Min = Number;
	}
	if (Obj->TryGetNumberField(TEXT("max"), Number))
	{
		Parsed.Max = Number;
	}

	Out = MoveTemp(Parsed);
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 许可

bool FDreamTextureLicenseNotice::FromJson(const TSharedPtr<FJsonObject>& Obj, FDreamTextureLicenseNotice& Out)
{
	if (!Obj.IsValid())
	{
		return false;
	}

	FDreamTextureLicenseNotice Parsed;
	Parsed.Component = ReadString(Obj, TEXT("component"));
	Parsed.License = ReadString(Obj, TEXT("license"));

	// commercial 缺失时按 false：宁可多提醒一次，也别在后端少发字段时把研究用途的东西说成可商用。
	Obj->TryGetBoolField(TEXT("commercial"), Parsed.bCommercial);
	Parsed.ReplaceableSegment = ReadString(Obj, TEXT("replaceable_segment"));
	Obj->TryGetStringArrayField(TEXT("alternatives"), Parsed.Alternatives);

	Out = MoveTemp(Parsed);
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 工作流元信息

bool FDreamTextureWorkflowMeta::IsCommercial() const
{
	for (const FDreamTextureLicenseNotice& Notice : Licenses)
	{
		if (!Notice.bCommercial)
		{
			return false;
		}
	}
	return true;
}

FString FDreamTextureWorkflowMeta::GetNonCommercialReason() const
{
	TArray<FString> Blockers;
	for (const FDreamTextureLicenseNotice& Notice : Licenses)
	{
		if (!Notice.bCommercial)
		{
			Blockers.Add(Notice.Component + TEXT(": ") + Notice.License);
		}
	}
	return FString::Join(Blockers, TEXT("; "));
}

const FDreamTextureParamSpec* FDreamTextureWorkflowMeta::FindParam(const FString& Key) const
{
	for (const FDreamTextureParamSpec& Spec : Params)
	{
		if (Spec.Key == Key)
		{
			return &Spec;
		}
	}
	for (const FDreamTextureParamSpec& Spec : Advanced)
	{
		if (Spec.Key == Key)
		{
			return &Spec;
		}
	}
	return nullptr;
}

const FDreamTextureParamSpec* FDreamTextureWorkflowMeta::FindParamByWidget(const FString& Widget) const
{
	if (Widget.IsEmpty())
	{
		return nullptr;
	}
	for (const FDreamTextureParamSpec& Spec : Params)
	{
		if (Spec.Widget == Widget)
		{
			return &Spec;
		}
	}
	for (const FDreamTextureParamSpec& Spec : Advanced)
	{
		if (Spec.Widget == Widget)
		{
			return &Spec;
		}
	}
	return nullptr;
}

bool FDreamTextureWorkflowMeta::FromJson(const TSharedPtr<FJsonObject>& Obj, FDreamTextureWorkflowMeta& Out)
{
	if (!Obj.IsValid())
	{
		return false;
	}

	FDreamTextureWorkflowMeta Parsed;
	Parsed.Id = ReadString(Obj, TEXT("id"));
	if (Parsed.Id.IsEmpty())
	{
		return false;
	}

	Parsed.Kind = ReadString(Obj, TEXT("kind"));
	Obj->TryGetNumberField(TEXT("version"), Parsed.Version);
	Parsed.Name = ReadString(Obj, TEXT("name"));
	Parsed.Style = ReadString(Obj, TEXT("style"));
	Parsed.Description = ReadString(Obj, TEXT("description"));
	Obj->TryGetNumberField(TEXT("resolution"), Parsed.Resolution);
	Obj->TryGetBoolField(TEXT("tileable"), Parsed.bTileable);

	const FString Segment = ReadString(Obj, TEXT("segment"));
	if (Segment == TEXT("source"))
	{
		Parsed.Segment = EDreamTextureSegmentKind::Source;
	}
	else if (Segment == TEXT("decompose"))
	{
		Parsed.Segment = EDreamTextureSegmentKind::Decompose;
	}

	Parsed.Domain = ReadString(Obj, TEXT("domain"));
	Obj->TryGetStringArrayField(TEXT("expects_domain"), Parsed.ExpectsDomain);
	Parsed.SourceSegment = ReadString(Obj, TEXT("source_segment"));
	Parsed.DecomposeSegment = ReadString(Obj, TEXT("decompose_segment"));
	Parsed.Mismatch = ReadString(Obj, TEXT("mismatch"));

	const TSharedPtr<FJsonObject>* Source = nullptr;
	if (Obj->TryGetObjectField(TEXT("source"), Source) && Source != nullptr)
	{
		Parsed.bCloudSource = true;
		Parsed.CloudProvider = ReadString(*Source, TEXT("provider"));
		(*Source)->TryGetBoolField(TEXT("direct_output"), Parsed.bDirectOutput);
	}

	// 许可看复数的 licenses：组合出来的管线是两段的并集，单文件模板最多一条。
	// 单数的 license_notice 只作兜底——后端把它抄进 licenses 之后两个字段同时存在，
	// 但更老的后端可能只发单数，那时候别把 research-only 标记丢了。
	TArray<TSharedPtr<FJsonObject>> LicenseObjects;
	ParseObjectArray(Obj, TEXT("licenses"), LicenseObjects);
	for (const TSharedPtr<FJsonObject>& LicenseObject : LicenseObjects)
	{
		FDreamTextureLicenseNotice Notice;
		if (FDreamTextureLicenseNotice::FromJson(LicenseObject, Notice))
		{
			Parsed.Licenses.Add(MoveTemp(Notice));
		}
	}
	if (Parsed.Licenses.Num() == 0)
	{
		const TSharedPtr<FJsonObject>* Single = nullptr;
		FDreamTextureLicenseNotice Notice;
		if (Obj->TryGetObjectField(TEXT("license_notice"), Single) && Single != nullptr
			&& FDreamTextureLicenseNotice::FromJson(*Single, Notice))
		{
			Parsed.Licenses.Add(MoveTemp(Notice));
		}
	}

	for (const TCHAR* Field : { TEXT("params"), TEXT("advanced") })
	{
		TArray<TSharedPtr<FJsonObject>> SpecObjects;
		ParseObjectArray(Obj, Field, SpecObjects);
		TArray<FDreamTextureParamSpec>& Target = FString(Field) == TEXT("params") ? Parsed.Params : Parsed.Advanced;
		for (const TSharedPtr<FJsonObject>& SpecObject : SpecObjects)
		{
			FDreamTextureParamSpec Spec;
			if (FDreamTextureParamSpec::FromJson(SpecObject, Spec))
			{
				Target.Add(MoveTemp(Spec));
			}
		}
	}

	Out = MoveTemp(Parsed);
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 目录

FString FDreamTextureWorkflowCatalog::MakeComposedId(const FString& SourceId, const FString& DecomposeId)
{
	return SourceId + TEXT(".") + DecomposeId;
}

const FDreamTextureWorkflowMeta* FDreamTextureWorkflowCatalog::FindWorkflow(const FString& Id) const
{
	return Workflows.FindByPredicate([&Id](const FDreamTextureWorkflowMeta& Meta) { return Meta.Id == Id; });
}

const FDreamTextureWorkflowMeta* FDreamTextureWorkflowCatalog::FindSegment(const FString& Id) const
{
	return Segments.FindByPredicate([&Id](const FDreamTextureWorkflowMeta& Meta) { return Meta.Id == Id; });
}

const FDreamTextureWorkflowMeta* FDreamTextureWorkflowCatalog::FindComposed(const FString& SourceId, const FString& DecomposeId) const
{
	if (SourceId.IsEmpty() || DecomposeId.IsEmpty())
	{
		return nullptr;
	}
	return FindWorkflow(MakeComposedId(SourceId, DecomposeId));
}

namespace
{
	/** 基础参数里有没有这个 key。Web 端的判据是 s.params.some(p => p.key === 'prompt')，只看基础参数、不看高级参数。 */
	bool HasBasicParam(const FDreamTextureWorkflowMeta& Meta, const TCHAR* Key)
	{
		return Meta.Params.ContainsByPredicate([Key](const FDreamTextureParamSpec& Spec) { return Spec.Key == Key; });
	}

	bool HasRequiredParam(const FDreamTextureWorkflowMeta& Meta)
	{
		return Meta.Params.ContainsByPredicate([](const FDreamTextureParamSpec& Spec) { return Spec.bRequired; })
			|| Meta.Advanced.ContainsByPredicate([](const FDreamTextureParamSpec& Spec) { return Spec.bRequired; });
	}

	TArray<const FDreamTextureWorkflowMeta*> CollectSegments(const TArray<FDreamTextureWorkflowMeta>& Segments, EDreamTextureSegmentKind Kind)
	{
		TArray<const FDreamTextureWorkflowMeta*> Out;
		for (const FDreamTextureWorkflowMeta& Meta : Segments)
		{
			// 图片类管线（kind=image）不分段，这里只管材质。
			if (Meta.Segment != Kind || Meta.Kind.Equals(TEXT("image"), ESearchCase::IgnoreCase))
			{
				continue;
			}

			// 生成页只列它填得了的段：出图段要有提示词可填，分解段不能有必填项（见 GetSourceSegments 的说明）。
			// 不过滤的话，「已有底图」会出现在出图模型下拉里，选了它提交必然 400。
			if (Kind == EDreamTextureSegmentKind::Source && !HasBasicParam(Meta, TEXT("prompt")))
			{
				continue;
			}
			if (Kind == EDreamTextureSegmentKind::Decompose && HasRequiredParam(Meta))
			{
				continue;
			}
			Out.Add(&Meta);
		}
		// 后端已经按 id 排过序，这里再排一次：顺序决定下拉里谁排第一、谁是默认选中，不该依赖对端的实现细节。
		Out.Sort([](const FDreamTextureWorkflowMeta& A, const FDreamTextureWorkflowMeta& B) { return A.Id < B.Id; });
		return Out;
	}
}

TArray<const FDreamTextureWorkflowMeta*> FDreamTextureWorkflowCatalog::GetSourceSegments() const
{
	return CollectSegments(Segments, EDreamTextureSegmentKind::Source);
}

TArray<const FDreamTextureWorkflowMeta*> FDreamTextureWorkflowCatalog::GetDecomposeSegments() const
{
	return CollectSegments(Segments, EDreamTextureSegmentKind::Decompose);
}

bool FDreamTextureWorkflowCatalog::FromJson(const TSharedPtr<FJsonObject>& Root, FDreamTextureWorkflowCatalog& Out)
{
	if (!Root.IsValid())
	{
		return false;
	}

	FDreamTextureWorkflowCatalog Parsed;

	// 两个数组后端都用 make 而不是 var 来保证不是 null，但这里不依赖它：缺失、null、[] 都当没有。
	for (const TCHAR* Field : { TEXT("workflows"), TEXT("segments") })
	{
		TArray<TSharedPtr<FJsonObject>> Objects;
		ParseObjectArray(Root, Field, Objects);
		TArray<FDreamTextureWorkflowMeta>& Target = FString(Field) == TEXT("workflows") ? Parsed.Workflows : Parsed.Segments;
		for (const TSharedPtr<FJsonObject>& Object : Objects)
		{
			// 解不出来的一项（没有 id）直接丢：一份坏声明不该让整个目录作废。
			FDreamTextureWorkflowMeta Meta;
			if (FDreamTextureWorkflowMeta::FromJson(Object, Meta))
			{
				Target.Add(MoveTemp(Meta));
			}
		}
	}

	Out = MoveTemp(Parsed);
	return true;
}

bool FDreamTextureWorkflowCatalog::FromJsonString(const FString& Json, FDreamTextureWorkflowCatalog& Out)
{
	return FromJson(DreamTexture::ParseJsonObject(Json), Out);
}

//////////////////////////////////////////////////////////////////////////
// 生成请求

bool FDreamTextureDerivedFrom::IsValidFingerprint(const FString& Text)
{
	// 与后端 sha16（^[0-9a-f]{16}$，规整成小写之后）同口径：这里大小写都放行。
	if (Text.Len() != 16)
	{
		return false;
	}
	for (const TCHAR C : Text)
	{
		const bool bHex = (C >= TEXT('0') && C <= TEXT('9')) || (C >= TEXT('a') && C <= TEXT('f')) || (C >= TEXT('A') && C <= TEXT('F'));
		if (!bHex)
		{
			return false;
		}
	}
	return true;
}

bool FDreamTextureDerivedFrom::Validate(FString& OutError) const
{
	if (!IsSet())
	{
		return true;
	}

	if (Kind != KindUEMaterial())
	{
		OutError = FText::Format(NSLOCTEXT("DreamTextureGenerate", "DerivedKindUnsupported", "derived_from.kind only supports {0} for now, but received '{1}'."), FText::FromString(KindUEMaterial()), FText::FromString(Kind)).ToString();
		return false;
	}
	if (Mode != ModeFillChannels() && Mode != ModeRestyle())
	{
		OutError = FText::Format(NSLOCTEXT("DreamTextureGenerate", "DerivedModeInvalid", "derived_from.mode must be {0} or {1}, but received '{2}'."), FText::FromString(ModeFillChannels()), FText::FromString(ModeRestyle()), FText::FromString(Mode)).ToString();
		return false;
	}
	if (AssetPath.Len() > 512)
	{
		OutError = FText::Format(NSLOCTEXT("DreamTextureGenerate", "DerivedAssetPathTooLong", "derived_from.asset_path is too long ({0} characters; the limit is 512)."), FText::AsNumber(AssetPath.Len())).ToString();
		return false;
	}
	if (Engine.Len() > 40)
	{
		OutError = FText::Format(NSLOCTEXT("DreamTextureGenerate", "DerivedEngineTooLong", "derived_from.engine is too long ({0} characters; the limit is 40)."), FText::AsNumber(Engine.Len())).ToString();
		return false;
	}
	for (const TPair<FString, FString>& Pair : MapsIn)
	{
		if (Pair.Key.TrimStartAndEnd().IsEmpty())
		{
			OutError = NSLOCTEXT("DreamTextureGenerate", "DerivedEmptyChannel", "derived_from.maps_in contains an empty channel name.").ToString();
			return false;
		}
		if (!IsValidFingerprint(Pair.Value))
		{
			OutError = FText::Format(NSLOCTEXT("DreamTextureGenerate", "DerivedBadFingerprint", "derived_from.maps_in[{0}] must be the first 16 hexadecimal digits of a SHA-256, but received '{1}'."), FText::FromString(Pair.Key), FText::FromString(Pair.Value)).ToString();
			return false;
		}
	}
	return true;
}

bool FDreamTextureGenerateRequest::IsRandomSeed() const
{
	const int64* Seed = IntParams.Find(SeedKey());
	return Seed == nullptr || *Seed == DreamTextureGenerateLimits::RandomSeed;
}

int64 FDreamTextureGenerateRequest::MaxFixedSeed(int32 Variants)
{
	const int32 Count = FMath::Clamp(Variants, 1, DreamTextureGenerateLimits::MaxVariants);
	return DreamTextureGenerateLimits::MaxSafeSeed - (Count - 1);
}

FString FDreamTextureGenerateRequest::MakeNameFromPrompt(const FString& Prompt)
{
	constexpr int32 MaxChars = 60;

	FString Name = Prompt.TrimStartAndEnd();
	Name.ReplaceInline(TEXT("\r\n"), TEXT(" "));
	Name.ReplaceInline(TEXT("\n"), TEXT(" "));
	Name.ReplaceInline(TEXT("\r"), TEXT(" "));
	Name.ReplaceInline(TEXT("\t"), TEXT(" "));

	if (Name.Len() > MaxChars)
	{
		// FString 是 UTF-16。第 60 个单元恰好是代理对的前半个时，切下去会留一个孤立的半个字符，
		// 到了后端存进 SQLite 再取出来就是乱码。
		int32 Cut = MaxChars;
		if (StringConv::IsHighSurrogate(Name[Cut - 1]))
		{
			--Cut;
		}
		Name = Name.Left(Cut);
	}
	return Name;
}

bool FDreamTextureGenerateRequest::Validate(FString& OutError) const
{
	if (WorkflowId.TrimStartAndEnd().IsEmpty())
	{
		OutError = NSLOCTEXT("DreamTextureGenerate", "NoWorkflow", "No workflow is selected.").ToString();
		return false;
	}

	if (Variants < 1 || Variants > DreamTextureGenerateLimits::MaxVariants)
	{
		OutError = FText::Format(NSLOCTEXT("DreamTextureGenerate", "VariantsOutOfRange", "The number of variants must be between 1 and {0}, but received {1}."), FText::AsNumber(DreamTextureGenerateLimits::MaxVariants), FText::AsNumber(Variants)).ToString();
		return false;
	}

	// 种子先查，给带上限的具体说法；通用的整数检查跳过它，免得越界的种子只拿到一句笼统的话。
	if (const int64* Seed = IntParams.Find(SeedKey()))
	{
		if (*Seed < DreamTextureGenerateLimits::RandomSeed)
		{
			OutError = FText::Format(NSLOCTEXT("DreamTextureGenerate", "SeedNegative", "The seed cannot be negative (-1 means random), but received {0}."), FText::FromString(LexToString(*Seed))).ToString();
			return false;
		}
		const int64 MaxSeed = MaxFixedSeed(Variants);
		if (*Seed > MaxSeed)
		{
			OutError = FText::Format(
				NSLOCTEXT("DreamTextureGenerate", "SeedTooLarge", "The seed is too large: the maximum is {0} (2^53-1, leaving room for a +1 for each of the following {1} {1}|plural(one=variant,other=variants)), but received {2}."),
				FText::FromString(LexToString(MaxSeed)), Variants - 1, FText::FromString(LexToString(*Seed))).ToString();
			return false;
		}
	}

	// 其余整数参数也要过 JSON 的安全区间：我们是用 double 写出去的，
	// 超过 2^53 的整数写出去就已经不是原来的数了。
	for (const TPair<FString, int64>& Pair : IntParams)
	{
		if (Pair.Key != SeedKey() && FMath::Abs(Pair.Value) > DreamTextureGenerateLimits::MaxSafeSeed)
		{
			OutError = FText::Format(NSLOCTEXT("DreamTextureGenerate", "IntParamOutOfRange", "The value {1} of parameter '{0}' is outside the range JSON can represent exactly (±2^53-1)."), FText::FromString(Pair.Key), FText::FromString(LexToString(Pair.Value))).ToString();
			return false;
		}
	}

	return DerivedFrom.Validate(OutError);
}

bool FDreamTextureGenerateRequest::ToJsonString(FString& OutJson, FString& OutError) const
{
	if (!Validate(OutError))
	{
		return false;
	}

	const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Pair : StringParams)
	{
		Params->SetStringField(Pair.Key, Pair.Value);
	}
	for (const TPair<FString, int64>& Pair : IntParams)
	{
		// |值| ≤ 2^53-1，转 double 是精确的；JSON 写出时用 17 位有效数字，整数原样写出。
		Params->SetNumberField(Pair.Key, static_cast<double>(Pair.Value));
	}
	for (const TPair<FString, double>& Pair : FloatParams)
	{
		Params->SetNumberField(Pair.Key, Pair.Value);
	}
	for (const TPair<FString, bool>& Pair : BoolParams)
	{
		Params->SetBoolField(Pair.Key, Pair.Value);
	}

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("workflow_id"), WorkflowId.TrimStartAndEnd());
	Root->SetObjectField(TEXT("params"), Params);
	Root->SetNumberField(TEXT("variants"), Variants);
	if (!Name.IsEmpty())
	{
		Root->SetStringField(TEXT("name"), Name);
	}

	if (DerivedFrom.IsSet())
	{
		const TSharedRef<FJsonObject> Derived = MakeShared<FJsonObject>();
		Derived->SetStringField(TEXT("kind"), DerivedFrom.Kind);
		if (!DerivedFrom.AssetPath.IsEmpty())
		{
			Derived->SetStringField(TEXT("asset_path"), DerivedFrom.AssetPath);
		}
		if (!DerivedFrom.Engine.IsEmpty())
		{
			Derived->SetStringField(TEXT("engine"), DerivedFrom.Engine);
		}
		Derived->SetStringField(TEXT("mode"), DerivedFrom.Mode);
		if (DerivedFrom.MapsIn.Num() > 0)
		{
			const TSharedRef<FJsonObject> Maps = MakeShared<FJsonObject>();
			for (const TPair<FString, FString>& Pair : DerivedFrom.MapsIn)
			{
				Maps->SetStringField(Pair.Key, Pair.Value);
			}
			Derived->SetObjectField(TEXT("maps_in"), Maps);
		}
		Root->SetObjectField(TEXT("derived_from"), Derived);
	}

	OutJson = SerializeCondensed(Root);
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 任务

const TCHAR* LexToString(EDreamTextureJobStatus Status)
{
	switch (Status)
	{
	case EDreamTextureJobStatus::Queued:
		return TEXT("queued");
	case EDreamTextureJobStatus::Running:
		return TEXT("running");
	case EDreamTextureJobStatus::Succeeded:
		return TEXT("succeeded");
	case EDreamTextureJobStatus::Failed:
		return TEXT("failed");
	case EDreamTextureJobStatus::Canceled:
		return TEXT("canceled");
	default:
		break;
	}
	return TEXT("unknown");
}

EDreamTextureJobStatus ParseJobStatus(const FString& Text)
{
	if (Text == TEXT("queued"))
	{
		return EDreamTextureJobStatus::Queued;
	}
	if (Text == TEXT("running"))
	{
		return EDreamTextureJobStatus::Running;
	}
	if (Text == TEXT("succeeded"))
	{
		return EDreamTextureJobStatus::Succeeded;
	}
	if (Text == TEXT("failed"))
	{
		return EDreamTextureJobStatus::Failed;
	}
	if (Text == TEXT("canceled"))
	{
		return EDreamTextureJobStatus::Canceled;
	}
	return EDreamTextureJobStatus::Unknown;
}

bool FDreamTextureJob::IsTerminal() const
{
	return Status == EDreamTextureJobStatus::Succeeded || Status == EDreamTextureJobStatus::Failed
		|| Status == EDreamTextureJobStatus::Canceled;
}

FText FDreamTextureJob::LocalizeStage(const FString& Stage)
{
	// 逐字、区分大小写地比较后端原文。FString 的 == 不区分大小写，这里对中文无所谓，但写明白。
	const auto Is = [&Stage](const TCHAR* Backend) { return Stage.Equals(Backend, ESearchCase::CaseSensitive); };

	if (Is(TEXT("提交中"))) // I18N-EXEMPT: 后端的阶段名原文，逐字比较后换成本地化文字
	{
		return NSLOCTEXT("DreamTextureJobStage", "Submitting", "Submitting");
	}
	if (Is(TEXT("等待 ComfyUI"))) // I18N-EXEMPT: 后端的阶段名原文，逐字比较后换成本地化文字
	{
		return NSLOCTEXT("DreamTextureJobStage", "WaitingForComfy", "Waiting for ComfyUI");
	}
	if (Is(TEXT("生成中"))) // I18N-EXEMPT: 后端的阶段名原文，逐字比较后换成本地化文字
	{
		return NSLOCTEXT("DreamTextureJobStage", "Generating", "Generating");
	}
	if (Is(TEXT("云端出图"))) // I18N-EXEMPT: 后端的阶段名原文，逐字比较后换成本地化文字
	{
		return NSLOCTEXT("DreamTextureJobStage", "CloudImage", "Generating cloud image");
	}
	if (Is(TEXT("落盘中"))) // I18N-EXEMPT: 后端的阶段名原文，逐字比较后换成本地化文字
	{
		return NSLOCTEXT("DreamTextureJobStage", "Saving", "Saving");
	}
	if (Is(TEXT("完成"))) // I18N-EXEMPT: 后端的阶段名原文，逐字比较后换成本地化文字
	{
		return NSLOCTEXT("DreamTextureJobStage", "Done", "Done");
	}
	return FText::FromString(Stage);
}

bool FDreamTextureJob::IsPlainCancelReason(const FString& Error)
{
	// 后端 job/runner.go 取消排队中和运行中的任务时都写这一句。
	return Error.IsEmpty() || Error.Equals(TEXT("已取消"), ESearchCase::CaseSensitive); // I18N-EXEMPT: 后端取消任务时写的原文，逐字比较，不显示
}

int32 FDreamTextureJob::GetRank() const
{
	switch (Status)
	{
	case EDreamTextureJobStatus::Queued:
		return 1;
	case EDreamTextureJobStatus::Running:
		return 2;
	case EDreamTextureJobStatus::Succeeded:
	case EDreamTextureJobStatus::Failed:
	case EDreamTextureJobStatus::Canceled:
		return 3;
	default:
		return 0;
	}
}

bool FDreamTextureJob::FromJson(const TSharedPtr<FJsonObject>& Obj, FDreamTextureJob& Out)
{
	if (!Obj.IsValid())
	{
		return false;
	}

	FDreamTextureJob Parsed;
	Parsed.Id = ReadString(Obj, TEXT("id"));
	if (Parsed.Id.IsEmpty())
	{
		return false;
	}

	Parsed.MaterialId = ReadString(Obj, TEXT("material_id"));
	Parsed.WorkflowId = ReadString(Obj, TEXT("workflow_id"));
	Parsed.BatchId = ReadString(Obj, TEXT("batch_id"));
	Parsed.PromptId = ReadString(Obj, TEXT("prompt_id"));
	Parsed.Status = ParseJobStatus(ReadString(Obj, TEXT("status")));
	Parsed.Stage = ReadString(Obj, TEXT("stage"));
	Parsed.Error = ReadString(Obj, TEXT("error"));

	double Progress = 0.0;
	if (Obj->TryGetNumberField(TEXT("progress"), Progress))
	{
		Parsed.Progress = static_cast<float>(FMath::Clamp(Progress, 0.0, 1.0));
	}

	const FString CreatedAt = ReadString(Obj, TEXT("created_at"));
	if (!CreatedAt.IsEmpty())
	{
		FDateTime::ParseIso8601(*CreatedAt, Parsed.CreatedAt);
	}

	// params 是提交时的参数：后端已经把「随机种子」换成了具体的数，所以这里读到的就是这个变体真正用的种子。
	// 种子 < 2^53，double 里是精确的。
	const TSharedPtr<FJsonObject>* Params = nullptr;
	if (Obj->TryGetObjectField(TEXT("params"), Params) && Params != nullptr)
	{
		int64 Seed = 0;
		if ((*Params)->TryGetNumberField(TEXT("seed"), Seed))
		{
			Parsed.bHasSeed = true;
			Parsed.Seed = Seed;
		}
		(*Params)->TryGetNumberField(TEXT("resolution"), Parsed.Resolution);
		(*Params)->TryGetStringField(TEXT("__name"), Parsed.Name);

		// 回流的出身：后端存进任务参数里，落盘时写进 manifest。读回来是为了能核对「后端真的记下了」。
		const TSharedPtr<FJsonObject>* Derived = nullptr;
		if ((*Params)->TryGetObjectField(TEXT("__derived_from"), Derived) && Derived != nullptr)
		{
			Parsed.DerivedFrom.Kind = ReadString(*Derived, TEXT("kind"));
			Parsed.DerivedFrom.AssetPath = ReadString(*Derived, TEXT("asset_path"));
			Parsed.DerivedFrom.Engine = ReadString(*Derived, TEXT("engine"));
			Parsed.DerivedFrom.Mode = ReadString(*Derived, TEXT("mode"));
			const TSharedPtr<FJsonObject>* Maps = nullptr;
			if ((*Derived)->TryGetObjectField(TEXT("maps_in"), Maps) && Maps != nullptr)
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Maps)->Values)
				{
					FString Fingerprint;
					if (Pair.Value.IsValid() && Pair.Value->TryGetString(Fingerprint))
					{
						Parsed.DerivedFrom.MapsIn.Add(Pair.Key, Fingerprint);
					}
				}
			}
		}
	}

	Out = MoveTemp(Parsed);
	return true;
}

bool FDreamTextureJob::FromEvent(const FDreamTextureEvent& Event, FDreamTextureJob& Out)
{
	if (!Event.Raw.IsValid())
	{
		return false;
	}

	EDreamTextureJobStatus Implied = EDreamTextureJobStatus::Unknown;
	if (Event.Type == TEXT("job.queued"))
	{
		Implied = EDreamTextureJobStatus::Queued;
	}
	else if (Event.Type == TEXT("job.progress"))
	{
		Implied = EDreamTextureJobStatus::Running;
	}
	else if (Event.Type == TEXT("job.done"))
	{
		Implied = EDreamTextureJobStatus::Succeeded;
	}
	else if (Event.Type == TEXT("job.failed"))
	{
		Implied = EDreamTextureJobStatus::Failed;
	}
	else
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* JobObject = nullptr;
	if (!Event.Raw->TryGetObjectField(TEXT("job"), JobObject) || JobObject == nullptr)
	{
		return false;
	}

	FDreamTextureJob Parsed;
	if (!FromJson(*JobObject, Parsed))
	{
		return false;
	}

	// 事件类型只在任务里没带状态（或带了不认识的）时才用来补：以任务自己说的为准——
	// job.failed 里的状态可能是 canceled，那和 failed 在界面上是两种说法。
	if (Parsed.Status == EDreamTextureJobStatus::Unknown)
	{
		Parsed.Status = Implied;
	}

	Out = MoveTemp(Parsed);
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 扩写

bool FDreamTextureRefineModels::FromJson(const TSharedPtr<FJsonObject>& Root, FDreamTextureRefineModels& Out)
{
	if (!Root.IsValid())
	{
		return false;
	}

	FDreamTextureRefineModels Parsed;
	Root->TryGetStringArrayField(TEXT("models"), Parsed.Models);
	Parsed.Default = ReadString(Root, TEXT("default"));
	Parsed.Error = ReadString(Root, TEXT("error"));

	Out = MoveTemp(Parsed);
	return true;
}

bool FDreamTextureRefineResult::FromJson(const TSharedPtr<FJsonObject>& Root, FDreamTextureRefineResult& Out)
{
	if (!Root.IsValid())
	{
		return false;
	}

	FDreamTextureRefineResult Parsed;
	Parsed.Prompt = ReadString(Root, TEXT("prompt"));
	Parsed.Model = ReadString(Root, TEXT("model"));
	Root->TryGetNumberField(TEXT("elapsed_ms"), Parsed.ElapsedMs);

	// 空结果不是结果：模型偶尔会回一个空串，拿它去覆盖用户写的提示词就丢数据了。
	if (Parsed.Prompt.TrimStartAndEnd().IsEmpty())
	{
		return false;
	}

	Out = MoveTemp(Parsed);
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 云端底图的模型清单

bool FDreamTextureImagenModel::FromJson(const TSharedPtr<FJsonObject>& Obj, FDreamTextureImagenModel& Out)
{
	if (!Obj.IsValid())
	{
		return false;
	}

	FDreamTextureImagenModel Parsed;
	Parsed.Id = ReadString(Obj, TEXT("id"));
	if (Parsed.Id.IsEmpty())
	{
		// 没有 id 的模型选了也填不进参数。
		return false;
	}
	Parsed.Label = ReadString(Obj, TEXT("label"));
	Parsed.Note = ReadString(Obj, TEXT("note"));
	Obj->TryGetBoolField(TEXT("known"), Parsed.bKnown);

	Out = MoveTemp(Parsed);
	return true;
}

bool FDreamTextureImagenProvider::FromJson(const TSharedPtr<FJsonObject>& Obj, FDreamTextureImagenProvider& Out)
{
	if (!Obj.IsValid())
	{
		return false;
	}

	FDreamTextureImagenProvider Parsed;
	Parsed.Id = ReadString(Obj, TEXT("id"));
	if (Parsed.Id.IsEmpty())
	{
		return false;
	}
	Parsed.Label = ReadString(Obj, TEXT("label"));
	Obj->TryGetBoolField(TEXT("configured"), Parsed.bConfigured);
	Parsed.Error = ReadString(Obj, TEXT("error"));

	// 没填令牌时 models 是 []，有令牌却列不出来时可能是 null。两种都当没有。
	TArray<TSharedPtr<FJsonObject>> ModelObjects;
	ParseObjectArray(Obj, TEXT("models"), ModelObjects);
	for (const TSharedPtr<FJsonObject>& ModelObject : ModelObjects)
	{
		FDreamTextureImagenModel Model;
		if (FDreamTextureImagenModel::FromJson(ModelObject, Model))
		{
			Parsed.Models.Add(MoveTemp(Model));
		}
	}

	Out = MoveTemp(Parsed);
	return true;
}

const FDreamTextureImagenProvider* FDreamTextureImagenProviders::Find(const FString& Id) const
{
	return Providers.FindByPredicate([&Id](const FDreamTextureImagenProvider& Provider) { return Provider.Id == Id; });
}

bool FDreamTextureImagenProviders::FromJson(const TSharedPtr<FJsonObject>& Root, FDreamTextureImagenProviders& Out)
{
	if (!Root.IsValid())
	{
		return false;
	}

	FDreamTextureImagenProviders Parsed;
	TArray<TSharedPtr<FJsonObject>> ProviderObjects;
	ParseObjectArray(Root, TEXT("providers"), ProviderObjects);
	for (const TSharedPtr<FJsonObject>& ProviderObject : ProviderObjects)
	{
		FDreamTextureImagenProvider Provider;
		if (FDreamTextureImagenProvider::FromJson(ProviderObject, Provider))
		{
			Parsed.Providers.Add(MoveTemp(Provider));
		}
	}

	Out = MoveTemp(Parsed);
	return true;
}

//////////////////////////////////////////////////////////////////////////
// 网络

void FDreamTextureGenerateApi::ListWorkflows(FOnCatalog Done)
{
	const FString URL = UDreamTextureSettings::Get().GetEndpoint(TEXT("/api/workflows"));

	DreamTexture::SendRequest(TEXT("GET"), URL, GenerateApiTimeoutSecs, FString(),
		[Done](const DreamTexture::FHttpOutcome& Outcome)
		{
			if (!Done)
			{
				return;
			}
			if (!Outcome.bOK)
			{
				Done(false, {}, FText::Format(NSLOCTEXT("DreamTextureGenerate", "WorkflowListFailed", "Could not load the workflow list: {0}"), FText::FromString(Outcome.Error)).ToString());
				return;
			}

			FDreamTextureWorkflowCatalog Catalog;
			if (!FDreamTextureWorkflowCatalog::FromJson(DreamTexture::ParseJsonObject(Outcome.Response->GetContentAsString()), Catalog))
			{
				Done(false, {}, NSLOCTEXT("DreamTextureGenerate", "WorkflowListBadJson", "Could not load the workflow list: the backend did not return valid JSON.").ToString());
				return;
			}
			Done(true, MoveTemp(Catalog), FString());
		});
}

void FDreamTextureGenerateApi::Submit(const FDreamTextureGenerateRequest& Request, FOnSubmitted Done)
{
	FString Body;
	FString Error;
	if (!Request.ToJsonString(Body, Error))
	{
		// 不合法的请求不发出去，但回调仍然异步：调用方不用区分「没发」和「发了失败」。
		const FString Message = FText::Format(NSLOCTEXT("DreamTextureGenerate", "SubmitFailed", "Could not submit the job: {0}"), FText::FromString(Error)).ToString();
		DreamTexture::RunNextTick([Done = MoveTemp(Done), Message]()
		{
			if (Done)
			{
				Done(false, TArray<FDreamTextureJob>(), Message);
			}
		});
		return;
	}

	const FString URL = UDreamTextureSettings::Get().GetEndpoint(TEXT("/api/generate"));

	DreamTexture::SendRequest(TEXT("POST"), URL, SubmitTimeoutSecs, Body,
		[Done](const DreamTexture::FHttpOutcome& Outcome)
		{
			if (!Done)
			{
				return;
			}
			if (!Outcome.bOK)
			{
				Done(false, {}, FText::Format(NSLOCTEXT("DreamTextureGenerate", "SubmitFailed", "Could not submit the job: {0}"), FText::FromString(Outcome.Error)).ToString());
				return;
			}

			const TSharedPtr<FJsonObject> Root = DreamTexture::ParseJsonObject(Outcome.Response->GetContentAsString());
			TArray<TSharedPtr<FJsonObject>> JobObjects;
			if (!Root.IsValid())
			{
				Done(false, {}, NSLOCTEXT("DreamTextureGenerate", "SubmitBadJson", "Could not submit the job: the backend did not return valid JSON.").ToString());
				return;
			}
			ParseObjectArray(Root, TEXT("jobs"), JobObjects);

			TArray<FDreamTextureJob> Jobs;
			for (const TSharedPtr<FJsonObject>& JobObject : JobObjects)
			{
				FDreamTextureJob Job;
				if (FDreamTextureJob::FromJson(JobObject, Job))
				{
					Jobs.Add(MoveTemp(Job));
				}
			}
			if (Jobs.Num() == 0)
			{
				// 后端接受了请求却没给任务：没有 id 就没法追踪，不能报成功。
				Done(false, {}, NSLOCTEXT("DreamTextureGenerate", "SubmitNoJobs", "Could not submit the job: the backend accepted the request but returned no jobs.").ToString());
				return;
			}
			Done(true, MoveTemp(Jobs), FString());
		});
}

void FDreamTextureGenerateApi::GetJob(const FString& JobId, FOnJob Done)
{
	const FString URL = UDreamTextureSettings::Get().GetEndpoint(TEXT("/api/jobs/") + DreamTexture::PercentEncode(JobId));

	DreamTexture::SendRequest(TEXT("GET"), URL, GenerateApiTimeoutSecs, FString(),
		[Done, JobId](const DreamTexture::FHttpOutcome& Outcome)
		{
			if (!Done)
			{
				return;
			}
			if (!Outcome.bOK)
			{
				Done(false, {}, FText::Format(NSLOCTEXT("DreamTextureGenerate", "QueryJobFailed", "Could not query job '{0}': {1}"), FText::FromString(JobId), FText::FromString(Outcome.Error)).ToString(), Outcome.StatusCode == 404);
				return;
			}

			FDreamTextureJob Job;
			if (!FDreamTextureJob::FromJson(DreamTexture::ParseJsonObject(Outcome.Response->GetContentAsString()), Job))
			{
				Done(false, {}, FText::Format(NSLOCTEXT("DreamTextureGenerate", "QueryJobNotAJob", "Could not query job '{0}': the backend did not return a job."), FText::FromString(JobId)).ToString(), false);
				return;
			}
			Done(true, MoveTemp(Job), FString(), false);
		});
}

void FDreamTextureGenerateApi::CancelJob(const FString& JobId, FOnDone Done)
{
	const FString URL = UDreamTextureSettings::Get().GetEndpoint(
		TEXT("/api/jobs/") + DreamTexture::PercentEncode(JobId) + TEXT("/cancel"));

	// 后端不读请求体。发个空对象只是不想让一个 Content-Length 为 0 的 POST 去碰各平台 HTTP 栈的边角。
	DreamTexture::SendRequest(TEXT("POST"), URL, GenerateApiTimeoutSecs, TEXT("{}"),
		[Done](const DreamTexture::FHttpOutcome& Outcome)
		{
			if (!Done)
			{
				return;
			}
			if (!Outcome.bOK)
			{
				Done(false, FText::Format(NSLOCTEXT("DreamTextureGenerate", "CancelFailed", "Could not cancel the job: {0}"), FText::FromString(Outcome.Error)).ToString());
				return;
			}
			Done(true, FString());
		});
}

void FDreamTextureGenerateApi::ListRefineModels(FOnRefineModels Done)
{
	const FString URL = UDreamTextureSettings::Get().GetEndpoint(TEXT("/api/prompts/models"));

	DreamTexture::SendRequest(TEXT("GET"), URL, ModelListTimeoutSecs, FString(),
		[Done](const DreamTexture::FHttpOutcome& Outcome)
		{
			if (!Done)
			{
				return;
			}
			if (!Outcome.bOK)
			{
				Done(false, {}, FText::Format(NSLOCTEXT("DreamTextureGenerate", "RefineModelsFailed", "Could not load the Refine prompt model list: {0}"), FText::FromString(Outcome.Error)).ToString());
				return;
			}

			FDreamTextureRefineModels Models;
			if (!FDreamTextureRefineModels::FromJson(DreamTexture::ParseJsonObject(Outcome.Response->GetContentAsString()), Models))
			{
				Done(false, {}, NSLOCTEXT("DreamTextureGenerate", "RefineModelsBadJson", "Could not load the Refine prompt model list: the backend did not return valid JSON.").ToString());
				return;
			}
			Done(true, MoveTemp(Models), FString());
		});
}

void FDreamTextureGenerateApi::RefinePrompt(const FString& Prompt, EDreamTextureRefinePurpose Purpose, const FString& Model, FOnRefined Done)
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("prompt"), Prompt);
	Root->SetStringField(TEXT("purpose"), Purpose == EDreamTextureRefinePurpose::Texture ? TEXT("texture") : TEXT("image"));
	if (!Model.TrimStartAndEnd().IsEmpty())
	{
		Root->SetStringField(TEXT("model"), Model.TrimStartAndEnd());
	}

	const FString URL = UDreamTextureSettings::Get().GetEndpoint(TEXT("/api/prompts/refine"));

	DreamTexture::SendRequest(TEXT("POST"), URL, RefineTimeoutSecs, SerializeCondensed(Root),
		[Done](const DreamTexture::FHttpOutcome& Outcome)
		{
			if (!Done)
			{
				return;
			}
			if (!Outcome.bOK)
			{
				Done(false, {}, FText::Format(NSLOCTEXT("DreamTextureGenerate", "RefineFailed", "Refine prompt failed: {0}"), FText::FromString(Outcome.Error)).ToString());
				return;
			}

			FDreamTextureRefineResult Result;
			if (!FDreamTextureRefineResult::FromJson(DreamTexture::ParseJsonObject(Outcome.Response->GetContentAsString()), Result))
			{
				Done(false, {}, NSLOCTEXT("DreamTextureGenerate", "RefineEmpty", "Refine prompt failed: the model returned an empty result.").ToString());
				return;
			}
			Done(true, MoveTemp(Result), FString());
		});
}

void FDreamTextureGenerateApi::ListImagenProviders(FOnImagenProviders Done)
{
	const FString URL = UDreamTextureSettings::Get().GetEndpoint(TEXT("/api/imagen/providers"));

	// 后端要现问外部网关列模型，比本机接口慢；与扩写模型列表同一档超时。
	DreamTexture::SendRequest(TEXT("GET"), URL, ModelListTimeoutSecs, FString(),
		[Done](const DreamTexture::FHttpOutcome& Outcome)
		{
			if (!Done)
			{
				return;
			}
			if (!Outcome.bOK)
			{
				Done(false, {}, FText::Format(NSLOCTEXT("DreamTextureGenerate", "CloudModelsFailed", "Could not load the cloud model list: {0}"), FText::FromString(Outcome.Error)).ToString());
				return;
			}

			FDreamTextureImagenProviders Providers;
			if (!FDreamTextureImagenProviders::FromJson(DreamTexture::ParseJsonObject(Outcome.Response->GetContentAsString()), Providers))
			{
				Done(false, {}, NSLOCTEXT("DreamTextureGenerate", "CloudModelsBadJson", "Could not load the cloud model list: the backend did not return valid JSON.").ToString());
				return;
			}
			Done(true, MoveTemp(Providers), FString());
		});
}
