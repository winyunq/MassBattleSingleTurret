#pragma once

#include "CoreMinimal.h"
#include "MBSTMobileFireTypes.h"
#include "MBSTSingleTurretTypes.h"
#include "MBSTSingleTurretEditorTypes.generated.h"

class UAnimToTextureDataAsset;
class UMassBattleAgentConfigDataAsset;
class UMBSTMobileFireProfile;
class UMBSTSingleTurretAsset;
class UStaticMesh;

/** Editor conversion options. The source Actor's current slot/visibility state is used. */
USTRUCT(BlueprintType)
struct MASSBATTLESINGLETURRETEDITOR_API FMBSTActorToSingleTurretSettings
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Gather")
    bool bIncludeAttachedActors = true;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Gather")
    bool bIncludeHiddenComponents = false;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Gather")
    bool bIncludeEditorOnlyComponents = false;

    /** Re-running construction may reset a slot switched just before conversion. */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Gather")
    bool bRerunConstructionScripts = false;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Mesh")
    bool bGenerateLightmapUVs = false;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Mesh", meta = (ClampMin = "0", ClampMax = "7"))
    int32 LightmapDestinationUV = 3;

    /** Conservative symmetric local bounds extension for WPO articulation. */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Mesh", meta = (ClampMin = "0.0"))
    float BoundsExtension = 50.0f;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Mesh")
    bool bCreatePivotSockets = true;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "VAT")
    bool bCreateVATDataAsset = false;

    /** Template carries animation list, mode, sample rate, precision and UV setup. */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "VAT", meta = (EditCondition = "bCreateVATDataAsset"))
    TObjectPtr<UAnimToTextureDataAsset> VATTemplateDataAsset = nullptr;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "VAT", meta = (EditCondition = "bCreateVATDataAsset"))
    bool bRunVATBake = false;

    /**
     * Optional normal-unit template used only to inherit movement, combat and
     * other MassBattle settings. Actor conversion always creates a new config
     * and always embeds the turret Tag/State/Shared data; when this is empty the
     * new config starts from MassBattle defaults.
     */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Unit")
    TObjectPtr<UMassBattleAgentConfigDataAsset> AgentConfigTemplate = nullptr;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Unit")
    FMBSTSingleTurretState InitialTurretState;

    /**
     * Optional per-unit attack-move profile. When assigned, Actor conversion
     * embeds the plugin-owned MobileFire Tag/State/Shared contract into the new
     * AgentConfig and applies the profile's built-in attack/chase compatibility flags.
     */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Mobile Fire")
    TObjectPtr<UMBSTMobileFireProfile> MobileFireProfile = nullptr;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Mobile Fire", meta = (EditCondition = "MobileFireProfile != nullptr", EditConditionHides))
    FMBSTMobileFireState InitialMobileFireState;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Diagnostics")
    bool bKeepIntermediateMeshes = false;
};

USTRUCT(BlueprintType)
struct MASSBATTLESINGLETURRETEDITOR_API FMBSTActorToSingleTurretResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Result")
    bool bSucceeded = false;

    UPROPERTY(BlueprintReadOnly, Category = "Result")
    TObjectPtr<UStaticMesh> ArticulatedMesh = nullptr;

    UPROPERTY(BlueprintReadOnly, Category = "Result")
    TObjectPtr<UMBSTSingleTurretAsset> LayoutAsset = nullptr;

    UPROPERTY(BlueprintReadOnly, Category = "Result")
    TObjectPtr<UAnimToTextureDataAsset> VATDataAsset = nullptr;

    /** A normal MassBattle AgentConfig whose ExtraData already carries turret support. */
    UPROPERTY(BlueprintReadOnly, Category = "Result")
    TObjectPtr<UMassBattleAgentConfigDataAsset> AgentConfig = nullptr;

    UPROPERTY(BlueprintReadOnly, Category = "Result")
    TArray<FString> Messages;

    UPROPERTY(BlueprintReadOnly, Category = "Result")
    TArray<FName> BodyComponents;

    UPROPERTY(BlueprintReadOnly, Category = "Result")
    TArray<FName> TurretComponents;

    UPROPERTY(BlueprintReadOnly, Category = "Result")
    TArray<FName> BarrelComponents;
};

USTRUCT(BlueprintType)
struct MASSBATTLESINGLETURRETEDITOR_API FMBSTAssetValidationResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Validation")
    bool bValid = false;

    UPROPERTY(BlueprintReadOnly, Category = "Validation")
    TArray<FString> Messages;
};
