#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "MBSTMobileFireTypes.h"
#include "MBSTMobileFireSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMBSTFireRequestDynamicDelegate, const FMBSTFireRequest&, Request);
DECLARE_MULTICAST_DELEGATE_OneParam(FMBSTFireRequestNativeDelegate, const FMBSTFireRequest&);

/**
 * Batched hand-off point between the Mass processor and project-specific weapon code.
 * Native code should prefer NativeOnFireRequest or DrainFireRequests. Blueprint
 * broadcasting is opt-in per profile because one dynamic delegate call per shot is
 * measurably more expensive than draining a batch.
 */
UCLASS()
class MASSBATTLESINGLETURRETRUNTIME_API UMBSTMobileFireSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;

    UPROPERTY(BlueprintAssignable, Category = "MassBattle|Single Turret|Mobile Fire")
    FMBSTFireRequestDynamicDelegate OnFireRequest;

    FMBSTFireRequestNativeDelegate NativeOnFireRequest;

    void SubmitFireRequest(const FMBSTFireRequest& Request, bool bBroadcastBlueprintEvent);

    UFUNCTION(BlueprintCallable, Category = "MassBattle|Single Turret|Mobile Fire")
    TArray<FMBSTFireRequest> DrainFireRequests();

    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret|Mobile Fire")
    int32 GetPendingFireRequestCount() const;

private:
    bool bLockstepProcessorsInstalled = false;
    mutable FCriticalSection PendingRequestsMutex;
    TArray<FMBSTFireRequest> PendingRequests;
};
