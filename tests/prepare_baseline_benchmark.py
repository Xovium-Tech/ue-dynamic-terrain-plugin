#!/usr/bin/env python3
"""Prepare the same demo with archived APX4Terrain sources for before/after tests.

Only module export names and read-only counters/timing instrumentation change.
The archived terrain algorithm, materials and native binary remain unchanged.
"""
import argparse
import json
from pathlib import Path
import shutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets', required=True, type=Path, help='Archived backend/assets/unreal directory')
    parser.add_argument('--native-library', required=True, type=Path)
    parser.add_argument('--material', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    sample = root / 'examples/TerrainDemo'
    output = args.output.resolve()
    if output.exists():
        parser.error('output must not already exist')
    output.mkdir(parents=True)
    for name in ('Source', 'Config'):
        shutil.copytree(sample / name, output / name)
    project = json.loads((sample / 'TerrainDemo.uproject').read_text())
    project['Plugins'] = [{'Name': 'ProceduralMeshComponent', 'Enabled': True},
                          {'Name': 'PythonScriptPlugin', 'Enabled': True}]
    (output / 'TerrainDemo.uproject').write_text(json.dumps(project, indent=2) + '\n')
    module = output / 'Source/TerrainDemo'
    header = (args.assets / 'Unreal/Source/PX4Unreal/Public/PX4Terrain.h').read_text()
    header = header.replace('PX4UNREAL_API', 'TERRAINDEMO_API')
    header = header.replace('    APX4Terrain();', '''    APX4Terrain();
    int64 ActiveGeneration=0; int32 CollisionCooks=0; double LastCommitMilliseconds=0;
    bool IsTerrainReady() const {FHitResult Hit;return TraceGround(FollowActor?FollowActor->GetActorLocation():GetActorLocation(),Hit);}''')
    (module / 'PX4Terrain.h').write_text(header)
    source = (args.assets / 'Unreal/Source/PX4Unreal/Private/PX4Terrain.cpp').read_text()
    source = source.replace('    xov_terrain_poll(Runtime,', '''    const double PollStart=FPlatformTime::Seconds();
    const int64 PreviousGeneration=ActiveGeneration;const int32 PreviousPages=LoadedPages;const int32 PreviousUpdates=TextureUpdates;
    xov_terrain_poll(Runtime,''')
    source = source.replace('static_cast<APX4Terrain*>(User)->ApplyPage(*Page);},this);', '''static_cast<APX4Terrain*>(User)->ApplyPage(*Page);},this);
    if(PreviousGeneration!=ActiveGeneration||PreviousPages!=LoadedPages||PreviousUpdates!=TextureUpdates)
        LastCommitMilliseconds=(FPlatformTime::Seconds()-PollStart)*1000;''')
    source = source.replace('    if(P.reset) {', '    if(P.reset) {++ActiveGeneration;')
    source = source.replace('        ++LoadedPages;', '        ++LoadedPages; ++CollisionCooks;')
    (module / 'PX4Terrain.cpp').write_text(source)
    for name in ('TerrainDemoGameMode.cpp', 'TerrainDemoGameMode.h'):
        source = (module / name).read_text().replace('DynamicTerrainActor.h', 'PX4Terrain.h').replace('ADynamicTerrainActor', 'APX4Terrain')
        source = '\n'.join(line for line in source.splitlines()
                           if 'const auto& D=Terrain->Diagnostics;' not in line and 'TERRAIN_DIAGNOSTICS' not in line) + '\n'
        (module / name).write_text(source)
    (module / 'TerrainDemo.Build.cs').write_text('''using UnrealBuildTool;
using System.IO;
public class TerrainDemo : ModuleRules {
 public TerrainDemo(ReadOnlyTargetRules Target) : base(Target) {
  PCHUsage=PCHUsageMode.UseExplicitOrSharedPCHs;CppStandard=CppStandardVersion.Cpp20;bUseUnity=false;
  PublicDependencyModuleNames.AddRange(new[]{"Core","CoreUObject","Engine","PhysicsCore","ProceduralMeshComponent","Json","RenderCore","RHI","ImageCore"});
  string Root=Path.GetFullPath(Path.Combine(ModuleDirectory,"../.."));
  PublicIncludePaths.Add(Path.Combine(Root,"native/include"));
  PublicAdditionalLibraries.Add(Path.Combine(Root,"Binaries/Linux/libxovium_terrain.so"));
  RuntimeDependencies.Add(Path.Combine(Root,"Binaries/Linux/libxovium_terrain.so"));
 }
}
''')
    shutil.copytree(args.assets / 'native/include', output / 'native/include')
    (output / 'Binaries/Linux').mkdir(parents=True)
    shutil.copy2(args.native_library, output / 'Binaries/Linux/libxovium_terrain.so')
    (output / 'Content/Worlds').mkdir(parents=True)
    shutil.copy2(args.material, output / 'Content/Worlds/M_Terrain.uasset')
    print(output / 'TerrainDemo.uproject')


if __name__ == '__main__':
    main()
