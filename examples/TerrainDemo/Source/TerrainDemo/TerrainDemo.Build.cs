using UnrealBuildTool;
public class TerrainDemo : ModuleRules {
 public TerrainDemo(ReadOnlyTargetRules Target) : base(Target) {
  PCHUsage=PCHUsageMode.UseExplicitOrSharedPCHs;
  PublicDependencyModuleNames.AddRange(new[]{"Core","CoreUObject","Engine","DynamicTerrain"});
 }
}
