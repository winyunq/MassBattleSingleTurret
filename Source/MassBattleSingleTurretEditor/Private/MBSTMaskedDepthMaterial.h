#pragma once

#include "CoreMinimal.h"

class UMaterial;

namespace MBSTEditorPrivate
{
    // Preserve the effective mask while retaining masked depth shader permutations.
    bool EnsureMaskedDepthMaterial(UMaterial* Material, FString& OutMessage);
}
