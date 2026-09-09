#include "MBSTSingleTurretProcessor.h"

#include "MBSTMobileFireTypes.h"
#include "MBSTSingleTurretTypes.h"
#include "Fragments/StyleType.h"
#include "MassAPIStructs.h"
#include "MassExecutionContext.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Subsystems/MassBattleSubsystem.h"

namespace MBSTSingleTurretProcessorPrivate
{
    static bool IsFeatureDisabledForBenchmarkAB()
    {
        FString ScenarioToken;
        return FParse::Value(FCommandLine::Get(), TEXT("MBSTScenario="), ScenarioToken)
            && (ScenarioToken.Equals(TEXT("mass"), ESearchCase::IgnoreCase)
                || ScenarioToken.Equals(TEXT("baseline"), ESearchCase::IgnoreCase));
    }
}

UMBSTSingleTurretPackProcessor::UMBSTSingleTurretPackProcessor()
    : EntityQuery(*this)
{
    ExecutionOrder.ExecuteBefore.Add(TEXT("MassBattleAgentRenderProcessor"));
    ExecutionFlags = static_cast<int32>(EProcessorExecutionFlags::Client | EProcessorExecutionFlags::Server | EProcessorExecutionFlags::Standalone);
    // MassBattleFrame 1.19.7 dispatches its renderer manually from
    // TG_PostUpdateWork. StartPhysics is the last public plugin phase that can
    // prepare Style before that renderer without changing MassBattleFrame.
    ProcessingPhase = EMassProcessingPhase::StartPhysics;
    // A/B feature-off runs retain the exact same entity archetype and renderer,
    // but omit this plugin processor just as a disabled turret feature would.
    bAutoRegisterWithProcessingPhases =
        !MBSTSingleTurretProcessorPrivate::IsFeatureDisabledForBenchmarkAB();
    bRequiresGameThreadExecution = false;
    ExecutionPriority = 5;
}

void UMBSTSingleTurretPackProcessor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
    FEntityQueryBuilder(EntityQuery)
        .All<FMBSTSingleTurretTag>()
        .None<FMBSTMobileFireTag>()
        .All<FMBSTSingleTurretState, FStyleType>(MARW)
        .All<FMBSTSingleTurretShared>(MARO)
        .RegisterWithProcessor(*this);
}

void UMBSTSingleTurretPackProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(MBSTSingleTurretLogicPack);

    UMassBattleSubsystem* MassBattle = UMassBattleSubsystem::GetPtr(this);
    if (!MassBattle || !MassBattle->IsSubFrameScheduled(ESubFrame::Subtick3))
    {
        return;
    }

    const float DeltaSeconds = FMath::Max(MassBattle->GetCalculatedStepTime(), 0.0f);

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

            if (!State.bExternalMotionDriver)
            {
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
            }

            Styles[EntityIndex].Index = MBSTPacking::SanitizeAndPack(State, Shared);
        }
    });
}
