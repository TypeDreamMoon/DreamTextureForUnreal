// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureAnalyzeTestUtils.h"
#include "DreamTextureMaterialBuilder.h"
#include "DreamTextureMaterialTestUtils.h"
#include "DreamTextureReflow.h"
#include "DreamTextureSettings.h"
#include "Editor.h"
#include "Editor/Transactor.h"

using namespace DreamTextureTestUtils;

/*
 * 材质回流（M8/M9）「材质这一头」：通道现状、勾选规则、新建与原地覆盖的计划、原地覆盖的执行、平铺/UV 展开的记忆。
 *
 * 计划与勾选是纯数据，用手搓的解析结果测，不碰任何资产；原地覆盖与「插件自己的实例」用真的母材质与实例测，
 * 都不需要着色器（-nullrhi 能跑）。流程那一头（导出→上传→提交→导入）在 DreamTexture.Editor.Reflow.Flow.*。
 */

namespace
{
	FDreamTextureMaterialTexture MakeRow(const TCHAR* Name, EDreamTextureRole Role, std::initializer_list<const TCHAR*> Parameters = {}, bool bExportable = true)
	{
		FDreamTextureMaterialTexture Row;
		Row.TexturePath = FString::Printf(TEXT("/Game/Test/%s.%s"), Name, Name);
		Row.AssetName = Name;
		Row.Role = Role;
		Row.bExportable = bExportable;
		for (const TCHAR* Parameter : Parameters)
		{
			Row.Parameters.Add(FName(Parameter));
		}
		return Row;
	}

	FDreamTextureMaterialTexture MakePacked(const TCHAR* Name, EDreamTextureRole R, EDreamTextureRole G, EDreamTextureRole B, EDreamTextureRole A,
		std::initializer_list<const TCHAR*> Parameters = { TEXT("ORM") })
	{
		FDreamTextureMaterialTexture Row = MakeRow(Name, EDreamTextureRole::Packed, Parameters);
		Row.Layout.Channels[0] = R;
		Row.Layout.Channels[1] = G;
		Row.Layout.Channels[2] = B;
		Row.Layout.Channels[3] = A;
		return Row;
	}

	FDreamTextureMaterialAnalysis MakeAnalysis(TArray<FDreamTextureMaterialTexture> Rows, bool bInstance = true)
	{
		FDreamTextureMaterialAnalysis Analysis;
		Analysis.bOK = true;
		Analysis.bIsInstance = bInstance;
		Analysis.AssetPath = TEXT("/Game/Test/MI_Test.MI_Test");
		Analysis.Textures = MoveTemp(Rows);
		return Analysis;
	}

	const FDreamTextureReflowChannelState* Find(const TArray<FDreamTextureReflowChannelState>& States, const TCHAR* Channel)
	{
		return States.FindByPredicate([Channel](const FDreamTextureReflowChannelState& State) { return State.Channel == Channel; });
	}

	using EPresence = FDreamTextureReflowChannelState::EPresence;

	/** 一个典型的材质：底色（参数）、法线（参数）、标准 ORM（参数），没有高度。 */
	TArray<FDreamTextureReflowChannelState> MakeTypicalStates()
	{
		return FDreamTextureReflowPlanner::Describe(MakeAnalysis({
			MakeRow(TEXT("T_Base"), EDreamTextureRole::BaseColor, { TEXT("BaseColor") }),
			MakeRow(TEXT("T_N"), EDreamTextureRole::Normal, { TEXT("Normal") }),
			MakePacked(TEXT("T_ORM"), EDreamTextureRole::AO, EDreamTextureRole::Roughness, EDreamTextureRole::Metallic, EDreamTextureRole::Unknown),
		}), nullptr);
	}

	TSet<FString> Set(std::initializer_list<const TCHAR*> Channels)
	{
		TSet<FString> Out;
		for (const TCHAR* Channel : Channels)
		{
			Out.Add(Channel);
		}
		return Out;
	}

	FString Join(const TSet<FString>& Set)
	{
		TArray<FString> Items = Set.Array();
		Items.Sort();
		return FString::Join(Items, TEXT(","));
	}

	FString Join(const TArray<FString>& Items)
	{
		return FString::Join(Items, TEXT(","));
	}

	/** 设置里「分通道导入」开关：测试期间固定成给定值，离开时还原。 */
	struct FScopedSeparateChannels
	{
		bool bPrevious;

		explicit FScopedSeparateChannels(bool bValue)
		{
			UDreamTextureSettings* Settings = GetMutableDefault<UDreamTextureSettings>();
			bPrevious = Settings->bImportSeparateChannels;
			Settings->bImportSeparateChannels = bValue;
		}

		~FScopedSeparateChannels()
		{
			GetMutableDefault<UDreamTextureSettings>()->bImportSeparateChannels = bPrevious;
		}
	};

	/**
	 * 撤销栈里「现在能撤销」的条数。不能直接看 GetQueueLength：队列里还躺着已经撤销、等着重做的条目，
	 * 新开一个事务会把它们丢掉，长度的增量就不是 1 了——前面的测试撤销过东西时，后面的测试就会因此误报。
	 */
	int32 NumUndoable()
	{
		return GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount();
	}

	FDreamTextureImportResult ReflowImportFixtureInto(const FDreamTextureManifest& Full, const TMap<FString, FString>& AllFiles, const FString& Root, std::initializer_list<const TCHAR*> Drop = {})
	{
		FDreamTextureImportRequest Request;
		Request.Manifest = Full;
		Request.LocalFiles = AllFiles;
		for (const TCHAR* Channel : Drop)
		{
			Request.Manifest.Maps.Remove(Channel);
			Request.LocalFiles.Remove(Channel);
		}
		Request.ContentRoot = Root;
		Request.bSave = false;
		return FDreamTextureImporter::Import(Request);
	}
}

// ---------------------------------------------------------------------------
// 通道现状
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowDescribeTest, "DreamTexture.Assets.Reflow.Describe", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowDescribeTest::RunTest(const FString& Parameters)
{
	// ---- 典型：底色、法线（参数）、标准 ORM ----
	{
		const TArray<FDreamTextureReflowChannelState> States = MakeTypicalStates();
		TestEqual(TEXT("六个通道，顺序固定"), Join(DreamTextureReflow::GetChannels()), FString(TEXT("basecolor,normal,roughness,metallic,ao,height")));
		if (TestEqual(TEXT("状态与通道一一对应"), States.Num(), 6))
		{
			for (int32 Index = 0; Index < 6; ++Index)
			{
				TestEqual(TEXT("顺序同 GetChannels"), States[Index].Channel, DreamTextureReflow::GetChannels()[Index]);
			}
		}

		const FDreamTextureReflowChannelState* Base = Find(States, TEXT("basecolor"));
		if (TestNotNull(TEXT("底色"), Base))
		{
			TestTrue(TEXT("底色：有专门的贴图"), Base->Presence == EPresence::Dedicated);
			TestEqual(TEXT("底色：贴图路径"), Base->TexturePath, FString(TEXT("/Game/Test/T_Base.T_Base")));
			TestEqual(TEXT("底色：参数"), Base->Parameters.Num(), 1);
		}
		const FDreamTextureReflowChannelState* Height = Find(States, TEXT("height"));
		if (TestNotNull(TEXT("高度"), Height))
		{
			TestTrue(TEXT("高度：缺失"), Height->Presence == EPresence::Missing);
			TestFalse(TEXT("缺失不算存在"), Height->IsPresent());
			TestTrue(TEXT("缺失没有贴图路径"), Height->TexturePath.IsEmpty());
		}
		for (const TCHAR* Channel : { TEXT("roughness"), TEXT("metallic"), TEXT("ao") })
		{
			const FDreamTextureReflowChannelState* State = Find(States, Channel);
			if (TestNotNull(Channel, State))
			{
				TestTrue(FString::Printf(TEXT("%s：在标准 ORM 里"), Channel), State->Presence == EPresence::InStandardORM);
				TestEqual(FString::Printf(TEXT("%s：与 ORM 里另两路成组"), Channel), Join(State->TiedChannels), FString(TEXT("ao,roughness,metallic")));
				TestEqual(FString::Printf(TEXT("%s：贴图是 ORM"), Channel), State->TextureName, FString(TEXT("T_ORM")));
			}
		}
	}

	// ---- 两张法线：只认第一张，说一声 ----
	{
		const TArray<FDreamTextureReflowChannelState> States = FDreamTextureReflowPlanner::Describe(MakeAnalysis({
			MakeRow(TEXT("T_N1"), EDreamTextureRole::Normal, { TEXT("Normal") }),
			MakeRow(TEXT("T_N2"), EDreamTextureRole::Normal, { TEXT("DetailNormal") }),
		}), nullptr);
		const FDreamTextureReflowChannelState* Normal = Find(States, TEXT("normal"));
		if (TestNotNull(TEXT("法线"), Normal))
		{
			TestEqual(TEXT("取解析顺序里的第一张"), Normal->TextureName, FString(TEXT("T_N1")));
			TestEqual(TEXT("记下有两张候选"), Normal->NumCandidates, 2);
			TestTrue(TEXT("附注点明只处理第一张"), Normal->Note.Contains(TEXT("first one")) && Normal->Note.Contains(TEXT("T_N1")));
			TestEqual(TEXT("参数是第一张的"), Normal->Parameters[0].ToString(), FString(TEXT("Normal")));
		}
	}

	// ---- 排布不是标准 ORM：整组都是「在非标准打包图里」，插件不碰 ----
	{
		const TArray<FDreamTextureReflowChannelState> States = FDreamTextureReflowPlanner::Describe(MakeAnalysis({
			MakePacked(TEXT("T_RMA"), EDreamTextureRole::Roughness, EDreamTextureRole::Metallic, EDreamTextureRole::AO, EDreamTextureRole::Unknown),
		}), nullptr);
		for (const TCHAR* Channel : { TEXT("roughness"), TEXT("metallic"), TEXT("ao") })
		{
			const FDreamTextureReflowChannelState* State = Find(States, Channel);
			if (TestNotNull(Channel, State))
			{
				TestTrue(FString::Printf(TEXT("%s：在非标准打包图里"), Channel), State->Presence == EPresence::InOtherPacked);
				TestTrue(FString::Printf(TEXT("%s：附注点明排布不是标准 ORM"), Channel), State->Note.Contains(TEXT("not the standard ORM")));
			}
		}
		TestTrue(TEXT("没出现的通道仍是缺失"), Find(States, TEXT("height"))->Presence == EPresence::Missing);
	}

	// ---- A 通道里存着高度的「ORM」：整张换会毁掉高度，按非标准处理，高度也算在这张图里 ----
	{
		const TArray<FDreamTextureReflowChannelState> States = FDreamTextureReflowPlanner::Describe(MakeAnalysis({
			MakePacked(TEXT("T_ORMH"), EDreamTextureRole::AO, EDreamTextureRole::Roughness, EDreamTextureRole::Metallic, EDreamTextureRole::Height),
		}), nullptr);
		TestTrue(TEXT("ORM 部分按非标准处理"), Find(States, TEXT("roughness"))->Presence == EPresence::InOtherPacked);
		TestTrue(TEXT("高度也在这张图里（不是缺失）"), Find(States, TEXT("height"))->Presence == EPresence::InOtherPacked);
		TestEqual(TEXT("四路成组"), Find(States, TEXT("ao"))->TiedChannels.Num(), 4);
	}

	// ---- A 通道是 Unknown / Other：仍是标准 ORM ----
	for (const EDreamTextureRole A : { EDreamTextureRole::Unknown, EDreamTextureRole::Other })
	{
		const TArray<FDreamTextureReflowChannelState> States = FDreamTextureReflowPlanner::Describe(MakeAnalysis({
			MakePacked(TEXT("T_ORM"), EDreamTextureRole::AO, EDreamTextureRole::Roughness, EDreamTextureRole::Metallic, A),
		}), nullptr);
		TestTrue(TEXT("A 通道没用：标准 ORM"), Find(States, TEXT("roughness"))->Presence == EPresence::InStandardORM);
	}

	// ---- 分开的粗糙度 / 金属度 / AO ----
	{
		const TArray<FDreamTextureReflowChannelState> States = FDreamTextureReflowPlanner::Describe(MakeAnalysis({
			MakeRow(TEXT("T_R"), EDreamTextureRole::Roughness, { TEXT("Roughness") }),
			MakeRow(TEXT("T_M"), EDreamTextureRole::Metallic),
		}), nullptr);
		TestTrue(TEXT("粗糙度：专门的贴图"), Find(States, TEXT("roughness"))->Presence == EPresence::Dedicated);
		TestEqual(TEXT("独立贴图只和自己成组"), Join(Find(States, TEXT("roughness"))->TiedChannels), FString(TEXT("roughness")));
		TestTrue(TEXT("金属度：直接引用，没有参数"), Find(States, TEXT("metallic"))->Parameters.Num() == 0);
		TestTrue(TEXT("AO 缺失"), Find(States, TEXT("ao"))->Presence == EPresence::Missing);
	}

	// ---- 底图候选：角色是底色且能导出的 ----
	{
		const FDreamTextureMaterialAnalysis Analysis = MakeAnalysis({
			MakeRow(TEXT("T_BaseA"), EDreamTextureRole::BaseColor, {}, /*bExportable*/ false),
			MakeRow(TEXT("T_BaseB"), EDreamTextureRole::BaseColor),
			MakeRow(TEXT("T_N"), EDreamTextureRole::Normal),
			MakeRow(TEXT("T_BaseC"), EDreamTextureRole::BaseColor),
		});
		const TArray<const FDreamTextureMaterialTexture*> Sources = FDreamTextureReflowPlanner::FindBaseColorSources(Analysis);
		if (TestEqual(TEXT("两张能导出的底色"), Sources.Num(), 2))
		{
			TestEqual(TEXT("按解析顺序，跳过导不出的"), Sources[0]->AssetName, FString(TEXT("T_BaseB")));
			TestEqual(TEXT("第二张"), Sources[1]->AssetName, FString(TEXT("T_BaseC")));
		}
	}

	// ---- 文案 ----
	TestEqual(TEXT("法线的显示名"), DreamTextureReflow::GetChannelLabel(TEXT("normal")), FString(TEXT("Normal")));
	TestEqual(TEXT("不认识的原样返回"), DreamTextureReflow::GetChannelLabel(TEXT("emissive")), FString(TEXT("emissive")));
	TestEqual(TEXT("模式的标识就是 derived_from.mode"), FString(LexToString(EDreamTextureReflowMode::FillChannels)), FString(TEXT("fill_channels")));
	TestEqual(TEXT("模式的标识就是 derived_from.mode"), FString(LexToString(EDreamTextureReflowMode::Restyle)), FString(TEXT("restyle")));
	return true;
}

// ---------------------------------------------------------------------------
// 勾选
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowSelectTest, "DreamTexture.Assets.Reflow.Select", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowSelectTest::RunTest(const FString& Parameters)
{
	const TArray<FDreamTextureReflowChannelState> States = MakeTypicalStates();
	using FPlanner = FDreamTextureReflowPlanner;

	// ---- 默认：缺的勾上，已有的不动 ----
	TestEqual(TEXT("补通道：只勾缺的（高度）"), Join(FPlanner::DefaultSelection(States, EDreamTextureReflowMode::FillChannels)), FString(TEXT("height")));
	TestEqual(TEXT("改写外观：底色一定勾（不重画就谈不上改写），已有的其余不勾"),
		Join(FPlanner::DefaultSelection(States, EDreamTextureReflowMode::Restyle)), FString(TEXT("basecolor,height")));

	// 什么都不缺：补通道时默认一个都不勾。
	{
		const TArray<FDreamTextureReflowChannelState> Complete = FPlanner::Describe(MakeAnalysis({
			MakeRow(TEXT("T_Base"), EDreamTextureRole::BaseColor),
			MakeRow(TEXT("T_N"), EDreamTextureRole::Normal),
			MakeRow(TEXT("T_H"), EDreamTextureRole::Height),
			MakePacked(TEXT("T_ORM"), EDreamTextureRole::AO, EDreamTextureRole::Roughness, EDreamTextureRole::Metallic, EDreamTextureRole::Unknown),
		}), nullptr);
		TestEqual(TEXT("什么都不缺：默认什么都不覆盖"), FPlanner::DefaultSelection(Complete, EDreamTextureReflowMode::FillChannels).Num(), 0);
	}

	// ---- 锁定：补通道时底色永远沿用原图 ----
	TestTrue(TEXT("补通道：底色锁定"), FPlanner::IsLocked(TEXT("basecolor"), EDreamTextureReflowMode::FillChannels));
	TestFalse(TEXT("补通道：法线不锁"), FPlanner::IsLocked(TEXT("normal"), EDreamTextureReflowMode::FillChannels));
	TestFalse(TEXT("改写外观：底色不锁"), FPlanner::IsLocked(TEXT("basecolor"), EDreamTextureReflowMode::Restyle));

	// ---- 规整：去掉锁定的、不认识的 ----
	TestEqual(TEXT("补通道时勾了底色：被去掉；不认识的通道：被去掉"),
		Join(FPlanner::Normalize(States, Set({ TEXT("basecolor"), TEXT("normal"), TEXT("emissive") }), EDreamTextureReflowMode::FillChannels)), FString(TEXT("normal")));
	TestEqual(TEXT("改写外观时底色留着"),
		Join(FPlanner::Normalize(States, Set({ TEXT("basecolor") }), EDreamTextureReflowMode::Restyle)), FString(TEXT("basecolor")));

	// ---- 打包图成组：勾一个，整组跟着勾 ----
	TestEqual(TEXT("勾粗糙度：ORM 三路一起勾"),
		Join(FPlanner::Toggle(States, TSet<FString>(), EDreamTextureReflowMode::FillChannels, TEXT("roughness"), true)), FString(TEXT("ao,metallic,roughness")));
	TestEqual(TEXT("取消金属度：三路一起取消"),
		Join(FPlanner::Toggle(States, Set({ TEXT("ao"), TEXT("roughness"), TEXT("metallic"), TEXT("height") }), EDreamTextureReflowMode::FillChannels, TEXT("metallic"), false)), FString(TEXT("height")));
	TestEqual(TEXT("规整：只剩一路的残缺勾选补成整组"),
		Join(FPlanner::Normalize(States, Set({ TEXT("ao") }), EDreamTextureReflowMode::FillChannels)), FString(TEXT("ao,metallic,roughness")));

	// ---- 独立的通道各管各的 ----
	TestEqual(TEXT("勾高度不牵连别的"),
		Join(FPlanner::Toggle(States, Set({ TEXT("normal") }), EDreamTextureReflowMode::FillChannels, TEXT("height"), true)), FString(TEXT("height,normal")));
	TestEqual(TEXT("缺失的通道取消勾选"),
		Join(FPlanner::Toggle(States, Set({ TEXT("height"), TEXT("normal") }), EDreamTextureReflowMode::FillChannels, TEXT("height"), false)), FString(TEXT("normal")));
	TestEqual(TEXT("补通道时点底色：被锁，勾不上"),
		FPlanner::Toggle(States, TSet<FString>(), EDreamTextureReflowMode::FillChannels, TEXT("basecolor"), true).Num(), 0);
	// ---- 重绘幅度的口径 ----
	TestTrue(TEXT("平铺：1.0 也行"), DreamTextureReflow::IsDenoiseAllowed(EDreamTextureUVKind::Tiled, 1.0));
	TestTrue(TEXT("UV 展开：0.35 恰好允许"), DreamTextureReflow::IsDenoiseAllowed(EDreamTextureUVKind::Unwrapped, 0.35));
	TestTrue(TEXT("UV 展开：滑块把 0.35 存成 float 的误差不算超限"), DreamTextureReflow::IsDenoiseAllowed(EDreamTextureUVKind::Unwrapped, static_cast<double>(0.35f)));
	TestFalse(TEXT("UV 展开：0.36 超限"), DreamTextureReflow::IsDenoiseAllowed(EDreamTextureUVKind::Unwrapped, 0.36));
	TestFalse(TEXT("UV 展开：0.55（平铺的默认值）超限"), DreamTextureReflow::IsDenoiseAllowed(EDreamTextureUVKind::Unwrapped, DreamTextureReflow::DefaultTiledDenoise));
	TestTrue(TEXT("UV 展开的默认值在上限以内"), DreamTextureReflow::IsDenoiseAllowed(EDreamTextureUVKind::Unwrapped, DreamTextureReflow::DefaultUnwrappedDenoise));
	TestTrue(TEXT("上限是 0.35"), FMath::IsNearlyEqual(DreamTextureReflow::MaxUnwrappedDenoise, 0.35));
	// 超限时的话说清楚是为什么（人话，不是「参数越界」）。
	{
		const FString Words = DreamTextureReflow::DescribeDenoiseLimit(0.6);
		TestTrue(TEXT("说明点出上限"), Words.Contains(TEXT("0.35")));
		TestTrue(TEXT("说明点出现在的值"), Words.Contains(TEXT("0.60")));
		TestTrue(TEXT("说明讲清楚后果：重绘破坏 UV 对应关系，贴回模型就乱了"), Words.Contains(TEXT("Restyl")) && Words.Contains(TEXT("UV")) && Words.Contains(TEXT("no longer fits the model")));
		TestTrue(TEXT("说明给出出路：调低或切回平铺"), Words.Contains(TEXT("switch back to Tileable")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// 新建的计划
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowCreatePlanTest, "DreamTexture.Assets.Reflow.Create", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowCreatePlanTest::RunTest(const FString& Parameters)
{
	using FPlanner = FDreamTextureReflowPlanner;
	const FString Source = TEXT("/Game/Test/T_Base.T_Base");
	const FScopedSeparateChannels NoSeparate(false);
	const TArray<FDreamTextureReflowChannelState> States = MakeTypicalStates();

	// ---- 补通道，只补高度：底色沿用原图（不导入），法线与 ORM 沿用用户原来的 ----
	{
		const FDreamTextureReflowCreatePlan Plan = FPlanner::PlanCreate(States, Set({ TEXT("height") }), EDreamTextureReflowMode::FillChannels, Source);
		TestTrue(TEXT("计划成立"), Plan.bOK);
		TestTrue(TEXT("显式给通道（不走设置里的默认）"), Plan.Options.bExplicitChannels);
		TestEqual(TEXT("只导入高度"), Join(Plan.Options.Channels), FString(TEXT("height")));
		TestEqual(TEXT("底色指向原图"), Plan.Options.ReuseTextures.FindRef(TEXT("basecolor")), Source);
		TestEqual(TEXT("没勾的法线沿用用户原来的"), Plan.Options.ReuseTextures.FindRef(TEXT("normal")), FString(TEXT("/Game/Test/T_N.T_N")));
		TestEqual(TEXT("没勾的 ORM 沿用用户原来的"), Plan.Options.ReuseTextures.FindRef(TEXT("orm")), FString(TEXT("/Game/Test/T_ORM.T_ORM")));
		TestFalse(TEXT("底色不在导入清单里（往返一圈重新编码的那张不要）"), Plan.Options.Channels.Contains(TEXT("basecolor")));
		TestTrue(TEXT("要建材质实例"), Plan.Options.bCreateMaterialInstance);
	}

	// ---- 补通道，补法线与 ORM（勾粗糙度，整个 ORM 跟着）----
	{
		const FDreamTextureReflowCreatePlan Plan = FPlanner::PlanCreate(States,
			FPlanner::Toggle(States, Set({ TEXT("height") }), EDreamTextureReflowMode::FillChannels, TEXT("roughness"), true), EDreamTextureReflowMode::FillChannels, Source);
		TestTrue(TEXT("计划成立"), Plan.bOK);
		TestEqual(TEXT("导入高度与 ORM"), Join(Plan.Options.Channels), FString(TEXT("orm,height")));
		TestFalse(TEXT("ORM 重新生成了，就不再沿用原来的"), Plan.Options.ReuseTextures.Contains(TEXT("orm")));
		TestTrue(TEXT("法线没勾，仍沿用"), Plan.Options.ReuseTextures.Contains(TEXT("normal")));
		TestEqual(TEXT("底色仍是原图"), Plan.Options.ReuseTextures.FindRef(TEXT("basecolor")), Source);
	}

	// ---- 补通道时底色被锁：就算勾了也不导入 ----
	{
		const FDreamTextureReflowCreatePlan Plan = FPlanner::PlanCreate(States, Set({ TEXT("basecolor"), TEXT("height") }), EDreamTextureReflowMode::FillChannels, Source);
		TestFalse(TEXT("底色不导入"), Plan.Options.Channels.Contains(TEXT("basecolor")));
		TestEqual(TEXT("底色指向原图"), Plan.Options.ReuseTextures.FindRef(TEXT("basecolor")), Source);
	}

	// ---- 改写外观：底色重新生成，所以导入，不再沿用 ----
	{
		const FDreamTextureReflowCreatePlan Plan = FPlanner::PlanCreate(States, FPlanner::DefaultSelection(States, EDreamTextureReflowMode::Restyle), EDreamTextureReflowMode::Restyle, Source);
		TestTrue(TEXT("计划成立"), Plan.bOK);
		TestEqual(TEXT("导入底色与高度"), Join(Plan.Options.Channels), FString(TEXT("basecolor,height")));
		TestFalse(TEXT("底色不沿用原图"), Plan.Options.ReuseTextures.Contains(TEXT("basecolor")));
		TestTrue(TEXT("没勾的法线仍沿用"), Plan.Options.ReuseTextures.Contains(TEXT("normal")));
	}

	// ---- 改写外观但把底色取消了：外观不变，底色沿用原图 ----
	{
		const FDreamTextureReflowCreatePlan Plan = FPlanner::PlanCreate(States, Set({ TEXT("height") }), EDreamTextureReflowMode::Restyle, Source);
		TestEqual(TEXT("只导入高度"), Join(Plan.Options.Channels), FString(TEXT("height")));
		TestEqual(TEXT("底色沿用原图"), Plan.Options.ReuseTextures.FindRef(TEXT("basecolor")), Source);
	}

	// ---- 一个都没勾 ----
	{
		const FDreamTextureReflowCreatePlan Fill = FPlanner::PlanCreate(States, TSet<FString>(), EDreamTextureReflowMode::FillChannels, Source);
		TestFalse(TEXT("补通道：什么都没勾，不成立"), Fill.bOK);
		TestTrue(TEXT("原因说清楚"), Fill.Error.Contains(TEXT("Check at least one")));
		const FDreamTextureReflowCreatePlan OnlyLocked = FPlanner::PlanCreate(States, Set({ TEXT("basecolor") }), EDreamTextureReflowMode::FillChannels, Source);
		TestFalse(TEXT("补通道：只勾了被锁的底色，等于什么都没勾"), OnlyLocked.bOK);
	}

	// ---- 分通道导入开着：勾了粗糙度就把分通道图也导进来 ----
	{
		const FScopedSeparateChannels Separate(true);
		const FDreamTextureReflowCreatePlan Plan = FPlanner::PlanCreate(States, Set({ TEXT("roughness") }), EDreamTextureReflowMode::FillChannels, Source);
		TestEqual(TEXT("ORM 与三张分通道图（组里三路都勾了）"), Join(Plan.Options.Channels), FString(TEXT("orm,roughness,metallic,ao")));
	}

	// ---- 带不过去的要说 ----
	{
		// 用户的粗糙度是单独的贴图：新实例的母材质只认 ORM，勾了法线补通道，粗糙度没法带过去。
		const TArray<FDreamTextureReflowChannelState> Separate = FPlanner::Describe(MakeAnalysis({
			MakeRow(TEXT("T_Base"), EDreamTextureRole::BaseColor),
			MakeRow(TEXT("T_R"), EDreamTextureRole::Roughness, { TEXT("Roughness") }),
		}), nullptr);
		const FDreamTextureReflowCreatePlan Plan = FPlanner::PlanCreate(Separate, Set({ TEXT("normal") }), EDreamTextureReflowMode::FillChannels, Source);
		TestTrue(TEXT("计划成立"), Plan.bOK);
		bool bMentionsRoughness = false;
		for (const FString& Note : Plan.Notes)
		{
			bMentionsRoughness |= Note.Contains(TEXT("Roughness")) && Note.Contains(TEXT("T_R"));
		}
		TestTrue(TEXT("说明点名了没带过去的粗糙度贴图"), bMentionsRoughness);
		TestFalse(TEXT("没勾的 ORM 里没有可沿用的"), Plan.Options.ReuseTextures.Contains(TEXT("orm")));
	}

	// ---- 非标准打包图里的高度：不能沿用，要说 ----
	{
		const TArray<FDreamTextureReflowChannelState> Odd = FPlanner::Describe(MakeAnalysis({
			MakePacked(TEXT("T_ORMH"), EDreamTextureRole::AO, EDreamTextureRole::Roughness, EDreamTextureRole::Metallic, EDreamTextureRole::Height),
		}), nullptr);
		const FDreamTextureReflowCreatePlan Plan = FPlanner::PlanCreate(Odd, Set({ TEXT("normal") }), EDreamTextureReflowMode::FillChannels, Source);
		TestFalse(TEXT("高度在非标准打包图里：不沿用"), Plan.Options.ReuseTextures.Contains(TEXT("height")));
		bool bMentionsHeight = false;
		for (const FString& Note : Plan.Notes)
		{
			bMentionsHeight |= Note.Contains(TEXT("Height"));
		}
		TestTrue(TEXT("说明里点名了高度"), bMentionsHeight);
	}
	return true;
}

// ---------------------------------------------------------------------------
// 原地覆盖的计划
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowInPlacePlanTest, "DreamTexture.Assets.Reflow.InPlace.Plan", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowInPlacePlanTest::RunTest(const FString& Parameters)
{
	using FPlanner = FDreamTextureReflowPlanner;

	const TArray<FDreamTextureReflowChannelState> States = FPlanner::Describe(MakeAnalysis({
		MakeRow(TEXT("T_Base"), EDreamTextureRole::BaseColor, { TEXT("BaseColor") }),
		MakeRow(TEXT("T_N"), EDreamTextureRole::Normal, { TEXT("Normal"), TEXT("NormalAlt") }),
		MakePacked(TEXT("T_ORM"), EDreamTextureRole::AO, EDreamTextureRole::Roughness, EDreamTextureRole::Metallic, EDreamTextureRole::Unknown),
	}), nullptr);

	// ---- 勾法线与高度：法线按它自己的两个参数名改，高度没有地方放 ----
	{
		const FDreamTextureReflowInPlacePlan Plan = FPlanner::PlanInPlace(States, Set({ TEXT("normal"), TEXT("height") }), EDreamTextureReflowMode::FillChannels);
		TestTrue(TEXT("有能改的，计划成立"), Plan.bOK);
		if (TestEqual(TEXT("一处覆盖"), Plan.Items.Num(), 1))
		{
			TestEqual(TEXT("用后端的法线"), Plan.Items[0].ImportChannel, FString(TEXT("normal")));
			TestEqual(TEXT("按选中材质自己的参数名改（两个）"), Plan.Items[0].Parameters.Num(), 2);
			TestEqual(TEXT("参数名是它自己的，不是插件母材质的"), Plan.Items[0].Parameters[1].ToString(), FString(TEXT("NormalAlt")));
			TestEqual(TEXT("被换掉的贴图"), Plan.Items[0].OldTexturePath, FString(TEXT("/Game/Test/T_N.T_N")));
		}
		if (TestEqual(TEXT("高度改不了：记一条"), Plan.Skipped.Num(), 1))
		{
			TestTrue(TEXT("原因：材质里没有高度贴图"), Plan.Skipped[0].Contains(TEXT("Height")) && Plan.Skipped[0].Contains(TEXT("has no")));
		}
		TestEqual(TEXT("只导入要换的通道"), Join(Plan.Options.Channels), FString(TEXT("normal")));
		TestTrue(TEXT("显式给通道"), Plan.Options.bExplicitChannels);
		TestFalse(TEXT("原地覆盖不另建材质实例"), Plan.Options.bCreateMaterialInstance);
		TestEqual(TEXT("原地覆盖不用「沿用」"), Plan.Options.ReuseTextures.Num(), 0);
	}

	// ---- 勾 AO：ORM 整组一起换 ----
	{
		const FDreamTextureReflowInPlacePlan Plan = FPlanner::PlanInPlace(States, Set({ TEXT("ao") }), EDreamTextureReflowMode::FillChannels);
		TestTrue(TEXT("成立"), Plan.bOK);
		if (TestEqual(TEXT("一处覆盖（ORM 只算一处）"), Plan.Items.Num(), 1))
		{
			TestEqual(TEXT("用后端的 ORM"), Plan.Items[0].ImportChannel, FString(TEXT("orm")));
			TestEqual(TEXT("覆盖 AO/粗糙度/金属度三路"), Plan.Items[0].Covers.Num(), 3);
			TestEqual(TEXT("参数是 ORM"), Plan.Items[0].Parameters[0].ToString(), FString(TEXT("ORM")));
		}
		TestEqual(TEXT("导入 ORM"), Join(Plan.Options.Channels), FString(TEXT("orm")));
	}

	// ---- 补通道时底色锁定：勾了也不改；改写外观时底色可改 ----
	{
		const FDreamTextureReflowInPlacePlan Fill = FPlanner::PlanInPlace(States, Set({ TEXT("basecolor"), TEXT("normal") }), EDreamTextureReflowMode::FillChannels);
		TestEqual(TEXT("补通道：底色不在覆盖里"), Fill.Items.Num(), 1);
		const FDreamTextureReflowInPlacePlan Restyle = FPlanner::PlanInPlace(States, Set({ TEXT("basecolor"), TEXT("normal") }), EDreamTextureReflowMode::Restyle);
		if (TestEqual(TEXT("改写外观：底色与法线"), Restyle.Items.Num(), 2))
		{
			TestEqual(TEXT("先底色"), Restyle.Items[0].ImportChannel, FString(TEXT("basecolor")));
			TestEqual(TEXT("按底色自己的参数名"), Restyle.Items[0].Parameters[0].ToString(), FString(TEXT("BaseColor")));
		}
		TestEqual(TEXT("导入清单"), Join(Restyle.Options.Channels), FString(TEXT("basecolor,normal")));
	}

	// ---- 直接引用（没有参数）：实例改不了 ----
	{
		const TArray<FDreamTextureReflowChannelState> Direct = FPlanner::Describe(MakeAnalysis({
			MakeRow(TEXT("T_N"), EDreamTextureRole::Normal),
		}), nullptr);
		const FDreamTextureReflowInPlacePlan Plan = FPlanner::PlanInPlace(Direct, Set({ TEXT("normal") }), EDreamTextureReflowMode::FillChannels);
		TestFalse(TEXT("唯一勾的改不了：计划不成立"), Plan.bOK);
		TestFalse(TEXT("有原因"), Plan.Error.IsEmpty());
		if (TestEqual(TEXT("跳过一条"), Plan.Skipped.Num(), 1))
		{
			TestTrue(TEXT("原因点明直接写在母材质的图里"), Plan.Skipped[0].Contains(TEXT("master material")) && Plan.Skipped[0].Contains(TEXT("T_N")));
		}
		FString Why;
		TestFalse(TEXT("CanOverwriteChannel 同样判不行"), FPlanner::CanOverwriteChannel(*Find(Direct, TEXT("normal")), Why));
		TestFalse(TEXT("原因不为空"), Why.IsEmpty());
	}

	// ---- 非标准打包图：不碰 ----
	{
		const TArray<FDreamTextureReflowChannelState> Odd = FPlanner::Describe(MakeAnalysis({
			MakePacked(TEXT("T_RMA"), EDreamTextureRole::Roughness, EDreamTextureRole::Metallic, EDreamTextureRole::AO, EDreamTextureRole::Unknown, { TEXT("RMA") }),
		}), nullptr);
		const FDreamTextureReflowInPlacePlan Plan = FPlanner::PlanInPlace(Odd, Set({ TEXT("roughness") }), EDreamTextureReflowMode::FillChannels);
		TestFalse(TEXT("不成立"), Plan.bOK);
		TestTrue(TEXT("每个被勾的通道各记一条原因"), Plan.Skipped.Num() >= 1 && Plan.Skipped[0].Contains(TEXT("not the standard ORM")));
	}

	// ---- 什么都没勾 ----
	{
		const FDreamTextureReflowInPlacePlan Plan = FPlanner::PlanInPlace(States, TSet<FString>(), EDreamTextureReflowMode::FillChannels);
		TestFalse(TEXT("不成立"), Plan.bOK);
		TestTrue(TEXT("原因说没勾"), Plan.Error.Contains(TEXT("No channel to overwrite")));
	}

	// ---- 分开的粗糙度：用后端的 roughness 通道，不是 ORM ----
	{
		const TArray<FDreamTextureReflowChannelState> Separate = FPlanner::Describe(MakeAnalysis({
			MakeRow(TEXT("T_R"), EDreamTextureRole::Roughness, { TEXT("Rough") }),
		}), nullptr);
		const FDreamTextureReflowInPlacePlan Plan = FPlanner::PlanInPlace(Separate, Set({ TEXT("roughness") }), EDreamTextureReflowMode::FillChannels);
		if (TestTrue(TEXT("成立"), Plan.bOK) && TestEqual(TEXT("一处"), Plan.Items.Num(), 1))
		{
			TestEqual(TEXT("用后端的 roughness 单通道图"), Plan.Items[0].ImportChannel, FString(TEXT("roughness")));
		}
	}

	// ---- 这个材质能不能原地覆盖 ----
	{
		FScopedTestContent Content;
		UMaterial* Master = MakeTestMaterial(Content.Root, TEXT("M_Plain"));
		UMaterialInstanceConstant* Instance = MakeTestInstance(Content.Root, TEXT("MI_Plain"), Master);
		if (TestTrue(TEXT("造好材质与实例"), Master != nullptr && Instance != nullptr))
		{
			FDreamTextureMaterialAnalysis OnMaster = MakeAnalysis({}, /*bInstance*/ false);
			FString Why;
			TestFalse(TEXT("母材质：原地覆盖置灰"), FPlanner::CanOverwriteInPlace(OnMaster, Master, Why));
			TestTrue(TEXT("原因点明母材质、贴图硬连在图里、没有参数"), Why.Contains(TEXT("master material")) && Why.Contains(TEXT("wired directly")) && Why.Contains(TEXT("no parameters")));

			FDreamTextureMaterialAnalysis OnInstance = MakeAnalysis({}, /*bInstance*/ true);
			TestTrue(TEXT("工程里的材质实例：可以"), FPlanner::CanOverwriteInPlace(OnInstance, Instance, Why));

			TestFalse(TEXT("没有材质：不行"), FPlanner::CanOverwriteInPlace(OnInstance, nullptr, Why));
			FDreamTextureMaterialAnalysis Failed;
			TestFalse(TEXT("解析失败：不行"), FPlanner::CanOverwriteInPlace(Failed, Instance, Why));
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// 插件自己建的实例：UsePOM 关着，Height 解析不到，但它是用户的东西
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowPluginParamsTest, "DreamTexture.Assets.Reflow.PluginParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowPluginParamsTest::RunTest(const FString& Parameters)
{
	FDreamTextureManifest Manifest;
	TMap<FString, FString> Files;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("读固定素材：%s"), *Error), LoadFixture(Manifest, Files, Error)))
	{
		return false;
	}
	using FPlanner = FDreamTextureReflowPlanner;

	// ---- 整套都有：插件建的实例，UsePOM 默认关 ----
	{
		FScopedTestContent Content;
		const FDreamTextureImportResult Imported = ReflowImportFixtureInto(Manifest, Files, Content.Root);
		const FDreamTextureImportedTexture Created = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Imported, Content.Root, false);
		UMaterialInstanceConstant* Instance = FindObject<UMaterialInstanceConstant>(nullptr, *Created.ObjectPath);
		if (!TestNotNull(FString::Printf(TEXT("材质实例建出来了（%s）"), *Created.Note), Instance))
		{
			return false;
		}

		const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(Instance);
		TestTrue(TEXT("解析成功"), Analysis.bOK);
		TestNull(TEXT("前提：解析结果里确实没有高度图（UsePOM 关）"), FindRowByParameter(Analysis, TEXT("Height")));

		// 只看解析结果：高度被当成缺失——这正是会让用户一勾就盖掉他高度图的错误。
		const TArray<FDreamTextureReflowChannelState> WithoutMaterial = FPlanner::Describe(Analysis, nullptr);
		TestTrue(TEXT("只看解析结果时高度显示缺失（所以必须再看实例的参数）"), Find(WithoutMaterial, TEXT("height"))->Presence == EPresence::Missing);

		const TArray<FDreamTextureReflowChannelState> States = FPlanner::Describe(Analysis, Instance);
		const FDreamTextureReflowChannelState* Height = Find(States, TEXT("height"));
		if (TestNotNull(TEXT("高度"), Height))
		{
			TestTrue(TEXT("高度：判为已有"), Height->Presence == EPresence::Dedicated);
			TestTrue(TEXT("标明是从插件参数上认出来的"), Height->bFromPluginParameter);
			TestEqual(TEXT("参数是 Height"), Height->Parameters.Num() == 1 ? Height->Parameters[0].ToString() : FString(), FString(TEXT("Height")));
			const UTexture* Actual = UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, DreamTextureMaterialParam::Height);
			TestTrue(TEXT("贴图就是实例上挂着的那张"), Actual != nullptr && Height->TexturePath == Actual->GetPathName());
		}
		TestFalse(TEXT("解析结果里认出来的底色不标成参数补的"), Find(States, TEXT("basecolor"))->bFromPluginParameter);
		TestTrue(TEXT("ORM 仍由解析结果认出（三路成组）"), Find(States, TEXT("roughness"))->Presence == EPresence::InStandardORM && !Find(States, TEXT("roughness"))->bFromPluginParameter);

		// 整套都有：补通道默认什么都不覆盖。
		TestEqual(TEXT("补通道：默认一个都不勾（含高度）"), FPlanner::DefaultSelection(States, EDreamTextureReflowMode::FillChannels).Num(), 0);

		// 原地覆盖时高度可以改：按插件母材质的参数名。
		FString Why;
		TestTrue(TEXT("高度可以原地覆盖"), FPlanner::CanOverwriteChannel(*Height, Why));
	}

	// ---- 实例上没有高度图：高度缺失，默认要补 ----
	{
		FScopedTestContent Content;
		const FDreamTextureImportResult Imported = ReflowImportFixtureInto(Manifest, Files, Content.Root, { TEXT("height") });
		FDreamTextureManifest Reduced = Manifest;
		Reduced.Maps.Remove(TEXT("height"));
		const FDreamTextureImportedTexture Created = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Reduced, Imported, Content.Root, false);
		UMaterialInstanceConstant* Instance = FindObject<UMaterialInstanceConstant>(nullptr, *Created.ObjectPath);
		if (TestNotNull(TEXT("没有高度图的实例建出来了"), Instance))
		{
			const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(Instance);
			const TArray<FDreamTextureReflowChannelState> States = FPlanner::Describe(Analysis, Instance);
			TestTrue(TEXT("没挂高度图：缺失（母材质参数上的引擎默认占位不算用户的）"), Find(States, TEXT("height"))->Presence == EPresence::Missing);
			TestFalse(TEXT("也不是参数补的"), Find(States, TEXT("height"))->bFromPluginParameter);
			TestEqual(TEXT("默认要补的就是高度"), Join(FPlanner::DefaultSelection(States, EDreamTextureReflowMode::FillChannels)), FString(TEXT("height")));
		}
	}

	// ---- 用户自己的材质，参数名恰好叫 Height：不按参数认，只看解析结果 ----
	{
		FScopedTestContent Content;
		UTexture2D* Gray = MakeGrayTexture(Content.Root, TEXT("T_UserHeight"), 3);
		UMaterial* Own = MakeTestMaterial(Content.Root, TEXT("M_Own"));
		FGraph G(Own);
		G.Connect(MP_BaseColor, G.Sample(MakeColorTexture(Content.Root, TEXT("T_UserBase"), 1), SAMPLERTYPE_Color), SampleOut::RGB);
		UMaterialExpressionTextureSampleParameter2D* HeightParam = G.Param(TEXT("Height"), Gray, SAMPLERTYPE_LinearGrayscale);
		(void)HeightParam; // 参数在图里但没连到任何输出：解析器不会把它算作用到的贴图。
		G.Finish();
		UMaterialInstanceConstant* Instance = MakeTestInstance(Content.Root, TEXT("MI_Own"), Own);
		if (TestTrue(TEXT("造好用户自己的材质"), Instance != nullptr))
		{
			const FDreamTextureMaterialAnalysis Analysis = FDreamTextureMaterialAnalyzer::Analyze(Instance);
			const TArray<FDreamTextureReflowChannelState> States = FPlanner::Describe(Analysis, Instance);
			TestTrue(TEXT("不是插件母材质：不按参数名猜，高度仍缺失"), Find(States, TEXT("height"))->Presence == EPresence::Missing);
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// 原地覆盖的执行
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowApplyTest, "DreamTexture.Assets.Reflow.InPlace.Apply", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowApplyTest::RunTest(const FString& Parameters)
{
	FDreamTextureManifest Manifest;
	TMap<FString, FString> Files;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("读固定素材：%s"), *Error), LoadFixture(Manifest, Files, Error)))
	{
		return false;
	}
	if (!TestNotNull(TEXT("编辑器的撤销栈"), GEditor != nullptr ? GEditor->Trans.Get() : nullptr))
	{
		return false;
	}

	FScopedTestContent Content;
	const FDreamTextureImportResult Imported = ReflowImportFixtureInto(Manifest, Files, Content.Root);
	const FDreamTextureImportedTexture Created = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Manifest, Imported, Content.Root, false);
	UMaterialInstanceConstant* Instance = FindObject<UMaterialInstanceConstant>(nullptr, *Created.ObjectPath);
	if (!TestNotNull(FString::Printf(TEXT("材质实例建出来了（%s）"), *Created.Note), Instance))
	{
		return false;
	}

	// 「后端生成、刚导入」的新贴图：用测试里造的代替，设置按各通道该有的样子。
	UTexture2D* NewNormal = MakeNormalTexture(Content.Root, TEXT("T_NewNormal"), 21);
	UTexture2D* NewHeight = MakeGrayTexture(Content.Root, TEXT("T_NewHeight"), 22);
	UTexture2D* WrongOrm = MakeColorTexture(Content.Root, TEXT("T_WrongOrm"), 23);  // sRGB + 默认压缩：不是 Masks
	if (!TestTrue(TEXT("造好新贴图"), NewNormal && NewHeight && WrongOrm))
	{
		return false;
	}

	auto Entry = [](const TCHAR* Channel, const UTexture* Texture, EDreamTextureImportAction Action = EDreamTextureImportAction::Created)
	{
		FDreamTextureImportedTexture Out;
		Out.Channel = Channel;
		Out.Action = Action;
		Out.ObjectPath = Texture->GetPathName();
		return Out;
	};
	FDreamTextureImportResult NewSet;
	NewSet.bOK = true;
	NewSet.Textures = { Entry(TEXT("normal"), NewNormal), Entry(TEXT("height"), NewHeight), Entry(TEXT("orm"), WrongOrm) };

	auto Overwrite = [](const TCHAR* ImportChannel, std::initializer_list<const TCHAR*> Params)
	{
		FDreamTextureReflowOverwrite Item;
		Item.ImportChannel = ImportChannel;
		Item.Covers = { ImportChannel };
		for (const TCHAR* Param : Params)
		{
			Item.Parameters.Add(FName(Param));
		}
		return Item;
	};

	UTexture* OldNormal = UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, DreamTextureMaterialParam::Normal);
	UTexture* OldHeight = UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, DreamTextureMaterialParam::Height);
	UTexture* OldOrm = UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, DreamTextureMaterialParam::ORM);
	UTexture* OldBase = UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, DreamTextureMaterialParam::BaseColor);
	if (!TestTrue(TEXT("前提：实例上四个参数都挂着导入的贴图"), OldNormal && OldHeight && OldOrm && OldBase))
	{
		return false;
	}

	// ---- 改法线与高度，ORM 的采样器对不上所以不改 ----
	const int32 QueueBefore = NumUndoable();
	const FDreamTextureReflowApplyResult Result = FDreamTextureReflowPlanner::ApplyInPlace(Instance,
		{ Overwrite(TEXT("normal"), { TEXT("Normal") }), Overwrite(TEXT("height"), { TEXT("Height") }), Overwrite(TEXT("orm"), { TEXT("ORM") }) }, NewSet, /*bSave*/ false);

	TestTrue(FString::Printf(TEXT("执行成功（%s）"), *Result.Error), Result.bOK);
	TestEqual(TEXT("改了两个参数"), Result.Changed.Num(), 2);
	TestTrue(TEXT("法线换成新的"), UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, DreamTextureMaterialParam::Normal) == NewNormal);
	TestTrue(TEXT("高度换成新的"), UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, DreamTextureMaterialParam::Height) == NewHeight);
	TestTrue(TEXT("ORM 采样器不匹配：没动"), UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, DreamTextureMaterialParam::ORM) == OldOrm);
	TestTrue(TEXT("没勾的底色：没动"), UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, DreamTextureMaterialParam::BaseColor) == OldBase);
	if (TestEqual(TEXT("跳过一处"), Result.Skipped.Num(), 1))
	{
		TestTrue(TEXT("原因点名参数与采样器"), Result.Skipped[0].Contains(TEXT("ORM")) && Result.Skipped[0].Contains(TEXT("sampler type")));
	}
	TestTrue(TEXT("写进了撤销栈"), Result.bTransacted);
	TestEqual(TEXT("整个覆盖只占撤销栈一条"), NumUndoable(), QueueBefore + 1);
	TestTrue(TEXT("实例被标脏（等用户存）"), Instance->GetOutermost()->IsDirty());

	// ---- Ctrl+Z：原样退回 ----
	TestTrue(TEXT("UndoTransaction 成功"), GEditor->UndoTransaction());
	TestTrue(TEXT("撤销后法线退回"), UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, DreamTextureMaterialParam::Normal) == OldNormal);
	TestTrue(TEXT("撤销后高度退回"), UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, DreamTextureMaterialParam::Height) == OldHeight);
	TestTrue(TEXT("撤销后 ORM 仍是原来的"), UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, DreamTextureMaterialParam::ORM) == OldOrm);

	TestTrue(TEXT("RedoTransaction 成功"), GEditor->RedoTransaction());
	TestTrue(TEXT("重做后法线又是新的"), UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, DreamTextureMaterialParam::Normal) == NewNormal);
	TestTrue(TEXT("再撤一次"), GEditor->UndoTransaction());

	// ---- 一个参数都没改（只剩采样器对不上的）：事务被取消，撤销栈不留空条目 ----
	{
		FDreamTextureImportResult OnlyWrong;
		OnlyWrong.Textures = { Entry(TEXT("orm"), WrongOrm) };
		const int32 Before = NumUndoable();
		const FDreamTextureReflowApplyResult Nothing = FDreamTextureReflowPlanner::ApplyInPlace(Instance, { Overwrite(TEXT("orm"), { TEXT("ORM") }) }, OnlyWrong, false);
		TestTrue(TEXT("没出错"), Nothing.bOK);
		TestEqual(TEXT("什么都没改"), Nothing.Changed.Num(), 0);
		TestFalse(TEXT("没进撤销栈"), Nothing.bTransacted);
		TestEqual(TEXT("撤销栈长度不变"), NumUndoable(), Before);
	}

	// ---- 后端结果里没有这个通道 / 导入失败 ----
	{
		FDreamTextureImportResult Missing;
		Missing.Textures = { Entry(TEXT("normal"), NewNormal, EDreamTextureImportAction::Failed) };
		const FDreamTextureReflowApplyResult R = FDreamTextureReflowPlanner::ApplyInPlace(Instance, { Overwrite(TEXT("normal"), { TEXT("Normal") }), Overwrite(TEXT("height"), { TEXT("Height") }) }, Missing, false);
		TestEqual(TEXT("两处都没改"), R.Changed.Num(), 0);
		TestEqual(TEXT("各记一条原因"), R.Skipped.Num(), 2);
		TestTrue(TEXT("法线仍是原来的"), UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, DreamTextureMaterialParam::Normal) == OldNormal);
	}

	// ---- 实例没了 ----
	{
		const FDreamTextureReflowApplyResult R = FDreamTextureReflowPlanner::ApplyInPlace(nullptr, {}, NewSet, false);
		TestFalse(TEXT("空实例：失败"), R.bOK);
		TestFalse(TEXT("有原因"), R.Error.IsEmpty());
	}

	// ---- 一个贴图绑在两个参数上：都改 ----
	{
		FDreamTextureImportResult One;
		One.Textures = { Entry(TEXT("height"), NewHeight) };
		// 第二个参数是 Normal 的话采样器对不上（灰度图不是法线），验证「逐参数核对」：Height 改、Normal 不改。
		const FDreamTextureReflowApplyResult R = FDreamTextureReflowPlanner::ApplyInPlace(Instance, { Overwrite(TEXT("height"), { TEXT("Height"), TEXT("Normal") }) }, One, false);
		TestEqual(TEXT("只改了 Height"), R.Changed.Num(), 1);
		TestEqual(TEXT("Normal 因采样器不符被跳过"), R.Skipped.Num(), 1);
		TestTrue(TEXT("Normal 没动"), UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, DreamTextureMaterialParam::Normal) == OldNormal);
		GEditor->UndoTransaction();
	}
	return true;
}

// ---------------------------------------------------------------------------
// 插件自己建的实例：换 Normal / ORM 时把对应的静态开关打开，否则材质不会采新贴图
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowApplySwitchesTest, "DreamTexture.Assets.Reflow.InPlace.Switches", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowApplySwitchesTest::RunTest(const FString& Parameters)
{
	FDreamTextureManifest Manifest;
	TMap<FString, FString> Files;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("读固定素材：%s"), *Error), LoadFixture(Manifest, Files, Error)))
	{
		return false;
	}
	if (!TestNotNull(TEXT("编辑器的撤销栈"), GEditor != nullptr ? GEditor->Trans.Get() : nullptr))
	{
		return false;
	}

	auto Entry = [](const TCHAR* Channel, const UTexture* Texture)
	{
		FDreamTextureImportedTexture Out;
		Out.Channel = Channel;
		Out.Action = EDreamTextureImportAction::Created;
		Out.ObjectPath = Texture->GetPathName();
		return Out;
	};
	auto Overwrite = [](const TCHAR* ImportChannel, const TCHAR* Parameter)
	{
		FDreamTextureReflowOverwrite Item;
		Item.ImportChannel = ImportChannel;
		Item.Covers = { ImportChannel };
		Item.Parameters = { FName(Parameter) };
		return Item;
	};

	// ---- 插件自己建的实例，建的时候只有底色：UseNormal / UseORM 是关的 ----
	{
		FScopedTestContent Content;
		const FDreamTextureImportResult Imported = ReflowImportFixtureInto(Manifest, Files, Content.Root, { TEXT("normal"), TEXT("orm"), TEXT("height"), TEXT("roughness"), TEXT("metallic"), TEXT("ao") });
		FDreamTextureManifest Reduced = Manifest;
		for (const TCHAR* Channel : { TEXT("normal"), TEXT("orm"), TEXT("height"), TEXT("roughness"), TEXT("metallic"), TEXT("ao") })
		{
			Reduced.Maps.Remove(Channel);
		}
		const FDreamTextureImportedTexture Created = FDreamTextureMaterialBuilder::CreateOrUpdateInstance(Reduced, Imported, Content.Root, false);
		UMaterialInstanceConstant* Instance = FindObject<UMaterialInstanceConstant>(nullptr, *Created.ObjectPath);
		if (!TestNotNull(FString::Printf(TEXT("只有底色的实例建出来了（%s）"), *Created.Note), Instance))
		{
			return false;
		}
		TestFalse(TEXT("前提：UseNormal 关着"), UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Instance, DreamTextureMaterialParam::UseNormal));
		TestFalse(TEXT("前提：UseORM 关着"), UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Instance, DreamTextureMaterialParam::UseORM));
		UTexture* DefaultNormal = UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, DreamTextureMaterialParam::Normal);

		UTexture2D* NewNormal = MakeNormalTexture(Content.Root, TEXT("T_NewNormal"), 31);
		UTexture2D* NewOrm = MakeMasksTexture(Content.Root, TEXT("T_NewOrm"), 32);
		FDreamTextureImportResult NewSet;
		NewSet.bOK = true;
		NewSet.Textures = { Entry(TEXT("normal"), NewNormal), Entry(TEXT("orm"), NewOrm) };

		const int32 QueueBefore = NumUndoable();
		const FDreamTextureReflowApplyResult Result = FDreamTextureReflowPlanner::ApplyInPlace(Instance, { Overwrite(TEXT("normal"), TEXT("Normal")), Overwrite(TEXT("orm"), TEXT("ORM")) }, NewSet, false);
		TestTrue(FString::Printf(TEXT("执行成功（%s）"), *Result.Error), Result.bOK);
		TestTrue(TEXT("Normal 换成新的"), UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, DreamTextureMaterialParam::Normal) == NewNormal);
		TestTrue(TEXT("ORM 换成新的"), UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, DreamTextureMaterialParam::ORM) == NewOrm);
		TestTrue(TEXT("UseNormal 打开了（否则材质不会采新法线，用户会看到「覆盖成功了、材质没变化」）"), UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Instance, DreamTextureMaterialParam::UseNormal));
		TestTrue(TEXT("UseORM 打开了"), UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Instance, DreamTextureMaterialParam::UseORM));
		TestFalse(TEXT("UsePOM 不碰（那是用户的选择）"), UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Instance, DreamTextureMaterialParam::UsePOM));
		TestEqual(TEXT("改了两个参数加两个开关，共四处"), Result.Changed.Num(), 4);
		bool bMentionsSwitch = false;
		for (const FString& Line : Result.Changed)
		{
			bMentionsSwitch |= Line.Contains(TEXT("UseNormal")) && Line.Contains(TEXT("-> on"));
		}
		TestTrue(TEXT("改动清单里说了开关"), bMentionsSwitch);
		TestEqual(TEXT("参数与开关同在一个事务里"), NumUndoable(), QueueBefore + 1);

		// ---- 一次撤销：贴图与开关一起退回 ----
		TestTrue(TEXT("UndoTransaction 成功"), GEditor->UndoTransaction());
		TestTrue(TEXT("撤销后 Normal 退回"), UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, DreamTextureMaterialParam::Normal) == DefaultNormal);
		TestFalse(TEXT("撤销后 UseNormal 关回去"), UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Instance, DreamTextureMaterialParam::UseNormal));
		TestFalse(TEXT("撤销后 UseORM 关回去"), UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Instance, DreamTextureMaterialParam::UseORM));

		// ---- 只换高度：不碰任何开关 ----
		UTexture2D* NewHeight = MakeGrayTexture(Content.Root, TEXT("T_NewHeight"), 33);
		FDreamTextureImportResult HeightSet;
		HeightSet.Textures = { Entry(TEXT("height"), NewHeight) };
		const FDreamTextureReflowApplyResult HeightResult = FDreamTextureReflowPlanner::ApplyInPlace(Instance, { Overwrite(TEXT("height"), TEXT("Height")) }, HeightSet, false);
		TestEqual(TEXT("只改了高度这一个参数"), HeightResult.Changed.Num(), 1);
		TestFalse(TEXT("UsePOM 仍然关着"), UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Instance, DreamTextureMaterialParam::UsePOM));
		TestFalse(TEXT("UseNormal 没被牵动"), UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Instance, DreamTextureMaterialParam::UseNormal));
		GEditor->UndoTransaction();
	}

	// ---- 用户自己的母材质：哪怕有同名的参数和开关，也不替它做主 ----
	{
		FScopedTestContent Content;
		UTexture2D* Default = MakeNormalTexture(Content.Root, TEXT("T_OwnDefaultNormal"), 41);
		UMaterial* Own = MakeTestMaterial(Content.Root, TEXT("M_Own"));
		FGraph G(Own);
		G.Param(TEXT("Normal"), Default, SAMPLERTYPE_Normal);
		UMaterialExpressionStaticBoolParameter* Switch = G.Add<UMaterialExpressionStaticBoolParameter>();
		Switch->ParameterName = TEXT("UseNormal");
		Switch->DefaultValue = false;
		G.Finish();
		UMaterialInstanceConstant* Instance = MakeTestInstance(Content.Root, TEXT("MI_Own"), Own);
		if (TestTrue(TEXT("造好用户自己的实例"), Instance != nullptr))
		{
			UTexture2D* NewNormal = MakeNormalTexture(Content.Root, TEXT("T_OwnNewNormal"), 42);
			FDreamTextureImportResult NewSet;
			NewSet.Textures = { Entry(TEXT("normal"), NewNormal) };
			const FDreamTextureReflowApplyResult Result = FDreamTextureReflowPlanner::ApplyInPlace(Instance, { Overwrite(TEXT("normal"), TEXT("Normal")) }, NewSet, false);
			TestTrue(TEXT("参数换了"), UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, TEXT("Normal")) == NewNormal);
			TestFalse(TEXT("用户的 UseNormal 开关没碰"), UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Instance, TEXT("UseNormal")));
			TestEqual(TEXT("只有一处改动"), Result.Changed.Num(), 1);
			GEditor->UndoTransaction();
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
