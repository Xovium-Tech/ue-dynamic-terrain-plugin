#!/usr/bin/env python3
"""Exercise owned frame lifetime and activation using deterministic local images."""
import ctypes as C
import io
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
import tempfile
import threading
import time
from PIL import Image

class Config(C.Structure):
    _fields_=[('lat',C.c_double),('lon',C.c_double),('alt',C.c_double),('radius',C.c_double),('zoom',C.c_int),('cells',C.c_int),('cache',C.c_char_p),('imagery',C.c_char_p),('elevation',C.c_char_p)]
class Quality(C.Structure):
    _fields_=[('texture',C.c_int),('zoom',C.c_int),('radius',C.c_double)]
class Info(C.Structure):
    _fields_=[('abi',C.c_uint32),('generation',C.c_uint64),('texture_only',C.c_int),('page_count',C.c_size_t),('coverage_revision',C.c_uint64)]
class Page(C.Structure):
    _fields_=[('section',C.c_int),('reset',C.c_int),('vertices',C.c_size_t),('indices',C.c_size_t),('positions',C.POINTER(C.c_double)),('normals',C.POINTER(C.c_double)),('uv',C.POINTER(C.c_double)),('triangles',C.POINTER(C.c_uint32)),('width',C.c_int),('height',C.c_int),('rgb',C.POINTER(C.c_uint8)),('texture_only',C.c_int),('zoom',C.c_int)]
class FramePage(C.Structure):
    _fields_=[('page',Page),('tile_x',C.c_int),('tile_y',C.c_int),('tile_zoom',C.c_int),('visual_only',C.c_int)]

def run():
    root=Path(__file__).resolve().parent.parent
    lib=C.CDLL(str(root/'Binaries/Linux/libxovium_dynamic_terrain.so'))
    lib.xov_terrain_create_with_quality.argtypes=[C.POINTER(Config),C.POINTER(Quality)];lib.xov_terrain_create_with_quality.restype=C.c_void_p
    lib.xov_terrain_create_with_coverage.argtypes=[C.POINTER(Config),C.POINTER(Quality),C.c_double];lib.xov_terrain_create_with_coverage.restype=C.c_void_p
    lib.xov_terrain_update.argtypes=[C.c_void_p,C.c_double,C.c_double,C.c_double]
    lib.xov_terrain_acquire_frame.argtypes=[C.c_void_p];lib.xov_terrain_acquire_frame.restype=C.c_void_p
    lib.xov_terrain_frame_get_info.argtypes=[C.c_void_p,C.POINTER(Info)]
    lib.xov_terrain_frame_get_page.argtypes=[C.c_void_p,C.c_size_t,C.POINTER(FramePage)]
    lib.xov_terrain_release_frame.argtypes=[C.c_void_p]
    lib.xov_terrain_activate.argtypes=[C.c_void_p,C.c_uint64]
    lib.xov_terrain_destroy.argtypes=[C.c_void_p]
    image=Image.new('RGB',(256,256),(129,44,0));encoded=io.BytesIO();image.save(encoded,'PNG');dem=encoded.getvalue()
    image=Image.new('RGB',(256,256),(71,131,211));encoded=io.BytesIO();image.save(encoded,'JPEG');imagery=encoded.getvalue()
    class Handler(BaseHTTPRequestHandler):
        protocol_version='HTTP/1.1'
        def log_message(self,*args):pass
        def do_GET(self):
            data=dem if self.path.startswith('/elevation') else imagery
            self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
    class Server(ThreadingHTTPServer):daemon_threads=True
    server=Server(('127.0.0.1',0),Handler);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    runtime=None;frame=None
    try:
        with tempfile.TemporaryDirectory(prefix='terrain-frame-') as cache:
            base=f'http://127.0.0.1:{server.server_port}'.encode()
            config=Config(35.7,-120.7,300,200,16,16,cache.encode(),base+b'/imagery/{z}/{x}/{y}.jpg',base+b'/elevation/{z}/{x}/{y}.png')
            quality=Quality(512,17,200)
            runtime=lib.xov_terrain_create_with_quality(C.byref(config),C.byref(quality));assert runtime
            lib.xov_terrain_update(runtime,0,0,0)
            deadline=time.monotonic()+30
            while time.monotonic()<deadline and not frame:
                frame=lib.xov_terrain_acquire_frame(runtime);time.sleep(.01)
            assert frame,'initial owned frame absent'
            info=Info();assert lib.xov_terrain_frame_get_info(frame,C.byref(info))==0
            assert info.abi==3 and not info.texture_only and info.page_count>1
            assert lib.xov_terrain_activate(runtime,info.generation+1)==-1
            p=FramePage();assert lib.xov_terrain_frame_get_page(frame,0,C.byref(p))==0
            assert p.page.vertices and p.page.rgb and p.tile_zoom==16
            positions=C.string_at(p.page.positions,p.page.vertices*3*8)
            assert lib.xov_terrain_frame_get_page(frame,info.page_count,C.byref(p))==-1
            assert lib.xov_terrain_activate(runtime,info.generation)==0
            # Destroying runtime must not invalidate an owned frame or its images.
            start=time.monotonic();lib.xov_terrain_destroy(runtime);runtime=None
            assert time.monotonic()-start<3,'shutdown blocked on downloads/refinement'
            assert lib.xov_terrain_frame_get_page(frame,0,C.byref(p))==0
            assert positions==C.string_at(p.page.positions,p.page.vertices*3*8)
            expected=bytes(Image.open(io.BytesIO(imagery)).getpixel((0,0)))
            assert C.string_at(p.page.rgb,3)==expected
            lib.xov_terrain_release_frame(frame);frame=None
            # Distant coverage is another revision of the same generation.
            # The exact protected near buffers survive, with no extra collision.
            runtime=lib.xov_terrain_create_with_coverage(C.byref(config),C.byref(quality),2000000);assert runtime
            lib.xov_terrain_update(runtime,0,0,0)
            protected={};outer_pages=0;deadline=time.monotonic()+90
            while time.monotonic()<deadline:
                frame=lib.xov_terrain_acquire_frame(runtime)
                if not frame:time.sleep(.01);continue
                assert lib.xov_terrain_frame_get_info(frame,C.byref(info))==0
                if not info.texture_only:
                    near_count=0
                    for i in range(info.page_count):
                        assert lib.xov_terrain_frame_get_page(frame,i,C.byref(p))==0
                        key=(p.tile_x,p.tile_y,p.tile_zoom)
                        if p.visual_only:outer_pages+=1;continue
                        near_count+=1
                        geometry=(C.string_at(p.page.positions,p.page.vertices*3*8),
                                  C.string_at(p.page.normals,p.page.vertices*3*8),
                                  C.string_at(p.page.uv,p.page.vertices*2*8),
                                  C.string_at(p.page.triangles,p.page.indices*4))
                        if info.coverage_revision:assert protected[key]==geometry
                        else:protected[key]=geometry
                    assert near_count==len(protected)
                    assert lib.xov_terrain_activate(runtime,info.generation)==0
                complete=info.coverage_revision>0 and not info.texture_only
                lib.xov_terrain_release_frame(frame);frame=None
                if complete:break
            else:raise AssertionError('distant coverage did not become ready')
            assert 0<outer_pages<1024 and info.generation==1
            # Repartition the quadtree as both camera footprints move. Every
            # replacement outer page must already have its intended final LOD;
            # publishing bootstrap images here causes a coarse-then-sharp flash.
            checked_outer=0
            for generation,(north,east,altitude) in enumerate(
                    ((2000,2000,1000),(4000,-2000,30000),(0,0,100)),start=2):
                lib.xov_terrain_update(runtime,north,east,altitude)
                deadline=time.monotonic()+90
                while time.monotonic()<deadline:
                    frame=lib.xov_terrain_acquire_frame(runtime)
                    if not frame:time.sleep(.01);continue
                    assert lib.xov_terrain_frame_get_info(frame,C.byref(info))==0
                    complete=not info.texture_only and info.generation==generation
                    if complete:
                        for i in range(info.page_count):
                            assert lib.xov_terrain_frame_get_page(frame,i,C.byref(p))==0
                            if p.visual_only:
                                target=min(config.zoom,p.tile_zoom+1)
                                assert p.page.zoom>=target, (generation,p.tile_zoom,p.page.zoom,target)
                                checked_outer+=1
                        assert lib.xov_terrain_activate(runtime,info.generation)==0
                    lib.xov_terrain_release_frame(frame);frame=None
                    if complete:break
                else:raise AssertionError('replacement coverage did not become ready')
            assert checked_outer>0
            print(f'PASS UE ABI replacement imagery: 3 recenters, {checked_outer} outer pages at final LOD before activation')
            lib.xov_terrain_destroy(runtime);runtime=None
        print('PASS ABI v3 owned frame lifetime, invalid indices, generation activation, JPEG decoding, bounded shutdown')
    finally:
        if frame:lib.xov_terrain_release_frame(frame)
        if runtime:lib.xov_terrain_destroy(runtime)
        server.shutdown();server.server_close();thread.join()

if __name__=='__main__':run()
