#pragma once

#include "CoreMinimal.h"
#include "Renderers/MassBattleAgentRenderer.h"
#include "MBSTSingleTurretRenderer.generated.h"

class UNiagaraComponent;

// Performance-first MassBattle renderer policy for articulated mechanical units.
// The renderer-owned Niagara component never changes transform, while its GPU
// particles remain fully dynamic. Marking the owner Stationary (not Static)
// preserves Niagara dynamic-data submission without making UE replay all WPO
// in a second opaque velocity pass.
UCLASS(Blueprintable)
class MASSBATTLESINGLETURRETRUNTIME_API AMBSTSingleTurretRenderer final
    : public AMassBattleAgentRenderer
{
    GENERATED_BODY()

public:
    AMBSTSingleTurretRenderer();

    virtual void Tick(float DeltaSeconds) override;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "MassBattle|Single Turret|Performance")
    bool bTreatBatchComponentsAsStationary = true;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "MassBattle|Single Turret|Performance")
    bool bDisableVertexDeformationVelocity = true;

private:
    void OptimizeNewBatchComponents();
    void AcquireVertexVelocityPolicy();
    void ReleaseVertexVelocityPolicy();

    TSet<TWeakObjectPtr<UNiagaraComponent>> OptimizedBatchComponents;
    bool bVelocityPolicyAcquired = false;
};
