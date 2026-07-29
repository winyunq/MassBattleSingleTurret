#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "MBSTMobileFireTypes.h"
#include "MBSTMobileFireProfile.generated.h"

class UMassBattleProjectileConfigDataAsset;

/**
 * Shared weapon/mobility policy used by all agents of one vehicle/weapon type.
 * Use separate assets for a moving-fire tank, stop-to-fire artillery, and a
 * fixed-forward gun even when they share the same articulated mesh layout.
 */
UCLASS(BlueprintType)
class MASSBATTLESINGLETURRETRUNTIME_API UMBSTMobileFireProfile : public UDataAsset
{
    GENERATED_BODY()

public:
    /** Builds the immutable shared-fragment snapshot consumed by Mass processors. */
    FMBSTMobileFireShared BuildSharedFragment() const;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Policy")
    EMBSTFireMobilityPolicy MobilityPolicy = EMBSTFireMobilityPolicy::AimAndFireWhileMoving;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Engagement", meta = (ClampMin = "0.0"))
    float MinimumRange = 0.0f;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Engagement", meta = (ClampMin = "0.0"))
    float MaximumRange = 2000.0f;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Engagement")
    FVector TargetAimOffset = FVector::ZeroVector;

    /** Cheap constant-velocity lead. 0 disables prediction. */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Engagement", meta = (ClampMin = "0.0"))
    float TargetPredictionSeconds = 0.0f;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Aim", meta = (ClampMin = "0.0"))
    float TurretYawToleranceDegrees = 2.0f;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Aim", meta = (ClampMin = "0.0"))
    float BarrelPitchToleranceDegrees = 2.0f;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Aim", meta = (ClampMin = "0.0"))
    float ChassisAimToleranceDegrees = 3.0f;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Aim", meta = (ClampMin = "0.0"))
    float ChassisTurnSpeedDegreesPerSecond = 90.0f;

    /** Set very high for genuine fire-on-the-move. Artillery commonly uses 5-30 cm/s. */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Movement Gate", meta = (ClampMin = "0.0"))
    float MaxFireLinearSpeed = 1000000.0f;

    /** Absolute FMoving::CurrentAngularVelocity limit in degrees/sec. */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Movement Gate", meta = (ClampMin = "0.0"))
    float MaxFireAngularSpeed = 1000000.0f;

    /** Extra time added to speed/deceleration stop-time prediction. */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Movement Gate", meta = (ClampMin = "0.0"))
    float BrakeLeadTimeSeconds = 0.0f;

    /** If true, policy 1/2 holds movement for the full time a target remains in range. */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Movement Gate")
    bool bBrakeImmediatelyWhenTargetInRange = false;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Movement Gate")
    bool bHoldDuringWindup = true;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Movement Gate")
    bool bHoldDuringRecover = true;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Timing", meta = (ClampMin = "0.0"))
    float WindupSeconds = 0.0f;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Timing", meta = (ClampMin = "0.0"))
    float RecoverSeconds = 0.1f;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Timing", meta = (ClampMin = "0.0"))
    float CooldownSeconds = 1.0f;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Presentation", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float RecoilNormalizedOnFire = 1.0f;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Presentation")
    bool bReturnTurretToZeroWhenIdle = false;

    /** Optional built-in executor. Fire requests are still emitted when enabled below. */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Projectile")
    bool bSpawnMassBattleProjectile = false;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Projectile", meta = (EditCondition = "bSpawnMassBattleProjectile", EditConditionHides))
    TObjectPtr<UMassBattleProjectileConfigDataAsset> ProjectileConfig = nullptr;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Projectile", meta = (EditCondition = "bSpawnMassBattleProjectile", EditConditionHides))
    FProjectileMultipliers ProjectileMultipliers;

    /** Queue FMBSTFireRequest for custom C++/Blueprint execution, FX and sound. */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Output")
    bool bEmitFireRequest = true;

    /** Convenient but slower than draining the batched request queue in C++. */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Output", meta = (EditCondition = "bEmitFireRequest", EditConditionHides))
    bool bBroadcastBlueprintFireEvent = false;

    /** Prevent the original BehaviorProcessor from firing a second shot. */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "MassBattle Compatibility")
    bool bDisableBuiltInAttack = true;

    /** Preserve the current MoveTo goal instead of replacing it with a chase goal. */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "MassBattle Compatibility")
    bool bDisableBuiltInChase = true;
};
