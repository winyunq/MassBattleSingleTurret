#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/EngineTypes.h"
#include "MBSTSingleTurretAuthoringComponent.generated.h"

class USkeletalMeshComponent;

UENUM(BlueprintType)
enum class EMBSTArticulationNodeRole : uint8
{
    TurretYaw      UMETA(DisplayName = "Turret Yaw Pivot"),
    BarrelPitch    UMETA(DisplayName = "Barrel Pitch Pivot"),
    Muzzle         UMETA(DisplayName = "Muzzle"),
    IgnoreSubtree  UMETA(DisplayName = "Ignore Subtree")
};

/** Optional scene marker. Parent the relevant mesh subtree below it. */
UCLASS(ClassGroup = (MassBattle), BlueprintType, Blueprintable,
    meta = (BlueprintSpawnableComponent, DisplayName = "MBST Articulation Node Marker"))
class MASSBATTLESINGLETURRETRUNTIME_API UMBSTArticulationNodeMarkerComponent : public USceneComponent
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "MassBattle|Single Turret")
    EMBSTArticulationNodeRole Role = EMBSTArticulationNodeRole::TurretYaw;
};

/** Selects existing Actor hierarchy nodes consumed by the converter. */
UCLASS(ClassGroup = (MassBattle), BlueprintType, Blueprintable,
    meta = (BlueprintSpawnableComponent, DisplayName = "MBST Single Turret Authoring"))
class MASSBATTLESINGLETURRETRUNTIME_API UMBSTSingleTurretAuthoringComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UMBSTSingleTurretAuthoringComponent();

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Nodes", meta = (UseComponentPicker, AllowAnyActor))
    FComponentReference TurretYawPivot;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Nodes", meta = (UseComponentPicker, AllowAnyActor))
    FComponentReference BarrelPitchPivot;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Nodes", meta = (UseComponentPicker, AllowAnyActor))
    FComponentReference Muzzle;

    /** Optional SkeletalMeshComponent that drives body AnimToTexture. */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "VAT", meta = (UseComponentPicker, AllowAnyActor))
    FComponentReference VATDriver;

    /** Axis in the selected turret pivot component's local space. */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Axes")
    FVector TurretLocalAxis = FVector::UpVector;

    /** Axis in the selected barrel pivot component's local space. */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Axes")
    FVector BarrelLocalAxis = FVector::YAxisVector;

    /** Barrel forward axis in barrel-pivot local space. Recoil moves backward. */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Axes")
    FVector BarrelLocalForwardAxis = FVector::ForwardVector;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Limits")
    FVector2D YawLimitsDegrees = FVector2D(-180.0, 180.0);

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Limits")
    FVector2D PitchLimitsDegrees = FVector2D(-15.0, 45.0);

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Limits", meta = (ClampMin = "0.0"))
    float MaximumRecoilDistance = 0.0f;

    /** Shared by all generated Agents using this layout. A value <= 0 snaps immediately. */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Motion", meta = (ClampMin = "0.0"))
    float YawSpeedDegreesPerSecond = 90.0f;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Motion", meta = (ClampMin = "0.0"))
    float PitchSpeedDegreesPerSecond = 60.0f;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Motion", meta = (ClampMin = "0.0"))
    float RecoilReturnSpeed = 5.0f;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "VAT")
    bool bBodyUsesVAT = false;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Fallback Tags")
    FName TurretYawTag = TEXT("MBST_TurretYaw");

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Fallback Tags")
    FName BarrelPitchTag = TEXT("MBST_BarrelPitch");

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Fallback Tags")
    FName MuzzleTag = TEXT("MBST_Muzzle");

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Fallback Tags")
    FName VATDriverTag = TEXT("MBST_VATDriver");

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Fallback Tags")
    FName IgnoreTag = TEXT("MBST_Ignore");

    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret")
    USceneComponent* ResolveTurretYawPivot() const;

    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret")
    USceneComponent* ResolveBarrelPitchPivot() const;

    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret")
    USceneComponent* ResolveMuzzle() const;

    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret")
    USkeletalMeshComponent* ResolveVATDriver() const;

    UFUNCTION(BlueprintCallable, CallInEditor, Category = "MassBattle|Single Turret")
    void ApplyFallbackTagsToSelectedNodes();

    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret")
    TArray<FString> ValidateAuthoring() const;

private:
    USceneComponent* ResolveSceneReference(const FComponentReference& Reference, FName FallbackTag) const;
};
