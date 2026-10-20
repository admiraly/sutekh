# Issue 65: instanced exhaust flame volumes

## Implemented

- Public API version 4: `SuRenderFlame`, `SuRenderFrameDesc.flames/flame_count`,
  `SuRenderTextureDesc.depth`, and drawn/skipped/run counters.
- Instanced additive, depth-tested, no-depth-write volume rendering in the existing
  depth-input subpass. One draw per contiguous texture triple, capped at 4096 instances.
- 2..12 midpoint ray samples (0 selects 6); gradient colour/radius, shape strips,
  actual trilinearly sampled 3D noise, animated displacement, throttle length/gate,
  soft opaque-depth intersections and near-plane/camera-inside handling.
- New GLSL and regenerated embedded SPIR-V; numeric/handle/dimension validation.
- Generic issue description, synthetic fixtures only, no external material/content.
- API contract and example: `docs/FLAME_VOLUME.md`.

Modified implementation paths: `include/sutekh/render.h`,
`src/render/su_render_vk.c`, `src/render/su_render_vk_int.h`,
`src/render/su_render_res.c`, `src/render/su_render_flame.c`,
`src/render/shaders/flame.vert`, `src/render/shaders/flame.frag`,
`src/render/su_render_shaders.h`, `tools/compile_shaders.py`,
`cmake/SutekhPresentation.cmake`, `tests/render/test_render_flame.c`.
Documentation and evidence paths are the standalone flame-volume document,
this handoff and `planning/evidence/flame-volume/`.

## Verified locally

Windows 11, LLVM 23.1.3, glslang 16.6.0, NVIDIA Quadro P600 over RDP.
Also compiled the renderer and new tests with MSVC 19.44 at `/W4 /WX`.
The implementation was rebased onto origin/main before final checks.
Exact source and executable SHA-256 hashes are in `fingerprints.json`.

Commands after `. .\tools\env-windows.ps1`:

- `python tools/compile_shaders.py --compiler <absolute local glslang.exe>`: generated successfully.
- `cmake -S . -B build-flame -G Ninja -DCMAKE_BUILD_TYPE=Release`: configured.
- `cmake --build build-flame -j 6`: whole default build passed.
- `ctest --test-dir build-flame --output-on-failure -j 4`, with
  `SUTEKH_REQUIRE_VALIDATION=1`: 113 tests, 0 failures; path_fifo and containment
  skipped on Windows. Repeated final run: 34.05 seconds.
- `build-flame/test_render_flame.exe`: P600, 0 failures, validation enabled,
  **0 errors, 0 warnings**.
- Same executable with `VK_DRIVER_FILES` selecting local Mesa 25.0.7 lavapipe:
  llvmpipe LLVM 20.1.6, 0 failures, validation enabled, **0 errors, 0 warnings**.
- `cmake --build build-flame-msvc --target test_render_flame -j 4` and its test:
  passed `/W4 /WX`; P600, 0 failures, validation enabled, 0 errors/warnings.
- `python tools/compile_shaders.py --check`: 25 sources and 1 include current.
- `python tools/validate_pack.py --check-report`: passed locally before staging
  the evidence JSON. CI caught its newly tracked file count (427 -> 428);
  regenerated `PACK_VALIDATION.json` using `--write-report`, then rechecked.
- `python tools/check_repository_license.py`: passed (not a legal/CLA review).
- `git diff --check`: passed.

## Measured, not qualified

Command: `build-flame/test_render_flame.exe --benchmark`.
1280x720, complete offscreen GPU frame timestamps, validation/readback disabled,
20 warm-up frames then 120 samples per case. Flame rectangles are about 25x25
pixels, a non-overlapping grid, all in one instanced draw; synthetic 8x8 maps,
patterned 16-cubed noise. Source, executable and raw driver identity are recorded.
Raw results: `planning/evidence/flame-volume/p600-benchmark.txt`.

| Steps | Instances | GPU median ms | GPU p95 ms | Median minus matching empty-frame median ms |
| --- | --- | --- | --- | --- |
| 2 | 32 | 0.956000 | 3.332160 | 0.128064 |
| 2 | 128 | 1.050688 | 3.246976 | 0.222752 |
| 6 | 32 | 0.994208 | 2.983264 | 0.167040 |
| 6 | 128 | 1.190080 | 3.132608 | 0.362912 |
| 12 | 32 | 1.050496 | 3.057504 | 0.223552 |
| 12 | 128 | 1.389440 | 14.474752 | 0.562496 |

Empty-frame medians: 0.827936, 0.827168, 0.826944 ms respectively.
At 6 steps, CPU submission median/p95 was 0.0425/0.1109 ms for 32 and
0.0545/0.1102 ms for 128 (whole frame, not isolated effect cost).
The differences of medians are indicative incremental costs, not paired timings.
RDP/shared GPU activity gives noisy tails; no fine-grained performance qualification
or full-screen overdraw claim. PMU/memory-bandwidth counters were not collected.

## Deliberate limits and next work

- Normalization follows the issue literally, sum / (path length / N). Quality
  changes constant-field brightness as N squared; the API document gives the
  colour compensation. This is not a physical emission integral.
- 3D uploads support single-mip RGBA8 UNORM, dimensions <=256; no 3D compressed
  textures or pack-format extension. Other existing texture behaviour is unchanged.
- At this record point, GitHub's Linux, native-tier, GCC-container, Windows and
  package gates still need to run on the PR. Final PR status is reported separately;
  this local evidence does not assert remote CI passed.
