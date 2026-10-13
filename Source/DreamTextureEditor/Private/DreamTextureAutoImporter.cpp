// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureAutoImporter.h"

#include "DreamTextureImportQueue.h"
#include "DreamTextureLog.h"
#include "DreamTextureSettings.h"

FDreamTextureAutoImporter::FHooks FDreamTextureAutoImporter::FHooks::MakeDefault()
{
	FHooks Out;
	Out.IsEnabled = []()
	{
		return UDreamTextureSettings::Get().bAutoImportGenerated;
	};
	Out.Enqueue = [](const FString& MaterialId)
	{
		return FDreamTextureImportQueue::Get().Enqueue(MaterialId);
	};
	return Out;
}

FDreamTextureAutoImporter::FDreamTextureAutoImporter(FHooks InHooks)
	: Hooks(MoveTemp(InHooks))
{
}

FDreamTextureAutoImporter::~FDreamTextureAutoImporter()
{
	Stop();
}

void FDreamTextureAutoImporter::Start(const TSharedPtr<FDreamTextureJobTracker>& InTracker)
{
	Stop();
	if (!InTracker.IsValid())
	{
		return;
	}
	Tracker = InTracker;
	Handle = InTracker->OnJobChanged.AddSP(this, &FDreamTextureAutoImporter::HandleJobChanged);
}

void FDreamTextureAutoImporter::Stop()
{
	// 追踪器可能先于我们销毁（模块关闭的顺序），所以先 Pin。
	if (const TSharedPtr<FDreamTextureJobTracker> Pinned = Tracker.Pin())
	{
		Pinned->OnJobChanged.Remove(Handle);
	}
	Tracker.Reset();
	Handle.Reset();
}

void FDreamTextureAutoImporter::HandleJobChanged(const FDreamTextureJob& Job)
{
	if (Job.Status != EDreamTextureJobStatus::Succeeded || Job.MaterialId.IsEmpty() || Handled.Contains(Job.Id))
	{
		return;
	}

	const TSharedPtr<FDreamTextureJobTracker> Pinned = Tracker.Pin();
	if (!Pinned.IsValid() || Pinned->GetOrigin(Job.Id) != FDreamTextureJobTracker::GetDefaultOrigin())
	{
		return;
	}

	// 先记下再看开关：开关关着时完成的任务算「处理过了」，事后打开开关不追认。
	Handled.Add(Job.Id);
	if (!Hooks.IsEnabled || !Hooks.IsEnabled() || !Hooks.Enqueue)
	{
		return;
	}

	++NumTriggered;
	if (!Hooks.Enqueue(Job.MaterialId))
	{
		// 已经在导入队列里（用户抢先点了「导入」）：正合适，不用再说。
		UE_LOG(LogDreamTexture, Verbose, TEXT("Auto import: %s is already in the import queue, skipping."), *Job.MaterialId);
	}
}
