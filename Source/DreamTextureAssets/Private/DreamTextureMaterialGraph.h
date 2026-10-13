// Copyright TypeDreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DreamTextureMaterialAnalysis.h"
#include "Engine/Texture.h"
#include "Engine/EngineTypes.h"

class UMaterialInterface;

/**
 * 材质表达式图的遍历（M7）。只读，不编译材质。
 *
 * 从材质的每个输出属性出发，**逆着连线**往上游走，走到贴图采样节点时记一笔：
 * 这张（实际生效的）贴图、读了它的哪几个通道、最终喂给哪个属性。
 */
namespace DreamTextureGraph
{
	/** 图里的一处使用，已经把材质实例的参数覆盖代入。 */
	struct FRawUse
	{
		/** 实际生效的贴图。参数没赋值时为 nullptr（此时不记入结果，只留一条警告）。 */
		UTexture* Texture = nullptr;

		FName Parameter;

		/** 所选材质实例自己覆盖了这个参数。 */
		bool bOverriddenBySelected = false;

		/** 采样节点上填的采样器类型。 */
		EMaterialSamplerType SamplerType = SAMPLERTYPE_Color;

		FDreamTextureTextureUse Use;
	};

	struct FWalkResult
	{
		TArray<FRawUse> Uses;
		TArray<FString> Warnings;
	};

	/**
	 * 遍历 Material 的图。材质实例沿父链找到根材质的图，参数值取实例（及中间各层）覆盖后的结果。
	 * **游戏线程。**
	 */
	FWalkResult Walk(UMaterialInterface* Material);

	/** 小写、去掉空格/下划线/连字符。 */
	FString NormalizePinName(const FString& Name);
}
