#pragma once

#include "CoreMinimal.h"
#include "Runtime/Launch/Resources/Version.h"
#include "MassAPIStructs.h"
#include "Fragments/MassBattleBaseStruct.h"
#include "Fragments/ProjectileHostConfig.h"
#include "MBSTMobileFireTypes.generated.h"

class UMBSTMobileFireProfile;
class UMassBattleProjectileConfigDataAsset;

/**
 * Defines how locomotion, aiming and firing interact.
 *
 * StopTurnChassisAndFire:
 *   Stop, rotate the whole chassis toward the target, then fire. The articulated
 *   turret is kept at its neutral angle. This is the fixed-gun/no-turret path.
 *
 * AimWhileMovingStopToFire:
 *   Track with the GPU turret while moving, request braking before the shot,
 *   and fire only after linear/angular speed and aim tolerances are satisfied.
 *
 * AimAndFireWhileMoving:
 *   Track and fire without requesting a movement hold. Optional speed limits
 *   can still reject shots at excessive speed.
 */
UENUM(BlueprintType)
enum class EMBSTFireMobilityPolicy : uint8
{
    StopTurnChassisAndFire UMETA(DisplayName = "1 - Stop, Turn Chassis And Fire"),
    AimWhileMovingStopToFire UMETA(DisplayName = "2 - Aim While Moving, Stop To Fire"),
    AimAndFireWhileMoving UMETA(DisplayName = "3 - Aim And Fire While Moving")
};

UENUM(BlueprintType)
enum class EMBSTMobileFirePhase : uint8
{
    Idle,
    Tracking,
    Braking,
    Windup,
    Firing,
    Recovering,
    Cooling
};

UENUM(BlueprintType)
enum class EMBSTTargetOverrideMode : uint8
{
    None,
    Entity,
    WorldLocation
};

/** Only entities carrying this tag are processed by the mobile-fire pipeline. */
USTRUCT(BlueprintType)
struct MASSBATTLESINGLETURRETRUNTIME_API FMBSTMobileFireTag : public FA_MassBattleBaseTag
{
    GENERATED_BODY()
};

/** Per-agent mobile-fire state. Static weapon settings live in the shared profile. */
USTRUCT(BlueprintType)
struct MASSBATTLESINGLETURRETRUNTIME_API FMBSTMobileFireState : public FA_MassBattleBaseFragment
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    bool bEnabled = true;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    EMBSTMobileFirePhase Phase = EMBSTMobileFirePhase::Idle;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    float PhaseTimeSeconds = 0.0f;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    float CooldownRemainingSeconds = 0.0f;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    FEntityHandle CurrentTarget;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    FVector LastTargetWorldLocation = FVector::ZeroVector;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    float LastYawErrorDegrees = 0.0f;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    float LastPitchErrorDegrees = 0.0f;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    int32 ShotSequence = 0;

    /** Written by the combat processor and consumed by the pre-move gate on the next simulation step. */
    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    bool bMovementHoldRequested = false;

    /** Optional per-agent target override. None uses FTracing::TraceResult. */
    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    EMBSTTargetOverrideMode TargetOverrideMode = EMBSTTargetOverrideMode::None;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    FEntityHandle OverrideTargetEntity;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    FVector OverrideTargetWorldLocation = FVector::ZeroVector;

    /**
     * Internal transient gate state. The pre-move processor saves the original
     * bStopActiveMovement bit, the original MoveProcessor consumes the temporary
     * override, and the restore processor writes the saved value back immediately.
     * This avoids permanently owning or clobbering another system's stop request.
     */
    bool bMovementGateAppliedThisStep = false;
    bool bSavedStopActiveMovement = false;

    FORCEINLINE void ResetCombatRuntime()
    {
        Phase = EMBSTMobileFirePhase::Idle;
        PhaseTimeSeconds = 0.0f;
        CooldownRemainingSeconds = 0.0f;
        CurrentTarget.Reset();
        LastTargetWorldLocation = FVector::ZeroVector;
        LastYawErrorDegrees = 0.0f;
        LastPitchErrorDegrees = 0.0f;
        bMovementHoldRequested = false;
    }
};

/** Shared, cache-friendly copy of UMBSTMobileFireProfile values. */
USTRUCT(BlueprintType)
struct MASSBATTLESINGLETURRETRUNTIME_API FMBSTMobileFireShared : public FA_MassBattleBaseSharedFragment
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    TObjectPtr<UMBSTMobileFireProfile> Profile = nullptr;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    EMBSTFireMobilityPolicy MobilityPolicy = EMBSTFireMobilityPolicy::AimAndFireWhileMoving;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    float MinimumRange = 0.0f;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    float MaximumRange = 2000.0f;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    float TurretYawToleranceDegrees = 2.0f;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    float BarrelPitchToleranceDegrees = 2.0f;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    float ChassisAimToleranceDegrees = 3.0f;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    float ChassisTurnSpeedDegreesPerSecond = 90.0f;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    float MaxFireLinearSpeed = 1000000.0f;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    float MaxFireAngularSpeed = 1000000.0f;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    float BrakeLeadTimeSeconds = 0.0f;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    float WindupSeconds = 0.0f;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    float RecoverSeconds = 0.1f;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    float CooldownSeconds = 1.0f;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    float RecoilNormalizedOnFire = 1.0f;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    float TargetPredictionSeconds = 0.0f;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    FVector TargetAimOffset = FVector::ZeroVector;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    bool bBrakeImmediatelyWhenTargetInRange = false;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    bool bHoldDuringWindup = true;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    bool bHoldDuringRecover = true;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    bool bReturnTurretToZeroWhenIdle = false;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    bool bSpawnMassBattleProjectile = false;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    TObjectPtr<UMassBattleProjectileConfigDataAsset> ProjectileConfig = nullptr;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    FProjectileMultipliers ProjectileMultipliers;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    bool bEmitFireRequest = true;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret|Mobile Fire")
    bool bBroadcastBlueprintFireEvent = false;

    bool operator==(const FMBSTMobileFireShared& Other) const
    {
        // Profiles are authored assets and are expected to be immutable during play.
        // Pointer equality is therefore the correct shared-fragment grouping key.
        return Profile == Other.Profile;
    }
};

/** Produced exactly once per accepted shot. Can drive projectiles, hitscan, FX and sound. */
USTRUCT(BlueprintType)
struct MASSBATTLESINGLETURRETRUNTIME_API FMBSTFireRequest
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "MassBattle|Single Turret|Mobile Fire")
    FEntityHandle Shooter;

    UPROPERTY(BlueprintReadOnly, Category = "MassBattle|Single Turret|Mobile Fire")
    FEntityHandle Target;

    /** Current (unpredicted) target location at the accepted shot sample. */
    UPROPERTY(BlueprintReadOnly, Category = "MassBattle|Single Turret|Mobile Fire")
    FVector TargetWorldLocation = FVector::ZeroVector;

    /** Point used by the turret/hitscan solution after aim offset and cheap lead. */
    UPROPERTY(BlueprintReadOnly, Category = "MassBattle|Single Turret|Mobile Fire")
    FVector AimWorldLocation = FVector::ZeroVector;

    UPROPERTY(BlueprintReadOnly, Category = "MassBattle|Single Turret|Mobile Fire")
    FVector TargetVelocity = FVector::ZeroVector;

    UPROPERTY(BlueprintReadOnly, Category = "MassBattle|Single Turret|Mobile Fire")
    FTransform MuzzleWorld = FTransform::Identity;

    UPROPERTY(BlueprintReadOnly, Category = "MassBattle|Single Turret|Mobile Fire")
    EMBSTFireMobilityPolicy MobilityPolicy = EMBSTFireMobilityPolicy::AimAndFireWhileMoving;

    UPROPERTY(BlueprintReadOnly, Category = "MassBattle|Single Turret|Mobile Fire")
    TObjectPtr<UMBSTMobileFireProfile> Profile = nullptr;

    UPROPERTY(BlueprintReadOnly, Category = "MassBattle|Single Turret|Mobile Fire")
    int32 ShotSequence = 0;

    UPROPERTY(BlueprintReadOnly, Category = "MassBattle|Single Turret|Mobile Fire")
    bool bProjectileSpawned = false;

    UPROPERTY(BlueprintReadOnly, Category = "MassBattle|Single Turret|Mobile Fire")
    FEntityHandle SpawnedProjectile;
};

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
template<>
struct TMassFragmentTraits<FMBSTMobileFireShared>
{
    enum
    {
        AuthorAcceptsItsNotTriviallyCopyable = true
    };
};
#endif
