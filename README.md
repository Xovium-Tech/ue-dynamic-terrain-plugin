# Dynamic Terrain for Unreal Engine

[![CI](https://github.com/Xovium-Tech/ue-dynamic-terrain-plugin/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/Xovium-Tech/ue-dynamic-terrain-plugin/actions/workflows/build.yml)
[![Latest release](https://img.shields.io/github/v/release/Xovium-Tech/ue-dynamic-terrain-plugin)](https://github.com/Xovium-Tech/ue-dynamic-terrain-plugin/releases/latest)

A standalone Unreal Engine 5.6.1 plugin for Linux x86_64. It streams satellite imagery and elevation around an actor, preserves full terrain triangles for Chaos collision, and shares the terrain core and tile cache with Gazebo and Isaac. No PX4, Gazebo installation, or Xovium launcher is required.

## Supported versions

Linux x86_64, Unreal Engine **5.6.1**, and a C++ Editor project. Native libraries
and Unreal modules must be rebuilt for the installed engine and system libraries.

## Dependencies

Install the official Linux Unreal Engine 5.6.1 distribution and its bundled
C++ toolchain. Install the native dependencies on Ubuntu 24.04:

```sh
sudo apt install cmake g++ python3 libcurl4-openssl-dev libopencv-dev libjpeg-dev
```

## Get the source and build

Download `ue-dynamic-terrain-plugin-v0.3.0.zip` from the
[latest release](https://github.com/Xovium-Tech/ue-dynamic-terrain-plugin/releases/latest)
and extract it, or clone the repository:

```sh
git clone https://github.com/Xovium-Tech/ue-dynamic-terrain-plugin.git
cd ue-dynamic-terrain-plugin
python3 scripts/install.py /path/to/MyProject.uproject --build-native
/path/to/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh \
  MyProjectEditor Linux Development /path/to/MyProject.uproject -WaitMutex
```

Replace the project and engine paths and `MyProjectEditor` with your project's
Editor target. Source releases contain the plugin, default material, native
sources and required helpers. Development examples and tests remain in Git.

## Load and use


Build the project's Editor target with its matching Unreal Engine toolchain, then open the project. Add **Dynamic Terrain** (`ADynamicTerrainActor`) to the level. Set latitude, longitude, altitude, radius and **Follow Actor**. The packaged `/DynamicTerrain/M_Terrain` material supplies satellite imagery; a custom material can expose the `TerrainTexture` texture parameter. The **Refresh Terrain** editor button previews it without starting a simulation.

**Stop Terrain** cancels terrain work and keeps it stopped across game ticks while preserving the saved **Stream Terrain** setting. **Refresh Terrain** resumes it when streaming is enabled. Turning **Stream Terrain** off for at least one game tick and then on also resumes automatic streaming. Refresh respects disabled streaming, including `PX4NoTerrain`; it does not enable the setting. In the editor, Stop also turns off preview, and either Refresh or explicitly enabling **Preview In Editor** resumes it.

For Unreal 5.6.1 on Linux Vulkan, apply the version-specific renderer workaround below before long sessions. The example already includes it; the plugin installer leaves existing project settings untouched.

The source installer refuses to overwrite an existing plugin directory. Keep local changes when updating it. `python3 scripts/package.py output.tar.gz` creates a source package. Add `--with-binaries --ue-root /path/to/UnrealEngine` to include local Linux binaries and `BINARY_MANIFEST.json`, which records the exact engine version/BuildId, architecture, source and binary hashes, and linked dependencies. These binaries must match the target engine and host dependencies; the local OpenCV 4.9 build is not ABI-compatible with Ubuntu's apt OpenCV 4.6. Rebuild the native library and Unreal module when installing on another host.

## Repository example

The [example](https://github.com/Xovium-Tech/ue-dynamic-terrain-plugin/tree/main/examples/TerrainDemo) and its build helper require a repository checkout:

```sh
bash scripts/build_example.sh /path/to/UnrealEngine
/path/to/UnrealEngine/Engine/Binaries/Linux/UnrealEditor "$PWD/examples/TerrainDemo/TerrainDemo.uproject" -game
```

`build_example.sh` builds the native C library, runs its lifecycle tests, builds the Unreal module, and creates the default material if it is missing. The material's reproducible authoring script is `scripts/create_material.py`. The sample's `Plugins/DynamicTerrain` directory is a generated source copy that excludes the example itself; it never links back into an ancestor. Edit top-level plugin sources and rerun the example build to refresh this copy.

## Configuration and compatibility

The actor exposes the existing Xovium origin, radius, geometry zoom/cells, imagery detail zoom/radius/texture size, provider URLs, cache directory and material settings. Geometry coordinates remain north/east/up in metres at the native boundary, converted to Unreal centimetres with identical winding, normals and UVs. `LoadSharedConfig(path)` accepts the shared schema-1 `terrain.json` used by Xovium; paths are relative to that file. `TraceGround` reports only active cooked terrain; `IsTerrainReady` remains false until the first safe activation.

Empty cache configuration uses `XOVIUM_TERRAIN_CACHE`, otherwise `~/.cache/gz_dynamic_terrain`; custom providers use a project-local cache unless configured explicitly. Provider templates support the shared core's `{z}`, `{x}`, and `{y}` placeholders. No resolution, coverage, shading or physics-rate reductions are used as performance shortcuts.

The native interface is plain C, isolating Unreal's libc++ from system OpenCV's libstdc++. ABI v3 retains owned frames and adds coverage revisions and visual-only page flags, so distant geometry is excluded from collision; legacy `xov_terrain_*` callbacks remain supported. Frame data is immutable and survives destruction of the runtime; release each acquired frame. A page's borrowed scratch pointers remain valid until the next page read or frame release, so copy each page before advancing. Runtime calls belong to the host thread, while frame preparation can run on a worker. JPEG decoding uses an isolated libjpeg namespace because Unreal exports a different JPEG ABI.

Xovium's `APX4Terrain` remains a thin compatibility subclass with its original serialized class name. It retains PX4 home-environment updates and the existing shared-project material preference. Terrain acquisition, rendering and collision are owned by this plugin. The PX4 game mode waits asynchronously for valid collision before spawning its vehicle.

The public `Terrain` component retains authored physical-material overrides, object type and channel responses. Runtime edits propagate to active and staging collision before physics; refinement also updates collider material slots, including a reused material instance whose inherited physical material changes. Use the actor's `SetActorEnableCollision` to enable or disable terrain collision. The public visual component itself remains `NoCollision`; direct component-level collision enable toggles do not control the separate collision bodies.

## Performance and stability

- Mesh conversion and the exact sRGB box-filter mip pyramid run on one persistent preparation worker per active actor. Internal image-processing parallelism is preserved.
- Same-size refinement reuses texture objects and material instances, updating every mip together in one render command. CPU mip data remains coherent for resource recreation.
- Overlapping pages backed by the same immutable cached image reuse their existing pixels without copying CPU mips or uploading them again. Weak native identity tokens retain no decoded image payload; newly decoded images, including changed imagery at the same zoom, take the full update path. Mesh and material binding still runs, and complete prepared mips remain available if a texture resource needs recreation. Diagnostics expose `ReusedImageryPages`, `SkippedTextureUploadBytes`, `TextureUploadPages` and `TextureUploadBytes` (source mip bytes, not padded driver allocations).
- Replaced and departed tile textures retire automatically. Cleanup waits for the next engine frame and render fences before releasing GPU resources and CPU mip storage. The weak cleanup ticker also drains a stopped editor preview; actor destruction hands any unfinished resources to Unreal's normal texture lifecycle. Active textures retain their complete mip chains.
- Texture updates are coalesced by generation and page; ready pages are not lost when the host is temporarily busy.
- One stable visual component and two reusable physics-only components bound component count. Physics components create no GPU scene proxies.
- Each generation cooks its original aggregate triangle mesh once, asynchronously in game worlds. Existing collision and imagery remain until a successful replacement is committed before physics. Editor previews use the same exact mesh with synchronous cooking.
- Activation checks cooked collision and a ground trace; failed or obsolete candidates retain current ground. Shutdown cancels preparation and releases native workers before the actor is destroyed.
- After a replacement passes its ground trace, the inactive collision component releases its old triangles, cooked data and material references. The two reusable components and the public visual component retain their existing roles.

The design bounds active/staging generations and resident page resources. Texture resolution, imagery detail, mesh density, filtering, physical materials and collision triangles are preserved. Diagnostics expose active generation, page count, texture updates, cook count and game-thread commit time, plus download queue/connection/retry counters, decoded-cache bytes, coalesced pending pages and resident textures.

Cleanup performs at most eight retirement steps or approximately two milliseconds of work per engine frame, without a global garbage collection or render flush. Admission of another prepared batch pauses above 128 MiB of retired CPU mips or 512 retired textures while the existing terrain remains active. These are admission thresholds: one atomic replacement can exceed them, but a stalled renderer cannot accumulate repeated replacement batches. `RetiredTextureBytes` and `PeakRetiredTextureBytes` count CPU mip storage only; `ReleasedTextureBytes`, `ReleasedTextures` and `CleanupBackpressureFrames` expose reclamation and waiting. Unreal normally collects the small empty UObject shells later. Engine, allocator and driver caches can retain freed pages, so process RSS need not fall by the same amount as released texture storage.

The dedicated preparation worker avoids spreading large temporary mesh and mip allocations across idle global-pool thread heaps. A matched 320-second Linux UE 5.6.1 rendered test used 610.5 MiB less RAM during final idle, with identical terrain, active mip storage and downloads; frame p95 changed from 11.65 to 11.78 ms and p99 from 14.68 to 14.62 ms. This short test does not establish long-session memory stability. The worker keeps background OS priority; nested image tasks use Unreal's default task priority rather than inheriting the global pool's background task priority. Stop cancels and drains preparation before joining the worker. No allocator purge or forced garbage collection is used.

Unreal 5.6.1 also has a separate Vulkan split-barrier event leak. [Epic documents the workaround and engine fix CL 47756075](https://forums.unrealengine.com/t/memory-leak-in-ue-5-6-in-linux/2684392); source inspection confirms the affected path in the locally installed 5.6.1 engine. The example enables the workaround. For an existing project, add this setting to its `Config/DefaultEngine.ini` (or merge the line into an existing `[SystemSettings]` section):

```ini
[SystemSettings]
r.Vulkan.AllowSplitBarriers=0
```

This changes Vulkan synchronization scheduling; terrain resolution, coverage, complete mip chains, materials and physics settings remain unchanged. It does not patch engine binaries. Restart Unreal after applying it: the setting cannot recover events already lost by a running process. Reassess the workaround when upgrading to an engine containing the fix; it is specific to the affected Vulkan implementation, not a general memory limit or garbage-collection policy.

## Distant coverage and camera turns

`OuterVisualRadiusM` adds a balanced, stitched quadtree outside the existing near terrain. Standalone actors keep the legacy default of zero; Xovium shared configuration defaults `visual_outer_radius_m` to 2,000,000 metres for flights up to 30 km AGL. A value of zero disables the additional layer. Nearby vertices, normals, UVs, textures and collision triangles are unchanged. Distant pages are visual only; they do not enlarge the collision footprint or increase collision cooking work. Keep terrain near the followed vehicle for landings.

Startup publishes nearby terrain first, then adds distant pages within a per-tick upload budget. `IsDistantTerrainReady()` reports completion of that first outer view. Subsequent recentering retains the old complete view until its replacement and near collision are ready. Failed downloads retry without removing active terrain. Waiting for distant readiness before a high-altitude camera test avoids confusing startup with a streaming hole.

The shared runtime reuses immutable distant meshes in a byte-limited 128 MiB cache and pins the active imagery. Diagnostics expose mesh bytes, limit, entries, hits and misses. Mesh vertex attributes, section bounds and index filtering are prepared on a worker using the same values as Unreal's procedural mesh implementation; the existing visual component remains the public component. Pixel decoding and the complete mip pyramid are unchanged. This is regular procedural terrain with bounded page streaming, not a Nanite or virtual-texture conversion.

For the two-camera 30 km regression, create an isolated project with `python3 tests/coverage/prepare.py /tmp/terrain-coverage`, build its `TerrainDemoEditor` target, then run `python3 tests/coverage/run.py --engine /path/to/UnrealEngine --project /tmp/terrain-coverage/TerrainDemo.uproject --output /tmp/terrain-coverage-results`. It checks front/bottom views during banks, yaw changes and recentering, exact prepared-section parity, and one near collision cook per generation.

## Validation

These commands require a repository checkout; tests are excluded from releases.

```sh
bash scripts/build_native.sh
python3 tests/test_native_frames.py
python3 scripts/run_mip_test.py --ue-root /path/to/UnrealEngine
python3 scripts/run_actor_tests.py --ue-root /path/to/UnrealEngine --output-dir build/actor-tests
python3 scripts/run_soak.py --ue-root /path/to/UnrealEngine --seconds 3600 \
  --output-dir /tmp/terrain-soak --https --outage-every 97 --outage-for 8
```

The frame test checks owned-buffer lifetime, invalid generation/index handling, JPEG decoding and shutdown. Native decoder tests reject incomplete JPEG files instead of caching reconstructed pixels. The mip test compares dimensions, byte counts and hashes of every mip at 256, 512 and 2048 pixels against the original synchronous algorithm inside Unreal's actual `FImageCore` implementation. Build the example before running the engine tests. The soak creates a private local JPEG/PNG fixture server, optionally with its own TLS trust anchor, moves the camera through a repeatable route and injects server outages. It checks active ground every frame after initial readiness, and records generations, cook count, memory, cache/queue diagnostics and frame timings in `summary.json`. Frame percentiles use a bounded 0.01 ms histogram after a 15-second warmup, with a final overflow bin at 1000 ms. Add `--sample-resources` to record RSS and per-process NVIDIA NVML VRAM every second in `resources.jsonl`. `--null-rhi` is available for CPU/collision diagnostics and does not count as rendered acceptance. Tests use an isolated example project and do not modify installed simulator projects.

Python fixture helpers require Pillow (`python3-pil` on Ubuntu). The actor suite uses offscreen Vulkan because texture-resource reuse cannot be tested with NullRHI. The actor-test helper runs all five `Xovium.DynamicTerrain` automation tests with a private loopback PNG fixture server. `ImageryIdentity` checks exact complete mip preservation, changed imagery at the same zoom, material rebinding, resource recreation and Stop cleanup. `TextureRetirement` verifies resized-texture retirement without collecting garbage, exact active mip preservation, material-parent replacement, stop, and destruction with a pending retirement fence. The suite also covers resident imagery, mip parity and streaming controls. The isolated rendered memory fixture additionally measures normal-GC object counts, retired mip storage and contact continuity while recentering; forced-GC diagnostic runs do not establish production cleanup latency.

`Xovium.DynamicTerrain.StreamingControls` exercises Stop across repeated actor ticks in private game and editor worlds, disabled startup and Refresh, explicit restart, an observed disable/enable cycle, and the editor preview checkbox. It uses isolated caches and loopback providers and leaves class defaults unchanged; the separate lifecycle fixture covers outstanding asynchronous work.

`tests/prepare_lifecycle_project.py /tmp/terrain-lifecycle` creates an independent test project for actual pending Chaos cooks. Build its `TerrainDemoEditor` target, then pass its `.uproject` to `run_soak.py --project ... --null-rhi`. This test stops and destroys actors during outstanding cooks, collects garbage, restarts terrain, and checks that current ground and updated custom material parents remain correct. `tests/serialized_compatibility.py` supplies old-save/new-load commandlets for projects containing the original and compatibility `APX4Terrain` classes.

`tests/collision_settings/prepare.py /tmp/terrain-settings` prepares another isolated `TerrainDemoEditor` target. After building it, run `tests/collision_settings/run.py --ue-root ... --project /tmp/terrain-settings/TerrainDemo.uproject --output /tmp/terrain-settings-results`. It verifies actual Chaos hit materials, live object/channel edits, edits made during pending cooks, material replacement and inherited physical-material changes without recentering, and actor collision disable across a recenter.

For the optional Xovium integration, `tests/px4_startup/run.py --ue-root ... --project /isolated/PX4Unreal.uproject --output-dir ...` adds a test-only derived game mode to an isolated integration project. It verifies that game ticks continue during an eight-second provider delay and the real production vehicle spawns only after valid ground. Add `--unavailable` to verify the original 120-second deadline, continued ticking and absence of a vehicle with a provider returning HTTP 503 throughout.

## License

BSD 3-Clause, Copyright (c) 2026 Xovium Tech. See `LICENSE` and `native/vendor/terrain/LICENSE` for the shared core's BSD license. `source-manifest.json` identifies the canonical source snapshot used in a packaged release.
## Recenter imagery continuity

The shared native runtime prepares replacement outer pages at their configured
final imagery LOD before publishing them. The active terrain stays visible during
downloads or failures, preventing the temporary coarse-then-sharp replacement
seen when the quadtree changes. First-start loading, final image resolution,
mesh generation and collision behavior retain their existing settings.

`python3 tests/test_native_frames.py` exercises three recenters through the actual
C ABI, checking every replacement outer page before activation. Actor automation
also checks that same-key finer textures and their complete mip chains survive
replacement.

