#!/usr/bin/env python3
"""Rendered deterministic JPEG/DEM route with optional TLS and outages."""
import argparse
from collections import Counter
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import os
from pathlib import Path
import re
import ssl
import subprocess
import threading
import time
from PIL import Image

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ue-root',required=True,type=Path)
    p.add_argument('--seconds',type=int,default=120)
    p.add_argument('--output-dir',required=True,type=Path)
    p.add_argument('--project',type=Path,help='Override the isolated TerrainDemo project for before/after comparisons')
    p.add_argument('--https',action='store_true')
    p.add_argument('--outage-every',type=float,default=120)
    p.add_argument('--outage-for',type=float,default=4)
    p.add_argument('--null-rhi',action='store_true',help='CPU/collision smoke only; not rendered acceptance')
    p.add_argument('--sample-resources',action='store_true',help='Record RSS and per-process NVML VRAM every second (requires NVIDIA NVML)')
    a=p.parse_args()
    root=Path(__file__).resolve().parent.parent;out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
    if a.seconds<1:p.error('--seconds must be positive')
    started=time.monotonic();counts=Counter();lock=threading.Lock()
    class Server(ThreadingHTTPServer):
        daemon_threads=True
    class Handler(BaseHTTPRequestHandler):
        protocol_version='HTTP/1.1'
        def log_message(self,*args):pass
        def do_GET(self):
            age=time.monotonic()-started
            if a.outage_every>0 and age>a.outage_every and age%a.outage_every<a.outage_for:
                with lock:counts['outages']+=1
                self.send_response(503);self.send_header('Content-Length','0');self.send_header('Retry-After','1');self.end_headers();return
            try:
                kind,z,x,y=self.path.strip('/').split('/');z=int(z);x=int(x);y=int(y.split('.')[0])
                if kind not in ('imagery','elevation'):raise ValueError()
            except ValueError:self.send_error(404);return
            image=Image.new('RGB',(256,256))
            if kind=='elevation':
                # Constant Terrarium height 300m makes collision trace expectations exact.
                image.paste((129,44,0),(0,0,256,256));fmt='PNG'
            else:
                image.putdata([((x+i//16*17)%256,(y+j//16*19)%256,(z*11+i+j)%256) for j in range(256) for i in range(256)]);fmt='JPEG'
            output=io.BytesIO();image.save(output,fmt,quality=95);data=output.getvalue()
            self.send_response(200);self.send_header('Content-Type','image/jpeg' if kind=='imagery' else 'image/png')
            self.send_header('Content-Length',str(len(data)));self.end_headers()
            try:self.wfile.write(data)
            except (BrokenPipeError,ConnectionResetError):return
            with lock:counts[kind]+=1
    server=Server(('127.0.0.1',0),Handler);environment=os.environ.copy()
    if a.https:
        cert=out/'fixture-ca.pem';key=out/'fixture-key.pem'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1','-keyout',str(key),'-out',str(cert),'-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost,IP:127.0.0.1'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(cert,key);server.socket=context.wrap_socket(server.socket,server_side=True)
        environment['CURL_CA_BUNDLE']=str(cert);environment['SSL_CERT_FILE']=str(cert)
    worker=threading.Thread(target=server.serve_forever,daemon=True);worker.start()
    base=f"{'https' if a.https else 'http'}://127.0.0.1:{server.server_port}"
    config={'schema':1,'origin':{'latitude':35.7162542,'longitude':-120.7615102,'altitude':300},'zoom':16,'radius':1,
            'cache':str(out/'cache'),'imagery_url':base+'/imagery/{z}/{x}/{y}.jpg','elevation_url':base+'/elevation/{z}/{x}/{y}.png'}
    (out/'terrain.json').write_text(json.dumps(config,indent=2)+'\n')
    project=(a.project or root/'examples/TerrainDemo/TerrainDemo.uproject').resolve()
    command=[str(a.ue_root.resolve()/'Engine/Binaries/Linux/UnrealEditor'),str(project),
             '/Engine/Maps/Entry?game=/Script/TerrainDemo.TerrainDemoGameMode','-game','-unattended','-nop4','-nosound',
             '-RenderOffscreen','-vulkan','-ResX=960','-ResY=540',f'-TerrainSeconds={a.seconds}',
             f'-TerrainConfig={out}/terrain.json','-TerrainDetailZoom=17',f'-TerrainScreenshot={out}/terrain.png',f'-abslog={out}/unreal.log']
    if a.null_rhi:command.append('-nullrhi')
    resources=None
    try:
        with (out/'stdout.log').open('w') as log:
            process=subprocess.Popen(command,env=environment,stdout=log,stderr=subprocess.STDOUT)
            sampler=None
            try:
                if a.sample_resources:
                    from sample_resources import ResourceSampler
                    sampler=ResourceSampler(process.pid,out/'resources.jsonl')
                returncode=process.wait(timeout=a.seconds+180)
            finally:
                if process.poll() is None:process.kill();process.wait()
                if sampler:resources=sampler.close()
    finally:server.shutdown();server.server_close();worker.join()
    log=(out/'unreal.log').read_text(errors='replace') if (out/'unreal.log').exists() else ''
    records=[];diagnostics=[];percentiles=[]
    for line in log.splitlines():
        if 'TERRAIN_SOAK elapsed=' in line:
            record={k:float(v) for k,v in re.findall(r'(\w+)=([\d.]+)',line)};records.append(record)
        if 'TERRAIN_DIAGNOSTICS elapsed=' in line:
            diagnostics.append({k:float(v) for k,v in re.findall(r'(\w+)=([\d.]+)',line)})
        if 'TERRAIN_PERCENTILES elapsed=' in line:
            percentiles.append({k:float(v) for k,v in re.findall(r'(\w+)=([\d.]+)',line)})
    pixels_ok=False
    screenshot=out/'terrain.png'
    if screenshot.exists():
        from PIL import ImageStat
        image=Image.open(screenshot).convert('RGB');stats=ImageStat.Stat(image)
        pixels_ok=max(stats.mean)>1 and max(stats.stddev)>1
    final=re.search(r'TERRAIN_SOAK complete ready=\d+ generations=(\d+)',log)
    final_line=next((line for line in reversed(log.splitlines()) if 'TERRAIN_SOAK complete' in line),'')
    final_metrics={k:float(v) for k,v in re.findall(r'(\w+)=([\d.]+)',final_line)}
    summary={'final_generation':int(final.group(1)) if final else 0,'nonblack_render':pixels_ok,'returncode':returncode,'seconds':a.seconds,'https':a.https,'rendered':not a.null_rhi,
             'requests':dict(counts),'records':records,'diagnostics':diagnostics,'percentiles':percentiles,'final_metrics':final_metrics,
             'complete':'TERRAIN_SOAK complete ready=1' in log,'resources':resources}
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps({k:v for k,v in summary.items() if k not in ('records','diagnostics','percentiles')}))
    if returncode or not summary['complete'] or (not a.null_rhi and not pixels_ok):raise SystemExit(1)
    if a.sample_resources and (resources['errors'] or not resources['vram_peak_bytes']):raise SystemExit('Resource sampling failed or recorded no GPU allocation')

if __name__=='__main__':main()
