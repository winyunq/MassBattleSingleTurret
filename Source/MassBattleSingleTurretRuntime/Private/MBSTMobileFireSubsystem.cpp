#include "MBSTMobileFireSubsystem.h"

#include "MBSTMobileFireProcessor.h"

// MassBattleFrame deliberately owns its lockstep processors in a private,
// manually-dispatched stage array and currently exposes no registration API.
// Keep the access shim local to this non-unity translation unit.
#define private public
#include "Subsystems/MassBattleSubsystem.h"
#undef private

#include "MassEntitySubsystem.h"
#include "Misc/ScopeLock.h"
#include "Processors/MassBattleAgentBehaviorProcessor.h"
#include "Processors/MassBattleAgentMoveProcessor.h"
#include "Processors/MassBattleAgentMoveProcessorFP.h"

void UMBSTMobileFireSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    UMassBattleSubsystem* MassBattle =
        Collection.InitializeDependency<UMassBattleSubsystem>();
    Collection.InitializeDependency<UMassEntitySubsystem>();
    UMassEntitySubsystem* MassEntity = GetWorld()
        ? GetWorld()->GetSubsystem<UMassEntitySubsystem>()
        : nullptr;
    if (!MassBattle || !MassEntity || !MassBattle->SimStages.IsValidIndex(0))
    {
        UE_LOG(LogTemp, Fatal,
            TEXT("MobileFire lockstep processor installation dependencies are unavailable."));
        return;
    }

    const auto ContainsMobileFireProcessor = [](const auto& Processors)
    {
        for (const UMassProcessor* Processor : Processors)
        {
            if (Processor
                && (Processor->IsA<UMBSTMobileFireMovementGateProcessor>()
                    || Processor->IsA<UMBSTMobileFireMovementGateRestoreProcessor>()
                    || Processor->IsA<UMBSTMobileFireCombatProcessor>()))
            {
                return true;
            }
        }
        return false;
    };
    if (MassBattle->SimStages.ContainsByPredicate(ContainsMobileFireProcessor)
        || ContainsMobileFireProcessor(MassBattle->OwnedProcessors))
    {
        UE_LOG(LogTemp, Fatal,
            TEXT("MobileFire lockstep processors were registered more than once."));
        return;
    }

    const auto IsMoveProcessor = [](const UMassProcessor* Processor)
    {
        return Processor
            && (Processor->IsA<UMassBattleAgentMoveProcessor>()
                || Processor->IsA<UMassBattleAgentMoveProcessorFP>());
    };
    TArray<UMassProcessor*>* MovementStage = nullptr;
    TArray<UMassProcessor*>* CombatStage = nullptr;
    const auto IsBehaviorProcessor = [](const UMassProcessor* Processor)
    {
        return Processor && Processor->IsA<UMassBattleAgentBehaviorProcessor>();
    };
    for (TArray<UMassProcessor*>& Stage : MassBattle->SimStages)
    {
        if (Stage.ContainsByPredicate(IsMoveProcessor))
        {
            if (MovementStage)
            {
                UE_LOG(LogTemp, Fatal,
                    TEXT("MobileFire requires both MassBattle movement variants in one stage."));
                return;
            }
            MovementStage = &Stage;
        }
        if (Stage.ContainsByPredicate(IsBehaviorProcessor))
        {
            CombatStage = &Stage;
        }
    }
    if (!MovementStage || !CombatStage)
    {
        UE_LOG(LogTemp, Fatal,
            TEXT("MobileFire could not locate the MassBattle movement or behavior stage."));
        return;
    }

    const TSharedRef<FMassEntityManager> EntityManager =
        MassEntity->GetMutableEntityManager().AsShared();
    UMBSTMobileFireMovementGateProcessor* Gate =
        NewObject<UMBSTMobileFireMovementGateProcessor>(MassBattle);
    UMBSTMobileFireMovementGateRestoreProcessor* Restore =
        NewObject<UMBSTMobileFireMovementGateRestoreProcessor>(MassBattle);
    UMBSTMobileFireCombatProcessor* Combat =
        NewObject<UMBSTMobileFireCombatProcessor>(MassBattle);
    Gate->CallInitialize(MassBattle, EntityManager);
    Restore->CallInitialize(MassBattle, EntityManager);
    Combat->CallInitialize(MassBattle, EntityManager);
    MassBattle->OwnedProcessors.Add(Gate);
    MassBattle->OwnedProcessors.Add(Restore);
    MassBattle->OwnedProcessors.Add(Combat);

    const int32 FirstMoveIndex = MovementStage->IndexOfByPredicate(IsMoveProcessor);
    MovementStage->Insert(Gate, FirstMoveIndex);
    int32 LastMoveIndex = INDEX_NONE;
    for (int32 Index = 0; Index < MovementStage->Num(); ++Index)
    {
        if (IsMoveProcessor((*MovementStage)[Index]))
        {
            LastMoveIndex = Index;
        }
    }
    MovementStage->Insert(Restore, LastMoveIndex + 1);

    const int32 BehaviorIndex = CombatStage->IndexOfByPredicate(IsBehaviorProcessor);
    CombatStage->Insert(Combat, BehaviorIndex + 1);

    bLockstepProcessorsInstalled = true;
    UE_LOG(LogTemp, Log,
        TEXT("Installed MobileFire Gate/Restore/Combat processors in the MassBattle lockstep stage."));
}

void UMBSTMobileFireSubsystem::SubmitFireRequest(
    const FMBSTFireRequest& Request,
    const bool bBroadcastBlueprintEvent)
{
    {
        FScopeLock Lock(&PendingRequestsMutex);
        PendingRequests.Add(Request);
    }

    NativeOnFireRequest.Broadcast(Request);
    if (bBroadcastBlueprintEvent)
    {
        OnFireRequest.Broadcast(Request);
    }
}

TArray<FMBSTFireRequest> UMBSTMobileFireSubsystem::DrainFireRequests()
{
    TArray<FMBSTFireRequest> Result;
    FScopeLock Lock(&PendingRequestsMutex);
    Swap(Result, PendingRequests);
    return Result;
}

int32 UMBSTMobileFireSubsystem::GetPendingFireRequestCount() const
{
    FScopeLock Lock(&PendingRequestsMutex);
    return PendingRequests.Num();
}
