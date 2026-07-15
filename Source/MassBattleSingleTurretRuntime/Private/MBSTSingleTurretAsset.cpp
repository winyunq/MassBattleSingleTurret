#include "MBSTSingleTurretAsset.h"

namespace MBSTPosePrivate
{
    static FTransform MakeRotationAboutPivot(const FVector& Pivot, const FVector& Axis, const float Degrees)
    {
        const FVector SafeAxis = Axis.GetSafeNormal(SMALL_NUMBER, FVector::UpVector);
        const FQuat Rotation(SafeAxis, FMath::DegreesToRadians(Degrees));
        const FVector Translation = Pivot - Rotation.RotateVector(Pivot);
        return FTransform(Rotation, Translation, FVector::OneVector);
    }

    static FTransform ToDoubleTransform(const FTransform3f& Value)
    {
        return FTransform(
            FQuat(Value.GetRotation()),
            FVector(Value.GetTranslation()),
            FVector(Value.GetScale3D()));
    }
}

void UMBSTSingleTurretAsset::ClampAngles(
    const float InYawDegrees,
    const float InPitchDegrees,
    float& OutYawDegrees,
    float& OutPitchDegrees) const
{
    const float MinYaw = FMath::Min(YawLimitsDegrees.X, YawLimitsDegrees.Y);
    const float MaxYaw = FMath::Max(YawLimitsDegrees.X, YawLimitsDegrees.Y);
    const float MinPitch = FMath::Min(PitchLimitsDegrees.X, PitchLimitsDegrees.Y);
    const float MaxPitch = FMath::Max(PitchLimitsDegrees.X, PitchLimitsDegrees.Y);

    OutYawDegrees = FMath::Clamp(FMath::UnwindDegrees(InYawDegrees), MinYaw, MaxYaw);
    OutPitchDegrees = bHasBarrelPitch ? FMath::Clamp(InPitchDegrees, MinPitch, MaxPitch) : 0.0f;
}

FMBSTSingleTurretPose UMBSTSingleTurretAsset::CalculateWorldPose(
    const FTransform& RootWorldTransform,
    const float YawDegrees,
    const float PitchDegrees,
    const float RecoilNormalized) const
{
    float ClampedYaw = 0.0f;
    float ClampedPitch = 0.0f;
    ClampAngles(YawDegrees, PitchDegrees, ClampedYaw, ClampedPitch);

    const FVector TurretPivot(TurretPivotObjectSpace);
    const FVector BarrelPivot(BarrelPivotObjectSpace);
    const FVector BarrelForward = FVector(BarrelForwardAxisObjectSpace).GetSafeNormal(SMALL_NUMBER, FVector::ForwardVector);

    const FTransform YawDelta = MBSTPosePrivate::MakeRotationAboutPivot(
        TurretPivot,
        FVector(TurretAxisObjectSpace),
        ClampedYaw);

    const FTransform PitchDelta = bHasBarrelPitch
        ? MBSTPosePrivate::MakeRotationAboutPivot(
            BarrelPivot,
            FVector(BarrelAxisObjectSpace),
            ClampedPitch)
        : FTransform::Identity;

    const float RecoilDistance = FMath::Clamp(RecoilNormalized, 0.0f, 1.0f) * MaximumRecoilDistance;
    const FTransform RecoilDelta(FQuat::Identity, -BarrelForward * RecoilDistance, FVector::OneVector);

    FMBSTSingleTurretPose Pose;
    Pose.TurretPivotWorld = FTransform(FQuat::Identity, TurretPivot, FVector::OneVector) * YawDelta * RootWorldTransform;
    Pose.BarrelPivotWorld = FTransform(FQuat::Identity, BarrelPivot, FVector::OneVector) * PitchDelta * YawDelta * RootWorldTransform;
    Pose.MuzzleWorld =
        MBSTPosePrivate::ToDoubleTransform(MuzzleTransformObjectSpace) *
        RecoilDelta *
        PitchDelta *
        YawDelta *
        RootWorldTransform;

    return Pose;
}

FTransform UMBSTSingleTurretAsset::CalculateMuzzleWorldTransform(
    const FTransform& RootWorldTransform,
    const float YawDegrees,
    const float PitchDegrees,
    const float RecoilNormalized) const
{
    return CalculateWorldPose(RootWorldTransform, YawDegrees, PitchDegrees, RecoilNormalized).MuzzleWorld;
}
