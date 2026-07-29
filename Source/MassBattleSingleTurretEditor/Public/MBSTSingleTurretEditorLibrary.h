#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MBSTSingleTurretAuthoringComponent.h"
#include "MBSTSingleTurretEditorTypes.h"
#include "MBSTSingleTurretEditorLibrary.generated.h"

class AActor;
class AMassBattleAgentRenderer;
class UAnimToTextureDataAsset;
class UMaterial;
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
    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Editor")
    static FMBSTActorToSingleTurretResult ConvertActorToSingleTurretVAT(
        AActor* SourceActor,
        UMBSTSingleTurretAuthoringComponent* Authoring,
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

    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret|Editor")
    static FMBSTAssetValidationResult ValidateGeneratedArticulatedMesh(
        UStaticMesh* ArticulatedMesh,
        bool bExpectBodyVATMask);

    /** Install the rigid-articulation normal path into the MBST VAT base material. */
    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Editor")
    static bool ConfigureArticulationBaseMaterial(
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
