using UnrealBuildTool;

public class MassBattleSingleTurretRuntime : ModuleRules
{
    public MassBattleSingleTurretRuntime(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = false;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "MassEntity",
            "MassCommon",
            "MassAPI",
            "MassBattle",
            "FlowFieldCanvas",
            "Niagara",
            "AnimToTexture"
        });

        if (Target.Version.MajorVersion >= 5 && Target.Version.MinorVersion >= 8)
        {
            PublicDependencyModuleNames.Add("MassCore");
        }

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Json",
            "Projects",
            "RenderCore",
            "RHI"
        });
    }
}
