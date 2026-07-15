#include "MBSTSingleTurretBlueprintLibrary.h"

#include "MBSTSingleTurretAsset.h"
#include "DataAssets/MassBattleAgentConfigDataAsset.h"
#include "Fragments/StyleType.h"
#include "Fragments/Transform.h"
#include "FuncLibs/MassBattleFuncLib.h"
#include "MassAPISubsystem.h"
#include "MassEntityTemplate.h"

namespace MBSTBlueprintPrivate
{
    static void FillSharedLayout(FMBSTSingleTurretShared& OutShared, UMBSTSingleTurretAsset* Layout)
    {
        OutShared.Layout = Layout;
        if (Layout)
        {
            OutShared.YawLimitsDegrees = Layout->YawLimitsDegrees;
            OutShared.PitchLimitsDegrees = Layout->PitchLimitsDegrees;
            OutShared.bHasBarrelPitch = Layout->bHasBarrelPitch;
            OutShared.YawSpeedDegreesPerSecond = Layout->YawSpeedDegreesPerSecond;
            OutShared.PitchSpeedDegreesPerSecond = Layout->PitchSpeedDegreesPerSecond;
            OutShared.RecoilReturnSpeed = Layout->RecoilReturnSpeed;
        }
    }

    static bool ResolveRuntimeData(
        const UObject* WorldContextObject,
        const FEntityHandle& Agent,
        UMassAPISubsystem*& OutMassAPI,
        FMBSTSingleTurretState*& OutState,
        FMBSTSingleTurretShared*& OutShared)
    {
        OutMassAPI = UMassAPISubsystem::GetPtr(WorldContextObject);
        OutState = nullptr;
        OutShared = nullptr;

        if (!OutMassAPI || !OutMassAPI->IsValid(Agent))
        {
            return false;
        }

        OutState = OutMassAPI->GetFragmentPtr<FMBSTSingleTurretState>(Agent);
        OutShared = OutMassAPI->GetSharedFragmentPtr<FMBSTSingleTurretShared>(Agent);
        return OutState != nullptr && OutShared != nullptr && IsValid(OutShared->Layout);
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

    static bool GetRootWorldTransform(
        UMassAPISubsystem& MassAPI,
        const FEntityHandle& Agent,
        FTransform& OutRootTransform)
    {
        const FLocating* Location = MassAPI.GetFragmentPtr<FLocating>(Agent);
        const FRotating* Rotation = MassAPI.GetFragmentPtr<FRotating>(Agent);
        if (!Location || !Rotation)
        {
            return false;
        }

        float UniformScale = 1.0f;
        if (const FScaling* Scaling = MassAPI.GetFragmentPtr<FScaling>(Agent))
        {
            UniformScale = Scaling->Scale;
        }

        const FQuat RootRotation(
            static_cast<double>(Rotation->RotationQuat.X),
            static_cast<double>(Rotation->RotationQuat.Y),
            static_cast<double>(Rotation->RotationQuat.Z),
            static_cast<double>(Rotation->RotationQuat.W));

        OutRootTransform = FTransform(
            RootRotation,
            Location->Location,
            FVector(UniformScale));
        return true;
    }
}

FEntityTemplateData UMBSTSingleTurretBlueprintLibrary::MakeSingleTurretTemplateFromAgentConfig(
    const UObject* WorldContextObject,
    const UMassBattleAgentConfigDataAsset* BaseAgentConfig,
    UMBSTSingleTurretAsset* Layout,
    const FMBSTSingleTurretState& InitialState)
{
    if (!BaseAgentConfig)
    {
        return FEntityTemplateData();
    }

    const FEntityTemplateData BaseTemplate = UMassBattleFuncLib::MakeTemplateDataFromDataAsset(
        WorldContextObject,
        BaseAgentConfig);

    return AddSingleTurretToTemplate(WorldContextObject, BaseTemplate, Layout, InitialState);
}

FEntityTemplateData UMBSTSingleTurretBlueprintLibrary::AddSingleTurretToTemplate(
    const UObject* WorldContextObject,
    const FEntityTemplateData& BaseTemplate,
    UMBSTSingleTurretAsset* Layout,
    const FMBSTSingleTurretState& InitialState)
{
    UMassAPISubsystem* MassAPI = UMassAPISubsystem::GetPtr(WorldContextObject);
    if (!MassAPI || !MassAPI->GetEntityManager() || !BaseTemplate.IsValid() || !BaseTemplate.Get() || !IsValid(Layout))
    {
        return FEntityTemplateData();
    }

    FMassEntityTemplateData ClonedTemplate = UMassAPISubsystem::CloneTemplate(*BaseTemplate.Get());
    UMassAPISubsystem::AddTag<FMBSTSingleTurretTag>(ClonedTemplate);
    UMassAPISubsystem::SetFragment<FMBSTSingleTurretState>(ClonedTemplate, InitialState);

    FStyleType InitialStyle;
    InitialStyle.Index = MBSTPacking::Pack(
        static_cast<int32>(InitialState.VisualStyle),
        InitialState.CurrentYawDegrees,
        InitialState.CurrentPitchDegrees,
        InitialState.RecoilNormalized);
    UMassAPISubsystem::SetFragment<FStyleType>(ClonedTemplate, InitialStyle);

    FMBSTSingleTurretShared Shared;
    MBSTBlueprintPrivate::FillSharedLayout(Shared, Layout);
    UMassAPISubsystem::SetSharedFragment<FMBSTSingleTurretShared>(
        ClonedTemplate,
        Shared,
        *MassAPI->GetEntityManager());

    return FEntityTemplateData(MakeShared<FMassEntityTemplateData>(MoveTemp(ClonedTemplate)));
}

bool UMBSTSingleTurretBlueprintLibrary::AddSingleTurretToExistingAgent(
    const UObject* WorldContextObject,
    const FEntityHandle& Agent,
    UMBSTSingleTurretAsset* Layout,
    const FMBSTSingleTurretState& InitialState)
{
    UMassAPISubsystem* MassAPI = UMassAPISubsystem::GetPtr(WorldContextObject);
    if (!MassAPI || !MassAPI->IsValid(Agent) || !IsValid(Layout))
    {
        return false;
    }

    if (FMBSTSingleTurretState* ExistingState = MassAPI->GetFragmentPtr<FMBSTSingleTurretState>(Agent))
    {
        *ExistingState = InitialState;
    }
    else
    {
        MassAPI->AddFragment<FMBSTSingleTurretState>(Agent, InitialState);
    }

    FStyleType InitialStyle;
    InitialStyle.Index = MBSTPacking::Pack(
        static_cast<int32>(InitialState.VisualStyle),
        InitialState.CurrentYawDegrees,
        InitialState.CurrentPitchDegrees,
        InitialState.RecoilNormalized);

    if (FStyleType* ExistingStyle = MassAPI->GetFragmentPtr<FStyleType>(Agent))
    {
        *ExistingStyle = InitialStyle;
    }
    else
    {
        MassAPI->AddFragment<FStyleType>(Agent, InitialStyle);
    }

    if (!MassAPI->HasTag<FMBSTSingleTurretTag>(Agent))
    {
        MassAPI->AddTag<FMBSTSingleTurretTag>(Agent);
    }

    if (MassAPI->HasSharedFragment<FMBSTSingleTurretShared>(Agent))
    {
        MassAPI->RemoveSharedFragment<FMBSTSingleTurretShared>(Agent);
    }

    FMBSTSingleTurretShared Shared;
    MBSTBlueprintPrivate::FillSharedLayout(Shared, Layout);
    return MassAPI->AddSharedFragment<FMBSTSingleTurretShared>(Agent, Shared);
}

bool UMBSTSingleTurretBlueprintLibrary::SetTurretTargetAngles(
    const UObject* WorldContextObject,
    const FEntityHandle& Agent,
    const float TargetYawDegrees,
    const float TargetPitchDegrees,
    const bool bSnap)
{
    UMassAPISubsystem* MassAPI = nullptr;
    FMBSTSingleTurretState* State = nullptr;
    FMBSTSingleTurretShared* Shared = nullptr;
    if (!MBSTBlueprintPrivate::ResolveRuntimeData(WorldContextObject, Agent, MassAPI, State, Shared))
    {
        return false;
    }

    float ClampedYaw = 0.0f;
    float ClampedPitch = 0.0f;
    Shared->Layout->ClampAngles(TargetYawDegrees, TargetPitchDegrees, ClampedYaw, ClampedPitch);

    State->TargetYawDegrees = ClampedYaw;
    State->TargetPitchDegrees = ClampedPitch;
    if (bSnap)
    {
        State->CurrentYawDegrees = ClampedYaw;
        State->CurrentPitchDegrees = ClampedPitch;
    }
    return true;
}

bool UMBSTSingleTurretBlueprintLibrary::AimTurretAtWorldLocation(
    const UObject* WorldContextObject,
    const FEntityHandle& Agent,
    const FVector& TargetWorldLocation,
    const bool bSnap)
{
    UMassAPISubsystem* MassAPI = nullptr;
    FMBSTSingleTurretState* State = nullptr;
    FMBSTSingleTurretShared* Shared = nullptr;
    if (!MBSTBlueprintPrivate::ResolveRuntimeData(WorldContextObject, Agent, MassAPI, State, Shared))
    {
        return false;
    }

    FTransform RootWorldTransform;
    if (!MBSTBlueprintPrivate::GetRootWorldTransform(*MassAPI, Agent, RootWorldTransform))
    {
        return false;
    }

    const UMBSTSingleTurretAsset* Layout = Shared->Layout;
    const FVector TargetObject = RootWorldTransform.InverseTransformPosition(TargetWorldLocation);

    const FVector TurretAxis = FVector(Layout->TurretAxisObjectSpace).GetSafeNormal(SMALL_NUMBER, FVector::UpVector);
    const FVector BarrelAxis = FVector(Layout->BarrelAxisObjectSpace).GetSafeNormal(SMALL_NUMBER, FVector::YAxisVector);
    const FVector DefaultForward = FVector(Layout->BarrelForwardAxisObjectSpace).GetSafeNormal(SMALL_NUMBER, FVector::ForwardVector);
    const FVector TurretPivot(Layout->TurretPivotObjectSpace);
    const FVector BarrelPivot(Layout->BarrelPivotObjectSpace);

    const FVector TargetFromTurret = TargetObject - TurretPivot;
    float DesiredYaw = MBSTBlueprintPrivate::SignedAngleDegrees(DefaultForward, TargetFromTurret, TurretAxis);

    float IgnoredPitch = 0.0f;
    Layout->ClampAngles(DesiredYaw, 0.0f, DesiredYaw, IgnoredPitch);

    const FQuat YawRotation(TurretAxis, FMath::DegreesToRadians(DesiredYaw));
    const FVector YawedForward = YawRotation.RotateVector(DefaultForward);
    const FVector YawedPitchAxis = YawRotation.RotateVector(BarrelAxis);
    const FVector YawedBarrelPivot = TurretPivot + YawRotation.RotateVector(BarrelPivot - TurretPivot);
    const FVector TargetFromBarrel = TargetObject - YawedBarrelPivot;

    const float DesiredPitch = Layout->bHasBarrelPitch
        ? MBSTBlueprintPrivate::SignedAngleDegrees(YawedForward, TargetFromBarrel, YawedPitchAxis)
        : 0.0f;

    return SetTurretTargetAngles(
        WorldContextObject,
        Agent,
        DesiredYaw,
        DesiredPitch,
        bSnap);
}

bool UMBSTSingleTurretBlueprintLibrary::TriggerTurretRecoil(
    const UObject* WorldContextObject,
    const FEntityHandle& Agent,
    const float NormalizedAmount)
{
    UMassAPISubsystem* MassAPI = nullptr;
    FMBSTSingleTurretState* State = nullptr;
    FMBSTSingleTurretShared* Shared = nullptr;
    if (!MBSTBlueprintPrivate::ResolveRuntimeData(WorldContextObject, Agent, MassAPI, State, Shared))
    {
        return false;
    }

    State->RecoilNormalized = FMath::Clamp(NormalizedAmount, 0.0f, 1.0f);
    return true;
}

bool UMBSTSingleTurretBlueprintLibrary::SetTurretVisualStyle(
    const UObject* WorldContextObject,
    const FEntityHandle& Agent,
    const int32 VisualStyle)
{
    UMassAPISubsystem* MassAPI = nullptr;
    FMBSTSingleTurretState* State = nullptr;
    FMBSTSingleTurretShared* Shared = nullptr;
    if (!MBSTBlueprintPrivate::ResolveRuntimeData(WorldContextObject, Agent, MassAPI, State, Shared))
    {
        return false;
    }

    State->VisualStyle = static_cast<uint8>(FMath::Clamp(VisualStyle, 0, 255));
    return true;
}

bool UMBSTSingleTurretBlueprintLibrary::GetSingleTurretState(
    const UObject* WorldContextObject,
    const FEntityHandle& Agent,
    FMBSTSingleTurretState& OutState)
{
    UMassAPISubsystem* MassAPI = UMassAPISubsystem::GetPtr(WorldContextObject);
    if (!MassAPI || !MassAPI->IsValid(Agent))
    {
        return false;
    }

    const FMBSTSingleTurretState* State = MassAPI->GetFragmentPtr<FMBSTSingleTurretState>(Agent);
    if (!State)
    {
        return false;
    }

    OutState = *State;
    return true;
}

bool UMBSTSingleTurretBlueprintLibrary::GetSingleTurretPose(
    const UObject* WorldContextObject,
    const FEntityHandle& Agent,
    FMBSTSingleTurretPose& OutPose)
{
    UMassAPISubsystem* MassAPI = nullptr;
    FMBSTSingleTurretState* State = nullptr;
    FMBSTSingleTurretShared* Shared = nullptr;
    if (!MBSTBlueprintPrivate::ResolveRuntimeData(WorldContextObject, Agent, MassAPI, State, Shared))
    {
        return false;
    }

    FTransform RootWorldTransform;
    if (!MBSTBlueprintPrivate::GetRootWorldTransform(*MassAPI, Agent, RootWorldTransform))
    {
        return false;
    }

    OutPose = Shared->Layout->CalculateWorldPose(
        RootWorldTransform,
        State->CurrentYawDegrees,
        State->CurrentPitchDegrees,
        State->RecoilNormalized);
    return true;
}

int32 UMBSTSingleTurretBlueprintLibrary::PackSingleTurretRenderState(
    const int32 VisualStyle,
    const float YawDegrees,
    const float PitchDegrees,
    const float RecoilNormalized)
{
    return MBSTPacking::Pack(VisualStyle, YawDegrees, PitchDegrees, RecoilNormalized);
}

void UMBSTSingleTurretBlueprintLibrary::UnpackSingleTurretRenderState(
    const int32 PackedState,
    int32& OutVisualStyle,
    float& OutYawDegrees,
    float& OutPitchDegrees,
    float& OutRecoilNormalized)
{
    MBSTPacking::Unpack(
        PackedState,
        OutVisualStyle,
        OutYawDegrees,
        OutPitchDegrees,
        OutRecoilNormalized);
}
