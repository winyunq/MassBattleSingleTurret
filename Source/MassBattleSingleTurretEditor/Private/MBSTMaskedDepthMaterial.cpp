#include "MBSTMaskedDepthMaterial.h"

#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionGetMaterialAttributes.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "Misc/ScopeExit.h"
#include "RHI.h"
#include "UObject/Package.h"

namespace MBSTEditorPrivate
{
    namespace
    {
        constexpr TCHAR MaskMarker[] = TEXT("MBST masked depth: preserve final opacity mask");
        constexpr TCHAR AttributesMarker[] = TEXT("MBST masked depth: final material attributes");

        bool IsMaskWrapper(const FExpressionInput& Input)
        {
            const UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Input.Expression);
            return Custom && Input.OutputIndex == 0 && Custom->Desc == MaskMarker
                && Custom->Code == TEXT("return Mask;") && Custom->OutputType == CMOT_Float1
                && Custom->Inputs.Num() == 1 && Custom->Inputs[0].InputName == TEXT("Mask")
                && Custom->Inputs[0].Input.IsConnected()
                && Custom->AdditionalOutputs.IsEmpty()
                && Custom->AdditionalDefines.IsEmpty() && Custom->IncludeFilePaths.IsEmpty()
                && Custom->ContainsClipInstruction == CMCI_No;
        }

        bool SameConnection(const FExpressionInput& A, const FExpressionInput& B)
        {
            return A.Expression == B.Expression && A.OutputIndex == B.OutputIndex
                && A.Mask == B.Mask && A.MaskR == B.MaskR && A.MaskG == B.MaskG
                && A.MaskB == B.MaskB && A.MaskA == B.MaskA;
        }

        bool HasFinalMaskWrapper(const UMaterial* Material, const UMaterialEditorOnlyData* Data)
        {
            if (!Material->bUseMaterialAttributes)
            {
                return !Data->OpacityMask.UseConstant && IsMaskWrapper(Data->OpacityMask);
            }

            const UMaterialExpressionSetMaterialAttributes* Set =
                Cast<UMaterialExpressionSetMaterialAttributes>(Data->MaterialAttributes.Expression);
            const FGuid MaskId = FMaterialAttributeDefinitionMap::GetID(MP_OpacityMask);
            if (!Set || Data->MaterialAttributes.OutputIndex != 0 || Set->Desc != AttributesMarker
                || Set->AttributeSetTypes.Num() != 1 || Set->AttributeSetTypes[0] != MaskId
                || Set->Inputs.Num() != 2 || !IsMaskWrapper(Set->Inputs[1]))
            {
                return false;
            }

            const UMaterialExpressionCustom* Custom = CastChecked<UMaterialExpressionCustom>(Set->Inputs[1].Expression);
            const FExpressionInput& MaskInput = Custom->Inputs[0].Input;
            const UMaterialExpressionGetMaterialAttributes* Get =
                Cast<UMaterialExpressionGetMaterialAttributes>(MaskInput.Expression);
            return Get && Get->AttributeGetTypes.Num() == 1 && Get->AttributeGetTypes[0] == MaskId
                && MaskInput.OutputIndex == 1 && SameConnection(Get->MaterialAttributes, Set->Inputs[0]);
        }

        TArray<FString> CompileMaterial(UMaterial* Material)
        {
            TArray<FString> Errors = UMaterialEditingLibrary::RecompileMaterial(Material);
            if (FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform))
            {
                Resource->FinishCompilation();
                for (const FString& Error : Resource->GetCompileErrors())
                {
                    Errors.AddUnique(Error);
                }
                if (!Resource->GetGameThreadShaderMap())
                {
                    Errors.Add(TEXT("Material compilation did not produce a shader map."));
                }
            }
            else
            {
                Errors.Add(TEXT("No material resource was available after recompilation."));
            }
            return Errors;
        }
    }

    bool EnsureMaskedDepthMaterial(UMaterial* Material, FString& OutMessage)
    {
        OutMessage.Reset();
        if (!IsValid(Material))
        {
            OutMessage = TEXT("Material is invalid.");
            return false;
        }
        if (Material->MaterialDomain != MD_Surface)
        {
            OutMessage = TEXT("Masked depth compatibility only applies to surface materials.");
            return true;
        }

        UMaterialEditorOnlyData* Data = Material->GetEditorOnlyData();
        if (!Data)
        {
            OutMessage = TEXT("Material editor data is unavailable.");
            return false;
        }
        if (HasFinalMaskWrapper(Material, Data))
        {
            OutMessage = TEXT("Final opacity mask already preserves masked depth shader permutations.");
            return true;
        }

        const FScalarMaterialInput OriginalMask = Data->OpacityMask;
        const FMaterialAttributesInput OriginalAttributes = Data->MaterialAttributes;
        const bool bWasDirty = Material->GetOutermost()->IsDirty();
        TArray<UMaterialExpression*> Created;
        bool bInstalled = false;
        Material->Modify();
        Data->Modify();
        ON_SCOPE_EXIT
        {
            if (!bInstalled)
            {
                Data->OpacityMask = OriginalMask;
                Data->MaterialAttributes = OriginalAttributes;
                for (UMaterialExpression* Expression : Created)
                {
                    UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
                }
                CompileMaterial(Material);
                Material->GetOutermost()->SetDirtyFlag(bWasDirty);
            }
        };

        const auto CreateExpression = [Material, &Created](UClass* Class, int32 X, int32 Y)
        {
            UMaterialExpression* Expression = UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, Y);
            if (Expression)
            {
                Created.Add(Expression);
            }
            return Expression;
        };

        UMaterialExpressionCustom* Wrapper = Cast<UMaterialExpressionCustom>(
            CreateExpression(UMaterialExpressionCustom::StaticClass(), -250, 650));
        if (!Wrapper)
        {
            OutMessage = TEXT("Could not create the final opacity-mask expression.");
            return false;
        }
        // UE 5.8 can omit DepthOnlyPS for a mask folded to its default value of 1,
        // while the runtime still requests it for Masked blend mode. A Custom
        // expression prevents that material-translation fold without changing the
        // mask value. It must wrap the final output, after every static switch.
        // Install on all surface masters: an instance can override the blend mode.
        Wrapper->Code = TEXT("return Mask;");
        Wrapper->OutputType = CMOT_Float1;
        Wrapper->Desc = MaskMarker;
        Wrapper->Description = TEXT("Preserve opacity mask values and masked WPO depth rendering");
        Wrapper->ContainsClipInstruction = CMCI_No;
        Wrapper->Inputs.Reset();
        FCustomInput& MaskInput = Wrapper->Inputs.AddDefaulted_GetRef();
        MaskInput.InputName = TEXT("Mask");

        if (Material->bUseMaterialAttributes)
        {
            UMaterialExpressionGetMaterialAttributes* Get = Cast<UMaterialExpressionGetMaterialAttributes>(
                CreateExpression(UMaterialExpressionGetMaterialAttributes::StaticClass(), -500, 650));
            UMaterialExpressionSetMaterialAttributes* Set = Cast<UMaterialExpressionSetMaterialAttributes>(
                CreateExpression(UMaterialExpressionSetMaterialAttributes::StaticClass(), 0, 650));
            if (!Get || !Set)
            {
                OutMessage = TEXT("Could not create the final material-attributes mask connections.");
                return false;
            }
            Get->MaterialAttributes = OriginalAttributes;
            const int32 MaskOutput = Get->CreateOrGetOutputAttribute(MP_OpacityMask);
            const int32 AttributesInput = Set->CreateOrGetInputAttribute(MP_MaterialAttributes);
            if (MaskOutput == INDEX_NONE || !Set->Inputs.IsValidIndex(AttributesInput))
            {
                OutMessage = TEXT("Could not resolve material-attributes mask pins.");
                return false;
            }
            Set->Desc = AttributesMarker;
            Set->Inputs[AttributesInput] = OriginalAttributes;
            MaskInput.Input.Connect(MaskOutput, Get);
            if (!Set->ConnectInputAttribute(MP_OpacityMask, Wrapper))
            {
                OutMessage = TEXT("Could not connect the final opacity mask to material attributes.");
                return false;
            }
            Data->MaterialAttributes.Connect(0, Set);
        }
        else
        {
            // Inline constants take precedence over dormant graph connections.
            if (OriginalMask.UseConstant || !OriginalMask.Expression)
            {
                UMaterialExpressionConstant* Constant = Cast<UMaterialExpressionConstant>(
                    CreateExpression(UMaterialExpressionConstant::StaticClass(), -500, 650));
                if (!Constant)
                {
                    OutMessage = TEXT("Could not preserve the effective opacity-mask constant.");
                    return false;
                }
                Constant->R = OriginalMask.UseConstant ? OriginalMask.Constant : 1.0f;
                MaskInput.Input.Connect(0, Constant);
            }
            else
            {
                MaskInput.Input = OriginalMask;
            }
            Data->OpacityMask.Connect(0, Wrapper);
            Data->OpacityMask.UseConstant = false;
        }
        Wrapper->RebuildOutputs();

        const TArray<FString> Errors = CompileMaterial(Material);
        if (!Errors.IsEmpty())
        {
            OutMessage = FString::Printf(TEXT("Final opacity-mask compatibility expression did not compile: %s"),
                *FString::Join(Errors, TEXT(" | ")));
            return false;
        }
        bInstalled = true;
        Material->MarkPackageDirty();
        OutMessage = TEXT("Preserved the final opacity mask and retained masked depth shader permutations.");
        return true;
    }
}
