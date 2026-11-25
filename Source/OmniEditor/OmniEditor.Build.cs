using UnrealBuildTool;

public class OmniEditor : ModuleRules
{
    public OmniEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "Omni",
                "NavigationSystem",
                "AIModule",
                "GameplayTasks",
                "UnrealEd",
                "ToolMenus",
                "Slate",
                "SlateCore"
            }
        );
    }
}
