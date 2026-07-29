#include "MBSTSingleTurretEditorLibrary.h"

#include "MassBattleSingleTurretEditor.h"
#include "MBSTMobileFireProfile.h"
#include "MBSTSingleTurretAsset.h"
#include "MBSTSingleTurretAuthoringComponent.h"
#include "MBSTSingleTurretTypes.h"

#include "AnimToTextureBPLibrary.h"
#include "AnimToTextureDataAsset.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/ChildActorComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/MeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DataAssets/MassBattleAgentConfigDataAsset.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "GameFramework/Actor.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "MeshUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "NiagaraDataInterfaceArrayInt.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "Renderers/MassBattleAgentRenderer.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace MBSTEditorPrivate
{
    struct FGatheredActor
    {
        TArray<USceneComponent*> SceneComponents;
        TArray<UMeshComponent*> MeshComponents;
        TArray<FString> Messages;
    };

    struct FClassifiedComponents
    {
        TArray<UMeshComponent*> Body;
        TArray<UMeshComponent*> Turret;
        TArray<UMeshComponent*> Barrel;
    };

    static FString SanitizeAssetName(const FString& InName)
    {
        FString Result = InName;
        Result.TrimStartAndEndInline();
        Result.ReplaceInline(TEXT(" "), TEXT("_"));
        Result.ReplaceInline(TEXT("/"), TEXT("_"));
        Result.ReplaceInline(TEXT("\\"), TEXT("_"));
        Result.ReplaceInline(TEXT("."), TEXT("_"));
        Result.ReplaceInline(TEXT(":"), TEXT("_"));
        Result.ReplaceInline(TEXT("*"), TEXT("_"));
        Result.ReplaceInline(TEXT("?"), TEXT("_"));
        Result.ReplaceInline(TEXT("\""), TEXT("_"));
        Result.ReplaceInline(TEXT("<"), TEXT("_"));
        Result.ReplaceInline(TEXT(">"), TEXT("_"));
        Result.ReplaceInline(TEXT("|"), TEXT("_"));
        return Result;
    }

    static FString NormalizePackagePath(const FString& InPath)
    {
        FString Result = InPath;
        Result.TrimStartAndEndInline();
        Result.ReplaceInline(TEXT("\\"), TEXT("/"));
        while (Result.EndsWith(TEXT("/")))
        {
            Result.LeftChopInline(1);
        }
        return Result;
    }

    static FString MakePackageName(const FString& PackagePath, const FString& AssetName)
    {
        return NormalizePackagePath(PackagePath) + TEXT("/") + SanitizeAssetName(AssetName);
    }

    template<typename T>
    static int32 RemoveInstancedStructsOfType(TArray<FInstancedStruct>& Values)
    {
        return Values.RemoveAll([](const FInstancedStruct& Value)
        {
            return Value.GetScriptStruct() == T::StaticStruct();
        });
    }

    template<typename T>
    static void UpsertInstancedStruct(TArray<FInstancedStruct>& Values, const T& Value)
    {
        RemoveInstancedStructsOfType<T>(Values);
        Values.Add(FInstancedStruct::Make(Value));
    }

    static FMBSTSingleTurretShared MakeSharedLayout(UMBSTSingleTurretAsset* Layout)
    {
        FMBSTSingleTurretShared Shared;
        Shared.Layout = Layout;
        if (Layout)
        {
            Shared.YawLimitsDegrees = Layout->YawLimitsDegrees;
            Shared.PitchLimitsDegrees = Layout->PitchLimitsDegrees;
            Shared.bHasBarrelPitch = Layout->bHasBarrelPitch;
            Shared.YawSpeedDegreesPerSecond = Layout->YawSpeedDegreesPerSecond;
            Shared.PitchSpeedDegreesPerSecond = Layout->PitchSpeedDegreesPerSecond;
            Shared.RecoilReturnSpeed = Layout->RecoilReturnSpeed;
        }
        return Shared;
    }

    template<typename T>
    static const T* FindInstancedStruct(const TArray<FInstancedStruct>& Values, int32& OutCount)
    {
        const T* Found = nullptr;
        OutCount = 0;
        for (const FInstancedStruct& Value : Values)
        {
            if (Value.GetScriptStruct() == T::StaticStruct())
            {
                ++OutCount;
                Found = Value.GetPtr<T>();
            }
        }
        return Found;
    }

    static USceneComponent* GetLogicalParent(const USceneComponent* Component)
    {
        if (!Component)
        {
            return nullptr;
        }

        if (USceneComponent* Parent = Component->GetAttachParent())
        {
            return Parent;
        }

        const AActor* Owner = Component->GetOwner();
        if (Owner && Owner->GetRootComponent() == Component)
        {
            return Owner->GetParentComponent();
        }

        return nullptr;
    }

    static bool IsDescendantOrSelf(const USceneComponent* Component, const USceneComponent* PossibleAncestor)
    {
        if (!Component || !PossibleAncestor)
        {
            return false;
        }

        TSet<const USceneComponent*> Visited;
        for (const USceneComponent* Current = Component; Current; Current = GetLogicalParent(Current))
        {
            if (Current == PossibleAncestor)
            {
                return true;
            }
            if (Visited.Contains(Current))
            {
                break;
            }
            Visited.Add(Current);
        }
        return false;
    }

    static bool HasIgnoreMarkerInAncestors(const USceneComponent* Component, const FName IgnoreTag)
    {
        TSet<const USceneComponent*> Visited;
        for (const USceneComponent* Current = Component; Current; Current = GetLogicalParent(Current))
        {
            if (!IgnoreTag.IsNone() && Current->ComponentHasTag(IgnoreTag))
            {
                return true;
            }

            if (const UMBSTArticulationNodeMarkerComponent* Marker = Cast<UMBSTArticulationNodeMarkerComponent>(Current))
            {
                if (Marker->Role == EMBSTArticulationNodeRole::IgnoreSubtree)
                {
                    return true;
                }
            }

            if (Visited.Contains(Current))
            {
                break;
            }
            Visited.Add(Current);
        }
        return false;
    }

    static bool IsConvertibleMesh(const UMeshComponent* Component)
    {
        if (!IsValid(Component) || Component->IsA<UInstancedStaticMeshComponent>())
        {
            return false;
        }

        if (const UStaticMeshComponent* StaticComponent = Cast<UStaticMeshComponent>(Component))
        {
            return IsValid(StaticComponent->GetStaticMesh());
        }

        if (const USkeletalMeshComponent* SkeletalComponent = Cast<USkeletalMeshComponent>(Component))
        {
            return IsValid(SkeletalComponent->GetSkeletalMeshAsset());
        }

        return false;
    }

    static void GatherActorRecursive(
        AActor* Actor,
        const FMBSTActorToSingleTurretSettings& Settings,
        TSet<AActor*>& VisitedActors,
        FGatheredActor& OutGathered)
    {
        if (!IsValid(Actor) || VisitedActors.Contains(Actor))
        {
            return;
        }
        VisitedActors.Add(Actor);

        TInlineComponentArray<USceneComponent*> SceneComponents(Actor);
        for (USceneComponent* SceneComponent : SceneComponents)
        {
            if (!IsValid(SceneComponent))
            {
                continue;
            }
            if (!Settings.bIncludeEditorOnlyComponents && SceneComponent->IsEditorOnly())
            {
                continue;
            }

            OutGathered.SceneComponents.AddUnique(SceneComponent);

            UMeshComponent* MeshComponent = Cast<UMeshComponent>(SceneComponent);
            if (!MeshComponent)
            {
                continue;
            }

            if (MeshComponent->IsA<UInstancedStaticMeshComponent>())
            {
                OutGathered.Messages.AddUnique(FString::Printf(
                    TEXT("Instanced mesh '%s' was skipped. Expand ISM/HISM instances before conversion."),
                    *MeshComponent->GetPathName()));
                continue;
            }

            if (!Settings.bIncludeHiddenComponents && !MeshComponent->IsVisible())
            {
                continue;
            }

            if (IsConvertibleMesh(MeshComponent))
            {
                OutGathered.MeshComponents.AddUnique(MeshComponent);
            }
        }

        if (!Settings.bIncludeAttachedActors)
        {
            return;
        }

        TArray<AActor*> AttachedActors;
        Actor->GetAttachedActors(AttachedActors);
        for (AActor* AttachedActor : AttachedActors)
        {
            GatherActorRecursive(AttachedActor, Settings, VisitedActors, OutGathered);
        }

        TInlineComponentArray<UChildActorComponent*> ChildActorComponents(Actor);
        for (UChildActorComponent* ChildActorComponent : ChildActorComponents)
        {
            if (IsValid(ChildActorComponent))
            {
                GatherActorRecursive(ChildActorComponent->GetChildActor(), Settings, VisitedActors, OutGathered);
            }
        }
    }

    static FGatheredActor GatherActor(AActor* SourceActor, const FMBSTActorToSingleTurretSettings& Settings)
    {
        FGatheredActor Gathered;
        TSet<AActor*> VisitedActors;
        GatherActorRecursive(SourceActor, Settings, VisitedActors, Gathered);

        Gathered.SceneComponents.Sort([](const USceneComponent& A, const USceneComponent& B)
        {
            return A.GetPathName() < B.GetPathName();
        });
        Gathered.MeshComponents.Sort([](const UMeshComponent& A, const UMeshComponent& B)
        {
            return A.GetPathName() < B.GetPathName();
        });

        for (USceneComponent* SceneComponent : Gathered.SceneComponents)
        {
            if (USkeletalMeshComponent* SkeletalComponent = Cast<USkeletalMeshComponent>(SceneComponent))
            {
                if (SkeletalComponent->IsRegistered())
                {
                    SkeletalComponent->RefreshBoneTransforms(nullptr);
                }
            }
        }
        for (USceneComponent* SceneComponent : Gathered.SceneComponents)
        {
            if (IsValid(SceneComponent))
            {
                SceneComponent->UpdateComponentToWorld();
            }
        }

        return Gathered;
    }

    static FClassifiedComponents ClassifyComponents(
        const FGatheredActor& Gathered,
        const UMBSTSingleTurretAuthoringComponent& Authoring,
        USceneComponent* TurretPivot,
        USceneComponent* BarrelPivot,
        TArray<FString>& OutMessages)
    {
        FClassifiedComponents Result;

        for (UMeshComponent* MeshComponent : Gathered.MeshComponents)
        {
            if (!IsValid(MeshComponent) || HasIgnoreMarkerInAncestors(MeshComponent, Authoring.IgnoreTag))
            {
                continue;
            }

            if (BarrelPivot && IsDescendantOrSelf(MeshComponent, BarrelPivot))
            {
                Result.Barrel.Add(MeshComponent);
            }
            else if (TurretPivot && IsDescendantOrSelf(MeshComponent, TurretPivot))
            {
                Result.Turret.Add(MeshComponent);
            }
            else
            {
                Result.Body.Add(MeshComponent);
            }
        }

        if (Result.Body.IsEmpty())
        {
            OutMessages.Add(TEXT("No body mesh components were found outside the turret subtree."));
        }
        if (Result.Turret.IsEmpty())
        {
            OutMessages.Add(TEXT("No mesh components were found in the turret yaw subtree."));
        }
        if (BarrelPivot && Result.Barrel.IsEmpty())
        {
            OutMessages.Add(TEXT("A barrel pivot was selected, but its subtree contains no supported mesh component."));
        }

        return Result;
    }

    static void AppendComponentNames(const TArray<UMeshComponent*>& Components, TArray<FName>& OutNames)
    {
        for (const UMeshComponent* Component : Components)
        {
            if (IsValid(Component))
            {
                OutNames.Add(Component->GetFName());
            }
        }
    }

    static UStaticMesh* ConvertGroup(
        const TArray<UMeshComponent*>& SourceComponents,
        const FTransform& RootTransform,
        const FString& PackageName,
        const FMBSTActorToSingleTurretSettings& Settings,
        TArray<FString>& OutMessages)
    {
        if (SourceComponents.IsEmpty())
        {
            return nullptr;
        }

        TArray<UMeshComponent*> Components;
        Components.Reserve(SourceComponents.Num());
        for (UMeshComponent* Component : SourceComponents)
        {
            if (IsConvertibleMesh(Component))
            {
                Components.Add(Component);
            }
        }
        if (Components.IsEmpty())
        {
            return nullptr;
        }

        struct FSavedVisibility
        {
            UMeshComponent* Component = nullptr;
            bool bVisible = true;
            bool bHiddenInGame = false;
        };

        TArray<FSavedVisibility> SavedVisibility;
        if (Settings.bIncludeHiddenComponents)
        {
            SavedVisibility.Reserve(Components.Num());
            for (UMeshComponent* Component : Components)
            {
                SavedVisibility.Add({ Component, Component->GetVisibleFlag(), Component->bHiddenInGame != 0 });
                Component->SetVisibility(true, false);
                Component->SetHiddenInGame(false, false);
            }
        }

        IMeshUtilities& MeshUtilities = FModuleManager::LoadModuleChecked<IMeshUtilities>(TEXT("MeshUtilities"));
        UStaticMesh* Result = MeshUtilities.ConvertMeshesToStaticMesh(Components, RootTransform, PackageName);

        for (const FSavedVisibility& Saved : SavedVisibility)
        {
            if (IsValid(Saved.Component))
            {
                Saved.Component->SetVisibility(Saved.bVisible, false);
                Saved.Component->SetHiddenInGame(Saved.bHiddenInGame, false);
            }
        }

        if (!Result)
        {
            OutMessages.Add(FString::Printf(TEXT("ConvertMeshesToStaticMesh failed for '%s'."), *PackageName));
            return nullptr;
        }

        Result->Modify();
        for (int32 LODIndex = 0; LODIndex < Result->GetNumSourceModels(); ++LODIndex)
        {
            FStaticMeshSourceModel& SourceModel = Result->GetSourceModel(LODIndex);
            SourceModel.BuildSettings.bGenerateLightmapUVs = Settings.bGenerateLightmapUVs;
            if (Settings.bGenerateLightmapUVs)
            {
                SourceModel.BuildSettings.DstLightmapIndex = Settings.LightmapDestinationUV;
            }
        }

        MeshUtilities.FixupMaterialSlotNames(Result);
        Result->Build();
        Result->PostEditChange();
        Result->MarkPackageDirty();
        return Result;
    }

    static bool PaintVertexMask(UStaticMesh* Mesh, const FVector4f& Mask, TArray<FString>& OutMessages)
    {
        if (!Mesh)
        {
            return false;
        }

        bool bPaintedAnyLOD = false;
        Mesh->Modify();

        for (int32 LODIndex = 0; LODIndex < Mesh->GetNumSourceModels(); ++LODIndex)
        {
            FMeshDescription* MeshDescription = Mesh->GetMeshDescription(LODIndex);
            if (!MeshDescription)
            {
                OutMessages.Add(FString::Printf(
                    TEXT("Mesh '%s' has no editable MeshDescription for LOD %d."),
                    *Mesh->GetName(), LODIndex));
                continue;
            }

            FStaticMeshAttributes Attributes(*MeshDescription);
            Attributes.Register();
            TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
            if (!Colors.IsValid())
            {
                OutMessages.Add(FString::Printf(
                    TEXT("Could not register vertex-instance colors on '%s' LOD %d."),
                    *Mesh->GetName(), LODIndex));
                continue;
            }

            for (const FVertexInstanceID VertexInstanceID : MeshDescription->VertexInstances().GetElementIDs())
            {
                Colors[VertexInstanceID] = Mask;
            }

            Mesh->CommitMeshDescription(LODIndex);
            bPaintedAnyLOD = true;
        }

        if (bPaintedAnyLOD)
        {
            Mesh->Build();
            Mesh->PostEditChange();
            Mesh->MarkPackageDirty();
        }
        return bPaintedAnyLOD;
    }

    static UStaticMesh* MergePaintedGroups(
        const TArray<UStaticMesh*>& GroupMeshes,
        const FString& FinalPackageName,
        const FMBSTActorToSingleTurretSettings& Settings,
        TArray<FString>& OutMessages)
    {
        if (!FPackageName::IsValidLongPackageName(FinalPackageName))
        {
            OutMessages.Add(FString::Printf(TEXT("Invalid final mesh package name '%s'."), *FinalPackageName));
            return nullptr;
        }

        TArray<UStaticMesh*> ValidMeshes;
        int32 NumLODs = MAX_int32;
        for (UStaticMesh* Mesh : GroupMeshes)
        {
            if (!IsValid(Mesh))
            {
                continue;
            }
            ValidMeshes.Add(Mesh);
            NumLODs = FMath::Min(NumLODs, Mesh->GetNumSourceModels());
        }
        if (ValidMeshes.IsEmpty() || NumLODs <= 0 || NumLODs == MAX_int32)
        {
            OutMessages.Add(TEXT("No painted source MeshDescription LODs were available for the final merge."));
            return nullptr;
        }

        const FString AssetName = FPackageName::GetLongPackageAssetName(FinalPackageName);
        const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *FinalPackageName, *AssetName);
        if (FindObject<UObject>(nullptr, *ObjectPath))
        {
            OutMessages.Add(FString::Printf(TEXT("Asset already exists: %s"), *ObjectPath));
            return nullptr;
        }

        UPackage* Package = CreatePackage(*FinalPackageName);
        UStaticMesh* Result = NewObject<UStaticMesh>(
            Package,
            *AssetName,
            RF_Public | RF_Standalone | RF_Transactional);
        if (!Result)
        {
            OutMessages.Add(FString::Printf(TEXT("Failed to create final static mesh '%s'."), *ObjectPath));
            return nullptr;
        }

        Result->InitResources();
        Result->SetLightingGuid();
        Result->SetImportVersion(EImportStaticMeshVersion::LastVersion);
        Result->SetNumSourceModels(NumLODs);

        // Preserve the source material slots by name. AppendMeshDescriptions uses
        // the polygon-group material slot name when combining geometry.
        TMap<FName, UMaterialInterface*> MaterialsBySlot;
        for (UStaticMesh* Mesh : ValidMeshes)
        {
            for (const FStaticMaterial& Material : Mesh->GetStaticMaterials())
            {
                const FName SlotName = !Material.MaterialSlotName.IsNone()
                    ? Material.MaterialSlotName
                    : Material.ImportedMaterialSlotName;
                if (UMaterialInterface** Existing = MaterialsBySlot.Find(SlotName))
                {
                    if (*Existing != Material.MaterialInterface)
                    {
                        OutMessages.Add(FString::Printf(
                            TEXT("Cannot merge material slot '%s': different source materials use the same slot name."),
                            *SlotName.ToString()));
                        Result->ClearFlags(RF_Public | RF_Standalone);
                        Package->SetDirtyFlag(false);
                        return nullptr;
                    }
                    continue;
                }
                MaterialsBySlot.Add(SlotName, Material.MaterialInterface);
                Result->GetStaticMaterials().Add(Material);
            }
        }

        FStaticMeshOperations::FAppendSettings AppendSettings;
        AppendSettings.bMergeVertexColor = true;
        for (int32 UVIndex = 0; UVIndex < FStaticMeshOperations::FAppendSettings::MAX_NUM_UV_CHANNELS; ++UVIndex)
        {
            AppendSettings.bMergeUVChannels[UVIndex] = true;
        }

        for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
        {
            TArray<const FMeshDescription*> SourceDescriptions;
            SourceDescriptions.Reserve(ValidMeshes.Num());
            for (UStaticMesh* Mesh : ValidMeshes)
            {
                const FMeshDescription* Description = Mesh->GetMeshDescription(LODIndex);
                if (!Description)
                {
                    OutMessages.Add(FString::Printf(
                        TEXT("Mesh '%s' has no MeshDescription for LOD %d."),
                        *Mesh->GetName(),
                        LODIndex));
                    Result->ClearFlags(RF_Public | RF_Standalone);
                    Package->SetDirtyFlag(false);
                    return nullptr;
                }
                SourceDescriptions.Add(Description);
            }

            FMeshDescription CombinedDescription;
            FStaticMeshAttributes(CombinedDescription).Register();
            FStaticMeshOperations::AppendMeshDescriptions(
                SourceDescriptions,
                CombinedDescription,
                AppendSettings);

            Result->CreateMeshDescription(LODIndex, MoveTemp(CombinedDescription));
            Result->CommitMeshDescription(LODIndex);

            FStaticMeshSourceModel& TargetSourceModel = Result->GetSourceModel(LODIndex);
            const FStaticMeshSourceModel& ReferenceSourceModel = ValidMeshes[0]->GetSourceModel(LODIndex);
            TargetSourceModel.BuildSettings = ReferenceSourceModel.BuildSettings;
            TargetSourceModel.ReductionSettings = ReferenceSourceModel.ReductionSettings;
            TargetSourceModel.ScreenSize = ReferenceSourceModel.ScreenSize;
            TargetSourceModel.BuildSettings.bGenerateLightmapUVs = Settings.bGenerateLightmapUVs;
            if (Settings.bGenerateLightmapUVs)
            {
                TargetSourceModel.BuildSettings.DstLightmapIndex = Settings.LightmapDestinationUV;
            }
        }

        Result->SetLightMapCoordinateIndex(Settings.bGenerateLightmapUVs ? Settings.LightmapDestinationUV : 0);
        Result->Build(false);
        Result->PostEditChange();
        Result->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(Result);

        return Result;
    }

    static FTransform ToActorSpace(const USceneComponent& Component, const AActor& SourceActor)
    {
        return Component.GetComponentTransform().GetRelativeTransform(SourceActor.GetActorTransform());
    }

    static FVector ToActorSpaceAxis(
        const USceneComponent& Component,
        const AActor& SourceActor,
        const FVector& ComponentLocalAxis,
        const FVector& Fallback)
    {
        const FVector WorldAxis = Component.GetComponentTransform().TransformVectorNoScale(ComponentLocalAxis);
        const FVector ActorAxis = SourceActor.GetActorTransform().InverseTransformVectorNoScale(WorldAxis);
        return ActorAxis.GetSafeNormal(SMALL_NUMBER, Fallback);
    }

    static void UpsertSocket(UStaticMesh* Mesh, const FName SocketName, const FTransform& LocalTransform)
    {
        if (!Mesh || SocketName.IsNone())
        {
            return;
        }

        Mesh->Modify();
        if (UStaticMeshSocket* Existing = Mesh->FindSocket(SocketName))
        {
            Mesh->RemoveSocket(Existing);
        }

        UStaticMeshSocket* Socket = NewObject<UStaticMeshSocket>(Mesh, NAME_None, RF_Transactional);
        Socket->SocketName = SocketName;
        Socket->RelativeLocation = LocalTransform.GetLocation();
        Socket->RelativeRotation = LocalTransform.Rotator();
        Socket->RelativeScale = LocalTransform.GetScale3D();
        Socket->Tag = TEXT("MassBattleSingleTurret");
        Mesh->AddSocket(Socket);
        Mesh->PostEditChange();
        Mesh->MarkPackageDirty();
    }

    template<typename TObjectType>
    static TObjectType* CreateAssetObject(
        const FString& PackageName,
        const FString& AssetName,
        TArray<FString>& OutMessages)
    {
        if (!FPackageName::IsValidLongPackageName(PackageName))
        {
            OutMessages.Add(FString::Printf(TEXT("Invalid long package name '%s'."), *PackageName));
            return nullptr;
        }

        const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        if (FindObject<UObject>(nullptr, *ObjectPath))
        {
            OutMessages.Add(FString::Printf(TEXT("Asset already exists: %s"), *ObjectPath));
            return nullptr;
        }

        UPackage* Package = CreatePackage(*PackageName);
        TObjectType* Asset = NewObject<TObjectType>(
            Package,
            *AssetName,
            RF_Public | RF_Standalone | RF_Transactional);
        if (Asset)
        {
            FAssetRegistryModule::AssetCreated(Asset);
            Asset->MarkPackageDirty();
        }
        return Asset;
    }

    static bool InvokeAnimationToTexture(UAnimToTextureDataAsset* DataAsset, FString& OutMessage)
    {
        UFunction* Function = UAnimToTextureBPLibrary::StaticClass()->FindFunctionByName(TEXT("AnimationToTexture"));
        UObject* LibraryCDO = UAnimToTextureBPLibrary::StaticClass()->GetDefaultObject();
        if (!Function || !LibraryCDO)
        {
            OutMessage = TEXT("AnimToTexture AnimationToTexture function was not found in this engine version.");
            return false;
        }

        TArray<uint8> Params;
        Params.SetNumZeroed(Function->ParmsSize);

        bool bAssignedDataAsset = false;
        FBoolProperty* ReturnProperty = nullptr;
        for (TFieldIterator<FProperty> It(Function); It && (It->PropertyFlags & CPF_Parm); ++It)
        {
            FProperty* Property = *It;
            if (Property->HasAnyPropertyFlags(CPF_ReturnParm))
            {
                ReturnProperty = CastField<FBoolProperty>(Property);
                continue;
            }

            if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
            {
                if (DataAsset && DataAsset->IsA(ObjectProperty->PropertyClass))
                {
                    ObjectProperty->SetObjectPropertyValue(
                        ObjectProperty->ContainerPtrToValuePtr<void>(Params.GetData()),
                        DataAsset);
                    bAssignedDataAsset = true;
                }
                continue;
            }

            // UE 5.2 exposed an additional RootTransform parameter; newer
            // versions only expose DataAsset. Initialize it explicitly when present.
            if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
            {
                if (StructProperty->Struct == TBaseStructure<FTransform>::Get())
                {
                    *StructProperty->ContainerPtrToValuePtr<FTransform>(Params.GetData()) = FTransform::Identity;
                }
            }
        }

        if (!bAssignedDataAsset)
        {
            OutMessage = TEXT("Could not bind the AnimToTexture data asset parameter by reflection.");
            return false;
        }

        LibraryCDO->ProcessEvent(Function, Params.GetData());
        if (ReturnProperty)
        {
            const bool bResult = ReturnProperty->GetPropertyValue(ReturnProperty->ContainerPtrToValuePtr<void>(Params.GetData()));
            OutMessage = bResult
                ? TEXT("AnimToTexture bake completed.")
                : TEXT("AnimToTexture bake returned false. Check the template animation list, mode and UV channel.");
            return bResult;
        }

        OutMessage = TEXT("AnimToTexture bake was invoked; this engine version exposes no Boolean return value.");
        return true;
    }

    static UAnimToTextureDataAsset* CreateVATDataAsset(
        const FString& PackagePath,
        const FString& BaseAssetName,
        UAnimToTextureDataAsset* Template,
        TArray<FString>& OutMessages)
    {
        const FString AssetName = SanitizeAssetName(BaseAssetName + TEXT("_VAT"));
        const FString PackageName = MakePackageName(PackagePath, AssetName);
        if (!FPackageName::IsValidLongPackageName(PackageName))
        {
            OutMessages.Add(FString::Printf(TEXT("Invalid VAT package name '%s'."), *PackageName));
            return nullptr;
        }

        UPackage* Package = CreatePackage(*PackageName);
        if (!Package)
        {
            OutMessages.Add(FString::Printf(TEXT("Could not create VAT package '%s'."), *PackageName));
            return nullptr;
        }

        if (FindObject<UObject>(Package, *AssetName))
        {
            OutMessages.Add(FString::Printf(TEXT("VAT asset already exists: %s.%s"), *PackageName, *AssetName));
            return nullptr;
        }

        UAnimToTextureDataAsset* DataAsset = Template
            ? DuplicateObject<UAnimToTextureDataAsset>(Template, Package, *AssetName)
            : NewObject<UAnimToTextureDataAsset>(
                Package,
                *AssetName,
                RF_Public | RF_Standalone | RF_Transactional);

        if (!DataAsset)
        {
            OutMessages.Add(TEXT("Failed to create the AnimToTexture data asset."));
            return nullptr;
        }

        DataAsset->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
        FAssetRegistryModule::AssetCreated(DataAsset);
        DataAsset->MarkPackageDirty();
        return DataAsset;
    }

    static void ReleaseIntermediateMesh(UStaticMesh* Mesh, const bool bKeep, TArray<FString>& OutMessages)
    {
        if (!Mesh)
        {
            return;
        }

        if (bKeep)
        {
            OutMessages.Add(FString::Printf(TEXT("Kept intermediate mesh: %s"), *Mesh->GetPathName()));
            return;
        }

        Mesh->ClearFlags(RF_Public | RF_Standalone);
        if (UPackage* Package = Mesh->GetOutermost())
        {
            Package->SetDirtyFlag(false);
        }
    }

    static bool ReadMaskStatistics(
        UStaticMesh* Mesh,
        int64& OutBodyCount,
        int64& OutTurretCount,
        int64& OutBarrelCount,
        int64& OutInvalidCount,
        int64& OutBodyVATCount)
    {
        OutBodyCount = 0;
        OutTurretCount = 0;
        OutBarrelCount = 0;
        OutInvalidCount = 0;
        OutBodyVATCount = 0;

        if (!Mesh)
        {
            return false;
        }

        bool bReadAny = false;
        for (int32 LODIndex = 0; LODIndex < Mesh->GetNumSourceModels(); ++LODIndex)
        {
            FMeshDescription* Description = Mesh->GetMeshDescription(LODIndex);
            if (!Description)
            {
                continue;
            }

            FStaticMeshAttributes Attributes(*Description);
            TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
            if (!Colors.IsValid())
            {
                continue;
            }

            for (const FVertexInstanceID VertexInstanceID : Description->VertexInstances().GetElementIDs())
            {
                const FVector4f Color = Colors[VertexInstanceID];
                const bool bYaw = Color.X > 0.5f;
                const bool bPitch = Color.Y > 0.5f;
                const bool bBodyVAT = Color.Z > 0.5f;

                if (!bYaw && !bPitch)
                {
                    ++OutBodyCount;
                    if (bBodyVAT)
                    {
                        ++OutBodyVATCount;
                    }
                }
                else if (bYaw && !bPitch)
                {
                    ++OutTurretCount;
                }
                else if (bYaw && bPitch)
                {
                    ++OutBarrelCount;
                }
                else
                {
                    ++OutInvalidCount;
                }
            }
            bReadAny = true;
        }
        return bReadAny;
    }
}

FMBSTActorToSingleTurretResult UMBSTSingleTurretEditorLibrary::ConvertActorToSingleTurretVAT(
    AActor* SourceActor,
    UMBSTSingleTurretAuthoringComponent* Authoring,
    const FString& PackagePath,
    const FString& AssetName,
    const FMBSTActorToSingleTurretSettings& Settings)
{
    FMBSTActorToSingleTurretResult Result;

    if (!IsValid(SourceActor))
    {
        Result.Messages.Add(TEXT("SourceActor is invalid."));
        return Result;
    }

    if (!IsValid(Authoring) || Authoring->GetOwner() != SourceActor)
    {
        Result.Messages.Add(TEXT("Authoring must be a valid MBSTSingleTurretAuthoringComponent owned by SourceActor."));
        return Result;
    }

    const FString SafeAssetName = MBSTEditorPrivate::SanitizeAssetName(AssetName);
    const FString SafePackagePath = MBSTEditorPrivate::NormalizePackagePath(PackagePath);
    if (SafeAssetName.IsEmpty() || !FPackageName::IsValidLongPackageName(SafePackagePath))
    {
        Result.Messages.Add(FString::Printf(
            TEXT("Invalid package path or asset name. Path='%s', Name='%s'."),
            *PackagePath,
            *AssetName));
        return Result;
    }

    if (Settings.bRerunConstructionScripts)
    {
        SourceActor->RerunConstructionScripts();
    }

    Result.Messages.Append(Authoring->ValidateAuthoring());

    USceneComponent* TurretPivot = Authoring->ResolveTurretYawPivot();
    USceneComponent* BarrelPivot = Authoring->ResolveBarrelPitchPivot();
    USceneComponent* Muzzle = Authoring->ResolveMuzzle();
    if (!TurretPivot)
    {
        Result.Messages.Add(TEXT("Conversion stopped because no turret yaw pivot resolved."));
        return Result;
    }
    if (BarrelPivot && !MBSTEditorPrivate::IsDescendantOrSelf(BarrelPivot, TurretPivot))
    {
        Result.Messages.Add(TEXT("Conversion stopped because BarrelPitchPivot is not inside TurretYawPivot's subtree."));
        return Result;
    }

    MBSTEditorPrivate::FGatheredActor Gathered = MBSTEditorPrivate::GatherActor(SourceActor, Settings);
    Result.Messages.Append(Gathered.Messages);

    MBSTEditorPrivate::FClassifiedComponents Classified = MBSTEditorPrivate::ClassifyComponents(
        Gathered,
        *Authoring,
        TurretPivot,
        BarrelPivot,
        Result.Messages);

    MBSTEditorPrivate::AppendComponentNames(Classified.Body, Result.BodyComponents);
    MBSTEditorPrivate::AppendComponentNames(Classified.Turret, Result.TurretComponents);
    MBSTEditorPrivate::AppendComponentNames(Classified.Barrel, Result.BarrelComponents);

    if (Classified.Body.IsEmpty() || Classified.Turret.IsEmpty())
    {
        Result.Messages.Add(TEXT("Conversion requires at least one body mesh and one turret mesh."));
        return Result;
    }

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString TempRoot = FString::Printf(TEXT("/Temp/MBST/%s"), *Guid);
    const FTransform ActorRootTransform = SourceActor->GetActorTransform();

    UStaticMesh* BodyMesh = MBSTEditorPrivate::ConvertGroup(
        Classified.Body,
        ActorRootTransform,
        MBSTEditorPrivate::MakePackageName(TempRoot, SafeAssetName + TEXT("_Body")),
        Settings,
        Result.Messages);

    UStaticMesh* TurretMesh = MBSTEditorPrivate::ConvertGroup(
        Classified.Turret,
        ActorRootTransform,
        MBSTEditorPrivate::MakePackageName(TempRoot, SafeAssetName + TEXT("_Turret")),
        Settings,
        Result.Messages);

    UStaticMesh* BarrelMesh = Classified.Barrel.IsEmpty()
        ? nullptr
        : MBSTEditorPrivate::ConvertGroup(
            Classified.Barrel,
            ActorRootTransform,
            MBSTEditorPrivate::MakePackageName(TempRoot, SafeAssetName + TEXT("_Barrel")),
            Settings,
            Result.Messages);

    ON_SCOPE_EXIT
    {
        MBSTEditorPrivate::ReleaseIntermediateMesh(BodyMesh, Settings.bKeepIntermediateMeshes, Result.Messages);
        MBSTEditorPrivate::ReleaseIntermediateMesh(TurretMesh, Settings.bKeepIntermediateMeshes, Result.Messages);
        MBSTEditorPrivate::ReleaseIntermediateMesh(BarrelMesh, Settings.bKeepIntermediateMeshes, Result.Messages);
    };

    const FVector4f BodyMask(0.0f, 0.0f, Authoring->bBodyUsesVAT ? 1.0f : 0.0f, 0.0f);
    const FVector4f TurretMask(1.0f, 0.0f, 0.0f, 0.0f);
    const FVector4f BarrelMask(1.0f, 1.0f, 0.0f, 1.0f);

    if (!BodyMesh || !TurretMesh
        || !MBSTEditorPrivate::PaintVertexMask(BodyMesh, BodyMask, Result.Messages)
        || !MBSTEditorPrivate::PaintVertexMask(TurretMesh, TurretMask, Result.Messages)
        || (BarrelMesh && !MBSTEditorPrivate::PaintVertexMask(BarrelMesh, BarrelMask, Result.Messages)))
    {
        Result.Messages.Add(TEXT("Conversion stopped while creating vertex articulation masks."));
        return Result;
    }

    TArray<UStaticMesh*> GroupMeshes;
    GroupMeshes.Add(BodyMesh);
    GroupMeshes.Add(TurretMesh);
    if (BarrelMesh)
    {
        GroupMeshes.Add(BarrelMesh);
    }

    const FString FinalMeshPackageName = MBSTEditorPrivate::MakePackageName(SafePackagePath, SafeAssetName);
    Result.ArticulatedMesh = MBSTEditorPrivate::MergePaintedGroups(
        GroupMeshes,
        FinalMeshPackageName,
        Settings,
        Result.Messages);

    if (!Result.ArticulatedMesh)
    {
        Result.Messages.Add(TEXT("Failed to merge the painted body/turret/barrel groups."));
        return Result;
    }

    int64 BodyVertexCount = 0;
    int64 TurretVertexCount = 0;
    int64 BarrelVertexCount = 0;
    int64 InvalidVertexCount = 0;
    int64 BodyVATVertexCount = 0;
    const bool bReadableMasks = MBSTEditorPrivate::ReadMaskStatistics(
        Result.ArticulatedMesh,
        BodyVertexCount,
        TurretVertexCount,
        BarrelVertexCount,
        InvalidVertexCount,
        BodyVATVertexCount);
    Result.Messages.Add(FString::Printf(
        TEXT("Generated mask counts: Body=%lld, Turret=%lld, Barrel=%lld, BodyVAT=%lld, Invalid=%lld."),
        BodyVertexCount,
        TurretVertexCount,
        BarrelVertexCount,
        BodyVATVertexCount,
        InvalidVertexCount));
    if (!bReadableMasks
        || BodyVertexCount == 0
        || TurretVertexCount == 0
        || InvalidVertexCount > 0
        || (Authoring->bBodyUsesVAT && BodyVATVertexCount == 0))
    {
        Result.Messages.Add(TEXT(
            "Conversion stopped because the final mesh did not preserve the required body/turret vertex masks."));
        Result.ArticulatedMesh->ClearFlags(RF_Public | RF_Standalone);
        Result.ArticulatedMesh->GetOutermost()->SetDirtyFlag(false);
        Result.ArticulatedMesh = nullptr;
        return Result;
    }

    Result.ArticulatedMesh->Modify();
    const FVector BoundsExtension(Settings.BoundsExtension);
    Result.ArticulatedMesh->SetPositiveBoundsExtension(BoundsExtension);
    Result.ArticulatedMesh->SetNegativeBoundsExtension(BoundsExtension);
    Result.ArticulatedMesh->PostEditChange();
    Result.ArticulatedMesh->MarkPackageDirty();

    const FTransform TurretTransform = MBSTEditorPrivate::ToActorSpace(*TurretPivot, *SourceActor);
    const FTransform BarrelTransform = BarrelPivot
        ? MBSTEditorPrivate::ToActorSpace(*BarrelPivot, *SourceActor)
        : TurretTransform;
    const FTransform MuzzleTransform = Muzzle
        ? MBSTEditorPrivate::ToActorSpace(*Muzzle, *SourceActor)
        : BarrelTransform;

    if (Settings.bCreatePivotSockets)
    {
        MBSTEditorPrivate::UpsertSocket(Result.ArticulatedMesh, TEXT("MBST_TurretPivot"), TurretTransform);
        MBSTEditorPrivate::UpsertSocket(Result.ArticulatedMesh, TEXT("MBST_BarrelPivot"), BarrelTransform);
        MBSTEditorPrivate::UpsertSocket(Result.ArticulatedMesh, TEXT("MBST_Muzzle"), MuzzleTransform);
    }

    const FString LayoutAssetName = MBSTEditorPrivate::SanitizeAssetName(SafeAssetName + TEXT("_SingleTurret"));
    const FString LayoutPackageName = MBSTEditorPrivate::MakePackageName(SafePackagePath, LayoutAssetName);
    Result.LayoutAsset = MBSTEditorPrivate::CreateAssetObject<UMBSTSingleTurretAsset>(
        LayoutPackageName,
        LayoutAssetName,
        Result.Messages);

    if (!Result.LayoutAsset)
    {
        Result.Messages.Add(TEXT("The mesh was generated, but the layout data asset could not be created."));
        return Result;
    }

    Result.LayoutAsset->ArticulatedMesh = Result.ArticulatedMesh;
    Result.LayoutAsset->TurretPivotObjectSpace = FVector3f(TurretTransform.GetLocation());
    Result.LayoutAsset->TurretAxisObjectSpace = FVector3f(MBSTEditorPrivate::ToActorSpaceAxis(
        *TurretPivot,
        *SourceActor,
        Authoring->TurretLocalAxis,
        FVector::UpVector));
    Result.LayoutAsset->BarrelPivotObjectSpace = FVector3f(BarrelTransform.GetLocation());
    Result.LayoutAsset->BarrelAxisObjectSpace = FVector3f(BarrelPivot
        ? MBSTEditorPrivate::ToActorSpaceAxis(
            *BarrelPivot,
            *SourceActor,
            Authoring->BarrelLocalAxis,
            FVector::YAxisVector)
        : FVector::YAxisVector);
    Result.LayoutAsset->BarrelForwardAxisObjectSpace = FVector3f(BarrelPivot
        ? MBSTEditorPrivate::ToActorSpaceAxis(
            *BarrelPivot,
            *SourceActor,
            Authoring->BarrelLocalForwardAxis,
            FVector::ForwardVector)
        : MBSTEditorPrivate::ToActorSpaceAxis(
            *TurretPivot,
            *SourceActor,
            Authoring->BarrelLocalForwardAxis,
            FVector::ForwardVector));
    Result.LayoutAsset->MuzzleTransformObjectSpace = FTransform3f(MuzzleTransform);
    Result.LayoutAsset->YawLimitsDegrees = FVector2f(
        static_cast<float>(Authoring->YawLimitsDegrees.X),
        static_cast<float>(Authoring->YawLimitsDegrees.Y));
    Result.LayoutAsset->PitchLimitsDegrees = FVector2f(
        static_cast<float>(Authoring->PitchLimitsDegrees.X),
        static_cast<float>(Authoring->PitchLimitsDegrees.Y));
    Result.LayoutAsset->MaximumRecoilDistance = Authoring->MaximumRecoilDistance;
    Result.LayoutAsset->bHasBarrelPitch = BarrelPivot != nullptr;
    Result.LayoutAsset->YawSpeedDegreesPerSecond = Authoring->YawSpeedDegreesPerSecond;
    Result.LayoutAsset->PitchSpeedDegreesPerSecond = Authoring->PitchSpeedDegreesPerSecond;
    Result.LayoutAsset->RecoilReturnSpeed = Authoring->RecoilReturnSpeed;
    Result.LayoutAsset->bBodyUsesVAT = Authoring->bBodyUsesVAT;
    Result.LayoutAsset->BodySourceComponents = Result.BodyComponents;
    Result.LayoutAsset->TurretSourceComponents = Result.TurretComponents;
    Result.LayoutAsset->BarrelSourceComponents = Result.BarrelComponents;
    Result.LayoutAsset->PostEditChange();
    Result.LayoutAsset->MarkPackageDirty();

    if (Settings.bCreateVATDataAsset)
    {
        USkeletalMeshComponent* Driver = Authoring->ResolveVATDriver();
        if (!Driver)
        {
            Result.Messages.Add(TEXT("VAT creation was requested, but VATDriver did not resolve."));
        }
        else
        {
            Result.VATDataAsset = MBSTEditorPrivate::CreateVATDataAsset(
                SafePackagePath,
                SafeAssetName,
                Settings.VATTemplateDataAsset,
                Result.Messages);

            if (Result.VATDataAsset)
            {
                FString VATMessage;
                if (!ConfigureAndBakeAnimToTexture(
                    Result.VATDataAsset,
                    Driver,
                    Result.ArticulatedMesh,
                    Settings.bRunVATBake,
                    VATMessage))
                {
                    Result.Messages.Add(VATMessage);
                }
                else if (!VATMessage.IsEmpty())
                {
                    Result.Messages.Add(VATMessage);
                }

                Result.LayoutAsset->VATDataAsset = Result.VATDataAsset;
                Result.LayoutAsset->PostEditChange();
                Result.LayoutAsset->MarkPackageDirty();
            }
        }
    }

    {
        // This is intentionally part of the Actor-to-unit transaction. Nothing
        // outside this plugin conversion path receives turret fragments.
        FString ConfigMessage;
        Result.AgentConfig = CreateSingleTurretAgentConfigFromTemplate(
            Settings.AgentConfigTemplate,
            Result.LayoutAsset,
            SafePackagePath,
            SafeAssetName + TEXT("_AgentConfig"),
            Settings.InitialTurretState,
            Settings.MobileFireProfile,
            Settings.InitialMobileFireState,
            ConfigMessage);
        if (!ConfigMessage.IsEmpty())
        {
            Result.Messages.Add(ConfigMessage);
        }
    }

    Result.bSucceeded = IsValid(Result.ArticulatedMesh)
        && IsValid(Result.LayoutAsset)
        && IsValid(Result.AgentConfig);
    if (Result.bSucceeded)
    {
        Result.Messages.Add(IsValid(Result.AgentConfig)
            ? TEXT("Generated one articulated mesh, one shared layout and one directly spawnable turret AgentConfig. No child Agent or Host is required.")
            : TEXT("Generated one articulated StaticMesh and one single-turret layout. No child Agent or Host is required."));
    }
    return Result;
}

FMBSTAssetValidationResult UMBSTSingleTurretEditorLibrary::ValidateActorForSingleTurretConversion(
    AActor* SourceActor,
    UMBSTSingleTurretAuthoringComponent* Authoring,
    const FMBSTActorToSingleTurretSettings& Settings)
{
    FMBSTAssetValidationResult Result;
    if (!IsValid(SourceActor))
    {
        Result.Messages.Add(TEXT("SourceActor is invalid."));
        return Result;
    }
    if (!IsValid(Authoring) || Authoring->GetOwner() != SourceActor)
    {
        Result.Messages.Add(TEXT("Authoring is invalid or belongs to another Actor."));
        return Result;
    }

    const TArray<FString> AuthoringMessages = Authoring->ValidateAuthoring();
    Result.Messages.Append(AuthoringMessages);

    USceneComponent* Turret = Authoring->ResolveTurretYawPivot();
    USceneComponent* Barrel = Authoring->ResolveBarrelPitchPivot();
    if (!Turret)
    {
        return Result;
    }

    MBSTEditorPrivate::FGatheredActor Gathered = MBSTEditorPrivate::GatherActor(SourceActor, Settings);
    Result.Messages.Append(Gathered.Messages);
    MBSTEditorPrivate::FClassifiedComponents Classified = MBSTEditorPrivate::ClassifyComponents(
        Gathered,
        *Authoring,
        Turret,
        Barrel,
        Result.Messages);

    bool bHasFatal = Classified.Body.IsEmpty() || Classified.Turret.IsEmpty();
    for (const FString& Message : AuthoringMessages)
    {
        if (Message.Contains(TEXT("No turret yaw pivot")) || Message.Contains(TEXT("not a descendant")))
        {
            bHasFatal = true;
        }
    }

    Result.bValid = !bHasFatal;
    if (Result.bValid && Result.Messages.IsEmpty())
    {
        Result.Messages.Add(TEXT("Actor hierarchy is valid for single-turret conversion."));
    }
    return Result;
}

void UMBSTSingleTurretEditorLibrary::SetComponentArticulationRole(
    USceneComponent* Component,
    const EMBSTArticulationNodeRole Role,
    const bool bEnabled)
{
    if (!IsValid(Component))
    {
        return;
    }

    static const FName TurretTag(TEXT("MBST_TurretYaw"));
    static const FName BarrelTag(TEXT("MBST_BarrelPitch"));
    static const FName MuzzleTag(TEXT("MBST_Muzzle"));
    static const FName IgnoreTag(TEXT("MBST_Ignore"));

    FName DesiredTag = NAME_None;
    switch (Role)
    {
    case EMBSTArticulationNodeRole::TurretYaw:
        DesiredTag = TurretTag;
        break;
    case EMBSTArticulationNodeRole::BarrelPitch:
        DesiredTag = BarrelTag;
        break;
    case EMBSTArticulationNodeRole::Muzzle:
        DesiredTag = MuzzleTag;
        break;
    case EMBSTArticulationNodeRole::IgnoreSubtree:
        DesiredTag = IgnoreTag;
        break;
    default:
        break;
    }

    Component->Modify();
    if (bEnabled)
    {
        Component->ComponentTags.AddUnique(DesiredTag);
    }
    else
    {
        Component->ComponentTags.Remove(DesiredTag);
    }
    Component->MarkPackageDirty();
}

bool UMBSTSingleTurretEditorLibrary::EnsureNiagaraStyleArray(
    UNiagaraSystem* NiagaraSystem,
    FString& OutMessage)
{
    OutMessage.Reset();
    if (!IsValid(NiagaraSystem))
    {
        OutMessage = TEXT("NiagaraSystem is invalid.");
        return false;
    }

    const FName ParameterName(TEXT("User.StyleArray"));
    const FNiagaraTypeDefinition ExpectedType(UNiagaraDataInterfaceArrayInt32::StaticClass());
    FNiagaraUserRedirectionParameterStore& Parameters = NiagaraSystem->GetExposedParameters();

    TArray<FNiagaraVariable> ExistingVariables;
    Parameters.GetParameters(ExistingVariables);
    for (const FNiagaraVariable& Existing : ExistingVariables)
    {
        if (Existing.GetName() == ParameterName && Existing.GetType() != ExpectedType)
        {
            Parameters.RemoveParameter(Existing);
        }
    }

    const FNiagaraVariable StyleArray(ExpectedType, ParameterName);
    NiagaraSystem->Modify();
    if (Parameters.IndexOf(StyleArray) == INDEX_NONE && !Parameters.AddParameter(StyleArray))
    {
        OutMessage = TEXT("Could not add User.StyleArray to the Niagara exposed parameter store.");
        return false;
    }

    UNiagaraDataInterfaceArrayInt32* StyleDataInterface =
        Cast<UNiagaraDataInterfaceArrayInt32>(Parameters.GetDataInterface(StyleArray));
    if (!StyleDataInterface)
    {
        StyleDataInterface = NewObject<UNiagaraDataInterfaceArrayInt32>(
            NiagaraSystem,
            NAME_None,
            UNiagaraDataInterface::BuildObjectFlagsForOwner(NiagaraSystem, RF_Transactional));
        if (!StyleDataInterface)
        {
            OutMessage = TEXT("Could not create the User.StyleArray Niagara data interface.");
            return false;
        }
        Parameters.SetDataInterface(StyleDataInterface, StyleArray);
    }

    Parameters.TriggerOnLayoutChanged();
    NiagaraSystem->PostEditChange();
    NiagaraSystem->MarkPackageDirty();
    OutMessage = TEXT("Ensured User.StyleArray as an independent Niagara Array Int32 data interface.");
    return true;
}

FMBSTAssetValidationResult UMBSTSingleTurretEditorLibrary::ValidateNiagaraStyleArray(UNiagaraSystem* NiagaraSystem)
{
    FMBSTAssetValidationResult Result;
    if (!IsValid(NiagaraSystem))
    {
        Result.Messages.Add(TEXT("NiagaraSystem is invalid."));
        return Result;
    }

    bool bFound = false;
    bool bCorrectType = false;
    bool bHasDataInterface = false;
    const FNiagaraTypeDefinition ExpectedType(UNiagaraDataInterfaceArrayInt32::StaticClass());
    TArray<FNiagaraVariable> Variables;
    NiagaraSystem->GetExposedParameters().GetParameters(Variables);
    for (const FNiagaraVariable& Variable : Variables)
    {
        if (Variable.GetName() == FName(TEXT("User.StyleArray")))
        {
            bFound = true;
            bCorrectType = Variable.GetType() == ExpectedType;
            bHasDataInterface = bCorrectType
                && IsValid(NiagaraSystem->GetExposedParameters().GetDataInterface(Variable));
            break;
        }
    }

    if (!bFound)
    {
        Result.Messages.Add(TEXT("Missing exposed Niagara parameter: User.StyleArray (Niagara Array Int32)."));
        return Result;
    }
    if (!bCorrectType)
    {
        Result.Messages.Add(TEXT("User.StyleArray exists, but it is not a Niagara Array Int32."));
        return Result;
    }
    if (!bHasDataInterface)
    {
        Result.Messages.Add(TEXT("User.StyleArray has no independent default data-interface instance."));
        return Result;
    }

    Result.bValid = true;
    Result.Messages.Add(TEXT("User.StyleArray was found. MassBattle will upload packed turret states to it."));
    return Result;
}

FMBSTAssetValidationResult UMBSTSingleTurretEditorLibrary::ValidateGeneratedArticulatedMesh(
    UStaticMesh* ArticulatedMesh,
    const bool bExpectBodyVATMask)
{
    FMBSTAssetValidationResult Result;
    if (!IsValid(ArticulatedMesh))
    {
        Result.Messages.Add(TEXT("ArticulatedMesh is invalid."));
        return Result;
    }

    int64 BodyCount = 0;
    int64 TurretCount = 0;
    int64 BarrelCount = 0;
    int64 InvalidCount = 0;
    int64 BodyVATCount = 0;
    if (!MBSTEditorPrivate::ReadMaskStatistics(
        ArticulatedMesh,
        BodyCount,
        TurretCount,
        BarrelCount,
        InvalidCount,
        BodyVATCount))
    {
        Result.Messages.Add(TEXT("No readable vertex-instance color attributes were found."));
        return Result;
    }

    if (BodyCount == 0)
    {
        Result.Messages.Add(TEXT("No body vertices (R=0,G=0) were found."));
    }
    if (TurretCount == 0)
    {
        Result.Messages.Add(TEXT("No turret vertices (R=1,G=0) were found."));
    }
    if (InvalidCount > 0)
    {
        Result.Messages.Add(FString::Printf(TEXT("Found %lld vertices with unsupported masks."), InvalidCount));
    }
    if (bExpectBodyVATMask && BodyVATCount == 0)
    {
        Result.Messages.Add(TEXT("Body VAT was expected, but no body vertex has B=1."));
    }

    Result.Messages.Add(FString::Printf(
        TEXT("Mask counts: Body=%lld, Turret=%lld, Barrel=%lld, BodyVAT=%lld, Invalid=%lld."),
        BodyCount,
        TurretCount,
        BarrelCount,
        BodyVATCount,
        InvalidCount));

    Result.bValid = BodyCount > 0
        && TurretCount > 0
        && InvalidCount == 0
        && (!bExpectBodyVATMask || BodyVATCount > 0);
    return Result;
}

bool UMBSTSingleTurretEditorLibrary::ConfigureArticulationBaseMaterial(
    UMaterial* Material,
    FString& OutMessage)
{
    OutMessage.Reset();
    if (!IsValid(Material))
    {
        OutMessage = TEXT("Material is invalid.");
        return false;
    }

    constexpr TCHAR PositionFunctionName[] = TEXT("MBST_ArticulatePositionObjectSpace");
    constexpr TCHAR NormalFunctionName[] = TEXT("MBST_ArticulateNormalObjectSpace");
    constexpr TCHAR NormalOutputMarker[] = TEXT("MBST normal: articulated local to world");

    UMaterialExpressionCustom* PositionCustom = nullptr;
    UMaterialExpressionSetMaterialAttributes* NormalSet = nullptr;
    int32 NormalInputIndex = INDEX_NONE;
    const FGuid NormalAttributeId = FMaterialAttributeDefinitionMap::GetID(MP_Normal);

    for (UMaterialExpression* Expression : Material->GetExpressions())
    {
        if (UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expression))
        {
            if (Custom->Code.Contains(PositionFunctionName))
            {
                PositionCustom = Custom;
            }
        }

        UMaterialExpressionSetMaterialAttributes* SetAttributes =
            Cast<UMaterialExpressionSetMaterialAttributes>(Expression);
        if (!SetAttributes)
        {
            continue;
        }

        int32 AttributeIndex = INDEX_NONE;
        if (SetAttributes->AttributeSetTypes.Find(NormalAttributeId, AttributeIndex))
        {
            const int32 CandidateInputIndex = AttributeIndex + 1;
            if (SetAttributes->Inputs.IsValidIndex(CandidateInputIndex)
                && SetAttributes->Inputs[CandidateInputIndex].IsConnected())
            {
                UMaterialExpression* CurrentNormalSource =
                    SetAttributes->Inputs[CandidateInputIndex].Expression;
                if (CurrentNormalSource
                    && CurrentNormalSource->Desc.Equals(NormalOutputMarker, ESearchCase::CaseSensitive))
                {
                    Material->Modify();
                    Material->bTangentSpaceNormal = false;
                    Material->PostEditChange();
                    Material->MarkPackageDirty();
                    OutMessage = TEXT("The MBST articulated world-space normal path is already installed.");
                    return true;
                }

                if (!NormalSet)
                {
                    NormalSet = SetAttributes;
                    NormalInputIndex = CandidateInputIndex;
                }
            }
        }
    }

    if (!PositionCustom)
    {
        OutMessage = TEXT("Could not find the MBST position-articulation Custom expression.");
        return false;
    }
    if (!NormalSet || !NormalSet->Inputs.IsValidIndex(NormalInputIndex))
    {
        OutMessage = TEXT("Could not find a connected Normal attribute in the material graph.");
        return false;
    }
    if (!Material->bTangentSpaceNormal)
    {
        OutMessage = TEXT("The material already expects world-space normals, but no MBST normal path was found.");
        return false;
    }

    const FCustomInput* VertexMaskInput = PositionCustom->Inputs.FindByPredicate([](const FCustomInput& Input)
    {
        return Input.InputName == TEXT("VertexMask");
    });
    const FCustomInput* PackedHalvesInput = PositionCustom->Inputs.FindByPredicate([](const FCustomInput& Input)
    {
        return Input.InputName == TEXT("PackedHalves");
    });
    const FCustomInput* TurretAxisInput = PositionCustom->Inputs.FindByPredicate([](const FCustomInput& Input)
    {
        return Input.InputName == TEXT("TurretAxisObject");
    });
    const FCustomInput* BarrelAxisInput = PositionCustom->Inputs.FindByPredicate([](const FCustomInput& Input)
    {
        return Input.InputName == TEXT("BarrelAxisObject");
    });
    if (!VertexMaskInput || !VertexMaskInput->Input.IsConnected()
        || !PackedHalvesInput || !PackedHalvesInput->Input.IsConnected()
        || !TurretAxisInput || !TurretAxisInput->Input.IsConnected()
        || !BarrelAxisInput || !BarrelAxisInput->Input.IsConnected())
    {
        OutMessage = TEXT("The MBST position Custom expression is missing a required mask, packed-state or axis input.");
        return false;
    }

    const FExpressionInput OriginalNormalInput = NormalSet->Inputs[NormalInputIndex];
    UMaterialExpressionTransform* TangentToLocal = Cast<UMaterialExpressionTransform>(
        UMaterialEditingLibrary::CreateMaterialExpression(
            Material,
            UMaterialExpressionTransform::StaticClass(),
            NormalSet->MaterialExpressionEditorX - 900,
            NormalSet->MaterialExpressionEditorY - 350));
    UMaterialExpressionCustom* NormalCustom = Cast<UMaterialExpressionCustom>(
        UMaterialEditingLibrary::CreateMaterialExpression(
            Material,
            UMaterialExpressionCustom::StaticClass(),
            NormalSet->MaterialExpressionEditorX - 600,
            NormalSet->MaterialExpressionEditorY - 350));
    UMaterialExpressionTransform* LocalToWorld = Cast<UMaterialExpressionTransform>(
        UMaterialEditingLibrary::CreateMaterialExpression(
            Material,
            UMaterialExpressionTransform::StaticClass(),
            NormalSet->MaterialExpressionEditorX - 300,
            NormalSet->MaterialExpressionEditorY - 350));

    if (!TangentToLocal || !NormalCustom || !LocalToWorld)
    {
        if (LocalToWorld)
        {
            UMaterialEditingLibrary::DeleteMaterialExpression(Material, LocalToWorld);
        }
        if (NormalCustom)
        {
            UMaterialEditingLibrary::DeleteMaterialExpression(Material, NormalCustom);
        }
        if (TangentToLocal)
        {
            UMaterialEditingLibrary::DeleteMaterialExpression(Material, TangentToLocal);
        }
        OutMessage = TEXT("Could not create the MBST normal-articulation material expressions.");
        return false;
    }

    TangentToLocal->Modify();
    TangentToLocal->TransformSourceType = TRANSFORMSOURCE_Tangent;
    TangentToLocal->TransformType = TRANSFORM_Local;
    TangentToLocal->Desc = TEXT("MBST normal: tangent to local before articulation");
    TangentToLocal->Input = OriginalNormalInput;

    NormalCustom->Modify();
    const TArray<FString> NormalCodeLines = {
        TEXT("uint Packed = (uint(round(PackedHalves.x)) & 65535u) | ((uint(round(PackedHalves.y)) & 65535u) << 16u);"),
        TEXT("if (Packed == 0u) Packed = 0x08080000u;"),
        TEXT("int VisualStyle;"),
        TEXT("float4 YawPitchSinCos;"),
        TEXT("float RecoilNormalized;"),
        TEXT("MBST_DecodePackedStateSinCos((int)Packed, VisualStyle, YawPitchSinCos, RecoilNormalized);"),
        TEXT("float4 ArticulationMask = float4(VertexMask.rgb, VertexMask.g);"),
        TEXT("return MBST_ArticulateNormalObjectSpace(NormalObject, ArticulationMask, TurretAxisObject, BarrelAxisObject, YawPitchSinCos);")
    };
    NormalCustom->Code = FString::Join(NormalCodeLines, TEXT("\n"));
    NormalCustom->OutputType = CMOT_Float3;
    NormalCustom->Description = TEXT("MBST single-turret articulated normal; preserves the source normal map");
    NormalCustom->Desc = TEXT("MBST rotate source normal with turret/barrel");
    NormalCustom->Inputs.Reset();
    NormalCustom->AdditionalOutputs.Reset();
    NormalCustom->AdditionalDefines.Reset();
    NormalCustom->IncludeFilePaths = { TEXT("/Plugin/MassBattleSingleTurret/Private/MBSTSingleTurret.ush") };
    NormalCustom->ContainsClipInstruction = CMCI_No;

    auto AddCustomInput = [NormalCustom](const FName Name, const FExpressionInput& Input)
    {
        FCustomInput& NewInput = NormalCustom->Inputs.AddDefaulted_GetRef();
        NewInput.InputName = Name;
        NewInput.Input = Input;
    };

    FExpressionInput NormalObjectInput;
    NormalObjectInput.Connect(0, TangentToLocal);
    AddCustomInput(TEXT("NormalObject"), NormalObjectInput);
    AddCustomInput(TEXT("VertexMask"), VertexMaskInput->Input);
    AddCustomInput(TEXT("PackedHalves"), PackedHalvesInput->Input);
    AddCustomInput(TEXT("TurretAxisObject"), TurretAxisInput->Input);
    AddCustomInput(TEXT("BarrelAxisObject"), BarrelAxisInput->Input);
    NormalCustom->RebuildOutputs();

    LocalToWorld->Modify();
    LocalToWorld->TransformSourceType = TRANSFORMSOURCE_Local;
    LocalToWorld->TransformType = TRANSFORM_World;
    LocalToWorld->Desc = NormalOutputMarker;
    LocalToWorld->Input.Connect(0, NormalCustom);

    Material->Modify();
    NormalSet->Modify();
    const bool bWasTangentSpaceNormal = Material->bTangentSpaceNormal;
    const bool bConnected = NormalSet->ConnectInputAttribute(MP_Normal, LocalToWorld, 0);
    Material->bTangentSpaceNormal = false;
    Material->PostEditChange();

    TArray<FString> CompileErrors;
    if (bConnected)
    {
        CompileErrors = UMaterialEditingLibrary::RecompileMaterial(Material);
    }
    if (!bConnected || !CompileErrors.IsEmpty())
    {
        NormalSet->Inputs[NormalInputIndex] = OriginalNormalInput;
        Material->bTangentSpaceNormal = bWasTangentSpaceNormal;
        UMaterialEditingLibrary::DeleteMaterialExpression(Material, LocalToWorld);
        UMaterialEditingLibrary::DeleteMaterialExpression(Material, NormalCustom);
        UMaterialEditingLibrary::DeleteMaterialExpression(Material, TangentToLocal);
        Material->PostEditChange();
        UMaterialEditingLibrary::RecompileMaterial(Material);

        OutMessage = bConnected
            ? FString::Printf(
                TEXT("The MBST articulated-normal graph did not compile: %s"),
                *FString::Join(CompileErrors, TEXT(" | ")))
            : TEXT("Could not connect the articulated world-space normal to the material attributes.");
        return false;
    }

    Material->MarkPackageDirty();
    OutMessage = FString::Printf(
        TEXT("Installed %s with tangent-to-local and local-to-world transforms; hull normals remain unchanged and articulated normals follow turret/barrel rotation."),
        NormalFunctionName);
    return true;
}

bool UMBSTSingleTurretEditorLibrary::ConfigureArticulationMaterialInstance(
    UMaterialInstanceConstant* MaterialInstance,
    UMBSTSingleTurretAsset* Layout,
    FString& OutMessage)
{
    OutMessage.Reset();
    if (!IsValid(MaterialInstance) || !IsValid(Layout))
    {
        OutMessage = TEXT("MaterialInstance or Layout is invalid.");
        return false;
    }

    MaterialInstance->Modify();
    MaterialInstance->SetVectorParameterValueEditorOnly(
        TEXT("MBST_TurretPivot"),
        FLinearColor(Layout->TurretPivotObjectSpace.X, Layout->TurretPivotObjectSpace.Y, Layout->TurretPivotObjectSpace.Z, 0.0f));
    MaterialInstance->SetVectorParameterValueEditorOnly(
        TEXT("MBST_TurretAxis"),
        FLinearColor(Layout->TurretAxisObjectSpace.X, Layout->TurretAxisObjectSpace.Y, Layout->TurretAxisObjectSpace.Z, 0.0f));
    MaterialInstance->SetVectorParameterValueEditorOnly(
        TEXT("MBST_BarrelPivot"),
        FLinearColor(Layout->BarrelPivotObjectSpace.X, Layout->BarrelPivotObjectSpace.Y, Layout->BarrelPivotObjectSpace.Z, 0.0f));
    MaterialInstance->SetVectorParameterValueEditorOnly(
        TEXT("MBST_BarrelAxis"),
        FLinearColor(Layout->BarrelAxisObjectSpace.X, Layout->BarrelAxisObjectSpace.Y, Layout->BarrelAxisObjectSpace.Z, 0.0f));
    MaterialInstance->SetVectorParameterValueEditorOnly(
        TEXT("MBST_BarrelForward"),
        FLinearColor(Layout->BarrelForwardAxisObjectSpace.X, Layout->BarrelForwardAxisObjectSpace.Y, Layout->BarrelForwardAxisObjectSpace.Z, 0.0f));
    MaterialInstance->SetScalarParameterValueEditorOnly(
        TEXT("MBST_MaxRecoilDistance"),
        Layout->MaximumRecoilDistance);
    MaterialInstance->PostEditChange();
    MaterialInstance->MarkPackageDirty();

    OutMessage = TEXT("Configured MBST pivot, axis and recoil material parameters.");
    return true;
}

bool UMBSTSingleTurretEditorLibrary::ConfigureMassBattleRendererClass(
    TSubclassOf<AMassBattleAgentRenderer> RendererClass,
    UMBSTSingleTurretAsset* Layout,
    UNiagaraSystem* NiagaraSystem,
    FString& OutMessage)
{
    OutMessage.Reset();
    if (!RendererClass || !IsValid(Layout) || !IsValid(Layout->ArticulatedMesh) || !IsValid(NiagaraSystem))
    {
        OutMessage = TEXT("RendererClass, Layout/ArticulatedMesh or NiagaraSystem is invalid.");
        return false;
    }

    AMassBattleAgentRenderer* CDO = RendererClass->GetDefaultObject<AMassBattleAgentRenderer>();
    if (!CDO)
    {
        OutMessage = TEXT("Could not get the MassBattle renderer class default object.");
        return false;
    }

    CDO->Modify();
    CDO->AgentMesh = Layout->ArticulatedMesh;
    CDO->NiagaraSystemAsset = NiagaraSystem;
    CDO->PostEditChange();
    CDO->MarkPackageDirty();
    RendererClass->MarkPackageDirty();

    OutMessage = TEXT("MassBattle renderer defaults now reference the articulated mesh and Niagara system.");
    return true;
}

bool UMBSTSingleTurretEditorLibrary::ConfigureAgentConfigRenderer(
    UMassBattleAgentConfigDataAsset* AgentConfig,
    TSubclassOf<AMassBattleAgentRenderer> RendererClass,
    FString& OutMessage)
{
    OutMessage.Reset();
    if (!IsValid(AgentConfig) || !RendererClass)
    {
        OutMessage = TEXT("AgentConfig or RendererClass is invalid.");
        return false;
    }

    AgentConfig->Modify();
    AgentConfig->Visualize.bEnable = true;
    AgentConfig->Visualize.RendererClass = RendererClass.Get();
    AgentConfig->PostEditChange();
    AgentConfig->MarkPackageDirty();

    OutMessage = TEXT("MassBattle Agent Config now references the single-turret renderer class.");
    return true;
}

bool UMBSTSingleTurretEditorLibrary::ConfigureAgentConfigSingleTurret(
    UMassBattleAgentConfigDataAsset* AgentConfig,
    UMBSTSingleTurretAsset* Layout,
    const FMBSTSingleTurretState& InitialState,
    const bool bEnable,
    FString& OutMessage)
{
    OutMessage.Reset();
    if (!IsValid(AgentConfig))
    {
        OutMessage = TEXT("AgentConfig is invalid.");
        return false;
    }
    if (bEnable && !IsValid(Layout))
    {
        OutMessage = TEXT("A valid single-turret Layout is required when enabling turret support.");
        return false;
    }

    AgentConfig->Modify();

    int32 ExistingStateCount = 0;
    const FMBSTSingleTurretState* ExistingState = MBSTEditorPrivate::FindInstancedStruct<FMBSTSingleTurretState>(
        AgentConfig->ExtraData.Fragments,
        ExistingStateCount);
    const uint8 RestoredVisualStyle = ExistingState
        ? ExistingState->VisualStyle
        : InitialState.VisualStyle;

    MBSTEditorPrivate::RemoveInstancedStructsOfType<FMBSTSingleTurretTag>(AgentConfig->ExtraData.Tags);
    MBSTEditorPrivate::RemoveInstancedStructsOfType<FMBSTSingleTurretState>(AgentConfig->ExtraData.Fragments);
    MBSTEditorPrivate::RemoveInstancedStructsOfType<FMBSTSingleTurretShared>(AgentConfig->ExtraData.MutableSharedFragments);
    // Remove misplaced copies too, so one generated config has one unambiguous contract.
    MBSTEditorPrivate::RemoveInstancedStructsOfType<FMBSTSingleTurretTag>(AgentConfig->ExtraData.Fragments);
    MBSTEditorPrivate::RemoveInstancedStructsOfType<FMBSTSingleTurretState>(AgentConfig->ExtraData.MutableSharedFragments);
    MBSTEditorPrivate::RemoveInstancedStructsOfType<FMBSTSingleTurretShared>(AgentConfig->ExtraData.ConstSharedFragments);

    if (bEnable)
    {
        MBSTEditorPrivate::UpsertInstancedStruct(
            AgentConfig->ExtraData.Tags,
            FMBSTSingleTurretTag());
        MBSTEditorPrivate::UpsertInstancedStruct(
            AgentConfig->ExtraData.Fragments,
            InitialState);
        MBSTEditorPrivate::UpsertInstancedStruct(
            AgentConfig->ExtraData.MutableSharedFragments,
            MBSTEditorPrivate::MakeSharedLayout(Layout));

        // MassBattle adds its regular FStyleType after ExtraData, so seed the
        // config property itself. The pack processor replaces it every frame.
        AgentConfig->StyleType.Index = MBSTPacking::Pack(
            static_cast<int32>(InitialState.VisualStyle),
            InitialState.CurrentYawDegrees,
            InitialState.CurrentPitchDegrees,
            InitialState.RecoilNormalized);
        AgentConfig->bShowExtraData = true;
        AgentConfig->bShowStyleType = true;
    }
    else
    {
        AgentConfig->StyleType.Index = static_cast<int32>(RestoredVisualStyle);
    }

    AgentConfig->PostEditChange();
    AgentConfig->MarkPackageDirty();

    OutMessage = bEnable
        ? TEXT("Embedded FMBSTSingleTurretTag, State and Shared layout in AgentConfig.ExtraData; normal SpawnByConfig now creates a turret entity directly.")
        : TEXT("Removed single-turret Tag, State and Shared layout from AgentConfig.ExtraData.");
    return true;
}

bool UMBSTSingleTurretEditorLibrary::ConfigureAgentConfigMobileFire(
    UMassBattleAgentConfigDataAsset* AgentConfig,
    UMBSTMobileFireProfile* Profile,
    const FMBSTMobileFireState& InitialState,
    const bool bEnable,
    FString& OutMessage)
{
    OutMessage.Reset();
    if (!IsValid(AgentConfig))
    {
        OutMessage = TEXT("AgentConfig is invalid.");
        return false;
    }
    if (bEnable && !IsValid(Profile))
    {
        OutMessage = TEXT("A valid MobileFireProfile is required when enabling attack-move.");
        return false;
    }

    int32 TurretStateCount = 0;
    const FMBSTSingleTurretState* ExistingTurretState =
        MBSTEditorPrivate::FindInstancedStruct<FMBSTSingleTurretState>(
            AgentConfig->ExtraData.Fragments,
            TurretStateCount);
    const TOptional<FMBSTSingleTurretState> ExistingTurretStateCopy = ExistingTurretState
        ? TOptional<FMBSTSingleTurretState>(*ExistingTurretState)
        : TOptional<FMBSTSingleTurretState>();
    if (bEnable && (TurretStateCount != 1 || !ExistingTurretState))
    {
        OutMessage = TEXT("Mobile fire requires the AgentConfig to contain exactly one single-turret state first.");
        return false;
    }

    AgentConfig->Modify();
    MBSTEditorPrivate::RemoveInstancedStructsOfType<FMBSTMobileFireTag>(AgentConfig->ExtraData.Tags);
    MBSTEditorPrivate::RemoveInstancedStructsOfType<FMBSTMobileFireState>(AgentConfig->ExtraData.Fragments);
    MBSTEditorPrivate::RemoveInstancedStructsOfType<FMBSTMobileFireShared>(AgentConfig->ExtraData.MutableSharedFragments);
    // Clean misplaced copies as well so the template contract remains unambiguous.
    MBSTEditorPrivate::RemoveInstancedStructsOfType<FMBSTMobileFireTag>(AgentConfig->ExtraData.Fragments);
    MBSTEditorPrivate::RemoveInstancedStructsOfType<FMBSTMobileFireState>(AgentConfig->ExtraData.MutableSharedFragments);
    MBSTEditorPrivate::RemoveInstancedStructsOfType<FMBSTMobileFireShared>(AgentConfig->ExtraData.ConstSharedFragments);

    if (ExistingTurretStateCopy.IsSet())
    {
        FMBSTSingleTurretState UpdatedTurretState = ExistingTurretStateCopy.GetValue();
        UpdatedTurretState.bExternalMotionDriver = bEnable;
        MBSTEditorPrivate::UpsertInstancedStruct(
            AgentConfig->ExtraData.Fragments,
            UpdatedTurretState);
    }

    if (bEnable)
    {
        FMBSTMobileFireState SanitizedState = InitialState;
        SanitizedState.Phase = SanitizedState.CooldownRemainingSeconds > 0.0f
            ? EMBSTMobileFirePhase::Cooling
            : EMBSTMobileFirePhase::Idle;
        SanitizedState.PhaseTimeSeconds = 0.0f;
        SanitizedState.CooldownRemainingSeconds = FMath::Max(
            SanitizedState.CooldownRemainingSeconds,
            0.0f);
        SanitizedState.CurrentTarget.Reset();
        SanitizedState.LastTargetWorldLocation = FVector::ZeroVector;
        SanitizedState.LastYawErrorDegrees = 0.0f;
        SanitizedState.LastPitchErrorDegrees = 0.0f;
        SanitizedState.ShotSequence = 0;
        SanitizedState.bMovementHoldRequested = false;
        SanitizedState.bMovementGateAppliedThisStep = false;
        SanitizedState.bSavedStopActiveMovement = false;

        MBSTEditorPrivate::UpsertInstancedStruct(
            AgentConfig->ExtraData.Tags,
            FMBSTMobileFireTag());
        MBSTEditorPrivate::UpsertInstancedStruct(
            AgentConfig->ExtraData.Fragments,
            SanitizedState);
        MBSTEditorPrivate::UpsertInstancedStruct(
            AgentConfig->ExtraData.MutableSharedFragments,
            Profile->BuildSharedFragment());

        if (Profile->bDisableBuiltInAttack)
        {
            AgentConfig->Attack.bEnable = false;
        }
        if (Profile->bDisableBuiltInChase)
        {
            AgentConfig->Chase.bEnable = false;
        }
        AgentConfig->bShowExtraData = true;
        AgentConfig->bShowAttack = true;
        AgentConfig->bShowChase = true;
    }

    AgentConfig->PostEditChange();
    AgentConfig->MarkPackageDirty();

    OutMessage = bEnable
        ? FString::Printf(
            TEXT("Embedded attack-move Tag/State/Shared profile '%s' in AgentConfig.ExtraData (policy: %s)."),
            *Profile->GetPathName(),
            *StaticEnum<EMBSTFireMobilityPolicy>()->GetDisplayNameTextByValue(
                static_cast<int64>(Profile->MobilityPolicy)).ToString())
        : TEXT("Removed the plugin-owned attack-move contract; the normal single-turret contract remains.");
    return true;
}

FMBSTAssetValidationResult UMBSTSingleTurretEditorLibrary::ValidateAgentConfigMobileFire(
    const UMassBattleAgentConfigDataAsset* AgentConfig,
    const UMBSTMobileFireProfile* ExpectedProfile)
{
    FMBSTAssetValidationResult Result;
    if (!IsValid(AgentConfig))
    {
        Result.Messages.Add(TEXT("AgentConfig is invalid."));
        return Result;
    }

    int32 TagCount = 0;
    int32 StateCount = 0;
    int32 SharedCount = 0;
    int32 TurretStateCount = 0;
    MBSTEditorPrivate::FindInstancedStruct<FMBSTMobileFireTag>(
        AgentConfig->ExtraData.Tags,
        TagCount);
    const FMBSTMobileFireState* State =
        MBSTEditorPrivate::FindInstancedStruct<FMBSTMobileFireState>(
            AgentConfig->ExtraData.Fragments,
            StateCount);
    const FMBSTMobileFireShared* Shared =
        MBSTEditorPrivate::FindInstancedStruct<FMBSTMobileFireShared>(
            AgentConfig->ExtraData.MutableSharedFragments,
            SharedCount);
    const FMBSTSingleTurretState* TurretState =
        MBSTEditorPrivate::FindInstancedStruct<FMBSTSingleTurretState>(
            AgentConfig->ExtraData.Fragments,
            TurretStateCount);

    if (TagCount != 1)
    {
        Result.Messages.Add(FString::Printf(
            TEXT("Expected exactly one FMBSTMobileFireTag; found %d."),
            TagCount));
    }
    if (StateCount != 1 || !State)
    {
        Result.Messages.Add(FString::Printf(
            TEXT("Expected exactly one FMBSTMobileFireState; found %d."),
            StateCount));
    }
    if (SharedCount != 1 || !Shared || !IsValid(Shared->Profile))
    {
        Result.Messages.Add(FString::Printf(
            TEXT("Expected exactly one mutable MobileFire shared profile; found %d."),
            SharedCount));
    }
    if (Shared && ExpectedProfile && Shared->Profile != ExpectedProfile)
    {
        Result.Messages.Add(FString::Printf(
            TEXT("Mobile-fire profile mismatch: expected '%s', found '%s'."),
            *ExpectedProfile->GetPathName(),
            Shared->Profile ? *Shared->Profile->GetPathName() : TEXT("None")));
    }
    if (TurretStateCount != 1 || !TurretState || !TurretState->bExternalMotionDriver)
    {
        Result.Messages.Add(TEXT("The single-turret state is not delegated to the mobile-fire motion driver."));
    }
    if (Shared && Shared->bSpawnMassBattleProjectile && !IsValid(Shared->ProjectileConfig))
    {
        Result.Messages.Add(TEXT("Projectile spawning is enabled, but ProjectileConfig is empty."));
    }
    if (Shared && IsValid(Shared->Profile))
    {
        if (Shared->Profile->bDisableBuiltInAttack && AgentConfig->Attack.bEnable)
        {
            Result.Messages.Add(TEXT("The profile requests built-in attack suppression, but AgentConfig.Attack is still enabled."));
        }
        if (Shared->Profile->bDisableBuiltInChase && AgentConfig->Chase.bEnable)
        {
            Result.Messages.Add(TEXT("The profile requests built-in chase suppression, but AgentConfig.Chase is still enabled."));
        }
    }

    Result.bValid = TagCount == 1
        && StateCount == 1
        && State != nullptr
        && SharedCount == 1
        && Shared != nullptr
        && IsValid(Shared->Profile)
        && (!ExpectedProfile || Shared->Profile == ExpectedProfile)
        && TurretStateCount == 1
        && TurretState != nullptr
        && TurretState->bExternalMotionDriver
        && (!Shared->bSpawnMassBattleProjectile || IsValid(Shared->ProjectileConfig))
        && (!Shared->Profile->bDisableBuiltInAttack || !AgentConfig->Attack.bEnable)
        && (!Shared->Profile->bDisableBuiltInChase || !AgentConfig->Chase.bEnable);
    if (Result.bValid)
    {
        Result.Messages.Add(FString::Printf(
            TEXT("AgentConfig has a valid per-unit attack-move contract using policy '%s'."),
            *StaticEnum<EMBSTFireMobilityPolicy>()->GetDisplayNameTextByValue(
                static_cast<int64>(Shared->MobilityPolicy)).ToString()));
    }
    return Result;
}

FMBSTAssetValidationResult UMBSTSingleTurretEditorLibrary::ValidateAgentConfigSingleTurret(
    const UMassBattleAgentConfigDataAsset* AgentConfig,
    const UMBSTSingleTurretAsset* ExpectedLayout)
{
    FMBSTAssetValidationResult Result;
    if (!IsValid(AgentConfig))
    {
        Result.Messages.Add(TEXT("AgentConfig is invalid."));
        return Result;
    }

    int32 TagCount = 0;
    int32 StateCount = 0;
    int32 SharedCount = 0;
    MBSTEditorPrivate::FindInstancedStruct<FMBSTSingleTurretTag>(AgentConfig->ExtraData.Tags, TagCount);
    const FMBSTSingleTurretState* State = MBSTEditorPrivate::FindInstancedStruct<FMBSTSingleTurretState>(
        AgentConfig->ExtraData.Fragments,
        StateCount);
    const FMBSTSingleTurretShared* Shared = MBSTEditorPrivate::FindInstancedStruct<FMBSTSingleTurretShared>(
        AgentConfig->ExtraData.MutableSharedFragments,
        SharedCount);

    if (TagCount != 1)
    {
        Result.Messages.Add(FString::Printf(TEXT("Expected exactly one FMBSTSingleTurretTag; found %d."), TagCount));
    }
    if (StateCount != 1 || !State)
    {
        Result.Messages.Add(FString::Printf(TEXT("Expected exactly one FMBSTSingleTurretState; found %d."), StateCount));
    }
    if (SharedCount != 1 || !Shared)
    {
        Result.Messages.Add(FString::Printf(TEXT("Expected exactly one mutable FMBSTSingleTurretShared; found %d."), SharedCount));
    }
    else
    {
        if (!IsValid(Shared->Layout))
        {
            Result.Messages.Add(TEXT("The shared turret fragment has no valid Layout."));
        }
        if (ExpectedLayout && Shared->Layout != ExpectedLayout)
        {
            Result.Messages.Add(FString::Printf(
                TEXT("Shared layout mismatch: expected '%s', found '%s'."),
                *ExpectedLayout->GetPathName(),
                Shared->Layout ? *Shared->Layout->GetPathName() : TEXT("None")));
        }
    }

    Result.bValid = TagCount == 1
        && StateCount == 1
        && State != nullptr
        && SharedCount == 1
        && Shared != nullptr
        && IsValid(Shared->Layout)
        && (!ExpectedLayout || Shared->Layout == ExpectedLayout);
    if (Result.bValid)
    {
        Result.Messages.Add(TEXT("AgentConfig is a directly spawnable single-entity turret unit (Tag + State Fragment + Shared Layout)."));
    }
    return Result;
}

UMassBattleAgentConfigDataAsset* UMBSTSingleTurretEditorLibrary::CreateSingleTurretAgentConfigFromTemplate(
    UMassBattleAgentConfigDataAsset* OptionalTemplate,
    UMBSTSingleTurretAsset* Layout,
    const FString& PackagePath,
    const FString& AssetName,
    const FMBSTSingleTurretState& InitialState,
    UMBSTMobileFireProfile* MobileFireProfile,
    const FMBSTMobileFireState& InitialMobileFireState,
    FString& OutMessage)
{
    OutMessage.Reset();
    if (!IsValid(Layout))
    {
        OutMessage = TEXT("Single-turret Layout is invalid.");
        return nullptr;
    }

    const FString SafePath = MBSTEditorPrivate::NormalizePackagePath(PackagePath);
    const FString SafeName = MBSTEditorPrivate::SanitizeAssetName(AssetName);
    const FString PackageName = MBSTEditorPrivate::MakePackageName(SafePath, SafeName);
    if (SafeName.IsEmpty() || !FPackageName::IsValidLongPackageName(PackageName))
    {
        OutMessage = FString::Printf(TEXT("Invalid output package/name: '%s' / '%s'."), *PackagePath, *AssetName);
        return nullptr;
    }

    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *SafeName);
    if (FindObject<UObject>(nullptr, *ObjectPath) || LoadObject<UObject>(nullptr, *ObjectPath))
    {
        OutMessage = FString::Printf(TEXT("AgentConfig already exists: %s"), *ObjectPath);
        return nullptr;
    }

    UPackage* Package = CreatePackage(*PackageName);
    UMassBattleAgentConfigDataAsset* Result = IsValid(OptionalTemplate)
        ? DuplicateObject<UMassBattleAgentConfigDataAsset>(OptionalTemplate, Package, *SafeName)
        : NewObject<UMassBattleAgentConfigDataAsset>(
            Package,
            *SafeName,
            RF_Public | RF_Standalone | RF_Transactional);
    if (!Result)
    {
        OutMessage = TEXT("Failed to create the AgentConfig.");
        return nullptr;
    }

    Result->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
    FAssetRegistryModule::AssetCreated(Result);

    FString ConfigureMessage;
    if (!ConfigureAgentConfigSingleTurret(Result, Layout, InitialState, true, ConfigureMessage))
    {
        Result->ClearFlags(RF_Public | RF_Standalone);
        OutMessage = ConfigureMessage;
        return nullptr;
    }

    FString MobileFireMessage;
    if (!ConfigureAgentConfigMobileFire(
        Result,
        MobileFireProfile,
        InitialMobileFireState,
        IsValid(MobileFireProfile),
        MobileFireMessage))
    {
        Result->ClearFlags(RF_Public | RF_Standalone);
        OutMessage = MobileFireMessage;
        return nullptr;
    }

    Result->MarkPackageDirty();
    OutMessage = FString::Printf(
        TEXT("Actor conversion created direct single-turret AgentConfig '%s'%s. %s %s"),
        *Result->GetPathName(),
        IsValid(OptionalTemplate) ? TEXT(" from the optional normal-unit template") : TEXT(" from MassBattle defaults"),
        *ConfigureMessage,
        *MobileFireMessage);
    return Result;
}

bool UMBSTSingleTurretEditorLibrary::ConfigureAndBakeAnimToTexture(
    UAnimToTextureDataAsset* DataAsset,
    USkeletalMeshComponent* DriverComponent,
    UStaticMesh* ArticulatedMesh,
    const bool bRunBake,
    FString& OutMessage)
{
    OutMessage.Reset();
    if (!IsValid(DataAsset)
        || !IsValid(DriverComponent)
        || !IsValid(DriverComponent->GetSkeletalMeshAsset())
        || !IsValid(ArticulatedMesh))
    {
        OutMessage = TEXT("ConfigureAndBakeAnimToTexture received an invalid data asset, driver, skeletal mesh or articulated mesh.");
        return false;
    }

    DataAsset->Modify();
    DataAsset->SkeletalMesh = DriverComponent->GetSkeletalMeshAsset();
    DataAsset->StaticMesh = ArticulatedMesh;

    DataAsset->PostEditChange();
    DataAsset->MarkPackageDirty();

    if (!bRunBake)
    {
        OutMessage = TEXT("AnimToTexture data asset configured. Confirm animations/mode/UV channel, then run the bake.");
        return true;
    }

    FString BakeMessage;
    const bool bBaked = MBSTEditorPrivate::InvokeAnimationToTexture(DataAsset, BakeMessage);
    OutMessage = BakeMessage;
    if (bBaked)
    {
        OutMessage += TEXT(" The articulation material must multiply VAT displacement by VertexColor.B.");
    }
    return bBaked;
}
