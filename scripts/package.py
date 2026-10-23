#!/usr/bin/env python3
"""Package source, optionally adding local binaries and their exact provenance."""
import argparse
import io
import json
from pathlib import Path
import tarfile
from binary_manifest import create_manifest

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--with-binaries', action='store_true')
parser.add_argument('--ue-root', type=Path, help='Required with --with-binaries to record the exact engine build')
args = parser.parse_args()
if args.with_binaries and not args.ue_root:
    parser.error('--with-binaries requires --ue-root')
root = Path(__file__).resolve().parent.parent
excluded = {'.git', 'build', 'Intermediate', 'Saved', 'DerivedDataCache', '__pycache__'}
if not args.with_binaries:
    excluded.add('Binaries')
files = []
for path in sorted(root.rglob('*')):
    parts = path.relative_to(root).parts
    if (not path.is_file() or path.is_symlink() or any(part in excluded for part in parts)
            or ('examples' in parts and 'Plugins' in parts) or 'Binaries' in parts[1:]
            or path.name == 'BINARY_MANIFEST.json' or path.name.endswith('.tar.gz')
            or path.resolve() == args.output.resolve()):
        continue
    files.append(path)
with tarfile.open(args.output, 'w:gz') as archive:
    for path in files:
        archive.add(path, arcname='DynamicTerrain/' + str(path.relative_to(root)), recursive=False)
    if args.with_binaries:
        sources = [path for path in files if 'Binaries' not in path.relative_to(root).parts]
        manifest = create_manifest(root, args.ue_root.resolve(), sources)
        data = (json.dumps(manifest, indent=2) + '\n').encode()
        entry = tarfile.TarInfo('DynamicTerrain/BINARY_MANIFEST.json')
        entry.size = len(data)
        archive.addfile(entry, io.BytesIO(data))
print(args.output)
