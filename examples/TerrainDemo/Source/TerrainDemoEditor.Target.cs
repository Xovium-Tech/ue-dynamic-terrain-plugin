using UnrealBuildTool;
public class TerrainDemoEditorTarget : TargetRules {
 public TerrainDemoEditorTarget(TargetInfo Target) : base(Target) {
  Type=TargetType.Editor;DefaultBuildSettings=BuildSettingsVersion.V5;
  IncludeOrderVersion=EngineIncludeOrderVersion.Unreal5_6;ExtraModuleNames.Add("TerrainDemo");
 }
}
