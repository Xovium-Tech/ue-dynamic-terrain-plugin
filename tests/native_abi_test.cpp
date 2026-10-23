#include "ue_px4/media.h"
#include <cassert>
#include <cmath>
int main() {
    assert(xov_terrain_abi_version()==XOV_TERRAIN_ABI_VERSION);
    assert(!xov_terrain_create(nullptr));
    assert(!xov_terrain_acquire_frame(nullptr));
    assert(xov_terrain_activate(nullptr,1)==-1);
    assert(xov_terrain_frame_get_info(nullptr,nullptr)==-1);
    assert(xov_terrain_frame_get_page(nullptr,0,nullptr)==-1);
    xov_terrain_diagnostics diagnostics{};
    assert(xov_terrain_get_diagnostics(nullptr,&diagnostics)==-1);
    xov_terrain_release_frame(nullptr);xov_terrain_destroy(nullptr);
    xov_terrain_config c{47,8,400,500,14,32,"/tmp/xovium-unreal-abi-test","",""};
    c.latitude=NAN;assert(!xov_terrain_create(&c));
    c.latitude=47;
    assert(!xov_terrain_create_with_coverage(&c,nullptr,NAN));
    assert(!xov_terrain_create_with_coverage(&c,nullptr,-1));
    assert(!xov_terrain_create_with_coverage(&c,nullptr,2000001));
    for(int i=0;i<250;++i) {void* temporary=xov_terrain_create(&c);assert(temporary);xov_terrain_destroy(temporary);}
    void* t=xov_terrain_create(&c);assert(t);
    assert(!xov_terrain_acquire_frame(t));
    assert(xov_terrain_activate(t,1)==-1);
    assert(xov_terrain_get_diagnostics(t,nullptr)==-1);
    assert(xov_terrain_get_diagnostics(t,&diagnostics)==0);
    assert(diagnostics.active==0&&diagnostics.queued==0&&diagnostics.pending_frames==0);
    xov_terrain_destroy(t);
}
