#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MassAPIStructs.h"
#include "MBSTSingleTurretTypes.h"
#include "MBSTMobileFireTypes.h"
#include "MBSTSingleTurretBlueprintLibrary.generated.h"

class UMassBattleAgentConfigDataAsset;
class UMBSTSingleTurretAsset;
class UMBSTMobileFireProfile;

/** Runtime entry points; all changes live in this plugin rather than MassBattle source. */
UCLASS()
class MASSBATTLESINGLETURRETRUNTIME_API UMBSTSingleTurretBlueprintLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:

    /** Creates the articulated template and installs the plugin-owned mobile-fire state machine in one step. */
    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Mobile Fire", meta = (WorldContext = "WorldContextObject"))
    static FEntityTemplateData MakeMobileFireSingleTurretTemplateFromAgentConfig(
        const UObject* WorldContextObject,
        const UMassBattleAgentConfigDataAsset* BaseAgentConfig,
        UMBSTSingleTurretAsset* Layout,
        const FMBSTSingleTurretState& InitialTurretState,
        UMBSTMobileFireProfile* FireProfile,
        const FMBSTMobileFireState& InitialFireState);

    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Mobile Fire", meta = (WorldContext = "WorldContextObject"))
    static FEntityTemplateData AddMobileFireToTemplate(
        const UObject* WorldContextObject,
        const FEntityTemplateData& BaseTemplate,
        UMBSTMobileFireProfile* FireProfile,
        const FMBSTMobileFireState& InitialFireState);

    /** One-time convenience; bulk spawning should add mobile fire to the template. */
    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Mobile Fire", meta = (WorldContext = "WorldContextObject"))
    static bool AddMobileFireToExistingAgent(
        const UObject* WorldContextObject,
        const FEntityHandle& Agent,
        UMBSTMobileFireProfile* FireProfile,
        const FMBSTMobileFireState& InitialFireState);

    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Mobile Fire", meta = (WorldContext = "WorldContextObject"))
    static bool SetMobileFireEnabled(
        const UObject* WorldContextObject,
        const FEntityHandle& Agent,
        bool bEnabled);

    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Mobile Fire", meta = (WorldContext = "WorldContextObject"))
    static bool SetMobileFireTargetEntity(
        const UObject* WorldContextObject,
        const FEntityHandle& Agent,
        const FEntityHandle& TargetEntity);

    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Mobile Fire", meta = (WorldContext = "WorldContextObject"))
    static bool SetMobileFireTargetWorldLocation(
        const UObject* WorldContextObject,
        const FEntityHandle& Agent,
        const FVector& TargetWorldLocation);

    /** Returns target selection to FTracing::TraceResult. */
    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Mobile Fire", meta = (WorldContext = "WorldContextObject"))
    static bool ClearMobileFireTargetOverride(
        const UObject* WorldContextObject,
        const FEntityHandle& Agent);

    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret|Mobile Fire", meta = (WorldContext = "WorldContextObject"))
    static bool GetMobileFireState(
        const UObject* WorldContextObject,
        const FEntityHandle& Agent,
        FMBSTMobileFireState& OutState);

    /** Batched hand-off for custom projectiles, hitscan, muzzle FX and sound. */
    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Mobile Fire", meta = (WorldContext = "WorldContextObject"))
    static TArray<FMBSTFireRequest> DrainMobileFireRequests(const UObject* WorldContextObject);

    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret", meta = (WorldContext = "WorldContextObject"))
    static FEntityTemplateData MakeSingleTurretTemplateFromAgentConfig(
        const UObject* WorldContextObject,
        const UMassBattleAgentConfigDataAsset* BaseAgentConfig,
        UMBSTSingleTurretAsset* Layout,
        const FMBSTSingleTurretState& InitialState);

    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret", meta = (WorldContext = "WorldContextObject"))
    static FEntityTemplateData AddSingleTurretToTemplate(
        const UObject* WorldContextObject,
        const FEntityTemplateData& BaseTemplate,
        UMBSTSingleTurretAsset* Layout,
        const FMBSTSingleTurretState& InitialState);

    /** One-time convenience; bulk spawning should augment the template instead. */
    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret", meta = (WorldContext = "WorldContextObject"))
    static bool AddSingleTurretToExistingAgent(
        const UObject* WorldContextObject,
        const FEntityHandle& Agent,
        UMBSTSingleTurretAsset* Layout,
        const FMBSTSingleTurretState& InitialState);

    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret", meta = (WorldContext = "WorldContextObject"))
    static bool SetTurretTargetAngles(
        const UObject* WorldContextObject,
        const FEntityHandle& Agent,
        float TargetYawDegrees,
        float TargetPitchDegrees,
        bool bSnap);

    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret", meta = (WorldContext = "WorldContextObject"))
    static bool AimTurretAtWorldLocation(
        const UObject* WorldContextObject,
        const FEntityHandle& Agent,
        const FVector& TargetWorldLocation,
        bool bSnap);

    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret", meta = (WorldContext = "WorldContextObject"))
    static bool TriggerTurretRecoil(
        const UObject* WorldContextObject,
        const FEntityHandle& Agent,
        float NormalizedAmount = 1.0f);

    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret", meta = (WorldContext = "WorldContextObject"))
    static bool SetTurretVisualStyle(
        const UObject* WorldContextObject,
        const FEntityHandle& Agent,
        int32 VisualStyle);

    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret", meta = (WorldContext = "WorldContextObject"))
    static bool GetSingleTurretState(
        const UObject* WorldContextObject,
        const FEntityHandle& Agent,
        FMBSTSingleTurretState& OutState);

    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret", meta = (WorldContext = "WorldContextObject"))
    static bool GetSingleTurretPose(
        const UObject* WorldContextObject,
        const FEntityHandle& Agent,
        FMBSTSingleTurretPose& OutPose);

    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret")
    static int32 PackSingleTurretRenderState(
        int32 VisualStyle,
        float YawDegrees,
        float PitchDegrees,
        float RecoilNormalized);

    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret")
    static void UnpackSingleTurretRenderState(
        int32 PackedState,
        int32& OutVisualStyle,
        float& OutYawDegrees,
        float& OutPitchDegrees,
        float& OutRecoilNormalized);
};
