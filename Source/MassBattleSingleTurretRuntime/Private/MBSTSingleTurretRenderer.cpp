#include "MBSTSingleTurretRenderer.h"

#include "MassBattleSingleTurretRuntime.h"
#include "HAL/IConsoleManager.h"
#include "NiagaraComponent.h"

namespace MBSTRendererPrivate
{
    int32 ActiveVertexVelocityPolicyUsers = 0;
    int32 PreviousVertexVelocitySetting = 2;
}

AMBSTSingleTurretRenderer::AMBSTSingleTurretRenderer()
{
    RenderBatchSize = 10000;
}

void AMBSTSingleTurretRenderer::BeginPlay()
{
    if (bDisableVertexDeformationVelocity)
    {
        AcquireVertexVelocityPolicy();
    }
    Super::BeginPlay();
}

void AMBSTSingleTurretRenderer::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    OptimizedBatchComponents.Reset();
    ReleaseVertexVelocityPolicy();
    Super::EndPlay(EndPlayReason);
}

void AMBSTSingleTurretRenderer::Tick(const float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    OptimizeNewBatchComponents();
}

void AMBSTSingleTurretRenderer::OptimizeNewBatchComponents()
{
    for (TPair<int32, FAgentRenderBatchData>& Pair : SpawnedRenderBatches)
    {
        UNiagaraComponent* Niagara = Pair.Value.SpawnedNiagaraSystem.Get();
        if (!IsValid(Niagara))
        {
            continue;
        }

        const bool bNeedsShadowDisable = Niagara->CastShadow;
        const bool bNeedsStationaryMobility = bTreatBatchComponentsAsStationary
            && Niagara->GetMobility() != EComponentMobility::Stationary;
        if (!bNeedsShadowDisable
            && !bNeedsStationaryMobility
            && OptimizedBatchComponents.Contains(Niagara))
        {
            continue;
        }

        if (bNeedsShadowDisable)
        {
            Niagara->SetCastShadow(false);
        }
        if (bNeedsStationaryMobility)
        {
            Niagara->SetMobility(EComponentMobility::Stationary);
        }
        OptimizedBatchComponents.Add(Niagara);
        UE_LOG(LogMassBattleSingleTurret, Display,
            TEXT("Optimized Niagara batch component %s: requested_stationary=%d effective_mobility=%d shadows=%d."),
            *Niagara->GetPathName(),
            bTreatBatchComponentsAsStationary ? 1 : 0,
            static_cast<int32>(Niagara->GetMobility()),
            Niagara->CastShadow ? 1 : 0);
    }

    for (auto It = OptimizedBatchComponents.CreateIterator(); It; ++It)
    {
        if (!It->IsValid())
        {
            It.RemoveCurrent();
        }
    }
}

void AMBSTSingleTurretRenderer::AcquireVertexVelocityPolicy()
{
    if (bVelocityPolicyAcquired)
    {
        return;
    }

    IConsoleVariable* VertexVelocity = IConsoleManager::Get().FindConsoleVariable(
        TEXT("r.Velocity.EnableVertexDeformation"));
    if (!VertexVelocity)
    {
        UE_LOG(LogMassBattleSingleTurret, Warning,
            TEXT("r.Velocity.EnableVertexDeformation was not found; the duplicate WPO velocity pass remains enabled."));
        return;
    }

    if (MBSTRendererPrivate::ActiveVertexVelocityPolicyUsers++ == 0)
    {
        MBSTRendererPrivate::PreviousVertexVelocitySetting = VertexVelocity->GetInt();
        VertexVelocity->Set(0, ECVF_SetByCode);
        UE_LOG(LogMassBattleSingleTurret, Display,
            TEXT("Mechanical army performance policy disabled vertex-deformation velocity (previous=%d, effective=%d)."),
            MBSTRendererPrivate::PreviousVertexVelocitySetting,
            VertexVelocity->GetInt());
    }
    bVelocityPolicyAcquired = true;
}

void AMBSTSingleTurretRenderer::ReleaseVertexVelocityPolicy()
{
    if (!bVelocityPolicyAcquired)
    {
        return;
    }
    bVelocityPolicyAcquired = false;

    MBSTRendererPrivate::ActiveVertexVelocityPolicyUsers = FMath::Max(
        MBSTRendererPrivate::ActiveVertexVelocityPolicyUsers - 1,
        0);
    if (MBSTRendererPrivate::ActiveVertexVelocityPolicyUsers != 0)
    {
        return;
    }

    if (IConsoleVariable* VertexVelocity = IConsoleManager::Get().FindConsoleVariable(
        TEXT("r.Velocity.EnableVertexDeformation")))
    {
        VertexVelocity->Set(
            MBSTRendererPrivate::PreviousVertexVelocitySetting,
            ECVF_SetByCode);
    }
}
