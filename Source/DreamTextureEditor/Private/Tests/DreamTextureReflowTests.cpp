// Copyright TypeDreamMoon. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamTextureReflowModel.h"
#include "HAL/FileManager.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "SDreamTextureReflowPanel.h"
#include "SDreamTextureShell.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SNullWidget.h"

/*
 * 回流页：页面模型的分支（没选东西、多个候选、空槽、解析失败、没有贴图、有结果、用户指认、导出）、
 * 文案、面板构造不崩。
 *
 * 全部不碰真实选区、不造真材质图、不写工程目录：取选区、解析、导出都换成假的，
 * 假解析按真解析的契约行事（读指认库、套到行上），这样「模型把指认写对了、写完又重新解析了」这类逻辑可以单测。
 * 真解析与真导出在 DreamTexture.Assets.Analyze / Export 里测。
 */

namespace
{
	UMaterial* MakeMaterial()
	{
		return NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
	}

	FDreamTextureMaterialTexture PanelMakeRow(const TCHAR* Name, EDreamTextureRole Role, EDreamTextureEvidence Evidence, bool bExportable = true, const TCHAR* Blocker = TEXT(""))
	{
		FDreamTextureMaterialTexture Row;
		Row.TexturePath = FString::Printf(TEXT("/Game/Test/%s.%s"), Name, Name);
		Row.AssetName = Name;
		Row.SourceWidth = 1024;
		Row.SourceHeight = 1024;
		Row.Role = Role;
		Row.Evidence = Evidence;
		Row.EvidenceDetail = TEXT("测试依据");
		Row.bExportable = bExportable;
		Row.ExportBlocker = Blocker;
		FDreamTextureTextureUse Use;
		Use.Sink = EDreamTextureSink::BaseColor;
		Use.Channels = DreamTextureChannelBit::RGB;
		Row.Uses.Add(Use);
		return Row;
	}

	FDreamTextureMaterialAnalysis PanelMakeAnalysis(UMaterialInterface* Material, const TCHAR* AssetPath, TArray<FDreamTextureMaterialTexture> Rows)
	{
		FDreamTextureMaterialAnalysis Analysis;
		Analysis.bOK = true;
		Analysis.Material = Material;
		Analysis.MaterialPath = AssetPath;
		Analysis.AssetPath = AssetPath;
		Analysis.DisplayName = FPaths::GetBaseFilename(AssetPath);
		Analysis.Textures = MoveTemp(Rows);
		return Analysis;
	}

	/** 假后端：选区、解析结果、导出结果都由测试摆好，并记下被调用了几次、带着什么参数。 */
	struct FFake
	{
		TArray<UMaterialInterface*> Materials;
		TArray<FDreamTextureMaterialSlot> Slots;
		TMap<FString, FDreamTextureMaterialAnalysis> Results;

		int32 AnalyzeCalls = 0;
		int32 ExportCalls = 0;
		int32 TempDirCalls = 0;
		TArray<FDreamTextureExportOptions> ExportOptions;
		FDreamTextureOverrideStore* Store = nullptr;

		FDreamTextureReflowBackend Make()
		{
			FDreamTextureReflowBackend Out;
			Out.Overrides = Store;
			Out.GetSelectedMaterials = [this]() { return Materials; };
			Out.GetSelectedActorSlots = [this]() { return Slots; };
			Out.Analyze = [this](UMaterialInterface* Material, const FDreamTextureAnalyzeOptions& Options)
			{
				++AnalyzeCalls;
				const FDreamTextureMaterialAnalysis* Preset = Results.Find(Material->GetPathName());
				if (Preset == nullptr)
				{
					FDreamTextureMaterialAnalysis Failed;
					Failed.Error = TEXT("测试：没有预设的解析结果");
					return Failed;
				}
				FDreamTextureMaterialAnalysis Copy = *Preset;
				// 按真解析的契约套用户指认。
				if (Options.bApplyOverrides && Options.Overrides != nullptr)
				{
					for (FDreamTextureMaterialTexture& Row : Copy.Textures)
					{
						FDreamTextureRoleOverride Override;
						if (Options.Overrides->Find(Copy.AssetPath, Row.TexturePath, Override))
						{
							Row.Role = Override.Role;
							Row.Layout = Override.Layout;
							Row.Evidence = EDreamTextureEvidence::UserOverride;
						}
					}
				}
				return Copy;
			};
			Out.Export = [this](const FDreamTextureMaterialAnalysis& Analysis, const FDreamTextureExportOptions& Options)
			{
				++ExportCalls;
				ExportOptions.Add(Options);
				TArray<FDreamTextureExportedImage> Results;
				for (const FDreamTextureMaterialTexture& Row : Analysis.Textures)
				{
					FDreamTextureExportedImage Image;
					Image.TexturePath = Row.TexturePath;
					if (Row.bExportable)
					{
						Image.bExported = true;
						Image.FilePath = Options.OutputDir / (Row.AssetName + TEXT(".png"));
						Image.Sha256 = TEXT("0123456789abcdef");
						Image.Width = Options.MaxEdge > 0 ? FMath::Min(Options.MaxEdge, Row.SourceWidth) : Row.SourceWidth;
						Image.Height = Image.Width;
						Image.bResized = Image.Width != Row.SourceWidth;
					}
					else
					{
						Image.Reason = Row.ExportBlocker;
					}
					Results.Add(MoveTemp(Image));
				}
				return Results;
			};
			Out.MakeTempDir = [this]()
			{
				++TempDirCalls;
				return FString::Printf(TEXT("C:/Fake/Reflow/run%d"), TempDirCalls);
			};
			return Out;
		}
	};

	FDreamTextureMaterialSlot MakeSlot(const TCHAR* Label, UMaterialInterface* Material)
	{
		FDreamTextureMaterialSlot Slot;
		Slot.Label = Label;
		Slot.Material = Material;
		return Slot;
	}

	/** 记下模型发出的通知。 */
	struct FNoticeLog
	{
		TArray<FString> Messages;
		TArray<bool> Errors;

		void Bind(FDreamTextureReflowModel& Model)
		{
			Model.OnNotice.AddLambda([this](const FText& Message, bool bIsError)
			{
				Messages.Add(Message.ToString());
				Errors.Add(bIsError);
			});
		}
	};

	/** 指认库的临时文件。 */
	struct FPanelTempStore
	{
		FString Path;
		FDreamTextureOverrideStore Store;

		FPanelTempStore()
			: Path(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamTextureTest") / TEXT("ReflowModel") / (FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".json"))))
			, Store(Path)
		{
		}

		~FPanelTempStore()
		{
			IFileManager::Get().Delete(*Path, false, true);
			IFileManager::Get().Delete(*(Path + TEXT(".tmp")), false, true);
		}
	};
}

// ---------------------------------------------------------------------------
// 选区与页面状态
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowSelectionTest, "DreamTexture.Editor.Reflow.Model.Selection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowSelectionTest::RunTest(const FString& Parameters)
{
	UMaterial* First = MakeMaterial();
	UMaterial* Second = MakeMaterial();

	FFake Fake;
	Fake.Results.Add(First->GetPathName(), PanelMakeAnalysis(First, TEXT("/Game/Test/M_First.M_First"), { PanelMakeRow(TEXT("T_A"), EDreamTextureRole::BaseColor, EDreamTextureEvidence::MaterialGraph), PanelMakeRow(TEXT("T_B"), EDreamTextureRole::Normal, EDreamTextureEvidence::MaterialGraph) }));
	Fake.Results.Add(Second->GetPathName(), PanelMakeAnalysis(Second, TEXT("/Game/Test/M_Second.M_Second"), { PanelMakeRow(TEXT("T_C"), EDreamTextureRole::Roughness, EDreamTextureEvidence::ParameterName) }));

	FDreamTextureReflowModel Model(Fake.Make());
	int32 Changes = 0;
	Model.OnChanged.AddLambda([&Changes]() { ++Changes; });

	// ---- 还没取过 / 取到空 ----
	TestEqual(TEXT("初始：没有选择"), static_cast<int32>(Model.GetViewState()), static_cast<int32>(FDreamTextureReflowModel::EViewState::NoSelection));
	TestFalse(TEXT("初始：说明文字不为空"), Model.GetStatusText().IsEmpty());
	Model.RefreshSelection();
	TestEqual(TEXT("取到空：仍是没有选择"), static_cast<int32>(Model.GetViewState()), static_cast<int32>(FDreamTextureReflowModel::EViewState::NoSelection));
	TestEqual(TEXT("没选东西时不解析"), Fake.AnalyzeCalls, 0);
	TestFalse(TEXT("没选东西时不能导出"), Model.CanExport());

	// ---- 一个候选：自动选中并解析 ----
	Fake.Materials = { First };
	Model.RefreshSelection();
	TestEqual(TEXT("一个候选"), Model.GetCandidates().Num(), 1);
	TestEqual(TEXT("自动选中"), Model.GetSelectedIndex(), 0);
	TestEqual(TEXT("解析了一次"), Fake.AnalyzeCalls, 1);
	TestEqual(TEXT("页面进入 Ready"), static_cast<int32>(Model.GetViewState()), static_cast<int32>(FDreamTextureReflowModel::EViewState::Ready));
	TestEqual(TEXT("两行"), Model.GetRows().Num(), 2);
	TestNotNull(TEXT("能按贴图路径找到行"), Model.FindTexture(TEXT("/Game/Test/T_A.T_A")));
	TestNull(TEXT("不存在的路径找不到"), Model.FindTexture(TEXT("/Game/Test/Nope.Nope")));
	TestTrue(TEXT("广播了变化"), Changes > 0);

	// ---- 再取一次：同一个还在 → 保持选中，且重新解析（图可能刚改过）----
	const FDreamTextureReflowRowItemPtr FirstRowBefore = Model.GetRows()[0];
	Model.RefreshSelection();
	TestEqual(TEXT("保持选中"), Model.GetSelectedIndex(), 0);
	TestEqual(TEXT("重新解析了"), Fake.AnalyzeCalls, 2);
	TestTrue(TEXT("同一路径的行复用同一个指针（列表控件的选中态与滚动位置不丢）"), Model.GetRows()[0] == FirstRowBefore);

	// ---- 多个候选：先不选，要用户挑 ----
	Fake.Materials = { First, Second };
	Fake.Slots.Reset();
	Model.RefreshSelection();
	TestEqual(TEXT("两个候选"), Model.GetCandidates().Num(), 2);
	TestEqual(TEXT("之前选的 First 还在，保持选中"), Model.GetSelectedIndex(), 0);

	// 选区变成另外两个（First 不在了）：没有可保持的，要重新挑。
	UMaterial* Third = MakeMaterial();
	Fake.Results.Add(Third->GetPathName(), PanelMakeAnalysis(Third, TEXT("/Game/Test/M_Third.M_Third"), {}));
	Fake.Materials = { Second, Third };
	Model.RefreshSelection();
	TestEqual(TEXT("选中的那个没了：不再选中"), Model.GetSelectedIndex(), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("页面要求挑一个"), static_cast<int32>(Model.GetViewState()), static_cast<int32>(FDreamTextureReflowModel::EViewState::PickOne));
	TestEqual(TEXT("没有行"), Model.GetRows().Num(), 0);
	TestTrue(TEXT("说明里带着候选数"), Model.GetStatusText().ToString().Contains(TEXT("2")));

	const int32 CallsBefore = Fake.AnalyzeCalls;
	Model.SelectCandidate(0);
	TestEqual(TEXT("挑了第一个：解析它"), Fake.AnalyzeCalls, CallsBefore + 1);
	TestEqual(TEXT("Ready"), static_cast<int32>(Model.GetViewState()), static_cast<int32>(FDreamTextureReflowModel::EViewState::Ready));
	TestEqual(TEXT("解析的是 Second（一行）"), Model.GetRows().Num(), 1);

	Model.SelectCandidate(1);
	TestEqual(TEXT("Third 没有任何贴图"), static_cast<int32>(Model.GetViewState()), static_cast<int32>(FDreamTextureReflowModel::EViewState::NoTextures));
	TestFalse(TEXT("NoTextures 有说明"), Model.GetStatusText().IsEmpty());

	Model.SelectCandidate(99);
	TestEqual(TEXT("越界下标：取消选中"), Model.GetSelectedIndex(), static_cast<int32>(INDEX_NONE));

	// ---- 关卡里的材质槽 ----
	UMaterial* SlotMaterial = MakeMaterial();
	Fake.Results.Add(SlotMaterial->GetPathName(), PanelMakeAnalysis(SlotMaterial, TEXT("/Game/Test/MI_Slot.MI_Slot"), { PanelMakeRow(TEXT("T_S"), EDreamTextureRole::BaseColor, EDreamTextureEvidence::MaterialGraph) }));
	Fake.Materials.Reset();
	Fake.Slots = { MakeSlot(TEXT("SM_Wall / 槽 0 · MI_Slot"), SlotMaterial) };
	Model.RefreshSelection();
	if (TestEqual(TEXT("一个槽候选"), Model.GetCandidates().Num(), 1))
	{
		TestTrue(TEXT("种类是材质槽"), Model.GetCandidates()[0].Kind == FDreamTextureReflowCandidate::EKind::ActorSlot);
		TestEqual(TEXT("槽候选自动选中并解析"), static_cast<int32>(Model.GetViewState()), static_cast<int32>(FDreamTextureReflowModel::EViewState::Ready));
	}

	// 同一个槽里换了材质：键不含材质，所以仍是「同一个槽」，保持选中。
	UMaterial* Replaced = MakeMaterial();
	Fake.Results.Add(Replaced->GetPathName(), PanelMakeAnalysis(Replaced, TEXT("/Game/Test/MI_Replaced.MI_Replaced"), {}));
	Fake.Slots = { MakeSlot(TEXT("SM_Wall / 槽 0 · MI_Slot"), Replaced) };
	Model.RefreshSelection();
	TestEqual(TEXT("槽里换了材质仍保持选中"), Model.GetSelectedIndex(), 0);
	TestEqual(TEXT("按新材质解析（没有贴图）"), static_cast<int32>(Model.GetViewState()), static_cast<int32>(FDreamTextureReflowModel::EViewState::NoTextures));

	// 空槽：没有材质。
	Fake.Slots = { MakeSlot(TEXT("SM_Wall / 槽 1 · （空）"), nullptr) };
	Model.RefreshSelection();
	TestEqual(TEXT("空槽"), static_cast<int32>(Model.GetViewState()), static_cast<int32>(FDreamTextureReflowModel::EViewState::EmptySlot));
	TestTrue(TEXT("空槽的说明点出了「空」"), Model.GetStatusText().ToString().Contains(TEXT("empty")));
	TestTrue(TEXT("候选标明是空槽"), Model.GetCandidates()[0].IsEmptySlot());

	// 材质被卸载（弱引用失效）：解析失败，说明里有原因。
	UMaterial* Doomed = MakeMaterial();
	Fake.Slots = { MakeSlot(TEXT("SM_Gone / 槽 0"), Doomed) };
	Model.RefreshSelection();
	TestEqual(TEXT("先是 Ready 之外的（没预设结果 → 失败）"), static_cast<int32>(Model.GetViewState()), static_cast<int32>(FDreamTextureReflowModel::EViewState::AnalysisFailed));
	TestTrue(TEXT("失败说明里是解析给的原因"), Model.GetStatusText().ToString().Contains(TEXT("没有预设")));
	Doomed->MarkAsGarbage();
	Model.Reanalyze();
	TestEqual(TEXT("材质没了：解析失败"), static_cast<int32>(Model.GetViewState()), static_cast<int32>(FDreamTextureReflowModel::EViewState::AnalysisFailed));
	TestTrue(TEXT("说明里点出材质不在了"), Model.GetStatusText().ToString().Contains(TEXT("no longer in memory")));

	// ---- 真实组件的槽：键是「组件 + 槽号」，标签里带着材质名，换材质标签就变，不能拿标签当键 ----
	{
		UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
		UMaterial* Before = MakeMaterial();
		UMaterial* After = MakeMaterial();
		Fake.Results.Add(Before->GetPathName(), PanelMakeAnalysis(Before, TEXT("/Game/Test/MI_Before.MI_Before"), { PanelMakeRow(TEXT("T_Z"), EDreamTextureRole::BaseColor, EDreamTextureEvidence::MaterialGraph) }));
		Fake.Results.Add(After->GetPathName(), PanelMakeAnalysis(After, TEXT("/Game/Test/MI_After.MI_After"), { PanelMakeRow(TEXT("T_Z"), EDreamTextureRole::BaseColor, EDreamTextureEvidence::MaterialGraph), PanelMakeRow(TEXT("T_Y"), EDreamTextureRole::Normal, EDreamTextureEvidence::MaterialGraph) }));

		FDreamTextureMaterialSlot SlotBefore = MakeSlot(TEXT("SM_Wall / 槽 0 · MI_Before"), Before);
		SlotBefore.Component = Component;
		SlotBefore.SlotIndex = 0;
		Fake.Slots = { SlotBefore };
		Model.RefreshSelection();
		TestEqual(TEXT("真实组件的槽：自动选中，一行"), Model.GetRows().Num(), 1);

		FDreamTextureMaterialSlot SlotAfter = MakeSlot(TEXT("SM_Wall / 槽 0 · MI_After"), After);
		SlotAfter.Component = Component;
		SlotAfter.SlotIndex = 0;
		FDreamTextureMaterialSlot OtherSlot = MakeSlot(TEXT("SM_Wall / 槽 1 · MI_Other"), Before);
		OtherSlot.Component = Component;
		OtherSlot.SlotIndex = 1;
		Fake.Slots = { OtherSlot, SlotAfter };
		Model.RefreshSelection();
		TestEqual(TEXT("槽里换了材质、标签变了、位置也变了：仍认出是同一个槽"), Model.GetSelectedIndex(), 1);
		TestEqual(TEXT("按新材质解析（两行）"), Model.GetRows().Num(), 2);
	}
	return true;
}

// ---------------------------------------------------------------------------
// 用户指认
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowOverridesTest, "DreamTexture.Editor.Reflow.Model.Overrides", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowOverridesTest::RunTest(const FString& Parameters)
{
	UMaterial* Material = MakeMaterial();

	FDreamTextureMaterialTexture Plain = PanelMakeRow(TEXT("T_Plain"), EDreamTextureRole::Unknown, EDreamTextureEvidence::None);
	Plain.bNeedsUserInput = true;
	FDreamTextureMaterialTexture Packed = PanelMakeRow(TEXT("T_Packed"), EDreamTextureRole::Packed, EDreamTextureEvidence::MaterialGraph);
	Packed.Layout.Channels[1] = EDreamTextureRole::Roughness;
	Packed.bNeedsUserInput = true;
	const FString PlainPath = Plain.TexturePath;
	const FString PackedPath = Packed.TexturePath;

	FPanelTempStore Temp;
	FFake Fake;
	Fake.Store = &Temp.Store;
	Fake.Materials = { Material };
	Fake.Results.Add(Material->GetPathName(), PanelMakeAnalysis(Material, TEXT("/Game/Test/MI_Over.MI_Over"), { Plain, Packed }));
	const FString AssetPath = TEXT("/Game/Test/MI_Over.MI_Over");

	FDreamTextureReflowModel Model(Fake.Make());
	FNoticeLog Notices;
	Notices.Bind(Model);
	Model.RefreshSelection();
	const int32 CallsAfterRefresh = Fake.AnalyzeCalls;

	// ---- 指认角色：写进库，并重新解析让行立刻变 ----
	Model.SetRole(PlainPath, EDreamTextureRole::Normal);
	FDreamTextureRoleOverride Stored;
	if (TestTrue(TEXT("角色指认记进了库"), Temp.Store.Find(AssetPath, PlainPath, Stored)))
	{
		TestEqual(TEXT("记的角色"), FString(LexToString(Stored.Role)), FString(TEXT("normal")));
	}
	TestEqual(TEXT("指认后重新解析了一次"), Fake.AnalyzeCalls, CallsAfterRefresh + 1);
	if (const FDreamTextureMaterialTexture* Row = Model.FindTexture(PlainPath))
	{
		TestEqual(TEXT("行变成了用户指认"), FString(LexToString(Row->Evidence)), FString(TEXT("user")));
		TestEqual(TEXT("行的角色"), FString(LexToString(Row->Role)), FString(TEXT("normal")));
	}
	TestTrue(TEXT("库文件落了盘"), IFileManager::Get().FileExists(*Temp.Path));

	// ---- 指认成 Packed 保留当前排布 ----
	Model.SetRole(PackedPath, EDreamTextureRole::Packed);
	if (TestTrue(TEXT("Packed 指认记进了库"), Temp.Store.Find(AssetPath, PackedPath, Stored)))
	{
		TestEqual(TEXT("保留了已知的 G=roughness"), FString(LexToString(Stored.Layout.Channels[1])), FString(TEXT("roughness")));
	}

	// ---- 改打包图的一个通道：其余保持 ----
	Model.SetLayoutChannel(PackedPath, 2, EDreamTextureRole::Metallic);
	if (TestTrue(TEXT("通道指认记进了库"), Temp.Store.Find(AssetPath, PackedPath, Stored)))
	{
		TestEqual(TEXT("B=metallic"), FString(LexToString(Stored.Layout.Channels[2])), FString(TEXT("metallic")));
		TestEqual(TEXT("G 仍是 roughness"), FString(LexToString(Stored.Layout.Channels[1])), FString(TEXT("roughness")));
		TestEqual(TEXT("角色是 packed"), FString(LexToString(Stored.Role)), FString(TEXT("packed")));
	}

	// ---- 对不是 Packed 的贴图改通道：先把它指认成 Packed，只有这一个通道有值 ----
	Model.ClearOverride(PlainPath);
	TestFalse(TEXT("撤销后库里没有了"), Temp.Store.Find(AssetPath, PlainPath, Stored));
	Model.SetLayoutChannel(PlainPath, 0, EDreamTextureRole::AO);
	if (TestTrue(TEXT("对普通贴图改通道：记进了库"), Temp.Store.Find(AssetPath, PlainPath, Stored)))
	{
		TestEqual(TEXT("角色变成 packed"), FString(LexToString(Stored.Role)), FString(TEXT("packed")));
		TestEqual(TEXT("R=ao"), FString(LexToString(Stored.Layout.Channels[0])), FString(TEXT("ao")));
		TestEqual(TEXT("其余未知"), FString(LexToString(Stored.Layout.Channels[1])), FString(TEXT("unknown")));
	}

	// ---- 指认成 Unknown = 撤销 ----
	Model.SetRole(PlainPath, EDreamTextureRole::Unknown);
	TestFalse(TEXT("SetRole(Unknown) 等于撤销"), Temp.Store.Find(AssetPath, PlainPath, Stored));

	// ---- 无效输入不碰库 ----
	const int32 CountBefore = Temp.Store.NumFor(AssetPath);
	Model.SetRole(TEXT("/Game/Test/Nope.Nope"), EDreamTextureRole::Normal);
	Model.SetLayoutChannel(PackedPath, 7, EDreamTextureRole::AO);
	Model.SetLayoutChannel(PackedPath, -1, EDreamTextureRole::AO);
	TestEqual(TEXT("不存在的贴图、越界通道都被忽略"), Temp.Store.NumFor(AssetPath), CountBefore);
	TestEqual(TEXT("正常操作没有报错通知"), Notices.Messages.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowOverrideFailureTest, "DreamTexture.Editor.Reflow.Model.OverrideFailure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowOverrideFailureTest::RunTest(const FString& Parameters)
{
	UMaterial* Material = MakeMaterial();
	FDreamTextureMaterialTexture Row = PanelMakeRow(TEXT("T_X"), EDreamTextureRole::Unknown, EDreamTextureEvidence::None);
	const FString RowPath = Row.TexturePath;

	// ---- 库文件写不出去：把「文件」的父级弄成一个普通文件，建目录必失败 ----
	const FString Blocker = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamTextureTest") / TEXT("ReflowModel") / (FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".blocker")));
	FFileHelper::SaveStringToFile(TEXT("x"), *Blocker);
	FDreamTextureOverrideStore Broken(Blocker / TEXT("overrides.json"));

	FFake Fake;
	Fake.Store = &Broken;
	Fake.Materials = { Material };
	Fake.Results.Add(Material->GetPathName(), PanelMakeAnalysis(Material, TEXT("/Game/Test/MI_Fail.MI_Fail"), { Row }));
	FDreamTextureReflowModel Model(Fake.Make());
	FNoticeLog Notices;
	Notices.Bind(Model);
	Model.RefreshSelection();

	Model.SetRole(RowPath, EDreamTextureRole::BaseColor);
	if (TestEqual(TEXT("写不出去：发了一条通知"), Notices.Messages.Num(), 1))
	{
		TestTrue(TEXT("是错误通知"), Notices.Errors[0]);
		TestTrue(TEXT("说明指认在本次会话里仍有效"), Notices.Messages[0].Contains(TEXT("this editor session")));
	}
	// 内存里仍然记着，重新解析后行用的是用户的。
	if (const FDreamTextureMaterialTexture* Found = Model.FindTexture(RowPath))
	{
		TestEqual(TEXT("写盘失败但本次会话里指认生效"), FString(LexToString(Found->Role)), FString(TEXT("basecolor")));
	}
	IFileManager::Get().Delete(*Blocker, false, true);

	// ---- 没有资产路径（运行时临时材质）：没法记，明说 ----
	{
		FDreamTextureMaterialAnalysis NoAsset = PanelMakeAnalysis(Material, TEXT("/Game/Test/Temp.Temp"), { Row });
		NoAsset.AssetPath.Reset();
		FFake Fake2;
		Fake2.Materials = { Material };
		Fake2.Results.Add(Material->GetPathName(), NoAsset);
		FPanelTempStore Temp;
		Fake2.Store = &Temp.Store;
		FDreamTextureReflowModel Model2(Fake2.Make());
		FNoticeLog Notices2;
		Notices2.Bind(Model2);
		Model2.RefreshSelection();
		Model2.SetRole(RowPath, EDreamTextureRole::Normal);
		TestEqual(TEXT("没有资产路径：发了一条通知"), Notices2.Messages.Num(), 1);
		TestEqual(TEXT("库里什么都没记"), Temp.Store.NumFor(TEXT("")), 0);
		TestFalse(TEXT("库文件没有被写出"), IFileManager::Get().FileExists(*Temp.Path));
	}
	return true;
}

// ---------------------------------------------------------------------------
// 导出
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowExportTest, "DreamTexture.Editor.Reflow.Model.Export", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowExportTest::RunTest(const FString& Parameters)
{
	UMaterial* Mixed = MakeMaterial();
	UMaterial* Blocked = MakeMaterial();

	FDreamTextureMaterialTexture Good = PanelMakeRow(TEXT("T_Good"), EDreamTextureRole::BaseColor, EDreamTextureEvidence::MaterialGraph);
	FDreamTextureMaterialTexture Bad = PanelMakeRow(TEXT("T_Bad"), EDreamTextureRole::Normal, EDreamTextureEvidence::MaterialGraph, false, TEXT("资产里没有源数据。"));
	FDreamTextureMaterialTexture AlsoBad = PanelMakeRow(TEXT("T_AlsoBad"), EDreamTextureRole::Normal, EDreamTextureEvidence::MaterialGraph, false, TEXT("虚拟纹理。"));

	FFake Fake;
	Fake.Materials = { Mixed };
	Fake.Results.Add(Mixed->GetPathName(), PanelMakeAnalysis(Mixed, TEXT("/Game/Test/M_Mixed.M_Mixed"), { Good, Bad }));
	Fake.Results.Add(Blocked->GetPathName(), PanelMakeAnalysis(Blocked, TEXT("/Game/Test/M_Blocked.M_Blocked"), { Bad, AlsoBad }));

	FDreamTextureReflowModel Model(Fake.Make());
	FNoticeLog Notices;
	Notices.Bind(Model);

	// ---- 没选东西 ----
	Model.Export();
	TestEqual(TEXT("没选东西：不调后端"), Fake.ExportCalls, 0);
	TestEqual(TEXT("没选东西：一条错误通知"), Notices.Messages.Num(), 1);
	TestTrue(TEXT("是错误通知"), Notices.Errors.Num() == 1 && Notices.Errors[0]);

	// ---- 一导一不导 ----
	Model.RefreshSelection();
	TestTrue(TEXT("有一张能导就能导出"), Model.CanExport());
	TestEqual(TEXT("能导的数量"), Model.GetExportableCount(), 1);
	TestFalse(TEXT("导出前没有目录"), Model.HasExported());
	TestNull(TEXT("导出前没有结果"), Model.FindExport(Good.TexturePath));

	Notices.Messages.Reset();
	Notices.Errors.Reset();
	Model.Export();
	TestEqual(TEXT("调了一次导出"), Fake.ExportCalls, 1);
	TestEqual(TEXT("向后端要了一个新的临时目录"), Fake.TempDirCalls, 1);
	if (TestEqual(TEXT("导出选项记下了"), Fake.ExportOptions.Num(), 1))
	{
		TestEqual(TEXT("用的是模型要来的临时目录"), Fake.ExportOptions[0].OutputDir, FString(TEXT("C:/Fake/Reflow/run1")));
		TestEqual(TEXT("默认不缩"), Fake.ExportOptions[0].MaxEdge, 0);
	}
	TestTrue(TEXT("导出过了"), Model.HasExported());
	TestEqual(TEXT("目录"), Model.GetExportDir(), FString(TEXT("C:/Fake/Reflow/run1")));
	if (const FDreamTextureExportedImage* Exported = Model.FindExport(Good.TexturePath))
	{
		TestTrue(TEXT("能导的导出了"), Exported->bExported);
		TestEqual(TEXT("哈希原样带回"), Exported->Sha256, FString(TEXT("0123456789abcdef")));
	}
	else
	{
		AddError(TEXT("找不到 T_Good 的导出结果"));
	}
	if (const FDreamTextureExportedImage* Refused = Model.FindExport(Bad.TexturePath))
	{
		TestFalse(TEXT("不能导的没导"), Refused->bExported);
		TestEqual(TEXT("原因原样带回"), Refused->Reason, FString(TEXT("资产里没有源数据。")));
	}
	else
	{
		AddError(TEXT("找不到 T_Bad 的导出结果"));
	}
	if (TestEqual(TEXT("导出完发了一条通知"), Notices.Messages.Num(), 1))
	{
		TestTrue(TEXT("通知里有目录"), Notices.Messages[0].Contains(TEXT("C:/Fake/Reflow/run1")));
		TestTrue(TEXT("通知里提了有一张导不出"), Notices.Messages[0].Contains(TEXT("could not be exported")));
		TestFalse(TEXT("部分成功不算错误"), Notices.Errors[0]);
	}

	// ---- 重新解析不丢导出结果；改指认后依然能看到上次导出的文件 ----
	Model.Reanalyze();
	TestNotNull(TEXT("重新解析后导出结果还在"), Model.FindExport(Good.TexturePath));

	// ---- 缩放选项透传 ----
	Model.SetExportMaxEdge(512);
	TestEqual(TEXT("记下了长边"), Model.GetExportMaxEdge(), 512);
	Model.Export();
	if (TestEqual(TEXT("第二次导出"), Fake.ExportOptions.Num(), 2))
	{
		TestEqual(TEXT("长边透传"), Fake.ExportOptions[1].MaxEdge, 512);
		TestEqual(TEXT("又要了一个新目录"), Fake.ExportOptions[1].OutputDir, FString(TEXT("C:/Fake/Reflow/run2")));
	}
	if (const FDreamTextureExportedImage* Resized = Model.FindExport(Good.TexturePath))
	{
		TestTrue(TEXT("缩过"), Resized->bResized);
	}
	Model.SetExportMaxEdge(-5);
	TestEqual(TEXT("负数当 0"), Model.GetExportMaxEdge(), 0);

	// ---- 换了对象，上一个对象的导出结果清掉 ----
	Fake.Materials = { Blocked };
	Model.RefreshSelection();
	TestFalse(TEXT("换了对象：导出目录清掉"), Model.HasExported());
	TestNull(TEXT("换了对象：导出结果清掉"), Model.FindExport(Good.TexturePath));

	// ---- 一张都导不出 ----
	TestFalse(TEXT("全导不出：CanExport 为假"), Model.CanExport());
	TestEqual(TEXT("全导不出：能导的数量为 0"), Model.GetExportableCount(), 0);
	const int32 CallsBefore = Fake.ExportCalls;
	Notices.Messages.Reset();
	Notices.Errors.Reset();
	Model.Export();
	TestEqual(TEXT("全导不出：不调后端"), Fake.ExportCalls, CallsBefore);
	if (TestEqual(TEXT("全导不出：一条错误通知"), Notices.Messages.Num(), 1))
	{
		TestTrue(TEXT("是错误通知"), Notices.Errors[0]);
	}
	return true;
}

// ---------------------------------------------------------------------------
// 文案
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowTextTest, "DreamTexture.Editor.Reflow.Model.Text", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowTextTest::RunTest(const FString& Parameters)
{
	const EDreamTextureRole Roles[] = {
		EDreamTextureRole::Unknown, EDreamTextureRole::BaseColor, EDreamTextureRole::Normal, EDreamTextureRole::Roughness, EDreamTextureRole::Metallic,
		EDreamTextureRole::AO, EDreamTextureRole::Height, EDreamTextureRole::Packed, EDreamTextureRole::Emissive, EDreamTextureRole::Opacity,
		EDreamTextureRole::Specular, EDreamTextureRole::Other,
	};
	TSet<FString> Seen;
	for (const EDreamTextureRole Role : Roles)
	{
		const FString Name = FDreamTextureReflowModel::RoleDisplayName(Role).ToString();
		TestFalse(FString::Printf(TEXT("%s 有中文名"), LexToString(Role)), Name.IsEmpty());
		TestFalse(FString::Printf(TEXT("%s 的中文名不重复"), LexToString(Role)), Seen.Contains(Name));
		Seen.Add(Name);
		TestFalse(FString::Printf(TEXT("%s 作为通道内容也有名字"), LexToString(Role)), FDreamTextureReflowModel::ChannelContentDisplayName(Role).IsEmpty());
	}
	TestEqual(TEXT("通道内容里的 Unknown 是问号"), FDreamTextureReflowModel::ChannelContentDisplayName(EDreamTextureRole::Unknown).ToString(), FString(TEXT("?")));

	for (const EDreamTextureEvidence Evidence : { EDreamTextureEvidence::None, EDreamTextureEvidence::UserOverride, EDreamTextureEvidence::SourceRecord,
		EDreamTextureEvidence::MaterialGraph, EDreamTextureEvidence::ParameterName, EDreamTextureEvidence::AssetName, EDreamTextureEvidence::SamplerType })
	{
		TestFalse(FString::Printf(TEXT("依据 %s 有中文名"), LexToString(Evidence)), FDreamTextureReflowModel::EvidenceDisplayName(Evidence).IsEmpty());
	}
	TestTrue(TEXT("采样器类型的依据标明是猜测"), FDreamTextureReflowModel::EvidenceDisplayName(EDreamTextureEvidence::SamplerType).ToString().Contains(TEXT("guess")));

	// ---- 用法描述 ----
	FDreamTextureMaterialTexture Row = PanelMakeRow(TEXT("T_Uses"), EDreamTextureRole::Packed, EDreamTextureEvidence::MaterialGraph);
	Row.Uses.Reset();
	FDreamTextureTextureUse Roughness;
	Roughness.Sink = EDreamTextureSink::Roughness;
	Roughness.Channels = DreamTextureChannelBit::G;
	Row.Uses.Add(Roughness);
	FDreamTextureTextureUse Uv;
	Uv.Sink = EDreamTextureSink::UVOffset;
	Uv.Channels = DreamTextureChannelBit::R | DreamTextureChannelBit::A;
	Uv.bApproximate = true;
	Row.Uses.Add(Uv);
	Row.Uses.Add(Roughness);
	TestEqual(TEXT("用法描述：属性名、通道、估计标记，重复的合并"), FDreamTextureReflowModel::DescribeUses(Row).ToString(), FString(TEXT("Roughness ← G; UV offset ← RA (approx.)")));

	Row.Uses.Reset();
	TestFalse(TEXT("没有使用记录也有话说"), FDreamTextureReflowModel::DescribeUses(Row).IsEmpty());
	return true;
}

// ---------------------------------------------------------------------------
// 外壳与面板
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowPanelTest, "DreamTexture.Editor.Reflow.Panel.Build", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowPanelTest::RunTest(const FString& Parameters)
{
	// 页枚举接在最后：它同时是 SWidgetSwitcher 的槽位下标，插在中间会让别人写死的下标错位。
	TestTrue(TEXT("回流页接在生成页之后"), static_cast<int32>(SDreamTextureShell::EPage::Reflow) > static_cast<int32>(SDreamTextureShell::EPage::Generate));
	TestTrue(TEXT("生成页接在连接状态页之后"), static_cast<int32>(SDreamTextureShell::EPage::Generate) > static_cast<int32>(SDreamTextureShell::EPage::Status));
	TestEqual(TEXT("素材库仍是 0"), static_cast<int32>(SDreamTextureShell::EPage::Library), 0);

	UMaterial* Material = MakeMaterial();
	UMaterial* Second = MakeMaterial();

	FDreamTextureMaterialTexture Base = PanelMakeRow(TEXT("T_Base"), EDreamTextureRole::BaseColor, EDreamTextureEvidence::SourceRecord);
	Base.Parameters = { TEXT("BaseColor") };
	Base.bOverriddenBySelectedInstance = true;
	FDreamTextureMaterialTexture Packed = PanelMakeRow(TEXT("T_Packed"), EDreamTextureRole::Packed, EDreamTextureEvidence::MaterialGraph);
	Packed.Layout.Channels[1] = EDreamTextureRole::Roughness;
	Packed.bNeedsUserInput = true;
	Packed.Notes = { TEXT("测试提示一"), TEXT("测试提示二") };
	Packed.Parameters = { TEXT("ORM"), TEXT("Extra") };
	FDreamTextureMaterialTexture Unknown = PanelMakeRow(TEXT("T_Unknown"), EDreamTextureRole::Unknown, EDreamTextureEvidence::None, false, TEXT("资产里没有源数据。"));
	Unknown.bNeedsUserInput = true;

	FDreamTextureMaterialAnalysis Analysis = PanelMakeAnalysis(Material, TEXT("/Game/Test/MI_Panel.MI_Panel"), { Base, Packed, Unknown });
	Analysis.bIsInstance = true;
	Analysis.BaseMaterialPath = TEXT("/Game/Test/M_Master.M_Master");
	Analysis.Warnings = { TEXT("测试警告") };

	// 指认库指到临时文件：下面会改角色，不能写进宿主工程的 Saved/DreamTexture 里。
	FPanelTempStore Temp;
	FFake Fake;
	Fake.Store = &Temp.Store;
	Fake.Materials = { Material, Second };
	Fake.Results.Add(Material->GetPathName(), Analysis);
	Fake.Results.Add(Second->GetPathName(), PanelMakeAnalysis(Second, TEXT("/Game/Test/M_Other.M_Other"), {}));

	const TSharedRef<FDreamTextureReflowModel> Model = MakeShared<FDreamTextureReflowModel>(Fake.Make());

	{
		TSharedRef<SDreamTextureReflowPanel> Panel = SNew(SDreamTextureReflowPanel).Model(Model).bThumbnails(false).bAutoRefresh(false).bActions(false);
		TestTrue(TEXT("面板用的是注入的模型"), Panel->GetModel() == Model);
		TestTrue(TEXT("不建动作区时，给回流留的挂载点是个空容器"), Panel->GetActionsHost()->GetChildren()->GetChildAt(0) == SNullWidget::NullWidget);

		Panel->BuildWidgetsForTest();  // 没有选区
		Model->RefreshSelection();      // 两个候选，没挑
		Panel->BuildWidgetsForTest();
		Model->SelectCandidate(0);      // Ready：三行
		Panel->BuildWidgetsForTest();
		Model->SelectCandidate(1);      // 没有贴图
		Panel->BuildWidgetsForTest();
		Model->SelectCandidate(0);
		Model->Export();                 // 有导出结果
		Panel->BuildWidgetsForTest();
		Model->SetRole(Packed.TexturePath, EDreamTextureRole::Normal);
		Panel->BuildWidgetsForTest();
	}

	// 面板先销毁、模型后销毁：之后模型再广播不能崩（面板析构里摘了委托）。
	Model->RefreshSelection();
	Model->Reanalyze();
	return true;
}

// ---------------------------------------------------------------------------
// 带资产缩略图的面板：真实贴图的行能建出缩略图控件（只构造，不渲染——渲染要真实 RHI）
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamTextureReflowPanelThumbnailsTest, "DreamTexture.Editor.Reflow.Panel.Thumbnails", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDreamTextureReflowPanelThumbnailsTest::RunTest(const FString& Parameters)
{
	UTexture2D* Texture = UTexture2D::CreateTransient(4, 4, PF_B8G8R8A8);
	if (!TestNotNull(TEXT("造好一张贴图"), Texture))
	{
		return false;
	}

	UMaterial* Material = MakeMaterial();
	FDreamTextureMaterialTexture Row = PanelMakeRow(TEXT("T_Thumb"), EDreamTextureRole::BaseColor, EDreamTextureEvidence::MaterialGraph, false, TEXT("测试：没有源数据。"));
	Row.Texture = Texture;

	FPanelTempStore Temp;
	FFake Fake;
	Fake.Store = &Temp.Store;
	Fake.Materials = { Material };
	Fake.Results.Add(Material->GetPathName(), PanelMakeAnalysis(Material, TEXT("/Game/Test/M_Thumb.M_Thumb"), { Row }));

	const TSharedRef<FDreamTextureReflowModel> Model = MakeShared<FDreamTextureReflowModel>(Fake.Make());
	TSharedRef<SDreamTextureReflowPanel> Panel = SNew(SDreamTextureReflowPanel).Model(Model).bThumbnails(true).bAutoRefresh(false).bActions(false);
	Model->RefreshSelection();
	Panel->BuildWidgetsForTest();
	TestEqual(TEXT("有一行"), Model->GetRows().Num(), 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
