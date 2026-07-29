#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MassAPIStructs.h"
#include "MBSTRtsMobileFireDemo.generated.h"

class UMassBattleAgentConfigDataAsset;
class UMassBattleBPTaskAgentsMoveTo;
class UMBSTMobileFireProfile;
class AFlowField;

/**
 * Playable RTS attack-move showcase for the single-turret plugin.
 *
 * The copied MassBattle RTS map keeps its original selection/camera/right-click
 * controller. This actor only spawns selectable Mass entities and configures
 * their team-aware trace filters. Player commands therefore remain the native
 * RTS AgentsMoveTo path. During a locked Move, stop-to-fire units only track,
 * while units with AimAndFireWhileMoving keep moving and fire automatically.
 */
UCLASS(BlueprintType)
class MASSBATTLESINGLETURRETRUNTIME_API AMBSTRtsMobileFireDemoActor : public AActor
{
    GENERATED_BODY()

public:
    AMBSTRtsMobileFireDemoActor();

    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTS Mobile Fire|Units", meta = (ClampMin = "1", ClampMax = "5000"))
    int32 StopToFirePlayerCount = 12;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTS Mobile Fire|Units", meta = (ClampMin = "1", ClampMax = "5000"))
    int32 FireWhileMovingPlayerCount = 12;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTS Mobile Fire|Units", meta = (ClampMin = "1", ClampMax = "10000"))
    int32 EnemyCount = 24;

    /**
     * Keep manual PIE focused on the reported behavior: only moving-fire Mass
     * tanks are spawned on both teams. Automated comparison runs still create
     * the stop-to-fire group as well.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTS Mobile Fire|Units")
    bool bSpawnOnlyMovingFireInManualPlay = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTS Mobile Fire|Units", meta = (ClampMin = "100.0"))
    // Keep the tank colliders from resolving overlaps before the first RTS order.
    float FormationSpacing = 720.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTS Mobile Fire|Units", meta = (ClampMin = "1.0"))
    float TankHealth = 1000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTS Mobile Fire|Placement")
    FVector StopToFirePlayerOrigin = FVector(17000.0, -21500.0, 1124.0);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTS Mobile Fire|Placement")
    FVector FireWhileMovingPlayerOrigin = FVector(21500.0, -18000.0, 1124.0);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTS Mobile Fire|Placement")
    // Keep both player formations outside acquisition range at PIE start.
    // The player must issue an RTS move order before either side can engage.
    FVector EnemyOrigin = FVector(8500.0, -9000.0, 1124.0);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTS Mobile Fire|Observation")
    bool bDrawWorldLabels = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTS Mobile Fire|Observation")
    bool bDrawRangeGuides = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTS Mobile Fire|Observation")
    bool bDrawFireRequestTracers = true;

    /** Manual map play leaves this false. The command line switch -MBSTRtsAutoEngage also enables it. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTS Mobile Fire|Test")
    bool bAutoEngageForTesting = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTS Mobile Fire|Test", meta = (ClampMin = "5.0"))
    float AutoTestSeconds = 50.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTS Mobile Fire|Assets")
    TObjectPtr<UMBSTMobileFireProfile> StopToFireProfile = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTS Mobile Fire|Assets")
    TObjectPtr<UMBSTMobileFireProfile> FireWhileMovingProfile = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTS Mobile Fire|Assets")
    TObjectPtr<UMassBattleAgentConfigDataAsset> StopToFireAgentConfig = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTS Mobile Fire|Assets")
    TObjectPtr<UMassBattleAgentConfigDataAsset> FireWhileMovingAgentConfig = nullptr;

    /** Ground canvas already present in MassBattle's RTS demo map. */
    UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "RTS Mobile Fire|Assets")
    TObjectPtr<AFlowField> GroundFlowField = nullptr;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "RTS Mobile Fire|Result")
    bool bSpawnSucceeded = false;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "RTS Mobile Fire|Result")
    bool bAutoTestResultReady = false;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "RTS Mobile Fire|Result")
    bool bAutoTestPassed = false;

private:
    struct FGroupSnapshot
    {
        int32 Alive = 0;
        int32 Targeted = 0;
        int32 AimAligned = 0;
        int32 LockedMove = 0;
        int32 AttackMoving = 0;
        int32 Tracking = 0;
        int32 Braking = 0;
        int32 Windup = 0;
        int32 Firing = 0;
        double Health = 0.0;
        double AverageSpeed = 0.0;
        double AverageYawError = 0.0;
        double MaximumYawError = 0.0;
        double SampleBodyYaw = 0.0;
        double SampleCurrentTurretYaw = 0.0;
        double SampleTargetTurretYaw = 0.0;
        double SampleTargetBearingYaw = 0.0;
        bool bHasAimSample = false;
        FVector Center = FVector::ZeroVector;
    };

    bool LoadAssets();
    bool ResolveGroundFlowField();
    bool SpawnGroup(
        UMassBattleAgentConfigDataAsset* AgentConfig,
        int32 Quantity,
        int32 Team,
        const FVector& Origin,
        float InitialYawDegrees,
        TArray<FEntityHandle>& OutHandles);
    void ConfigureSpawnedEntity(const FEntityHandle& Entity, int32 Team);
    void StartAutoEngage();
    TObjectPtr<UMassBattleBPTaskAgentsMoveTo> StartMoveTask(
        const TArray<FEntityHandle>& Handles,
        const FVector& Destination,
        bool bBehaviorsCanInterrupt);
    void ConsumeFireRequests();
    FGroupSnapshot GatherSnapshot(const TArray<FEntityHandle>& Handles) const;
    void DrawObservationGuides(
        const FGroupSnapshot& StopSnapshot,
        const FGroupSnapshot& MoveSnapshot,
        const FGroupSnapshot& EnemySnapshot) const;
    void PublishStatus(
        const FGroupSnapshot& StopSnapshot,
        const FGroupSnapshot& MoveSnapshot,
        const FGroupSnapshot& EnemySnapshot);
    void PublishSelectionDiagnostic();
    void EvaluateAutoTest(
        const FGroupSnapshot& StopSnapshot,
        const FGroupSnapshot& MoveSnapshot,
        const FGroupSnapshot& EnemySnapshot);

    TArray<FEntityHandle> StopToFirePlayerHandles;
    TArray<FEntityHandle> FireWhileMovingPlayerHandles;
    TArray<FEntityHandle> EnemyHandles;

    UPROPERTY(Transient)
    TObjectPtr<UMassBattleBPTaskAgentsMoveTo> StopAutoMoveTask = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UMassBattleBPTaskAgentsMoveTo> MoveAutoMoveTask = nullptr;

    float ElapsedSeconds = 0.0f;
    float StatusAccumulator = 0.0f;
    FString LastSelectionSignature;
    float StopMaxSpeedAtFire = 0.0f;
    float MoveMaxSpeedAtFire = 0.0f;
    int64 StopPlayerShots = 0;
    int64 MovingPlayerShots = 0;
    int64 EnemyShots = 0;
    int64 SpawnedProjectiles = 0;
    bool bStopAcquiredDuringAttackMove = false;
    bool bMoveAcquiredDuringAttackMove = false;
    bool bStopAcquiredDuringLockedMove = false;
    bool bMoveAcquiredDuringLockedMove = false;
    bool bStopShotWithMovementHold = false;
    bool bMoveShotDuringAttackMove = false;
    bool bMoveShotDuringLockedMove = false;
    bool bMoveShotWithoutMovementHold = false;
    bool bAutoEngageStarted = false;
    bool bLockedMoveProbe = false;
    bool bAutoExit = false;
};
