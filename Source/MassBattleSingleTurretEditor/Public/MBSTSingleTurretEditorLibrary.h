#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MBSTSingleTurretAuthoringComponent.h"
#include "MBSTSingleTurretEditorTypes.h"
#include "MBSTSingleTurretEditorLibrary.generated.h"

class AActor;
class AMassBattleAgentRenderer;
class UAnimationAsset;
class UAnimToTextureDataAsset;
class UMaterial;
class UMaterialInterface;
class UMaterialInstanceConstant;
class UMassBattleAgentConfigDataAsset;
class UMBSTMobileFireProfile;
class UMBSTSingleTurretAsset;
class UMBSTSingleTurretAuthoringComponent;
class UNiagaraSystem;
class USceneComponent;
class USkeletalMeshComponent;
class UStaticMesh;

/** Standalone editor API; no MassBattle source file is modified. */
UCLASS()
class MASSBATTLESINGLETURRETEDITOR_API UMBSTSingleTurretEditorLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /**
     * Add a registered authoring component to a transient source Actor.
     * This keeps Python/commandlet authoring on the same supported component
     * ownership path on engine versions that do not expose AddInstanceComponent.
     */
    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Editor")
    static UMBSTSingleTurretAuthoringComponent* AddTransientAuthoringComponent(
        AActor* SourceActor,
        FString& OutMessage);

    /** Evaluate and freeze a skeletal component at an authored animation pose before mesh conversion. */
    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Editor")
    static bool PrepareSkeletalComponentPoseForConversion(
        USkeletalMeshComponent* SkeletalComponent,
        UAnimationAsset* Animation,
        float TimeSeconds,
        FString& OutMessage);

    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Editor")
    static FMBSTActorToSingleTurretResult ConvertActorToSingleTurretVAT(
        AActor* SourceActor,
        UMBSTSingleTurretAuthoringComponent* Authoring,
        const FString& PackagePath,
        const FString& AssetName,
        const FMBSTActorToSingleTurretSettings& Settings);

    // Production authoring entry for normalized mechanical units whose body,
    // turret and optional barrel meshes share one object-space coordinate system.
    // The transient assembly exists only while assets are generated; runtime
    // units depend solely on the resulting AgentConfig, layout and renderer.
    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Editor")
    static FMBSTActorToSingleTurretResult ConvertStaticMeshesToSingleTurret(
        UStaticMesh* BodyMesh,
        UStaticMesh* TurretMesh,
        UStaticMesh* BarrelMesh,
        UMaterialInterface* MaterialOverride,
        FVector TurretPivotObjectSpace,
        FVector BarrelPivotObjectSpace,
        FVector MuzzleObjectSpace,
        float MaximumRecoilDistance,
        float YawSpeedDegreesPerSecond,
        float PitchSpeedDegreesPerSecond,
        const FString& PackagePath,
        const FString& AssetName,
        const FMBSTActorToSingleTurretSettings& Settings);

    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret|Editor")
    static FMBSTAssetValidationResult ValidateActorForSingleTurretConversion(
        AActor* SourceActor,
        UMBSTSingleTurretAuthoringComponent* Authoring,
        const FMBSTActorToSingleTurretSettings& Settings);

    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Editor")
    static void SetComponentArticulationRole(
        USceneComponent* Component,
        EMBSTArticulationNodeRole Role,
        bool bEnabled = true);

    /** Ensure the MassBattle renderer can upload the packed turret state. */
    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Editor")
    static bool EnsureNiagaraStyleArray(UNiagaraSystem* NiagaraSystem, FString& OutMessage);

    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret|Editor")
    static FMBSTAssetValidationResult ValidateNiagaraStyleArray(UNiagaraSystem* NiagaraSystem);

    /** Move packed-state decode and trigonometry from the material vertex path to one GPU particle evaluation. */
    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Editor")
    static bool ConfigureNiagaraPrecomputedArticulation(
        UNiagaraSystem* NiagaraSystem,
        FString& OutMessage);

    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret|Editor")
    static FMBSTAssetValidationResult ValidateGeneratedArticulatedMesh(
        UStaticMesh* ArticulatedMesh,
        bool bExpectBodyVATMask);

    /** Install the rigid-articulation normal path into the MBST VAT base material. */
    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Editor")
    static bool ConfigureArticulationBaseMaterial(
        UMaterial* Material,
        FString& OutMessage);

    /** Consume precomputed articulation parameters from DynamicMaterialParameter1. */
    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Editor")
    static bool ConfigurePrecomputedArticulationMaterial(
        UMaterial* Material,
        FString& OutMessage);

    /** Default to a compiled rigid-normal path; opt into articulated normals only for close quality variants. */
    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Editor")
    static bool ConfigurePerformanceNormalSwitch(
        UMaterial* Material,
        FString& OutMessage);

    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Editor")
    static bool ConfigureArticulationMaterialInstance(
        UMaterialInstanceConstant* MaterialInstance,
        UMBSTSingleTurretAsset* Layout,
        FString& OutMessage);

    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Editor")
    static bool ConfigureMassBattleRendererClass(
        TSubclassOf<AMassBattleAgentRenderer> RendererClass,
        UMBSTSingleTurretAsset* Layout,
        UNiagaraSystem* NiagaraSystem,
        FString& OutMessage);

    /** Assign the configured renderer class to a MassBattle Agent Config DataAsset. */
    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Editor")
    static bool ConfigureAgentConfigRenderer(
        UMassBattleAgentConfigDataAsset* AgentConfig,
        TSubclassOf<AMassBattleAgentRenderer> RendererClass,
        FString& OutMessage);

    /** Add, replace, or remove the per-unit attack-move contract in ExtraData. */
    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Editor")
    static bool ConfigureAgentConfigMobileFire(
        UMassBattleAgentConfigDataAsset* AgentConfig,
        UMBSTMobileFireProfile* Profile,
        const FMBSTMobileFireState& InitialState,
        bool bEnable,
        FString& OutMessage);

private:
    /** Internal finalization step used by Actor-to-single-turret conversion. */
    static bool ConfigureAgentConfigSingleTurret(
        UMassBattleAgentConfigDataAsset* AgentConfig,
        UMBSTSingleTurretAsset* Layout,
        const FMBSTSingleTurretState& InitialState,
        bool bEnable,
        FString& OutMessage);

    /** Internal asset step; Actor conversion always invokes this automatically. */
    static UMassBattleAgentConfigDataAsset* CreateSingleTurretAgentConfigFromTemplate(
        UMassBattleAgentConfigDataAsset* OptionalTemplate,
        UMBSTSingleTurretAsset* Layout,
        const FString& PackagePath,
        const FString& AssetName,
        const FMBSTSingleTurretState& InitialState,
        UMBSTMobileFireProfile* MobileFireProfile,
        const FMBSTMobileFireState& InitialMobileFireState,
        FString& OutMessage);

public:
    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret|Editor")
    static FMBSTAssetValidationResult ValidateAgentConfigSingleTurret(
        const UMassBattleAgentConfigDataAsset* AgentConfig,
        const UMBSTSingleTurretAsset* ExpectedLayout = nullptr);

    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret|Editor")
    static FMBSTAssetValidationResult ValidateAgentConfigMobileFire(
        const UMassBattleAgentConfigDataAsset* AgentConfig,
        const UMBSTMobileFireProfile* ExpectedProfile = nullptr);

    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Editor")
    static bool ConfigureAndBakeAnimToTexture(
        UAnimToTextureDataAsset* DataAsset,
        USkeletalMeshComponent* DriverComponent,
        UStaticMesh* ArticulatedMesh,
        bool bRunBake,
        FString& OutMessage);
};
