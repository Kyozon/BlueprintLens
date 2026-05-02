using UnrealBuildTool;

public class BlueprintLensExporter : ModuleRules
{
    public BlueprintLensExporter(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "AssetRegistry",
            "BlueprintEditorLibrary",
            "BlueprintGraph",
            "Json",
            "JsonUtilities",
            "KismetCompiler",
            "UnrealEd"
        });
    }
}
