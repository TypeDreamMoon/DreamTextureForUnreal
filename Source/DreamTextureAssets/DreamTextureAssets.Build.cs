// Copyright TypeDreamMoon. All Rights Reserved.

using UnrealBuildTool;

/// 资产落地：把后端的材质套装变成工程里的资产。
///
/// M1 是贴图导入；M2 的母材质与材质实例、M3 的材质槽赋值、M7 的材质解析与导出
/// 都会落在这里。它可以依赖 UnrealEd，但**不许依赖任何 Slate 模块**——
/// 资产逻辑要能在没有界面的自动化测试里跑，通知、内容浏览器跳转这些是界面模块的事。
public class DreamTextureAssets : ModuleRules
{
	public DreamTextureAssets(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"DreamTextureClient",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"AssetTools",
			"AssetRegistry",
			"ImageWrapper",
			"ImageCore",
			"DeveloperSettings",
			// ApplyDefaultsForNewlyImportedTextures：新导入贴图该有的默认值（新式 mip 滤波、非 2 的幂的处理…）
			// 引擎自己的导入流程都过这一道，我们不走 Factory，得自己补上。
			"TextureUtilitiesCommon",
			// 母材质与材质实例（M2）：UMaterialEditingLibrary 管表达式的创建、连线、重编译和实例参数的写入。
			// 它是 Editor 模块，不是 Slate 模块——本模块仍然不直接依赖任何 Slate。
			"MaterialEditor",
			// 渲染测试（DreamTexture.Render.*）：问 RHI 能不能渲染、等着色器编译、刷新渲染线程。
			"RHI",
			"RenderCore",
			// 自动化测试要定位插件目录里的固定素材。
			"Projects",
			// 材质回流的用户指认（Saved/DreamTexture/ReflowOverrides.json）。
			"Json",
		});
	}
}
