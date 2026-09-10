#include "MBSTSingleTurretEditorLibrary.h"
#include "MBSTMaskedDepthMaterial.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "MBSTSingleTurretAsset.h"
#include "MBSTSingleTurretBenchmark.h"
#include "MBSTMobileFireDemo.h"
#include "MBSTMobileFireProfile.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DataAssets/MassBattleAgentConfigDataAsset.h"
#include "DataAssets/MassBattleProjectileConfigDataAsset.h"
#include "Components/ActorComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Fragments/Team.h"
#include "HAL/PlatformTime.h"
#include "Materials/Material.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionGetMaterialAttributes.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraSystem.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "RHI.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UObjectIterator.h"
#include "UObject/StrongObjectPtr.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "GameFramework/WorldSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMBSTMaterialDecodeCompatibilityTest,
    "MassBattle.SingleTurret.Material.FrameworkDecodeCompatibility",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMBSTMaterialDecodeCompatibilityTest::RunTest(const FString& Parameters)
{
    UMaterial* Source = LoadObject<UMaterial>(nullptr,
        TEXT("/MassBattleSingleTurret/Materials/M_MBST_VATSingleTurret.M_MBST_VATSingleTurret"));
    if (!TestNotNull(TEXT("Turret base material"), Source))
    {
        return false;
    }
    UMaterial* Material = DuplicateObject<UMaterial>(Source, GetTransientPackage());
    UMaterialExpressionCustom* Decoder = nullptr;
    for (UMaterialExpression* Expression : Material->GetExpressions())
    {
        UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expression);
        if (Custom && Custom->AdditionalOutputs.Num() == 6
            && Custom->AdditionalOutputs[0].OutputName == TEXT("Team"))
        {
            Decoder = Custom;
            break;
        }
    }
    if (!TestNotNull(TEXT("Framework DP0 decoder"), Decoder)
        || !TestEqual(TEXT("Packed input count"), Decoder->Inputs.Num(), 1))
    {
        return false;
    }
    const FExpressionInput PackedInput = Decoder->Inputs[0].Input;
    const int32 ExpressionCount = Material->GetExpressions().Num();
    // Reproduce an asset copied before the framework changed its wire format.
    Decoder->Code = TEXT("uint u = asuint(In); Team = float(u & 1023u); Dissolve = float((u >> 10u) & 1023u) / 1023.0; LODIndex = float((u >> 20u) & 511u); DrawLOD = float((u >> 29u) & 1u); BeingSelect = float((u >> 30u) & 1u); Selected = float((u >> 31u) & 1u); return 0.0;");
    Decoder->IncludeFilePaths.Reset();
    FString Message;
    TestTrue(TEXT("Existing authoring entry upgrades and compiles the legacy decoder"),
        UMBSTSingleTurretEditorLibrary::ConfigureArticulationBaseMaterial(Material, Message));
    AddInfo(Message);
    TestTrue(TEXT("Uses the framework's canonical decoder"), Decoder->Code.Contains(TEXT("MassBattle_UnpackDP0W(")));
    TestTrue(TEXT("Includes the framework shader contract"),
        Decoder->IncludeFilePaths.Contains(TEXT("/MassBattle/MassBattle_MaterialDecode.ush")));
    TestTrue(TEXT("Preserves the packed parameter connection"),
        Decoder->Inputs[0].Input.Expression == PackedInput.Expression
        && Decoder->Inputs[0].Input.OutputIndex == PackedInput.OutputIndex);
    const int32 MigratedExpressionCount = Material->GetExpressions().Num();
    TestTrue(TEXT("Preserves the source graph while installing material compatibility expressions"),
        MigratedExpressionCount >= ExpressionCount);
    TestTrue(TEXT("Migration can run again"),
        UMBSTSingleTurretEditorLibrary::ConfigureArticulationBaseMaterial(Material, Message));
    TestEqual(TEXT("Migration does not duplicate graph nodes"), Material->GetExpressions().Num(), MigratedExpressionCount);
    return !HasAnyErrors();
}

namespace MBSTMaskedDepthTests
{
    template<typename T>
    T* AddExpression(UMaterial* Material)
    {
        return CastChecked<T>(UMaterialEditingLibrary::CreateMaterialExpression(Material, T::StaticClass()));
    }

    UMaterial* CreateSurface()
    {
        UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
        Material->BlendMode = BLEND_Masked;
        Material->bUsedWithNiagaraMeshParticles = true;
        UMaterialExpressionConstant3Vector* Offset = AddExpression<UMaterialExpressionConstant3Vector>(Material);
        Offset->Constant = FLinearColor(0.0f, 0.0f, 50.0f);
        Material->GetEditorOnlyData()->WorldPositionOffset.Connect(0, Offset);
        Material->GetEditorOnlyData()->WorldPositionOffset.UseConstant = false;
        return Material;
    }

    bool CheckCompiledMaskUsage(FAutomationTestBase& Test, UMaterial* Material, bool bExpected)
    {
        FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform);
        if (!Test.TestNotNull(TEXT("Compiled material resource"), Resource))
        {
            return false;
        }
        Resource->FinishCompilation();
        for (const FString& Error : Resource->GetCompileErrors())
        {
            Test.AddError(Error);
        }
        const FMaterialShaderMap* ShaderMap = Resource->GetGameThreadShaderMap();
        if (!Test.TestNotNull(TEXT("Compiled material shader map"), ShaderMap))
        {
            return false;
        }
        Test.TestTrue(TEXT("Regression material displaces vertices in the depth and base passes"),
            bool(ShaderMap->GetCompilationOutput().bUsesWorldPositionOffset));
        return Test.TestEqual(TEXT("Compiler retains the opacity mask required by the runtime depth pass"),
            bool(ShaderMap->GetCompilationOutput().bUsesOpacityMask), bExpected);
    }

    void Recompile(FAutomationTestBase& Test, UMaterial* Material)
    {
        for (const FString& Error : UMaterialEditingLibrary::RecompileMaterial(Material))
        {
            Test.AddError(Error);
        }
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMBSTMaskedDepthInputsTest,
    "MassBattle.SingleTurret.Material.MaskedDepthInputs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMBSTMaskedDepthInputsTest::RunTest(const FString& Parameters)
{
    // Cover both a dormant graph hidden by UseConstant and an unconnected default.
    for (int32 CaseIndex = 0; CaseIndex < 3; ++CaseIndex)
    {
        TStrongObjectPtr<UMaterial> MaterialOwner(MBSTMaskedDepthTests::CreateSurface());
        UMaterial* Material = MaterialOwner.Get();
        UMaterialEditorOnlyData* Data = Material->GetEditorOnlyData();
        const bool bInlineConstant = CaseIndex != 2;
        const float EffectiveMask = CaseIndex == 1 ? 0.125f : 1.0f;
        if (bInlineConstant)
        {
            UMaterialExpressionConstant* DormantMask = MBSTMaskedDepthTests::AddExpression<UMaterialExpressionConstant>(Material);
            DormantMask->R = 0.75f;
            Data->OpacityMask.Connect(0, DormantMask);
        }
        Data->OpacityMask.UseConstant = bInlineConstant;
        Data->OpacityMask.Constant = EffectiveMask;
        const FExpressionInput OriginalWPO = Data->WorldPositionOffset;
        const float OriginalClip = Material->OpacityMaskClipValue;
        MBSTMaskedDepthTests::Recompile(*this, Material);
        MBSTMaskedDepthTests::CheckCompiledMaskUsage(*this, Material, EffectiveMask != 1.0f);

        FString Message;
        if (!TestTrue(TEXT("Installs masked depth compatibility"),
                MBSTEditorPrivate::EnsureMaskedDepthMaterial(Material, Message)))
        {
            AddError(Message);
            continue;
        }
        MBSTMaskedDepthTests::CheckCompiledMaskUsage(*this, Material, true);
        TestFalse(TEXT("The root mask evaluates its new connection"), bool(Data->OpacityMask.UseConstant));
        UMaterialExpressionCustom* Wrapper = Cast<UMaterialExpressionCustom>(Data->OpacityMask.Expression);
        if (TestNotNull(TEXT("Final mask wrapper"), Wrapper)
            && TestEqual(TEXT("One preserved mask input"), Wrapper->Inputs.Num(), 1))
        {
            UMaterialExpressionConstant* Constant = Cast<UMaterialExpressionConstant>(Wrapper->Inputs[0].Input.Expression);
            if (TestNotNull(TEXT("Effective inline/default constant is preserved"), Constant))
            {
                TestEqual(TEXT("Mask value is unchanged, including values below the clipping threshold"), Constant->R, EffectiveMask);
            }
        }
        TestEqual(TEXT("Preserves blend mode"), Material->BlendMode, TEnumAsByte<EBlendMode>(BLEND_Masked));
        TestEqual(TEXT("Preserves opacity-mask clipping threshold"), Material->OpacityMaskClipValue, OriginalClip);
        TestTrue(TEXT("Preserves the original WPO connection"), Data->WorldPositionOffset.Expression == OriginalWPO.Expression
            && Data->WorldPositionOffset.OutputIndex == OriginalWPO.OutputIndex);
        const int32 Count = Material->GetExpressions().Num();
        TestTrue(TEXT("Repeated mask migration succeeds"), MBSTEditorPrivate::EnsureMaskedDepthMaterial(Material, Message));
        TestEqual(TEXT("Repeated mask migration does not grow the graph"), Material->GetExpressions().Num(), Count);
    }
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMBSTMaskedDepthAttributesTest,
    "MassBattle.SingleTurret.Material.MaskedDepthAttributesAndSwitches",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMBSTMaskedDepthAttributesTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UMaterial> MaterialOwner(MBSTMaskedDepthTests::CreateSurface());
    UMaterial* Material = MaterialOwner.Get();
    UMaterialEditorOnlyData* Data = Material->GetEditorOnlyData();
    Material->bUseMaterialAttributes = true;
    UMaterialExpressionConstant* One = MBSTMaskedDepthTests::AddExpression<UMaterialExpressionConstant>(Material);
    One->R = 1.0f;
    UMaterialExpressionConstant* Cutout = MBSTMaskedDepthTests::AddExpression<UMaterialExpressionConstant>(Material);
    Cutout->R = 0.125f;
    UMaterialExpressionStaticSwitchParameter* Switch =
        MBSTMaskedDepthTests::AddExpression<UMaterialExpressionStaticSwitchParameter>(Material);
    Switch->ParameterName = TEXT("MBST_TestMaskSwitch");
    Switch->ExpressionGUID = FGuid::NewGuid();
    Switch->DynamicBranch = false;
    Switch->DefaultValue = true;
    Switch->A.Connect(0, One);
    Switch->B.Connect(0, Cutout);
    UMaterialExpressionSetMaterialAttributes* OriginalAttributes =
        MBSTMaskedDepthTests::AddExpression<UMaterialExpressionSetMaterialAttributes>(Material);
    OriginalAttributes->ConnectInputAttribute(MP_OpacityMask, Switch);
    OriginalAttributes->ConnectInputAttribute(MP_WorldPositionOffset, Data->WorldPositionOffset.Expression);
    Data->MaterialAttributes.Connect(0, OriginalAttributes);
    MBSTMaskedDepthTests::Recompile(*this, Material);
    MBSTMaskedDepthTests::CheckCompiledMaskUsage(*this, Material, false);

    // Surface masters must also be prepared when currently opaque: a material
    // instance can later select Masked without rebuilding the parent graph.
    Material->BlendMode = BLEND_Opaque;
    FString Message;
    if (!TestTrue(TEXT("Installs compatibility on a surface master before a masked override"),
            MBSTEditorPrivate::EnsureMaskedDepthMaterial(Material, Message)))
    {
        AddError(Message);
        return false;
    }
    TestTrue(TEXT("Preserves material-attributes mode"), bool(Material->bUseMaterialAttributes));
    TestEqual(TEXT("Does not force the master blend mode"), Material->BlendMode, TEnumAsByte<EBlendMode>(BLEND_Opaque));
    UMaterialExpressionSetMaterialAttributes* FinalAttributes =
        Cast<UMaterialExpressionSetMaterialAttributes>(Data->MaterialAttributes.Expression);
    if (TestNotNull(TEXT("Final attributes wrapper"), FinalAttributes)
        && TestEqual(TEXT("Only overrides the opacity mask"), FinalAttributes->Inputs.Num(), 2))
    {
        TestTrue(TEXT("All original material attributes are retained"), FinalAttributes->Inputs[0].Expression == OriginalAttributes);
        UMaterialExpressionCustom* Wrapper = Cast<UMaterialExpressionCustom>(FinalAttributes->Inputs[1].Expression);
        if (TestNotNull(TEXT("Final attributes mask wrapper"), Wrapper)
            && TestEqual(TEXT("One original attributes mask input"), Wrapper->Inputs.Num(), 1))
        {
            UMaterialExpressionGetMaterialAttributes* Get =
                Cast<UMaterialExpressionGetMaterialAttributes>(Wrapper->Inputs[0].Input.Expression);
            if (TestNotNull(TEXT("Reads the final source attributes mask"), Get))
            {
                TestTrue(TEXT("Reads the same attributes that are passed through"), Get->MaterialAttributes.Expression == OriginalAttributes);
            }
        }
    }
    Material->BlendMode = BLEND_Masked;
    MBSTMaskedDepthTests::Recompile(*this, Material);
    MBSTMaskedDepthTests::CheckCompiledMaskUsage(*this, Material, true);
    Switch->DefaultValue = false;
    MBSTMaskedDepthTests::Recompile(*this, Material);
    MBSTMaskedDepthTests::CheckCompiledMaskUsage(*this, Material, true);
    TestTrue(TEXT("Preserves both static-switch mask branches"), Switch->A.Expression == One && Switch->B.Expression == Cutout);
    TestEqual(TEXT("Preserves the cutout branch value"), Cutout->R, 0.125f);
    const int32 Count = Material->GetExpressions().Num();
    TestTrue(TEXT("Repeated attributes migration succeeds"), MBSTEditorPrivate::EnsureMaskedDepthMaterial(Material, Message));
    TestEqual(TEXT("Repeated attributes migration does not grow the graph"), Material->GetExpressions().Num(), Count);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMBSTConversionPreservesSurfaceUVTest,
    "MassBattle.SingleTurret.Authoring.PreservesSurfaceUVChannels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMBSTConversionPreservesSurfaceUVTest::RunTest(const FString& Parameters)
{
    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!TestNotNull(TEXT("Source cube"), Cube)
        || !TestNotNull(TEXT("Editor world"), GEditor ? GEditor->GetEditorWorldContext().World() : nullptr))
    {
        return false;
    }

    const FVector2f SurfaceUVs[] = {
        FVector2f(0.125f, 0.875f),
        FVector2f(0.375f, 0.625f),
        FVector2f(0.75f, 0.25f)
    };
    UStaticMesh* Parts[3] = {};
    for (int32 PartIndex = 0; PartIndex < UE_ARRAY_COUNT(Parts); ++PartIndex)
    {
        UStaticMesh* Part = DuplicateObject<UStaticMesh>(Cube, GetTransientPackage());
        Parts[PartIndex] = Part;
        FMeshDescription* Description = Part ? Part->GetMeshDescription(0) : nullptr;
        if (!TestNotNull(TEXT("Editable source part"), Description))
        {
            return false;
        }
        FStaticMeshAttributes Attributes(*Description);
        Attributes.Register(true);
        Description->SetNumUVChannels(2);
        TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
        UVs.SetNumChannels(2);
        for (const FVertexInstanceID Vertex : Description->VertexInstances().GetElementIDs())
        {
            UVs.Set(Vertex, 1, SurfaceUVs[PartIndex]);
        }
        Part->GetSourceModel(0).BuildSettings.bGenerateLightmapUVs = false;
        Part->CommitMeshDescription(0);
        Part->Build(false);
        if (!TestEqual(TEXT("Source render data retains UV0 and UV1"),
            Part->GetRenderData()->LODResources[0].VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords(), 2u))
        {
            return false;
        }
    }

    FMBSTActorToSingleTurretSettings Settings;
    Settings.bGenerateLightmapUVs = false;
    Settings.bCreateVATDataAsset = false;
    Settings.bCreatePivotSockets = false;
    const FString OutputRoot = TEXT("/Game/Developers/MBSTSurfaceUVTest/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FMBSTActorToSingleTurretResult Result =
        UMBSTSingleTurretEditorLibrary::ConvertStaticMeshesToSingleTurret(
            Parts[0], Parts[1], Parts[2], nullptr,
            FVector::ZeroVector, FVector(25.0, 0.0, 25.0), FVector(100.0, 0.0, 25.0),
            0.0f, 45.0f, 30.0f, OutputRoot, TEXT("SurfaceUVTank"), Settings);
    ON_SCOPE_EXIT
    {
        UObject* GeneratedAssets[] = { Result.ArticulatedMesh.Get(), Result.LayoutAsset.Get(), Result.AgentConfig.Get() };
        for (UObject* Asset : GeneratedAssets)
        {
            if (Asset)
            {
                FAssetRegistryModule::AssetDeleted(Asset);
                Asset->ClearFlags(RF_Public | RF_Standalone);
                Asset->GetOutermost()->SetDirtyFlag(false);
            }
        }
    };
    for (const FString& Message : Result.Messages)
    {
        AddInfo(Message);
    }
    if (!TestTrue(TEXT("Public mechanical conversion succeeds"), Result.bSucceeded)
        || !TestNotNull(TEXT("Generated mesh"), Result.ArticulatedMesh.Get()))
    {
        return false;
    }
    const FMeshDescription* Description = Result.ArticulatedMesh->GetMeshDescription(0);
    if (!TestNotNull(TEXT("Generated mesh description"), Description))
    {
        return false;
    }
    const FStaticMeshConstAttributes Attributes(*Description);
    const TVertexInstanceAttributesConstRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
    // RawMesh conversion has one UV element channel. Re-registering attributes
    // during mask painting used to truncate its two vertex-instance UV channels.
    if (!TestEqual(TEXT("Articulation mask painting preserves both surface UV channels"), UVs.GetNumChannels(), 2))
    {
        return false;
    }
    TestEqual(TEXT("Generated render buffer also retains both surface UV channels"),
        Result.ArticulatedMesh->GetRenderData()->LODResources[0].VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords(), 2u);
    const TVertexInstanceAttributesConstRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
    int32 Counts[3] = {};
    bool bAllSurfaceUVsPreserved = true;
    for (const FVertexInstanceID Vertex : Description->VertexInstances().GetElementIDs())
    {
        const FVector4f Mask = Colors[Vertex];
        const int32 PartIndex = Mask.X < 0.5f ? 0 : (Mask.Y < 0.5f ? 1 : 2);
        ++Counts[PartIndex];
        bAllSurfaceUVsPreserved &= UVs.Get(Vertex, 1).Equals(SurfaceUVs[PartIndex], 0.0001f);
    }
    TestTrue(TEXT("All body, turret and barrel UV1 values survive conversion"), bAllSurfaceUVsPreserved);
    TestTrue(TEXT("UV verification covers all three articulation masks"), Counts[0] > 0 && Counts[1] > 0 && Counts[2] > 0);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMBSTMobileFireAgentConfigContractTest,
    "MassBattle.SingleTurret.MobileFire.AgentConfigContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMBSTMobileFireAgentConfigContractTest::RunTest(const FString& Parameters)
{
    UMassBattleAgentConfigDataAsset* SourceConfig = LoadObject<UMassBattleAgentConfigDataAsset>(
        nullptr,
        TEXT("/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret_AgentConfig.Tank_SingleTurret_AgentConfig"));
    if (!TestNotNull(TEXT("Existing single-turret demo AgentConfig"), SourceConfig))
    {
        return false;
    }

    UMassBattleAgentConfigDataAsset* TestConfig = DuplicateObject<UMassBattleAgentConfigDataAsset>(
        SourceConfig,
        GetTransientPackage());
    UMBSTMobileFireProfile* StopProfile = NewObject<UMBSTMobileFireProfile>(GetTransientPackage());
    UMBSTMobileFireProfile* MoveProfile = NewObject<UMBSTMobileFireProfile>(GetTransientPackage());
    if (!TestNotNull(TEXT("Transient AgentConfig copy"), TestConfig)
        || !TestNotNull(TEXT("Stop-to-fire profile"), StopProfile)
        || !TestNotNull(TEXT("Move-fire profile"), MoveProfile))
    {
        return false;
    }

    StopProfile->MobilityPolicy = EMBSTFireMobilityPolicy::AimWhileMovingStopToFire;
    StopProfile->MaxFireLinearSpeed = 10.0f;
    StopProfile->MaxFireAngularSpeed = 2.0f;
    StopProfile->bBrakeImmediatelyWhenTargetInRange = true;

    MoveProfile->MobilityPolicy = EMBSTFireMobilityPolicy::AimAndFireWhileMoving;
    MoveProfile->MaxFireLinearSpeed = 1000000.0f;
    MoveProfile->MaxFireAngularSpeed = 1000000.0f;

    FString Message;
    FMBSTMobileFireState InitialState;
    TestTrue(
        TEXT("Configure stop-to-fire contract"),
        UMBSTSingleTurretEditorLibrary::ConfigureAgentConfigMobileFire(
            TestConfig,
            StopProfile,
            InitialState,
            true,
            Message));
    TestTrue(
        TEXT("Stop-to-fire contract validates"),
        UMBSTSingleTurretEditorLibrary::ValidateAgentConfigMobileFire(
            TestConfig,
            StopProfile).bValid);
    TestFalse(TEXT("Built-in attack is suppressed"), TestConfig->Attack.bEnable);
    TestFalse(TEXT("Built-in chase is suppressed"), TestConfig->Chase.bEnable);

    TestTrue(
        TEXT("Replace with move-fire contract"),
        UMBSTSingleTurretEditorLibrary::ConfigureAgentConfigMobileFire(
            TestConfig,
            MoveProfile,
            InitialState,
            true,
            Message));
    TestTrue(
        TEXT("Move-fire contract validates"),
        UMBSTSingleTurretEditorLibrary::ValidateAgentConfigMobileFire(
            TestConfig,
            MoveProfile).bValid);

    TestTrue(
        TEXT("Remove mobile-fire contract without removing turret support"),
        UMBSTSingleTurretEditorLibrary::ConfigureAgentConfigMobileFire(
            TestConfig,
            nullptr,
            InitialState,
            false,
            Message));
    TestTrue(
        TEXT("Single-turret contract remains valid"),
        UMBSTSingleTurretEditorLibrary::ValidateAgentConfigSingleTurret(
            TestConfig,
            nullptr).bValid);
    TestFalse(
        TEXT("Removed mobile-fire contract no longer validates"),
        UMBSTSingleTurretEditorLibrary::ValidateAgentConfigMobileFire(
            TestConfig,
            nullptr).bValid);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMBSTCreateMobileFireDemoTest,
    "MassBattle.SingleTurret.MobileFire.CreateDemoAssets",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMBSTCreateMobileFireDemoTest::RunTest(const FString& Parameters)
{
    static const FString DemoRoot = TEXT("/MassBattleSingleTurret/Demo/MobileFire");
    UEditorAssetSubsystem* AssetSubsystem = GEditor
        ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>()
        : nullptr;
    if (!TestNotNull(TEXT("Editor asset subsystem"), AssetSubsystem))
    {
        return false;
    }

    UMassBattleAgentConfigDataAsset* BaseConfig = LoadObject<UMassBattleAgentConfigDataAsset>(
        nullptr,
        TEXT("/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret_AgentConfig.Tank_SingleTurret_AgentConfig"));
    if (!TestNotNull(TEXT("Base single-turret AgentConfig"), BaseConfig))
    {
        return false;
    }

    auto GetOrCreateProfile = [&](const TCHAR* Name) -> UMBSTMobileFireProfile*
    {
        const FString PackageName = DemoRoot / Name;
        const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, Name);
        if (UMBSTMobileFireProfile* Existing = LoadObject<UMBSTMobileFireProfile>(nullptr, *ObjectPath))
        {
            return Existing;
        }
        UPackage* Package = CreatePackage(*PackageName);
        UMBSTMobileFireProfile* Created = NewObject<UMBSTMobileFireProfile>(
            Package,
            Name,
            RF_Public | RF_Standalone | RF_Transactional);
        if (Created)
        {
            FAssetRegistryModule::AssetCreated(Created);
        }
        return Created;
    };

    UMBSTMobileFireProfile* StopProfile = GetOrCreateProfile(TEXT("DA_MBST_StopToFire"));
    UMBSTMobileFireProfile* MoveProfile = GetOrCreateProfile(TEXT("DA_MBST_FireWhileMoving"));
    if (!TestNotNull(TEXT("Persistent stop-to-fire profile"), StopProfile)
        || !TestNotNull(TEXT("Persistent fire-while-moving profile"), MoveProfile))
    {
        return false;
    }

    StopProfile->Modify();
    StopProfile->MobilityPolicy = EMBSTFireMobilityPolicy::AimWhileMovingStopToFire;
    StopProfile->MinimumRange = 0.0f;
    StopProfile->MaximumRange = 100000.0f;
    StopProfile->TurretYawToleranceDegrees = 6.0f;
    StopProfile->BarrelPitchToleranceDegrees = 6.0f;
    StopProfile->MaxFireLinearSpeed = 25.0f;
    StopProfile->MaxFireAngularSpeed = 5.0f;
    StopProfile->BrakeLeadTimeSeconds = 0.15f;
    StopProfile->bBrakeImmediatelyWhenTargetInRange = false;
    StopProfile->bHoldDuringWindup = true;
    StopProfile->bHoldDuringRecover = true;
    StopProfile->WindupSeconds = 0.1f;
    StopProfile->RecoverSeconds = 0.15f;
    StopProfile->CooldownSeconds = 1.25f;
    StopProfile->bSpawnMassBattleProjectile = false;
    StopProfile->bEmitFireRequest = true;
    StopProfile->bBroadcastBlueprintFireEvent = false;
    StopProfile->bDisableBuiltInAttack = true;
    StopProfile->bDisableBuiltInChase = true;
    StopProfile->PostEditChange();
    StopProfile->MarkPackageDirty();

    MoveProfile->Modify();
    MoveProfile->MobilityPolicy = EMBSTFireMobilityPolicy::AimAndFireWhileMoving;
    MoveProfile->MinimumRange = 0.0f;
    MoveProfile->MaximumRange = 100000.0f;
    MoveProfile->TurretYawToleranceDegrees = 6.0f;
    MoveProfile->BarrelPitchToleranceDegrees = 6.0f;
    MoveProfile->MaxFireLinearSpeed = 1000000.0f;
    MoveProfile->MaxFireAngularSpeed = 1000000.0f;
    MoveProfile->bBrakeImmediatelyWhenTargetInRange = false;
    MoveProfile->WindupSeconds = 0.1f;
    MoveProfile->RecoverSeconds = 0.15f;
    MoveProfile->CooldownSeconds = 1.25f;
    MoveProfile->bSpawnMassBattleProjectile = false;
    MoveProfile->bEmitFireRequest = true;
    MoveProfile->bBroadcastBlueprintFireEvent = false;
    MoveProfile->bDisableBuiltInAttack = true;
    MoveProfile->bDisableBuiltInChase = true;
    MoveProfile->PostEditChange();
    MoveProfile->MarkPackageDirty();

    auto GetOrCreateConfig = [&](const TCHAR* Name) -> UMassBattleAgentConfigDataAsset*
    {
        const FString PackageName = DemoRoot / Name;
        const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, Name);
        if (UMassBattleAgentConfigDataAsset* Existing =
            LoadObject<UMassBattleAgentConfigDataAsset>(nullptr, *ObjectPath))
        {
            return Existing;
        }
        UPackage* Package = CreatePackage(*PackageName);
        UMassBattleAgentConfigDataAsset* Created =
            DuplicateObject<UMassBattleAgentConfigDataAsset>(BaseConfig, Package, Name);
        if (Created)
        {
            Created->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
            FAssetRegistryModule::AssetCreated(Created);
        }
        return Created;
    };

    UMassBattleAgentConfigDataAsset* StopConfig = GetOrCreateConfig(TEXT("Tank_StopToFire_AgentConfig"));
    UMassBattleAgentConfigDataAsset* MoveConfig = GetOrCreateConfig(TEXT("Tank_FireWhileMoving_AgentConfig"));
    if (!TestNotNull(TEXT("Stop-to-fire AgentConfig"), StopConfig)
        || !TestNotNull(TEXT("Fire-while-moving AgentConfig"), MoveConfig))
    {
        return false;
    }

    FMBSTMobileFireState InitialState;
    FString ConfigureMessage;
    TestTrue(
        TEXT("Write stop-to-fire contract into AgentConfig"),
        UMBSTSingleTurretEditorLibrary::ConfigureAgentConfigMobileFire(
            StopConfig,
            StopProfile,
            InitialState,
            true,
            ConfigureMessage));
    TestTrue(
        TEXT("Write fire-while-moving contract into AgentConfig"),
        UMBSTSingleTurretEditorLibrary::ConfigureAgentConfigMobileFire(
            MoveConfig,
            MoveProfile,
            InitialState,
            true,
            ConfigureMessage));
    TestTrue(
        TEXT("Stop-to-fire persistent AgentConfig validates"),
        UMBSTSingleTurretEditorLibrary::ValidateAgentConfigMobileFire(
            StopConfig,
            StopProfile).bValid);
    TestTrue(
        TEXT("Fire-while-moving persistent AgentConfig validates"),
        UMBSTSingleTurretEditorLibrary::ValidateAgentConfigMobileFire(
            MoveConfig,
            MoveProfile).bValid);

    bool bSavedAssets = true;
    bSavedAssets &= AssetSubsystem->SaveLoadedAsset(StopProfile, false);
    bSavedAssets &= AssetSubsystem->SaveLoadedAsset(MoveProfile, false);
    bSavedAssets &= AssetSubsystem->SaveLoadedAsset(StopConfig, false);
    bSavedAssets &= AssetSubsystem->SaveLoadedAsset(MoveConfig, false);
    TestTrue(TEXT("Saved mobile-fire profiles and AgentConfigs"), bSavedAssets);

    UWorld* World = UEditorLoadingAndSavingUtils::NewBlankMap(false);
    if (!TestNotNull(TEXT("New mobile-fire demo world"), World))
    {
        return false;
    }
    AWorldSettings* WorldSettings = World->GetWorldSettings();
    if (!TestNotNull(TEXT("Mobile-fire demo world settings"), WorldSettings))
    {
        return false;
    }
    WorldSettings->DefaultGameMode = AMBSTMobileFireDemoGameMode::StaticClass();
    WorldSettings->bForceNoPrecomputedLighting = true;

    AMBSTMobileFireDemoActor* Demo = World->SpawnActor<AMBSTMobileFireDemoActor>(
        FVector::ZeroVector,
        FRotator::ZeroRotator);
    if (!TestNotNull(TEXT("Mobile-fire demo controller"), Demo))
    {
        return false;
    }
    Demo->UnitsPerPolicy = 64;
    Demo->BaseSingleTurretConfig = BaseConfig;
    Demo->StopToFireProfile = StopProfile;
    Demo->FireWhileMovingProfile = MoveProfile;
    Demo->StopToFireAgentConfig = StopConfig;
    Demo->FireWhileMovingAgentConfig = MoveConfig;
    Demo->FunctionalTestSeconds = 8.0f;

    const bool bSavedMap = UEditorLoadingAndSavingUtils::SaveMap(
        World,
        TEXT("/MassBattleSingleTurret/Demo/MobileFire/Map_MBST_MobileFire"));
    TestTrue(TEXT("Saved native mobile-fire demo map"), bSavedMap);
    return bSavedAssets && bSavedMap;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMBSTCreateRtsMobileFireDemoAssetsTest,
    "MassBattle.SingleTurret.MobileFire.CreateRtsDemoAssets",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMBSTCreateRtsMobileFireDemoAssetsTest::RunTest(const FString& Parameters)
{
    static const FString DemoRoot = TEXT("/MassBattleSingleTurret/Demo/RTS");
    static constexpr TCHAR ProjectilePath[] =
        TEXT("/MassBattle/Demo/Projectile/Batched/CannonBall/ProjectileConfig_CannonBall_WarSim.ProjectileConfig_CannonBall_WarSim");

    UEditorAssetSubsystem* AssetSubsystem = GEditor
        ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>()
        : nullptr;
    UMassBattleAgentConfigDataAsset* BaseConfig = LoadObject<UMassBattleAgentConfigDataAsset>(
        nullptr,
        TEXT("/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret_AgentConfig.Tank_SingleTurret_AgentConfig"));
    UMassBattleProjectileConfigDataAsset* ProjectileConfig =
        LoadObject<UMassBattleProjectileConfigDataAsset>(nullptr, ProjectilePath);
    if (!TestNotNull(TEXT("Editor asset subsystem"), AssetSubsystem)
        || !TestNotNull(TEXT("Base single-turret AgentConfig"), BaseConfig)
        || !TestNotNull(TEXT("Existing MassBattle cannonball projectile"), ProjectileConfig))
    {
        return false;
    }

    auto GetOrCreateProfile = [&](const TCHAR* Name) -> UMBSTMobileFireProfile*
    {
        const FString PackageName = DemoRoot / Name;
        const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, Name);
        if (UMBSTMobileFireProfile* Existing =
            LoadObject<UMBSTMobileFireProfile>(nullptr, *ObjectPath))
        {
            return Existing;
        }

        UPackage* Package = CreatePackage(*PackageName);
        UMBSTMobileFireProfile* Created = NewObject<UMBSTMobileFireProfile>(
            Package,
            Name,
            RF_Public | RF_Standalone | RF_Transactional);
        if (Created)
        {
            FAssetRegistryModule::AssetCreated(Created);
        }
        return Created;
    };

    UMBSTMobileFireProfile* StopProfile =
        GetOrCreateProfile(TEXT("DA_MBST_RTS_StopToFire"));
    UMBSTMobileFireProfile* MoveProfile =
        GetOrCreateProfile(TEXT("DA_MBST_RTS_FireWhileMoving"));
    if (!TestNotNull(TEXT("RTS stop-to-fire profile"), StopProfile)
        || !TestNotNull(TEXT("RTS fire-while-moving profile"), MoveProfile))
    {
        return false;
    }

    auto ConfigureCommonProfile = [ProjectileConfig](UMBSTMobileFireProfile* Profile)
    {
        Profile->MinimumRange = 0.0f;
        Profile->TurretYawToleranceDegrees = 6.0f;
        Profile->BarrelPitchToleranceDegrees = 8.0f;
        Profile->TargetPredictionSeconds = 0.0f;
        Profile->WindupSeconds = 0.10f;
        Profile->RecoverSeconds = 0.20f;
        Profile->CooldownSeconds = 2.0f;
        Profile->RecoilNormalizedOnFire = 1.0f;
        Profile->bReturnTurretToZeroWhenIdle = false;
        Profile->bSpawnMassBattleProjectile = true;
        Profile->ProjectileConfig = ProjectileConfig;
        Profile->ProjectileMultipliers = FProjectileMultipliers();
        Profile->bEmitFireRequest = true;
        Profile->bBroadcastBlueprintFireEvent = false;
        Profile->bDisableBuiltInAttack = true;
        Profile->bDisableBuiltInChase = true;
    };

    StopProfile->Modify();
    ConfigureCommonProfile(StopProfile);
    StopProfile->MobilityPolicy = EMBSTFireMobilityPolicy::AimWhileMovingStopToFire;
    StopProfile->MaximumRange = 6000.0f;
    StopProfile->MaxFireLinearSpeed = 25.0f;
    StopProfile->MaxFireAngularSpeed = 5.0f;
    StopProfile->BrakeLeadTimeSeconds = 0.15f;
    StopProfile->bBrakeImmediatelyWhenTargetInRange = true;
    StopProfile->bHoldDuringWindup = true;
    StopProfile->bHoldDuringRecover = true;
    StopProfile->PostEditChange();
    StopProfile->MarkPackageDirty();

    MoveProfile->Modify();
    ConfigureCommonProfile(MoveProfile);
    MoveProfile->MobilityPolicy = EMBSTFireMobilityPolicy::AimAndFireWhileMoving;
    // Keep range equal in the behavior-comparison map so a fast unit cannot
    // cross a short firing window before its turret finishes aligning. Moving
    // fire retains the intended 0.5x damage tradeoff.
    MoveProfile->MaximumRange = 6000.0f;
    MoveProfile->MaxFireLinearSpeed = 1000000.0f;
    MoveProfile->MaxFireAngularSpeed = 1000000.0f;
    MoveProfile->BrakeLeadTimeSeconds = 0.0f;
    MoveProfile->bBrakeImmediatelyWhenTargetInRange = false;
    MoveProfile->bHoldDuringWindup = false;
    MoveProfile->bHoldDuringRecover = false;
    MoveProfile->ProjectileMultipliers.Static.DmgMult = 0.5f;
    MoveProfile->ProjectileMultipliers.Interped.DmgMult = 0.5f;
    MoveProfile->ProjectileMultipliers.Ballistic.DmgMult = 0.5f;
    MoveProfile->ProjectileMultipliers.Tracking.DmgMult = 0.5f;
    MoveProfile->PostEditChange();
    MoveProfile->MarkPackageDirty();

    auto GetOrCreateConfig = [&](const TCHAR* Name) -> UMassBattleAgentConfigDataAsset*
    {
        const FString PackageName = DemoRoot / Name;
        const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, Name);
        if (UMassBattleAgentConfigDataAsset* Existing =
            LoadObject<UMassBattleAgentConfigDataAsset>(nullptr, *ObjectPath))
        {
            return Existing;
        }

        UPackage* Package = CreatePackage(*PackageName);
        UMassBattleAgentConfigDataAsset* Created =
            DuplicateObject<UMassBattleAgentConfigDataAsset>(BaseConfig, Package, Name);
        if (Created)
        {
            Created->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
            FAssetRegistryModule::AssetCreated(Created);
        }
        return Created;
    };

    UMassBattleAgentConfigDataAsset* StopConfig =
        GetOrCreateConfig(TEXT("Tank_RTS_StopToFire_AgentConfig"));
    UMassBattleAgentConfigDataAsset* MoveConfig =
        GetOrCreateConfig(TEXT("Tank_RTS_FireWhileMoving_AgentConfig"));
    if (!TestNotNull(TEXT("RTS stop-to-fire AgentConfig"), StopConfig)
        || !TestNotNull(TEXT("RTS fire-while-moving AgentConfig"), MoveConfig))
    {
        return false;
    }

    auto ConfigureRtsAgent = [&](
        UMassBattleAgentConfigDataAsset* Config,
        UMBSTMobileFireProfile* Profile) -> bool
    {
        Config->Modify();
        Config->Health.Current = 1000000.0f;
        Config->Health.Maximum = 1000000.0f;
        Config->Health.bLockHealth = false;
        Config->Select.bEnable = true;
        Config->Navigation.bMoveByFlowfieldOnIdle = false;
        Config->Navigation.bUseAStar = false;
        Config->Trace.bEnable = true;
        Config->Trace.Mode = ETraceMode::SectorTraceByTraits;
        Config->Trace.SectorTrace.Common.TraceRadius = 7500.0f;
        Config->Trace.SectorTrace.Common.TraceAngle = 360.0f;
        Config->Trace.SectorTrace.Common.TraceHeight = 5000.0f;
        Config->Trace.SectorTrace.Common.SortMode = ESortMode::NearToFar;
        Config->Trace.SectorTrace.Common.CoolDown = 0.5f;
        Config->Trace.SectorTrace.Common.KeepCount = 1;
        Config->Trace.SectorTrace.Common.bCheckObstacle = false;
        Config->Trace.RandomDelayOnInit = FVector2f(0.0f, 0.25f);
        Config->Trace.bSkipTraceWhileTargetValid = true;
        Config->Trace.bCheckLOSWhileTargetValid = false;
        Config->Trace.bRetraceImmediatelyOnTargetLoss = true;
        Config->Trace.Query = FMassBattleQuery();
        Config->Attack.Range = Profile->MaximumRange;
        Config->Attack.RangeToleranceHit = Profile->MaximumRange;

        FMBSTMobileFireState InitialState;
        FString ConfigureMessage;
        const bool bConfigured =
            UMBSTSingleTurretEditorLibrary::ConfigureAgentConfigMobileFire(
                Config,
                Profile,
                InitialState,
                true,
                ConfigureMessage);
        AddInfo(ConfigureMessage);
        Config->PostEditChange();
        Config->MarkPackageDirty();
        return bConfigured;
    };

    const bool bStopConfigured = ConfigureRtsAgent(StopConfig, StopProfile);
    const bool bMoveConfigured = ConfigureRtsAgent(MoveConfig, MoveProfile);
    TestTrue(TEXT("Configured RTS stop-to-fire AgentConfig"), bStopConfigured);
    TestTrue(TEXT("Configured RTS fire-while-moving AgentConfig"), bMoveConfigured);

    // Refresh the optional team-1 map clone when it already exists. It keeps
    // the same renderer/layout as the corrected base unit; only target filters
    // differ between teams.
    UMassBattleAgentConfigDataAsset* MoveTeam1Config =
        LoadObject<UMassBattleAgentConfigDataAsset>(
            nullptr,
            TEXT("/MassBattleSingleTurret/Demo/RTS/Tank_RTS_FireWhileMoving_Team1_AgentConfig.Tank_RTS_FireWhileMoving_Team1_AgentConfig"));
    bool bMoveTeam1Configured = true;
    if (MoveTeam1Config)
    {
        bMoveTeam1Configured = ConfigureRtsAgent(MoveTeam1Config, MoveProfile);
        MoveTeam1Config->Trace.Query = FMassBattleQuery();
        MoveTeam1Config->Trace.Query.All<FAgentTag, FTeam0Tag>();
        MoveTeam1Config->Damage.Query = FMassBattleQuery();
        MoveTeam1Config->Damage.Query.All<FAgentTag, FTeam0Tag>();
        MoveTeam1Config->PostEditChange();
        MoveTeam1Config->MarkPackageDirty();
        TestTrue(TEXT("Refreshed team-1 moving-fire map clone"), bMoveTeam1Configured);
    }

    const FMBSTAssetValidationResult StopValidation =
        UMBSTSingleTurretEditorLibrary::ValidateAgentConfigMobileFire(
            StopConfig,
            StopProfile);
    const FMBSTAssetValidationResult MoveValidation =
        UMBSTSingleTurretEditorLibrary::ValidateAgentConfigMobileFire(
            MoveConfig,
            MoveProfile);
    for (const FString& Message : StopValidation.Messages)
    {
        AddInfo(Message);
    }
    for (const FString& Message : MoveValidation.Messages)
    {
        AddInfo(Message);
    }
    TestTrue(TEXT("RTS stop-to-fire contract validates"), StopValidation.bValid);
    TestTrue(TEXT("RTS fire-while-moving contract validates"), MoveValidation.bValid);
    TestEqual(TEXT("Stop-to-fire range"), StopProfile->MaximumRange, 6000.0f);
    TestEqual(TEXT("Fire-while-moving comparison range"), MoveProfile->MaximumRange, 6000.0f);
    TestEqual(TEXT("Fire-while-moving ballistic damage is 0.5x"),
        MoveProfile->ProjectileMultipliers.Ballistic.DmgMult,
        0.5f);
    TestTrue(TEXT("Stop-to-fire spawns the existing cannonball"),
        StopProfile->bSpawnMassBattleProjectile && StopProfile->ProjectileConfig == ProjectileConfig);
    TestTrue(TEXT("Fire-while-moving spawns the existing cannonball"),
        MoveProfile->bSpawnMassBattleProjectile && MoveProfile->ProjectileConfig == ProjectileConfig);

    bool bSaved = true;
    bSaved &= AssetSubsystem->SaveLoadedAsset(StopProfile, false);
    bSaved &= AssetSubsystem->SaveLoadedAsset(MoveProfile, false);
    bSaved &= AssetSubsystem->SaveLoadedAsset(StopConfig, false);
    bSaved &= AssetSubsystem->SaveLoadedAsset(MoveConfig, false);
    if (MoveTeam1Config)
    {
        bSaved &= AssetSubsystem->SaveLoadedAsset(MoveTeam1Config, false);
    }
    TestTrue(TEXT("Saved dedicated RTS profiles and AgentConfigs"), bSaved);

    return bStopConfigured
        && bMoveConfigured
        && bMoveTeam1Configured
        && StopValidation.bValid
        && MoveValidation.bValid
        && bSaved;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMBSTCreateBenchmarkMapsTest,
    "MassBattle.SingleTurret.Benchmark.CreateDemoMaps",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMBSTCreateBenchmarkMapsTest::RunTest(const FString& Parameters)
{
    struct FMapSpec
    {
        const TCHAR* PackagePath;
        EMBSTBenchmarkScenario Scenario;
    };
    const FMapSpec Specs[] =
    {
        // One neutral map is launched as three independent processes through
        // -MBSTScenario=actor|mass|turret, keeping scene/camera/target identical.
        { TEXT("/MassBattleSingleTurret/Demo/Benchmark/Map_MBST_NativeTrackingBenchmark"), EMBSTBenchmarkScenario::SingleTurret }
    };

    bool bAllSaved = true;
    for (const FMapSpec& Spec : Specs)
    {
        UWorld* World = UEditorLoadingAndSavingUtils::NewBlankMap(false);
        if (!TestNotNull(TEXT("New benchmark world"), World))
        {
            return false;
        }
        AWorldSettings* WorldSettings = World->GetWorldSettings();
        if (!TestNotNull(TEXT("Benchmark world settings"), WorldSettings))
        {
            return false;
        }
        WorldSettings->DefaultGameMode = AMBSTSingleTurretBenchmarkGameMode::StaticClass();
        WorldSettings->bForceNoPrecomputedLighting = true;

        AMBSTSingleTurretBenchmarkActor* Controller = World->SpawnActor<AMBSTSingleTurretBenchmarkActor>(
            FVector::ZeroVector,
            FRotator::ZeroRotator);
        if (!TestNotNull(TEXT("Benchmark controller"), Controller))
        {
            return false;
        }
        Controller->Scenario = Spec.Scenario;
        Controller->UnitCount = 500;
        Controller->WarmupSeconds = 8.0f;
        Controller->SampleSeconds = 15.0f;
        Controller->LegacyActorsPerFrame = 100;
        Controller->TargetOrbitRadius = 10000.0f;
        Controller->TargetOrbitPeriodSeconds = 12.0f;
        Controller->TargetHealth = 1000000000.0f;

        const bool bSaved = UEditorLoadingAndSavingUtils::SaveMap(World, Spec.PackagePath);
        TestTrue(FString::Printf(TEXT("Saved %s"), Spec.PackagePath), bSaved);
        bAllSaved &= bSaved;
    }
    return bAllSaved;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMBSTYawHandednessTest,
    "MassBattle.SingleTurret.Articulation.YawHandedness",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMBSTYawHandednessTest::RunTest(const FString& Parameters)
{
    const UMBSTSingleTurretAsset* Layout = LoadObject<UMBSTSingleTurretAsset>(
        nullptr,
        TEXT("/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret_SingleTurret.Tank_SingleTurret_SingleTurret"));
    if (!TestNotNull(TEXT("Generated tank single-turret layout"), Layout))
    {
        return false;
    }

    const FVector Pivot(Layout->TurretPivotObjectSpace);
    UStaticMesh* ArticulatedMesh = Layout->ArticulatedMesh.Get();
    if (!TestNotNull(TEXT("Generated articulated mesh"), ArticulatedMesh))
    {
        return false;
    }
    FMeshDescription* MeshDescription = ArticulatedMesh->GetMeshDescription(0);
    if (!TestNotNull(TEXT("Generated articulated mesh LOD0 description"), MeshDescription))
    {
        return false;
    }

    FStaticMeshAttributes MeshAttributes(*MeshDescription);
    const TVertexAttributesConstRef<FVector3f> Positions = MeshAttributes.GetVertexPositions();
    const TVertexInstanceAttributesConstRef<FVector4f> Colors = MeshAttributes.GetVertexInstanceColors();
    FVector FarthestYawVertexDirection = FVector::ZeroVector;
    float FarthestYawVertexDistanceSquared = 0.0f;
    for (const FVertexInstanceID VertexInstanceID : MeshDescription->VertexInstances().GetElementIDs())
    {
        if (!Colors.IsValid() || Colors[VertexInstanceID].X <= 0.5f)
        {
            continue;
        }
        const FVertexID VertexID = MeshDescription->GetVertexInstanceVertex(VertexInstanceID);
        FVector Direction = FVector(Positions[VertexID]) - Pivot;
        Direction.Z = 0.0;
        const float DistanceSquared = Direction.SizeSquared();
        if (DistanceSquared > FarthestYawVertexDistanceSquared)
        {
            FarthestYawVertexDistanceSquared = DistanceSquared;
            FarthestYawVertexDirection = Direction.GetSafeNormal();
        }
    }
    AddInfo(FString::Printf(
        TEXT("Generated yaw-mask farthest horizontal direction=(%.3f, %.3f, %.3f), declared barrel forward=(%.3f, %.3f, %.3f)."),
        FarthestYawVertexDirection.X,
        FarthestYawVertexDirection.Y,
        FarthestYawVertexDirection.Z,
        Layout->BarrelForwardAxisObjectSpace.X,
        Layout->BarrelForwardAxisObjectSpace.Y,
        Layout->BarrelForwardAxisObjectSpace.Z));

    struct FYawCase
    {
        float YawDegrees;
        FVector ExpectedForward;
        const TCHAR* Label;
    };
    const FYawCase Cases[] =
    {
        { 0.0f, FVector::ForwardVector, TEXT("0 degrees maps +X to +X") },
        { 90.0f, FVector::YAxisVector, TEXT("+90 degrees maps +X to +Y") },
        { -90.0f, -FVector::YAxisVector, TEXT("-90 degrees maps +X to -Y") },
        { 180.0f, -FVector::ForwardVector, TEXT("180 degrees maps +X to -X") }
    };

    for (const FYawCase& Case : Cases)
    {
        const FTransform Muzzle = Layout->CalculateMuzzleWorldTransform(
            FTransform::Identity,
            Case.YawDegrees,
            0.0f,
            0.0f);
        const FVector ActualForward = FVector(
            Muzzle.GetLocation().X - Pivot.X,
            Muzzle.GetLocation().Y - Pivot.Y,
            0.0).GetSafeNormal();
        TestTrue(
            Case.Label,
            FVector::DotProduct(ActualForward, Case.ExpectedForward) > 0.999f);
    }

    FString ShaderText;
    const FString ShaderPath = FPaths::Combine(
        FPaths::ProjectPluginsDir(),
        TEXT("MassBattleSingleTurret/Shaders/Private/MBSTSingleTurret.ush"));
    if (!TestTrue(TEXT("Yaw articulation shader can be read"), FFileHelper::LoadFileToString(ShaderText, *ShaderPath)))
    {
        return false;
    }

    const auto CountOccurrences = [](const FString& Haystack, const TCHAR* Needle)
    {
        int32 Count = 0;
        int32 SearchFrom = 0;
        const int32 NeedleLength = FCString::Strlen(Needle);
        while (NeedleLength > 0)
        {
            const int32 FoundAt = Haystack.Find(
                Needle,
                ESearchCase::CaseSensitive,
                ESearchDir::FromStart,
                SearchFrom);
            if (FoundAt == INDEX_NONE)
            {
                break;
            }
            ++Count;
            SearchFrom = FoundAt + NeedleLength;
        }
        return Count;
    };

    const int32 NegatedYawCount = CountOccurrences(ShaderText, TEXT("-YawPitchSinCos.x"));
    const int32 DirectYawCount = CountOccurrences(ShaderText, TEXT("YawPitchSinCos.x,"));

    TestEqual(
        TEXT("GPU yaw must not negate the CPU/FQuat yaw sine"),
        NegatedYawCount,
        0);
    TestTrue(
        TEXT("Position and normal articulation both consume the direct yaw sine"),
        DirectYawCount >= 2);

    AddInfo(TEXT("Yaw contract: UE +X forward, +Y right, +Z up; +90 degrees about +Z maps +X to +Y on both CPU and GPU."));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMBSTGenerateDemoTankTest,
    "MassBattle.SingleTurret.Authoring.GenerateDemoTank",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMBSTGenerateDemoTankTest::RunTest(const FString& Parameters)
{
    static const TCHAR* OutputRoot = TEXT("/MassBattleSingleTurret/Demo/Tank");
    static const TCHAR* OutputName = TEXT("Tank_SingleTurret");
    static const TCHAR* LayoutPath = TEXT("/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret_SingleTurret.Tank_SingleTurret_SingleTurret");
    static const TCHAR* ConfigPath = TEXT("/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret_AgentConfig.Tank_SingleTurret_AgentConfig");

    // Idempotent verification path for an already-generated demo.
    if (UMassBattleAgentConfigDataAsset* ExistingConfig = LoadObject<UMassBattleAgentConfigDataAsset>(nullptr, ConfigPath))
    {
        UMBSTSingleTurretAsset* ExistingLayout = LoadObject<UMBSTSingleTurretAsset>(nullptr, LayoutPath);
        const FMBSTAssetValidationResult ConfigValidation =
            UMBSTSingleTurretEditorLibrary::ValidateAgentConfigSingleTurret(ExistingConfig, ExistingLayout);
        const FMBSTAssetValidationResult MeshValidation =
            UMBSTSingleTurretEditorLibrary::ValidateGeneratedArticulatedMesh(
                ExistingLayout ? ExistingLayout->ArticulatedMesh : nullptr,
                ExistingLayout ? ExistingLayout->bBodyUsesVAT : false);
        for (const FString& Message : ConfigValidation.Messages)
        {
            AddInfo(Message);
        }
        for (const FString& Message : MeshValidation.Messages)
        {
            AddInfo(Message);
        }
        if (ConfigValidation.bValid && MeshValidation.bValid)
        {
            TestTrue(TEXT("Existing demo AgentConfig contains the generated turret contract"), ConfigValidation.bValid);
            TestTrue(TEXT("Existing demo mesh preserves body/turret articulation masks"), MeshValidation.bValid);
            return true;
        }
        AddWarning(TEXT("The existing generated demo is invalid and will be rebuilt."));

        UEditorAssetSubsystem* CleanupSubsystem = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>();
        if (!TestNotNull(TEXT("Editor asset subsystem for invalid demo cleanup"), CleanupSubsystem))
        {
            return false;
        }

        TArray<UObject*> InvalidGeneratedAssets;
        InvalidGeneratedAssets.Add(ExistingConfig);
        if (ExistingLayout)
        {
            if (ExistingLayout->ArticulatedMesh)
            {
                InvalidGeneratedAssets.Add(ExistingLayout->ArticulatedMesh);
            }
            InvalidGeneratedAssets.Add(ExistingLayout);
        }
        if (!TestTrue(
            TEXT("Deleted invalid generated demo assets before regeneration"),
            CleanupSubsystem->DeleteLoadedAssets(InvalidGeneratedAssets)))
        {
            return false;
        }
        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    }

    UBlueprint* TankBlueprint = LoadObject<UBlueprint>(
        nullptr,
        TEXT("/MassBattle/Test/CompoundUnitAsset/BP_TankActor.BP_TankActor"));
    UMassBattleAgentConfigDataAsset* TemplateConfig = LoadObject<UMassBattleAgentConfigDataAsset>(
        nullptr,
        TEXT("/MassBattle/Demo/Agent/Tank/AgentConfig_Tank_WarSim.AgentConfig_Tank_WarSim"));
    UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;

    if (!TestNotNull(TEXT("Compound demo tank Blueprint"), TankBlueprint)
        || !TestNotNull(TEXT("Demo tank AgentConfig template"), TemplateConfig)
        || !TestNotNull(TEXT("Editor world"), EditorWorld)
        || !TestNotNull(TEXT("Generated tank class"), TankBlueprint ? TankBlueprint->GeneratedClass.Get() : nullptr))
    {
        return false;
    }

    FActorSpawnParameters SpawnParameters;
    SpawnParameters.Name = TEXT("MBST_DemoTankConversionSource");
    SpawnParameters.ObjectFlags = RF_Transient | RF_Transactional;
    SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AActor* SourceActor = EditorWorld->SpawnActor<AActor>(
        TankBlueprint->GeneratedClass,
        FTransform::Identity,
        SpawnParameters);
    if (!TestNotNull(TEXT("Transient source tank actor"), SourceActor))
    {
        return false;
    }
    ON_SCOPE_EXIT
    {
        if (IsValid(SourceActor))
        {
            EditorWorld->DestroyActor(SourceActor);
        }
    };

    USceneComponent* TurretPivot = nullptr;
    USceneComponent* Muzzle = nullptr;
    USceneComponent* TurretVisual = nullptr;
    TInlineComponentArray<USceneComponent*> Components(SourceActor);
    for (USceneComponent* Component : Components)
    {
        if (!Component)
        {
            continue;
        }
        if (Component->GetFName() == TEXT("Turret"))
        {
            TurretPivot = Component;
        }
        else if (Component->GetFName() == TEXT("Cannon"))
        {
            Muzzle = Component;
        }
        else if (Component->GetFName() == TEXT("PreviewMeshTurret"))
        {
            TurretVisual = Component;
        }
    }
    if (!TestNotNull(TEXT("Tank turret pivot component"), TurretPivot))
    {
        return false;
    }
    if (!TestNotNull(TEXT("Tank turret preview mesh"), TurretVisual))
    {
        return false;
    }

    // The legacy compound demo keeps preview meshes outside its four-Agent
    // logical hierarchy. Adapt only this transient conversion instance so the
    // selected marker owns the visual subtree; the source Blueprint is untouched.
    TurretVisual->AttachToComponent(TurretPivot, FAttachmentTransformRules::KeepWorldTransform);

    UMBSTSingleTurretAuthoringComponent* Authoring = NewObject<UMBSTSingleTurretAuthoringComponent>(
        SourceActor,
        TEXT("MBSTSingleTurretAuthoring_Demo"),
        RF_Transient | RF_Transactional);
    SourceActor->AddInstanceComponent(Authoring);
    Authoring->RegisterComponent();
    Authoring->TurretYawPivot.OverrideComponent = TurretPivot;
    Authoring->TurretYawPivot.ComponentProperty = TurretPivot->GetFName();
    Authoring->Muzzle.OverrideComponent = Muzzle;
    Authoring->Muzzle.ComponentProperty = Muzzle ? Muzzle->GetFName() : NAME_None;
    TurretPivot->ComponentTags.AddUnique(Authoring->TurretYawTag);
    if (Muzzle)
    {
        Muzzle->ComponentTags.AddUnique(Authoring->MuzzleTag);
    }
    Authoring->bBodyUsesVAT = false;
    Authoring->YawLimitsDegrees = FVector2D(-180.0, 180.0);
    Authoring->MaximumRecoilDistance = 8.0f;
    if (!TestEqual(
        TEXT("Authoring component resolves the explicitly selected turret pivot"),
        Authoring->ResolveTurretYawPivot(),
        TurretPivot))
    {
        return false;
    }

    FMBSTActorToSingleTurretSettings Settings;
    Settings.bIncludeHiddenComponents = true;
    Settings.bCreateVATDataAsset = false;
    Settings.AgentConfigTemplate = TemplateConfig;

    const FMBSTAssetValidationResult SourceValidation =
        UMBSTSingleTurretEditorLibrary::ValidateActorForSingleTurretConversion(SourceActor, Authoring, Settings);
    for (const FString& Message : SourceValidation.Messages)
    {
        AddInfo(Message);
    }
    if (!TestTrue(TEXT("Demo tank hierarchy can be classified as body + turret"), SourceValidation.bValid))
    {
        return false;
    }

    const FMBSTActorToSingleTurretResult Result =
        UMBSTSingleTurretEditorLibrary::ConvertActorToSingleTurretVAT(
            SourceActor,
            Authoring,
            OutputRoot,
            OutputName,
            Settings);
    for (const FString& Message : Result.Messages)
    {
        AddInfo(Message);
    }

    TestTrue(TEXT("Actor-to-single-turret conversion succeeded"), Result.bSucceeded);
    TestNotNull(TEXT("Generated articulated tank mesh"), Result.ArticulatedMesh.Get());
    TestNotNull(TEXT("Generated shared turret layout"), Result.LayoutAsset.Get());
    TestNotNull(TEXT("Generated direct turret AgentConfig"), Result.AgentConfig.Get());
    if (!Result.bSucceeded)
    {
        return false;
    }

    const FMBSTAssetValidationResult ConfigValidation =
        UMBSTSingleTurretEditorLibrary::ValidateAgentConfigSingleTurret(Result.AgentConfig, Result.LayoutAsset);
    for (const FString& Message : ConfigValidation.Messages)
    {
        AddInfo(Message);
    }
    TestTrue(TEXT("Generated config embeds exactly one turret Tag/State/Shared layout"), ConfigValidation.bValid);

    const FMBSTAssetValidationResult MeshValidation =
        UMBSTSingleTurretEditorLibrary::ValidateGeneratedArticulatedMesh(
            Result.ArticulatedMesh,
            Authoring->bBodyUsesVAT);
    for (const FString& Message : MeshValidation.Messages)
    {
        AddInfo(Message);
    }
    TestTrue(TEXT("Generated mesh preserves body/turret articulation masks"), MeshValidation.bValid);

    UEditorAssetSubsystem* AssetSubsystem = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>();
    if (!TestNotNull(TEXT("Editor asset subsystem"), AssetSubsystem))
    {
        return false;
    }
    const bool bSavedMesh = AssetSubsystem->SaveLoadedAsset(Result.ArticulatedMesh, false);
    const bool bSavedLayout = AssetSubsystem->SaveLoadedAsset(Result.LayoutAsset, false);
    const bool bSavedConfig = AssetSubsystem->SaveLoadedAsset(Result.AgentConfig, false);
    TestTrue(TEXT("Saved generated demo assets"), bSavedMesh && bSavedLayout && bSavedConfig);
    return ConfigValidation.bValid && MeshValidation.bValid && bSavedMesh && bSavedLayout && bSavedConfig;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMBSTConfigureDemoNiagaraTest,
    "MassBattle.SingleTurret.Authoring.ConfigureDemoNiagaraStyleArray",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMBSTConfigureDemoNiagaraTest::RunTest(const FString& Parameters)
{
    UNiagaraSystem* NiagaraSystem = LoadObject<UNiagaraSystem>(
        nullptr,
        TEXT("/MassBattleSingleTurret/Demo/Tank/NS_Tank_SingleTurret.NS_Tank_SingleTurret"));
    if (!TestNotNull(TEXT("Single-turret demo Niagara system"), NiagaraSystem))
    {
        return false;
    }

    FString Message;
    const bool bConfigured = UMBSTSingleTurretEditorLibrary::EnsureNiagaraStyleArray(NiagaraSystem, Message);
    AddInfo(Message);
    if (!TestTrue(TEXT("Created the independent User.StyleArray data interface"), bConfigured))
    {
        return false;
    }

    const FMBSTAssetValidationResult Validation =
        UMBSTSingleTurretEditorLibrary::ValidateNiagaraStyleArray(NiagaraSystem);
    for (const FString& ValidationMessage : Validation.Messages)
    {
        AddInfo(ValidationMessage);
    }
    TestTrue(TEXT("User.StyleArray has the exact Niagara Array Int32 contract"), Validation.bValid);

    UEditorAssetSubsystem* AssetSubsystem = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>();
    const bool bSaved = TestNotNull(TEXT("Editor asset subsystem"), AssetSubsystem)
        && AssetSubsystem->SaveLoadedAsset(NiagaraSystem, false);
    TestTrue(TEXT("Saved configured demo Niagara system"), bSaved);
    return Validation.bValid && bSaved;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMBSTConfigureDemoPrecomputedArticulationTest,
    "MassBattle.SingleTurret.Authoring.ConfigureDemoPrecomputedArticulation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMBSTConfigureDemoPrecomputedArticulationTest::RunTest(const FString& Parameters)
{
    UNiagaraSystem* NiagaraSystem = LoadObject<UNiagaraSystem>(
        nullptr,
        TEXT("/MassBattleSingleTurret/Demo/Tank/NS_Tank_SingleTurret.NS_Tank_SingleTurret"));
    UMaterial* Material = LoadObject<UMaterial>(
        nullptr,
        TEXT("/MassBattleSingleTurret/Materials/M_MBST_VATSingleTurret.M_MBST_VATSingleTurret"));
    if (!TestNotNull(TEXT("Single-turret demo Niagara system"), NiagaraSystem)
        || !TestNotNull(TEXT("Single-turret base material"), Material))
    {
        return false;
    }

    FString NiagaraMessage;
    const bool bNiagaraConfigured =
        UMBSTSingleTurretEditorLibrary::ConfigureNiagaraPrecomputedArticulation(
            NiagaraSystem,
            NiagaraMessage);
    AddInfo(NiagaraMessage);
    TestTrue(TEXT("Moved packed decode and trigonometry to the GPU particle stage"), bNiagaraConfigured);

    FString MaterialMessage;
    const bool bMaterialConfigured =
        UMBSTSingleTurretEditorLibrary::ConfigurePrecomputedArticulationMaterial(
            Material,
            MaterialMessage);
    AddInfo(MaterialMessage);
    TestTrue(TEXT("Removed packed decode and trigonometry from the material vertex path"), bMaterialConfigured);

    FString NormalSwitchMessage;
    const bool bNormalSwitchConfigured =
        UMBSTSingleTurretEditorLibrary::ConfigurePerformanceNormalSwitch(
            Material,
            NormalSwitchMessage);
    AddInfo(NormalSwitchMessage);
    TestTrue(TEXT("Default material permutation removes articulated pixel-normal rotation"), bNormalSwitchConfigured);

    UEditorAssetSubsystem* AssetSubsystem = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>();
    const bool bSaved = TestNotNull(TEXT("Editor asset subsystem"), AssetSubsystem)
        && AssetSubsystem->SaveLoadedAsset(NiagaraSystem, false)
        && AssetSubsystem->SaveLoadedAsset(Material, false);
    TestTrue(TEXT("Saved precomputed articulation assets"), bSaved);
    return bNiagaraConfigured && bMaterialConfigured && bNormalSwitchConfigured && bSaved;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMBSTDemoStructurePerformanceTest,
    "MassBattle.SingleTurret.Performance.DemoTankStructure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMBSTDemoStructurePerformanceTest::RunTest(const FString& Parameters)
{
    UStaticMesh* SingleTurretMesh = LoadObject<UStaticMesh>(
        nullptr,
        TEXT("/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret.Tank_SingleTurret"));
    UMassBattleAgentConfigDataAsset* SingleTurretConfig = LoadObject<UMassBattleAgentConfigDataAsset>(
        nullptr,
        TEXT("/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret_AgentConfig.Tank_SingleTurret_AgentConfig"));
    UNiagaraSystem* NiagaraSystem = LoadObject<UNiagaraSystem>(
        nullptr,
        TEXT("/MassBattleSingleTurret/Demo/Tank/NS_Tank_SingleTurret.NS_Tank_SingleTurret"));

    if (!TestNotNull(TEXT("Generated single-turret mesh"), SingleTurretMesh)
        || !TestNotNull(TEXT("Generated single-turret AgentConfig"), SingleTurretConfig)
        || !TestNotNull(TEXT("Configured single-turret Niagara system"), NiagaraSystem))
    {
        return false;
    }

    const FMBSTAssetValidationResult ContractValidation =
        UMBSTSingleTurretEditorLibrary::ValidateAgentConfigSingleTurret(SingleTurretConfig, nullptr);
    int32 AgentMeshLODSlotCount = 0;
    bool bAgentMeshLODSlotsPreserveABI = true;
    ForEachObjectWithPackage(NiagaraSystem->GetOutermost(), [&](UObject* Object)
    {
        const UNiagaraMeshRendererProperties* MeshRenderer =
            Cast<UNiagaraMeshRendererProperties>(Object);
        if (!MeshRenderer)
        {
            return true;
        }
        for (int32 MeshIndex = 0; MeshIndex < MeshRenderer->Meshes.Num(); ++MeshIndex)
        {
            const FNiagaraMeshRendererMeshProperties& MeshProperties =
                MeshRenderer->Meshes[MeshIndex];
            if (MeshProperties.MeshParameterBinding.ResolvedParameter.GetName()
                != FName(TEXT("User.AgentMesh")))
            {
                continue;
            }
            ++AgentMeshLODSlotCount;
            bAgentMeshLODSlotsPreserveABI = bAgentMeshLODSlotsPreserveABI
                && MeshProperties.LODMode == ENiagaraMeshLODMode::LODLevel
                && MeshProperties.LODLevel == MeshIndex
                && MeshProperties.LODLevelBinding.GetDefaultValue<int32>() == MeshIndex;
        }
        return true;
    });
    TestTrue(TEXT("Single-turret config carries the direct Mass entity contract"), ContractValidation.bValid);
    TestEqual(TEXT("Generated tank merged mesh LOD count"), SingleTurretMesh->GetNumLODs(), 3);
    TestEqual(TEXT("MassBattle User.AgentMesh LOD slot count"), AgentMeshLODSlotCount, 5);
    TestTrue(TEXT("MassBattle User.AgentMesh slots preserve LOD 0..N ABI"), bAgentMeshLODSlotsPreserveABI);

    AddInfo(FString::Printf(
        TEXT("Plugin tank validates as one direct Mass entity; authored mesh MinLOD remains %d."),
        SingleTurretMesh->GetMinLODIdx()));
    AddInfo(TEXT("The external MassBattle compound demo is intentionally not loaded by this plugin test."));
    return ContractValidation.bValid
        && SingleTurretMesh->GetNumLODs() == 3
        && AgentMeshLODSlotCount == 5
        && bAgentMeshLODSlotsPreserveABI;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMBSTPackingPerformanceTest,
    "MassBattle.SingleTurret.Performance.PackingMicrobenchmark",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMBSTPackingPerformanceTest::RunTest(const FString& Parameters)
{
    constexpr int32 IterationCount = 4 * 1024 * 1024;
    constexpr int32 SampleCount = 5;

    TArray<double, TInlineAllocator<SampleCount>> NanosecondsPerEntitySamples;
    NanosecondsPerEntitySamples.Reserve(SampleCount);
    int32 Checksum = 0;

    for (int32 SampleIndex = -1; SampleIndex < SampleCount; ++SampleIndex)
    {
        const double StartSeconds = FPlatformTime::Seconds();
        int32 SampleChecksum = 0;
        for (int32 EntityIndex = 0; EntityIndex < IterationCount; ++EntityIndex)
        {
            const float Yaw = static_cast<float>((EntityIndex % 4096) * (360.0 / 4095.0) - 180.0);
            const float Pitch = static_cast<float>((EntityIndex % 256) * (180.0 / 255.0) - 90.0);
            const float Recoil = static_cast<float>(EntityIndex & 15) / 15.0f;
            SampleChecksum ^= MBSTPacking::Pack(EntityIndex & 255, Yaw, Pitch, Recoil);
        }
        const double ElapsedSeconds = FPlatformTime::Seconds() - StartSeconds;
        Checksum ^= SampleChecksum;
        if (SampleIndex >= 0)
        {
            NanosecondsPerEntitySamples.Add(
                ElapsedSeconds * 1.0e9 / static_cast<double>(IterationCount));
        }
    }

    NanosecondsPerEntitySamples.Sort();
    const double MedianNanosecondsPerEntity = NanosecondsPerEntitySamples[SampleCount / 2];
    const double MillionEntitiesMilliseconds = MedianNanosecondsPerEntity * 1.0e6 / 1.0e6;

    AddInfo(FString::Printf(
        TEXT("Packing median: %.3f ns/entity (%.3f ms per 1,000,000 entities), %d measured samples plus one warm-up."),
        MedianNanosecondsPerEntity,
        MillionEntitiesMilliseconds,
        SampleCount));
    AddInfo(FString::Printf(
        TEXT("Memory contract: FMBSTSingleTurretState=%llu bytes/entity; packed render payload=%llu bytes/entity; shared layout=%llu bytes/archetype value."),
        static_cast<uint64>(sizeof(FMBSTSingleTurretState)),
        static_cast<uint64>(sizeof(int32)),
        static_cast<uint64>(sizeof(FMBSTSingleTurretShared))));
    AddInfo(FString::Printf(TEXT("Benchmark checksum: %d"), Checksum));
    AddInfo(TEXT("This isolates CPU packing arithmetic; it does not measure Mass iteration, Niagara upload, vertex cost or frame rate."));

    TestTrue(TEXT("Packing benchmark produced a finite positive duration"),
        FMath::IsFinite(MedianNanosecondsPerEntity) && MedianNanosecondsPerEntity > 0.0);
    return !HasAnyErrors();
}

#endif
