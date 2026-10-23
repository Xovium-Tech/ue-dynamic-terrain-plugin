#!/usr/bin/env python3
"""Record the exact engine, sources and dynamic dependencies of local binaries."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def create_manifest(root, engine, sources):
    build_version = json.loads((engine / 'Engine/Build/Build.version').read_text())
    binaries = root / 'Binaries/Linux'
    modules = json.loads((binaries / 'UnrealEditor.modules').read_text())
    libraries = {}
    for name in ('libUnrealEditor-DynamicTerrain.so', 'libxovium_dynamic_terrain.so'):
        library = binaries / name
        dynamic = subprocess.check_output(['readelf', '-d', str(library)], text=True)
        libraries[name] = {
            'sha256': sha256(library),
            'needed': re.findall(r'\(NEEDED\).*\[(.*?)\]', dynamic),
        }
    native = binaries / 'libxovium_dynamic_terrain.so'
    native_dependencies = subprocess.check_output(['ldd', str(native)], text=True)
    if 'not found' in native_dependencies:
        raise RuntimeError('The local native library has unresolved system dependencies')
    return {
        'schema': 1,
        'platform': platform.system(),
        'architecture': platform.machine(),
        'engine_version': build_version,
        'engine_build_id': modules['BuildId'],
        'core_source_manifest_sha256': sha256(root / 'source-manifest.json'),
        'adapter_source_provenance': json.loads((root / 'adapter-source-manifest.json').read_text()),
        'source_sha256': {str(path.relative_to(root)): sha256(path) for path in sources},
        'binary_sha256': {str(path.relative_to(root)): sha256(path)
                          for path in sorted(binaries.iterdir()) if path.is_file()},
        'libraries': libraries,
        'opencv_abi': [name for name in libraries['libxovium_dynamic_terrain.so']['needed'] if name.startswith('libopencv')],
        'native_resolved_dependencies': native_dependencies.splitlines(),
        'requirements': 'Locally built Linux binaries require this engine BuildId and the listed system library ABIs. Rebuild from source when engine or dependencies differ; OpenCV 4.6 and 4.9 are not ABI-compatible.',
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ue-root', required=True, type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    excluded = {'.git', 'build', 'Binaries', 'Intermediate', 'Saved', 'DerivedDataCache', '__pycache__'}
    sources = [path for path in sorted(root.rglob('*'))
               if path.is_file() and not path.is_symlink()
               and not any(part in excluded for part in path.relative_to(root).parts)
               and not ('examples' in path.relative_to(root).parts and 'Plugins' in path.relative_to(root).parts)
               and path.name != 'BINARY_MANIFEST.json' and not path.name.endswith('.tar.gz')]
    output = args.output or root / 'BINARY_MANIFEST.json'
    output.write_text(json.dumps(create_manifest(root, args.ue_root.resolve(), sources), indent=2) + '\n')
    print(output)


if __name__ == '__main__':
    main()
