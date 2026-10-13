// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

const TCHAR* LexToString(EDreamTextureLink Link)
{
	// 这里刻意给英文：它是日志和自动化断言用的，跟界面上给人看的中文文案是两回事。
	// 界面自己去做一份中文映射，别把两者绑在一起——改了文案就得改测试很烦。
	switch (Link)
	{
	case EDreamTextureLink::Offline:
		return TEXT("Offline");
	case EDreamTextureLink::Connecting:
		return TEXT("Connecting");
	case EDreamTextureLink::Live:
		return TEXT("Live");
	case EDreamTextureLink::Stale:
		return TEXT("Stale");
	default:
		break;
	}
	return TEXT("Unknown");
}

bool FDreamTextureHealth::FromJson(const TSharedPtr<FJsonObject>& Obj, FDreamTextureHealth& Out)
{
	if (!Obj.IsValid())
	{
		return false;
	}

	// 先解到一份临时的再整体搬过去：解到一半发现字段类型不对的话，
	// Out 至少还是上一次那份完整状态，不会变成半旧半新的四不像。
	FDreamTextureHealth Parsed;

	// 所有 TryGet*Field 在字段缺失、或值是 null 的时候都返回 false 且不动出参，
	// 于是自动落到 Parsed 里的默认值上——这正是我们要的容忍度。
	// Go 那边好几个字段带 omitempty，nil 切片还会序列化成 null 而不是 []，
	// 后端仓库里这个坑踩过两次，别假设字段一定在。
	Obj->TryGetStringField(TEXT("mode"), Parsed.Mode);
	Obj->TryGetBoolField(TEXT("alive"), Parsed.bAlive);
	Obj->TryGetBoolField(TEXT("ready"), Parsed.bReady);
	Obj->TryGetStringField(TEXT("reason"), Parsed.Reason);
	Obj->TryGetStringField(TEXT("comfyui_version"), Parsed.ComfyUIVersion);
	Obj->TryGetStringField(TEXT("device"), Parsed.Device);
	Obj->TryGetNumberField(TEXT("vram_total_mb"), Parsed.VRAMTotalMB);
	Obj->TryGetNumberField(TEXT("vram_free_mb"), Parsed.VRAMFreeMB);
	Obj->TryGetNumberField(TEXT("queue_depth"), Parsed.QueueDepth);
	Obj->TryGetNumberField(TEXT("restarts"), Parsed.Restarts);
	Obj->TryGetBoolField(TEXT("starting"), Parsed.bStarting);
	Obj->TryGetNumberField(TEXT("starting_secs"), Parsed.StartingSecs);
	Obj->TryGetBoolField(TEXT("user_stopped"), Parsed.bUserStopped);

	// base_url / pid / last_checked_at 后端也发，但界面和判定都用不上，不解。
	// 将来要用的话记得 last_checked_at 是 RFC3339 字符串，不是时间戳。

	Out = MoveTemp(Parsed);
	return true;
}

bool FDreamTextureCheck::FromJson(const TSharedPtr<FJsonObject>& Obj, FDreamTextureCheck& Out)
{
	if (!Obj.IsValid())
	{
		return false;
	}

	FDreamTextureCheck Parsed;

	Obj->TryGetStringField(TEXT("key"), Parsed.Key);
	Obj->TryGetStringField(TEXT("label"), Parsed.Label);
	Obj->TryGetStringField(TEXT("status"), Parsed.Status);
	Obj->TryGetStringField(TEXT("detail"), Parsed.Detail);

	// fix 带 omitempty，没有可修复动作时字段整个不出现——空串就是「只能人工处理」。
	Obj->TryGetStringField(TEXT("fix"), Parsed.Fix);

	// items 有三种形态：没这个字段、值是 null、真的是数组。前两种都当空数组，
	// 不算解析失败——一项自检没有明细是很正常的事。
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (Obj->TryGetArrayField(TEXT("items"), Items) && Items != nullptr)
	{
		Parsed.Items.Reserve(Items->Num());
		for (const TSharedPtr<FJsonValue>& Value : *Items)
		{
			FString Item;
			// 数组里混进 null 或者数字也不当错，跳过就是了。
			if (Value.IsValid() && Value->TryGetString(Item))
			{
				Parsed.Items.Add(MoveTemp(Item));
			}
		}
	}

	Out = MoveTemp(Parsed);
	return true;
}
