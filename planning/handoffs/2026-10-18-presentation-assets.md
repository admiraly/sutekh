# Handoff: assets, PBR-lite rendering, large scenes, effects, HUD, audio

Branch `feat/presentation-assets`, based on `feat/presentation-input` at `8c30b31`; `origin/main` merged twice
(PR #31 at `15b0dd7`, then PR #32 sim-runtime at `ef610d5`; `PACK_VALIDATION.json` regenerated, not merged).
Draft PR: https://github.com/admiraly/sutekh/pull/33. Authorization and scope: the second extension of
`planning/amendments/2026-10-18-downstream-host-work.md` (owner-authorized downstream work; no M3 gate is closed;
M5 is not closed). Machine: i5-12400F, 16 GB, Windows 11 Pro 26200, clang 23.1.3 with the MSVC 14.44 libraries,
NVIDIA Quadro P600 2 GB (driver 582.16, Vulkan 1.4.312), reached over RDP.

Tools reused from the previous stream, outside the repository: glslang 16.6.0 (`C:\Users\dev\sutekh-scratch\
glslang-16.6.0\bin\glslang.exe`) and the Khronos validation layer built from source (`...\sutekh-scratch\
vvl-build\layers`, loaded with `VK_ADD_LAYER_PATH`). The Vulkan SDK is still not installed (its installer needs a
UAC elevation nobody can approve in this session).

## 1. Asset pipeline (`include/sutekh/asset.h`)

- **Pack format SUAPAK v1** (`src/asset/su_asset_pak.c`, library `su_asset`, always built, no third-party code):
  64-byte header (magic, version, entry count, total size, SHA-256 of everything after the header = the pack's
  address), a sorted entry table (stable 64-bit id, type, type version, 16-byte-aligned offset, size, payload
  SHA-256), payloads. Types: mesh (56-byte vertices: position, normal, tangent with sign, two UV sets; u32
  indices; submeshes with material id and bounding sphere), texture (RGBA8 / sRGB, BC1/3/4/5/7, up to 16 mips),
  material (metallic-roughness, emissive strength, alpha mode, double-sided, unlit, five texture slots with UV set
  and wrap modes), model (placed nodes with up to four LOD meshes and screen-size thresholds, bounds).
  `su_asset_pak_open` checks every header field, hash, payload layout (counts, ranges, index bounds, finite
  values, mip sizes) and cross-reference before returning zero-copy views. Ids come from names
  (`su_asset_id`: first 8 bytes of SHA-256 of `"sutekh.asset.v1:" + name`), so a re-cooked source keeps its ids.
  A writer (`su_asset_writer_*`) builds packs (the importer and the demo's procedural content use it); a
  one-thread background loader (`su_asset_loader_*`) reads, optionally cooks, and validates off the frame thread.
- **glTF importer and cook step** (`src/asset/su_asset_gltf.c`, library `su_asset_import`): cgltf 1.15 and
  stb_image 2.30, fetched by `tools/bootstrap_asset_deps.py` from pinned commits and refused unless every file's
  SHA-256 matches the pins in the script (lock file `third_party/asset-deps.lock.json`; files gitignored, not
  vendored; listed in `THIRD_PARTY_NOTICES.md`). Without them `su_asset_import` is an honest UNSUPPORTED stub and
  the pack runtime still builds. Supports .gltf (external and data-URI buffers and images) and .glb; triangles,
  strips, fans (lines/points skipped with a warning); flat normals when missing (the glTF rule); tangents from UV0
  when missing; the default scene flattened to world transforms; `<base>_LOD<n>` node names grouped as LODs;
  mirrored nodes get a winding-flipped mesh variant (`#mirrored`); PNG/JPEG decoded with sRGB-correct mips
  (normal maps renormalized); KTX2 RGBA8/BCn without supercompression (Basis/zstd refused with a warning, slot left
  empty); image URIs only next to the source (no `..`, no absolute paths). Ignored with warnings: animation,
  skins, morph targets, KHR_texture_transform, lights, Draco.
- **Cook tool** `asset_cook cook IN.gltf OUT.supak [--name N] [--max-texture N] [--no-mips]` and
  `asset_cook info PACK` (prints the content SHA-256 and every resource).
- **Asynchronous upload** (renderer, item 2 below): resource creation validates and allocates at once; bytes are
  copied by later frames through per-frame staging within `upload_budget_bytes` (default 16 MiB per frame);
  textures go in whole block rows, so one mip can span frames; nothing waits on the queue.

| Command | Result |
|---|---|
| `python -I tools/bootstrap_asset_deps.py` then `--check` | 5 files fetched, hashes match the pins; `asset deps ok (5 files)` |
| `build-win\test_asset_pak.exe` | 103,519 checks, 0 failures: id vectors computed independently with Python hashlib; round trip and byte-identical output whatever the insertion order; every one of 4,336 single-byte corruptions refused; 10 forged structural corruptions (hashes recomputed) refused with the expected reason; 3,000-pack fuzz (1,397 layout-valid accepted, 1,603 refused, no out-of-range view); borrowed and file opens; loader statuses (OK, PARSE, IO, UNSUPPORTED), bounded queue |
| `build-win\test_asset_gltf.exe build-win\asset_gltf_work` | 149 checks, 0 failures: the test writes .gltf + .bin + PNG, GLB with an embedded PNG, a data-URI buffer, KTX2 RGBA8, BC1 with mips and a supercompressed KTX2; hierarchy flattening, materials and samplers, generated tangents (direction and sign), sRGB-correct mip (red/green average = 188, not 128), normal-map renormalization, flat normals for strips and fans, LOD grouping, mirrored variants, stable ids across a re-cook, determinism, truncated GLB, out-of-range index (refused by cgltf_validate), missing file, escaping URI |
| `asset_cook cook ...scene_c.gltf c.supak` / `info` | 4 meshes, 1 LOD group, content SHA-256 printed and re-validated |

## 2. Materials, lighting, scale (`include/sutekh/render.h`, API version 2)

- **Resources**: `su_render_mesh_create`, `_texture_create`, `_material_create`, `_model_create`,
  `su_render_resource_state`, `su_render_release`; generational 64-bit handles (type, generation, index);
  release bumps the generation at once and moves the Vulkan objects to a retirement list destroyed only after the
  last frame submitted before the release has completed (frame serials and fences). Material descriptor sets use
  white / flat-normal defaults while textures stream and are replaced (old set retired) when they become ready.
  `su_render_upload_pak` / `su_render_pak_state` / `su_render_pak_handle` / `su_render_pak_release` handle a whole
  pack.
- **Frame**: `su_render_frame_ex(SuRenderFrameDesc)`: camera and objects at Q32.32 `SuWorldPos`; the camera is
  subtracted in integers before anything becomes a float. Reversed-Z infinite projection with D32. Per-object and
  per-node sphere culling, LOD by projected size (falls back to the nearest READY LOD), node x submesh items sorted
  by pipeline/material/mesh (blend back to front) into instanced draws. Scene pass into RGBA16F: procedural sky
  (gradient, sun glow and disc, hashed stars), PBR-lite (GGX + height-correlated Smith + Schlick, Lambert, one sun,
  IBL-lite from the sky with an analytic environment BRDF, tangent-space normal maps, occlusion, emissive,
  mask/blend, unlit), debug meshes and lines, additive effects. Output pass: ACES fitted (or clamp) into the sRGB
  swapchain or offscreen image, then the HUD. `su_render_frame` (the first slice) still works and renders the same
  pixels through the same passes (clamp tonemap, converted depth).
- **Pipeline cache**: `pipeline_cache_path` in `SuRenderDesc`; own header (vendor, device, driver version,
  pipelineCacheUUID, size, SHA-256), rejected and rebuilt when anything differs; saved atomically on destroy. All
  pipelines are created at `su_render_create`, never in a frame.
- **Portable helpers** (`su_render_util`, no GPU): `SuWorldPos` (`su_wpos`, `_delta`, `_lerp`, `_add`),
  `su_mat4f_perspective_rz`, `su_frustum_*`, `su_lod_select`, quaternion helpers, effects and HUD builders.
- Shaders: `src/render/shaders/*.vert|frag` plus the shared `frame.glsl`; `tools/compile_shaders.py` now records
  the include's hash too (13 sources, 1 include).

| Command | Result |
|---|---|
| `python -I tools/compile_shaders.py --check --compiler <glslang 16.6.0>` | shaders: ok (13 sources, 1 include, recompiled, identical SPIR-V) |
| `build-win\test_render_portable.exe` | 84 checks, 0 failures (1 mm at 10,000 km: camera-relative 0.0010000 m, naive float32 0.0000000 m; reversed-Z depths; frustum planes incl. a tangent sphere; LOD thresholds; trails, billboards, HUD layout, font atlas) |
| `build-win\test_render_offscreen.exe` | 23 checks, 0 failures; same pixels as the first slice; draw count now 5 (the tonemap draw) |
| `build-win\test_render_assets.exe build-win\render_assets_work` (Debug and Release) | 101 checks, 0 failures each: pack uploaded in 6 frames at a 256 KiB budget (1,461,796 bytes), nodes pending until READY; camera 1,000 km from the origin; exact emissive pixels (0, 255, 0); a 0.4-scale cube 2 m right at 10 m on the pixel doubles predict; LOD 0/1 switch; sRGB checker bytes kept to +-2; mirrored node with the flipped mesh lit like the original, the unflipped one culled to its dark inside; ACES of 1.0 = sRGB 231; lit front vs back; sky up is blue; 2,000-object ring: 337 drawn, 1,663 culled, one instanced draw; HUD rect and text pixels; billboard hot centre and soft edge; release: stale handle refused, 0 resources and 0 retired pending after 3 frames; pipeline cache absent (3), loaded (1), corrupt byte rejected (2) |
| CI run 39137189595 (commit `a13974f`): all five jobs passed | Linux checked runtime (GCC, Mesa lavapipe): `render_offscreen`, `render_assets` (rendered and pixel-checked on a CPU Vulkan device), `asset_pak`, `asset_gltf`, `render_portable`, `render_font_fresh`, `audio_mixer` passed, `audio_device` skipped (no backend); Windows checked runtime: the same, with `render_offscreen`, `render_assets`, `demo_offscreen_rollback` and `audio_device` skipped (no Vulkan device or audio device on the runner) |

## 3. Effects and HUD

`su_trail_*` (ring of Q32.32 points pushed from interpolated transforms; the head follows the emitter until it is
`spacing` away; lifetime fade; camera-facing ribbon built camera-relative), `su_fx_billboard` (round soft spot),
`su_fx_beam`; drawn additively into the HDR target with depth test, no depth write. HUD: `su_hud_begin`,
`_rect`, `_text`, `_textf`, `_measure`; a project-authored 5x7 font (`tools/gen_font.py`, glyphs drawn by hand on
a grid, ASCII 32..126; `render_font_fresh` checks the generated header), alpha-blended after tonemapping with
sRGB-encoded vertex colours decoded in the shader.

## 4. Audio (`include/sutekh/audio.h`, library `su_audio`)

Clips: WAV (8/16/24/32-bit PCM, float, extensible), Ogg Vorbis through stb_vorbis 1.22 (same bootstrap), generated
tones and noise. Mixer: preallocated voices with generational handles; the game thread sends commands through a
lock-free single-producer single-consumer ring and learns about finished voices through a second ring; the render
side (device thread or offline caller) never allocates, locks or blocks. Equal-power panning in listener space,
`(min/d)^rolloff` attenuation with a max distance, Doppler `(c + v_listener) / (c - v_source)` clamped to [0.25,
4], linear resampling, per-block gain ramps, fades, loops, priority stealing (oldest of lower or equal priority),
clamp statistics. WASAPI shared mode, event-driven, float32 stereo with AUTOCONVERTPCM; other platforms report no
backend and keep the offline mixer. `SuAudioEventCursor` passes each host event sequence number once.

| Command | Result |
|---|---|
| `build-win\test_audio.exe tests\fixtures\audio\tone440_stereo.ogg` | 220 checks, 0 failures: unity-rate output equals the clip sample for sample; 24 kHz clip measured 1000.00 Hz, pitch 1.5 = 1500.00 Hz; front 0.5000 RMS each side, right 0.7071 / 0, 4 m / 1 m = 0.2500; Doppler approaching 555.56 Hz and receding 454.55 Hz for a 500 Hz source (exact expectations 555.56 / 454.55), listener motion (c + v)/c; loops, one-shots, 10 ms fades, stale handles, stealing and refusal, full command queue (4 refused, nothing corrupted), clamp counting, byte-identical repeated runs; WAV formats and refusals; Ogg: 22,050 frames stereo 44.1 kHz, RMS 0.3561 / 0.1779 (expected 0.3536 / 0.1768), 176 cycles in 0.4 s, 440 Hz after resampling to 48 kHz; event cursor across a re-emitted sequence |
| `build-win\test_audio_device.exe` | device "Remote Audio" (the RDP endpoint), 48 kHz, buffer 1,124 frames; 22,278 frames rendered by the device thread in about 0.46 s while a spatial voice swept left to right; the stopped voice ended. The test cannot hear; it checks the stream ran |

The Ogg fixture is a synthetic 440 Hz signal computed by `tools/gen_audio_fixture.py` and encoded with
python-soundfile 0.14.0 / libsndfile 1.2.2 in a throwaway environment outside the repository (tool listed in
`THIRD_PARTY_NOTICES.md`; not a dependency).

## 5. Demo (`examples/presentation/space_demo.c`, `demo_content.c`)

Draws through `su_render_frame_ex` (`--legacy` keeps the first-slice frame): a procedural pack (ship, gate with
light strips, a spinning station, asteroids with two LODs) cooked with the writer at start and uploaded
asynchronously; `--debris N` instanced asteroids (default 3,000); ribbon trails from each ship's interpolated
tail; a boost flare on the boost button (presentation only); a gate burst and a brighter gate emission on the gate
event; a HUD (speed, gates, time, renderer telemetry); `--world-offset KM` moves the whole presentation; `--model
FILE` loads a local .gltf/.glb (cooked on the loader thread) or .supak and places it; `--pipeline-cache FILE`.
Audio: an engine loop following the player's interpolated transform and a chime per gate event through the event
cursor; the rollback drills feed every re-emitted event through the cursor and require that none plays again;
without a device the mixer runs offline per frame. The simulation is untouched: `demo_headless_rollback` and
`demo_offscreen_rollback` still reach the pinned digest `fa02b76e...defd`.

| Command (Debug unless noted) | Result |
|---|---|
| `space_demo --offscreen ... --frames 240 --rollback-check --expect-digest fa02...defd` | digest matches; 59 drills, 0 differed; gate sounds 1 for 1 gate event, re-emitted events let through 0; shadow world 0 differing digests; image `planning/evidence/presentation-assets/space-demo-assets.png` |
| `space_demo --headless --script --frames 240 --rollback-check --expect-digest ...` | digest matches; audio path as above (offline) |
| `space_demo --script --seconds 5 --model build-win\asset_gltf_work\scene_a.gltf --model-scale 3` (window over RDP) | model loaded and validated in 0.6 ms on the loader thread, uploaded, placed; FIFO paced at 32 fps by the remote display (wait about 30 ms in acquire), 120 ticks/s, 0 dropped |
| `space_demo --script --frames 140 --exercise-window` | 3 swapchain recreations, 0 failed frames |
| `ctest --test-dir build-win -j 8` | 92 passed, 0 failed, 2 skipped (`path_fifo`, `containment`, POSIX-only) of 94 |
| `python -I tools/validate_pack.py --check-report`, `python tools/check_repository_license.py` | passed, passed |

## 6. Validation layer

`planning/evidence/presentation-assets/validation-runs.log` (Debug, source `76e63ac`): with
`VK_KHRONOS_VALIDATION_VALIDATE_SYNC` false and true, over `test_render_offscreen`, `test_render_assets`, the demo
offscreen with rollback drills, the window exercise (resize, minimize, restore) with a glTF model loading and
uploading during it, a MAILBOX run and a `--legacy` run: **0 errors, 0 warnings in all 12 runs**.

## Measurements (observations, not qualification)

Quadro P600 over RDP (not the Arc A770 target); timestamps around the whole frame command buffer; medians and p95
over the frames after a 30-frame warm-up; `planning/evidence/presentation-assets/runs.log`.

| Run (Release unless noted) | GPU median (p95) | Render CPU median (p95) |
|---|---|---|
| 1920x1080 offscreen, 3,000 asteroids (1,515 drawn after culling) | 1.430 ms (1.437) | 0.198 ms (0.266) |
| 1920x1080 offscreen, 10,000 asteroids (4,960 drawn) | 1.846 ms (1.866) | 0.493 ms (0.659) |
| 1920x1080 offscreen, 20,000 asteroids (9,807 drawn) | 2.376 ms (2.399) | 0.886 ms (1.116) |
| 1920x1080 window, MAILBOX, 10,000 asteroids, chase camera | 0.711 ms (1.266) | 0.356 ms (0.498) |
| 1280x720 window, Debug, MAILBOX, `--world-offset 1000` | 0.411 ms (0.566) | 0.259 ms (0.382) |

Other observations: pipeline creation 6.6 to 8.8 ms without a Sutekh cache file (the NVIDIA driver keeps its own
shader cache on disk, so this is not a cold compile; the first run of the session measured 71.5 ms), 0.58 to 0.77
ms with the Sutekh cache loaded. Scene building (culling, LOD, batching, qsort) 0.14 / 0.54 / 0.93 ms for 3,000 /
10,000 / 20,000 objects (Release). The 1080p 60 FPS floor is not at risk in these scenes on this GPU; a scene made of
large converted meshes was not measured (none available here).

## Not run or not done

- No downstream glTF content and no Khronos sample model were used (none present locally; none fetched). The
  importer is tested on the files the tests generate; a large real model's cook time, memory and upload frames are
  unmeasured.
- Basis Universal / zstd-supercompressed KTX2 (needs a transcoder), BC encoding of PNG/JPEG at cook time (images
  are cooked as RGBA8 with mips), KHR_texture_transform, animation, skinning, morph targets, punctual lights.
- GPU memory sub-allocation: every buffer and image has its own allocation (fine for thousands, not for
  `maxMemoryAllocationCount`-scale content). Uploads use the graphics queue (no dedicated transfer queue).
- Shadows, bloom, MSAA, clustered lights, a cubemap sky (procedural only), GPU-driven culling, GPU particles (trails
  and billboards are CPU-built).
- Pipelines are prepared at creation; there is no asynchronous preparation of new variants after start (there are
  no variants after start yet).
- Audio: no HRTF or occlusion, no streaming (clips are decoded fully), no audio device on Linux; the device test
  ran through the RDP audio endpoint only. Voice pitch cannot be changed after start (Doppler and the clip rate are
  applied; `su_audio_voice_update` carries position, velocity and gain).
- No Arc A770 run; frame pacing over RDP is the remote display's.
- Window presentation on Linux (offscreen only there, as before).

## Contract notes for the integrator and the other stream

- New public headers: `include/sutekh/asset.h`, `include/sutekh/audio.h`; `render.h` grows to API version 2
  (additive; `SuRenderDesc` accepts the first-slice size via `SU_RENDER_DESC_V1_SIZE`; `SuRenderStats` grows at the
  end; `render.h` now includes `asset.h`). `SuRenderStats.draws` counts the tonemap draw.
- No change to `host.h`, `input.h`, the IR, the VM, the scheduler or space3d. The demo reads Q32.32 positions with
  `su_host_read_i64` and uses the raw values as `SuWorldPos`.
- Shared files touched: `cmake/SutekhPresentation.cmake` (this stream's), `.github/workflows/repository-checks.yml`
  (one bootstrap line in each checked-runtime job and `-DSUTEKH_ASSET_IMPORT=ON`), `tools/validate_pack.py` (skips
  the fetched `third_party/cgltf` and `third_party/stb`), `tools/compile_shaders.py`, `.gitignore`,
  `THIRD_PARTY_NOTICES.md`, `docs/IMPLEMENTATION_STATUS.md`, the amendment.
- CI note for the sim-runtime stream: `native_avx2` (Linux native-tier job) failed on two consecutive runs of this
  PR at `test_native_avx2.c:1057` ("upper YMM state ... probe NOT valid: dirty state not observed") and passed on the
  re-run of the same commit; it looks runner-dependent. Not touched here.
- `PACK_VALIDATION.json` must be regenerated on every merge (`tools/validate_pack.py --write-report`).

## Next

1. Cook a real downstream model locally (outside the repository) and record cook time, pack size, upload frames
   and frame times; tune `upload_budget_bytes` and the LOD thresholds from that.
2. A GPU memory sub-allocator and a dedicated transfer queue when content grows; BC7 encoding at cook time.
3. Bloom (emissive station lights and engines would benefit), shadows from the sun, MSAA or TAA.
4. Audio: pitch/parameter updates after start, streaming for long music, a Linux backend.
5. A physical-console run on the Arc A770 (frame pacing, GPU timestamps, present modes).
