#!/usr/bin/env python3
"""Isolated two-camera, high-altitude terrain regression project."""
import argparse
from pathlib import Path
import shutil
p=argparse.ArgumentParser();p.add_argument('output',type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[2];out=a.output.resolve()
if out.exists():p.error('output already exists')
out.mkdir(parents=True)
for name in ('Source','Config'):
    shutil.copytree(root/'examples/TerrainDemo'/name,out/name)
shutil.copy2(root/'examples/TerrainDemo/TerrainDemo.uproject',out/'TerrainDemo.uproject')
plugin=out/'Plugins/DynamicTerrain';plugin.mkdir(parents=True)
for name in ('Source','native','Content','Binaries'):
    shutil.copytree(root/name,plugin/name,ignore=shutil.ignore_patterns('Intermediate','__pycache__'))
shutil.copy2(root/'DynamicTerrain.uplugin',plugin/'DynamicTerrain.uplugin')
for path in (root/'tests/coverage').glob('*.h'):shutil.copy2(path,out/'Source/TerrainDemo'/path.name)
for path in (root/'tests/coverage').glob('*.cpp'):shutil.copy2(path,out/'Source/TerrainDemo'/path.name)
build=out/'Source/TerrainDemo/TerrainDemo.Build.cs'
build.write_text(build.read_text().replace('"DynamicTerrain"','"DynamicTerrain", "ProceduralMeshComponent", "RenderCore", "RHI", "ImageWrapper"'))
print(out/'TerrainDemo.uproject')
