#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Plain C boundary: Unreal uses libc++, distro GStreamer/OpenCV use libstdc++.
 * Owned handles must be destroyed before module unload. Callbacks borrow data
 * only for their duration and always execute on the caller of terrain_poll. */
typedef struct {
    double latitude, longitude, altitude, radius_m;
    int zoom, cells;
    const char* cache;
    const char* imagery_url;
    const char* elevation_url;
} xov_terrain_config;
typedef struct {
    int texture_size; /* Per-page limit, power of two: 256..4096. */
    int imagery_zoom; /* Desired near-field source zoom: geometry zoom..20. */
    double detail_radius_m;
} xov_terrain_quality;
typedef struct xov_terrain_page {
    int section, reset;
    size_t vertices, indices;
    const double* positions; /* north/east/up metres */
    const double* normals;
    const double* uv;
    const uint32_t* triangles;
    int width, height;
    const uint8_t* rgb;
    int texture_only; /* Update an existing section; mesh pointers are null. */
    int imagery_zoom; /* Actual source detail, not the upscaled texture width. */
} xov_terrain_page;
typedef void (*xov_terrain_callback)(void* user, const xov_terrain_page* page);
/* Legacy callers retain the original small preview texture budget. */
void* xov_terrain_create(const xov_terrain_config* config);
void* xov_terrain_create_with_quality(const xov_terrain_config* config, const xov_terrain_quality* quality);
void* xov_terrain_create_with_coverage(const xov_terrain_config* config, const xov_terrain_quality* quality, double outer_radius_m);
void xov_terrain_update(void* terrain, double north, double east, double up);
int xov_terrain_poll(void* terrain, xov_terrain_callback callback, void* user);
void xov_terrain_destroy(void* terrain);
/* ABI v3: an acquired frame owns immutable CPU buffers independently of runtime.
 * Acquire/update/activate are serialized by the host. Read/release may run on a
 * worker; a frame must only be accessed by one thread at a time. Page pointers
 * borrow frame scratch storage until the next get_page call or frame release.
 * Copy a page before requesting another page from the same frame. */
#define XOV_TERRAIN_ABI_VERSION 3u
typedef struct xov_terrain_frame xov_terrain_frame;
typedef struct {
    uint32_t abi_version;
    uint64_t generation;
    int texture_only;
    size_t page_count;
    uint64_t coverage_revision;
} xov_terrain_frame_info;
typedef struct {
    xov_terrain_page page;
    int tile_x, tile_y, tile_zoom;
    int visual_only;
} xov_terrain_frame_page;
uint32_t xov_terrain_abi_version(void);
xov_terrain_frame* xov_terrain_acquire_frame(void* terrain);
int xov_terrain_frame_get_info(const xov_terrain_frame* frame, xov_terrain_frame_info* info);
int xov_terrain_frame_get_page(xov_terrain_frame* frame, size_t index, xov_terrain_frame_page* page);
void xov_terrain_release_frame(xov_terrain_frame* frame);
/* Optional immutable-image identity, additive to ABI v3. The token holds only a
 * weak owner: it never retains RGB pixels, the frame, or the terrain runtime.
 * Equality means the same published image object, not equal-sized/equal-zoom
 * imagery or a probabilistic pixel hash. Equal expired tokens remain equal.
 * Null tokens never compare equal. Acquire returns null for invalid/missing
 * images or allocation failure. Tokens may outlive their frame and runtime;
 * release each acquired token exactly once through this C boundary. */
typedef struct xov_terrain_image_identity xov_terrain_image_identity;
xov_terrain_image_identity* xov_terrain_frame_acquire_image_identity(
    const xov_terrain_frame* frame, size_t index);
int xov_terrain_image_identity_equal(const xov_terrain_image_identity* a,
                                    const xov_terrain_image_identity* b);
void xov_terrain_release_image_identity(xov_terrain_image_identity* identity);
/* Acknowledge host acceptance and keep only readiness metadata in the bridge.
 * Already acquired frames retain their buffers until explicitly released. */
int xov_terrain_activate(void* terrain, uint64_t generation);
/* Read-only, fixed-width counters; call on the serialized runtime host thread. */
typedef struct {
    uint64_t queued, active, peak_queued, peak_active, buffered_bytes;
    uint64_t cache_hits, downloaded, failed, cancelled, bytes, connections, retries;
    uint64_t decoded_dem_bytes, decoded_dem_entries, decoded_imagery_bytes, decoded_imagery_entries;
    uint64_t page_cache_bytes, page_cache_entries, pending_frames, pending_texture_pages, pending_texture_bytes;
    uint64_t outer_mesh_bytes, outer_mesh_entries, outer_mesh_limit_bytes, outer_mesh_hits, outer_mesh_misses;
} xov_terrain_diagnostics;
int xov_terrain_get_diagnostics(void* terrain, xov_terrain_diagnostics* diagnostics);
#ifdef __cplusplus
}
#endif
