#include "MBSTSingleTurretBenchmark.h"

#include "MBSTSingleTurretTypes.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/MassBattleAgentComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DataAssets/MassBattleAgentConfigDataAsset.h"
#include "DrawDebugHelpers.h"
#include "Engine/Canvas.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Fragments/Attack.h"
#include "Fragments/Debug.h"
#include "Fragments/Move.h"
#include "Fragments/Render.h"
#include "Fragments/Team.h"
#include "Fragments/Trace.h"
#include "Fragments/Transform.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpectatorPawn.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "MassAPIStructs.h"
#include "MassAPISubsystem.h"
#include "MassExecutionContext.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Renderers/MassBattleAgentRenderer.h"
#include "Subsystems/MassBattleAgentSubsystem.h"
#include "Subsystems/MassBattleSubsystem.h"
#include "UnrealClient.h"

namespace MBSTBenchmark
{
    static constexpr float SweepAmplitudeDegrees = 75.0f;
    static constexpr float SweepPeriodSeconds = 8.0f;
    static constexpr float TwoPi = 2.0f * PI;
}

UMBSTSingleTurretBenchmarkDriveProcessor::UMBSTSingleTurretBenchmarkDriveProcessor()
    : LegacyQuery(*this)
    , SingleTurretQuery(*this)
{
    ExecutionOrder.ExecuteBefore.Add(TEXT("MBSTSingleTurretPackProcessor"));
    ExecutionOrder.ExecuteBefore.Add(TEXT("MassBattleAgentRenderProcessor"));
    ExecutionFlags = static_cast<int32>(EProcessorExecutionFlags::Client | EProcessorExecutionFlags::Server | EProcessorExecutionFlags::Standalone);
    ProcessingPhase = EMassProcessingPhase::FrameEnd;
    bAutoRegisterWithProcessingPhases = true;
    bRequiresGameThreadExecution = false;
    ExecutionPriority = 6;
}

void UMBSTSingleTurretBenchmarkDriveProcessor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
    FEntityQueryBuilder(LegacyQuery)
        .All<FMBSTBenchmarkLegacyTurretTag>()
        .All<FRotating>(MARW)
        .All<FTeam>(MARO)
        .RegisterWithProcessor(*this);

    FEntityQueryBuilder(SingleTurretQuery)
        .All<FMBSTBenchmarkSingleTurretTag>()
        .All<FMBSTSingleTurretState>(MARW)
        .RegisterWithProcessor(*this);
}

void UMBSTSingleTurretBenchmarkDriveProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
    if (!Context.GetWorld())
    {
        return;
    }

    const float SweepYaw = MBSTBenchmark::SweepAmplitudeDegrees * FMath::Sin(
        MBSTBenchmark::TwoPi * static_cast<float>(
            FMath::Fmod(FPlatformTime::Seconds(), static_cast<double>(MBSTBenchmark::SweepPeriodSeconds))
            / static_cast<double>(MBSTBenchmark::SweepPeriodSeconds)));

    LegacyQuery.ForEachEntityChunk(Context, [SweepYaw](FMassExecutionContext& ChunkContext)
    {
        TArrayView<FRotating> Rotations = ChunkContext.GetMutableFragmentView<FRotating>();
        const TConstArrayView<FTeam> Teams = ChunkContext.GetFragmentView<FTeam>();
        for (int32 EntityIndex = 0; EntityIndex < ChunkContext.GetNumEntities(); ++EntityIndex)
        {
            const float BodyYaw = Teams[EntityIndex].index == 2 ? 180.0f : 0.0f;
            const FRotator3f ForcedRotation(0.0f, BodyYaw + SweepYaw, 0.0f);
            FRotating& Rotating = Rotations[EntityIndex];
            Rotating.Rotation = ForcedRotation;
            Rotating.RotationQuat = ForcedRotation.Quaternion();
            Rotating.DesiredRotationQuat = Rotating.RotationQuat;
            Rotating.Direction = ForcedRotation.Vector();
        }
    });

    SingleTurretQuery.ForEachEntityChunk(Context, [SweepYaw](FMassExecutionContext& ChunkContext)
    {
        TArrayView<FMBSTSingleTurretState> States = ChunkContext.GetMutableFragmentView<FMBSTSingleTurretState>();
        for (FMBSTSingleTurretState& State : States)
        {
            State.TargetYawDegrees = SweepYaw;
            State.TargetPitchDegrees = 0.0f;
            State.bInterpolate = true;
            State.bArticulationEnabled = true;
        }
    });
}

AMBSTSingleTurretBenchmarkActor::AMBSTSingleTurretBenchmarkActor()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = true;
    PrimaryActorTick.TickGroup = TG_PostUpdateWork;
}

void AMBSTSingleTurretBenchmarkActor::BeginPlay()
{
    Super::BeginPlay();
    LastFrameWallClockSeconds = FPlatformTime::Seconds();

    ApplyCommandLineOverrides();
    TanksPerSide = FMath::Max(TanksPerSide, 1);
    WarmupSeconds = FMath::Max(WarmupSeconds, 0.0f);
    SampleSeconds = FMath::Max(SampleSeconds, 1.0f);
    LegacyActorsPerFrame = FMath::Max(LegacyActorsPerFrame, 1);

    FormationDepth = FMath::Max(1, FMath::CeilToInt(FMath::Sqrt(static_cast<float>(TanksPerSide) * 0.5f)));
    FormationWidth = FMath::Max(1, FMath::CeilToInt(static_cast<float>(TanksPerSide) / static_cast<float>(FormationDepth)));
    const float RegionDepth = static_cast<float>(FormationDepth - 1) * FormationSpacing;
    ArmyCenterX = RegionDepth * 0.5f + 3000.0f;

    CreateBenchmarkEnvironment();
    if (!LoadScenarioAssets())
    {
        Phase = EMBSTBenchmarkPhase::Failed;
        UE_LOG(LogTemp, Error, TEXT("MBST_BENCHMARK_FAILED: required scenario assets could not be loaded."));
        return;
    }

    BeginSpawning();
}

void AMBSTSingleTurretBenchmarkActor::ApplyCommandLineOverrides()
{
    const TCHAR* CommandLine = FCommandLine::Get();
    FParse::Value(CommandLine, TEXT("MBSTTanksPerSide="), TanksPerSide);
    FParse::Value(CommandLine, TEXT("MBSTWarmup="), WarmupSeconds);
    FParse::Value(CommandLine, TEXT("MBSTSample="), SampleSeconds);
    FParse::Value(CommandLine, TEXT("MBSTLegacySpawnPerFrame="), LegacyActorsPerFrame);
    FParse::Value(CommandLine, TEXT("MBSTOutputDir="), OutputDirectoryOverride);
    bSkipScreenshots = FParse::Param(CommandLine, TEXT("MBSTSkipScreenshots"));
    bDoNotExit = FParse::Param(CommandLine, TEXT("MBSTNoExit"));
}

void AMBSTSingleTurretBenchmarkActor::CreateBenchmarkEnvironment()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    if (AWorldSettings* Settings = World->GetWorldSettings())
    {
        Settings->bEnableWorldBoundsChecks = false;
        Settings->bForceNoPrecomputedLighting = true;
    }

    if (UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
    {
        AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>(FVector(0.0, 0.0, -10.0), FRotator::ZeroRotator);
        if (Floor)
        {
            Floor->SetActorLabel(TEXT("MBST_BenchmarkFloor"));
            Floor->GetStaticMeshComponent()->SetStaticMesh(Cube);
            Floor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
            Floor->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Floor->SetActorScale3D(FVector(800.0, 800.0, 0.2));
        }
    }

    ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector::ZeroVector, FRotator(-52.0, -35.0, 0.0));
    if (Sun && Sun->GetLightComponent())
    {
        Sun->GetLightComponent()->SetMobility(EComponentMobility::Movable);
        Sun->GetLightComponent()->SetIntensity(7.5f);
        Sun->GetLightComponent()->SetLightColor(FLinearColor(1.0f, 0.94f, 0.82f));
    }

    WideCamera = World->SpawnActor<ACameraActor>();
    CloseCamera = World->SpawnActor<ACameraActor>();
    if (WideCamera)
    {
        const FVector Location(0.0, -62000.0, 44000.0);
        WideCamera->SetActorLocationAndRotation(Location, (FVector::ZeroVector - Location).Rotation());
        WideCamera->GetCameraComponent()->SetFieldOfView(55.0f);
    }
    if (CloseCamera)
    {
        const float RegionDepth = static_cast<float>(FormationDepth - 1) * FormationSpacing;
        const float RegionWidth = static_cast<float>(FormationWidth - 1) * FormationSpacing;
        const FVector Target(
            -ArmyCenterX + RegionDepth * 0.35f,
            -RegionWidth * 0.5f + FMath::Min(4000.0f, RegionWidth * 0.25f),
            300.0f);
        const FVector Location = Target + FVector(-5000.0, -6500.0, 2400.0);
        CloseCamera->SetActorLocationAndRotation(Location, (Target - Location).Rotation());
        CloseCamera->GetCameraComponent()->SetFieldOfView(48.0f);
    }

    if (APlayerController* PlayerController = World->GetFirstPlayerController())
    {
        PlayerController->ConsoleCommand(TEXT("r.VSync 0"), true);
        PlayerController->ConsoleCommand(TEXT("t.MaxFPS 0"), true);
        PlayerController->ConsoleCommand(TEXT("r.MotionBlurQuality 0"), true);
        PlayerController->ConsoleCommand(TEXT("r.ScreenPercentage 100"), true);
        PlayerController->ConsoleCommand(TEXT("DisableAllScreenMessages"), true);
        if (WideCamera)
        {
            PlayerController->SetViewTarget(WideCamera);
        }
    }
}

bool AMBSTSingleTurretBenchmarkActor::LoadScenarioAssets()
{
    if (Scenario == EMBSTBenchmarkScenario::LegacyCompound)
    {
        UClass* LoadedClass = StaticLoadClass(
            AActor::StaticClass(),
            nullptr,
            TEXT("/MassBattle/Test/CompoundUnitAsset/BP_TankActor.BP_TankActor_C"));
        LegacyTankClass = LoadedClass;
        return LegacyTankClass != nullptr;
    }

    SingleTurretConfig = LoadObject<UMassBattleAgentConfigDataAsset>(
        nullptr,
        TEXT("/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret_AgentConfig.Tank_SingleTurret_AgentConfig"));
    return SingleTurretConfig != nullptr;
}

void AMBSTSingleTurretBenchmarkActor::BeginSpawning()
{
    Phase = EMBSTBenchmarkPhase::Spawning;
    PhaseElapsedSeconds = 0.0f;
    SpawnedTankCount = 0;
    SpawnedUnitEntityCount = 0;
    SpawnedSideCounts[0] = 0;
    SpawnedSideCounts[1] = 0;

    if (Scenario == EMBSTBenchmarkScenario::SingleTurret)
    {
        SpawnSingleTurretForces();
    }
}

void AMBSTSingleTurretBenchmarkActor::SpawnSingleTurretForces()
{
    UWorld* World = GetWorld();
    UMassBattleAgentSubsystem* Spawner = World ? World->GetSubsystem<UMassBattleAgentSubsystem>() : nullptr;
    if (!Spawner || !SingleTurretConfig)
    {
        Phase = EMBSTBenchmarkPhase::Failed;
        return;
    }

    FAgentSpawnRectangleShapeData Shape;
    Shape.Region = FVector2D(
        static_cast<float>(FormationDepth - 1) * FormationSpacing,
        static_cast<float>(FormationWidth - 1) * FormationSpacing);
    Shape.Spacing = FVector2D(FormationSpacing, FormationSpacing);
    Shape.ShapeDirection = FVector::ForwardVector;
    Shape.PositioningMode = ESpawnPositioningMode::Sequential;

    for (int32 SideIndex = 0; SideIndex < 2; ++SideIndex)
    {
        const int32 Team = SideIndex + 1;
        const FVector Origin(SideIndex == 0 ? -ArmyCenterX : ArmyCenterX, 0.0, 0.0);
        const FRotator Rotation = GetFormationRotation(SideIndex);
        TArray<FEntityHandle> Handles = Spawner->SpawnAgentsByConfigRectangular(
            SingleTurretConfig,
            TanksPerSide,
            Team,
            Origin,
            Shape,
            FVector2D::ZeroVector,
            EInitialRotation::CustomRotation,
            Rotation,
            FSpawnerMult(),
            true);

        for (const FEntityHandle& Handle : Handles)
        {
            ConfigureControlledEntity(Handle, false, true, Team);
        }
        SpawnedSideCounts[SideIndex] = Handles.Num();
        SpawnedTankCount += Handles.Num();
        SpawnedUnitEntityCount += Handles.Num();
    }

    if (SpawnedSideCounts[0] == TanksPerSide && SpawnedSideCounts[1] == TanksPerSide)
    {
        EnterWarmup();
    }
    else
    {
        Phase = EMBSTBenchmarkPhase::Failed;
        UE_LOG(LogTemp, Error, TEXT("MBST_BENCHMARK_FAILED: direct tank spawn count was %d/%d and %d/%d."),
            SpawnedSideCounts[0], TanksPerSide, SpawnedSideCounts[1], TanksPerSide);
    }
}

void AMBSTSingleTurretBenchmarkActor::SpawnLegacyBatch()
{
    int32 RemainingThisFrame = LegacyActorsPerFrame;
    while (RemainingThisFrame-- > 0 && SpawnedTankCount < TanksPerSide * 2)
    {
        const int32 SideIndex = SpawnedSideCounts[0] <= SpawnedSideCounts[1] ? 0 : 1;
        const int32 SideTankIndex = SpawnedSideCounts[SideIndex];
        if (!SpawnOneLegacyTank(SideIndex, SideTankIndex))
        {
            Phase = EMBSTBenchmarkPhase::Failed;
            UE_LOG(LogTemp, Error, TEXT("MBST_BENCHMARK_FAILED: legacy tank %d on side %d failed to spawn."), SideTankIndex, SideIndex);
            return;
        }
        ++SpawnedSideCounts[SideIndex];
        ++SpawnedTankCount;
    }

    if (SpawnedTankCount == TanksPerSide * 2)
    {
        EnterWarmup();
    }
}

bool AMBSTSingleTurretBenchmarkActor::SpawnOneLegacyTank(const int32 SideIndex, const int32 SideTankIndex)
{
    UWorld* World = GetWorld();
    if (!World || !LegacyTankClass)
    {
        return false;
    }

    const FTransform SpawnTransform(GetFormationRotation(SideIndex), GetFormationPosition(SideIndex, SideTankIndex));
    AActor* Tank = World->SpawnActorDeferred<AActor>(
        LegacyTankClass,
        SpawnTransform,
        this,
        nullptr,
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
    if (!Tank)
    {
        return false;
    }

    UGameplayStatics::FinishSpawningActor(Tank, SpawnTransform);

    // Blueprint SCS components are constructed by FinishSpawningActor. The
    // legacy demo's Auto components have therefore already emitted their four
    // original entities with the Blueprint default team; update FTeam here and
    // let MassBattle's own behavior processor migrate the corresponding tag.
    TInlineComponentArray<UMassBattleAgentComponent*> AgentComponents(Tank);
    int32 ValidEntityCount = 0;
    for (UMassBattleAgentComponent* Component : AgentComponents)
    {
        if (!Component)
        {
            continue;
        }
        const FEntityHandle Handle = Component->GetEntityHandle();
        if (!Handle.IsSet())
        {
            continue;
        }
        Component->TeamIndex = SideIndex + 1;
        const bool bTurret = Component->GetFName() == TEXT("Turret");
        ConfigureControlledEntity(Handle, bTurret, false, SideIndex + 1);
        ++ValidEntityCount;
    }
    SpawnedUnitEntityCount += ValidEntityCount;
    return ValidEntityCount == 4;
}

void AMBSTSingleTurretBenchmarkActor::ConfigureControlledEntity(
    const FEntityHandle& EntityHandle,
    const bool bTurretEntity,
    const bool bSingleTurretEntity,
    const int32 ForcedTeamIndex)
{
    UMassAPISubsystem* MassAPI = GetWorld() ? GetWorld()->GetSubsystem<UMassAPISubsystem>() : nullptr;
    if (!MassAPI || !EntityHandle.IsSet() || !MassAPI->IsValid(EntityHandle))
    {
        return;
    }

    if (FAttack* Attack = MassAPI->GetFragmentPtr<FAttack>(EntityHandle))
    {
        Attack->bEnable = false;
    }
    if (FTrace* Trace = MassAPI->GetFragmentPtr<FTrace>(EntityHandle))
    {
        Trace->bEnable = false;
    }
    if (FAgentDebug* Debug = MassAPI->GetFragmentPtr<FAgentDebug>(EntityHandle))
    {
        Debug->bEnable = false;
        Debug->bDrawColliderShape = false;
        Debug->bDrawTraceShape = false;
        Debug->bDrawMoveShape = false;
        Debug->bDrawNavShape = false;
        Debug->bDrawAvoShape = false;
        Debug->bDrawGatherShape = false;
        Debug->bDrawBehavior = false;
        Debug->bDrawTask = false;
    }
    // The legacy compound demo ships its individual AgentConfigs with
    // Visualize.bEnable disabled because BP_TankActor owns their composition.
    // This benchmark compares the actual Mass render paths, so enable the
    // per-entity renderer at runtime without changing those source assets.
    if (FVisualize* Visualize = MassAPI->GetFragmentPtr<FVisualize>(EntityHandle))
    {
        Visualize->bEnable = true;
    }
    if (FMove* Move = MassAPI->GetFragmentPtr<FMove>(EntityHandle))
    {
        // Keep the controlled armies on their formation plane. The demo map has
        // no FlowField ground data, so leaving the source tank's strict ground
        // mode enabled would make every entity fall out of the camera view.
        Move->bEnable = false;
        Move->Z.bEnable = false;
    }
    if (ForcedTeamIndex >= 0)
    {
        if (FTeam* Team = MassAPI->GetFragmentPtr<FTeam>(EntityHandle))
        {
            Team->index = ForcedTeamIndex;
        }
    }
    if (bTurretEntity)
    {
        MassAPI->AddTag<FMBSTBenchmarkLegacyTurretTag>(EntityHandle);
    }
    if (bSingleTurretEntity)
    {
        MassAPI->AddTag<FMBSTBenchmarkSingleTurretTag>(EntityHandle);
    }
}

void AMBSTSingleTurretBenchmarkActor::EnterWarmup()
{
    Phase = EMBSTBenchmarkPhase::Warmup;
    PhaseElapsedSeconds = 0.0f;
    FrameTimeSamplesMilliseconds.Reset();
    SetCloseCamera(false);
    UE_LOG(LogTemp, Display, TEXT("MBST_BENCHMARK_READY: %s, tanks=%d, unit_entities=%d."),
        *GetScenarioLabel(), SpawnedTankCount, SpawnedUnitEntityCount);
}

void AMBSTSingleTurretBenchmarkActor::EnterSampling()
{
    Phase = EMBSTBenchmarkPhase::Sampling;
    PhaseElapsedSeconds = 0.0f;
    FrameTimeSamplesMilliseconds.Reset();
    FrameTimeSamplesMilliseconds.Reserve(FMath::CeilToInt(SampleSeconds * 240.0f));
    SetCloseCamera(false);
}

void AMBSTSingleTurretBenchmarkActor::FinishSampling()
{
    if (FrameTimeSamplesMilliseconds.IsEmpty())
    {
        Phase = EMBSTBenchmarkPhase::Failed;
        return;
    }

    TArray<float> Sorted = FrameTimeSamplesMilliseconds;
    Sorted.Sort();
    double Sum = 0.0;
    for (const float Value : Sorted)
    {
        Sum += Value;
    }
    AverageFrameMilliseconds = static_cast<float>(Sum / static_cast<double>(Sorted.Num()));
    MedianFrameMilliseconds = GetPercentile(Sorted, 0.50f);
    P95FrameMilliseconds = GetPercentile(Sorted, 0.95f);
    P99FrameMilliseconds = GetPercentile(Sorted, 0.99f);
    MaximumFrameMilliseconds = Sorted.Last();

    Phase = EMBSTBenchmarkPhase::Complete;
    PhaseElapsedSeconds = 0.0f;
    CompletionElapsedSeconds = 0.0f;
    WriteResultJson();
    UE_LOG(LogTemp, Display,
        TEXT("MBST_BENCHMARK_RESULT: scenario=%s tanks=%d entities=%d samples=%d avg_ms=%.4f avg_fps=%.2f p50_ms=%.4f p95_ms=%.4f p99_ms=%.4f max_ms=%.4f result=%s"),
        *GetScenarioLabel(),
        SpawnedTankCount,
        SpawnedUnitEntityCount,
        FrameTimeSamplesMilliseconds.Num(),
        AverageFrameMilliseconds,
        AverageFrameMilliseconds > 0.0f ? 1000.0f / AverageFrameMilliseconds : 0.0f,
        MedianFrameMilliseconds,
        P95FrameMilliseconds,
        P99FrameMilliseconds,
        MaximumFrameMilliseconds,
        *ResultFilePath);
}

void AMBSTSingleTurretBenchmarkActor::Tick(const float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    const double CurrentWallClockSeconds = FPlatformTime::Seconds();
    const float FrameWallSeconds = LastFrameWallClockSeconds > 0.0
        ? static_cast<float>(FMath::Max(CurrentWallClockSeconds - LastFrameWallClockSeconds, 0.0))
        : DeltaSeconds;
    LastFrameWallClockSeconds = CurrentWallClockSeconds;
    PhaseElapsedSeconds += FrameWallSeconds;
    FlushPendingScreenshot();

    switch (Phase)
    {
    case EMBSTBenchmarkPhase::Spawning:
        if (Scenario == EMBSTBenchmarkScenario::LegacyCompound)
        {
            SpawnLegacyBatch();
        }
        break;

    case EMBSTBenchmarkPhase::Warmup:
    {
        if (!bRenderDiagnosticsLogged && PhaseElapsedSeconds >= 3.0f)
        {
            if (UMassBattleSubsystem* BattleSubsystem = UMassBattleSubsystem::GetPtr(this))
            {
                if (BattleSubsystem->AgentRenderers.IsEmpty())
                {
                    break;
                }
                bRenderDiagnosticsLogged = true;
                UE_LOG(LogTemp, Display, TEXT("MBST_RENDER_DIAGNOSTIC: renderer_count=%d"), BattleSubsystem->AgentRenderers.Num());
                for (const TPair<int32, TObjectPtr<AMassBattleAgentRenderer>>& Pair : BattleSubsystem->AgentRenderers)
                {
                    const AMassBattleAgentRenderer* Renderer = Pair.Value.Get();
                    int32 BatchCount = 0;
                    int32 InstanceCount = 0;
                    if (Renderer)
                    {
                        BatchCount = Renderer->SpawnedRenderBatches.Num();
                        for (const TPair<int32, FAgentRenderBatchData>& BatchPair : Renderer->SpawnedRenderBatches)
                        {
                            InstanceCount += BatchPair.Value.LocationArray.Num();
                            if (!BatchPair.Value.LocationArray.IsEmpty())
                            {
                                const int32 FirstIndex = 0;
                                UE_LOG(LogTemp, Display,
                                    TEXT("MBST_RENDER_DIAGNOSTIC: batch=%d first_location=%s first_scale=%s first_hidden=%d first_mesh_index=%d first_style=%d"),
                                    BatchPair.Key,
                                    *BatchPair.Value.LocationArray[FirstIndex].ToString(),
                                    *BatchPair.Value.ScaleArray[FirstIndex].ToString(),
                                    BatchPair.Value.IsHiddenArray[FirstIndex] ? 1 : 0,
                                    BatchPair.Value.CurrentLODArray[FirstIndex],
                                    BatchPair.Value.StyleArray.IsValidIndex(FirstIndex) ? BatchPair.Value.StyleArray[FirstIndex] : -1);
                            }
                        }
                    }
                    UE_LOG(LogTemp, Display,
                        TEXT("MBST_RENDER_DIAGNOSTIC: subtype=%d renderer=%s mesh=%s niagara=%s batches=%d instances=%d"),
                        Pair.Key,
                        *GetNameSafe(Renderer),
                        Renderer ? *GetNameSafe(Renderer->AgentMesh) : TEXT("None"),
                        Renderer ? *GetNameSafe(Renderer->NiagaraSystemAsset) : TEXT("None"),
                        BatchCount,
                        InstanceCount);
                }
            }
        }

        const float SweepYaw = GetLiveSweepYawDegrees();
        if (PendingScreenshotFrames == 0 && !bSkipScreenshots && !bWideCaptured && PhaseElapsedSeconds >= 0.5f)
        {
            CaptureScreenshot(TEXT("Wide"), false, true);
            bWideCaptured = true;
        }
        else if (PendingScreenshotFrames == 0 && !bSkipScreenshots && !bYawPlusCaptured && SweepYaw >= 65.0f)
        {
            CaptureScreenshot(TEXT("YawPlus"), true, true);
            bYawPlusCaptured = true;
        }
        else if (PendingScreenshotFrames == 0 && !bSkipScreenshots && bYawPlusCaptured && !bYawMinusCaptured && SweepYaw <= -65.0f)
        {
            CaptureScreenshot(TEXT("YawMinus"), true, true);
            bYawMinusCaptured = true;
        }

        const bool bVisualProofComplete = bSkipScreenshots
            || (bWideCaptured && bYawPlusCaptured && bYawMinusCaptured && PendingScreenshotFrames == 0);
        const float RequiredWarmup = bSkipScreenshots ? WarmupSeconds : FMath::Max(WarmupSeconds, 8.25f);
        if (PhaseElapsedSeconds >= RequiredWarmup && bVisualProofComplete)
        {
            EnterSampling();
        }
        break;
    }

    case EMBSTBenchmarkPhase::Sampling:
        FrameTimeSamplesMilliseconds.Add(FrameWallSeconds * 1000.0f);
        if (PhaseElapsedSeconds >= SampleSeconds)
        {
            FinishSampling();
        }
        break;

    case EMBSTBenchmarkPhase::Complete:
        CompletionElapsedSeconds += FrameWallSeconds;
        if (!bSkipScreenshots && !bResultCaptured && CompletionElapsedSeconds >= 0.5f)
        {
            CaptureScreenshot(TEXT("Result"), false, false);
            bResultCaptured = true;
        }
        if (!bDoNotExit && CompletionElapsedSeconds >= (bSkipScreenshots ? 0.5f : 3.0f))
        {
            FPlatformMisc::RequestExit(false);
        }
        break;

    default:
        break;
    }
}

float AMBSTSingleTurretBenchmarkActor::GetLiveSweepYawDegrees() const
{
    return MBSTBenchmark::SweepAmplitudeDegrees * FMath::Sin(
        MBSTBenchmark::TwoPi * static_cast<float>(
            FMath::Fmod(FPlatformTime::Seconds(), static_cast<double>(MBSTBenchmark::SweepPeriodSeconds))
            / static_cast<double>(MBSTBenchmark::SweepPeriodSeconds)));
}

FString AMBSTSingleTurretBenchmarkActor::GetScenarioLabel() const
{
    return Scenario == EMBSTBenchmarkScenario::LegacyCompound
        ? TEXT("LEGACY COMPOUND TANK")
        : TEXT("PLUGIN SINGLE-ENTITY TURRET TANK");
}

FString AMBSTSingleTurretBenchmarkActor::GetPhaseLabel() const
{
    switch (Phase)
    {
    case EMBSTBenchmarkPhase::Waiting: return TEXT("WAITING");
    case EMBSTBenchmarkPhase::Spawning: return TEXT("SPAWNING");
    case EMBSTBenchmarkPhase::Warmup: return TEXT("WARMUP / VISUAL PROOF");
    case EMBSTBenchmarkPhase::Sampling: return TEXT("TIMED SAMPLE (NO SALVO OVERLAY)");
    case EMBSTBenchmarkPhase::Complete: return TEXT("COMPLETE");
    case EMBSTBenchmarkPhase::Failed: return TEXT("FAILED");
    default: return TEXT("UNKNOWN");
    }
}

FVector AMBSTSingleTurretBenchmarkActor::GetFormationPosition(const int32 SideIndex, const int32 SideTankIndex) const
{
    const int32 DepthIndex = SideTankIndex / FormationWidth;
    const int32 WidthIndex = SideTankIndex % FormationWidth;
    const float LocalX = (static_cast<float>(DepthIndex) - static_cast<float>(FormationDepth - 1) * 0.5f) * FormationSpacing;
    const float LocalY = (static_cast<float>(WidthIndex) - static_cast<float>(FormationWidth - 1) * 0.5f) * FormationSpacing;
    return FVector((SideIndex == 0 ? -ArmyCenterX : ArmyCenterX) + LocalX, LocalY, 0.0f);
}

FRotator AMBSTSingleTurretBenchmarkActor::GetFormationRotation(const int32 SideIndex) const
{
    return FRotator(0.0, SideIndex == 0 ? 0.0 : 180.0, 0.0);
}

float AMBSTSingleTurretBenchmarkActor::GetPercentile(const TArray<float>& SortedValues, const float Alpha) const
{
    if (SortedValues.IsEmpty())
    {
        return 0.0f;
    }
    const float Position = FMath::Clamp(Alpha, 0.0f, 1.0f) * static_cast<float>(SortedValues.Num() - 1);
    const int32 Lower = FMath::FloorToInt(Position);
    const int32 Upper = FMath::CeilToInt(Position);
    return FMath::Lerp(SortedValues[Lower], SortedValues[Upper], Position - static_cast<float>(Lower));
}

FString AMBSTSingleTurretBenchmarkActor::GetOutputDirectory() const
{
    if (!OutputDirectoryOverride.IsEmpty())
    {
        return FPaths::ConvertRelativePathToFull(OutputDirectoryOverride);
    }
    return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MassBattleSingleTurret/Benchmark"));
}

void AMBSTSingleTurretBenchmarkActor::WriteResultJson()
{
    const FString OutputDirectory = GetOutputDirectory();
    IFileManager::Get().MakeDirectory(*OutputDirectory, true);
    const FString ScenarioToken = Scenario == EMBSTBenchmarkScenario::LegacyCompound ? TEXT("Legacy") : TEXT("SingleTurret");
    ResultFilePath = OutputDirectory / FString::Printf(TEXT("%s_%dv%d.json"), *ScenarioToken, TanksPerSide, TanksPerSide);

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("benchmark_schema_version"), 1);
    Root->SetStringField(TEXT("scenario"), GetScenarioLabel());
    Root->SetStringField(TEXT("comparison_mode"), TEXT("controlled_turret_articulation"));
    Root->SetStringField(TEXT("frame_timing_source"), TEXT("FPlatformTime wall-clock interval"));
    Root->SetBoolField(TEXT("source_weapon_logic_disabled"), true);
    Root->SetStringField(TEXT("visual_salvo_timing"), TEXT("warmup_only_excluded_from_sample"));
    Root->SetNumberField(TEXT("tanks_per_side"), TanksPerSide);
    Root->SetNumberField(TEXT("spawned_tanks"), SpawnedTankCount);
    Root->SetNumberField(TEXT("spawned_unit_entities"), SpawnedUnitEntityCount);
    Root->SetNumberField(TEXT("sweep_amplitude_degrees"), MBSTBenchmark::SweepAmplitudeDegrees);
    Root->SetNumberField(TEXT("sweep_period_seconds"), MBSTBenchmark::SweepPeriodSeconds);
    Root->SetNumberField(TEXT("warmup_seconds"), WarmupSeconds);
    Root->SetNumberField(TEXT("requested_sample_seconds"), SampleSeconds);
    Root->SetNumberField(TEXT("sample_count"), FrameTimeSamplesMilliseconds.Num());
    Root->SetNumberField(TEXT("average_frame_ms"), AverageFrameMilliseconds);
    Root->SetNumberField(TEXT("average_fps"), AverageFrameMilliseconds > 0.0f ? 1000.0f / AverageFrameMilliseconds : 0.0f);
    Root->SetNumberField(TEXT("median_frame_ms"), MedianFrameMilliseconds);
    Root->SetNumberField(TEXT("p95_frame_ms"), P95FrameMilliseconds);
    Root->SetNumberField(TEXT("p99_frame_ms"), P99FrameMilliseconds);
    Root->SetNumberField(TEXT("maximum_frame_ms"), MaximumFrameMilliseconds);
    Root->SetStringField(TEXT("cpu"), FPlatformMisc::GetCPUBrand());
    Root->SetStringField(TEXT("command_line"), FCommandLine::Get());

    FString JsonText;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
    FJsonSerializer::Serialize(Root, Writer);
    if (!FFileHelper::SaveStringToFile(JsonText, *ResultFilePath))
    {
        UE_LOG(LogTemp, Error, TEXT("MBST benchmark could not write %s"), *ResultFilePath);
    }
}

void AMBSTSingleTurretBenchmarkActor::SetCloseCamera(const bool bCloseView) const
{
    if (APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
    {
        ACameraActor* Target = bCloseView ? CloseCamera.Get() : WideCamera.Get();
        if (Target)
        {
            PlayerController->SetViewTarget(Target);
        }
    }
}

void AMBSTSingleTurretBenchmarkActor::CaptureScreenshot(const FString& Suffix, const bool bCloseView, const bool bVisualSalvo)
{
    SetCloseCamera(bCloseView);
    const FString OutputDirectory = GetOutputDirectory();
    IFileManager::Get().MakeDirectory(*OutputDirectory, true);
    const FString ScenarioToken = Scenario == EMBSTBenchmarkScenario::LegacyCompound ? TEXT("Legacy") : TEXT("SingleTurret");
    PendingScreenshotFilename = OutputDirectory / FString::Printf(
        TEXT("%s_%dv%d_%s.png"), *ScenarioToken, TanksPerSide, TanksPerSide, *Suffix);
    bPendingVisualSalvo = bVisualSalvo;
    PendingScreenshotFrames = 2;
}

void AMBSTSingleTurretBenchmarkActor::FlushPendingScreenshot()
{
    if (PendingScreenshotFrames <= 0 || PendingScreenshotFilename.IsEmpty())
    {
        return;
    }
    --PendingScreenshotFrames;
    if (PendingScreenshotFrames > 0)
    {
        return;
    }
    if (bPendingVisualSalvo)
    {
        DrawVisualSalvo();
    }
    FScreenshotRequest::RequestScreenshot(PendingScreenshotFilename, true, false);
    UE_LOG(LogTemp, Display, TEXT("MBST_BENCHMARK_SCREENSHOT: %s"), *PendingScreenshotFilename);
    PendingScreenshotFilename.Reset();
    bPendingVisualSalvo = false;
}

void AMBSTSingleTurretBenchmarkActor::DrawVisualSalvo() const
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }
    const float SweepYaw = GetLiveSweepYawDegrees();
    const int32 LineCountPerSide = FMath::Min(24, TanksPerSide);
    const int32 Step = FMath::Max(1, TanksPerSide / LineCountPerSide);
    for (int32 SideIndex = 0; SideIndex < 2; ++SideIndex)
    {
        const float BodyYaw = SideIndex == 0 ? 0.0f : 180.0f;
        const FVector Direction = FRotator(0.0f, BodyYaw + SweepYaw, 0.0f).Vector();
        const FColor Color = SideIndex == 0 ? FColor(255, 65, 35) : FColor(40, 130, 255);
        for (int32 TankIndex = 0; TankIndex < TanksPerSide; TankIndex += Step)
        {
            const FVector Start = GetFormationPosition(SideIndex, TankIndex) + FVector(0.0, 0.0, 310.0) + Direction * 260.0;
            const FVector End = Start + Direction * 5200.0;
            DrawDebugLine(World, Start, End, Color, false, 0.3f, 0, 2.0f);
        }
    }
}

void AMBSTSingleTurretBenchmarkHUD::DrawHUD()
{
    Super::DrawHUD();
    if (!Canvas)
    {
        return;
    }

    if (!CachedBenchmark.IsValid())
    {
        TArray<AActor*> Actors;
        UGameplayStatics::GetAllActorsOfClass(GetWorld(), AMBSTSingleTurretBenchmarkActor::StaticClass(), Actors);
        if (!Actors.IsEmpty())
        {
            CachedBenchmark = Cast<AMBSTSingleTurretBenchmarkActor>(Actors[0]);
        }
    }
    AMBSTSingleTurretBenchmarkActor* Benchmark = CachedBenchmark.Get();
    if (!Benchmark)
    {
        return;
    }

    const float Scale = FMath::Clamp(static_cast<float>(Canvas->SizeY) / 1080.0f, 0.7f, 1.5f);
    const float X = 28.0f * Scale;
    float Y = 24.0f * Scale;
    const float Line = 27.0f * Scale;
    UFont* Font = GEngine ? GEngine->GetMediumFont() : nullptr;
    UFont* SmallFont = GEngine ? GEngine->GetSmallFont() : Font;

    DrawRect(FLinearColor(0.015f, 0.02f, 0.03f, 0.86f), 14.0f * Scale, 14.0f * Scale, 820.0f * Scale, 270.0f * Scale);
    DrawText(TEXT("MassBattle 10,000-Tank Single-Turret Benchmark"), FLinearColor(1.0f, 0.82f, 0.16f), X, Y, Font, 1.15f * Scale, false);
    Y += Line * 1.35f;
    DrawText(FString::Printf(TEXT("Scenario: %s"), *Benchmark->GetScenarioLabel()), FLinearColor::White, X, Y, Font, Scale, false);
    Y += Line;
    DrawText(FString::Printf(TEXT("Forces: %d vs %d tanks | Spawned: %d tanks / %d unit entities"),
        Benchmark->TanksPerSide, Benchmark->TanksPerSide, Benchmark->SpawnedTankCount, Benchmark->SpawnedUnitEntityCount),
        FLinearColor(0.72f, 0.9f, 1.0f), X, Y, SmallFont, Scale, false);
    Y += Line;
    DrawText(FString::Printf(TEXT("Forced turret sweep: +/-75 deg, 8.0 s period | Live yaw: %+06.1f deg"), Benchmark->GetLiveSweepYawDegrees()),
        FLinearColor(0.55f, 1.0f, 0.58f), X, Y, SmallFont, Scale, false);
    Y += Line;
    DrawText(FString::Printf(TEXT("Phase: %s"), *Benchmark->GetPhaseLabel()), FLinearColor(1.0f, 0.72f, 0.36f), X, Y, SmallFont, Scale, false);
    Y += Line;

    if (Benchmark->Phase == EMBSTBenchmarkPhase::Complete)
    {
        DrawText(FString::Printf(TEXT("RESULT: Avg %.3f ms (%.1f FPS) | P50 %.3f | P95 %.3f | P99 %.3f | Max %.3f ms"),
            Benchmark->AverageFrameMilliseconds,
            Benchmark->AverageFrameMilliseconds > 0.0f ? 1000.0f / Benchmark->AverageFrameMilliseconds : 0.0f,
            Benchmark->MedianFrameMilliseconds,
            Benchmark->P95FrameMilliseconds,
            Benchmark->P99FrameMilliseconds,
            Benchmark->MaximumFrameMilliseconds),
            FLinearColor(0.2f, 1.0f, 0.4f), X, Y, SmallFont, Scale, false);
    }
    else
    {
        DrawText(TEXT("Controlled comparison: source weapon logic OFF; visual salvos occur only outside timed sampling."),
            FLinearColor(0.82f, 0.82f, 0.82f), X, Y, SmallFont, Scale, false);
    }
}

AMBSTSingleTurretBenchmarkGameMode::AMBSTSingleTurretBenchmarkGameMode()
{
    HUDClass = AMBSTSingleTurretBenchmarkHUD::StaticClass();
    DefaultPawnClass = ASpectatorPawn::StaticClass();
}
