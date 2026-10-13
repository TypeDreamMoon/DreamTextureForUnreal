// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureAnalyzeTestUtils.h"
#include "DreamTextureMaterialAnalysis.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"

using namespace DreamTextureTestUtils;

/*
 * 指认记录（FDreamTextureOverrideStore）读失败时的保护。
 *
 * 文件在、但读不出来（被别的进程独占着、权限、磁盘错误……）：旧写法把它当成空记录用，并把「已加载」置上，
 * 于是下一次 Set 就把一份空的（外加这一条）写回去，用户攒下的全部指认没了。现在读失败不置「已加载」，
 * 写操作一律拒绝并给原因；文件能读之后下一次访问重新读盘。
 */

namespace
{
	/**
	 * 独占着这个文件：以不允许别人读的方式打开它（Windows 上是共享模式 0）。
	 * 句柄活着期间，别的 FFileHelper::LoadFileToString 读它会失败——这就是「文件在、读不出来」。
	 */
	TUniquePtr<IFileHandle> HoldExclusively(const FString& Path)
	{
		return TUniquePtr<IFileHandle>(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*Path, /*bAppend*/ true, /*bAllowRead*/ false));
	}

	FString OverrideId(EDreamTextureRole Role)
	{
		return LexToString(Role);
	}

	FDreamTextureRoleOverride MakeRole(EDreamTextureRole Role)
	{
		FDreamTextureRoleOverride Override;
		Override.Role = Role;
		return Override;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureOverrideReadFailTest, "DreamTexture.Assets.Analyze.Overrides.ReadFail", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureOverrideReadFailTest::RunTest(const FString& Parameters)
{
	FScopedOverrideFile File;

	// ---- 先攒下两条记录和一条平铺/展开记录，落盘 ----
	{
		FDreamTextureOverrideStore Store(File.Path);
		TestTrue(TEXT("准备：记第一条"), Store.Set(TEXT("/Game/A.A"), TEXT("/Game/T1.T1"), MakeRole(EDreamTextureRole::Normal)));
		TestTrue(TEXT("准备：记第二条"), Store.Set(TEXT("/Game/A.A"), TEXT("/Game/T2.T2"), MakeRole(EDreamTextureRole::Height)));
		TestTrue(TEXT("准备：记平铺/展开"), Store.SetUVKind(TEXT("/Game/A.A"), EDreamTextureUVKind::Unwrapped));
	}
	FString Original;
	if (!TestTrue(TEXT("准备：文件写到了磁盘上"), FFileHelper::LoadFileToString(Original, *File.Path)) || !TestTrue(TEXT("准备：文件不是空的"), Original.Len() > 20))
	{
		return false;
	}

	// ---- 文件被独占：读不出来 ----
	TUniquePtr<IFileHandle> Lock = HoldExclusively(File.Path);
	if (!Lock.IsValid())
	{
		AddInfo(TEXT("已跳过：这台机器上独占不了文件，模拟不了「读不出来」。"));
		return true;
	}
	{
		FString Probe;
		if (FFileHelper::LoadFileToString(Probe, *File.Path))
		{
			AddInfo(TEXT("已跳过：独占着文件时别人照样读得出来，模拟不了「读不出来」。"));
			return true;
		}
	}

	FDreamTextureOverrideStore Store(File.Path);
	FDreamTextureRoleOverride Found;
	EDreamTextureUVKind Kind = EDreamTextureUVKind::Tiled;

	// 读：找不到、说明原因（读不了）。
	TestFalse(TEXT("读不出来时找不到记录"), Store.Find(TEXT("/Game/A.A"), TEXT("/Game/T1.T1"), Found));
	TestTrue(FString::Printf(TEXT("说明了原因：%s"), *Store.GetLastError()), Store.GetLastError().Contains(TEXT("Could not read")));
	TestFalse(TEXT("读不出来时没有平铺/展开记录"), Store.FindUVKind(TEXT("/Game/A.A"), Kind));

	// 写：一律拒绝，并说清楚为什么拒绝、记录还在文件里。
	TestFalse(TEXT("读不出来时 Set 拒绝写"), Store.Set(TEXT("/Game/A.A"), TEXT("/Game/T3.T3"), MakeRole(EDreamTextureRole::Roughness)));
	TestTrue(FString::Printf(TEXT("拒绝的原因说清楚了：%s"), *Store.GetLastError()), Store.GetLastError().Contains(TEXT("nothing was written")) && Store.GetLastError().Contains(TEXT("the existing record")));
	TestFalse(TEXT("读不出来时 Remove 也不做"), Store.Remove(TEXT("/Game/A.A"), TEXT("/Game/T1.T1")));
	TestFalse(TEXT("读不出来时 SetUVKind 拒绝写"), Store.SetUVKind(TEXT("/Game/B.B"), EDreamTextureUVKind::Unwrapped));
	TestFalse(TEXT("拒绝的 SetUVKind 没有在内存里留下记录"), Store.FindUVKind(TEXT("/Game/B.B"), Kind));
	TestFalse(TEXT("拒绝的 Set 没有在内存里留下记录"), Store.Find(TEXT("/Game/A.A"), TEXT("/Game/T3.T3"), Found));

	// ---- 放开文件：同一个 Store 下一次访问重新读盘，原有记录都在，没被空记录覆盖 ----
	Lock.Reset();
	{
		FString Now;
		TestTrue(TEXT("放开之后文件读得出来"), FFileHelper::LoadFileToString(Now, *File.Path));
		TestEqual(TEXT("磁盘上的文件在这期间一个字节都没变"), Now, Original);
	}

	if (TestTrue(TEXT("放开之后同一个 Store 重读到第一条"), Store.Find(TEXT("/Game/A.A"), TEXT("/Game/T1.T1"), Found)))
	{
		TestEqual(TEXT("第一条的角色"), OverrideId(Found.Role), OverrideId(EDreamTextureRole::Normal));
	}
	TestTrue(TEXT("第二条也在"), Store.Find(TEXT("/Game/A.A"), TEXT("/Game/T2.T2"), Found));
	TestTrue(TEXT("平铺/展开记录也在"), Store.FindUVKind(TEXT("/Game/A.A"), Kind) && Kind == EDreamTextureUVKind::Unwrapped);

	// 现在能写了，写的是「原有记录 + 新记录」，不是只有新记录。
	TestTrue(TEXT("放开之后 Set 正常写"), Store.Set(TEXT("/Game/A.A"), TEXT("/Game/T3.T3"), MakeRole(EDreamTextureRole::Roughness)));
	{
		FDreamTextureOverrideStore Fresh(File.Path);
		TestEqual(TEXT("重开之后 A 材质下有三条（原来两条没丢）"), Fresh.NumFor(TEXT("/Game/A.A")), 3);
		TestTrue(TEXT("重开之后平铺/展开记录还在"), Fresh.FindUVKind(TEXT("/Game/A.A"), Kind) && Kind == EDreamTextureUVKind::Unwrapped);
	}

	// ---- 文件不存在不算读失败：第一次写正常 ----
	{
		FScopedOverrideFile Missing;
		FDreamTextureOverrideStore Store2(Missing.Path);
		TestFalse(TEXT("文件不存在：找不到"), Store2.Find(TEXT("/Game/A.A"), TEXT("/Game/T1.T1"), Found));
		TestTrue(TEXT("文件不存在：Set 正常写"), Store2.Set(TEXT("/Game/A.A"), TEXT("/Game/T1.T1"), MakeRole(EDreamTextureRole::AO)));
		TestTrue(TEXT("文件被建出来了"), IFileManager::Get().FileExists(*Missing.Path));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
