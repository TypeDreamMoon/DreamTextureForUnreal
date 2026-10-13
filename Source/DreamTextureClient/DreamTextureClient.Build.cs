// Copyright TypeDreamMoon. All Rights Reserved.

using UnrealBuildTool;

public class DreamTextureClient : ModuleRules
{
	public DreamTextureClient(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"DeveloperSettings",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"HTTP",
			"WebSockets",
			"Json",
			"JsonUtilities",
			"Projects",
		});

		// 这个模块刻意不依赖 UnrealEd：它要能脱离编辑器 UI 跑自动化测试，
		// 将来做 commandlet 或 Python 自动化也直接复用。加依赖前先想清楚。
	}
}
