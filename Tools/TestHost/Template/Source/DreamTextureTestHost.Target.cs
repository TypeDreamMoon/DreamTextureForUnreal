// Copyright (c) 2026 TypeDreamMoon. All rights reserved.

using UnrealBuildTool;
using System.Collections.Generic;

/*
 * The game target of the DreamTextureForUnreal test host.
 *
 * Nothing builds this one: the plugin's tests run entirely in the editor (automation tests).
 * It exists so the host is an ordinary, complete project whose project files generate.
 *
 * Do not build it casually. A game target against a source engine is monolithic, and a monolithic target
 * defaults to a UNIQUE build environment, which compiles the whole engine again into this project.
 */
public class DreamTextureTestHostTarget : TargetRules
{
	public DreamTextureTestHostTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
		ExtraModuleNames.Add("DreamTextureTestHost");
	}
}
