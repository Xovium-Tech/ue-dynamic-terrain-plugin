#!/usr/bin/env python3
"""Prepare an isolated project for live collision/material regression checks."""
import argparse
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
output = args.output.resolve()
if output.exists():
    parser.error('output must not exist')
output.mkdir(parents=True)
sample = root / 'examples/TerrainDemo'
for name in ('Source', 'Config'):
    shutil.copytree(sample / name, output / name)
shutil.copy2(sample / 'TerrainDemo.uproject', output / 'TerrainDemo.uproject')
subprocess.run(['python3', str(root / 'scripts/install.py'), str(output / 'TerrainDemo.uproject')], check=True)
for name in ('TerrainDemoGameMode.cpp', 'TerrainDemoGameMode.h'):
    shutil.copy2(Path(__file__).parent / name, output / 'Source/TerrainDemo' / name)
build = output / 'Source/TerrainDemo/TerrainDemo.Build.cs'
build.write_text(build.read_text().replace('"DynamicTerrain"', '"DynamicTerrain", "ProceduralMeshComponent", "PhysicsCore"'))
print(output / 'TerrainDemo.uproject')
