using UnrealBuildTool;

public class MassBattleSingleTurretEditor : ModuleRules
{
    public MassBattleSingleTurretEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = false;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "MassBattleSingleTurretRuntime",
            "MassBattle",
            "Niagara",
            "AnimToTexture"
        });

        if (Target.Version.MajorVersion >= 5 && Target.Version.MinorVersion >= 8)
        {
            PublicDependencyModuleNames.Add("MassCore");
        }

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "UnrealEd",
            "AssetRegistry",
            "MeshUtilities",
            "MeshDescription",
            "StaticMeshDescription",
            "NiagaraCore",
            "AnimToTextureEditor",
            "MassAPI"
        });
    }
}
