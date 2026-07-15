#pragma once

#include "CoreMinimal.h"
#include "MassEntityQuery.h"
#include "MassProcessor.h"
#include "MBSTSingleTurretProcessor.generated.h"

/** Fixed single-turret hot path that writes one packed int32 before the MassBattle renderer runs. */
UCLASS()
class MASSBATTLESINGLETURRETRUNTIME_API UMBSTSingleTurretPackProcessor : public UMassProcessor
{
    GENERATED_BODY()

public:
    UMBSTSingleTurretPackProcessor();

protected:
    virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
    virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
    FMassEntityQuery EntityQuery;
};
