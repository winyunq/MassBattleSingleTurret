#include "MBSTSingleTurretAuthoringComponent.h"

#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Actor.h"

UMBSTSingleTurretAuthoringComponent::UMBSTSingleTurretAuthoringComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    bIsEditorOnly = true;
}

USceneComponent* UMBSTSingleTurretAuthoringComponent::ResolveSceneReference(
    const FComponentReference& Reference,
    const FName FallbackTag) const
{
    AActor* Owner = GetOwner();
    if (!IsValid(Owner))
    {
        return nullptr;
    }

    // Empty FComponentReference may resolve to the root component. Only use the
    // reference when the picker actually stores a property/path/other actor.
    const bool bReferenceConfigured =
        !Reference.ComponentProperty.IsNone() ||
        !Reference.PathToComponent.IsEmpty() ||
        Reference.OtherActor.IsValid() ||
        Reference.OverrideComponent.IsValid();

    if (bReferenceConfigured)
    {
        if (UActorComponent* ReferencedComponent = Reference.GetComponent(Owner))
        {
            if (USceneComponent* SceneComponent = Cast<USceneComponent>(ReferencedComponent))
            {
                return SceneComponent;
            }
        }
    }

    if (!FallbackTag.IsNone())
    {
        TInlineComponentArray<USceneComponent*> Components(Owner);
        for (USceneComponent* Component : Components)
        {
            if (IsValid(Component) && Component->ComponentHasTag(FallbackTag))
            {
                return Component;
            }
        }
    }

    return nullptr;
}

USceneComponent* UMBSTSingleTurretAuthoringComponent::ResolveTurretYawPivot() const
{
    if (USceneComponent* Resolved = ResolveSceneReference(TurretYawPivot, TurretYawTag))
    {
        return Resolved;
    }

    if (AActor* Owner = GetOwner())
    {
        TInlineComponentArray<UMBSTArticulationNodeMarkerComponent*> Markers(Owner);
        for (UMBSTArticulationNodeMarkerComponent* Marker : Markers)
        {
            if (IsValid(Marker) && Marker->Role == EMBSTArticulationNodeRole::TurretYaw)
            {
                return Marker;
            }
        }
    }

    return nullptr;
}

USceneComponent* UMBSTSingleTurretAuthoringComponent::ResolveBarrelPitchPivot() const
{
    if (USceneComponent* Resolved = ResolveSceneReference(BarrelPitchPivot, BarrelPitchTag))
    {
        return Resolved;
    }

    if (AActor* Owner = GetOwner())
    {
        TInlineComponentArray<UMBSTArticulationNodeMarkerComponent*> Markers(Owner);
        for (UMBSTArticulationNodeMarkerComponent* Marker : Markers)
        {
            if (IsValid(Marker) && Marker->Role == EMBSTArticulationNodeRole::BarrelPitch)
            {
                return Marker;
            }
        }
    }

    return nullptr;
}

USceneComponent* UMBSTSingleTurretAuthoringComponent::ResolveMuzzle() const
{
    if (USceneComponent* Resolved = ResolveSceneReference(Muzzle, MuzzleTag))
    {
        return Resolved;
    }

    if (AActor* Owner = GetOwner())
    {
        TInlineComponentArray<UMBSTArticulationNodeMarkerComponent*> Markers(Owner);
        for (UMBSTArticulationNodeMarkerComponent* Marker : Markers)
        {
            if (IsValid(Marker) && Marker->Role == EMBSTArticulationNodeRole::Muzzle)
            {
                return Marker;
            }
        }
    }

    return nullptr;
}

USkeletalMeshComponent* UMBSTSingleTurretAuthoringComponent::ResolveVATDriver() const
{
    return Cast<USkeletalMeshComponent>(ResolveSceneReference(VATDriver, VATDriverTag));
}

void UMBSTSingleTurretAuthoringComponent::ApplyFallbackTagsToSelectedNodes()
{
    auto AddTag = [](USceneComponent* Component, const FName Tag)
    {
        if (IsValid(Component) && !Tag.IsNone() && !Component->ComponentHasTag(Tag))
        {
            Component->Modify();
            Component->ComponentTags.Add(Tag);
            Component->MarkPackageDirty();
        }
    };

    AddTag(ResolveSceneReference(TurretYawPivot, NAME_None), TurretYawTag);
    AddTag(ResolveSceneReference(BarrelPitchPivot, NAME_None), BarrelPitchTag);
    AddTag(ResolveSceneReference(Muzzle, NAME_None), MuzzleTag);
    AddTag(ResolveSceneReference(VATDriver, NAME_None), VATDriverTag);
}

TArray<FString> UMBSTSingleTurretAuthoringComponent::ValidateAuthoring() const
{
    TArray<FString> Messages;

    const USceneComponent* Turret = ResolveTurretYawPivot();
    const USceneComponent* Barrel = ResolveBarrelPitchPivot();
    const USceneComponent* MuzzleComponent = ResolveMuzzle();

    if (!Turret)
    {
        Messages.Add(TEXT("No turret yaw pivot was resolved. Select TurretYawPivot, add MBST_TurretYaw, or use a marker."));
        return Messages;
    }

    auto IsDescendantOf = [](const USceneComponent* Child, const USceneComponent* Parent)
    {
        TSet<const USceneComponent*> Visited;
        for (const USceneComponent* Current = Child; Current; )
        {
            if (Current == Parent)
            {
                return true;
            }
            if (Visited.Contains(Current))
            {
                break;
            }
            Visited.Add(Current);

            if (const USceneComponent* AttachParent = Current->GetAttachParent())
            {
                Current = AttachParent;
            }
            else if (const AActor* CurrentOwner = Current->GetOwner(); CurrentOwner && CurrentOwner->GetRootComponent() == Current)
            {
                Current = CurrentOwner->GetParentComponent();
            }
            else
            {
                Current = nullptr;
            }
        }
        return false;
    };

    if (Barrel && !IsDescendantOf(Barrel, Turret))
    {
        Messages.Add(TEXT("The barrel pitch pivot is not a descendant of the turret yaw pivot."));
    }

    if (MuzzleComponent && Barrel && !IsDescendantOf(MuzzleComponent, Barrel))
    {
        Messages.Add(TEXT("The muzzle is not a descendant of the barrel pitch pivot."));
    }

    if (TurretLocalAxis.IsNearlyZero())
    {
        Messages.Add(TEXT("TurretLocalAxis is zero."));
    }

    if (Barrel && BarrelLocalAxis.IsNearlyZero())
    {
        Messages.Add(TEXT("BarrelLocalAxis is zero."));
    }

    if (MaximumRecoilDistance > 0.0f && BarrelLocalForwardAxis.IsNearlyZero())
    {
        Messages.Add(TEXT("MaximumRecoilDistance is non-zero but BarrelLocalForwardAxis is zero."));
    }

    if (bBodyUsesVAT && !ResolveVATDriver())
    {
        Messages.Add(TEXT("bBodyUsesVAT is true but VATDriver does not resolve to a SkeletalMeshComponent."));
    }

    return Messages;
}
