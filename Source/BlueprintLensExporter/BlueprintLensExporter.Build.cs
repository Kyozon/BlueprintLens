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
            "AssetTools",
            "BlueprintEditorLibrary",
            "BlueprintGraph",
            "GameFeatures",
            "Json",
            "JsonUtilities",
            "KismetCompiler",
            "UMG",
            "UMGEditor",
            "UnrealEd"
        });
    }
}
