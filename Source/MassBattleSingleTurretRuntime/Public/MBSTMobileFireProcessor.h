#pragma once

#include "CoreMinimal.h"
#include "MassEntityQuery.h"
#include "MassProcessor.h"
#include "MBSTMobileFireProcessor.generated.h"

/** Applies the combat processor's previous-step hold request before MassBattle movement. */
UCLASS()
class MASSBATTLESINGLETURRETRUNTIME_API UMBSTMobileFireMovementGateProcessor : public UMassProcessor
{
    GENERATED_BODY()

public:
    UMBSTMobileFireMovementGateProcessor();

protected:
    virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
    virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
    FMassEntityQuery EntityQuery;
};


/** Restores the original movement stop bit at FrameEnd, after the MassBattle move pass. */
UCLASS()
class MASSBATTLESINGLETURRETRUNTIME_API UMBSTMobileFireMovementGateRestoreProcessor : public UMassProcessor
{
    GENERATED_BODY()

public:
    UMBSTMobileFireMovementGateRestoreProcessor();

protected:
    virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
    virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
    FMassEntityQuery EntityQuery;
};

/**
 * Uses the previous completed TraceResult at the Combat sub-frame, drives the CPU
 * turret state, packs its render sample, evaluates the three mobility policies, and
 * emits exactly one request per shot. MassBattleFrame exposes no plugin hook inside
 * its manually-dispatched same-tick Trace/Behavior/Render chain.
 */
UCLASS()
class MASSBATTLESINGLETURRETRUNTIME_API UMBSTMobileFireCombatProcessor : public UMassProcessor
{
    GENERATED_BODY()

public:
    UMBSTMobileFireCombatProcessor();

protected:
    virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
    virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
    FMassEntityQuery EntityQuery;
};
