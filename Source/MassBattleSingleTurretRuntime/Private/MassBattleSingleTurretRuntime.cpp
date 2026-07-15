#include "MassBattleSingleTurretRuntime.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

DEFINE_LOG_CATEGORY(LogMassBattleSingleTurret);

void FMassBattleSingleTurretRuntimeModule::StartupModule()
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MassBattleSingleTurret"));
    if (Plugin.IsValid())
    {
        const FString ShaderDirectory = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
        AddShaderSourceDirectoryMapping(TEXT("/Plugin/MassBattleSingleTurret"), ShaderDirectory);
    }
    else
    {
        UE_LOG(LogMassBattleSingleTurret, Warning,
            TEXT("Could not locate MassBattleSingleTurret; shader include mapping was not registered."));
    }
}

void FMassBattleSingleTurretRuntimeModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FMassBattleSingleTurretRuntimeModule, MassBattleSingleTurretRuntime)
