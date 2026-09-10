#include "MBSTSingleTurretAsset.h"

#include "Fragments/Collider.h"
#include "Fragments/Move.h"
#include "Fragments/Render.h"
#include "Fragments/Transform.h"
#include "Renderers/MassBattleAgentRenderer.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

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

FTransform UMBSTSingleTurretAsset::CalculateMeshRootWorldTransform(
    const FLocating& Location,
    const FRotating& Rotation,
    const FScaling& Scaling,
    const FCollider* Collider,
    const FMove* Move,
    const FMoving* Moving,
    const FVisualize* Visualize)
{
    // Same mesh-root convention as MassBattleAgentRenderProcessor. Do not read
    // FVisualizing.Target*: that cache depends on visibility and render timing.
    FTransform Combined = Visualize ? FTransform(Visualize->Transform) : FTransform::Identity;
    if (Visualize && !Visualize->RendererClass.IsNull())
    {
        if (UClass* RendererClass = Visualize->RendererClass.LoadSynchronous())
        {
            Combined = Combined * RendererClass->GetDefaultObject<AMassBattleAgentRenderer>()->Offset;
        }
    }

    const FQuat EntityRotation(Rotation.RotationQuat);
    FQuat Facing = EntityRotation;
    if (Move && Moving)
    {
        if (Move->Tilt.TiltMode == ETiltMode::OnlyMesh)
        {
            Facing = Facing * FQuat(Moving->CurrentTilt);
        }
        else if (Move->Tilt.TiltMode == ETiltMode::OnlyCollider)
        {
            Facing = Facing * FQuat(Moving->CurrentTilt.Inverse());
        }
    }

    FVector MeshLocation = Location.Location;
    if (Collider)
    {
        const float HalfHeight = (Collider->Height * 0.5f + Collider->Radius) * Scaling.Scale;
        const FQuat PhysicsRotation = EntityRotation * FQuat(Collider->RelativeRotation.Quaternion());
        MeshLocation -= PhysicsRotation.RotateVector(FVector(0.0, 0.0, HalfHeight));
    }
    MeshLocation += Facing.RotateVector(Combined.GetLocation());
    return FTransform(
        Facing * Combined.GetRotation(),
        MeshLocation,
        Combined.GetScale3D() * Scaling.Scale * FVector(Scaling.JiggleMultiplier));
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

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMBSTMeshRootAimTest,
    "MassBattle.SingleTurret.MobileFire.MeshRootAim",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMBSTMeshRootAimTest::RunTest(const FString& Parameters)
{
    FLocating Location;
    Location.Location = FVector(100.0, 200.0, 32.0);
    FRotating Rotation;
    FScaling Scaling;
    Scaling.Scale = 0.1f;
    FCollider Collider;
    Collider.Radius = 320.0f;
    Collider.Height = 0.0f;
    FMove Move;
    FMoving Moving;
    FVisualize Visualize;

    const FTransform Flat = UMBSTSingleTurretAsset::CalculateMeshRootWorldTransform(
        Location, Rotation, Scaling, &Collider, &Move, &Moving, &Visualize);
    TestTrue(TEXT("Formal tank mesh rests at ground, not capsule center"),
        Flat.GetLocation().Equals(FVector(100.0, 200.0, 0.0), 0.001));

    UMBSTSingleTurretAsset* Layout = NewObject<UMBSTSingleTurretAsset>();
    Layout->BarrelPivotObjectSpace = FVector3f(0.0f, 0.0f, 160.0f);
    Layout->MuzzleTransformObjectSpace.SetTranslation(FVector3f(200.0f, 0.0f, 160.0f));
    const FVector Target(300.0, 200.0, 32.0);
    // Analytic line from the visible hinge (100,200,16) to target (300,200,32).
    const float CorrectPitch = -FMath::RadiansToDegrees(FMath::Atan2(16.0f, 200.0f));
    const FTransform Muzzle = Layout->CalculateMuzzleWorldTransform(Flat, 0.0f, CorrectPitch, 0.0f);
    TestTrue(TEXT("Visible barrel points at target rather than 32cm below it"),
        Muzzle.GetRotation().GetForwardVector().Equals((Target - Muzzle.GetLocation()).GetSafeNormal(), 0.0001));

    Rotation.RotationQuat = FQuat4f(FRotator(45.0, 70.0, 12.0).Quaternion());
    Collider.RelativeRotation = FRotator3f(10.0f, 0.0f, 0.0f);
    Collider.Height = 200.0f;
    Visualize.Transform.SetTranslation(FVector3f(8.0f, -4.0f, 3.0f));
    Visualize.Transform.SetRotation(FQuat4f(FRotator(0.0, 25.0, 0.0).Quaternion()));
    Move.Tilt.TiltMode = ETiltMode::OnlyMesh;
    Moving.CurrentTilt = FQuat4f(FRotator(15.0, 0.0, 0.0).Quaternion());
    const FQuat Facing = FQuat(Rotation.RotationQuat) * FQuat(Moving.CurrentTilt);
    const FQuat Physics = FQuat(Rotation.RotationQuat) * FQuat(Collider.RelativeRotation.Quaternion());
    const FVector ExpectedFoot = Location.Location - Physics.GetUpVector() * 42.0;
    const FTransform Tilted = UMBSTSingleTurretAsset::CalculateMeshRootWorldTransform(
        Location, Rotation, Scaling, &Collider, &Move, &Moving, &Visualize);
    TestTrue(TEXT("Tilted capsule foot follows local up, including relative collider rotation"),
        Tilted.GetLocation().Equals(ExpectedFoot + Facing.RotateVector(FVector(8.0, -4.0, 3.0)), 0.001));
    TestTrue(TEXT("Visual rotation composes after mesh tilt"),
        Tilted.GetRotation().Equals(Facing * FQuat(Visualize.Transform.GetRotation()), 0.0001));
    return true;
}
#endif
