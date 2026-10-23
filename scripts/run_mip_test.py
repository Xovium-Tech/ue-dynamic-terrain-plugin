#!/usr/bin/env python3
"""Compare every prepared mip with the original algorithm inside Unreal."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ue-root', required=True, type=Path)
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    output = (args.output_dir or root / 'build/mip-parity').resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = output / 'index.json'
    report.unlink(missing_ok=True)
    log = output / 'unreal.log'
    command = [str(args.ue_root.resolve() / 'Engine/Binaries/Linux/UnrealEditor-Cmd'),
               str(root / 'examples/TerrainDemo/TerrainDemo.uproject'),
               '-unattended', '-nop4', '-nosound', '-nullrhi', '-nowrite', '-noini',
               '-ExecCmds=Automation RunTests Xovium.DynamicTerrain.MipParity',
               '-TestExit=Automation Test Queue Empty',
               f'-ReportExportPath={output}', f'-abslog={log}']
    with (output / 'stdout.log').open('w') as stdout:
        process = subprocess.run(command, stdout=stdout, stderr=subprocess.STDOUT, timeout=240)
    # UE 5.6's TestExit requests forced exit(1), including for a passing suite.
    # Require a fresh report and the explicit normal TestExit log marker.
    data = json.loads(report.read_text(encoding='utf-8-sig')) if report.exists() else {}
    passed = (process.returncode in (0, 1) and data.get('succeeded') == 1
              and data.get('failed') == 0 and data.get('notRun') == 0
              and log.exists() and 'TestExit: Automation Test Queue Empty' in log.read_text(errors='replace'))
    print(json.dumps({'passed': passed, 'report': str(report), 'engine_returncode': process.returncode}))
    raise SystemExit(0 if passed else 1)


if __name__ == '__main__':
    main()
