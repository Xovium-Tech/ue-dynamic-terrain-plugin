#!/usr/bin/env python3
"""Create an isolated game project for Stop/EndPlay during actual Chaos cooks."""
import argparse
from pathlib import Path
import shutil

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
output = args.output.resolve()
if output.exists():
    parser.error('output must not exist')
output.mkdir(parents=True)
sample = root / 'examples/TerrainDemo'
for name in ('Source', 'Config'):
    shutil.copytree(sample / name, output / name)
shutil.copy2(sample / 'TerrainDemo.uproject', output / 'TerrainDemo.uproject')
plugin = output / 'Plugins/DynamicTerrain'
shutil.copytree(sample / 'Plugins/DynamicTerrain', plugin,
                ignore=shutil.ignore_patterns('Intermediate', 'Saved', 'DerivedDataCache', '__pycache__'))
for path in (root / 'tests/lifecycle').glob('*'):
    shutil.copy2(path, output / 'Source/TerrainDemo' / path.name)
build = output / 'Source/TerrainDemo/TerrainDemo.Build.cs'
build.write_text(build.read_text().replace('"DynamicTerrain"', '"DynamicTerrain", "ProceduralMeshComponent", "PhysicsCore"'))
print(output / 'TerrainDemo.uproject')
