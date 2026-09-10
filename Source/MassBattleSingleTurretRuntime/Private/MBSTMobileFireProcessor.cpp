#include "MBSTMobileFireProcessor.h"

#include "MBSTMobileFireProfile.h"
#include "MBSTMobileFireSubsystem.h"
#include "MBSTMobileFireTypes.h"
#include "MBSTSingleTurretAsset.h"
#include "MBSTSingleTurretTypes.h"

#include "DataAssets/MassBattleProjectileConfigDataAsset.h"
#include "Engine/World.h"
#include "Fragments/Collider.h"
#include "Fragments/Death.h"
#include "Fragments/Determinism.h"
#include "Fragments/Health.h"
#include "Fragments/Move.h"
#include "Fragments/Network.h"
#include "Fragments/Render.h"
#include "Fragments/StyleType.h"
#include "Fragments/Trace.h"
#include "Fragments/Transform.h"
#include "MassAPISubsystem.h"
#include "MassExecutionContext.h"
#include "Subsystems/MassBattleProjectileSubsystem.h"
#include "Subsystems/MassBattleSubsystem.h"
#include "Subsystems/MassBattleNetworkSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

namespace MBSTMobileFirePrivate
{
    struct FResolvedTarget
    {
        bool bValid = false;
        FEntityHandle Entity;
        FVector Location = FVector::ZeroVector;
        FVector Velocity = FVector::ZeroVector;
        float Radius = 0.0f;
    };

    /** Raw angles preserve unreachable-limit error; clamped angles drive the physical joints. */
    struct FAimSolution
    {
        float RawYawDegrees = 0.0f;
        float RawPitchDegrees = 0.0f;
        float ClampedYawDegrees = 0.0f;
        float ClampedPitchDegrees = 0.0f;
    };

    struct FPendingShot
    {
        FMBSTFireRequest Request;
        bool bSpawnProjectile = false;
        UMassBattleProjectileConfigDataAsset* ProjectileConfig = nullptr;
        FProjectileMultipliers ProjectileMultipliers;
        bool bEmitFireRequest = true;
        bool bBroadcastBlueprintFireEvent = false;
    };

    static FVector ProjectAndNormalize(
        const FVector& Vector,
        const FVector& PlaneNormal,
        const FVector& Fallback)
    {
        FVector Projected = FVector::VectorPlaneProject(Vector, PlaneNormal);
        if (!Projected.Normalize())
        {
            Projected = FVector::VectorPlaneProject(Fallback, PlaneNormal);
            if (!Projected.Normalize())
            {
                Projected = FVector::ForwardVector;
            }
        }
        return Projected;
    }

    static float SignedAngleDegrees(
        const FVector& From,
        const FVector& To,
        const FVector& Axis)
    {
        const FVector SafeAxis = Axis.GetSafeNormal(SMALL_NUMBER, FVector::UpVector);
        const FVector SafeFrom = ProjectAndNormalize(From, SafeAxis, FVector::ForwardVector);
        const FVector SafeTo = ProjectAndNormalize(To, SafeAxis, SafeFrom);
        const float SinAngle = FVector::DotProduct(SafeAxis, FVector::CrossProduct(SafeFrom, SafeTo));
        const float CosAngle = FMath::Clamp(FVector::DotProduct(SafeFrom, SafeTo), -1.0f, 1.0f);
        return FMath::RadiansToDegrees(FMath::Atan2(SinAngle, CosAngle));
    }

    static FAimSolution ComputeDesiredTurretAim(
        const UMBSTSingleTurretAsset& Layout,
        const FTransform& RootWorldTransform,
        const FVector& TargetWorldLocation)
    {
        FAimSolution Result;

        const FVector TargetObject = RootWorldTransform.InverseTransformPosition(TargetWorldLocation);
        const FVector TurretAxis = FVector(Layout.TurretAxisObjectSpace)
            .GetSafeNormal(SMALL_NUMBER, FVector::UpVector);
        const FVector BarrelAxis = FVector(Layout.BarrelAxisObjectSpace)
            .GetSafeNormal(SMALL_NUMBER, FVector::YAxisVector);
        const FVector DefaultForward = FVector(Layout.BarrelForwardAxisObjectSpace)
            .GetSafeNormal(SMALL_NUMBER, FVector::ForwardVector);
        const FVector TurretPivot(Layout.TurretPivotObjectSpace);
        const FVector BarrelPivot(Layout.BarrelPivotObjectSpace);

        const FVector TargetFromTurret = TargetObject - TurretPivot;
        Result.RawYawDegrees = SignedAngleDegrees(DefaultForward, TargetFromTurret, TurretAxis);

        float IgnoredPitch = 0.0f;
        Layout.ClampAngles(
            Result.RawYawDegrees,
            0.0f,
            Result.ClampedYawDegrees,
            IgnoredPitch);

        const FQuat YawRotation(
            TurretAxis,
            FMath::DegreesToRadians(Result.ClampedYawDegrees));
        const FVector YawedForward = YawRotation.RotateVector(DefaultForward);
        const FVector YawedPitchAxis = YawRotation.RotateVector(BarrelAxis);
        const FVector YawedBarrelPivot =
            TurretPivot + YawRotation.RotateVector(BarrelPivot - TurretPivot);
        const FVector TargetFromBarrel = TargetObject - YawedBarrelPivot;

        Result.RawPitchDegrees = Layout.bHasBarrelPitch
            ? SignedAngleDegrees(YawedForward, TargetFromBarrel, YawedPitchAxis)
            : 0.0f;

        Layout.ClampAngles(
            Result.RawYawDegrees,
            Result.RawPitchDegrees,
            Result.ClampedYawDegrees,
            Result.ClampedPitchDegrees);
        return Result;
    }

    static void UpdateTurretMotion(
        FMBSTSingleTurretState& State,
        const FMBSTSingleTurretShared& Shared,
        const float DeltaSeconds)
    {
        const float MinYaw = FMath::Min(Shared.YawLimitsDegrees.X, Shared.YawLimitsDegrees.Y);
        const float MaxYaw = FMath::Max(Shared.YawLimitsDegrees.X, Shared.YawLimitsDegrees.Y);
        const float MinPitch = FMath::Min(Shared.PitchLimitsDegrees.X, Shared.PitchLimitsDegrees.Y);
        const float MaxPitch = FMath::Max(Shared.PitchLimitsDegrees.X, Shared.PitchLimitsDegrees.Y);

        const float TargetYaw = FMath::Clamp(
            FMath::UnwindDegrees(State.TargetYawDegrees),
            MinYaw,
            MaxYaw);
        const float TargetPitch = Shared.bHasBarrelPitch
            ? FMath::Clamp(State.TargetPitchDegrees, MinPitch, MaxPitch)
            : 0.0f;

        if (!State.bArticulationEnabled)
        {
            State.CurrentYawDegrees = 0.0f;
            State.CurrentPitchDegrees = 0.0f;
            State.RecoilNormalized = 0.0f;
            return;
        }

        if (State.bInterpolate && DeltaSeconds > 0.0f)
        {
            State.CurrentYawDegrees = Shared.YawSpeedDegreesPerSecond > 0.0f
                ? FMath::FixedTurn(
                    State.CurrentYawDegrees,
                    TargetYaw,
                    Shared.YawSpeedDegreesPerSecond * DeltaSeconds)
                : TargetYaw;

            State.CurrentPitchDegrees = Shared.PitchSpeedDegreesPerSecond > 0.0f
                ? FMath::FInterpConstantTo(
                    State.CurrentPitchDegrees,
                    TargetPitch,
                    DeltaSeconds,
                    Shared.PitchSpeedDegreesPerSecond)
                : TargetPitch;
        }
        else
        {
            State.CurrentYawDegrees = TargetYaw;
            State.CurrentPitchDegrees = TargetPitch;
        }

        State.CurrentYawDegrees = FMath::Clamp(
            FMath::UnwindDegrees(State.CurrentYawDegrees),
            MinYaw,
            MaxYaw);
        State.CurrentPitchDegrees = Shared.bHasBarrelPitch
            ? FMath::Clamp(State.CurrentPitchDegrees, MinPitch, MaxPitch)
            : 0.0f;

        if (State.RecoilNormalized > 0.0f
            && Shared.RecoilReturnSpeed > 0.0f
            && DeltaSeconds > 0.0f)
        {
            State.RecoilNormalized = FMath::FInterpConstantTo(
                State.RecoilNormalized,
                0.0f,
                DeltaSeconds,
                Shared.RecoilReturnSpeed);
        }
        State.RecoilNormalized = FMath::Clamp(State.RecoilNormalized, 0.0f, 1.0f);
    }

    static FResolvedTarget ResolveTarget(
        UMassAPISubsystem& MassAPI,
        const FMBSTMobileFireState& State,
        const FTracing& Tracing)
    {
        FResolvedTarget Result;

        if (State.TargetOverrideMode == EMBSTTargetOverrideMode::WorldLocation)
        {
            Result.bValid = true;
            Result.Location = State.OverrideTargetWorldLocation;
            return Result;
        }

        const FEntityHandle Candidate = State.TargetOverrideMode == EMBSTTargetOverrideMode::Entity
            ? State.OverrideTargetEntity
            : Tracing.TraceResult;

        if (!MassAPI.IsValid(Candidate)
            || MassAPI.HasTag<FDyingTag>(Candidate)
            || !MassAPI.HasFragment<FLocating>(Candidate))
        {
            return Result;
        }

        if (const FHealth* Health = MassAPI.GetFragmentPtr<FHealth>(Candidate))
        {
            if (Health->Current <= 0.0f)
            {
                return Result;
            }
        }

        Result.bValid = true;
        Result.Entity = Candidate;
        Result.Location = MassAPI.GetFragmentRef<FLocating>(Candidate).Location;

        if (const FMoving* Moving = MassAPI.GetFragmentPtr<FMoving>(Candidate))
        {
            Result.Velocity = FVector(Moving->CurrentVelocity);
        }

        if (const FCollider* Collider = MassAPI.GetFragmentPtr<FCollider>(Candidate))
        {
            const float TargetScale = MassAPI.HasFragment<FScaling>(Candidate)
                ? MassAPI.GetFragmentRef<FScaling>(Candidate).Scale
                : 1.0f;
            Result.Radius = FMath::Max(Collider->Radius * TargetScale, 0.0f);
        }

        return Result;
    }

    static float CalculateSurfaceDistance(
        const FVector& SelfLocation,
        const float SelfRadius,
        const FResolvedTarget& Target)
    {
        return FMath::Max(
            FVector::Distance(SelfLocation, Target.Location) - SelfRadius - Target.Radius,
            0.0f);
    }

    static float UpdateChassisAim(
        FRotating& Rotating,
        const FLocating& Locating,
        const FVector& TargetWorldLocation,
        const float LocalForwardYawOffsetDegrees,
        const float TurnSpeedDegreesPerSecond,
        const float DeltaSeconds)
    {
        FVector ToTarget = TargetWorldLocation - Locating.Location;
        ToTarget.Z = 0.0;
        if (!ToTarget.Normalize())
        {
            return 0.0f;
        }

        FRotator CurrentRotation = FQuat(Rotating.RotationQuat).Rotator();
        const float DesiredYaw = FMath::UnwindDegrees(
            ToTarget.Rotation().Yaw - LocalForwardYawOffsetDegrees);
        CurrentRotation.Yaw = TurnSpeedDegreesPerSecond > 0.0f
            ? FMath::FixedTurn(
                CurrentRotation.Yaw,
                DesiredYaw,
                TurnSpeedDegreesPerSecond * FMath::Max(DeltaSeconds, 0.0f))
            : DesiredYaw;

        const FQuat NewRotation = CurrentRotation.Quaternion();
        Rotating.RotationQuat = FQuat4f(NewRotation);
        Rotating.Rotation = FRotator3f(CurrentRotation);
        Rotating.Direction = FVector3f(NewRotation.GetForwardVector());
        return FMath::Abs(FMath::FindDeltaAngleDegrees(CurrentRotation.Yaw, DesiredYaw));
    }

    static bool ShouldRequestMovementHold(
        const FMBSTMobileFireShared& Shared,
        const FMBSTMobileFireState& State,
        const bool bTargetInRange,
        const float CurrentLinearSpeed,
        const float MoveDeceleration)
    {
        if (!bTargetInRange
            || Shared.MobilityPolicy == EMBSTFireMobilityPolicy::AimAndFireWhileMoving)
        {
            return false;
        }

        if (Shared.bBrakeImmediatelyWhenTargetInRange)
        {
            return true;
        }

        switch (State.Phase)
        {
        case EMBSTMobileFirePhase::Braking:
        case EMBSTMobileFirePhase::Firing:
            return true;

        case EMBSTMobileFirePhase::Windup:
            return Shared.bHoldDuringWindup;

        case EMBSTMobileFirePhase::Recovering:
            return Shared.bHoldDuringRecover;

        default:
            break;
        }

        const float SafeDeceleration = FMath::Max(MoveDeceleration, 1.0f);
        const float EstimatedStopTime = CurrentLinearSpeed / SafeDeceleration;
        const float TimeNeeded =
            EstimatedStopTime
            + Shared.WindupSeconds
            + Shared.BrakeLeadTimeSeconds;
        return State.CooldownRemainingSeconds <= TimeNeeded;
    }

    /**
     * A locked Move order owns locomotion, but it does not universally forbid
     * weapons. The moving-fire policy is the explicit capability that may aim
     * and shoot while that order remains active. Other policies may continue
     * visual turret tracking, but cannot brake, turn the chassis, or fire until
     * the Move finishes or an interruptible combat order replaces it.
     */
    static bool IsCombatSuppressedByLockedMove(
        const FMoving& Moving,
        const EMBSTFireMobilityPolicy MobilityPolicy)
    {
        return Moving.bMoveToLocked
            && MobilityPolicy != EMBSTFireMobilityPolicy::AimAndFireWhileMoving;
    }

    static bool ShouldUseChassisAim(
        const EMBSTFireMobilityPolicy MobilityPolicy,
        const bool bCombatSuppressedByLockedMove)
    {
        return MobilityPolicy == EMBSTFireMobilityPolicy::StopTurnChassisAndFire
            && !bCombatSuppressedByLockedMove;
    }

    static bool ShouldApplyMovementHold(
        const FMBSTMobileFireState& State,
        const FMoving& Moving)
    {
        return State.bEnabled
            && State.bMovementHoldRequested
            // A newly issued normal Move must always clear a stale hold from a
            // prior combat order. Moving-fire never requests a hold itself.
            && !Moving.bMoveToLocked;
    }

    static void SpawnMassBattleProjectile(
        FPendingShot& PendingShot,
        FMassCommandBuffer& CommandBuffer,
        UMassBattleProjectileSubsystem* ProjectileSubsystem)
    {
        UMassBattleProjectileConfigDataAsset* ProjectileConfig = PendingShot.ProjectileConfig;
        if (!PendingShot.bSpawnProjectile
            || !IsValid(ProjectileConfig)
            || !IsValid(ProjectileSubsystem))
        {
            return;
        }

        UMassBattleNetworkSubsystem* Network =
            UMassBattleNetworkSubsystem::GetPtr(ProjectileSubsystem);
        if (!Network)
        {
            return;
        }
        // Combat is dispatched by the existing lockstep SimStages at Subtick1.
        // These accepted shots are simulation results on every peer, not new
        // player inputs. Use the native execution scope for the existing deferred
        // projectile spawn instead of sending a second network command.
        const TGuardValue<bool> CommandExecution(Network->bInCommandExecution, true);

        // Keep the deferred spawner's instigator inheritance and trajectory math,
        // but take its snapshot identity from the existing registered DA template.
        FEntityTemplateData NetworkTemplateData;
        const FNetworking* ProjectileNetworking = nullptr;
        if (const UMassBattleSubsystem* MassBattle = ProjectileSubsystem->GetMassBattleSubsystem();
            MassBattle && MassBattle->bNetworkedMode)
        {
            const FName TemplateKey(*ProjectileConfig->GetPathName());
            if (!Network->GetNetworkTemplateData(TemplateKey, NetworkTemplateData))
            {
                NetworkTemplateData = ProjectileSubsystem->MakeProjectileTemplateFromDataAsset(ProjectileConfig);
            }
            if (!ensure(NetworkTemplateData.IsValid()
                && UMassAPISubsystem::HasFragment<FNetworking>(*NetworkTemplateData.Get())))
            {
                return;
            }
            ProjectileNetworking = NetworkTemplateData.Get()->GetMutableFragment<FNetworking>();
        }

        bool bSuccessful = false;
        FEntityHandle ProjectileHandle;
        FEntityArray IgnoreEntities;
        IgnoreEntities.Entities.Add(PendingShot.Request.Shooter);

        const FVector FromPoint = PendingShot.Request.MuzzleWorld.GetLocation();
        const FVector ToPoint = PendingShot.Request.Target.IsSet()
            ? PendingShot.Request.TargetWorldLocation
            : PendingShot.Request.AimWorldLocation;
        const FVector Direction = (PendingShot.Request.AimWorldLocation - FromPoint).GetSafeNormal(
            SMALL_NUMBER,
            PendingShot.Request.MuzzleWorld.GetRotation().GetForwardVector());

        switch (ProjectileConfig->MovementMode)
        {
        case EProjectileMoveMode::Static:
            ProjectileSubsystem->SpawnProjectile_Static(
                CommandBuffer,
                bSuccessful,
                ProjectileHandle,
                ProjectileConfig,
                PendingShot.ProjectileMultipliers,
                FromPoint,
                PendingShot.Request.Shooter,
                IgnoreEntities);
            break;

        case EProjectileMoveMode::Interped:
            ProjectileSubsystem->SpawnProjectile_Interped(
                CommandBuffer,
                bSuccessful,
                ProjectileHandle,
                ProjectileConfig,
                PendingShot.ProjectileMultipliers,
                FromPoint,
                ToPoint,
                PendingShot.Request.Target,
                PendingShot.Request.Shooter,
                IgnoreEntities);
            break;

        case EProjectileMoveMode::Ballistic:
            ProjectileSubsystem->SpawnProjectile_Ballistic(
                CommandBuffer,
                bSuccessful,
                ProjectileHandle,
                ProjectileConfig,
                PendingShot.ProjectileMultipliers,
                FromPoint,
                ToPoint,
                PendingShot.Request.Target,
                PendingShot.Request.TargetVelocity,
                PendingShot.Request.Shooter,
                IgnoreEntities);
            break;

        case EProjectileMoveMode::Tracking:
            ProjectileSubsystem->SpawnProjectile_Tracking(
                CommandBuffer,
                bSuccessful,
                ProjectileHandle,
                ProjectileConfig,
                PendingShot.ProjectileMultipliers,
                FromPoint,
                ToPoint,
                PendingShot.Request.Target,
                Direction,
                PendingShot.Request.Shooter,
                IgnoreEntities);
            break;

        default:
            break;
        }

        // MassBattle's command-buffer projectile overload reserves and builds
        // the entity, but unlike its synchronous overload it does not stamp
        // Activated. Queue that flag immediately after the build command so
        // the projectile mono processor can move, collide and deal damage.
        if (bSuccessful && ProjectileHandle.IsSet())
        {
            if (UMassAPISubsystem* MassAPI = ProjectileSubsystem->GetMassAPISubsystem())
            {
                if (ProjectileNetworking)
                {
                    // The native buffer builds before adding tags and values;
                    // all finish in this stage's flush before snapshot capture.
                    MassAPI->AddFragment(CommandBuffer, ProjectileHandle, *ProjectileNetworking);
                    MassAPI->AddTag<FNetworkTag>(CommandBuffer, ProjectileHandle);
                }
                MassAPI->SetFlagDefer(CommandBuffer, ProjectileHandle, TEXT("Activated"));
            }
        }

        PendingShot.Request.bProjectileSpawned = bSuccessful;
        PendingShot.Request.SpawnedProjectile = ProjectileHandle;
    }
}

UMBSTMobileFireMovementGateProcessor::UMBSTMobileFireMovementGateProcessor()
    : EntityQuery(*this)
{
    ExecutionOrder.ExecuteBefore.Add(TEXT("MassBattleAgentMoveProcessor"));
    ExecutionFlags = static_cast<int32>(
        EProcessorExecutionFlags::Client
        | EProcessorExecutionFlags::Server
        | EProcessorExecutionFlags::Standalone);
    ProcessingPhase = EMassProcessingPhase::StartPhysics;
    // MassBattle simulation has one execution path: its manually-owned lockstep
    // SimStages. UMBSTMobileFireSubsystem installs this processor there.
    bAutoRegisterWithProcessingPhases = false;
    bRequiresGameThreadExecution = false;
    ExecutionPriority = 20;
}

void UMBSTMobileFireMovementGateProcessor::ConfigureQueries(
    const TSharedRef<FMassEntityManager>& EntityManager)
{
    FEntityQueryBuilder(EntityQuery)
        .All<FMBSTMobileFireTag>()
        .All<FMove, FMBSTMobileFireState>(MARW)
        .All<FMoving>(MARO)
        .RegisterWithProcessor(*this);
}

void UMBSTMobileFireMovementGateProcessor::Execute(
    FMassEntityManager& EntityManager,
    FMassExecutionContext& Context)
{
    UMassBattleSubsystem* MassBattle = UMassBattleSubsystem::GetPtr(this);
    if (!MassBattle
        || (!MassBattle->IsSubFrameScheduled(ESubFrame::Subtick1)
            && !MassBattle->IsSubFrameScheduled(ESubFrame::Subtick2)))
    {
        return;
    }

    EntityQuery.ForEachEntityChunk(Context, [](FMassExecutionContext& ChunkContext)
    {
        TArrayView<FMove> Moves = ChunkContext.GetMutableFragmentView<FMove>();
        TConstArrayView<FMoving> Movings = ChunkContext.GetFragmentView<FMoving>();
        TArrayView<FMBSTMobileFireState> FireStates =
            ChunkContext.GetMutableFragmentView<FMBSTMobileFireState>();

        for (int32 EntityIndex = 0; EntityIndex < ChunkContext.GetNumEntities(); ++EntityIndex)
        {
            FMove& Move = Moves[EntityIndex];
            const FMoving& Moving = Movings[EntityIndex];
            FMBSTMobileFireState& State = FireStates[EntityIndex];

            // Recover safely if a prior restore pass was skipped during processor reconfiguration.
            if (State.bMovementGateAppliedThisStep)
            {
                Move.XY.bStopActiveMovement = State.bSavedStopActiveMovement;
                State.bMovementGateAppliedThisStep = false;
            }

            State.bSavedStopActiveMovement = Move.XY.bStopActiveMovement;
            State.bMovementGateAppliedThisStep = true;
            Move.XY.bStopActiveMovement =
                State.bSavedStopActiveMovement
                || MBSTMobileFirePrivate::ShouldApplyMovementHold(State, Moving);
        }
    });
}

UMBSTMobileFireMovementGateRestoreProcessor::UMBSTMobileFireMovementGateRestoreProcessor()
    : EntityQuery(*this)
{
    ExecutionOrder.ExecuteAfter.Add(TEXT("MassBattleAgentMoveProcessor"));
    ExecutionOrder.ExecuteBefore.Add(TEXT("MassBattleAgentTraceProcessor"));
    ExecutionFlags = static_cast<int32>(
        EProcessorExecutionFlags::Client
        | EProcessorExecutionFlags::Server
        | EProcessorExecutionFlags::Standalone);
    // The manually owned movement stage places this after both move variants,
    // restoring the temporary stop bit before later processors consume it.
    ProcessingPhase = EMassProcessingPhase::FrameEnd;
    bAutoRegisterWithProcessingPhases = false;
    bRequiresGameThreadExecution = false;
    ExecutionPriority = 10;
}

void UMBSTMobileFireMovementGateRestoreProcessor::ConfigureQueries(
    const TSharedRef<FMassEntityManager>& EntityManager)
{
    FEntityQueryBuilder(EntityQuery)
        .All<FMBSTMobileFireTag>()
        .All<FMove, FMBSTMobileFireState>(MARW)
        .RegisterWithProcessor(*this);
}

void UMBSTMobileFireMovementGateRestoreProcessor::Execute(
    FMassEntityManager& EntityManager,
    FMassExecutionContext& Context)
{
    UMassBattleSubsystem* MassBattle = UMassBattleSubsystem::GetPtr(this);
    if (!MassBattle
        || (!MassBattle->IsSubFrameScheduled(ESubFrame::Subtick1)
            && !MassBattle->IsSubFrameScheduled(ESubFrame::Subtick2)))
    {
        return;
    }

    EntityQuery.ForEachEntityChunk(Context, [](FMassExecutionContext& ChunkContext)
    {
        TArrayView<FMove> Moves = ChunkContext.GetMutableFragmentView<FMove>();
        TArrayView<FMBSTMobileFireState> FireStates =
            ChunkContext.GetMutableFragmentView<FMBSTMobileFireState>();

        for (int32 EntityIndex = 0; EntityIndex < ChunkContext.GetNumEntities(); ++EntityIndex)
        {
            FMBSTMobileFireState& State = FireStates[EntityIndex];
            if (!State.bMovementGateAppliedThisStep)
            {
                continue;
            }

            Moves[EntityIndex].XY.bStopActiveMovement = State.bSavedStopActiveMovement;
            State.bMovementGateAppliedThisStep = false;
        }
    });
}

UMBSTMobileFireCombatProcessor::UMBSTMobileFireCombatProcessor()
    : EntityQuery(*this)
{
    ExecutionOrder.ExecuteAfter.Add(TEXT("MassBattleAgentBehaviorProcessor"));
    ExecutionFlags = static_cast<int32>(
        EProcessorExecutionFlags::Client
        | EProcessorExecutionFlags::Server
        | EProcessorExecutionFlags::Standalone);
    ProcessingPhase = EMassProcessingPhase::StartPhysics;
    bAutoRegisterWithProcessingPhases = false;
    bRequiresGameThreadExecution = true;
    ExecutionPriority = 5;
}

void UMBSTMobileFireCombatProcessor::ConfigureQueries(
    const TSharedRef<FMassEntityManager>& EntityManager)
{
    FEntityQueryBuilder(EntityQuery)
        .All<FMBSTSingleTurretTag, FMBSTMobileFireTag>()
        .All<FLocating, FScaling, FCollider, FMove, FMoving, FTracing, FVisualize>(MARO)
        .All<FRotating, FStyleType, FMBSTSingleTurretState, FMBSTMobileFireState>(MARW)
        .All<FMBSTSingleTurretShared, FMBSTMobileFireShared>(MARO)
        .RegisterWithProcessor(*this);
}

void UMBSTMobileFireCombatProcessor::Execute(
    FMassEntityManager& EntityManager,
    FMassExecutionContext& Context)
{
    UMassBattleSubsystem* MassBattle = UMassBattleSubsystem::GetPtr(this);
    UMassAPISubsystem* MassAPI = UMassAPISubsystem::GetPtr(this);
    if (!MassBattle
        || !MassBattle->IsSubFrameScheduled(ESubFrame::Subtick1)
        || !MassAPI)
    {
        return;
    }

    const float DeltaSeconds = FMath::Max(MassBattle->GetCalculatedStepTime(), 0.0f);

    TArray<MBSTMobileFirePrivate::FPendingShot> PendingShots;
    PendingShots.Reserve(64);

    EntityQuery.ForEachEntityChunk(Context, [&](FMassExecutionContext& ChunkContext)
    {
        const FMBSTSingleTurretShared& TurretShared =
            ChunkContext.GetSharedFragment<FMBSTSingleTurretShared>();
        const FMBSTMobileFireShared& FireShared =
            ChunkContext.GetSharedFragment<FMBSTMobileFireShared>();
        TArrayView<FStyleType> Styles = ChunkContext.GetMutableFragmentView<FStyleType>();
        TArrayView<FMBSTSingleTurretState> TurretStates =
            ChunkContext.GetMutableFragmentView<FMBSTSingleTurretState>();
        if (!IsValid(TurretShared.Layout) || !IsValid(FireShared.Profile))
        {
            for (int32 EntityIndex = 0; EntityIndex < ChunkContext.GetNumEntities(); ++EntityIndex)
            {
                Styles[EntityIndex].Index = MBSTPacking::SanitizeAndPack(
                    TurretStates[EntityIndex],
                    TurretShared);
            }
            return;
        }

        TConstArrayView<FLocating> Locations = ChunkContext.GetFragmentView<FLocating>();
        TConstArrayView<FScaling> Scalings = ChunkContext.GetFragmentView<FScaling>();
        TConstArrayView<FCollider> Colliders = ChunkContext.GetFragmentView<FCollider>();
        TConstArrayView<FMove> Moves = ChunkContext.GetFragmentView<FMove>();
        TConstArrayView<FMoving> Movings = ChunkContext.GetFragmentView<FMoving>();
        TConstArrayView<FTracing> Tracings = ChunkContext.GetFragmentView<FTracing>();
        TConstArrayView<FVisualize> Visualizes = ChunkContext.GetFragmentView<FVisualize>();
        TArrayView<FRotating> Rotations = ChunkContext.GetMutableFragmentView<FRotating>();
        TArrayView<FMBSTMobileFireState> FireStates =
            ChunkContext.GetMutableFragmentView<FMBSTMobileFireState>();

        for (int32 EntityIndex = 0; EntityIndex < ChunkContext.GetNumEntities(); ++EntityIndex)
        {
            const FLocating& Locating = Locations[EntityIndex];
            const FScaling& Scaling = Scalings[EntityIndex];
            const FCollider& Collider = Colliders[EntityIndex];
            const FMove& Move = Moves[EntityIndex];
            const FMoving& Moving = Movings[EntityIndex];
            const FTracing& Tracing = Tracings[EntityIndex];
            const FVisualize& Visualize = Visualizes[EntityIndex];
            FRotating& Rotating = Rotations[EntityIndex];
            FMBSTSingleTurretState& TurretState = TurretStates[EntityIndex];
            FMBSTMobileFireState& FireState = FireStates[EntityIndex];

            FireState.CooldownRemainingSeconds = FMath::Max(
                FireState.CooldownRemainingSeconds - DeltaSeconds,
                0.0f);

            // Firing is a one-simulation-step observable phase; recovery begins next step.
            if (FireState.Phase == EMBSTMobileFirePhase::Firing)
            {
                FireState.Phase = EMBSTMobileFirePhase::Recovering;
                FireState.PhaseTimeSeconds = 0.0f;
            }

            const MBSTMobileFirePrivate::FResolvedTarget Target =
                MBSTMobileFirePrivate::ResolveTarget(*MassAPI, FireState, Tracing);

            const float SelfRadius = FMath::Max(Collider.Radius * Scaling.Scale, 0.0f);
            const float SurfaceDistance = Target.bValid
                ? MBSTMobileFirePrivate::CalculateSurfaceDistance(
                    Locating.Location,
                    SelfRadius,
                    Target)
                : TNumericLimits<float>::Max();
            const bool bTargetInRange = Target.bValid
                && SurfaceDistance >= FireShared.MinimumRange
                && SurfaceDistance <= FireShared.MaximumRange;

            const float CurrentLinearSpeed = FVector(Moving.CurrentVelocity).Size2D();
            const float CurrentAngularSpeed = FMath::Abs(Moving.CurrentAngularVelocity);
            const bool bCombatSuppressedByLockedMove =
                MBSTMobileFirePrivate::IsCombatSuppressedByLockedMove(
                    Moving,
                    FireShared.MobilityPolicy);

            if (!FireState.bEnabled)
            {
                FireState.bMovementHoldRequested = false;
                FireState.Phase = EMBSTMobileFirePhase::Idle;
                FireState.PhaseTimeSeconds = 0.0f;
                FireState.CurrentTarget.Reset();
                MBSTMobileFirePrivate::UpdateTurretMotion(
                    TurretState,
                    TurretShared,
                    DeltaSeconds);
                continue;
            }

            FVector AimLocation = Target.Location;
            if (Target.bValid)
            {
                AimLocation += FireShared.TargetAimOffset;
                AimLocation += Target.Velocity * FireShared.TargetPredictionSeconds;
                FireState.CurrentTarget = Target.Entity;
                FireState.LastTargetWorldLocation = AimLocation;
            }
            else
            {
                FireState.CurrentTarget.Reset();
            }

            bool bAimAligned = false;
            if (Target.bValid)
            {
                if (MBSTMobileFirePrivate::ShouldUseChassisAim(
                    FireShared.MobilityPolicy,
                    bCombatSuppressedByLockedMove))
                {
                    const float LocalForwardYawOffset =
                        FVector(TurretShared.Layout->BarrelForwardAxisObjectSpace).Rotation().Yaw;
                    FireState.LastYawErrorDegrees = MBSTMobileFirePrivate::UpdateChassisAim(
                        Rotating,
                        Locating,
                        AimLocation,
                        LocalForwardYawOffset,
                        FireShared.ChassisTurnSpeedDegreesPerSecond,
                        DeltaSeconds);

                    // Policy 1 owns horizontal aim with the chassis. The barrel may still pitch.
                    const FTransform RootWorld = UMBSTSingleTurretAsset::CalculateMeshRootWorldTransform(
                        Locating, Rotating, Scaling, &Collider, &Move, &Moving, &Visualize);
                    const MBSTMobileFirePrivate::FAimSolution Aim =
                        MBSTMobileFirePrivate::ComputeDesiredTurretAim(
                            *TurretShared.Layout,
                            RootWorld,
                            AimLocation);
                    TurretState.TargetYawDegrees = 0.0f;
                    TurretState.TargetPitchDegrees = Aim.ClampedPitchDegrees;
                    MBSTMobileFirePrivate::UpdateTurretMotion(
                        TurretState,
                        TurretShared,
                        DeltaSeconds);

                    FireState.LastPitchErrorDegrees = FMath::Abs(
                        TurretState.CurrentPitchDegrees - Aim.RawPitchDegrees);
                    bAimAligned =
                        FireState.LastYawErrorDegrees <= FireShared.ChassisAimToleranceDegrees
                        && FireState.LastPitchErrorDegrees <= FireShared.BarrelPitchToleranceDegrees;
                }
                else
                {
                    const FTransform RootWorld = UMBSTSingleTurretAsset::CalculateMeshRootWorldTransform(
                        Locating, Rotating, Scaling, &Collider, &Move, &Moving, &Visualize);
                    const MBSTMobileFirePrivate::FAimSolution Aim =
                        MBSTMobileFirePrivate::ComputeDesiredTurretAim(
                            *TurretShared.Layout,
                            RootWorld,
                            AimLocation);
                    TurretState.TargetYawDegrees = Aim.ClampedYawDegrees;
                    TurretState.TargetPitchDegrees = Aim.ClampedPitchDegrees;
                    MBSTMobileFirePrivate::UpdateTurretMotion(
                        TurretState,
                        TurretShared,
                        DeltaSeconds);

                    // Compare against raw angles so a target outside mechanical limits never
                    // becomes "aligned" merely because the joint reached its clamp.
                    FireState.LastYawErrorDegrees = FMath::Abs(FMath::FindDeltaAngleDegrees(
                        TurretState.CurrentYawDegrees,
                        Aim.RawYawDegrees));
                    FireState.LastPitchErrorDegrees = FMath::Abs(
                        TurretState.CurrentPitchDegrees - Aim.RawPitchDegrees);
                    bAimAligned =
                        FireState.LastYawErrorDegrees <= FireShared.TurretYawToleranceDegrees
                        && FireState.LastPitchErrorDegrees <= FireShared.BarrelPitchToleranceDegrees;
                }
            }
            else
            {
                if (FireShared.bReturnTurretToZeroWhenIdle)
                {
                    TurretState.TargetYawDegrees = 0.0f;
                    TurretState.TargetPitchDegrees = 0.0f;
                }
                MBSTMobileFirePrivate::UpdateTurretMotion(
                    TurretState,
                    TurretShared,
                    DeltaSeconds);
                FireState.LastYawErrorDegrees = 0.0f;
                FireState.LastPitchErrorDegrees = 0.0f;
            }

            // A normal Move blocks combat only for policies that need to stop or
            // turn the chassis. AimAndFireWhileMoving is deliberately allowed
            // through this gate: it keeps the Move order, acquires a target and
            // fires without requesting a movement hold.
            if (bCombatSuppressedByLockedMove)
            {
                FireState.bMovementHoldRequested = false;
                FireState.Phase = Target.bValid
                    ? EMBSTMobileFirePhase::Tracking
                    : EMBSTMobileFirePhase::Idle;
                FireState.PhaseTimeSeconds = 0.0f;
                continue;
            }

            if (FireState.Phase == EMBSTMobileFirePhase::Recovering)
            {
                FireState.PhaseTimeSeconds += DeltaSeconds;
                FireState.bMovementHoldRequested =
                    bTargetInRange
                    && FireShared.MobilityPolicy != EMBSTFireMobilityPolicy::AimAndFireWhileMoving
                    && FireShared.bHoldDuringRecover;

                if (FireState.PhaseTimeSeconds < FireShared.RecoverSeconds)
                {
                    continue;
                }

                FireState.Phase = FireState.CooldownRemainingSeconds > 0.0f
                    ? EMBSTMobileFirePhase::Cooling
                    : EMBSTMobileFirePhase::Tracking;
                FireState.PhaseTimeSeconds = 0.0f;
            }

            if (!bTargetInRange)
            {
                FireState.bMovementHoldRequested = false;
                FireState.Phase = Target.bValid
                    ? EMBSTMobileFirePhase::Tracking
                    : EMBSTMobileFirePhase::Idle;
                FireState.PhaseTimeSeconds = 0.0f;
                continue;
            }

            FireState.bMovementHoldRequested =
                MBSTMobileFirePrivate::ShouldRequestMovementHold(
                    FireShared,
                    FireState,
                    bTargetInRange,
                    CurrentLinearSpeed,
                    Move.XY.MoveDeceleration);

            const bool bMovementStable =
                CurrentLinearSpeed <= FireShared.MaxFireLinearSpeed
                && CurrentAngularSpeed <= FireShared.MaxFireAngularSpeed;

            if (FireState.CooldownRemainingSeconds > 0.0f)
            {
                FireState.Phase = FireState.bMovementHoldRequested
                    ? EMBSTMobileFirePhase::Braking
                    : EMBSTMobileFirePhase::Cooling;
                FireState.PhaseTimeSeconds = 0.0f;
                continue;
            }

            if (!bAimAligned || !bMovementStable)
            {
                FireState.Phase = FireState.bMovementHoldRequested
                    ? EMBSTMobileFirePhase::Braking
                    : EMBSTMobileFirePhase::Tracking;
                FireState.PhaseTimeSeconds = 0.0f;
                continue;
            }

            if (FireState.Phase != EMBSTMobileFirePhase::Windup)
            {
                FireState.Phase = EMBSTMobileFirePhase::Windup;
                FireState.PhaseTimeSeconds = 0.0f;
            }

            FireState.PhaseTimeSeconds += DeltaSeconds;
            if (FireState.PhaseTimeSeconds + KINDA_SMALL_NUMBER < FireShared.WindupSeconds)
            {
                continue;
            }

            const FTransform RootWorld = UMBSTSingleTurretAsset::CalculateMeshRootWorldTransform(
                Locating, Rotating, Scaling, &Collider, &Move, &Moving, &Visualize);
            const float PreShotRecoil = TurretState.RecoilNormalized;
            const FTransform MuzzleWorld =
                TurretShared.Layout->CalculateMuzzleWorldTransform(
                    RootWorld,
                    TurretState.CurrentYawDegrees,
                    TurretState.CurrentPitchDegrees,
                    PreShotRecoil);

            TurretState.RecoilNormalized = FMath::Clamp(
                FireShared.RecoilNormalizedOnFire,
                0.0f,
                1.0f);
            FireState.Phase = EMBSTMobileFirePhase::Firing;
            FireState.PhaseTimeSeconds = 0.0f;
            FireState.CooldownRemainingSeconds = FireShared.CooldownSeconds;
            ++FireState.ShotSequence;

            if (FireShared.MobilityPolicy != EMBSTFireMobilityPolicy::AimAndFireWhileMoving)
            {
                FireState.bMovementHoldRequested = true;
            }

            // A profile may use the processor only for articulation/recoil. Avoid
            // constructing a hand-off record when no projectile or request sink is enabled.
            if (FireShared.bSpawnMassBattleProjectile || FireShared.bEmitFireRequest)
            {
                MBSTMobileFirePrivate::FPendingShot& PendingShot =
                    PendingShots.AddDefaulted_GetRef();
                PendingShot.Request.Shooter = FEntityHandle(ChunkContext.GetEntity(EntityIndex));
                PendingShot.Request.Target = Target.Entity;
                PendingShot.Request.TargetWorldLocation = Target.Location;
                PendingShot.Request.AimWorldLocation = AimLocation;
                PendingShot.Request.TargetVelocity = Target.Velocity;
                PendingShot.Request.MuzzleWorld = MuzzleWorld;
                PendingShot.Request.MobilityPolicy = FireShared.MobilityPolicy;
                PendingShot.Request.Profile = FireShared.Profile;
                PendingShot.Request.ShotSequence = FireState.ShotSequence;
                PendingShot.bSpawnProjectile = FireShared.bSpawnMassBattleProjectile;
                PendingShot.ProjectileConfig = FireShared.ProjectileConfig;
                PendingShot.ProjectileMultipliers = FireShared.ProjectileMultipliers;
                PendingShot.bEmitFireRequest = FireShared.bEmitFireRequest;
                PendingShot.bBroadcastBlueprintFireEvent =
                    FireShared.bBroadcastBlueprintFireEvent;
            }
        }

        // MobileFire is the single writer for its packed articulation sample.
        // Keep this as one cache-hot tail loop so every early-continue path is covered.
        for (int32 EntityIndex = 0; EntityIndex < ChunkContext.GetNumEntities(); ++EntityIndex)
        {
            Styles[EntityIndex].Index = MBSTPacking::SanitizeAndPack(
                TurretStates[EntityIndex],
                TurretShared);
        }
    });

    if (MassBattle->bDeterministic && PendingShots.Num() > 1)
    {
        // Snapshot restoration can reorder chunks. The native projectile call
        // allocates its UID immediately, so drain this existing array by shooter
        // UID, just as the native registration queues use stable entity keys.
        TArray<int32> ShooterKeys;
        ShooterKeys.Reserve(PendingShots.Num());
        for (const MBSTMobileFirePrivate::FPendingShot& PendingShot : PendingShots)
        {
            const FDeterminism* Determinism =
                MassAPI->GetFragmentPtr<FDeterminism>(PendingShot.Request.Shooter);
            ShooterKeys.Add(Determinism ? Determinism->UniqueID : -1);
        }
        SortByPreExtractedKeys(PendingShots, ShooterKeys);
    }

    UMBSTMobileFireSubsystem* FireSubsystem = GetWorld()
        ? GetWorld()->GetSubsystem<UMBSTMobileFireSubsystem>()
        : nullptr;
    UMassBattleProjectileSubsystem* ProjectileSubsystem = GetWorld()
        ? GetWorld()->GetSubsystem<UMassBattleProjectileSubsystem>()
        : nullptr;

    for (MBSTMobileFirePrivate::FPendingShot& PendingShot : PendingShots)
    {
        // Projectile entities must be created through the execution context's
        // command buffer: synchronous BuildEntity is forbidden while Mass is
        // processing this processor.
        MBSTMobileFirePrivate::SpawnMassBattleProjectile(
            PendingShot,
            Context.Defer(),
            ProjectileSubsystem);
        if (PendingShot.bEmitFireRequest && FireSubsystem)
        {
            FireSubsystem->SubmitFireRequest(
                PendingShot.Request,
                PendingShot.bBroadcastBlueprintFireEvent);
        }
    }
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMBSTMobileFirePolicyGateTest,
    "MassBattle.SingleTurret.MobileFire.PolicyGate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMBSTMobileFirePolicyGateTest::RunTest(const FString& Parameters)
{
    FMBSTMobileFireShared Shared;
    FMBSTMobileFireState State;
    State.Phase = EMBSTMobileFirePhase::Braking;

    Shared.MobilityPolicy = EMBSTFireMobilityPolicy::AimAndFireWhileMoving;
    TestFalse(
        TEXT("Move-fire policy never injects a movement hold"),
        MBSTMobileFirePrivate::ShouldRequestMovementHold(
            Shared,
            State,
            true,
            800.0f,
            400.0f));

    Shared.MobilityPolicy = EMBSTFireMobilityPolicy::AimWhileMovingStopToFire;
    TestTrue(
        TEXT("Stop-to-fire policy holds while braking"),
        MBSTMobileFirePrivate::ShouldRequestMovementHold(
            Shared,
            State,
            true,
            800.0f,
            400.0f));

    Shared.MobilityPolicy = EMBSTFireMobilityPolicy::StopTurnChassisAndFire;
    Shared.bBrakeImmediatelyWhenTargetInRange = true;
    State.Phase = EMBSTMobileFirePhase::Tracking;
    TestTrue(
        TEXT("Fixed-gun policy can brake immediately on target acquisition"),
        MBSTMobileFirePrivate::ShouldRequestMovementHold(
            Shared,
            State,
            true,
            800.0f,
            400.0f));

    TestFalse(
        TEXT("No policy holds movement when the target is out of range"),
        MBSTMobileFirePrivate::ShouldRequestMovementHold(
            Shared,
            State,
            false,
            800.0f,
            400.0f));

    FMoving Moving;
    State.bEnabled = true;
    State.bMovementHoldRequested = true;
    Moving.bMoveToLocked = true;
    TestTrue(
        TEXT("An exclusive Move suppresses stop-to-fire combat"),
        MBSTMobileFirePrivate::IsCombatSuppressedByLockedMove(
            Moving,
            EMBSTFireMobilityPolicy::AimWhileMovingStopToFire));
    TestFalse(
        TEXT("Moving-fire is explicitly allowed during an exclusive Move"),
        MBSTMobileFirePrivate::IsCombatSuppressedByLockedMove(
            Moving,
            EMBSTFireMobilityPolicy::AimAndFireWhileMoving));
    TestFalse(
        TEXT("A stale stop request cannot interrupt an exclusive Move"),
        MBSTMobileFirePrivate::ShouldApplyMovementHold(State, Moving));

    Moving.bMoveToLocked = false;
    TestFalse(
        TEXT("An interruptible Attack-Move authorizes turret combat"),
        MBSTMobileFirePrivate::IsCombatSuppressedByLockedMove(
            Moving,
            EMBSTFireMobilityPolicy::AimWhileMovingStopToFire));
    TestTrue(
        TEXT("Stop-to-fire may brake during an interruptible Attack-Move"),
        MBSTMobileFirePrivate::ShouldApplyMovementHold(State, Moving));

    TestFalse(
        TEXT("Stop-to-fire turret policy never steers the chassis to aim"),
        MBSTMobileFirePrivate::ShouldUseChassisAim(
            EMBSTFireMobilityPolicy::AimWhileMovingStopToFire,
            false));
    TestFalse(
        TEXT("Moving-fire turret policy never steers the chassis to aim"),
        MBSTMobileFirePrivate::ShouldUseChassisAim(
            EMBSTFireMobilityPolicy::AimAndFireWhileMoving,
            false));
    TestTrue(
        TEXT("Only the explicit fixed-gun policy may steer the chassis"),
        MBSTMobileFirePrivate::ShouldUseChassisAim(
            EMBSTFireMobilityPolicy::StopTurnChassisAndFire,
            false));
    TestFalse(
        TEXT("Even the fixed-gun policy cannot steer during an exclusive Move"),
        MBSTMobileFirePrivate::ShouldUseChassisAim(
            EMBSTFireMobilityPolicy::StopTurnChassisAndFire,
            true));
    return true;
}
#endif
