#include "MBSTMobileFireProfile.h"

FMBSTMobileFireShared UMBSTMobileFireProfile::BuildSharedFragment() const
{
    FMBSTMobileFireShared Shared;
    Shared.Profile = const_cast<UMBSTMobileFireProfile*>(this);
    Shared.MobilityPolicy = MobilityPolicy;
    Shared.MinimumRange = FMath::Max(MinimumRange, 0.0f);
    Shared.MaximumRange = FMath::Max(MaximumRange, Shared.MinimumRange);
    Shared.TurretYawToleranceDegrees = FMath::Max(TurretYawToleranceDegrees, 0.0f);
    Shared.BarrelPitchToleranceDegrees = FMath::Max(BarrelPitchToleranceDegrees, 0.0f);
    Shared.ChassisAimToleranceDegrees = FMath::Max(ChassisAimToleranceDegrees, 0.0f);
    Shared.ChassisTurnSpeedDegreesPerSecond = FMath::Max(ChassisTurnSpeedDegreesPerSecond, 0.0f);
    Shared.MaxFireLinearSpeed = FMath::Max(MaxFireLinearSpeed, 0.0f);
    Shared.MaxFireAngularSpeed = FMath::Max(MaxFireAngularSpeed, 0.0f);
    Shared.BrakeLeadTimeSeconds = FMath::Max(BrakeLeadTimeSeconds, 0.0f);
    Shared.WindupSeconds = FMath::Max(WindupSeconds, 0.0f);
    Shared.RecoverSeconds = FMath::Max(RecoverSeconds, 0.0f);
    Shared.CooldownSeconds = FMath::Max(CooldownSeconds, 0.0f);
    Shared.RecoilNormalizedOnFire = FMath::Clamp(RecoilNormalizedOnFire, 0.0f, 1.0f);
    Shared.TargetPredictionSeconds = FMath::Max(TargetPredictionSeconds, 0.0f);
    Shared.TargetAimOffset = TargetAimOffset;
    Shared.bBrakeImmediatelyWhenTargetInRange = bBrakeImmediatelyWhenTargetInRange;
    Shared.bHoldDuringWindup = bHoldDuringWindup;
    Shared.bHoldDuringRecover = bHoldDuringRecover;
    Shared.bReturnTurretToZeroWhenIdle = bReturnTurretToZeroWhenIdle;
    Shared.bSpawnMassBattleProjectile = bSpawnMassBattleProjectile;
    Shared.ProjectileConfig = ProjectileConfig;
    Shared.ProjectileMultipliers = ProjectileMultipliers;
    Shared.bEmitFireRequest = bEmitFireRequest;
    Shared.bBroadcastBlueprintFireEvent = bBroadcastBlueprintFireEvent;
    return Shared;
}
