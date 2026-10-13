// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureSettings.h"

namespace
{
	/**
	 * 后端默认地址。端口跟后端仓库 configs/dreamtexture.json 里的一致。
	 *
	 * 这个常量同时兼任「用户把地址填坏了」时的兜底值：宁可去连一个默认地址连不上，
	 * 也别拼出个空 URL 让 HTTP 层报一句谁也看不懂的错。
	 */
	const TCHAR* const DefaultBackendURL = TEXT("http://127.0.0.1:8777");
}

UDreamTextureSettings::UDreamTextureSettings()
{
	BackendURL = DefaultBackendURL;
	bAutoStartBackend = false;

	// 这三个数跟 Web 端 store.ts 是同一套，改要两边一起改。
	// 40000 是心跳 15 秒的两拍多一点：短暂的调度停顿不该掐掉一条好连接。
	ReconnectBaseMs = 1000;
	ReconnectMaxMs = 15000;
	StaleAfterMs = 40000;

	ImportRoot = TEXT("/Game/DreamTexture");

	// 默认不导分通道图：ORM 已经覆盖了 roughness / metallic / ao 三张，
	// 母材质也只采样 ORM，多导只是白占内容目录。
	bImportSeparateChannels = false;
	bCreateMaterialInstance = true;
	bAutoImportGenerated = false;
}

const UDreamTextureSettings& UDreamTextureSettings::Get()
{
	// config=Editor 的 UDeveloperSettings 只有 CDO 这一份实例，配置也是写在它身上的。
	const UDreamTextureSettings* Settings = GetDefault<UDreamTextureSettings>();
	check(Settings != nullptr);
	return *Settings;
}

FString UDreamTextureSettings::GetNormalizedBackendURL() const
{
	FString URL = BackendURL.TrimStartAndEnd();

	// 只认 http/https。GetWebSocketURL() 是把 scheme 换掉得到 ws 地址的，
	// 别的协议换不出来；填了 "127.0.0.1:8777" 这种没 scheme 的也一样接不上。
	// 与其让底层报一句天书，不如直接退回默认地址，界面上连不上时给的提示还有救。
	if (!URL.StartsWith(TEXT("http://")) && !URL.StartsWith(TEXT("https://")))
	{
		URL = DefaultBackendURL;
	}

	// 去掉尾巴上的斜杠——可能不止一根。留着的话 GetEndpoint() 会拼出 "//api/health"，
	// Go 的 mux 认不出这条路由，直接 404。
	while (URL.RemoveFromEnd(TEXT("/")))
	{
	}

	// 只填了 "http://" 的话上面会把它啃成空串，再兜一次。
	if (URL.IsEmpty() || URL.EndsWith(TEXT(":")))
	{
		URL = DefaultBackendURL;
	}

	return URL;
}

FString UDreamTextureSettings::GetEndpoint(const FString& Path) const
{
	const FString Base = GetNormalizedBackendURL();
	if (Path.IsEmpty())
	{
		return Base;
	}

	// Base 保证没有尾斜杠，所以这里只用管 Path 有没有头斜杠。
	return Path.StartsWith(TEXT("/")) ? Base + Path : Base + TEXT("/") + Path;
}

FString UDreamTextureSettings::GetWebSocketURL() const
{
	FString URL = GetNormalizedBackendURL();

	// GetNormalizedBackendURL() 已经保证了前缀只可能是这两个之一。
	if (URL.StartsWith(TEXT("https://")))
	{
		URL = TEXT("wss://") + URL.RightChop(8);
	}
	else
	{
		URL = TEXT("ws://") + URL.RightChop(7);
	}

	return URL + TEXT("/api/ws");
}

FName UDreamTextureSettings::GetCategoryName() const
{
	// 落在「项目设置 → 插件 → DreamTexture」下。基类默认会按 config=Editor 猜成
	// Editor 分类，那是引擎自己的地盘，插件不该往里挤。
	return TEXT("Plugins");
}
