#include "ue_px4/media.h"
#include "dynamic_terrain/core/TerrainRuntime.hh"
#include <deque>
#include <mutex>
#include <cmath>
#include <stdexcept>
#include <map>
namespace dt = dynamic_terrain;
namespace {
struct Geographic final : dt::GeographicTransform {
    double lat, lon, alt;
    explicit Geographic(const xov_terrain_config& c):lat(c.latitude),lon(c.longitude),alt(c.altitude) {}
    dt::Vec3 LocalFromGeodetic(double a,double b,double h) const override {
        constexpr double meters=6378137.0*dt::kPi/180;
        return {(a-lat)*meters,(b-lon)*meters*std::cos(lat*dt::kPi/180),h-alt};
    }
    dt::Vec3 GeodeticFromLocal(const dt::Vec3& p) const override {
        constexpr double meters=6378137.0*dt::kPi/180;
        return {lat+p.X()/meters,lon+p.Y()/(meters*std::cos(lat*dt::kPi/180)),alt+p.Z()};
    }
    double LatitudeDeg() const override {return lat;}
    double LongitudeDeg() const override {return lon;}
    double ElevationReference() const override {return alt;}
};
struct Terrain final : dt::TerrainRenderSink {
    std::mutex mutex;
    std::shared_ptr<const dt::TerrainSnapshot> pending, staged;
    // Activation acknowledges that the host owns its prepared mesh and imagery.
    // Keep readiness metadata only: caller-owned frames and worker/cache owners
    // independently retain any CPU buffers they still need.
    struct Activation {
        uint64_t generation, coverageRevision;
        dt::TileKey centerTile;
    };
    std::optional<Activation> active;
    std::map<size_t, dt::TexturePageUpdate> textures;
    uint64_t textureGeneration=0;
    std::shared_ptr<Geographic> geo;
    std::unique_ptr<dt::TerrainRuntime> runtime; // destroyed first, joins workers before sink dies
    Terrain(const xov_terrain_config& c,const xov_terrain_quality& quality,double outerRadius=0) : geo(std::make_shared<Geographic>(c)) {
        dt::Config cfg;
        cfg.modelName="unreal";
        cfg.cacheDir=c.cache && *c.cache ? c.cache : "~/.cache/gz_dynamic_terrain";
        cfg.visualRadiusM=std::clamp(c.radius_m,100.0,15000.0);
        cfg.visualOuterRadiusM=outerRadius;
        cfg.visualGeometryZoom=std::clamp(c.zoom,10,17);
        cfg.visualElevationZoom=cfg.visualGeometryZoom;
        cfg.visualMeshCellsPerTile=std::clamp(c.cells,8,128);
        cfg.visualRecenterDistanceM=cfg.visualRadiusM*.4;
        cfg.visualBootstrapImageryZoom=std::min(12,cfg.visualGeometryZoom-1);
        const int requested=std::clamp(quality.texture_size,256,4096);
        int textureSize=256;
        while(textureSize*2<=requested) textureSize*=2;
        cfg.visualPageTextureMaxSize=textureSize;
        cfg.visualTextureSize=textureSize;
        cfg.visualDetailZoom=std::clamp(quality.imagery_zoom,cfg.visualGeometryZoom,20);
        cfg.visualDetailRadiusM=std::clamp(quality.detail_radius_m,100.0,15000.0);
        const auto radius=cfg.visualDetailRadiusM;
        const int detail=cfg.visualDetailZoom;
        cfg.visualImageryLodTable={{radius,detail},{radius*2.5,detail-1},
                                  {radius*5.5,detail-2},{1000000.0,cfg.visualGeometryZoom}};
        cfg.visualRefineTexture=true;
        cfg.visualProgressiveUploads=true;
        cfg.visualActivationTimeoutSec=60;
        cfg.enableCollision=false; // Unreal cooks the visual triangles into Chaos collision.
        cfg.imageryProvider="google_satellite";
        if(c.imagery_url && *c.imagery_url) {cfg.imageryProvider="custom"; cfg.imageryUrl=c.imagery_url;}
        if(c.elevation_url && *c.elevation_url) cfg.elevationUrl=c.elevation_url;
        cfg.diagnostics=false;
        dt::normalizeConfig(cfg);
        runtime=std::make_unique<dt::TerrainRuntime>(cfg,geo,*this);
    }
    void QueueSnapshot(std::shared_ptr<const dt::TerrainSnapshot> s) override {
        std::lock_guard<std::mutex> lock(mutex); pending=std::move(s); textures.clear();
    }
    void QueueTexture(dt::TextureUpdate t) override {
        std::lock_guard<std::mutex> lock(mutex);
        if(!active || t.generation!=active->generation) return;
        if (t.generation != textureGeneration) { textures.clear(); textureGeneration=t.generation; }
        for(auto& page:t.pages) {
            auto old=textures.find(page.pageIndex);
            if(old==textures.end() || page.imageryZoom>=old->second.imageryZoom)
                textures[page.pageIndex]=std::move(page);
        }
    }
    uint64_t ActiveGeneration() const override {auto* self=const_cast<Terrain*>(this); std::lock_guard<std::mutex> lock(self->mutex);return active?active->generation:0;}
    uint64_t ActiveCoverageRevision() const override {auto* self=const_cast<Terrain*>(this); std::lock_guard<std::mutex> lock(self->mutex);return active?active->coverageRevision:0;}
    bool HasActiveTerrain() const override {return ActiveGeneration()!=0;}
    std::optional<dt::TileKey> ActiveCenterTile() const override {
        auto* self=const_cast<Terrain*>(this);std::lock_guard<std::mutex> lock(self->mutex);
        return active?std::optional<dt::TileKey>(active->centerTile):std::nullopt;
    }
};
}
extern "C" {
void* xov_terrain_create(const xov_terrain_config* c) {
    const xov_terrain_quality preview{512,17,1000};
    return xov_terrain_create_with_quality(c,&preview);
}
void* xov_terrain_create_with_quality(const xov_terrain_config* c,const xov_terrain_quality* q) {
    return xov_terrain_create_with_coverage(c,q,0);
}
void* xov_terrain_create_with_coverage(const xov_terrain_config* c,const xov_terrain_quality* q,double outerRadius) {
    const xov_terrain_quality defaults{2048,17,1000};
    const auto& quality=q?*q:defaults;
    if(!std::isfinite(quality.detail_radius_m)||!std::isfinite(outerRadius)||outerRadius<0||outerRadius>2000000) return nullptr;
    if(!c||!std::isfinite(c->latitude)||std::abs(c->latitude)>85||!std::isfinite(c->longitude)||std::abs(c->longitude)>180
       ||!std::isfinite(c->altitude)||!std::isfinite(c->radius_m)) return nullptr;
    try {return new Terrain(*c,quality,outerRadius);} catch(...) {return nullptr;}
}
void xov_terrain_update(void* p,double n,double e,double u) {
    if(!p||!std::isfinite(n)||!std::isfinite(e)||!std::isfinite(u)) return;
    try {auto& t=*static_cast<Terrain*>(p);const dt::Vec3 pos(n,e,u);auto ll=t.geo->GeodeticFromLocal(pos);
        t.runtime->UpdateVisual(pos,ll.X(),ll.Y());} catch(...) {}
}
int xov_terrain_poll(void* p,xov_terrain_callback cb,void* user) {
    if(!p||!cb) return -1;
    auto* frame=xov_terrain_acquire_frame(p);
    if(!frame) return 0;
    xov_terrain_frame_info info{}; xov_terrain_frame_get_info(frame,&info);
    int count=0;
    for(size_t i=0;i<info.page_count;++i) {
        xov_terrain_frame_page page{};
        if(xov_terrain_frame_get_page(frame,i,&page)==0) {cb(user,&page.page);++count;}
    }
    if(!info.texture_only) xov_terrain_activate(p,info.generation);
    xov_terrain_release_frame(frame);
    return count;
}
void xov_terrain_destroy(void* p) {delete static_cast<Terrain*>(p);}
}

struct xov_terrain_frame {
    std::shared_ptr<const dt::TerrainSnapshot> snapshot;
    std::vector<dt::TexturePageUpdate> textures;
    uint64_t generation=0;
    std::vector<double> positions,normals,uv;
};
struct xov_terrain_image_identity {
    std::weak_ptr<const dt::ImageData> owner;
    const dt::ImageData* address;
    explicit xov_terrain_image_identity(const std::shared_ptr<const dt::ImageData>& image)
        : owner(image),address(image.get()) {}
};
extern "C" {
uint32_t xov_terrain_abi_version(void) {return XOV_TERRAIN_ABI_VERSION;}
xov_terrain_frame* xov_terrain_acquire_frame(void* p) {
    if(!p) return nullptr;
    try {
        auto& t=*static_cast<Terrain*>(p);
        auto frame=std::make_unique<xov_terrain_frame>();
        std::lock_guard<std::mutex> lock(t.mutex);
        if(t.pending) {
            frame->snapshot=std::move(t.pending);
            frame->generation=frame->snapshot->generation;
            t.staged=frame->snapshot;
        } else if(t.active && t.textureGeneration==t.active->generation && !t.textures.empty()) {
            frame->generation=t.active->generation;
            for(auto& item:t.textures) frame->textures.push_back(std::move(item.second));
            t.textures.clear();
        } else return nullptr;
        return frame.release();
    } catch(...) {return nullptr;}
}
int xov_terrain_frame_get_info(const xov_terrain_frame* f,xov_terrain_frame_info* info) {
    if(!f||!info) return -1;
    *info={XOV_TERRAIN_ABI_VERSION,f->generation,f->snapshot?0:1,
           f->snapshot?f->snapshot->pages.size():f->textures.size(),
           f->snapshot?f->snapshot->coverageRevision:0};
    return 0;
}
int xov_terrain_frame_get_page(xov_terrain_frame* f,size_t i,xov_terrain_frame_page* out) {
    if(!f||!out) return -1;
    try {
        dt::TileKey key; int index,zoom; std::shared_ptr<const dt::ImageData> tex;
        std::shared_ptr<const dt::MeshData> mesh;
        out->visual_only=0;
        if(f->snapshot) {
            if(i>=f->snapshot->pages.size()) return -1;
            const auto& p=f->snapshot->pages[i];key=p.key;index=int(p.index);zoom=p.imageryZoom;tex=p.texture;mesh=p.mesh;
            out->visual_only=p.visualOnly?1:0;
        } else {
            if(i>=f->textures.size()) return -1;
            const auto& p=f->textures[i];key=p.pageKey;index=int(p.pageIndex);zoom=p.imageryZoom;tex=p.texture;
        }
        f->positions.clear();f->normals.clear();f->uv.clear();
        if(mesh) {
            f->positions.reserve(mesh->positions.size()*3);f->normals.reserve(mesh->normals.size()*3);f->uv.reserve(mesh->texCoords.size()*2);
            for(const auto& v:mesh->positions) f->positions.insert(f->positions.end(),{v.X(),v.Y(),v.Z()});
            for(const auto& v:mesh->normals) f->normals.insert(f->normals.end(),{v.X(),v.Y(),v.Z()});
            for(const auto& v:mesh->texCoords) f->uv.insert(f->uv.end(),{v.X(),v.Y()});
        }
        out->tile_x=key.x;out->tile_y=key.y;out->tile_zoom=key.z;
        out->page={index,int(f->snapshot && i==0),f->positions.size()/3,mesh?mesh->indices.size():0,
            f->positions.data(),f->normals.data(),f->uv.data(),mesh?mesh->indices.data():nullptr,
            tex?int(tex->width):0,tex?int(tex->height):0,tex?tex->rgb.data():nullptr,
            f->snapshot?0:1,zoom};
        return 0;
    } catch(...) {return -1;}
}
void xov_terrain_release_frame(xov_terrain_frame* f) {delete f;}
xov_terrain_image_identity* xov_terrain_frame_acquire_image_identity(
    const xov_terrain_frame* f,size_t i) {
    if(!f) return nullptr;
    try {
        std::shared_ptr<const dt::ImageData> image;
        if(f->snapshot) {
            if(i>=f->snapshot->pages.size()) return nullptr;
            image=f->snapshot->pages[i].texture;
        } else {
            if(i>=f->textures.size()) return nullptr;
            image=f->textures[i].texture;
        }
        if(!image||!image->Valid()) return nullptr;
        return new xov_terrain_image_identity(image);
    } catch(...) {return nullptr;}
}
int xov_terrain_image_identity_equal(const xov_terrain_image_identity* a,
                                    const xov_terrain_image_identity* b) {
    // Weak owners keep their control blocks (not image payloads) alive, so even
    // expired tokens cannot match a new allocation that reused the image address.
    // Also distinguish aliases to different images sharing one control block.
    return a&&b&&a->address==b->address&&
        !a->owner.owner_before(b->owner)&&!b->owner.owner_before(a->owner);
}
void xov_terrain_release_image_identity(xov_terrain_image_identity* identity) {delete identity;}
int xov_terrain_activate(void* p,uint64_t generation) {
    if(!p) return -1;
    auto& t=*static_cast<Terrain*>(p);std::lock_guard<std::mutex> lock(t.mutex);
    if(!t.staged || t.staged->generation!=generation) return -1;
    t.active=Terrain::Activation{t.staged->generation,t.staged->coverageRevision,t.staged->centerTile};
    t.staged.reset();t.textures.clear();t.textureGeneration=generation;return 0;
}
int xov_terrain_get_diagnostics(void* p,xov_terrain_diagnostics* out) {
    if(!p||!out) return -1;
    try {
        auto& t=*static_cast<Terrain*>(p);const auto data=t.runtime->Diagnostics();const auto& d=data.downloads;
        *out={d.queued,d.active,d.peakQueued,d.peakActive,d.bufferedBytes,
              d.cacheHits,d.downloaded,d.failed,d.cancelled,d.bytes,d.connections,d.retries,
              data.decodedDemBytes,data.decodedDemEntries,data.decodedImageryBytes,data.decodedImageryEntries,
              data.pageTextureBytes,data.pageTextureEntries,0,0,0,
              data.outerMeshBytes,data.outerMeshEntries,data.outerMeshLimitBytes,data.outerMeshHits,data.outerMeshMisses};
        std::lock_guard<std::mutex> lock(t.mutex);
        out->pending_frames=uint64_t(bool(t.pending))+uint64_t(bool(t.staged));
        out->pending_texture_pages=t.textures.size();
        for(const auto& item:t.textures) if(item.second.texture) out->pending_texture_bytes+=item.second.texture->rgb.size();
        return 0;
    } catch(...) {return -1;}
}
}
