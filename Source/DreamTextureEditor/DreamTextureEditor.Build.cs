// Copyright TypeDreamMoon. All Rights Reserved.

using UnrealBuildTool;

public class DreamTextureEditor : ModuleRules
{
	public DreamTextureEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"DreamTextureClient",
			"DreamTextureAssets",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"InputCore",
			"Slate",
			"SlateCore",
			"UnrealEd",
			"ToolMenus",
			"WorkspaceMenuStructure",
			"Projects",
			// 缩略图：后台线程用 ImageWrapper 解码（见 DreamTextureThumbCache.h）。
			"ImageWrapper",
			// 导入完成的通知里「在内容浏览器中显示」要跳转。
			"ContentBrowser",
			// 通知里「选中材质实例」要按对象路径查 FAssetData。
			"AssetRegistry",
			// 自动化测试（DreamTexture.Live.Push）直接对后端发 REST：列会话、触发「发送到 UE」。
			"HTTP",
			"Json",
		});

		// 面板上的「设置」按钮要 ISettingsModule::ShowViewer 才能跳到项目设置那一页。
		// 只要头文件、不要链接：这个模块是纯接口，运行时用 FModuleManager 按名字取。
		// UnrealEd 自己也是这么引它的（UnrealEd.Build.cs 的 PrivateIncludePathModuleNames），
		// 而且它那份是私有的，传不到我们这儿来，所以得自己写一遍。
		PrivateIncludePathModuleNames.AddRange(new string[]
		{
			"Settings",
		});
	}
}
