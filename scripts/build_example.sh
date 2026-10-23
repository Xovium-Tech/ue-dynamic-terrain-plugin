#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
engine="${1:?Usage: build_example.sh UE_ROOT}"
project="$root/examples/TerrainDemo/TerrainDemo.uproject"
bash "$root/scripts/build_native.sh"
python3 "$root/scripts/prepare_example.py"
bash "$engine/Engine/Build/BatchFiles/Linux/Build.sh" TerrainDemoEditor Linux Development "$project" \
 -WaitMutex -NoHotReloadFromIDE -NoUBA "-MaxParallelActions=${JOBS:-4}" "-Log=$root/build/UnrealBuildTool.log"
for binary in libUnrealEditor-DynamicTerrain.so libUnrealEditor-DynamicTerrain.debug libUnrealEditor-DynamicTerrain.sym UnrealEditor.modules; do
 cp "$root/examples/TerrainDemo/Plugins/DynamicTerrain/Binaries/Linux/$binary" "$root/Binaries/Linux/$binary"
done
if [[ ! -f "$root/Content/M_Terrain.uasset" ]]; then
"$engine/Engine/Binaries/Linux/UnrealEditor-Cmd" "$project" -run=pythonscript \
 "-script=$root/scripts/create_material.py" -unattended -nop4 -nosound -nullrhi \
 "-abslog=$root/build/material.log"
cp "$root/examples/TerrainDemo/Plugins/DynamicTerrain/Content/M_Terrain.uasset" "$root/Content/M_Terrain.uasset"
fi
