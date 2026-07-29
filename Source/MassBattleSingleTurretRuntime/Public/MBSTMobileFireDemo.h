#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "MassAPIStructs.h"
#include "MBSTMobileFireDemo.generated.h"

class AStaticMeshActor;
class UMassBattleAgentConfigDataAsset;
class UMassBattleBPTaskAgentsMoveTo;
class UMBSTMobileFireProfile;

/**
 * Native attack-move proof: both groups use the same MassBattle AgentsMoveTo
 * path. Their per-AgentConfig MobileFireProfile is the only behavior difference.
 */
UCLASS(BlueprintType)
class MASSBATTLESINGLETURRETRUNTIME_API AMBSTMobileFireDemoActor : public AActor
{
    GENERATED_BODY()

public:
    AMBSTMobileFireDemoActor();

    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mobile Fire Demo", meta = (ClampMin = "1", ClampMax = "5000"))
    int32 UnitsPerPolicy = 64;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mobile Fire Demo", meta = (ClampMin = "50.0"))
    float FormationSpacing = 280.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mobile Fire Demo", meta = (ClampMin = "1000.0"))
    float MoveDistance = 30000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mobile Fire Demo", meta = (ClampMin = "0.0"))
    float InitialFireDelaySeconds = 2.5f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mobile Fire Demo", meta = (ClampMin = "2.0"))
    float FunctionalTestSeconds = 8.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mobile Fire Demo", meta = (ClampMin = "1.0"))
    float TargetOrbitPeriodSeconds = 8.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mobile Fire Demo", meta = (ClampMin = "100.0"))
    float TargetOrbitRadius = 2600.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mobile Fire Demo")
    bool bDrawFireRequestTracers = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mobile Fire Demo")
    TObjectPtr<UMassBattleAgentConfigDataAsset> BaseSingleTurretConfig = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mobile Fire Demo")
    TObjectPtr<UMBSTMobileFireProfile> StopToFireProfile = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mobile Fire Demo")
    TObjectPtr<UMBSTMobileFireProfile> FireWhileMovingProfile = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mobile Fire Demo")
    TObjectPtr<UMassBattleAgentConfigDataAsset> StopToFireAgentConfig = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mobile Fire Demo")
    TObjectPtr<UMassBattleAgentConfigDataAsset> FireWhileMovingAgentConfig = nullptr;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "Mobile Fire Demo|Result")
    bool bFunctionalResultReady = false;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "Mobile Fire Demo|Result")
    bool bFunctionalResultPassed = false;

private:
    struct FGroupStats
    {
        int32 ValidEntities = 0;
        int32 HoldingEntities = 0;
        int64 ShotSequenceTotal = 0;
        float AverageSpeed = 0.0f;
        float AverageX = 0.0f;
    };

    bool LoadAssets();
    bool SpawnTarget();
    bool SpawnPolicyGroup(
        UMassBattleAgentConfigDataAsset* AgentConfig,
        const FVector& Origin,
        TArray<FEntityHandle>& OutHandles,
        TObjectPtr<UMassBattleBPTaskAgentsMoveTo>& OutMoveTask);
    void ConfigureTargetEntity();
    void UpdateMovingTarget(float DeltaSeconds);
    void ConsumeFireRequests();
    FGroupStats GatherStats(const TArray<FEntityHandle>& Handles) const;
    void UpdateObserver();
    void PublishStatus(const FGroupStats& StopStats, const FGroupStats& MoveStats);
    void EvaluateFunctionalResult(const FGroupStats& StopStats, const FGroupStats& MoveStats);

    TArray<FEntityHandle> StopToFireHandles;
    TArray<FEntityHandle> FireWhileMovingHandles;
    FEntityHandle TargetEntity;

    UPROPERTY(Transient)
    TObjectPtr<UMassBattleBPTaskAgentsMoveTo> StopToFireMoveTask = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UMassBattleBPTaskAgentsMoveTo> FireWhileMovingMoveTask = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<AStaticMeshActor> TargetVisual = nullptr;

    float ElapsedSeconds = 0.0f;
    float StatusAccumulator = 0.0f;
    float StopPeakSpeed = 0.0f;
    float MovePeakSpeed = 0.0f;
    float StopMaxSpeedAtFire = 0.0f;
    float MoveMaxSpeedAtFire = 0.0f;
    int32 StopPeakHoldingEntities = 0;
    int32 MovePeakHoldingEntities = 0;
    int64 StopFireRequests = 0;
    int64 MoveFireRequests = 0;
    int64 MovingFireRequestsDuringActiveMove = 0;
    bool bAutoExit = false;
    bool bDisableVisualizationForTest = false;
    bool bObserverConfigured = false;
};

UCLASS()
class MASSBATTLESINGLETURRETRUNTIME_API AMBSTMobileFireDemoGameMode : public AGameModeBase
{
    GENERATED_BODY()

public:
    AMBSTMobileFireDemoGameMode();
};
