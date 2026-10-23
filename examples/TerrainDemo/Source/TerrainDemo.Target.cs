using UnrealBuildTool;
public class TerrainDemoTarget : TargetRules {
 public TerrainDemoTarget(TargetInfo Target) : base(Target) {
  Type=TargetType.Game;DefaultBuildSettings=BuildSettingsVersion.V5;
  IncludeOrderVersion=EngineIncludeOrderVersion.Unreal5_6;ExtraModuleNames.Add("TerrainDemo");
 }
}
