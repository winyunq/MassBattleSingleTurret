#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/HUD.h"
#include "MassAPIStructs.h"
#include "MassEntityQuery.h"
#include "MassProcessor.h"
#include "Fragments/MassBattleBaseStruct.h"
#include "MBSTSingleTurretBenchmark.generated.h"

class ACameraActor;
class AStaticMeshActor;
class UMassBattleAgentConfigDataAsset;

/** Three separately launched native benchmark architectures. */
UENUM(BlueprintType)
enum class EMBSTBenchmarkScenario : uint8
{
    /** The untouched MassBattleFrame BP_TankActor compound architecture. */
    LegacyCompound,

    /** One ordinary Mass entity; the complete tank rotates to track the target. */
    MassBaseline,

    /** One Mass entity whose GPU-rendered turret tracks independently. */
    SingleTurret
};

UENUM(BlueprintType)
enum class EMBSTBenchmarkPhase : uint8
{
    Waiting,
    Spawning,
    Warmup,
    Sampling,
    Complete,
    Failed
};

/** Benchmark-only tag for the turret entity inside the original BP_TankActor. */
USTRUCT()
struct MASSBATTLESINGLETURRETRUNTIME_API FMBSTBenchmarkLegacyTurretTag : public FA_MassBattleBaseTag
{
    GENERATED_BODY()
};

/** Benchmark-only tag for an ordinary one-entity Mass tank. */
USTRUCT()
struct MASSBATTLESINGLETURRETRUNTIME_API FMBSTBenchmarkMassBaselineTag : public FA_MassBattleBaseTag
{
    GENERATED_BODY()
};

/** Benchmark-only tag for the plugin's direct single-turret entity. */
USTRUCT()
struct MASSBATTLESINGLETURRETRUNTIME_API FMBSTBenchmarkSingleTurretTag : public FA_MassBattleBaseTag
{
    GENERATED_BODY()
};

/**
 * Native Mass processor that forces every tested unit to continuously track the
 * same deterministic orbiting target.  It contains no Blueprint Tick or Actor
 * loop, and it only matches entities carrying one of the benchmark tags.
 */
UCLASS()
class MASSBATTLESINGLETURRETRUNTIME_API UMBSTSingleTurretBenchmarkDriveProcessor : public UMassProcessor
{
    GENERATED_BODY()

public:
    UMBSTSingleTurretBenchmarkDriveProcessor();

protected:
    virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
    virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
    FMassEntityQuery LegacyQuery;
    FMassEntityQuery MassBaselineQuery;
    FMassEntityQuery SingleTurretQuery;
};

/** Controller for the adjustable native moving-target benchmark demo. */
UCLASS(BlueprintType)
class MASSBATTLESINGLETURRETRUNTIME_API AMBSTSingleTurretBenchmarkActor : public AActor
{
    GENERATED_BODY()

public:
    AMBSTSingleTurretBenchmarkActor();

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark")
    EMBSTBenchmarkScenario Scenario = EMBSTBenchmarkScenario::SingleTurret;

    /** Number of tanks in the one tested group. CLI: -MBSTUnits=500. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark", meta = (ClampMin = "1"))
    int32 UnitCount = 500;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark", meta = (ClampMin = "0.0"))
    float WarmupSeconds = 8.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark", meta = (ClampMin = "1.0"))
    float SampleSeconds = 15.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark", meta = (ClampMin = "1"))
    int32 LegacyActorsPerFrame = 100;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark|Moving Target", meta = (ClampMin = "100.0"))
    float TargetOrbitRadius = 10000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark|Moving Target", meta = (ClampMin = "1.0"))
    float TargetOrbitPeriodSeconds = 12.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark|Moving Target")
    float TargetHeight = 300.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark|Moving Target", meta = (ClampMin = "1.0"))
    float TargetHealth = 1000000000.0f;

    /** Optional deterministic target bearing used by yaw/handedness proofs. CLI: -MBSTFixedTargetBearing=90. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark|Moving Target")
    bool bUseFixedTargetBearing = false;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark|Moving Target")
    float FixedTargetBearingDegrees = 90.0f;

    /** Uses a straight-down close camera so barrel/target alignment is unambiguous. CLI: -MBSTTopDownAimProof. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark|Moving Target")
    bool bTopDownAimProof = false;

    /** Leaves the PlayerController on its SpectatorPawn instead of forcing benchmark cameras. CLI: -MBSTFreeObserve. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark|Observation")
    bool bFreeObservation = false;

    /** Editor Play sessions automatically become freely observable; standalone benchmark processes remain deterministic. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark|Observation")
    bool bAutoFreeObservationInPIE = true;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark|Observation", meta = (ClampMin = "100.0"))
    float FreeObservationSpeed = 6000.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark")
    EMBSTBenchmarkPhase Phase = EMBSTBenchmarkPhase::Waiting;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark")
    int32 SpawnedTankCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark")
    int32 SpawnedUnitEntityCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark")
    float AverageFrameMilliseconds = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark")
    float MedianFrameMilliseconds = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark")
    float P95FrameMilliseconds = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark")
    float P99FrameMilliseconds = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark")
    float MaximumFrameMilliseconds = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark")
    float AverageGameThreadMilliseconds = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark")
    float AverageRenderThreadMilliseconds = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark")
    float AverageGPUMilliseconds = 0.0f;

    /** Kept for old demo/HUD callers; now returns the target's orbit bearing. */
    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret|Benchmark")
    float GetLiveSweepYawDegrees() const;

    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret|Benchmark")
    FVector GetMovingTargetLocation() const;

    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret|Benchmark")
    FString GetScenarioLabel() const;

    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret|Benchmark")
    FString GetPhaseLabel() const;

    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret|Benchmark")
    FString GetResultFilePath() const { return ResultFilePath; }

protected:
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;

private:
    void ApplyCommandLineOverrides();
    void CreateBenchmarkEnvironment();
    bool LoadScenarioAssets();
    bool SpawnMovingTarget();
    void UpdateMovingTarget();
    void BeginSpawning();
    void SpawnMassScenario(bool bSingleTurret);
    void SpawnLegacyBatch();
    bool SpawnOneLegacyTank(int32 TankIndex);
    void ConfigureControlledEntity(const FEntityHandle& EntityHandle, bool bLegacyTurret, bool bMassBaseline, bool bSingleTurret, bool bEnableVisualization, int32 ForcedTeamIndex = -1);
    void ConfigureMovingTargetEntity(const FEntityHandle& EntityHandle);
    void EnterWarmup();
    void EnterSampling();
    void FinishSampling();
    void WriteResultJson();
    void CaptureScreenshot(const FString& Suffix, bool bCloseView, bool bVisualAimLines);
    void FlushPendingScreenshot();
    void DrawVisualAimProof() const;
    void SetCloseCamera(bool bCloseView) const;
    FVector GetFormationPosition(int32 TankIndex) const;
    float GetPercentile(const TArray<float>& SortedValues, float Alpha) const;
    FString GetOutputDirectory() const;
    FString GetScenarioToken() const;

    UPROPERTY(Transient)
    TObjectPtr<UClass> LegacyTankClass = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UMassBattleAgentConfigDataAsset> SingleTurretConfig = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<ACameraActor> WideCamera = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<ACameraActor> CloseCamera = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<AStaticMeshActor> MovingTargetVisual = nullptr;

    FEntityHandle MovingTargetEntity;
    TArray<float> FrameTimeSamplesMilliseconds;
    TArray<float> GameThreadSamplesMilliseconds;
    TArray<float> RenderThreadSamplesMilliseconds;
    TArray<float> GPUSamplesMilliseconds;
    FString ResultFilePath;
    FString OutputDirectoryOverride;
    FString PendingScreenshotFilename;
    double LastFrameWallClockSeconds = 0.0;
    float PhaseElapsedSeconds = 0.0f;
    float CompletionElapsedSeconds = 0.0f;
    float FormationSpacing = 500.0f;
    int32 FormationDepth = 1;
    int32 FormationWidth = 1;
    bool bSkipScreenshots = false;
    bool bDoNotExit = false;
    bool bWideCaptured = false;
    bool bCloseCaptured = false;
    bool bResultCaptured = false;
    bool bPendingVisualAimProof = false;
    bool bRenderDiagnosticsLogged = false;
    int32 PendingScreenshotFrames = 0;
};

/** Compact proof overlay embedded in benchmark screenshots. */
UCLASS()
class MASSBATTLESINGLETURRETRUNTIME_API AMBSTSingleTurretBenchmarkHUD : public AHUD
{
    GENERATED_BODY()

public:
    virtual void DrawHUD() override;

private:
    TWeakObjectPtr<AMBSTSingleTurretBenchmarkActor> CachedBenchmark;
};

UCLASS()
class MASSBATTLESINGLETURRETRUNTIME_API AMBSTSingleTurretBenchmarkGameMode : public AGameModeBase
{
    GENERATED_BODY()

public:
    AMBSTSingleTurretBenchmarkGameMode();
};
