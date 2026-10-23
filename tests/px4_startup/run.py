#!/usr/bin/env python3
"""Run real PX4GameMode with delayed local tiles and a silent rover TCP peer.

The project must be an isolated, already built Xovium integration copy. This
adds only the fixture subclass to that copy; it does not edit production code.
"""
import argparse
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
from pathlib import Path
import re
import shutil
import socket
import subprocess
import threading
import time
from PIL import Image

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--ue-root', type=Path, required=True)
p.add_argument('--project', type=Path, required=True)
p.add_argument('--output-dir', type=Path, required=True)
p.add_argument('--unavailable', action='store_true', help='Exercise the actual 120-second production deadline with HTTP 503 responses')
a = p.parse_args()
engine, project, out = a.ue_root.resolve(), a.project.resolve(), a.output_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
source = project.parent / 'Source/PX4Unreal'
fixture = Path(__file__).resolve().parent
for extension, subdir in [('h', 'Public'), ('cpp', 'Private')]:
    shutil.copy2(fixture / f'PX4TerrainStartupTestMode.{extension}', source / subdir)
with (out / 'build.log').open('w') as log:
    subprocess.run(['bash', str(engine / 'Engine/Build/BatchFiles/Linux/Build.sh'), 'PX4UnrealEditor', 'Linux', 'Development', str(project),
                    '-WaitMutex', '-NoHotReloadFromIDE', '-NoUBA', '-MaxParallelActions=4', f'-Log={out}/build-ubt.log'],
                   stdout=log, stderr=subprocess.STDOUT, check=True)
data = {}
for kind, color, format in [('elevation', (129, 44, 0), 'PNG'), ('imagery', (80, 140, 60), 'JPEG')]:
    image = Image.new('RGB', (256, 256), color)
    buffer = io.BytesIO(); image.save(buffer, format); data[kind] = buffer.getvalue()
first_request = [None]
requests = [0]
lock = threading.Lock()

class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *args):
        pass
    def do_GET(self):
        with lock:
            if first_request[0] is None:
                first_request[0] = time.monotonic()
            remaining = first_request[0] + 8 - time.monotonic()
            requests[0] += 1
        if a.unavailable:
            self.send_response(503); self.send_header('Content-Length', '0'); self.send_header('Retry-After', '1'); self.end_headers(); return
        if remaining > 0:
            time.sleep(remaining)
        kind = self.path.split('/')[1]
        body = data.get(kind)
        if body is None:
            self.send_error(404); return
        self.send_response(200); self.send_header('Content-Length', str(len(body))); self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
server.daemon_threads = True
threading.Thread(target=server.serve_forever, daemon=True).start()
with socket.socket() as probe:
    probe.bind(('127.0.0.1', 0)); rover_port = probe.getsockname()[1]
stop = threading.Event()
connected = [False]
def rover_peer():
    while not stop.is_set():
        try:
            connection = socket.create_connection(('127.0.0.1', rover_port), timeout=.2)
        except OSError:
            stop.wait(.02); continue
        connected[0] = True
        with connection:
            connection.settimeout(.2)
            while not stop.is_set():
                try:
                    if not connection.recv(65536):
                        return
                except socket.timeout:
                    continue
                except OSError:
                    return
        return
threading.Thread(target=rover_peer, daemon=True).start()
base = f'http://127.0.0.1:{server.server_port}'
config = {'schema': 1, 'origin': {'latitude': 35.7162542, 'longitude': -120.7615102, 'altitude': 300},
          'zoom': 16, 'radius': 1, 'cache': str(out / 'cache'),
          'imagery_url': base + '/imagery/{z}/{x}/{y}.jpg', 'elevation_url': base + '/elevation/{z}/{x}/{y}.png'}
(out / 'terrain.json').write_text(json.dumps(config, indent=2) + '\n')
command = [str(engine / 'Engine/Binaries/Linux/UnrealEditor'), str(project),
           '/Engine/Maps/Entry?game=/Script/PX4Unreal.PX4TerrainStartupTestMode', '-game', '-unattended', '-nop4', '-nosound', '-nullrhi',
           f'-PX4TerrainConfig={out}/terrain.json', '-PX4Vehicle=r1_rover', f'-PX4Port={rover_port}', '-PX4StartupTimeout=10',
           f'-abslog={out}/unreal.log']
if a.unavailable:
    command.append('-PX4TerrainStartupExpectTimeout')
try:
    with (out / 'stdout.log').open('w') as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=160 if a.unavailable else 90)
finally:
    stop.set(); server.shutdown(); server.server_close()
log = (out / 'unreal.log').read_text(errors='replace')
line = next((line for line in log.splitlines() if 'TERRAIN_STARTUP complete' in line), '')
metrics = {key: float(value) for key, value in re.findall(r'(\w+)=([\d.]+)', line)}
report = {'passed': result.returncode == 0 and metrics.get('passed') == 1 and connected[0],
          'returncode': result.returncode, 'delay_seconds': 0 if a.unavailable else 8, 'requests': requests[0], 'rover_peer_connected': connected[0],
          'metrics': metrics, 'production_timeout_seconds': 120}
if a.unavailable:
    waiting = [line for line in log.splitlines() if 'TERRAIN_STARTUP waiting' in line]
    progress = {key: float(value) for key, value in re.findall(r'(\w+)=([\d.]+)', waiting[-1])} if waiting else {}
    def timestamp(marker):
        line = next(line for line in log.splitlines() if marker in line)
        return datetime.strptime(re.search(r'\[(\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\d+)\]', line).group(1), '%Y.%m.%d-%H.%M.%S:%f')
    deadline_elapsed = (timestamp('Shared dynamic terrain did not provide spawn collision within 120 seconds.') - timestamp('TERRAIN_STARTUP begin_play_seconds=')).total_seconds()
    report.update({'mode': 'unavailable', 'last_waiting_progress': progress, 'observed_deadline_seconds': deadline_elapsed,
                   'passed': result.returncode == 1 and not connected[0] and 'PX4_SIM_LISTENING' not in log
                   and 'Shared dynamic terrain did not provide spawn collision within 120 seconds.' in log
                   and progress.get('elapsed', 0) >= 115 and progress.get('ticks', 0) > 100
                   and 119.5 <= deadline_elapsed <= 121
                   and all('vehicles=0 ground=0' in line for line in waiting)})
(out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report))
raise SystemExit(0 if report['passed'] else 1)
