#include "MBSTSingleTurretBenchmark.h"

#include "MBSTSingleTurretAsset.h"
#include "MBSTSingleTurretTypes.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/MassBattleAgentComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DataAssets/MassBattleAgentConfigDataAsset.h"
#include "DrawDebugHelpers.h"
#include "DynamicRHI.h"
#include "Engine/Canvas.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Fragments/Attack.h"
#include "Fragments/Debug.h"
#include "Fragments/Health.h"
#include "Fragments/Move.h"
#include "Fragments/Render.h"
#include "Fragments/StyleType.h"
#include "Fragments/Team.h"
#include "Fragments/Trace.h"
#include "Fragments/Transform.h"
#include "FuncLibs/MassBattleFuncLib.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpectatorPawn.h"
#include "GameFramework/SpectatorPawnMovement.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "MassAPISubsystem.h"
#include "MassEntityTemplate.h"
#include "MassExecutionContext.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "RenderTimer.h"
#include "Renderers/MassBattleAgentRenderer.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Subsystems/MassBattleSubsystem.h"
#include "UnrealClient.h"

namespace MBSTBenchmark
{
    static constexpr float TwoPi = 2.0f * PI;

    // Written once by the benchmark actor before tagged entities are spawned,
    // then read by the native worker-thread processor for the rest of the run.
    static float RuntimeTargetRadius = 10000.0f;
    static float RuntimeTargetPeriodSeconds = 12.0f;
    static float RuntimeTargetHeight = 300.0f;
    static bool bRuntimeUseFixedTargetBearing = false;
    static float RuntimeFixedTargetBearingDegrees = 90.0f;

    struct FTimingStats
    {
        int32 Count = 0;
        float Average = 0.0f;
        float P50 = 0.0f;
        float P95 = 0.0f;
        float P99 = 0.0f;
        float Maximum = 0.0f;
    };

    static FVector CalculateTargetLocation(const double PlatformSeconds)
    {
        const double Period = FMath::Max(static_cast<double>(RuntimeTargetPeriodSeconds), 1.0);
        const float Angle = bRuntimeUseFixedTargetBearing
            ? FMath::DegreesToRadians(RuntimeFixedTargetBearingDegrees)
            : TwoPi * static_cast<float>(FMath::Fmod(PlatformSeconds, Period) / Period);
        return FVector(
            FMath::Cos(Angle) * RuntimeTargetRadius,
            FMath::Sin(Angle) * RuntimeTargetRadius,
            RuntimeTargetHeight);
    }

    static FVector ProjectAndNormalize(const FVector& Vector, const FVector& PlaneNormal, const FVector& Fallback)
    {
        FVector Projected = FVector::VectorPlaneProject(Vector, PlaneNormal);
        if (!Projected.Normalize())
        {
            Projected = FVector::VectorPlaneProject(Fallback, PlaneNormal);
            Projected.Normalize();
        }
        return Projected;
    }

    static float SignedAngleDegrees(const FVector& From, const FVector& To, const FVector& Axis)
    {
        const FVector SafeAxis = Axis.GetSafeNormal(SMALL_NUMBER, FVector::UpVector);
        const FVector SafeFrom = ProjectAndNormalize(From, SafeAxis, FVector::ForwardVector);
        const FVector SafeTo = ProjectAndNormalize(To, SafeAxis, SafeFrom);
        const float SinAngle = FVector::DotProduct(SafeAxis, FVector::CrossProduct(SafeFrom, SafeTo));
        const float CosAngle = FMath::Clamp(FVector::DotProduct(SafeFrom, SafeTo), -1.0f, 1.0f);
        return FMath::RadiansToDegrees(FMath::Atan2(SinAngle, CosAngle));
    }

    static float Percentile(const TArray<float>& SortedValues, const float Alpha)
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

    static FTimingStats CalculateTimingStats(const TArray<float>& Values)
    {
        FTimingStats Result;
        if (Values.IsEmpty())
        {
            return Result;
        }

        TArray<float> Sorted = Values;
        Sorted.Sort();
        double Sum = 0.0;
        for (const float Value : Sorted)
        {
            Sum += Value;
        }

        Result.Count = Sorted.Num();
        Result.Average = static_cast<float>(Sum / static_cast<double>(Sorted.Num()));
        Result.P50 = Percentile(Sorted, 0.50f);
        Result.P95 = Percentile(Sorted, 0.95f);
        Result.P99 = Percentile(Sorted, 0.99f);
        Result.Maximum = Sorted.Last();
        return Result;
    }

    static TSharedRef<FJsonObject> TimingStatsToJson(const FTimingStats& Stats)
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetNumberField(TEXT("sample_count"), Stats.Count);
        Json->SetNumberField(TEXT("average"), Stats.Average);
        Json->SetNumberField(TEXT("p50"), Stats.P50);
        Json->SetNumberField(TEXT("p95"), Stats.P95);
        Json->SetNumberField(TEXT("p99"), Stats.P99);
        Json->SetNumberField(TEXT("maximum"), Stats.Maximum);
        return Json;
    }

    static FEntityTemplateData MakeMassTemplate(
        const UObject* WorldContextObject,
        const UMassBattleAgentConfigDataAsset* Config,
        const bool bSingleTurret,
        const bool bAddBenchmarkTag)
    {
        const FEntityTemplateData Source = UMassBattleFuncLib::MakeTemplateDataFromDataAsset(
            WorldContextObject,
            Config);
        if (!Source.IsValid() || !Source.Get())
        {
            return FEntityTemplateData();
        }

        FMassEntityTemplateData Cloned = UMassAPISubsystem::CloneTemplate(*Source.Get());
        if (bSingleTurret)
        {
            if (bAddBenchmarkTag)
            {
                UMassAPISubsystem::AddTag<FMBSTBenchmarkSingleTurretTag>(Cloned);
            }
        }
        else
        {
            UMassAPISubsystem::RemoveTag<FMBSTSingleTurretTag>(Cloned);
            UMassAPISubsystem::RemoveFragment<FMBSTSingleTurretState>(Cloned);
            UMassAPISubsystem::RemoveSharedFragment<FMBSTSingleTurretShared>(Cloned);
            if (bAddBenchmarkTag)
            {
                UMassAPISubsystem::AddTag<FMBSTBenchmarkMassBaselineTag>(Cloned);
            }
        }

        return FEntityTemplateData(MakeShared<FMassEntityTemplateData>(MoveTemp(Cloned)));
    }

    static void ForceWorldYaw(FRotating& Rotating, const FVector& From, const FVector& Target)
    {
        const FVector FlatDirection(Target.X - From.X, Target.Y - From.Y, 0.0);
        if (FlatDirection.IsNearlyZero())
        {
            return;
        }
        const FRotator3f ForcedRotation(0.0f, static_cast<float>(FlatDirection.Rotation().Yaw), 0.0f);
        Rotating.Rotation = ForcedRotation;
        Rotating.RotationQuat = ForcedRotation.Quaternion();
        Rotating.DesiredRotationQuat = Rotating.RotationQuat;
        Rotating.Direction = ForcedRotation.Vector();
    }
}

UMBSTSingleTurretBenchmarkDriveProcessor::UMBSTSingleTurretBenchmarkDriveProcessor()
    : LegacyQuery(*this)
    , MassBaselineQuery(*this)
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
        .All<FLocating>(MARO)
        .All<FRotating>(MARW)
        .RegisterWithProcessor(*this);

    FEntityQueryBuilder(MassBaselineQuery)
        .All<FMBSTBenchmarkMassBaselineTag>()
        .All<FLocating>(MARO)
        .All<FRotating>(MARW)
        .RegisterWithProcessor(*this);

    FEntityQueryBuilder(SingleTurretQuery)
        .All<FMBSTBenchmarkSingleTurretTag>()
        .All<FLocating, FRotating, FScaling>(MARO)
        .All<FMBSTSingleTurretState>(MARW)
        .All<FMBSTSingleTurretShared>(MARO)
        .RegisterWithProcessor(*this);
}

void UMBSTSingleTurretBenchmarkDriveProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
    if (!Context.GetWorld())
    {
        return;
    }

    const FVector Target = MBSTBenchmark::CalculateTargetLocation(FPlatformTime::Seconds());

    LegacyQuery.ForEachEntityChunk(Context, [Target](FMassExecutionContext& ChunkContext)
    {
        const TConstArrayView<FLocating> Locations = ChunkContext.GetFragmentView<FLocating>();
        TArrayView<FRotating> Rotations = ChunkContext.GetMutableFragmentView<FRotating>();
        for (int32 EntityIndex = 0; EntityIndex < ChunkContext.GetNumEntities(); ++EntityIndex)
        {
            MBSTBenchmark::ForceWorldYaw(Rotations[EntityIndex], Locations[EntityIndex].Location, Target);
        }
    });

    MassBaselineQuery.ForEachEntityChunk(Context, [Target](FMassExecutionContext& ChunkContext)
    {
        const TConstArrayView<FLocating> Locations = ChunkContext.GetFragmentView<FLocating>();
        TArrayView<FRotating> Rotations = ChunkContext.GetMutableFragmentView<FRotating>();
        for (int32 EntityIndex = 0; EntityIndex < ChunkContext.GetNumEntities(); ++EntityIndex)
        {
            MBSTBenchmark::ForceWorldYaw(Rotations[EntityIndex], Locations[EntityIndex].Location, Target);
        }
    });

    SingleTurretQuery.ForEachEntityChunk(Context, [Target](FMassExecutionContext& ChunkContext)
    {
        const FMBSTSingleTurretShared& Shared = ChunkContext.GetSharedFragment<FMBSTSingleTurretShared>();
        const UMBSTSingleTurretAsset* Layout = Shared.Layout.Get();
        if (!Layout)
        {
            return;
        }

        const FVector TurretAxis = FVector(Layout->TurretAxisObjectSpace).GetSafeNormal(SMALL_NUMBER, FVector::UpVector);
        const FVector DefaultForward = FVector(Layout->BarrelForwardAxisObjectSpace).GetSafeNormal(SMALL_NUMBER, FVector::ForwardVector);
        const FVector TurretPivot(Layout->TurretPivotObjectSpace);
        const float MinYaw = FMath::Min(Shared.YawLimitsDegrees.X, Shared.YawLimitsDegrees.Y);
        const float MaxYaw = FMath::Max(Shared.YawLimitsDegrees.X, Shared.YawLimitsDegrees.Y);

        const TConstArrayView<FLocating> Locations = ChunkContext.GetFragmentView<FLocating>();
        const TConstArrayView<FRotating> Rotations = ChunkContext.GetFragmentView<FRotating>();
        const TConstArrayView<FScaling> Scales = ChunkContext.GetFragmentView<FScaling>();
        TArrayView<FMBSTSingleTurretState> States = ChunkContext.GetMutableFragmentView<FMBSTSingleTurretState>();

        for (int32 EntityIndex = 0; EntityIndex < ChunkContext.GetNumEntities(); ++EntityIndex)
        {
            const FQuat RootRotation(
                static_cast<double>(Rotations[EntityIndex].RotationQuat.X),
                static_cast<double>(Rotations[EntityIndex].RotationQuat.Y),
                static_cast<double>(Rotations[EntityIndex].RotationQuat.Z),
                static_cast<double>(Rotations[EntityIndex].RotationQuat.W));
            const float SafeScale = FMath::Max(FMath::Abs(Scales[EntityIndex].Scale), SMALL_NUMBER);
            const FVector TargetObject = RootRotation.UnrotateVector(Target - Locations[EntityIndex].Location) / SafeScale;
            const float DesiredYaw = FMath::Clamp(
                FMath::UnwindDegrees(MBSTBenchmark::SignedAngleDegrees(DefaultForward, TargetObject - TurretPivot, TurretAxis)),
                MinYaw,
                MaxYaw);

            FMBSTSingleTurretState& State = States[EntityIndex];
            State.TargetYawDegrees = DesiredYaw;
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
    if (bAutoFreeObservationInPIE && GetWorld() && GetWorld()->WorldType == EWorldType::PIE)
    {
        bFreeObservation = true;
    }
    if (bFreeObservation)
    {
        // Observation mode is intentionally not a benchmark capture: never
        // steal the view for screenshots and never close the user's PIE/game.
        bSkipScreenshots = true;
        bDoNotExit = true;
    }
    UnitCount = FMath::Max(UnitCount, 1);
    WarmupSeconds = FMath::Max(WarmupSeconds, 0.0f);
    SampleSeconds = FMath::Max(SampleSeconds, 1.0f);
    LegacyActorsPerFrame = FMath::Max(LegacyActorsPerFrame, 1);
    TargetOrbitRadius = FMath::Max(TargetOrbitRadius, 100.0f);
    TargetOrbitPeriodSeconds = FMath::Max(TargetOrbitPeriodSeconds, 1.0f);
    TargetHealth = FMath::Max(TargetHealth, 1.0f);
    FreeObservationSpeed = FMath::Max(FreeObservationSpeed, 100.0f);

    MBSTBenchmark::RuntimeTargetRadius = TargetOrbitRadius;
    MBSTBenchmark::RuntimeTargetPeriodSeconds = TargetOrbitPeriodSeconds;
    MBSTBenchmark::RuntimeTargetHeight = TargetHeight;
    MBSTBenchmark::bRuntimeUseFixedTargetBearing = bUseFixedTargetBearing;
    MBSTBenchmark::RuntimeFixedTargetBearingDegrees = FixedTargetBearingDegrees;

    FormationWidth = FMath::Max(1, FMath::CeilToInt(FMath::Sqrt(static_cast<float>(UnitCount))));
    FormationDepth = FMath::Max(1, FMath::CeilToInt(static_cast<float>(UnitCount) / static_cast<float>(FormationWidth)));

    CreateBenchmarkEnvironment();
    if (!LoadScenarioAssets() || !SpawnMovingTarget())
    {
        Phase = EMBSTBenchmarkPhase::Failed;
        UE_LOG(LogTemp, Error, TEXT("MBST_BENCHMARK_FAILED: required benchmark assets or moving target could not be created."));
        return;
    }

    BeginSpawning();
}

void AMBSTSingleTurretBenchmarkActor::ApplyCommandLineOverrides()
{
    const TCHAR* CommandLine = FCommandLine::Get();
    FParse::Value(CommandLine, TEXT("MBSTUnits="), UnitCount);
    // Compatibility with the first prototype's command line spelling.
    FParse::Value(CommandLine, TEXT("MBSTTanksPerSide="), UnitCount);
    FParse::Value(CommandLine, TEXT("MBSTWarmup="), WarmupSeconds);
    FParse::Value(CommandLine, TEXT("MBSTSample="), SampleSeconds);
    FParse::Value(CommandLine, TEXT("MBSTLegacySpawnPerFrame="), LegacyActorsPerFrame);
    FParse::Value(CommandLine, TEXT("MBSTTargetRadius="), TargetOrbitRadius);
    FParse::Value(CommandLine, TEXT("MBSTTargetPeriod="), TargetOrbitPeriodSeconds);
    FParse::Value(CommandLine, TEXT("MBSTTargetHeight="), TargetHeight);
    FParse::Value(CommandLine, TEXT("MBSTTargetHealth="), TargetHealth);
    FParse::Value(CommandLine, TEXT("MBSTOutputDir="), OutputDirectoryOverride);
    FParse::Value(CommandLine, TEXT("MBSTFreeObserveSpeed="), FreeObservationSpeed);
    if (FParse::Value(CommandLine, TEXT("MBSTFixedTargetBearing="), FixedTargetBearingDegrees))
    {
        bUseFixedTargetBearing = true;
    }

    FString ScenarioToken;
    if (FParse::Value(CommandLine, TEXT("MBSTScenario="), ScenarioToken))
    {
        if (ScenarioToken.Equals(TEXT("actor"), ESearchCase::IgnoreCase)
            || ScenarioToken.Equals(TEXT("legacy"), ESearchCase::IgnoreCase))
        {
            Scenario = EMBSTBenchmarkScenario::LegacyCompound;
        }
        else if (ScenarioToken.Equals(TEXT("mass"), ESearchCase::IgnoreCase)
            || ScenarioToken.Equals(TEXT("baseline"), ESearchCase::IgnoreCase))
        {
            Scenario = EMBSTBenchmarkScenario::MassBaseline;
        }
        else if (ScenarioToken.Equals(TEXT("turret"), ESearchCase::IgnoreCase)
            || ScenarioToken.Equals(TEXT("single"), ESearchCase::IgnoreCase))
        {
            Scenario = EMBSTBenchmarkScenario::SingleTurret;
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("Unknown -MBSTScenario=%s; using map/default scenario."), *ScenarioToken);
        }
    }

    bSkipScreenshots = FParse::Param(CommandLine, TEXT("MBSTSkipScreenshots"));
    bDoNotExit = FParse::Param(CommandLine, TEXT("MBSTNoExit"));
    bTopDownAimProof = bTopDownAimProof || FParse::Param(CommandLine, TEXT("MBSTTopDownAimProof"));
    bFreeObservation = bFreeObservation || FParse::Param(CommandLine, TEXT("MBSTFreeObserve"));
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
            Floor->SetActorLabel(TEXT("MBST_NativeBenchmarkFloor"));
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

    const float FormationX = static_cast<float>(FormationDepth - 1) * FormationSpacing;
    const float FormationY = static_cast<float>(FormationWidth - 1) * FormationSpacing;
    const float ViewExtent = FMath::Max3(FormationX, FormationY, TargetOrbitRadius * 2.0f);

    WideCamera = World->SpawnActor<ACameraActor>();
    CloseCamera = World->SpawnActor<ACameraActor>();
    if (WideCamera)
    {
        const FVector Location(0.0, -ViewExtent * 1.45f, ViewExtent * 0.9f + 2500.0f);
        WideCamera->SetActorLocationAndRotation(Location, (FVector(0.0, 0.0, 200.0) - Location).Rotation());
        WideCamera->GetCameraComponent()->SetFieldOfView(55.0f);
    }
    if (CloseCamera)
    {
        CloseCamera->GetCameraComponent()->SetFieldOfView(52.0f);
    }

    if (APlayerController* PlayerController = World->GetFirstPlayerController())
    {
        PlayerController->ConsoleCommand(TEXT("r.VSync 0"), true);
        PlayerController->ConsoleCommand(TEXT("t.MaxFPS 0"), true);
        PlayerController->ConsoleCommand(TEXT("r.MotionBlurQuality 0"), true);
        PlayerController->ConsoleCommand(TEXT("r.ScreenPercentage 100"), true);
        PlayerController->ConsoleCommand(TEXT("DisableAllScreenMessages"), true);
        if (bFreeObservation)
        {
            if (APawn* ObservationPawn = PlayerController->GetPawn())
            {
                const float ObservationDistance = FMath::Max(5000.0f, ViewExtent * 0.65f);
                const FVector ObservationLocation(
                    0.0,
                    -ObservationDistance,
                    FMath::Max(3000.0f, ViewExtent * 0.35f));
                const FRotator ObservationRotation =
                    (FVector(0.0, 0.0, 200.0) - ObservationLocation).Rotation();
                ObservationPawn->SetActorLocationAndRotation(ObservationLocation, ObservationRotation);
                PlayerController->SetControlRotation(ObservationRotation);
                PlayerController->SetViewTarget(ObservationPawn);
                PlayerController->ResetIgnoreLookInput();
                PlayerController->ResetIgnoreMoveInput();

                if (ASpectatorPawn* SpectatorPawn = Cast<ASpectatorPawn>(ObservationPawn))
                {
                    if (USpectatorPawnMovement* Movement = Cast<USpectatorPawnMovement>(SpectatorPawn->GetMovementComponent()))
                    {
                        Movement->MaxSpeed = FreeObservationSpeed;
                        Movement->Acceleration = FreeObservationSpeed * 4.0f;
                        Movement->Deceleration = FreeObservationSpeed * 6.0f;
                    }
                }

                UE_LOG(LogTemp, Display,
                    TEXT("MBST_FREE_OBSERVATION_READY: view_target=%s speed=%.0f; fixed benchmark cameras, screenshots and auto-exit are disabled."),
                    *ObservationPawn->GetName(),
                    FreeObservationSpeed);
            }
            else
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("MBST_FREE_OBSERVATION: PlayerController has no pawn; the benchmark camera was not forced."));
            }
        }
        else if (WideCamera)
        {
            PlayerController->SetViewTarget(WideCamera);
        }
    }
}

bool AMBSTSingleTurretBenchmarkActor::LoadScenarioAssets()
{
    SingleTurretConfig = LoadObject<UMassBattleAgentConfigDataAsset>(
        nullptr,
        TEXT("/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret_AgentConfig.Tank_SingleTurret_AgentConfig"));
    if (!SingleTurretConfig)
    {
        return false;
    }

    if (Scenario == EMBSTBenchmarkScenario::LegacyCompound)
    {
        LegacyTankClass = StaticLoadClass(
            AActor::StaticClass(),
            nullptr,
            TEXT("/MassBattle/Test/CompoundUnitAsset/BP_TankActor.BP_TankActor_C"));
        return LegacyTankClass != nullptr;
    }
    return true;
}

bool AMBSTSingleTurretBenchmarkActor::SpawnMovingTarget()
{
    UWorld* World = GetWorld();
    if (!World || !SingleTurretConfig)
    {
        return false;
    }

    const FVector TargetLocation = GetMovingTargetLocation();
    const FEntityTemplateData TargetTemplate = MBSTBenchmark::MakeMassTemplate(
        this,
        SingleTurretConfig,
        false,
        false);
    if (!TargetTemplate.IsValid())
    {
        return false;
    }

    FAgentSpawnRectangleShapeData Shape;
    Shape.Region = FVector2D::ZeroVector;
    Shape.Spacing = FVector2D(1.0f, 1.0f);
    Shape.PositioningMode = ESpawnPositioningMode::Sequential;
    const TArray<FEntityHandle> Targets = UMassBattleFuncLib::SpawnAgentsByTemplateRectangular(
        this,
        TargetTemplate,
        1,
        2,
        TargetLocation,
        Shape,
        FVector2D::ZeroVector,
        EInitialRotation::CustomRotation,
        FRotator::ZeroRotator,
        FSpawnerMult(),
        true);
    if (Targets.Num() != 1)
    {
        return false;
    }
    MovingTargetEntity = Targets[0];
    ConfigureMovingTargetEntity(MovingTargetEntity);

    if (UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder")))
    {
        MovingTargetVisual = World->SpawnActor<AStaticMeshActor>(TargetLocation, FRotator::ZeroRotator);
        if (MovingTargetVisual)
        {
            MovingTargetVisual->SetActorLabel(TEXT("MBST_HighHealthMovingSandbag"));
            MovingTargetVisual->GetStaticMeshComponent()->SetStaticMesh(Cylinder);
            MovingTargetVisual->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
            MovingTargetVisual->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            MovingTargetVisual->SetActorScale3D(FVector(2.5, 2.5, 4.0));
        }
    }

    return MovingTargetEntity.IsSet() && MovingTargetVisual != nullptr;
}

void AMBSTSingleTurretBenchmarkActor::ConfigureMovingTargetEntity(const FEntityHandle& EntityHandle)
{
    UMassAPISubsystem* MassAPI = UMassAPISubsystem::GetPtr(this);
    if (!MassAPI || !EntityHandle.IsSet() || !MassAPI->IsValid(EntityHandle))
    {
        return;
    }

    if (FHealth* Health = MassAPI->GetFragmentPtr<FHealth>(EntityHandle))
    {
        Health->Current = TargetHealth;
        Health->Maximum = TargetHealth;
        Health->bLockHealth = true;
    }
    if (FVisualize* Visualize = MassAPI->GetFragmentPtr<FVisualize>(EntityHandle))
    {
        Visualize->bEnable = false;
    }
    if (FMove* Move = MassAPI->GetFragmentPtr<FMove>(EntityHandle))
    {
        Move->bEnable = false;
        Move->Z.bEnable = false;
    }
    if (FAttack* Attack = MassAPI->GetFragmentPtr<FAttack>(EntityHandle))
    {
        Attack->bEnable = false;
    }
    if (FTrace* Trace = MassAPI->GetFragmentPtr<FTrace>(EntityHandle))
    {
        Trace->bEnable = false;
    }
}

void AMBSTSingleTurretBenchmarkActor::UpdateMovingTarget()
{
    const FVector TargetLocation = GetMovingTargetLocation();
    if (MovingTargetVisual)
    {
        MovingTargetVisual->SetActorLocation(TargetLocation);
    }

    if (UMassAPISubsystem* MassAPI = UMassAPISubsystem::GetPtr(this))
    {
        if (MovingTargetEntity.IsSet() && MassAPI->IsValid(MovingTargetEntity))
        {
            if (FLocating* Location = MassAPI->GetFragmentPtr<FLocating>(MovingTargetEntity))
            {
                Location->PreLocation = Location->Location;
                Location->Location = TargetLocation;
            }
            if (FHealth* Health = MassAPI->GetFragmentPtr<FHealth>(MovingTargetEntity))
            {
                Health->Current = TargetHealth;
                Health->Maximum = TargetHealth;
                Health->bLockHealth = true;
            }
        }
    }
}

void AMBSTSingleTurretBenchmarkActor::BeginSpawning()
{
    Phase = EMBSTBenchmarkPhase::Spawning;
    PhaseElapsedSeconds = 0.0f;
    SpawnedTankCount = 0;
    SpawnedUnitEntityCount = 0;

    if (Scenario == EMBSTBenchmarkScenario::MassBaseline)
    {
        SpawnMassScenario(false);
    }
    else if (Scenario == EMBSTBenchmarkScenario::SingleTurret)
    {
        SpawnMassScenario(true);
    }
}

void AMBSTSingleTurretBenchmarkActor::SpawnMassScenario(const bool bSingleTurret)
{
    const FEntityTemplateData Template = MBSTBenchmark::MakeMassTemplate(
        this,
        SingleTurretConfig,
        bSingleTurret,
        true);
    if (!Template.IsValid())
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

    const TArray<FEntityHandle> Handles = UMassBattleFuncLib::SpawnAgentsByTemplateRectangular(
        this,
        Template,
        UnitCount,
        1,
        FVector::ZeroVector,
        Shape,
        FVector2D::ZeroVector,
        EInitialRotation::CustomRotation,
        FRotator::ZeroRotator,
        FSpawnerMult(),
        true);

    for (const FEntityHandle& Handle : Handles)
    {
        ConfigureControlledEntity(Handle, false, !bSingleTurret, bSingleTurret, true, 1);
    }
    SpawnedTankCount = Handles.Num();
    SpawnedUnitEntityCount = Handles.Num();

    if (Handles.Num() == UnitCount)
    {
        EnterWarmup();
    }
    else
    {
        Phase = EMBSTBenchmarkPhase::Failed;
        UE_LOG(LogTemp, Error, TEXT("MBST_BENCHMARK_FAILED: Mass spawn count was %d/%d."), Handles.Num(), UnitCount);
    }
}

void AMBSTSingleTurretBenchmarkActor::SpawnLegacyBatch()
{
    int32 RemainingThisFrame = LegacyActorsPerFrame;
    while (RemainingThisFrame-- > 0 && SpawnedTankCount < UnitCount)
    {
        if (!SpawnOneLegacyTank(SpawnedTankCount))
        {
            Phase = EMBSTBenchmarkPhase::Failed;
            UE_LOG(LogTemp, Error, TEXT("MBST_BENCHMARK_FAILED: original tank Actor %d failed to spawn."), SpawnedTankCount);
            return;
        }
        ++SpawnedTankCount;
    }

    if (SpawnedTankCount == UnitCount)
    {
        EnterWarmup();
    }
}

bool AMBSTSingleTurretBenchmarkActor::SpawnOneLegacyTank(const int32 TankIndex)
{
    UWorld* World = GetWorld();
    if (!World || !LegacyTankClass)
    {
        return false;
    }

    const FTransform SpawnTransform(FRotator::ZeroRotator, GetFormationPosition(TankIndex));
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

    TInlineComponentArray<UMassBattleAgentComponent*> AgentComponents(Tank);
    int32 ValidEntityCount = 0;
    UMassBattleAgentComponent* TurretMarker = nullptr;
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
        Component->TeamIndex = 1;
        const FName ComponentName = Component->GetFName();
        const bool bTurret = ComponentName == TEXT("Turret");
        if (bTurret)
        {
            TurretMarker = Component;
        }
        // Preserve the source AgentConfig setting: all four logical Mass
        // entities are invisible.  The Actor path below uses the BP's own two
        // StaticMeshComponents, avoiding synthetic Mass renderer instances.
        ConfigureControlledEntity(Handle, bTurret, false, false, false, 1);
        ++ValidEntityCount;
    }

    TInlineComponentArray<UStaticMeshComponent*> StaticMeshComponents(Tank);
    int32 VisiblePreviewPartCount = 0;
    for (UStaticMeshComponent* MeshComponent : StaticMeshComponents)
    {
        if (!MeshComponent)
        {
            continue;
        }
        const FName ComponentName = MeshComponent->GetFName();
        if (ComponentName != TEXT("PreviewMeshVehicle") && ComponentName != TEXT("PreviewMeshTurret"))
        {
            continue;
        }

        MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        MeshComponent->SetVisibility(true, true);
        MeshComponent->SetHiddenInGame(false, true);
        if (ComponentName == TEXT("PreviewMeshTurret") && TurretMarker)
        {
            // The source preview mesh is outside the logical marker hierarchy.
            // Attach once so the original EntityToComponent turret sync drives
            // the Actor's visible turret without a second per-Actor aim loop.
            MeshComponent->AttachToComponent(TurretMarker, FAttachmentTransformRules::KeepWorldTransform);
        }
        ++VisiblePreviewPartCount;
    }

    SpawnedUnitEntityCount += ValidEntityCount;
    return ValidEntityCount == 4 && VisiblePreviewPartCount == 2 && TurretMarker != nullptr;
}

void AMBSTSingleTurretBenchmarkActor::ConfigureControlledEntity(
    const FEntityHandle& EntityHandle,
    const bool bLegacyTurret,
    const bool bMassBaseline,
    const bool bSingleTurret,
    const bool bEnableVisualization,
    const int32 ForcedTeamIndex)
{
    UMassAPISubsystem* MassAPI = UMassAPISubsystem::GetPtr(this);
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
    if (FVisualize* Visualize = MassAPI->GetFragmentPtr<FVisualize>(EntityHandle))
    {
        Visualize->bEnable = bEnableVisualization;
    }
    if (FMove* Move = MassAPI->GetFragmentPtr<FMove>(EntityHandle))
    {
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

    if (bLegacyTurret && !MassAPI->HasTag<FMBSTBenchmarkLegacyTurretTag>(EntityHandle))
    {
        MassAPI->AddTag<FMBSTBenchmarkLegacyTurretTag>(EntityHandle);
    }
    if (bMassBaseline && !MassAPI->HasTag<FMBSTBenchmarkMassBaselineTag>(EntityHandle))
    {
        MassAPI->AddTag<FMBSTBenchmarkMassBaselineTag>(EntityHandle);
    }
    if (bSingleTurret && !MassAPI->HasTag<FMBSTBenchmarkSingleTurretTag>(EntityHandle))
    {
        MassAPI->AddTag<FMBSTBenchmarkSingleTurretTag>(EntityHandle);
    }
}

void AMBSTSingleTurretBenchmarkActor::EnterWarmup()
{
    Phase = EMBSTBenchmarkPhase::Warmup;
    PhaseElapsedSeconds = 0.0f;
    FrameTimeSamplesMilliseconds.Reset();
    GameThreadSamplesMilliseconds.Reset();
    RenderThreadSamplesMilliseconds.Reset();
    GPUSamplesMilliseconds.Reset();
    SetCloseCamera(false);
    UE_LOG(LogTemp, Display,
        TEXT("MBST_BENCHMARK_READY: scenario=%s tanks=%d unit_entities=%d target_health=%.0f target_radius=%.0f."),
        *GetScenarioLabel(), SpawnedTankCount, SpawnedUnitEntityCount, TargetHealth, TargetOrbitRadius);
}

void AMBSTSingleTurretBenchmarkActor::EnterSampling()
{
    Phase = EMBSTBenchmarkPhase::Sampling;
    PhaseElapsedSeconds = 0.0f;
    FrameTimeSamplesMilliseconds.Reset();
    GameThreadSamplesMilliseconds.Reset();
    RenderThreadSamplesMilliseconds.Reset();
    GPUSamplesMilliseconds.Reset();
    const int32 ReserveCount = FMath::CeilToInt(SampleSeconds * 240.0f);
    FrameTimeSamplesMilliseconds.Reserve(ReserveCount);
    GameThreadSamplesMilliseconds.Reserve(ReserveCount);
    RenderThreadSamplesMilliseconds.Reserve(ReserveCount);
    GPUSamplesMilliseconds.Reserve(ReserveCount);
    SetCloseCamera(false);
    UE_LOG(LogTemp, Display, TEXT("MBST_BENCHMARK_SAMPLE_BEGIN: %s"), *GetScenarioLabel());
}

void AMBSTSingleTurretBenchmarkActor::FinishSampling()
{
    if (FrameTimeSamplesMilliseconds.IsEmpty())
    {
        Phase = EMBSTBenchmarkPhase::Failed;
        return;
    }

    const MBSTBenchmark::FTimingStats FrameStats = MBSTBenchmark::CalculateTimingStats(FrameTimeSamplesMilliseconds);
    const MBSTBenchmark::FTimingStats GameStats = MBSTBenchmark::CalculateTimingStats(GameThreadSamplesMilliseconds);
    const MBSTBenchmark::FTimingStats RenderStats = MBSTBenchmark::CalculateTimingStats(RenderThreadSamplesMilliseconds);
    const MBSTBenchmark::FTimingStats GPUStats = MBSTBenchmark::CalculateTimingStats(GPUSamplesMilliseconds);

    AverageFrameMilliseconds = FrameStats.Average;
    MedianFrameMilliseconds = FrameStats.P50;
    P95FrameMilliseconds = FrameStats.P95;
    P99FrameMilliseconds = FrameStats.P99;
    MaximumFrameMilliseconds = FrameStats.Maximum;
    AverageGameThreadMilliseconds = GameStats.Average;
    AverageRenderThreadMilliseconds = RenderStats.Average;
    AverageGPUMilliseconds = GPUStats.Average;

    Phase = EMBSTBenchmarkPhase::Complete;
    PhaseElapsedSeconds = 0.0f;
    CompletionElapsedSeconds = 0.0f;
    WriteResultJson();
    UE_LOG(LogTemp, Display,
        TEXT("MBST_BENCHMARK_RESULT: scenario=%s tanks=%d entities=%d samples=%d frame_avg_ms=%.4f fps=%.2f game_avg_ms=%.4f render_avg_ms=%.4f gpu_avg_ms=%.4f p95_frame_ms=%.4f result=%s"),
        *GetScenarioLabel(),
        SpawnedTankCount,
        SpawnedUnitEntityCount,
        FrameStats.Count,
        FrameStats.Average,
        FrameStats.Average > 0.0f ? 1000.0f / FrameStats.Average : 0.0f,
        GameStats.Average,
        RenderStats.Average,
        GPUStats.Average,
        FrameStats.P95,
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

    UpdateMovingTarget();
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
        if (!bRenderDiagnosticsLogged && PhaseElapsedSeconds >= 2.0f)
        {
            bRenderDiagnosticsLogged = true;
            if (UMassBattleSubsystem* BattleSubsystem = UMassBattleSubsystem::GetPtr(this))
            {
                int32 TotalBatches = 0;
                int32 TotalInstances = 0;
                for (const TPair<int32, TObjectPtr<AMassBattleAgentRenderer>>& Pair : BattleSubsystem->AgentRenderers)
                {
                    if (const AMassBattleAgentRenderer* Renderer = Pair.Value.Get())
                    {
                        TotalBatches += Renderer->SpawnedRenderBatches.Num();
                        for (const TPair<int32, FAgentRenderBatchData>& BatchPair : Renderer->SpawnedRenderBatches)
                        {
                            TotalInstances += BatchPair.Value.LocationArray.Num();
                        }
                    }
                }
                UE_LOG(LogTemp, Display,
                    TEXT("MBST_RENDER_DIAGNOSTIC: renderers=%d batches=%d instances=%d"),
                    BattleSubsystem->AgentRenderers.Num(), TotalBatches, TotalInstances);
            }
        }

        if (!bSkipScreenshots && PendingScreenshotFrames == 0 && !bWideCaptured && PhaseElapsedSeconds >= 2.0f)
        {
            CaptureScreenshot(TEXT("TrackingWide"), false, true);
            bWideCaptured = true;
        }
        else if (!bSkipScreenshots && PendingScreenshotFrames == 0 && bWideCaptured && !bCloseCaptured && PhaseElapsedSeconds >= 4.0f)
        {
            CaptureScreenshot(TEXT("TrackingClose"), true, true);
            bCloseCaptured = true;
        }

        const bool bVisualProofComplete = bSkipScreenshots
            || (bWideCaptured && bCloseCaptured && PendingScreenshotFrames == 0);
        const float RequiredWarmup = bSkipScreenshots ? WarmupSeconds : FMath::Max(WarmupSeconds, 5.0f);
        if (PhaseElapsedSeconds >= RequiredWarmup && bVisualProofComplete)
        {
            EnterSampling();
        }
        break;
    }

    case EMBSTBenchmarkPhase::Sampling:
    {
        FrameTimeSamplesMilliseconds.Add(FrameWallSeconds * 1000.0f);
        GameThreadSamplesMilliseconds.Add(static_cast<float>(FPlatformTime::ToMilliseconds(GGameThreadTime)));
        RenderThreadSamplesMilliseconds.Add(static_cast<float>(FPlatformTime::ToMilliseconds(GRenderThreadTime)));
        const float GPUTimeMilliseconds = static_cast<float>(FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles()));
        if (GPUTimeMilliseconds > 0.0f && FMath::IsFinite(GPUTimeMilliseconds))
        {
            GPUSamplesMilliseconds.Add(GPUTimeMilliseconds);
        }
        if (PhaseElapsedSeconds >= SampleSeconds)
        {
            FinishSampling();
        }
        break;
    }

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
    const FVector Target = GetMovingTargetLocation();
    return static_cast<float>(FMath::RadiansToDegrees(FMath::Atan2(Target.Y, Target.X)));
}

FVector AMBSTSingleTurretBenchmarkActor::GetMovingTargetLocation() const
{
    return MBSTBenchmark::CalculateTargetLocation(FPlatformTime::Seconds());
}

FString AMBSTSingleTurretBenchmarkActor::GetScenarioLabel() const
{
    switch (Scenario)
    {
    case EMBSTBenchmarkScenario::LegacyCompound: return TEXT("ORIGINAL BP_TANKACTOR (2 MESH COMPONENTS + 4 MASS ENTITIES)");
    case EMBSTBenchmarkScenario::MassBaseline: return TEXT("ORDINARY ONE-ENTITY MASS TANK (WHOLE-BODY AIM)");
    case EMBSTBenchmarkScenario::SingleTurret: return TEXT("PLUGIN ONE-ENTITY MASS TANK (GPU TURRET AIM)");
    default: return TEXT("UNKNOWN");
    }
}

FString AMBSTSingleTurretBenchmarkActor::GetScenarioToken() const
{
    switch (Scenario)
    {
    case EMBSTBenchmarkScenario::LegacyCompound: return TEXT("Actor");
    case EMBSTBenchmarkScenario::MassBaseline: return TEXT("Mass");
    case EMBSTBenchmarkScenario::SingleTurret: return TEXT("TurretMass");
    default: return TEXT("Unknown");
    }
}

FString AMBSTSingleTurretBenchmarkActor::GetPhaseLabel() const
{
    switch (Phase)
    {
    case EMBSTBenchmarkPhase::Waiting: return TEXT("WAITING");
    case EMBSTBenchmarkPhase::Spawning: return TEXT("SPAWNING");
    case EMBSTBenchmarkPhase::Warmup: return TEXT("WARMUP / TRACKING PROOF");
    case EMBSTBenchmarkPhase::Sampling: return TEXT("NATIVE ENGINE TIMING SAMPLE");
    case EMBSTBenchmarkPhase::Complete: return TEXT("COMPLETE");
    case EMBSTBenchmarkPhase::Failed: return TEXT("FAILED");
    default: return TEXT("UNKNOWN");
    }
}

FVector AMBSTSingleTurretBenchmarkActor::GetFormationPosition(const int32 TankIndex) const
{
    const int32 DepthIndex = TankIndex / FormationWidth;
    const int32 WidthIndex = TankIndex % FormationWidth;
    const float LocalX = (static_cast<float>(DepthIndex) - static_cast<float>(FormationDepth - 1) * 0.5f) * FormationSpacing;
    const float LocalY = (static_cast<float>(WidthIndex) - static_cast<float>(FormationWidth - 1) * 0.5f) * FormationSpacing;
    return FVector(LocalX, LocalY, 0.0f);
}

float AMBSTSingleTurretBenchmarkActor::GetPercentile(const TArray<float>& SortedValues, const float Alpha) const
{
    return MBSTBenchmark::Percentile(SortedValues, Alpha);
}

FString AMBSTSingleTurretBenchmarkActor::GetOutputDirectory() const
{
    if (!OutputDirectoryOverride.IsEmpty())
    {
        return FPaths::ConvertRelativePathToFull(OutputDirectoryOverride);
    }
    return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MassBattleSingleTurret/NativeTrackingBenchmark"));
}

void AMBSTSingleTurretBenchmarkActor::WriteResultJson()
{
    const FString OutputDirectory = GetOutputDirectory();
    IFileManager::Get().MakeDirectory(*OutputDirectory, true);
    ResultFilePath = OutputDirectory / FString::Printf(TEXT("%s_%d_units.json"), *GetScenarioToken(), UnitCount);

    const MBSTBenchmark::FTimingStats FrameStats = MBSTBenchmark::CalculateTimingStats(FrameTimeSamplesMilliseconds);
    const MBSTBenchmark::FTimingStats GameStats = MBSTBenchmark::CalculateTimingStats(GameThreadSamplesMilliseconds);
    const MBSTBenchmark::FTimingStats RenderStats = MBSTBenchmark::CalculateTimingStats(RenderThreadSamplesMilliseconds);
    const MBSTBenchmark::FTimingStats GPUStats = MBSTBenchmark::CalculateTimingStats(GPUSamplesMilliseconds);

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("benchmark_schema_version"), 2);
    Root->SetStringField(TEXT("scenario"), GetScenarioLabel());
    Root->SetStringField(TEXT("scenario_token"), GetScenarioToken());
    Root->SetStringField(TEXT("comparison_mode"), TEXT("native_orbiting_target_tracking"));
    Root->SetStringField(TEXT("timing_source"), TEXT("UE GGameThreadTime + GRenderThreadTime + RHIGetGPUFrameCycles + platform wall frame interval"));
    Root->SetStringField(TEXT("unit_update_path"), TEXT("native Mass processor; no Blueprint Tick and no Python timing"));
    Root->SetBoolField(TEXT("separate_process_per_scenario"), true);
    Root->SetBoolField(TEXT("source_weapon_logic_disabled"), true);
    Root->SetStringField(TEXT("reason_weapon_logic_disabled"), TEXT("isolates representation, tracking processor and rendering cost from projectile/FX variance"));
    Root->SetNumberField(TEXT("requested_units"), UnitCount);
    Root->SetNumberField(TEXT("spawned_tanks"), SpawnedTankCount);
    Root->SetNumberField(TEXT("spawned_unit_entities"), SpawnedUnitEntityCount);
    Root->SetNumberField(TEXT("spawned_actors"), Scenario == EMBSTBenchmarkScenario::LegacyCompound ? SpawnedTankCount : 0);
    Root->SetNumberField(TEXT("mass_entities_per_tank"), Scenario == EMBSTBenchmarkScenario::LegacyCompound ? 4 : 1);
    Root->SetNumberField(TEXT("warmup_seconds"), WarmupSeconds);
    Root->SetNumberField(TEXT("requested_sample_seconds"), SampleSeconds);
    Root->SetNumberField(TEXT("average_fps"), FrameStats.Average > 0.0f ? 1000.0f / FrameStats.Average : 0.0f);
    Root->SetBoolField(TEXT("gpu_timing_available"), GPUStats.Count > 0);

    TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
    Target->SetStringField(TEXT("kind"), TEXT("one hidden Mass entity plus one native StaticMeshActor visual proxy"));
    Target->SetNumberField(TEXT("team"), 2);
    Target->SetNumberField(TEXT("health"), TargetHealth);
    Target->SetBoolField(TEXT("health_locked"), true);
    Target->SetNumberField(TEXT("orbit_radius"), TargetOrbitRadius);
    Target->SetNumberField(TEXT("orbit_period_seconds"), TargetOrbitPeriodSeconds);
    Target->SetNumberField(TEXT("height"), TargetHeight);
    Target->SetBoolField(TEXT("fixed_bearing"), bUseFixedTargetBearing);
    if (bUseFixedTargetBearing)
    {
        Target->SetNumberField(TEXT("fixed_bearing_degrees"), FixedTargetBearingDegrees);
    }
    Root->SetObjectField(TEXT("moving_target"), Target);

    TSharedRef<FJsonObject> Timings = MakeShared<FJsonObject>();
    Timings->SetObjectField(TEXT("frame_wall_ms"), MBSTBenchmark::TimingStatsToJson(FrameStats));
    Timings->SetObjectField(TEXT("game_thread_ms"), MBSTBenchmark::TimingStatsToJson(GameStats));
    Timings->SetObjectField(TEXT("render_thread_ms"), MBSTBenchmark::TimingStatsToJson(RenderStats));
    Timings->SetObjectField(TEXT("gpu_frame_ms"), MBSTBenchmark::TimingStatsToJson(GPUStats));
    Root->SetObjectField(TEXT("timings"), Timings);

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
    if (bFreeObservation)
    {
        return;
    }

    if (bCloseView && CloseCamera)
    {
        const FVector Target = GetMovingTargetLocation();
        if (bTopDownAimProof)
        {
            const FVector Focus = Target * 0.5f + FVector(0.0, 0.0, 120.0);
            const float Height = FMath::Max(2000.0f, TargetOrbitRadius * 1.5f);
            const FVector Location = Focus + FVector(0.0, 0.0, Height);
            CloseCamera->SetActorLocationAndRotation(Location, (Focus - Location).Rotation());
            CloseCamera->GetCameraComponent()->SetFieldOfView(45.0f);
        }
        else
        {
            const FVector Focus = Target * 0.42f + FVector(0.0, 0.0, 180.0);
            const float Distance = UnitCount <= 32 ? 4800.0f : 8500.0f;
            const FVector Location = Focus + FVector(-Distance * 0.55f, -Distance, Distance * 0.55f);
            CloseCamera->SetActorLocationAndRotation(Location, (Focus - Location).Rotation());
        }
    }

    if (APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
    {
        ACameraActor* TargetCamera = bCloseView ? CloseCamera.Get() : WideCamera.Get();
        if (TargetCamera)
        {
            PlayerController->SetViewTarget(TargetCamera);
        }
    }
}

void AMBSTSingleTurretBenchmarkActor::CaptureScreenshot(
    const FString& Suffix,
    const bool bCloseView,
    const bool bVisualAimLines)
{
    SetCloseCamera(bCloseView);
    const FString OutputDirectory = GetOutputDirectory();
    IFileManager::Get().MakeDirectory(*OutputDirectory, true);
    PendingScreenshotFilename = OutputDirectory / FString::Printf(
        TEXT("%s_%d_units_%s.png"), *GetScenarioToken(), UnitCount, *Suffix);
    bPendingVisualAimProof = bVisualAimLines;
    PendingScreenshotFrames = 3;
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
    if (bPendingVisualAimProof)
    {
        DrawVisualAimProof();
    }
    FScreenshotRequest::RequestScreenshot(PendingScreenshotFilename, true, false);
    UE_LOG(LogTemp, Display, TEXT("MBST_BENCHMARK_SCREENSHOT: %s"), *PendingScreenshotFilename);
    PendingScreenshotFilename.Reset();
    bPendingVisualAimProof = false;
}

void AMBSTSingleTurretBenchmarkActor::DrawVisualAimProof() const
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    const FVector Target = GetMovingTargetLocation();
    const float ProofSphereRadius = bTopDownAimProof ? 160.0f : 420.0f;
    DrawDebugSphere(World, Target, ProofSphereRadius, 24, FColor::Yellow, false, 0.35f, 0, 8.0f);
    const FString TargetLabel = bUseFixedTargetBearing
        ? FString::Printf(TEXT("FIXED MASS TARGET %+0.0f DEG | HP %.0f (LOCKED)"), FixedTargetBearingDegrees, TargetHealth)
        : FString::Printf(TEXT("MOVING MASS TARGET | HP %.0f (LOCKED)"), TargetHealth);
    DrawDebugString(
        World,
        Target + FVector(0.0, 0.0, 650.0),
        TargetLabel,
        nullptr,
        FColor::Yellow,
        0.35f,
        true,
        1.2f);

    const int32 LineCount = FMath::Min(24, UnitCount);
    const int32 Step = FMath::Max(1, UnitCount / LineCount);
    for (int32 TankIndex = 0; TankIndex < UnitCount; TankIndex += Step)
    {
        const float ProofLineHeight = bTopDownAimProof ? 600.0f : 300.0f;
        const FVector Start = GetFormationPosition(TankIndex) + FVector(0.0, 0.0, ProofLineHeight);
        const FVector TargetAtLineHeight(Target.X, Target.Y, ProofLineHeight);
        DrawDebugDirectionalArrow(
            World,
            Start,
            TargetAtLineHeight,
            bTopDownAimProof ? 140.0f : 40.0f,
            FColor(255, 50, 20),
            false,
            0.35f,
            0,
            bTopDownAimProof ? 7.0f : 1.5f);
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

    DrawRect(FLinearColor(0.015f, 0.02f, 0.03f, 0.88f), 14.0f * Scale, 14.0f * Scale, 930.0f * Scale, 305.0f * Scale);
    DrawText(TEXT("MassBattle Native Moving-Target Benchmark"), FLinearColor(1.0f, 0.82f, 0.16f), X, Y, Font, 1.15f * Scale, false);
    Y += Line * 1.35f;
    DrawText(FString::Printf(TEXT("Scenario: %s"), *Benchmark->GetScenarioLabel()), FLinearColor::White, X, Y, Font, Scale, false);
    Y += Line;
    DrawText(FString::Printf(TEXT("Requested: %d tanks | Spawned: %d tanks / %d unit entities"),
        Benchmark->UnitCount, Benchmark->SpawnedTankCount, Benchmark->SpawnedUnitEntityCount),
        FLinearColor(0.72f, 0.9f, 1.0f), X, Y, SmallFont, Scale, false);
    Y += Line;
    const FString TargetDescription = Benchmark->bUseFixedTargetBearing
        ? FString::Printf(TEXT("Locked Mass target: HP %.0f | FIXED bearing %+06.1f deg | R %.0f"),
            Benchmark->TargetHealth, Benchmark->FixedTargetBearingDegrees, Benchmark->TargetOrbitRadius)
        : FString::Printf(TEXT("Locked Mass target: HP %.0f | orbit R %.0f, period %.1f s | bearing %+06.1f deg"),
            Benchmark->TargetHealth, Benchmark->TargetOrbitRadius, Benchmark->TargetOrbitPeriodSeconds, Benchmark->GetLiveSweepYawDegrees());
    DrawText(TargetDescription,
        FLinearColor(0.55f, 1.0f, 0.58f), X, Y, SmallFont, Scale, false);
    Y += Line;
    if (Benchmark->bFreeObservation)
    {
        DrawText(TEXT("FREE OBSERVE: click viewport | mouse look | WASD fly | no forced camera / auto-exit"),
            FLinearColor(0.35f, 0.95f, 1.0f), X, Y, SmallFont, Scale, false);
        Y += Line;
    }
    DrawText(FString::Printf(TEXT("Phase: %s"), *Benchmark->GetPhaseLabel()), FLinearColor(1.0f, 0.72f, 0.36f), X, Y, SmallFont, Scale, false);
    Y += Line;

    if (Benchmark->Phase == EMBSTBenchmarkPhase::Complete)
    {
        DrawText(FString::Printf(TEXT("Frame %.3f ms (%.1f FPS) | Game %.3f | Render %.3f | GPU %.3f ms"),
            Benchmark->AverageFrameMilliseconds,
            Benchmark->AverageFrameMilliseconds > 0.0f ? 1000.0f / Benchmark->AverageFrameMilliseconds : 0.0f,
            Benchmark->AverageGameThreadMilliseconds,
            Benchmark->AverageRenderThreadMilliseconds,
            Benchmark->AverageGPUMilliseconds),
            FLinearColor(0.2f, 1.0f, 0.4f), X, Y, SmallFont, Scale, false);
        Y += Line;
        DrawText(FString::Printf(TEXT("Frame P50 %.3f | P95 %.3f | P99 %.3f | Max %.3f ms"),
            Benchmark->MedianFrameMilliseconds,
            Benchmark->P95FrameMilliseconds,
            Benchmark->P99FrameMilliseconds,
            Benchmark->MaximumFrameMilliseconds),
            FLinearColor(0.2f, 1.0f, 0.4f), X, Y, SmallFont, Scale, false);
    }
    else
    {
        DrawText(TEXT("Native Mass tracking processor; UE thread/RHI counters; weapon/projectile/FX logic disabled equally."),
            FLinearColor(0.82f, 0.82f, 0.82f), X, Y, SmallFont, Scale, false);
    }
}

AMBSTSingleTurretBenchmarkGameMode::AMBSTSingleTurretBenchmarkGameMode()
{
    HUDClass = AMBSTSingleTurretBenchmarkHUD::StaticClass();
    DefaultPawnClass = ASpectatorPawn::StaticClass();
}
