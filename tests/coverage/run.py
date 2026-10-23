#!/usr/bin/env python3
import argparse
from collections import Counter
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
import io,json,re,subprocess,threading,time,shutil
from urllib.parse import urlparse
from PIL import Image
p=argparse.ArgumentParser();p.add_argument('--engine',type=Path,required=True);p.add_argument('--project',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--warm-cache-from',type=Path);a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
port=0
if a.warm_cache_from:
    prior=json.loads((a.warm_cache_from/'terrain.json').read_text())
    provider=urlparse(prior['imagery_url'])
    if provider.hostname!='127.0.0.1' or urlparse(prior['elevation_url']).port!=provider.port:p.error('Warm cache must use this local fixture provider')
    port=provider.port
    shutil.copytree(Path(prior['cache']),out/'cache',dirs_exist_ok=True)
counts=Counter();encoded={}
for kind,color,fmt in [('imagery',(35,185,45),'JPEG'),('elevation',(129,44,0),'PNG')]:
    buf=io.BytesIO();Image.new('RGB',(256,256),color).save(buf,fmt);encoded[kind]=buf.getvalue()
class Handler(BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def log_message(self,*args):pass
    def do_GET(self):
        kind=self.path.strip('/').split('/')[0]
        if kind not in encoded:self.send_error(404);return
        data=encoded[kind];self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers()
        try:self.wfile.write(data);counts[kind]+=1
        except (ConnectionResetError,BrokenPipeError):pass
server=ThreadingHTTPServer(('127.0.0.1',port),Handler);server.daemon_threads=True
worker=threading.Thread(target=server.serve_forever,daemon=True);worker.start()
base=f'http://127.0.0.1:{server.server_port}'
config=dict(schema=1,origin=dict(latitude=35.7162542,longitude=-120.7615102,altitude=300),zoom=15,radius=2,
 cache=str(out/'cache'),visual_outer_radius_m=2000000,imagery_url=base+'/imagery/{z}/{x}/{y}.jpg',elevation_url=base+'/elevation/{z}/{x}/{y}.png')
(out/'terrain.json').write_text(json.dumps(config,indent=2)+'\n')
cmd=[str(a.engine.resolve()/'Engine/Binaries/Linux/UnrealEditor'),str(a.project.resolve()),'/Engine/Maps/Entry?game=/Script/TerrainDemo.TerrainDemoGameMode','-game','-unattended','-nop4','-nosound','-RenderOffscreen','-vulkan','-ResX=1280','-ResY=960',f'-TerrainConfig={out}/terrain.json',f'-CoverageOutput={out}',f'-abslog={out}/unreal.log']
started=time.monotonic()
try:
    with (out/'stdout.log').open('w') as log:
        process=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,timeout=240)
    text=(out/'unreal.log').read_text(errors='replace')
    match=re.search(r'TERRAIN_COVERAGE passed=(\d+) images=(\d+) checked=(\d+) missing=(\d+) cooks=(\d+) generation=(\d+) revision=(\d+) reason=([^\n]+)',text)
    report=dict(command=cmd,exit_code=process.returncode,seconds=time.monotonic()-started,requests=dict(counts),warm_cache_from=str(a.warm_cache_from) if a.warm_cache_from else None,passed=bool(match and match[1]=='1' and process.returncode==0))
    if match:report['result']=dict(zip(['passed','images','checked','missing','cooks','generation','revision','reason'],match.groups()))
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
    raise SystemExit(0 if report['passed'] else 1)
finally:server.shutdown();server.server_close();worker.join()
