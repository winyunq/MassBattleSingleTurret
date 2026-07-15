#include "MBSTSingleTurretEditorLibrary.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "MBSTSingleTurretAsset.h"
#include "DataAssets/MassBattleAgentConfigDataAsset.h"
#include "Components/ActorComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "NiagaraSystem.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "Editor.h"

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
        const UMBSTSingleTurretAsset* ExistingLayout = LoadObject<UMBSTSingleTurretAsset>(nullptr, LayoutPath);
        const FMBSTAssetValidationResult Validation =
            UMBSTSingleTurretEditorLibrary::ValidateAgentConfigSingleTurret(ExistingConfig, ExistingLayout);
        for (const FString& Message : Validation.Messages)
        {
            AddInfo(Message);
        }
        TestTrue(TEXT("Existing demo AgentConfig contains the generated turret contract"), Validation.bValid);
        return Validation.bValid;
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

    UEditorAssetSubsystem* AssetSubsystem = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>();
    if (!TestNotNull(TEXT("Editor asset subsystem"), AssetSubsystem))
    {
        return false;
    }
    const bool bSavedMesh = AssetSubsystem->SaveLoadedAsset(Result.ArticulatedMesh, false);
    const bool bSavedLayout = AssetSubsystem->SaveLoadedAsset(Result.LayoutAsset, false);
    const bool bSavedConfig = AssetSubsystem->SaveLoadedAsset(Result.AgentConfig, false);
    TestTrue(TEXT("Saved generated demo assets"), bSavedMesh && bSavedLayout && bSavedConfig);
    return ConfigValidation.bValid && bSavedMesh && bSavedLayout && bSavedConfig;
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
    FMBSTDemoStructurePerformanceTest,
    "MassBattle.SingleTurret.Performance.DemoTankStructure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMBSTDemoStructurePerformanceTest::RunTest(const FString& Parameters)
{
    UBlueprint* LegacyTankBlueprint = LoadObject<UBlueprint>(
        nullptr,
        TEXT("/MassBattle/Test/CompoundUnitAsset/BP_TankActor.BP_TankActor"));
    UStaticMesh* SingleTurretMesh = LoadObject<UStaticMesh>(
        nullptr,
        TEXT("/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret.Tank_SingleTurret"));
    UMassBattleAgentConfigDataAsset* SingleTurretConfig = LoadObject<UMassBattleAgentConfigDataAsset>(
        nullptr,
        TEXT("/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret_AgentConfig.Tank_SingleTurret_AgentConfig"));
    UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;

    if (!TestNotNull(TEXT("Legacy compound tank Blueprint"), LegacyTankBlueprint)
        || !TestNotNull(TEXT("Generated single-turret mesh"), SingleTurretMesh)
        || !TestNotNull(TEXT("Generated single-turret AgentConfig"), SingleTurretConfig)
        || !TestNotNull(TEXT("Editor world"), EditorWorld)
        || !TestNotNull(TEXT("Legacy compound tank generated class"),
            LegacyTankBlueprint ? LegacyTankBlueprint->GeneratedClass.Get() : nullptr))
    {
        return false;
    }

    FActorSpawnParameters SpawnParameters;
    SpawnParameters.Name = TEXT("MBST_DemoTankStructureSource");
    SpawnParameters.ObjectFlags = RF_Transient | RF_Transactional;
    SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AActor* LegacyTankInstance = EditorWorld->SpawnActor<AActor>(
        LegacyTankBlueprint->GeneratedClass,
        FTransform::Identity,
        SpawnParameters);
    if (!TestNotNull(TEXT("Transient legacy compound tank instance"), LegacyTankInstance))
    {
        return false;
    }
    ON_SCOPE_EXIT
    {
        if (IsValid(LegacyTankInstance))
        {
            EditorWorld->DestroyActor(LegacyTankInstance);
        }
    };

    TInlineComponentArray<UActorComponent*> LegacyComponents(LegacyTankInstance);
    int32 LegacyAgentComponentCount = 0;
    for (const UActorComponent* Component : LegacyComponents)
    {
        if (Component && Component->GetClass()->GetName() == TEXT("MassBattleAgentComponent"))
        {
            ++LegacyAgentComponentCount;
        }
    }

    const FMBSTAssetValidationResult ContractValidation =
        UMBSTSingleTurretEditorLibrary::ValidateAgentConfigSingleTurret(SingleTurretConfig, nullptr);
    TestTrue(TEXT("Single-turret config carries the direct Mass entity contract"), ContractValidation.bValid);
    TestEqual(TEXT("Legacy demo tank MassBattleAgentComponent count"), LegacyAgentComponentCount, 4);
    TestEqual(TEXT("Generated tank merged mesh LOD count"), SingleTurretMesh->GetNumLODs(), 3);

    AddInfo(FString::Printf(
        TEXT("Structural baseline: legacy BP_TankActor=%d Mass agent components; plugin tank=1 Mass entity. Entity-count reduction=%.0f%%."),
        LegacyAgentComponentCount,
        LegacyAgentComponentCount > 0
            ? (1.0 - 1.0 / static_cast<double>(LegacyAgentComponentCount)) * 100.0
            : 0.0));
    AddInfo(TEXT("This is a structural comparison, not an FPS/GPU timing claim."));
    return ContractValidation.bValid && LegacyAgentComponentCount == 4 && SingleTurretMesh->GetNumLODs() == 3;
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
