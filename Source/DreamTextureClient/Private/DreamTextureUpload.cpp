// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureUpload.h"

#include "DreamTextureHttpUtil.h"
#include "DreamTextureSettings.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace
{
	/** 上传 32 MB 以内的本机文件，给得宽一点：后端还要转存给 ComfyUI。 */
	constexpr float UploadTimeoutSecs = 120.0f;

	void FailNextTick(FDreamTextureUploadApi::FOnUploaded Done, const FString& Message)
	{
		// 不发请求的失败也走下一帧：调用方不用区分「没发」和「发了失败」。
		DreamTexture::RunNextTick([Done = MoveTemp(Done), Message]()
		{
			if (Done)
			{
				Done(false, FString(), Message);
			}
		});
	}

	void AppendAscii(TArray<uint8>& Out, const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}
}

bool FDreamTextureUploadApi::IsSupportedExtension(const FString& FileName)
{
	const FString Extension = FPaths::GetExtension(FileName, /*bIncludeDot*/ true).ToLower();
	return Extension == TEXT(".png") || Extension == TEXT(".jpg") || Extension == TEXT(".jpeg") || Extension == TEXT(".webp") || Extension == TEXT(".bmp");
}

FString FDreamTextureUploadApi::GuessContentType(const FString& FileName)
{
	const FString Extension = FPaths::GetExtension(FileName, true).ToLower();
	if (Extension == TEXT(".png"))
	{
		return TEXT("image/png");
	}
	if (Extension == TEXT(".jpg") || Extension == TEXT(".jpeg"))
	{
		return TEXT("image/jpeg");
	}
	if (Extension == TEXT(".webp"))
	{
		return TEXT("image/webp");
	}
	if (Extension == TEXT(".bmp"))
	{
		return TEXT("image/bmp");
	}
	return TEXT("application/octet-stream");
}

void FDreamTextureUploadApi::BuildMultipartBody(const FString& Boundary, const FString& FieldName, const FString& FileName,
	const FString& ContentType, TConstArrayView<uint8> Bytes, TArray<uint8>& OutBody)
{
	// 头部里的引号和换行会让后端把一个字段切成两个；文件名多半是我们自己起的，但这是个通用函数。
	FString SafeName = FileName;
	SafeName.ReplaceInline(TEXT("\""), TEXT("_"));
	SafeName.ReplaceInline(TEXT("\r"), TEXT("_"));
	SafeName.ReplaceInline(TEXT("\n"), TEXT("_"));

	OutBody.Reset();
	OutBody.Reserve(Bytes.Num() + 512);

	AppendAscii(OutBody, FString::Printf(TEXT("--%s\r\n"), *Boundary));
	AppendAscii(OutBody, FString::Printf(TEXT("Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"), *FieldName, *SafeName));
	AppendAscii(OutBody, FString::Printf(TEXT("Content-Type: %s\r\n\r\n"), *ContentType));
	OutBody.Append(Bytes.GetData(), Bytes.Num());
	AppendAscii(OutBody, FString::Printf(TEXT("\r\n--%s--\r\n"), *Boundary));
}

bool FDreamTextureUploadApi::ParseResponse(const FString& Json, FString& OutStoredName)
{
	const TSharedPtr<FJsonObject> Root = DreamTexture::ParseJsonObject(Json);
	if (!Root.IsValid())
	{
		return false;
	}
	FString Name;
	if (!Root->TryGetStringField(TEXT("name"), Name) || Name.TrimStartAndEnd().IsEmpty())
	{
		return false;
	}
	OutStoredName = Name;
	return true;
}

void FDreamTextureUploadApi::UploadImage(const FString& FilePath, FOnUploaded Done)
{
	const FString FileName = FPaths::GetCleanFilename(FilePath);

	if (!IsSupportedExtension(FileName))
	{
		FailNextTick(MoveTemp(Done), FText::Format(NSLOCTEXT("DreamTextureUpload", "UnsupportedType", "The backend only accepts PNG, JPG, WebP and BMP files; '{0}' is not one of them."), FText::FromString(FileName)).ToString());
		return;
	}

	const int64 Size = IFileManager::Get().FileSize(*FilePath);
	if (Size < 0)
	{
		FailNextTick(MoveTemp(Done), FText::Format(NSLOCTEXT("DreamTextureUpload", "FileNotFound", "The file to upload was not found: {0}"), FText::FromString(FilePath)).ToString());
		return;
	}
	if (Size > MaxUploadBytes)
	{
		FailNextTick(MoveTemp(Done), FText::Format(NSLOCTEXT("DreamTextureUpload", "TooLarge", "The image is {0} MB, which exceeds the backend's limit of {1} MB, so it was not uploaded."),
			FText::AsNumber(FMath::RoundToInt(static_cast<double>(Size) / (1024.0 * 1024.0) * 10.0) / 10.0), FText::AsNumber(static_cast<int32>(MaxUploadBytes >> 20))).ToString());
		return;
	}

	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *FilePath))
	{
		FailNextTick(MoveTemp(Done), FText::Format(NSLOCTEXT("DreamTextureUpload", "ReadFailed", "The file to upload could not be read: {0}"), FText::FromString(FilePath)).ToString());
		return;
	}

	const FString Boundary = TEXT("----DreamTextureBoundary") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TArray<uint8> Body;
	BuildMultipartBody(Boundary, TEXT("image"), FileName, GuessContentType(FileName), Bytes, Body);
	Bytes.Empty();

	const FString URL = UDreamTextureSettings::Get().GetEndpoint(TEXT("/api/uploads"));

	DreamTexture::SendRequestBytes(TEXT("POST"), URL, UploadTimeoutSecs, FString::Printf(TEXT("multipart/form-data; boundary=%s"), *Boundary), MoveTemp(Body),
		[Done = MoveTemp(Done)](const DreamTexture::FHttpOutcome& Outcome)
		{
			if (!Done)
			{
				return;
			}
			if (!Outcome.bOK)
			{
				Done(false, FString(), FText::Format(NSLOCTEXT("DreamTextureUpload", "UploadFailed", "Upload failed: {0}"), FText::FromString(Outcome.Error)).ToString());
				return;
			}
			FString Stored;
			if (!ParseResponse(Outcome.Response->GetContentAsString(), Stored))
			{
				Done(false, FString(), NSLOCTEXT("DreamTextureUpload", "UploadNoFileName", "Upload failed: the backend's reply contains no file name.").ToString());
				return;
			}
			Done(true, MoveTemp(Stored), FString());
		});
}
