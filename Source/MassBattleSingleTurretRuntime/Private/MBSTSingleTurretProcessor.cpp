#include "MBSTSingleTurretProcessor.h"

#include "MBSTSingleTurretTypes.h"
#include "Fragments/StyleType.h"
#include "MassAPIStructs.h"
#include "MassExecutionContext.h"
#include "Engine/World.h"

UMBSTSingleTurretPackProcessor::UMBSTSingleTurretPackProcessor()
    : EntityQuery(*this)
{
    ExecutionOrder.ExecuteBefore.Add(TEXT("MassBattleAgentRenderProcessor"));
    ExecutionFlags = static_cast<int32>(EProcessorExecutionFlags::Client | EProcessorExecutionFlags::Server | EProcessorExecutionFlags::Standalone);
    ProcessingPhase = EMassProcessingPhase::FrameEnd;
    bAutoRegisterWithProcessingPhases = true;
    bRequiresGameThreadExecution = false;
    ExecutionPriority = 5;
}

void UMBSTSingleTurretPackProcessor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
    FEntityQueryBuilder(EntityQuery)
        .All<FMBSTSingleTurretTag>()
        .All<FMBSTSingleTurretState, FStyleType>(MARW)
        .All<FMBSTSingleTurretShared>(MARO)
        .RegisterWithProcessor(*this);
}

void UMBSTSingleTurretPackProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
    const float DeltaSeconds = Context.GetWorld()
        ? FMath::Max(Context.GetWorld()->GetDeltaSeconds(), 0.0f)
        : 0.0f;

    EntityQuery.ForEachEntityChunk(Context, [DeltaSeconds](FMassExecutionContext& ChunkContext)
    {
        const FMBSTSingleTurretShared& Shared = ChunkContext.GetSharedFragment<FMBSTSingleTurretShared>();
        const float MinYaw = FMath::Min(Shared.YawLimitsDegrees.X, Shared.YawLimitsDegrees.Y);
        const float MaxYaw = FMath::Max(Shared.YawLimitsDegrees.X, Shared.YawLimitsDegrees.Y);
        const float MinPitch = FMath::Min(Shared.PitchLimitsDegrees.X, Shared.PitchLimitsDegrees.Y);
        const float MaxPitch = FMath::Max(Shared.PitchLimitsDegrees.X, Shared.PitchLimitsDegrees.Y);

        TArrayView<FMBSTSingleTurretState> States = ChunkContext.GetMutableFragmentView<FMBSTSingleTurretState>();
        TArrayView<FStyleType> Styles = ChunkContext.GetMutableFragmentView<FStyleType>();

        const int32 NumEntities = ChunkContext.GetNumEntities();
        for (int32 EntityIndex = 0; EntityIndex < NumEntities; ++EntityIndex)
        {
            FMBSTSingleTurretState& State = States[EntityIndex];

            const float TargetYaw = FMath::Clamp(
                FMath::UnwindDegrees(State.TargetYawDegrees),
                MinYaw,
                MaxYaw);
            const float TargetPitch = Shared.bHasBarrelPitch
                ? FMath::Clamp(State.TargetPitchDegrees, MinPitch, MaxPitch)
                : 0.0f;

            if (!State.bArticulationEnabled)
            {
                State.CurrentYawDegrees = 0.0f;
                State.CurrentPitchDegrees = 0.0f;
                State.RecoilNormalized = 0.0f;
            }
            else if (State.bInterpolate && DeltaSeconds > 0.0f)
            {
                State.CurrentYawDegrees = Shared.YawSpeedDegreesPerSecond > 0.0f
                    ? FMath::FixedTurn(
                        State.CurrentYawDegrees,
                        TargetYaw,
                        Shared.YawSpeedDegreesPerSecond * DeltaSeconds)
                    : TargetYaw;

                State.CurrentPitchDegrees = Shared.PitchSpeedDegreesPerSecond > 0.0f
                    ? FMath::FInterpConstantTo(
                        State.CurrentPitchDegrees,
                        TargetPitch,
                        DeltaSeconds,
                        Shared.PitchSpeedDegreesPerSecond)
                    : TargetPitch;
            }
            else
            {
                State.CurrentYawDegrees = TargetYaw;
                State.CurrentPitchDegrees = TargetPitch;
            }

            State.CurrentYawDegrees = FMath::Clamp(
                FMath::UnwindDegrees(State.CurrentYawDegrees),
                MinYaw,
                MaxYaw);
            State.CurrentPitchDegrees = Shared.bHasBarrelPitch
                ? FMath::Clamp(State.CurrentPitchDegrees, MinPitch, MaxPitch)
                : 0.0f;

            if (State.RecoilNormalized > 0.0f
                && Shared.RecoilReturnSpeed > 0.0f
                && DeltaSeconds > 0.0f)
            {
                State.RecoilNormalized = FMath::FInterpConstantTo(
                    State.RecoilNormalized,
                    0.0f,
                    DeltaSeconds,
                    Shared.RecoilReturnSpeed);
            }
            State.RecoilNormalized = FMath::Clamp(State.RecoilNormalized, 0.0f, 1.0f);

            Styles[EntityIndex].Index = MBSTPacking::Pack(
                static_cast<int32>(State.VisualStyle),
                State.CurrentYawDegrees,
                State.CurrentPitchDegrees,
                State.RecoilNormalized);
        }
    });
}
