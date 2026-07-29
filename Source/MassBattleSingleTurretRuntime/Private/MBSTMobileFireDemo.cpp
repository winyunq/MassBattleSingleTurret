#include "MBSTMobileFireDemo.h"

#include "MBSTMobileFireProfile.h"
#include "MBSTMobileFireSubsystem.h"
#include "MBSTSingleTurretBlueprintLibrary.h"
#include "DataAssets/MassBattleAgentConfigDataAsset.h"
#include "Components/LightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Fragments/Attack.h"
#include "Fragments/Chase.h"
#include "Fragments/Health.h"
#include "Fragments/Move.h"
#include "Fragments/Render.h"
#include "Fragments/Team.h"
#include "Fragments/Trace.h"
#include "Fragments/Transform.h"
#include "FuncLibs/MassBattleFuncLib.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpectatorPawn.h"
#include "GameFramework/SpectatorPawnMovement.h"
#include "HAL/PlatformMisc.h"
#include "Kismet/GameplayStatics.h"
#include "MassAPISubsystem.h"
#include "MassEntityTemplate.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Tasks/MassBattleBPTaskAgentsMoveTo.h"

namespace MBSTMobileFireDemoPrivate
{
    static constexpr TCHAR BaseConfigPath[] =
        TEXT("/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret_AgentConfig.Tank_SingleTurret_AgentConfig");
    static constexpr TCHAR StopProfilePath[] =
        TEXT("/MassBattleSingleTurret/Demo/MobileFire/DA_MBST_StopToFire.DA_MBST_StopToFire");
    static constexpr TCHAR MoveProfilePath[] =
        TEXT("/MassBattleSingleTurret/Demo/MobileFire/DA_MBST_FireWhileMoving.DA_MBST_FireWhileMoving");
    static constexpr TCHAR StopConfigPath[] =
        TEXT("/MassBattleSingleTurret/Demo/MobileFire/Tank_StopToFire_AgentConfig.Tank_StopToFire_AgentConfig");
    static constexpr TCHAR MoveConfigPath[] =
        TEXT("/MassBattleSingleTurret/Demo/MobileFire/Tank_FireWhileMoving_AgentConfig.Tank_FireWhileMoving_AgentConfig");

    static FEntityTemplateData MakeTargetTemplate(
        const UObject* WorldContextObject,
        const UMassBattleAgentConfigDataAsset* Config)
    {
        const FEntityTemplateData Source = UMassBattleFuncLib::MakeTemplateDataFromDataAsset(
            WorldContextObject,
            Config);
        if (!Source.IsValid() || !Source.Get())
        {
            return FEntityTemplateData();
        }

        FMassEntityTemplateData Cloned = UMassAPISubsystem::CloneTemplate(*Source.Get());
        UMassAPISubsystem::RemoveTag<FMBSTSingleTurretTag>(Cloned);
        UMassAPISubsystem::RemoveTag<FMBSTMobileFireTag>(Cloned);
        UMassAPISubsystem::RemoveFragment<FMBSTSingleTurretState>(Cloned);
        UMassAPISubsystem::RemoveFragment<FMBSTMobileFireState>(Cloned);
        UMassAPISubsystem::RemoveSharedFragment<FMBSTSingleTurretShared>(Cloned);
        UMassAPISubsystem::RemoveSharedFragment<FMBSTMobileFireShared>(Cloned);
        return FEntityTemplateData(MakeShared<FMassEntityTemplateData>(MoveTemp(Cloned)));
    }
}

AMBSTMobileFireDemoActor::AMBSTMobileFireDemoActor()
{
    PrimaryActorTick.bCanEverTick = true;
}

void AMBSTMobileFireDemoActor::BeginPlay()
{
    Super::BeginPlay();
    bAutoExit = FParse::Param(FCommandLine::Get(), TEXT("MBSTMobileFireAutoExit"));
    bDisableVisualizationForTest = FParse::Param(
        FCommandLine::Get(),
        TEXT("MBSTMobileFireNoRender"));

    int32 CommandLineUnits = UnitsPerPolicy;
    if (FParse::Value(FCommandLine::Get(), TEXT("MBSTMobileFireUnits="), CommandLineUnits))
    {
        UnitsPerPolicy = FMath::Clamp(CommandLineUnits, 1, 5000);
    }

    float CommandLineTestSeconds = FunctionalTestSeconds;
    if (FParse::Value(
        FCommandLine::Get(),
        TEXT("MBSTMobileFireTestSeconds="),
        CommandLineTestSeconds))
    {
        FunctionalTestSeconds = FMath::Max(CommandLineTestSeconds, 2.0f);
    }

    if (!LoadAssets() || !SpawnTarget())
    {
        UE_LOG(LogTemp, Error, TEXT("MBST_MOBILE_FIRE_DEMO_FAILED: required assets or target could not be created."));
        bFunctionalResultReady = true;
        bFunctionalResultPassed = false;
        if (bAutoExit)
        {
            FPlatformMisc::RequestExit(false);
        }
        return;
    }

    const bool bStopSpawned = SpawnPolicyGroup(
        StopToFireAgentConfig,
        FVector(-6000.0, -2600.0, 0.0),
        StopToFireHandles,
        StopToFireMoveTask);
    const bool bMoveSpawned = SpawnPolicyGroup(
        FireWhileMovingAgentConfig,
        FVector(-6000.0, 2600.0, 0.0),
        FireWhileMovingHandles,
        FireWhileMovingMoveTask);

    if (!bStopSpawned || !bMoveSpawned)
    {
        UE_LOG(LogTemp, Error,
            TEXT("MBST_MOBILE_FIRE_DEMO_FAILED: stop=%d move=%d expected_per_group=%d."),
            StopToFireHandles.Num(),
            FireWhileMovingHandles.Num(),
            UnitsPerPolicy);
        bFunctionalResultReady = true;
        bFunctionalResultPassed = false;
        if (bAutoExit)
        {
            FPlatformMisc::RequestExit(false);
        }
        return;
    }

    UE_LOG(LogTemp, Display,
        TEXT("MBST_MOBILE_FIRE_DEMO_READY: stop_to_fire=%d fire_while_moving=%d. Both groups use native AgentsMoveTo; only MobileFireProfile differs."),
        StopToFireHandles.Num(),
        FireWhileMovingHandles.Num());
}

bool AMBSTMobileFireDemoActor::LoadAssets()
{
    if (!BaseSingleTurretConfig)
    {
        BaseSingleTurretConfig = LoadObject<UMassBattleAgentConfigDataAsset>(
            nullptr,
            MBSTMobileFireDemoPrivate::BaseConfigPath);
    }
    if (!StopToFireProfile)
    {
        StopToFireProfile = LoadObject<UMBSTMobileFireProfile>(
            nullptr,
            MBSTMobileFireDemoPrivate::StopProfilePath);
    }
    if (!FireWhileMovingProfile)
    {
        FireWhileMovingProfile = LoadObject<UMBSTMobileFireProfile>(
            nullptr,
            MBSTMobileFireDemoPrivate::MoveProfilePath);
    }
    if (!StopToFireAgentConfig)
    {
        StopToFireAgentConfig = LoadObject<UMassBattleAgentConfigDataAsset>(
            nullptr,
            MBSTMobileFireDemoPrivate::StopConfigPath);
    }
    if (!FireWhileMovingAgentConfig)
    {
        FireWhileMovingAgentConfig = LoadObject<UMassBattleAgentConfigDataAsset>(
            nullptr,
            MBSTMobileFireDemoPrivate::MoveConfigPath);
    }
    return IsValid(BaseSingleTurretConfig)
        && IsValid(StopToFireProfile)
        && IsValid(FireWhileMovingProfile)
        && IsValid(StopToFireAgentConfig)
        && IsValid(FireWhileMovingAgentConfig);
}

bool AMBSTMobileFireDemoActor::SpawnTarget()
{
    const FEntityTemplateData Template = MBSTMobileFireDemoPrivate::MakeTargetTemplate(
        this,
        BaseSingleTurretConfig);
    if (!Template.IsValid())
    {
        return false;
    }

    FAgentSpawnRectangleShapeData Shape;
    Shape.Region = FVector2D::ZeroVector;
    Shape.Spacing = FVector2D(1.0f, 1.0f);
    Shape.PositioningMode = ESpawnPositioningMode::Sequential;
    const TArray<FEntityHandle> Spawned = UMassBattleFuncLib::SpawnAgentsByTemplateRectangular(
        this,
        Template,
        1,
        2,
        FVector(TargetOrbitRadius, 0.0, 300.0),
        Shape,
        FVector2D::ZeroVector,
        EInitialRotation::CustomRotation,
        FRotator::ZeroRotator,
        FSpawnerMult(),
        true);
    if (Spawned.Num() != 1)
    {
        return false;
    }

    TargetEntity = Spawned[0];
    ConfigureTargetEntity();

    UWorld* World = GetWorld();
    UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
    if (World && Cylinder)
    {
        TargetVisual = World->SpawnActor<AStaticMeshActor>(
            FVector(TargetOrbitRadius, 0.0, 300.0),
            FRotator::ZeroRotator);
        if (TargetVisual)
        {
            TargetVisual->SetActorLabel(TEXT("MBST_Moving_Target"));
            TargetVisual->GetStaticMeshComponent()->SetStaticMesh(Cylinder);
            TargetVisual->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            TargetVisual->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
            TargetVisual->SetActorScale3D(FVector(2.0, 2.0, 4.0));
        }
    }

    if (World)
    {
        if (ADirectionalLight* Light = World->SpawnActor<ADirectionalLight>(
            FVector::ZeroVector,
            FRotator(-55.0f, -35.0f, 0.0f)))
        {
            Light->GetLightComponent()->SetIntensity(6.0f);
        }
    }
    return TargetEntity.IsSet();
}

void AMBSTMobileFireDemoActor::ConfigureTargetEntity()
{
    UMassAPISubsystem* MassAPI = UMassAPISubsystem::GetPtr(this);
    if (!MassAPI || !MassAPI->IsValid(TargetEntity))
    {
        return;
    }
    if (FHealth* Health = MassAPI->GetFragmentPtr<FHealth>(TargetEntity))
    {
        Health->Current = 1000000000.0f;
        Health->Maximum = 1000000000.0f;
        Health->bLockHealth = true;
    }
    if (FVisualize* Visualize = MassAPI->GetFragmentPtr<FVisualize>(TargetEntity))
    {
        Visualize->bEnable = false;
    }
    if (FMove* Move = MassAPI->GetFragmentPtr<FMove>(TargetEntity))
    {
        Move->bEnable = false;
    }
    if (FAttack* Attack = MassAPI->GetFragmentPtr<FAttack>(TargetEntity))
    {
        Attack->bEnable = false;
    }
    if (FTrace* Trace = MassAPI->GetFragmentPtr<FTrace>(TargetEntity))
    {
        Trace->bEnable = false;
    }
    if (FTeam* Team = MassAPI->GetFragmentPtr<FTeam>(TargetEntity))
    {
        Team->index = 2;
    }
}

bool AMBSTMobileFireDemoActor::SpawnPolicyGroup(
    UMassBattleAgentConfigDataAsset* AgentConfig,
    const FVector& Origin,
    TArray<FEntityHandle>& OutHandles,
    TObjectPtr<UMassBattleBPTaskAgentsMoveTo>& OutMoveTask)
{
    const FEntityTemplateData Template = UMassBattleFuncLib::MakeTemplateDataFromDataAsset(
        this,
        AgentConfig);
    if (!Template.IsValid())
    {
        return false;
    }

    const int32 Width = FMath::Max(1, FMath::CeilToInt(FMath::Sqrt(static_cast<float>(UnitsPerPolicy))));
    const int32 Depth = FMath::Max(1, FMath::CeilToInt(static_cast<float>(UnitsPerPolicy) / Width));
    FAgentSpawnRectangleShapeData Shape;
    Shape.Region = FVector2D(
        static_cast<float>(Depth - 1) * FormationSpacing,
        static_cast<float>(Width - 1) * FormationSpacing);
    Shape.Spacing = FVector2D(FormationSpacing, FormationSpacing);
    Shape.ShapeDirection = FVector::ForwardVector;
    Shape.PositioningMode = ESpawnPositioningMode::Sequential;

    OutHandles = UMassBattleFuncLib::SpawnAgentsByTemplateRectangular(
        this,
        Template,
        UnitsPerPolicy,
        1,
        Origin,
        Shape,
        FVector2D::ZeroVector,
        EInitialRotation::CustomRotation,
        FRotator::ZeroRotator,
        FSpawnerMult(),
        true);

    UMassAPISubsystem* MassAPI = UMassAPISubsystem::GetPtr(this);
    TArray<FVector> Goals;
    Goals.Reserve(OutHandles.Num());
    for (const FEntityHandle& Handle : OutHandles)
    {
        if (!MassAPI || !MassAPI->IsValid(Handle))
        {
            continue;
        }
        if (FTeam* Team = MassAPI->GetFragmentPtr<FTeam>(Handle))
        {
            Team->index = 1;
        }
        if (FVisualize* Visualize = MassAPI->GetFragmentPtr<FVisualize>(Handle))
        {
            Visualize->bEnable = !bDisableVisualizationForTest;
        }
        if (FTrace* Trace = MassAPI->GetFragmentPtr<FTrace>(Handle))
        {
            Trace->bEnable = false;
        }
        if (FMove* Move = MassAPI->GetFragmentPtr<FMove>(Handle))
        {
            Move->bEnable = true;
            Move->Z.bEnable = false;
            Move->XY.bStopActiveMovement = false;
        }
        if (FAttack* Attack = MassAPI->GetFragmentPtr<FAttack>(Handle))
        {
            Attack->bEnable = false;
        }
        if (FChase* Chase = MassAPI->GetFragmentPtr<FChase>(Handle))
        {
            Chase->bEnable = false;
        }
        if (FMBSTMobileFireState* FireState = MassAPI->GetFragmentPtr<FMBSTMobileFireState>(Handle))
        {
            FireState->CooldownRemainingSeconds = InitialFireDelaySeconds;
            FireState->Phase = EMBSTMobileFirePhase::Cooling;
            FireState->PhaseTimeSeconds = 0.0f;
            FireState->bMovementHoldRequested = false;
        }
        UMBSTSingleTurretBlueprintLibrary::SetMobileFireTargetEntity(
            this,
            Handle,
            TargetEntity);

        const FLocating* Location = MassAPI->GetFragmentPtr<FLocating>(Handle);
        Goals.Add(Location
            ? Location->Location + FVector(MoveDistance, 0.0, 0.0)
            : Origin + FVector(MoveDistance, 0.0, 0.0));
    }

    if (OutHandles.Num() != UnitsPerPolicy || Goals.Num() != OutHandles.Num())
    {
        return false;
    }

    FMBMoveGoal MoveGoal;
    MoveGoal.GoalType = EMBMoveGoalType::Location;
    MoveGoal.Locations = MoveTemp(Goals);
    FMBMoveNavigation Navigation;
    Navigation.Mode = ENavMode::Individual;
    FMBMoveFailCondition FailConditions;
    FailConditions.FailRadius = MoveDistance * 2.0f;
    FailConditions.StuckTimeLimit = FunctionalTestSeconds * 4.0f;
    OutMoveTask = UMassBattleBPTaskAgentsMoveTo::AgentsMoveTo(
        this,
        OutHandles,
        MoveGoal,
        Navigation,
        FailConditions,
        false,
        FAgentTaskVisualizationConfig());
    if (!OutMoveTask)
    {
        return false;
    }
    OutMoveTask->Activate();
    return true;
}

void AMBSTMobileFireDemoActor::UpdateMovingTarget(const float DeltaSeconds)
{
    UMassAPISubsystem* MassAPI = UMassAPISubsystem::GetPtr(this);
    if (!MassAPI || !MassAPI->IsValid(TargetEntity))
    {
        return;
    }

    const float SafePeriod = FMath::Max(TargetOrbitPeriodSeconds, 1.0f);
    const float Angle = 2.0f * PI * ElapsedSeconds / SafePeriod;
    const FVector NewLocation(
        FMath::Cos(Angle) * TargetOrbitRadius,
        FMath::Sin(Angle) * TargetOrbitRadius,
        300.0f);
    FVector PreviousLocation = NewLocation;
    if (FLocating* Location = MassAPI->GetFragmentPtr<FLocating>(TargetEntity))
    {
        PreviousLocation = Location->Location;
        Location->PreLocation = PreviousLocation;
        Location->Location = NewLocation;
    }
    if (FMoving* Moving = MassAPI->GetFragmentPtr<FMoving>(TargetEntity))
    {
        Moving->CurrentVelocity = DeltaSeconds > SMALL_NUMBER
            ? FVector3f((NewLocation - PreviousLocation) / DeltaSeconds)
            : FVector3f::ZeroVector;
    }
    if (FHealth* Health = MassAPI->GetFragmentPtr<FHealth>(TargetEntity))
    {
        Health->Current = Health->Maximum;
        Health->bLockHealth = true;
    }
    if (TargetVisual)
    {
        TargetVisual->SetActorLocation(NewLocation);
    }
}

void AMBSTMobileFireDemoActor::ConsumeFireRequests()
{
    UMBSTMobileFireSubsystem* Subsystem = GetWorld()
        ? GetWorld()->GetSubsystem<UMBSTMobileFireSubsystem>()
        : nullptr;
    if (!Subsystem)
    {
        return;
    }

    UMassAPISubsystem* MassAPI = UMassAPISubsystem::GetPtr(this);
    for (const FMBSTFireRequest& Request : Subsystem->DrainFireRequests())
    {
        const bool bMoveFire = Request.MobilityPolicy == EMBSTFireMobilityPolicy::AimAndFireWhileMoving;
        float ShooterSpeed = 0.0f;
        bool bActiveInterruptibleMove = false;
        if (MassAPI && MassAPI->IsValid(Request.Shooter))
        {
            if (const FMoving* Moving = MassAPI->GetFragmentPtr<FMoving>(Request.Shooter))
            {
                ShooterSpeed = FVector(Moving->CurrentVelocity).Size2D();
                bActiveInterruptibleMove =
                    Moving->bMovingToGoal && !Moving->bMoveToLocked;
            }
        }
        if (bMoveFire)
        {
            ++MoveFireRequests;
            MoveMaxSpeedAtFire = FMath::Max(MoveMaxSpeedAtFire, ShooterSpeed);
            MovingFireRequestsDuringActiveMove +=
                bActiveInterruptibleMove && ShooterSpeed > KINDA_SMALL_NUMBER ? 1 : 0;
        }
        else
        {
            ++StopFireRequests;
            StopMaxSpeedAtFire = FMath::Max(StopMaxSpeedAtFire, ShooterSpeed);
        }
        if (bDrawFireRequestTracers && GetWorld())
        {
            DrawDebugLine(
                GetWorld(),
                Request.MuzzleWorld.GetLocation(),
                Request.AimWorldLocation,
                bMoveFire ? FColor::Cyan : FColor::Orange,
                false,
                0.12f,
                0,
                3.0f);
        }
    }
}

AMBSTMobileFireDemoActor::FGroupStats AMBSTMobileFireDemoActor::GatherStats(
    const TArray<FEntityHandle>& Handles) const
{
    FGroupStats Stats;
    UMassAPISubsystem* MassAPI = UMassAPISubsystem::GetPtr(this);
    if (!MassAPI)
    {
        return Stats;
    }

    double SpeedSum = 0.0;
    double XSum = 0.0;
    for (const FEntityHandle& Handle : Handles)
    {
        if (!MassAPI->IsValid(Handle))
        {
            continue;
        }
        const FLocating* Location = MassAPI->GetFragmentPtr<FLocating>(Handle);
        const FMoving* Moving = MassAPI->GetFragmentPtr<FMoving>(Handle);
        const FMBSTMobileFireState* FireState = MassAPI->GetFragmentPtr<FMBSTMobileFireState>(Handle);
        if (!Location || !Moving || !FireState)
        {
            continue;
        }
        ++Stats.ValidEntities;
        SpeedSum += FVector(Moving->CurrentVelocity).Size2D();
        XSum += Location->Location.X;
        Stats.ShotSequenceTotal += FireState->ShotSequence;
        Stats.HoldingEntities += FireState->bMovementHoldRequested ? 1 : 0;
    }
    if (Stats.ValidEntities > 0)
    {
        Stats.AverageSpeed = static_cast<float>(SpeedSum / Stats.ValidEntities);
        Stats.AverageX = static_cast<float>(XSum / Stats.ValidEntities);
    }
    return Stats;
}

void AMBSTMobileFireDemoActor::UpdateObserver()
{
    if (bObserverConfigured || !GetWorld())
    {
        return;
    }
    APlayerController* Controller = UGameplayStatics::GetPlayerController(this, 0);
    APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
    if (!Controller || !Pawn)
    {
        return;
    }

    const FVector ViewLocation(-1000.0, -14500.0, 8500.0);
    const FRotator ViewRotation = (FVector(2500.0, 0.0, 0.0) - ViewLocation).Rotation();
    Pawn->SetActorLocationAndRotation(ViewLocation, ViewRotation);
    Controller->SetControlRotation(ViewRotation);
    Controller->SetViewTarget(Pawn);
    if (ASpectatorPawn* Spectator = Cast<ASpectatorPawn>(Pawn))
    {
        if (USpectatorPawnMovement* Movement = Cast<USpectatorPawnMovement>(Spectator->GetMovementComponent()))
        {
            Movement->MaxSpeed = 8000.0f;
            Movement->Acceleration = 32000.0f;
            Movement->Deceleration = 48000.0f;
        }
    }
    bObserverConfigured = true;
}

void AMBSTMobileFireDemoActor::PublishStatus(
    const FGroupStats& StopStats,
    const FGroupStats& MoveStats)
{
    const FString Status = FString::Printf(
        TEXT("MBST Attack-Move | ORANGE stop-to-fire: speed %.0f hold %d/%d shots %lld fire-speed-max %.0f | CYAN fire-while-moving: speed %.0f hold %d/%d shots %lld fire-speed-max %.0f"),
        StopStats.AverageSpeed,
        StopStats.HoldingEntities,
        StopStats.ValidEntities,
        StopStats.ShotSequenceTotal,
        StopMaxSpeedAtFire,
        MoveStats.AverageSpeed,
        MoveStats.HoldingEntities,
        MoveStats.ValidEntities,
        MoveStats.ShotSequenceTotal,
        MoveMaxSpeedAtFire);
    UE_LOG(LogTemp, Display, TEXT("MBST_MOBILE_FIRE_STATUS: %s"), *Status);
    if (GEngine)
    {
        GEngine->AddOnScreenDebugMessage(910011, 1.2f, FColor::White, Status);
        GEngine->AddOnScreenDebugMessage(
            910012,
            1.2f,
            FColor::Silver,
            TEXT("WASD + mouse: free spectator. Both groups use the same native AgentsMoveTo command."));
    }
}

void AMBSTMobileFireDemoActor::EvaluateFunctionalResult(
    const FGroupStats& StopStats,
    const FGroupStats& MoveStats)
{
    if (bFunctionalResultReady)
    {
        return;
    }

    const bool bStopMovedBeforeFiring = StopPeakSpeed > KINDA_SMALL_NUMBER;
    const bool bStopBrakedBeforeFiring = StopPeakHoldingEntities > 0;
    const bool bStopFired = StopStats.ShotSequenceTotal > 0 && StopFireRequests > 0;
    const float StopFireSpeedLimit = IsValid(StopToFireProfile)
        ? StopToFireProfile->MaxFireLinearSpeed
        : 25.0f;
    const bool bStopOnlyFiredAtLowSpeed =
        StopMaxSpeedAtFire <= StopFireSpeedLimit + 5.0f;
    const bool bMoveHadMotion = MovePeakSpeed > KINDA_SMALL_NUMBER;
    const bool bMoveNeverHeld = MovePeakHoldingEntities == 0;
    const bool bMoveFired = MoveStats.ShotSequenceTotal > 0
        && MoveFireRequests > 0
        && MovingFireRequestsDuringActiveMove > 0;

    bFunctionalResultReady = true;
    bFunctionalResultPassed = bStopMovedBeforeFiring
        && bStopBrakedBeforeFiring
        && bStopFired
        && bStopOnlyFiredAtLowSpeed
        && bMoveHadMotion
        && bMoveNeverHeld
        && bMoveFired;

    UE_LOG(LogTemp, Display,
        TEXT("MBST_MOBILE_FIRE_FUNCTIONAL_RESULT: %s stop_peak=%.1f stop_now=%.1f stop_hold_peak=%d stop_shots=%lld stop_requests=%lld stop_fire_speed_max=%.1f stop_fire_speed_limit=%.1f move_peak=%.1f move_now=%.1f move_hold_peak=%d move_shots=%lld move_requests=%lld move_fire_speed_max=%.1f move_fire_during_active_move=%lld"),
        bFunctionalResultPassed ? TEXT("PASS") : TEXT("FAIL"),
        StopPeakSpeed,
        StopStats.AverageSpeed,
        StopPeakHoldingEntities,
        StopStats.ShotSequenceTotal,
        StopFireRequests,
        StopMaxSpeedAtFire,
        StopFireSpeedLimit,
        MovePeakSpeed,
        MoveStats.AverageSpeed,
        MovePeakHoldingEntities,
        MoveStats.ShotSequenceTotal,
        MoveFireRequests,
        MoveMaxSpeedAtFire,
        MovingFireRequestsDuringActiveMove);

    if (GEngine)
    {
        GEngine->AddOnScreenDebugMessage(
            910013,
            20.0f,
            bFunctionalResultPassed ? FColor::Green : FColor::Red,
            bFunctionalResultPassed
                ? TEXT("PASS: stop-to-fire braked and fired; fire-while-moving kept moving and fired.")
                : TEXT("FAIL: inspect MBST_MOBILE_FIRE_STATUS in the Output Log."));
    }
    if (bAutoExit)
    {
        FPlatformMisc::RequestExit(false);
    }
}

void AMBSTMobileFireDemoActor::Tick(const float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (bFunctionalResultReady && bAutoExit)
    {
        return;
    }

    ElapsedSeconds += FMath::Max(DeltaSeconds, 0.0f);
    StatusAccumulator += FMath::Max(DeltaSeconds, 0.0f);
    UpdateObserver();
    UpdateMovingTarget(DeltaSeconds);
    ConsumeFireRequests();

    const FGroupStats StopStats = GatherStats(StopToFireHandles);
    const FGroupStats MoveStats = GatherStats(FireWhileMovingHandles);
    StopPeakSpeed = FMath::Max(StopPeakSpeed, StopStats.AverageSpeed);
    MovePeakSpeed = FMath::Max(MovePeakSpeed, MoveStats.AverageSpeed);
    StopPeakHoldingEntities = FMath::Max(
        StopPeakHoldingEntities,
        StopStats.HoldingEntities);
    MovePeakHoldingEntities = FMath::Max(
        MovePeakHoldingEntities,
        MoveStats.HoldingEntities);

    if (StatusAccumulator >= 1.0f)
    {
        StatusAccumulator = 0.0f;
        PublishStatus(StopStats, MoveStats);
    }
    if (!bFunctionalResultReady && ElapsedSeconds >= FunctionalTestSeconds)
    {
        EvaluateFunctionalResult(StopStats, MoveStats);
    }
}

AMBSTMobileFireDemoGameMode::AMBSTMobileFireDemoGameMode()
{
    DefaultPawnClass = ASpectatorPawn::StaticClass();
}
