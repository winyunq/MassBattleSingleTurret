#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "MBSTSingleTurretTypes.h"
#include "MBSTSingleTurretAsset.generated.h"

class UAnimToTextureDataAsset;
class UStaticMesh;

/** Generated description of one single-agent articulated vehicle mesh. */
UCLASS(BlueprintType)
class MASSBATTLESINGLETURRETRUNTIME_API UMBSTSingleTurretAsset : public UDataAsset
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "Generated")
    TObjectPtr<UStaticMesh> ArticulatedMesh = nullptr;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "Generated")
    TObjectPtr<UAnimToTextureDataAsset> VATDataAsset = nullptr;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "Articulation")
    FVector3f TurretPivotObjectSpace = FVector3f::ZeroVector;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "Articulation")
    FVector3f TurretAxisObjectSpace = FVector3f::UpVector;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "Articulation")
    FVector3f BarrelPivotObjectSpace = FVector3f::ZeroVector;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "Articulation")
    FVector3f BarrelAxisObjectSpace = FVector3f(0.0f, 1.0f, 0.0f);

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "Articulation")
    FVector3f BarrelForwardAxisObjectSpace = FVector3f::ForwardVector;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "Articulation")
    FTransform3f MuzzleTransformObjectSpace = FTransform3f::Identity;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "Limits")
    FVector2f YawLimitsDegrees = FVector2f(-180.0f, 180.0f);

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "Limits")
    FVector2f PitchLimitsDegrees = FVector2f(-15.0f, 45.0f);

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "Articulation", meta = (ClampMin = "0.0"))
    float MaximumRecoilDistance = 0.0f;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "Articulation")
    bool bHasBarrelPitch = true;

    /** Shared by all agents using this layout; not duplicated in each entity fragment. */
    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "Motion", meta = (ClampMin = "0.0"))
    float YawSpeedDegreesPerSecond = 90.0f;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "Motion", meta = (ClampMin = "0.0"))
    float PitchSpeedDegreesPerSecond = 60.0f;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "Motion", meta = (ClampMin = "0.0"))
    float RecoilReturnSpeed = 5.0f;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "VAT")
    bool bBodyUsesVAT = false;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "Generated")
    FString VertexMaskConvention = TEXT("R=YawWeight, G=PitchWeight, B=BodyVATWeight, A=RecoilWeight");

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "Generated")
    TArray<FName> BodySourceComponents;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "Generated")
    TArray<FName> TurretSourceComponents;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "Generated")
    TArray<FName> BarrelSourceComponents;

    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret")
    void ClampAngles(float InYawDegrees, float InPitchDegrees, float& OutYawDegrees, float& OutPitchDegrees) const;

    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret")
    FMBSTSingleTurretPose CalculateWorldPose(
        const FTransform& RootWorldTransform,
        float YawDegrees,
        float PitchDegrees,
        float RecoilNormalized) const;

    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret")
    FTransform CalculateMuzzleWorldTransform(
        const FTransform& RootWorldTransform,
        float YawDegrees,
        float PitchDegrees,
        float RecoilNormalized) const;
};
