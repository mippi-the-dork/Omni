using UnrealBuildTool;

public class Omni : ModuleRules
{
    public Omni(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "NavigationSystem"
            }
        );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "AIModule"
            }
        );
    }
}
