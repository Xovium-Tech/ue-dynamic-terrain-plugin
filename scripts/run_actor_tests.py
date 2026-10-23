#!/usr/bin/env python3
"""Run the complete actor automation group in the canonical local example."""
import argparse
from collections import Counter
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
from pathlib import Path
import socket
import subprocess
import threading

from PIL import Image

root = Path(__file__).resolve().parent.parent
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--ue-root', type=Path, required=True)
p.add_argument('--output-dir', type=Path, required=True)
a = p.parse_args()
out = a.output_dir.resolve()
out.mkdir(parents=True, exist_ok=False)
engine = a.ue_root.resolve()
encoded = {}
for name in ('identity-a', 'identity-b', 'elevation'):
    image = Image.new('RGB', (256, 256), (129, 44, 0))
    if name != 'elevation':
        pixels = image.load()
        for y in range(256):
            for x in range(256):
                rgb = ((x * 17 + y * 3) % 256, (x * 5 + y * 19) % 256, (x ^ y) % 256)
                pixels[x, y] = rgb if name == 'identity-a' else tuple(255 - c for c in rgb)
    buffer = io.BytesIO()
    image.save(buffer, 'PNG')
    encoded[name] = buffer.getvalue()
counts = Counter()
count_lock = threading.Lock()


class FixtureHandler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def setup(self):
        super().setup()
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def log_message(self, *unused):
        pass

    def do_GET(self):
        name = self.path.strip('/').split('/')[0]
        data = encoded.get(name)
        if data is None:
            self.send_error(404)
            return
        with count_lock:
            counts[name] += 1
        self.send_response(200)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Content-Type', 'image/png')
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass


server = ThreadingHTTPServer(('127.0.0.1', 0), FixtureHandler)
server.daemon_threads = True
server_thread = threading.Thread(target=server.serve_forever, daemon=True)
server_thread.start()
command = [str(engine / 'Engine/Binaries/Linux/UnrealEditor-Cmd'),
           str(root / 'examples/TerrainDemo/TerrainDemo.uproject'),
           '-unattended', '-nop4', '-nosound', '-RenderOffscreen', '-vulkan', '-nowrite', '-noini',
           '-ExecCmds=Automation RunTests Xovium.DynamicTerrain',
           f'-TerrainIdentityFixtureURL=http://127.0.0.1:{server.server_port}',
           '-TestExit=Automation Test Queue Empty', f'-ReportExportPath={out}',
           f'-abslog={out}/unreal.log']
(out / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
try:
    with (out / 'stdout.log').open('w') as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=240)
finally:
    server.shutdown()
    server.server_close()
    server_thread.join()
    (out / 'identity-fixture.json').write_text(json.dumps(dict(requests=counts), indent=2) + '\n')
report = out / 'index.json'
data = json.loads(report.read_text(encoding='utf-8-sig')) if report.exists() else {}
log = (out / 'unreal.log').read_text(errors='replace') if (out / 'unreal.log').exists() else ''
passed = (result.returncode in (0, 1) and data.get('succeeded') == 5
          and data.get('failed') == 0 and data.get('notRun') == 0
          and all(counts[name] > 0 for name in encoded)
          and 'TestExit: Automation Test Queue Empty' in log)
summary = dict(passed=passed, engine_returncode=result.returncode,
               succeeded=data.get('succeeded'), failed=data.get('failed'),
               not_run=data.get('notRun'), report=str(report))
(out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
print(json.dumps(summary, indent=2))
raise SystemExit(0 if passed else 1)
