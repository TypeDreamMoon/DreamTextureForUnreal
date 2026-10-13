// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureApi.h"

#include "DreamTextureHttpUtil.h"
#include "DreamTextureSettings.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/*
 * 线程假设：回调都在游戏线程（见 DreamTextureHttpUtil.h 顶部）。
 *
 * 这一层是「薄封装」：只负责拼地址、发请求、把 JSON 变成结构体，不缓存、不重试、不管调用方的生命周期。
 * 基地址每次现取项目设置，用户在设置里改了地址，下一个请求立刻生效。
 */

namespace
{
	/** 列表、详情、收藏这些小请求的超时（秒）。后端在本机，正常是毫秒级；给 15 秒留给冷启动。 */
	constexpr float ApiTimeoutSecs = 15.0f;

	/** 缩略图 / 预览图这种整个读进内存的文件。比 API 宽松，但不该是几分钟。 */
	constexpr float SmallFileTimeoutSecs = 60.0f;

	/** 列表接口的 limit 上限。后端 >500 会静默改成 60，这里提前夹住，免得调用方以为自己要到了 500。 */
	constexpr int32 MaxListLimit = 500;
}

void FDreamTextureApi::ListMaterials(const FDreamTextureMaterialQuery& Query, FOnList Done)
{
	// 空参数不拼进去：后端对缺省值的处理就是「不过滤」，拼个空的 q= 没有任何好处。
	TArray<FString> Params;

	const FString Text = Query.Text.TrimStartAndEnd();
	if (!Text.IsEmpty())
	{
		Params.Add(TEXT("q=") + DreamTexture::PercentEncode(Text));
	}
	if (!Query.Style.IsEmpty())
	{
		Params.Add(TEXT("style=") + DreamTexture::PercentEncode(Query.Style));
	}
	if (Query.bFavoriteOnly)
	{
		Params.Add(TEXT("fav=1"));
	}
	if (Query.Limit > 0)
	{
		Params.Add(FString::Printf(TEXT("limit=%d"), FMath::Min(Query.Limit, MaxListLimit)));
	}
	if (Query.Offset > 0)
	{
		Params.Add(FString::Printf(TEXT("offset=%d"), Query.Offset));
	}

	FString URL = UDreamTextureSettings::Get().GetEndpoint(TEXT("/api/materials"));
	if (Params.Num() > 0)
	{
		URL += TEXT("?") + FString::Join(Params, TEXT("&"));
	}

	DreamTexture::SendRequest(TEXT("GET"), URL, ApiTimeoutSecs, FString(),
		[Done](const DreamTexture::FHttpOutcome& Outcome)
		{
			if (!Done)
			{
				return;
			}
			if (!Outcome.bOK)
			{
				Done(false, {}, FText::Format(NSLOCTEXT("DreamTextureApi", "ListFailed", "Could not load the Library list: {0}"), FText::FromString(Outcome.Error)).ToString());
				return;
			}

			const TSharedPtr<FJsonObject> Root = DreamTexture::ParseJsonObject(Outcome.Response->GetContentAsString());
			if (!Root.IsValid())
			{
				Done(false, {}, NSLOCTEXT("DreamTextureApi", "ListBadJson", "Could not load the Library list: the backend did not return valid JSON.").ToString());
				return;
			}

			// materials 为空时后端很可能给 null（Go 的 nil 切片），缺失、null、[] 都是「没有素材」，
			// 不是错误。TryGetArrayField 对 null 返回 false，正好落到空列表上。
			TArray<FDreamTextureMaterialSummary> Materials;
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (Root->TryGetArrayField(TEXT("materials"), Items) && Items != nullptr)
			{
				Materials.Reserve(Items->Num());
				for (const TSharedPtr<FJsonValue>& Item : *Items)
				{
					const TSharedPtr<FJsonObject>* ItemObject = nullptr;
					if (!Item.IsValid() || !Item->TryGetObject(ItemObject) || ItemObject == nullptr)
					{
						continue;
					}

					// 解不出来的行（没有 id）直接丢：整页列表不该因为一行坏数据全部作废。
					FDreamTextureMaterialSummary Summary;
					if (FDreamTextureMaterialSummary::FromJson(*ItemObject, Summary))
					{
						Materials.Add(MoveTemp(Summary));
					}
				}
			}

			Done(true, MoveTemp(Materials), FString());
		});
}

void FDreamTextureApi::GetManifest(const FString& MaterialId, FOnManifest Done)
{
	const FString URL = UDreamTextureSettings::Get().GetEndpoint(
		TEXT("/api/materials/") + DreamTexture::PercentEncode(MaterialId));

	DreamTexture::SendRequest(TEXT("GET"), URL, ApiTimeoutSecs, FString(),
		[Done, MaterialId](const DreamTexture::FHttpOutcome& Outcome)
		{
			if (!Done)
			{
				return;
			}
			if (!Outcome.bOK)
			{
				Done(false, {}, FText::Format(NSLOCTEXT("DreamTextureApi", "ReadMaterialFailed", "Could not read material set '{0}': {1}"), FText::FromString(MaterialId), FText::FromString(Outcome.Error)).ToString());
				return;
			}

			// 响应体是 {"manifest":{...},"index":{...}|null}。index 是素材库的索引行，
			// 这里用不上（收藏状态走列表接口），只取 manifest。
			const TSharedPtr<FJsonObject> Root = DreamTexture::ParseJsonObject(Outcome.Response->GetContentAsString());
			const TSharedPtr<FJsonObject>* ManifestObject = nullptr;
			if (!Root.IsValid() || !Root->TryGetObjectField(TEXT("manifest"), ManifestObject) || ManifestObject == nullptr)
			{
				Done(false, {}, FText::Format(NSLOCTEXT("DreamTextureApi", "ReadMaterialNoManifest", "Could not read material set '{0}': the backend response contains no manifest."), FText::FromString(MaterialId)).ToString());
				return;
			}

			FDreamTextureManifest Manifest;
			FString ParseError;
			if (!FDreamTextureManifest::FromJson(*ManifestObject, Manifest, ParseError))
			{
				Done(false, {}, FText::Format(NSLOCTEXT("DreamTextureApi", "ReadMaterialFailed", "Could not read material set '{0}': {1}"), FText::FromString(MaterialId), FText::FromString(ParseError)).ToString());
				return;
			}

			// 目录名就是 ID，URL 里请求的也是它。manifest 里的 id 字段万一缺了，用请求的这个补上，
			// 下游（下载落盘、资产命名）不用各自再处理一遍空 ID。
			if (Manifest.Id.IsEmpty())
			{
				Manifest.Id = MaterialId;
			}

			Done(true, MoveTemp(Manifest), FString());
		});
}

void FDreamTextureApi::SetFavorite(const FString& MaterialId, bool bFavorite, FOnDone Done)
{
	const FString URL = UDreamTextureSettings::Get().GetEndpoint(
		TEXT("/api/materials/") + DreamTexture::PercentEncode(MaterialId) + TEXT("/favorite"));
	const FString Body = bFavorite ? TEXT("{\"favorite\":true}") : TEXT("{\"favorite\":false}");

	DreamTexture::SendRequest(TEXT("POST"), URL, ApiTimeoutSecs, Body,
		[Done](const DreamTexture::FHttpOutcome& Outcome)
		{
			if (!Done)
			{
				return;
			}
			if (!Outcome.bOK)
			{
				Done(false, FText::Format(NSLOCTEXT("DreamTextureApi", "FavoriteFailed", "Could not change the favorite state: {0}"), FText::FromString(Outcome.Error)).ToString());
				return;
			}
			Done(true, FString());
		});
}

void FDreamTextureApi::FetchFile(const FString& MaterialId, const FString& FileName, FOnBytes Done)
{
	const FString URL = MakeFileURL(MaterialId, FileName);

	DreamTexture::SendRequest(TEXT("GET"), URL, SmallFileTimeoutSecs, FString(),
		[Done, FileName](const DreamTexture::FHttpOutcome& Outcome)
		{
			if (!Done)
			{
				return;
			}
			if (!Outcome.bOK)
			{
				Done(false, {}, FText::Format(NSLOCTEXT("DreamTextureApi", "FetchFileFailed", "Could not fetch file '{0}': {1}"), FText::FromString(FileName), FText::FromString(Outcome.Error)).ToString());
				return;
			}

			// GetContent() 是应答对象内部的缓冲，借出来的引用不能越过这次回调，所以拷一份给调用方。
			// 这个入口是给缩略图用的，几十 KB 的拷贝可以忽略；大文件走 FDreamTextureFetcher。
			TArray<uint8> Bytes = Outcome.Response->GetContent();
			Done(true, MoveTemp(Bytes), FString());
		});
}

FString FDreamTextureApi::MakeFileURL(const FString& MaterialId, const FString& FileName)
{
	// 两段都要编码：这两个值最终来自后端的数据，不能默认它们干净。
	// 路径穿越由后端的 safeSegment 挡着，本地落盘那一侧由 FDreamTextureFetcher::IsSafePathSegment 挡着，
	// 这里只负责不让它们的字符破坏 URL 结构。
	return UDreamTextureSettings::Get().GetEndpoint(
		TEXT("/api/materials/") + DreamTexture::PercentEncode(MaterialId)
		+ TEXT("/files/") + DreamTexture::PercentEncode(FileName));
}
