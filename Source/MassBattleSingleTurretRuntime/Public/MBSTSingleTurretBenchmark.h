#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/HUD.h"
#include "MassEntityQuery.h"
#include "MassProcessor.h"
#include "Fragments/MassBattleBaseStruct.h"
#include "MBSTSingleTurretBenchmark.generated.h"

class ACameraActor;
class UMassBattleAgentConfigDataAsset;

UENUM(BlueprintType)
enum class EMBSTBenchmarkScenario : uint8
{
    LegacyCompound,
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

/** Benchmark-only tag for the legacy turret entity inside BP_TankActor. */
USTRUCT()
struct MASSBATTLESINGLETURRETRUNTIME_API FMBSTBenchmarkLegacyTurretTag : public FA_MassBattleBaseTag
{
    GENERATED_BODY()
};

/** Benchmark-only tag for the plugin's direct single-turret entity. */
USTRUCT()
struct MASSBATTLESINGLETURRETRUNTIME_API FMBSTBenchmarkSingleTurretTag : public FA_MassBattleBaseTag
{
    GENERATED_BODY()
};

/** Applies the identical sinusoidal engagement sweep to both benchmark architectures. */
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
    FMassEntityQuery SingleTurretQuery;
};

/** Runtime controller used by the two generated 5,000-vs-5,000 demo maps. */
UCLASS(BlueprintType)
class MASSBATTLESINGLETURRETRUNTIME_API AMBSTSingleTurretBenchmarkActor : public AActor
{
    GENERATED_BODY()

public:
    AMBSTSingleTurretBenchmarkActor();

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark")
    EMBSTBenchmarkScenario Scenario = EMBSTBenchmarkScenario::SingleTurret;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark", meta = (ClampMin = "1"))
    int32 TanksPerSide = 5000;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark", meta = (ClampMin = "0.0"))
    float WarmupSeconds = 10.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark", meta = (ClampMin = "1.0"))
    float SampleSeconds = 20.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MassBattle|Single Turret|Benchmark", meta = (ClampMin = "1"))
    int32 LegacyActorsPerFrame = 250;

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

    UFUNCTION(BlueprintPure, Category = "MassBattle|Single Turret|Benchmark")
    float GetLiveSweepYawDegrees() const;

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
    void BeginSpawning();
    void SpawnSingleTurretForces();
    void SpawnLegacyBatch();
    bool SpawnOneLegacyTank(int32 SideIndex, int32 SideTankIndex);
    void ConfigureControlledEntity(const struct FEntityHandle& EntityHandle, bool bTurretEntity, bool bSingleTurretEntity, int32 ForcedTeamIndex = -1);
    void EnterWarmup();
    void EnterSampling();
    void FinishSampling();
    void WriteResultJson();
    void CaptureScreenshot(const FString& Suffix, bool bCloseView, bool bVisualSalvo);
    void FlushPendingScreenshot();
    void DrawVisualSalvo() const;
    void SetCloseCamera(bool bCloseView) const;
    FVector GetFormationPosition(int32 SideIndex, int32 SideTankIndex) const;
    FRotator GetFormationRotation(int32 SideIndex) const;
    float GetPercentile(const TArray<float>& SortedValues, float Alpha) const;
    FString GetOutputDirectory() const;

    UPROPERTY(Transient)
    TObjectPtr<UClass> LegacyTankClass = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UMassBattleAgentConfigDataAsset> SingleTurretConfig = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<ACameraActor> WideCamera = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<ACameraActor> CloseCamera = nullptr;

    TArray<float> FrameTimeSamplesMilliseconds;
    FString ResultFilePath;
    FString OutputDirectoryOverride;
    FString PendingScreenshotFilename;
    double LastFrameWallClockSeconds = 0.0;
    float PhaseElapsedSeconds = 0.0f;
    float CompletionElapsedSeconds = 0.0f;
    float FormationSpacing = 500.0f;
    float ArmyCenterX = 15000.0f;
    int32 FormationDepth = 50;
    int32 FormationWidth = 100;
    int32 SpawnedSideCounts[2] = { 0, 0 };
    bool bSkipScreenshots = false;
    bool bDoNotExit = false;
    bool bWideCaptured = false;
    bool bYawPlusCaptured = false;
    bool bYawMinusCaptured = false;
    bool bResultCaptured = false;
    bool bPendingVisualSalvo = false;
    bool bRenderDiagnosticsLogged = false;
    int32 PendingScreenshotFrames = 0;
};

/** Compact proof overlay embedded in every benchmark screenshot. */
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
