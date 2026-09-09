#include "MBSTRtsMobileFireDemo.h"

#include "MBSTMobileFireProfile.h"
#include "MBSTMobileFireSubsystem.h"
#include "MBSTMobileFireTypes.h"
#include "MBSTSingleTurretTypes.h"
#include "DataAssets/MassBattleAgentConfigDataAsset.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FlowField.h"
#include "Fragments/Attack.h"
#include "Fragments/Chase.h"
#include "Fragments/Health.h"
#include "Fragments/Move.h"
#include "Fragments/Navigation.h"
#include "Fragments/MainType.h"
#include "Fragments/Select.h"
#include "Fragments/SubType.h"
#include "Fragments/Team.h"
#include "Fragments/Trace.h"
#include "Fragments/Transform.h"
#include "FuncLibs/MassBattleFuncLib.h"
#include "HAL/PlatformMisc.h"
#include "MassAPIFuncLib.h"
#include "MassAPISubsystem.h"
#include "MassBattleStructs.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Tasks/MassBattleBPTaskAgentsMoveTo.h"

namespace MBSTRtsMobileFireDemoPrivate
{
    static constexpr TCHAR StopProfilePath[] =
        TEXT("/MassBattleSingleTurret/Demo/RTS/DA_MBST_RTS_StopToFire.DA_MBST_RTS_StopToFire");
    static constexpr TCHAR MoveProfilePath[] =
        TEXT("/MassBattleSingleTurret/Demo/RTS/DA_MBST_RTS_FireWhileMoving.DA_MBST_RTS_FireWhileMoving");
    static constexpr TCHAR StopConfigPath[] =
        TEXT("/MassBattleSingleTurret/Demo/RTS/Tank_RTS_StopToFire_AgentConfig.Tank_RTS_StopToFire_AgentConfig");
    static constexpr TCHAR MoveConfigPath[] =
        TEXT("/MassBattleSingleTurret/Demo/RTS/Tank_RTS_FireWhileMoving_AgentConfig.Tank_RTS_FireWhileMoving_AgentConfig");

    static constexpr int32 PlayerTeam = 0;
    static constexpr int32 EnemyTeam = 1;

    static bool ContainsEntity(
        const TArray<FEntityHandle>& Handles,
        const FEntityHandle& Entity)
    {
        return Handles.Contains(Entity);
    }
}

AMBSTRtsMobileFireDemoActor::AMBSTRtsMobileFireDemoActor()
{
    PrimaryActorTick.bCanEverTick = true;
}

void AMBSTRtsMobileFireDemoActor::BeginPlay()
{
    Super::BeginPlay();

    // The copied RTS map keeps its original GameMode, so apply the same timing
    // contract here as the project GameMode used by the user's own maps.
    UMassBattleFuncLib::SetTimingConfig(
        this,
        1.0f / 24.0f,
        0.0f,
        true,
        true);

    bAutoEngageForTesting = bAutoEngageForTesting
        || FParse::Param(FCommandLine::Get(), TEXT("MBSTRtsAutoEngage"));
    bAutoExit = FParse::Param(FCommandLine::Get(), TEXT("MBSTRtsAutoExit"));
    bLockedMoveProbe = FParse::Param(
        FCommandLine::Get(),
        TEXT("MBSTRtsLockedMoveProbe"));

    int32 CommandLineCount = 0;
    if (FParse::Value(FCommandLine::Get(), TEXT("MBSTRtsPlayerUnits="), CommandLineCount))
    {
        StopToFirePlayerCount = FMath::Clamp(CommandLineCount, 1, 5000);
        FireWhileMovingPlayerCount = StopToFirePlayerCount;
    }
    if (FParse::Value(FCommandLine::Get(), TEXT("MBSTRtsEnemyUnits="), CommandLineCount))
    {
        EnemyCount = FMath::Clamp(CommandLineCount, 1, 10000);
    }

    float CommandLineTestSeconds = AutoTestSeconds;
    if (FParse::Value(
        FCommandLine::Get(),
        TEXT("MBSTRtsTestSeconds="),
        CommandLineTestSeconds))
    {
        AutoTestSeconds = FMath::Max(CommandLineTestSeconds, 5.0f);
    }

    if (bAutoEngageForTesting)
    {
        // Keep the unattended integration test deterministic: both policy
        // groups start just outside weapon range on parallel approach lanes.
        // Manual PIE preserves the authored, widely separated formations.
        StopToFirePlayerOrigin = EnemyOrigin + FVector(6800.0, -1800.0, 0.0);
        FireWhileMovingPlayerOrigin = EnemyOrigin + FVector(6800.0, 1800.0, 0.0);
    }

    if (!LoadAssets() || !ResolveGroundFlowField())
    {
        UE_LOG(LogTemp, Error,
            TEXT("MBST_RTS_MOBILE_FIRE_FAILED: dedicated assets or the RTS ground FlowField are missing."));
        bAutoTestResultReady = bAutoEngageForTesting;
        if (bAutoExit)
        {
            FPlatformMisc::RequestExit(false);
        }
        return;
    }

    const bool bMovingFireOnly =
        bSpawnOnlyMovingFireInManualPlay && !bAutoEngageForTesting;
    const bool bStopSpawned = bMovingFireOnly || SpawnGroup(
        StopToFireAgentConfig,
        StopToFirePlayerCount,
        MBSTRtsMobileFireDemoPrivate::PlayerTeam,
        StopToFirePlayerOrigin,
        (EnemyOrigin - StopToFirePlayerOrigin).Rotation().Yaw,
        StopToFirePlayerHandles);
    const bool bMoveSpawned = SpawnGroup(
        FireWhileMovingAgentConfig,
        FireWhileMovingPlayerCount,
        MBSTRtsMobileFireDemoPrivate::PlayerTeam,
        FireWhileMovingPlayerOrigin,
        (EnemyOrigin - FireWhileMovingPlayerOrigin).Rotation().Yaw,
        FireWhileMovingPlayerHandles);
    const bool bEnemySpawned = SpawnGroup(
        bMovingFireOnly ? FireWhileMovingAgentConfig.Get() : StopToFireAgentConfig.Get(),
        EnemyCount,
        MBSTRtsMobileFireDemoPrivate::EnemyTeam,
        EnemyOrigin,
        (((StopToFirePlayerOrigin + FireWhileMovingPlayerOrigin) * 0.5f) - EnemyOrigin)
            .Rotation().Yaw,
        EnemyHandles);

    bSpawnSucceeded = bStopSpawned && bMoveSpawned && bEnemySpawned;
    UE_LOG(LogTemp, Display,
        TEXT("MBST_RTS_MOBILE_FIRE_READY: success=%d moving_fire_only=%d player_stop=%d player_move=%d enemy=%d stop_range=%.0f move_range=%.0f native_rts_commands=1 turret_profiles_chassis_aim=0."),
        bSpawnSucceeded ? 1 : 0,
        bMovingFireOnly ? 1 : 0,
        StopToFirePlayerHandles.Num(),
        FireWhileMovingPlayerHandles.Num(),
        EnemyHandles.Num(),
        StopToFireProfile ? StopToFireProfile->MaximumRange : 0.0f,
        FireWhileMovingProfile ? FireWhileMovingProfile->MaximumRange : 0.0f);

    if (!bSpawnSucceeded && bAutoExit)
    {
        bAutoTestResultReady = true;
        FPlatformMisc::RequestExit(false);
    }
}

bool AMBSTRtsMobileFireDemoActor::LoadAssets()
{
    if (!StopToFireProfile)
    {
        StopToFireProfile = LoadObject<UMBSTMobileFireProfile>(
            nullptr,
            MBSTRtsMobileFireDemoPrivate::StopProfilePath);
    }
    if (!FireWhileMovingProfile)
    {
        FireWhileMovingProfile = LoadObject<UMBSTMobileFireProfile>(
            nullptr,
            MBSTRtsMobileFireDemoPrivate::MoveProfilePath);
    }
    if (!StopToFireAgentConfig)
    {
        StopToFireAgentConfig = LoadObject<UMassBattleAgentConfigDataAsset>(
            nullptr,
            MBSTRtsMobileFireDemoPrivate::StopConfigPath);
    }
    if (!FireWhileMovingAgentConfig)
    {
        FireWhileMovingAgentConfig = LoadObject<UMassBattleAgentConfigDataAsset>(
            nullptr,
            MBSTRtsMobileFireDemoPrivate::MoveConfigPath);
    }

    return IsValid(StopToFireProfile)
        && IsValid(FireWhileMovingProfile)
        && IsValid(StopToFireAgentConfig)
        && IsValid(FireWhileMovingAgentConfig);
}

bool AMBSTRtsMobileFireDemoActor::ResolveGroundFlowField()
{
    if (IsValid(GroundFlowField))
    {
        return true;
    }

    // The source RTS map contains ground and air canvases. Its stable ground
    // instance is BP_FlowFieldCanvas_C_0; keep a first-canvas fallback so a
    // renamed duplicate still works without any MassBattleFrame modification.
    AFlowField* FirstFlowField = nullptr;
    for (TActorIterator<AFlowField> It(GetWorld()); It; ++It)
    {
        AFlowField* Candidate = *It;
        if (!IsValid(Candidate))
        {
            continue;
        }
        if (!FirstFlowField)
        {
            FirstFlowField = Candidate;
        }
        if (Candidate->GetName().Contains(TEXT("FlowFieldCanvas_C_0")))
        {
            GroundFlowField = Candidate;
            break;
        }
    }
    if (!GroundFlowField)
    {
        GroundFlowField = FirstFlowField;
    }

    UE_LOG(LogTemp, Display,
        TEXT("MBST_RTS_GROUND_FLOW_FIELD: %s"),
        GroundFlowField ? *GroundFlowField->GetPathName() : TEXT("None"));
    return IsValid(GroundFlowField);
}

bool AMBSTRtsMobileFireDemoActor::SpawnGroup(
    UMassBattleAgentConfigDataAsset* AgentConfig,
    const int32 Quantity,
    const int32 Team,
    const FVector& Origin,
    const float InitialYawDegrees,
    TArray<FEntityHandle>& OutHandles)
{
    const FEntityTemplateData Template = UMassBattleFuncLib::MakeTemplateDataFromDataAsset(
        this,
        AgentConfig);
    if (!Template.IsValid())
    {
        return false;
    }

    const int32 Columns = FMath::Max(
        1,
        FMath::CeilToInt(FMath::Sqrt(static_cast<float>(Quantity))));
    const int32 Rows = FMath::Max(
        1,
        FMath::CeilToInt(static_cast<float>(Quantity) / Columns));

    FAgentSpawnRectangleShapeData Shape;
    Shape.Region = FVector2D(
        static_cast<float>(Rows - 1) * FormationSpacing,
        static_cast<float>(Columns - 1) * FormationSpacing);
    Shape.Spacing = FVector2D(FormationSpacing, FormationSpacing);
    Shape.ShapeDirection = FRotator(0.0f, InitialYawDegrees, 0.0f).Vector();
    Shape.PositioningMode = ESpawnPositioningMode::Sequential;

    OutHandles = UMassBattleFuncLib::SpawnAgentsByTemplateRectangular(
        this,
        Template,
        Quantity,
        Team,
        Origin,
        Shape,
        FVector2D::ZeroVector,
        EInitialRotation::CustomRotation,
        FRotator(0.0f, InitialYawDegrees, 0.0f),
        FSpawnerMult(),
        true);

    for (const FEntityHandle& Entity : OutHandles)
    {
        ConfigureSpawnedEntity(Entity, Team);
    }
    return OutHandles.Num() == Quantity;
}

void AMBSTRtsMobileFireDemoActor::ConfigureSpawnedEntity(
    const FEntityHandle& Entity,
    const int32 Team)
{
    UMassAPISubsystem* MassAPI = UMassAPISubsystem::GetPtr(this);
    if (!MassAPI || !MassAPI->IsValid(Entity))
    {
        return;
    }

    if (FTeam* TeamFragment = MassAPI->GetFragmentPtr<FTeam>(Entity))
    {
        TeamFragment->index = Team;
        TeamFragment->PreviousIndex = Team;
    }
    if (FHealth* Health = MassAPI->GetFragmentPtr<FHealth>(Entity))
    {
        Health->Current = TankHealth;
        Health->Maximum = TankHealth;
        Health->bLockHealth = false;
    }
    if (FMove* Move = MassAPI->GetFragmentPtr<FMove>(Entity))
    {
        Move->bEnable = true;
        Move->XY.bStopActiveMovement = false;
    }
    if (FMoving* Moving = MassAPI->GetFragmentPtr<FMoving>(Entity))
    {
        // The template's default Goal can predate the rectangular spawner's
        // final transform. Pin idle agents to their actual spawn location;
        // the native RTS AgentsMoveTo task will replace Goal on right-click.
        if (const FLocating* Locating = MassAPI->GetFragmentPtr<FLocating>(Entity))
        {
            Moving->Goal = Locating->Location;
        }
        Moving->CurrentVelocity = FVector3f::ZeroVector;
        Moving->DesiredVelocity = FVector3f::ZeroVector;
        Moving->PendingInputVector = FVector3f::ZeroVector;
        Moving->LastInputVector = FVector3f::ZeroVector;
        Moving->CurrentAngularVelocity = 0.0f;
        Moving->MoveState = EMoveState::None;
        Moving->bMovingToGoal = false;
        Moving->bPreviouslyInAcceptanceRadius = true;
    }
    if (FNavigation* Navigation = MassAPI->GetFragmentPtr<FNavigation>(Entity))
    {
        // Idle until the RTS controller creates an AgentsMoveTo task.
        Navigation->bMoveByFlowfieldOnIdle = false;
        Navigation->FlowFieldToUse = GroundFlowField;
    }
    if (FNavigating* Navigating = MassAPI->GetFragmentPtr<FNavigating>(Entity))
    {
        Navigating->FlowFieldToUsePtr = GroundFlowField;
        Navigating->FlowFieldToUse_Previous = GroundFlowField;
    }
    if (FSelect* Select = MassAPI->GetFragmentPtr<FSelect>(Entity))
    {
        Select->bEnable = true;
    }
    if (FAttack* Attack = MassAPI->GetFragmentPtr<FAttack>(Entity))
    {
        Attack->bEnable = false;
    }
    if (FChase* Chase = MassAPI->GetFragmentPtr<FChase>(Entity))
    {
        Chase->bEnable = false;
    }
    if (FTrace* Trace = MassAPI->GetFragmentPtr<FTrace>(Entity))
    {
        Trace->bEnable = true;
        Trace->Mode = ETraceMode::SectorTraceByTraits;
        Trace->SectorTrace.Common.TraceRadius = 7500.0f;
        Trace->SectorTrace.Common.TraceAngle = 360.0f;
        Trace->SectorTrace.Common.TraceHeight = 5000.0f;
        Trace->SectorTrace.Common.SortMode = ESortMode::NearToFar;
        Trace->SectorTrace.Common.CoolDown = 0.5f;
        Trace->SectorTrace.Common.KeepCount = 1;
        Trace->SectorTrace.Common.bCheckObstacle = false;
        Trace->bSkipTraceWhileTargetValid = true;
        Trace->bCheckLOSWhileTargetValid = false;
        Trace->bRetraceImmediatelyOnTargetLoss = true;

        // AgentConfig is shared by both teams. Restrict each per-entity query to
        // the exact opposing team so neutral RTS-map agents are never acquired.
        // Projectile damage inherits this same target query.
        Trace->Query = FMassBattleQuery();
        if (Team == MBSTRtsMobileFireDemoPrivate::PlayerTeam)
        {
            Trace->Query.All<FAgentTag, FTeam1Tag>();
        }
        else
        {
            Trace->Query.All<FAgentTag, FTeam0Tag>();
        }
    }
    if (FMBSTMobileFireState* FireState =
        MassAPI->GetFragmentPtr<FMBSTMobileFireState>(Entity))
    {
        FireState->ResetCombatRuntime();
        FireState->CooldownRemainingSeconds = 0.5f;
        FireState->Phase = EMBSTMobileFirePhase::Cooling;
        FireState->TargetOverrideMode = EMBSTTargetOverrideMode::None;
        FireState->OverrideTargetEntity.Reset();
        FireState->OverrideTargetWorldLocation = FVector::ZeroVector;
    }
}

TObjectPtr<UMassBattleBPTaskAgentsMoveTo> AMBSTRtsMobileFireDemoActor::StartMoveTask(
    const TArray<FEntityHandle>& Handles,
    const FVector& Destination,
    const bool bBehaviorsCanInterrupt)
{
    UMassAPISubsystem* MassAPI = UMassAPISubsystem::GetPtr(this);
    if (!MassAPI || Handles.IsEmpty())
    {
        return nullptr;
    }

    FVector Center = FVector::ZeroVector;
    int32 ValidCount = 0;
    for (const FEntityHandle& Handle : Handles)
    {
        if (MassAPI->IsValid(Handle))
        {
            if (const FLocating* Location = MassAPI->GetFragmentPtr<FLocating>(Handle))
            {
                Center += Location->Location;
                ++ValidCount;
            }
        }
    }
    if (ValidCount == 0)
    {
        return nullptr;
    }
    Center /= static_cast<double>(ValidCount);

    FMBMoveGoal Goal;
    Goal.GoalType = EMBMoveGoalType::Location;
    Goal.Locations.Reserve(Handles.Num());
    for (const FEntityHandle& Handle : Handles)
    {
        const FLocating* Location = MassAPI->IsValid(Handle)
            ? MassAPI->GetFragmentPtr<FLocating>(Handle)
            : nullptr;
        Goal.Locations.Add(Destination + (Location ? Location->Location - Center : FVector::ZeroVector));
    }

    FMBMoveNavigation Navigation;
    Navigation.Mode = ENavMode::Individual;
    FMBMoveFailCondition FailConditions;
    FailConditions.FailRadius = 100000.0f;
    FailConditions.StuckTimeLimit = AutoTestSeconds * 2.0f;

    UMassBattleBPTaskAgentsMoveTo* Task = UMassBattleBPTaskAgentsMoveTo::AgentsMoveTo(
        this,
        Handles,
        MoveTemp(Goal),
        Navigation,
        FailConditions,
        bBehaviorsCanInterrupt,
        FAgentTaskVisualizationConfig());
    if (Task)
    {
        Task->Activate();
    }
    return Task;
}

void AMBSTRtsMobileFireDemoActor::StartAutoEngage()
{
    if (bAutoEngageStarted)
    {
        return;
    }
    bAutoEngageStarted = true;

    const FVector StopDirection = (EnemyOrigin - StopToFirePlayerOrigin).GetSafeNormal2D();
    const FVector MoveDirection = (EnemyOrigin - FireWhileMovingPlayerOrigin).GetSafeNormal2D();
    StopAutoMoveTask = StartMoveTask(
        StopToFirePlayerHandles,
        EnemyOrigin + StopDirection * 6500.0f,
        !bLockedMoveProbe);
    MoveAutoMoveTask = StartMoveTask(
        FireWhileMovingPlayerHandles,
        EnemyOrigin + MoveDirection * 9000.0f,
        !bLockedMoveProbe);

    UE_LOG(LogTemp, Display,
        TEXT("MBST_RTS_AUTO_ENGAGE_STARTED: mode=%s stop_task=%d move_task=%d."),
        bLockedMoveProbe ? TEXT("NORMAL_MOVE_POLICY_PROBE") : TEXT("ATTACK_MOVE_COMBAT"),
        StopAutoMoveTask ? 1 : 0,
        MoveAutoMoveTask ? 1 : 0);
}

void AMBSTRtsMobileFireDemoActor::ConsumeFireRequests()
{
    UMBSTMobileFireSubsystem* FireSubsystem = GetWorld()
        ? GetWorld()->GetSubsystem<UMBSTMobileFireSubsystem>()
        : nullptr;
    UMassAPISubsystem* MassAPI = UMassAPISubsystem::GetPtr(this);
    if (!FireSubsystem)
    {
        return;
    }

    for (const FMBSTFireRequest& Request : FireSubsystem->DrainFireRequests())
    {
        FColor Color = FColor::White;
        if (MBSTRtsMobileFireDemoPrivate::ContainsEntity(
            StopToFirePlayerHandles,
            Request.Shooter))
        {
            ++StopPlayerShots;
            Color = FColor::Orange;
            if (MassAPI && MassAPI->IsValid(Request.Shooter))
            {
                if (const FMBSTMobileFireState* FireState =
                    MassAPI->GetFragmentPtr<FMBSTMobileFireState>(Request.Shooter))
                {
                    bStopShotWithMovementHold |= FireState->bMovementHoldRequested;
                }
                if (const FMoving* Moving = MassAPI->GetFragmentPtr<FMoving>(Request.Shooter))
                {
                    StopMaxSpeedAtFire = FMath::Max(
                        StopMaxSpeedAtFire,
                        FVector(Moving->CurrentVelocity).Size2D());
                }
            }
        }
        else if (MBSTRtsMobileFireDemoPrivate::ContainsEntity(
            FireWhileMovingPlayerHandles,
            Request.Shooter))
        {
            ++MovingPlayerShots;
            Color = FColor::Cyan;
            if (MassAPI && MassAPI->IsValid(Request.Shooter))
            {
                if (const FMBSTMobileFireState* FireState =
                    MassAPI->GetFragmentPtr<FMBSTMobileFireState>(Request.Shooter))
                {
                    bMoveShotWithoutMovementHold |= !FireState->bMovementHoldRequested;
                }
                if (const FMoving* Moving = MassAPI->GetFragmentPtr<FMoving>(Request.Shooter))
                {
                    bMoveShotDuringAttackMove |=
                        Moving->bMovingToGoal && !Moving->bMoveToLocked;
                    bMoveShotDuringLockedMove |=
                        Moving->bMovingToGoal && Moving->bMoveToLocked;
                    MoveMaxSpeedAtFire = FMath::Max(
                        MoveMaxSpeedAtFire,
                        FVector(Moving->CurrentVelocity).Size2D());
                }
            }
        }
        else if (MBSTRtsMobileFireDemoPrivate::ContainsEntity(EnemyHandles, Request.Shooter))
        {
            ++EnemyShots;
            Color = FColor::Red;
        }

        SpawnedProjectiles += Request.bProjectileSpawned ? 1 : 0;
        if (bDrawFireRequestTracers && GetWorld())
        {
            DrawDebugLine(
                GetWorld(),
                Request.MuzzleWorld.GetLocation(),
                Request.AimWorldLocation,
                Color,
                false,
                0.12f,
                0,
                3.0f);
        }
    }
}

AMBSTRtsMobileFireDemoActor::FGroupSnapshot AMBSTRtsMobileFireDemoActor::GatherSnapshot(
    const TArray<FEntityHandle>& Handles) const
{
    FGroupSnapshot Snapshot;
    UMassAPISubsystem* MassAPI = UMassAPISubsystem::GetPtr(this);
    if (!MassAPI)
    {
        return Snapshot;
    }

    double SpeedSum = 0.0;
    double YawErrorSum = 0.0;
    for (const FEntityHandle& Handle : Handles)
    {
        if (!MassAPI->IsValid(Handle))
        {
            continue;
        }
        const FLocating* Location = MassAPI->GetFragmentPtr<FLocating>(Handle);
        const FMoving* Moving = MassAPI->GetFragmentPtr<FMoving>(Handle);
        const FHealth* Health = MassAPI->GetFragmentPtr<FHealth>(Handle);
        if (!Location || !Health || Health->Current <= 0.0f)
        {
            continue;
        }

        ++Snapshot.Alive;
        Snapshot.Health += Health->Current;
        Snapshot.Center += Location->Location;
        SpeedSum += Moving ? FVector(Moving->CurrentVelocity).Size2D() : 0.0;

        if (Moving && Moving->bMoveToLocked)
        {
            ++Snapshot.LockedMove;
        }
        if (Moving && Moving->bMovingToGoal && !Moving->bMoveToLocked)
        {
            ++Snapshot.AttackMoving;
        }

        const FMBSTMobileFireState* FireState =
            MassAPI->GetFragmentPtr<FMBSTMobileFireState>(Handle);
        const FMBSTSingleTurretState* TurretState =
            MassAPI->GetFragmentPtr<FMBSTSingleTurretState>(Handle);
        if (!FireState)
        {
            continue;
        }

        switch (FireState->Phase)
        {
        case EMBSTMobileFirePhase::Tracking:
            ++Snapshot.Tracking;
            break;
        case EMBSTMobileFirePhase::Braking:
            ++Snapshot.Braking;
            break;
        case EMBSTMobileFirePhase::Windup:
            ++Snapshot.Windup;
            break;
        case EMBSTMobileFirePhase::Firing:
            ++Snapshot.Firing;
            break;
        default:
            break;
        }

        if (!FireState->CurrentTarget.IsSet())
        {
            continue;
        }

        ++Snapshot.Targeted;
        YawErrorSum += FireState->LastYawErrorDegrees;
        Snapshot.MaximumYawError = FMath::Max(
            Snapshot.MaximumYawError,
            static_cast<double>(FireState->LastYawErrorDegrees));

        const FMBSTMobileFireShared* FireShared =
            MassAPI->GetSharedFragmentPtr<FMBSTMobileFireShared>(Handle);
        if (FireShared
            && FireState->LastYawErrorDegrees <= FireShared->TurretYawToleranceDegrees
            && FireState->LastPitchErrorDegrees <= FireShared->BarrelPitchToleranceDegrees)
        {
            ++Snapshot.AimAligned;
        }

        if (!Snapshot.bHasAimSample && TurretState)
        {
            Snapshot.bHasAimSample = true;
            Snapshot.SampleCurrentTurretYaw = TurretState->CurrentYawDegrees;
            Snapshot.SampleTargetTurretYaw = TurretState->TargetYawDegrees;

            if (const FRotating* Rotating = MassAPI->GetFragmentPtr<FRotating>(Handle))
            {
                Snapshot.SampleBodyYaw = FQuat(Rotating->RotationQuat).Rotator().Yaw;
            }

            FVector ToTarget = FireState->LastTargetWorldLocation - Location->Location;
            ToTarget.Z = 0.0;
            if (!ToTarget.IsNearlyZero())
            {
                Snapshot.SampleTargetBearingYaw = ToTarget.Rotation().Yaw;
            }
        }
    }
    if (Snapshot.Alive > 0)
    {
        Snapshot.Center /= static_cast<double>(Snapshot.Alive);
        Snapshot.AverageSpeed = SpeedSum / static_cast<double>(Snapshot.Alive);
    }
    if (Snapshot.Targeted > 0)
    {
        Snapshot.AverageYawError = YawErrorSum / static_cast<double>(Snapshot.Targeted);
    }
    return Snapshot;
}

void AMBSTRtsMobileFireDemoActor::DrawObservationGuides(
    const FGroupSnapshot& StopSnapshot,
    const FGroupSnapshot& MoveSnapshot,
    const FGroupSnapshot& EnemySnapshot) const
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    if (bDrawWorldLabels)
    {
        if (StopSnapshot.Alive > 0)
        {
            DrawDebugString(
                World,
                StopSnapshot.Center + FVector(0.0, 0.0, 900.0),
                FString::Printf(
                    TEXT("PLAYER / ORANGE: STOP TO FIRE  [%d alive, range %.0f]"),
                    StopSnapshot.Alive,
                    StopToFireProfile ? StopToFireProfile->MaximumRange : 0.0f),
                nullptr,
                FColor::Orange,
                0.0f,
                true,
                1.2f);
        }
        if (MoveSnapshot.Alive > 0)
        {
            DrawDebugString(
                World,
                MoveSnapshot.Center + FVector(0.0, 0.0, 900.0),
                FString::Printf(
                    TEXT("PLAYER / CYAN: FIRE WHILE MOVING  [%d alive, range %.0f, damage x0.5]"),
                    MoveSnapshot.Alive,
                    FireWhileMovingProfile ? FireWhileMovingProfile->MaximumRange : 0.0f),
                nullptr,
                FColor::Cyan,
                0.0f,
                true,
                1.2f);
        }
        if (EnemySnapshot.Alive > 0)
        {
            DrawDebugString(
                World,
                EnemySnapshot.Center + FVector(0.0, 0.0, 900.0),
                FString::Printf(TEXT("ENEMY / RED: %d alive"), EnemySnapshot.Alive),
                nullptr,
                FColor::Red,
                0.0f,
                true,
                1.2f);
        }
    }

    if (bDrawRangeGuides)
    {
        if (StopSnapshot.Alive > 0 && StopToFireProfile)
        {
            DrawDebugCircle(
                World,
                StopSnapshot.Center + FVector(0.0, 0.0, 25.0),
                StopToFireProfile->MaximumRange,
                64,
                FColor::Orange,
                false,
                0.0f,
                0,
                2.0f,
                FVector::ForwardVector,
                FVector::RightVector,
                false);
        }
        if (MoveSnapshot.Alive > 0 && FireWhileMovingProfile)
        {
            DrawDebugCircle(
                World,
                MoveSnapshot.Center + FVector(0.0, 0.0, 25.0),
                FireWhileMovingProfile->MaximumRange,
                64,
                FColor::Cyan,
                false,
                0.0f,
                0,
                2.0f,
                FVector::ForwardVector,
                FVector::RightVector,
                false);
        }
    }
}

void AMBSTRtsMobileFireDemoActor::PublishStatus(
    const FGroupSnapshot& StopSnapshot,
    const FGroupSnapshot& MoveSnapshot,
    const FGroupSnapshot& EnemySnapshot)
{
    const FString Status = FString::Printf(
        TEXT("RTS turret demo | stop: %d alive speed %.0f shots %lld target %d aligned %d atk-moving %d yaw %.1f/%.1f phase T%d B%d W%d F%d | moving: %d alive speed %.0f shots %lld target %d aligned %d atk-moving %d yaw %.1f/%.1f phase T%d B%d W%d F%d | enemy: %d alive shots %lld | projectiles %lld"),
        StopSnapshot.Alive,
        StopSnapshot.AverageSpeed,
        StopPlayerShots,
        StopSnapshot.Targeted,
        StopSnapshot.AimAligned,
        StopSnapshot.AttackMoving,
        StopSnapshot.AverageYawError,
        StopSnapshot.MaximumYawError,
        StopSnapshot.Tracking,
        StopSnapshot.Braking,
        StopSnapshot.Windup,
        StopSnapshot.Firing,
        MoveSnapshot.Alive,
        MoveSnapshot.AverageSpeed,
        MovingPlayerShots,
        MoveSnapshot.Targeted,
        MoveSnapshot.AimAligned,
        MoveSnapshot.AttackMoving,
        MoveSnapshot.AverageYawError,
        MoveSnapshot.MaximumYawError,
        MoveSnapshot.Tracking,
        MoveSnapshot.Braking,
        MoveSnapshot.Windup,
        MoveSnapshot.Firing,
        EnemySnapshot.Alive,
        EnemyShots,
        SpawnedProjectiles);
    UE_LOG(LogTemp, Display, TEXT("MBST_RTS_MOBILE_FIRE_STATUS: %s"), *Status);
    UE_LOG(LogTemp, Display,
        TEXT("MBST_RTS_AIM_SAMPLE: stop body=%.1f current=%.1f target=%.1f bearing=%.1f locked=%d | moving body=%.1f current=%.1f target=%.1f bearing=%.1f locked=%d"),
        StopSnapshot.SampleBodyYaw,
        StopSnapshot.SampleCurrentTurretYaw,
        StopSnapshot.SampleTargetTurretYaw,
        StopSnapshot.SampleTargetBearingYaw,
        StopSnapshot.LockedMove,
        MoveSnapshot.SampleBodyYaw,
        MoveSnapshot.SampleCurrentTurretYaw,
        MoveSnapshot.SampleTargetTurretYaw,
        MoveSnapshot.SampleTargetBearingYaw,
        MoveSnapshot.LockedMove);

    if (GEngine)
    {
        GEngine->AddOnScreenDebugMessage(920101, 1.2f, FColor::White, Status);
        GEngine->AddOnScreenDebugMessage(
            920102,
            1.2f,
            FColor::Yellow,
            TEXT("MOVE / right-click: MOVING-FIRE auto-shoots without stopping; STOP-TO-FIRE only tracks. ATTACK: combat may interrupt movement."));
    }
}

void AMBSTRtsMobileFireDemoActor::PublishSelectionDiagnostic()
{
    UMassAPISubsystem* MassAPI = UMassAPISubsystem::GetPtr(this);
    if (!MassAPI)
    {
        return;
    }

    FMassBattleQuery BattleQuery;
    BattleQuery.BattleAllFlagsList.Add(EBattleFlags::Selected);
    const FEntityQuery EntityQuery = BattleQuery.ToEntityQuery();
    TArray<FEntityHandle> Selected = UMassAPIFuncLib::GetMatchingEntities(this, EntityQuery);
    Selected.Sort([](const FEntityHandle& Left, const FEntityHandle& Right)
    {
        return Left.Index == Right.Index
            ? Left.Serial < Right.Serial
            : Left.Index < Right.Index;
    });

    int32 StopToFireCount = 0;
    int32 FireWhileMovingCount = 0;
    int32 NoTurretCount = 0;
    int32 ActorCompoundCount = 0;
    int32 OtherCount = 0;
    int32 LockedMoveCount = 0;
    int32 AttackMoveCount = 0;
    int32 FreeCount = 0;

    FString Signature = FString::Printf(TEXT("%d|"), Selected.Num());
    for (const FEntityHandle& Entity : Selected)
    {
        if (!MassAPI->IsValid(Entity))
        {
            continue;
        }

        TCHAR UnitCode = TEXT('O');
        if (MassAPI->HasSharedFragment<FMBSTMobileFireShared>(Entity))
        {
            const FMBSTMobileFireShared* Shared =
                MassAPI->GetSharedFragmentPtr<FMBSTMobileFireShared>(Entity);
            if (Shared
                && Shared->MobilityPolicy == EMBSTFireMobilityPolicy::AimAndFireWhileMoving)
            {
                ++FireWhileMovingCount;
                UnitCode = TEXT('M');
            }
            else
            {
                ++StopToFireCount;
                UnitCode = TEXT('S');
            }
        }
        else if (const FSubType* SubType = MassAPI->GetFragmentPtr<FSubType>(Entity))
        {
            if (SubType->Index == 2)
            {
                ++NoTurretCount;
                UnitCode = TEXT('N');
            }
            else if (SubType->Index == 3 || SubType->Index == 4 || SubType->Index == 99)
            {
                ++ActorCompoundCount;
                UnitCode = TEXT('A');
            }
            else
            {
                ++OtherCount;
            }
        }
        else
        {
            ++OtherCount;
        }

        TCHAR CommandCode = TEXT('F');
        if (const FMoving* Moving = MassAPI->GetFragmentPtr<FMoving>(Entity))
        {
            if (Moving->bMoveToLocked)
            {
                ++LockedMoveCount;
                CommandCode = TEXT('L');
            }
            else if (Moving->ActiveMoveToTaskID != 0)
            {
                ++AttackMoveCount;
                CommandCode = TEXT('A');
            }
            else
            {
                ++FreeCount;
            }
        }
        else
        {
            ++FreeCount;
        }

        Signature += FString::Printf(
            TEXT("%d:%d:%c:%c|"),
            Entity.Index,
            Entity.Serial,
            UnitCode,
            CommandCode);
    }

    if (Signature == LastSelectionSignature)
    {
        return;
    }
    LastSelectionSignature = MoveTemp(Signature);

    const FString Diagnostic = FString::Printf(
        TEXT("selected=%d | stop_to_fire=%d moving_fire=%d no_turret=%d actor_compound=%d other=%d | move_order=%d attack_move=%d free=%d"),
        Selected.Num(),
        StopToFireCount,
        FireWhileMovingCount,
        NoTurretCount,
        ActorCompoundCount,
        OtherCount,
        LockedMoveCount,
        AttackMoveCount,
        FreeCount);
    UE_LOG(LogTemp, Display, TEXT("MBST_RTS_SELECTION: %s"), *Diagnostic);

    if (GEngine)
    {
        GEngine->AddOnScreenDebugMessage(
            920103,
            4.0f,
            FColor::Green,
            FString::Printf(
                TEXT("SELECTED %d | STOP %d | MOVING-FIRE %d | NO-TURRET %d | ACTOR %d\nMOVE ORDER %d (MOVING-FIRE MAY SHOOT) | ATTACK-MOVE %d"),
                Selected.Num(),
                StopToFireCount,
                FireWhileMovingCount,
                NoTurretCount,
                ActorCompoundCount,
                LockedMoveCount,
                AttackMoveCount));
    }
}

void AMBSTRtsMobileFireDemoActor::EvaluateAutoTest(
    const FGroupSnapshot& StopSnapshot,
    const FGroupSnapshot& MoveSnapshot,
    const FGroupSnapshot& EnemySnapshot)
{
    if (bAutoTestResultReady)
    {
        return;
    }

    const bool bDamageObserved = EnemySnapshot.Alive < EnemyHandles.Num()
        || EnemySnapshot.Health < static_cast<double>(EnemyHandles.Num()) * TankHealth - 1.0;
    const float StopSpeedLimit = StopToFireProfile
        ? StopToFireProfile->MaxFireLinearSpeed
        : 25.0f;
    if (bLockedMoveProbe)
    {
        bAutoTestPassed = bSpawnSucceeded
            && bStopAcquiredDuringLockedMove
            && bMoveAcquiredDuringLockedMove
            && StopPlayerShots == 0
            && MovingPlayerShots > 0
            && bMoveShotDuringLockedMove
            && bMoveShotWithoutMovementHold
            && MoveMaxSpeedAtFire > KINDA_SMALL_NUMBER;
        bAutoTestResultReady = true;

        UE_LOG(LogTemp, Display,
            TEXT("MBST_RTS_LOCKED_MOVE_FUNCTIONAL_RESULT: %s stop_acquired_locked_move=%d stop_shots=%lld move_acquired_locked_move=%d move_shots=%lld move_shot_during_locked_move=%d move_shot_without_hold=%d move_fire_speed_max=%.1f."),
            bAutoTestPassed ? TEXT("PASS") : TEXT("FAIL"),
            bStopAcquiredDuringLockedMove ? 1 : 0,
            StopPlayerShots,
            bMoveAcquiredDuringLockedMove ? 1 : 0,
            MovingPlayerShots,
            bMoveShotDuringLockedMove ? 1 : 0,
            bMoveShotWithoutMovementHold ? 1 : 0,
            MoveMaxSpeedAtFire);

        if (bAutoExit)
        {
            FPlatformMisc::RequestExit(false);
        }
        return;
    }

    bAutoTestPassed = bSpawnSucceeded
        && bStopAcquiredDuringAttackMove
        && bMoveAcquiredDuringAttackMove
        && StopPlayerShots > 0
        && MovingPlayerShots > 0
        && SpawnedProjectiles > 0
        && bStopShotWithMovementHold
        && bMoveShotDuringAttackMove
        && bMoveShotWithoutMovementHold
        && StopMaxSpeedAtFire <= StopSpeedLimit + 5.0f
        && MoveMaxSpeedAtFire > KINDA_SMALL_NUMBER;
    bAutoTestResultReady = true;

    UE_LOG(LogTemp, Display,
        TEXT("MBST_RTS_MOBILE_FIRE_FUNCTIONAL_RESULT: %s stop_alive=%d stop_acquired_attack_move=%d stop_shots=%lld stop_shot_with_hold=%d stop_fire_speed_max=%.1f move_alive=%d move_acquired_attack_move=%d move_shots=%lld move_shot_during_attack_move=%d move_shot_without_hold=%d move_fire_speed_max=%.1f enemy_alive=%d enemy_health=%.1f enemy_shots=%lld projectiles=%lld damage_observed=%d."),
        bAutoTestPassed ? TEXT("PASS") : TEXT("FAIL"),
        StopSnapshot.Alive,
        bStopAcquiredDuringAttackMove ? 1 : 0,
        StopPlayerShots,
        bStopShotWithMovementHold ? 1 : 0,
        StopMaxSpeedAtFire,
        MoveSnapshot.Alive,
        bMoveAcquiredDuringAttackMove ? 1 : 0,
        MovingPlayerShots,
        bMoveShotDuringAttackMove ? 1 : 0,
        bMoveShotWithoutMovementHold ? 1 : 0,
        MoveMaxSpeedAtFire,
        EnemySnapshot.Alive,
        EnemySnapshot.Health,
        EnemyShots,
        SpawnedProjectiles,
        bDamageObserved ? 1 : 0);

    if (bAutoExit)
    {
        FPlatformMisc::RequestExit(false);
    }
}

void AMBSTRtsMobileFireDemoActor::Tick(const float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!bSpawnSucceeded)
    {
        return;
    }

    ElapsedSeconds += FMath::Max(DeltaSeconds, 0.0f);
    StatusAccumulator += FMath::Max(DeltaSeconds, 0.0f);
    if (bAutoEngageForTesting && !bAutoEngageStarted && ElapsedSeconds >= 1.0f)
    {
        StartAutoEngage();
    }

    ConsumeFireRequests();
    const FGroupSnapshot StopSnapshot = GatherSnapshot(StopToFirePlayerHandles);
    const FGroupSnapshot MoveSnapshot = GatherSnapshot(FireWhileMovingPlayerHandles);
    const FGroupSnapshot EnemySnapshot = GatherSnapshot(EnemyHandles);
    bStopAcquiredDuringAttackMove |=
        StopSnapshot.Targeted > 0 && StopSnapshot.AttackMoving > 0;
    bMoveAcquiredDuringAttackMove |=
        MoveSnapshot.Targeted > 0 && MoveSnapshot.AttackMoving > 0;
    bStopAcquiredDuringLockedMove |=
        StopSnapshot.Targeted > 0 && StopSnapshot.LockedMove > 0;
    bMoveAcquiredDuringLockedMove |=
        MoveSnapshot.Targeted > 0 && MoveSnapshot.LockedMove > 0;
    DrawObservationGuides(StopSnapshot, MoveSnapshot, EnemySnapshot);

    if (StatusAccumulator >= 1.0f)
    {
        StatusAccumulator = 0.0f;
        PublishStatus(StopSnapshot, MoveSnapshot, EnemySnapshot);
        PublishSelectionDiagnostic();
    }
    if (bAutoEngageForTesting
        && !bAutoTestResultReady
        && ElapsedSeconds >= AutoTestSeconds)
    {
        EvaluateAutoTest(StopSnapshot, MoveSnapshot, EnemySnapshot);
    }
}
