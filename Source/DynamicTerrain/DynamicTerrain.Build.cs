using UnrealBuildTool;
using System.IO;
public class DynamicTerrain : ModuleRules {
    public DynamicTerrain(ReadOnlyTargetRules Target) : base(Target) {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp20;
        bUseUnity = false;
        PublicDependencyModuleNames.AddRange(new[] {"Core", "CoreUObject", "Engine", "PhysicsCore", "ProceduralMeshComponent"});
        PrivateDependencyModuleNames.AddRange(new[] {"Json", "RenderCore", "RHI", "ImageCore", "Projects"});
        if (Target.Platform != UnrealTargetPlatform.Linux) throw new BuildException("DynamicTerrain currently supports Linux x86_64.");
        string Root = Path.GetFullPath(Path.Combine(ModuleDirectory, "../.."));
        PublicIncludePaths.Add(Path.Combine(Root, "native/include"));
        string Native = Path.Combine(Root, "Binaries/Linux/libxovium_dynamic_terrain.so");
        PublicAdditionalLibraries.Add(Native);
        RuntimeDependencies.Add(Native);
    }
}
