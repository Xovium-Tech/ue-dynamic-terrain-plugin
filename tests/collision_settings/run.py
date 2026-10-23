#!/usr/bin/env python3
"""Verify live terrain physics/material propagation in actual Chaos bodies.

Uses a separately built TerrainDemo project with the adjacent game mode. Detail
imagery is delayed so material edits happen between activation and refinement.
"""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
from pathlib import Path
import subprocess
import threading
import time
from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ue-root', type=Path, required=True)
    parser.add_argument('--project', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    results = []
    for inherited in (False, True):
        out = args.output.resolve() / ('inherited' if inherited else 'replacement')
        out.mkdir(parents=True, exist_ok=True)
        lock = threading.Lock()
        first_detail = [None]
        counts = {'elevation': 0, 'imagery': 0}

        class Handler(BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def log_message(self, *_):
                pass

            def do_GET(self):
                kind, zoom, x, y = self.path.strip('/').split('/')
                if kind == 'imagery' and int(zoom) == 17:
                    with lock:
                        if first_detail[0] is None:
                            first_detail[0] = time.monotonic()
                        delay = max(0, 4 - (time.monotonic() - first_detail[0]))
                    time.sleep(delay)
                image = Image.new('RGB', (256, 256), (129, 44, 0) if kind == 'elevation' else (31, 150, 211))
                data = io.BytesIO()
                image.save(data, 'PNG' if kind == 'elevation' else 'JPEG', quality=95)
                payload = data.getvalue()
                self.send_response(200)
                self.send_header('Content-Length', str(len(payload)))
                self.end_headers()
                try:
                    self.wfile.write(payload)
                except (BrokenPipeError, ConnectionResetError):
                    return
                with lock:
                    counts[kind] += 1

        class Server(ThreadingHTTPServer):
            daemon_threads = True

        server = Server(('127.0.0.1', 0), Handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        base = f'http://127.0.0.1:{server.server_port}'
        config = {'schema': 1, 'origin': {'latitude': 35.7162542, 'longitude': -120.7615102, 'altitude': 300},
                  'zoom': 16, 'radius': 1, 'cache': str(out / 'cache'),
                  'imagery_url': base + '/imagery/{z}/{x}/{y}.jpg',
                  'elevation_url': base + '/elevation/{z}/{x}/{y}.png'}
        (out / 'terrain.json').write_text(json.dumps(config, indent=2) + '\n')
        command = [str(args.ue_root.resolve() / 'Engine/Binaries/Linux/UnrealEditor'), str(args.project.resolve()),
                   '/Engine/Maps/Entry?game=/Script/TerrainDemo.TerrainDemoGameMode', '-game', '-nullrhi',
                   '-unattended', '-nop4', '-nosound', f'-TerrainConfig={out}/terrain.json', f'-abslog={out}/unreal.log']
        if inherited:
            command.append('-InheritedPhysicalMaterial')
        try:
            with (out / 'stdout.log').open('w') as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=100)
        finally:
            server.shutdown()
            server.server_close()
            worker.join()
        log = (out / 'unreal.log').read_text(errors='replace')
        marker = next((line for line in log.splitlines() if 'TERRAIN_SETTINGS passed=' in line), '')
        passed = result.returncode == 0 and all(v in marker for v in ('passed=1', 'pending_settings=1', 'without_recenter=1'))
        results.append({'case': out.name, 'passed': passed, 'returncode': result.returncode, 'marker': marker, 'requests': counts})
        print(json.dumps(results[-1]), flush=True)
    args.output.resolve().joinpath('summary.json').write_text(json.dumps(results, indent=2) + '\n')
    if not all(item['passed'] for item in results):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
