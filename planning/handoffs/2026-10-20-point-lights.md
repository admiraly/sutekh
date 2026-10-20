# Dynamic point lights: issue #86

## Scope and state

Implemented in isolated branch `feat/point-lights`, based on
`7e3c6ebda67f236fc1ccfee22d5e0b048b190ba6`. Local checks passed; CI/integration pending.
Issue prose was rewritten to describe the technical request generically.
The owner authorized the render contract extension and integration.

Modified paths:
- `include/sutekh/render.h`
- `src/render/su_render_lights.h`
- `src/render/su_render_lights.c`
- `src/render/su_render_vk_int.h`
- `src/render/su_render_vk.c`
- `src/render/su_render_scene.c`
- `src/render/shaders/frame.glsl`
- `src/render/shaders/point_lights.glsl`
- `src/render/shaders/pbr.vert`
- `src/render/shaders/pbr.frag`
- `src/render/shaders/paint.frag`
- `src/render/su_render_shaders.h` (generated, never hand edited)
- `tools/compile_shaders.py`
- `tests/render/test_point_lights.c`
- `tests/render/test_render_paint.c`
- `cmake/SutekhPresentation.cmake`
- `docs/RENDERING_AND_ASSETS.md`
- this handoff and its adjacent task-state JSON

API generation 8 appends a caller-owned list of up to 128 Q32.32 point lights to the frame.
Every visible model node receives a conservative 128-bit sphere-intersection mask, preserving batching.
The shared point-light BRDF uses each material's final normal, albedo, roughness, metalness or RGB F0.
Light radiance is camera-relative; the sun shadow term and ambient AO are independent of these lights.
No descriptor binding, pipeline-layout slot, material flag or public object flag was consumed.
The bounded frame UBO grows by 4096 bytes and the internal instance stride by 16 bytes.
The existing `struct_size` contract requires recompilation, not binary compatibility with old descriptors.

## Checks

Windows 11, i5-12400F, Quadro P600 over RDP, NVIDIA driver 32.0.15.8216,
clang 23.1.3 and MSVC 19.44.35229, Release, Ninja, native specialization disabled.
Environment: dot-source `tools/env-windows.ps1`. Shader compiler: local glslang 16.6.0.

- `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSUTEKH_NATIVE=OFF -DSUTEKH_RENDER=ON -DSUTEKH_ASSET_IMPORT=ON`
- `cmake --build build -j 3`: passed.
- `SUTEKH_REQUIRE_VALIDATION=1 ctest --test-dir build --output-on-failure -j 2`:
  all 119 tests passed, including the new portable light test; two existing platform-only skips.
- `test_point_lights`: zero failures; validates capacity, malformed inputs, packed layout, large-world fractions,
  all four mask words, disabled lights, conservative intersection and large radii.
- `test_render_paint`: P600 and Mesa 25.0.7 lavapipe passed with Vulkan validation enabled,
  including synchronization validation: zero errors and zero warnings.
  Pixel cases include zero-count identity under sun lighting, a BRDF reference value, RGB response,
  inverse-square attenuation, smooth edge, range/back-face exclusion, moving lights, 128-light accumulation,
  highest mask bit, huge common translation, sun equivalence, AO, normal maps, RGB F0,
  alpha/overlay paths, and unlit exclusion.
- MSVC `/W4 /WX`: renderer and both affected tests build and pass on P600.
- `python tools/compile_shaders.py --check --compiler <absolute-glslang-path>`:
  all 28 shaders and three includes recompiled and match the generated header.
- `python tools/check_repository_license.py`: passed.
- `python tools/validate_pack.py --write-report`: passed.
- `git diff --check`: passed.

CI Linux GCC, native-tier and checked runtimes are not claimed locally; they must pass before merge.

## P600 measurements

Reproduce with `SUTEKH_POINT_LIGHT_BENCH=1 test_render_paint --no-validation`.
1280x720 HDR offscreen frame and tonemap, fullscreen quad, two material models, no texture maps,
no ambient, one sun, 40 warm-up frames and 100 timestamp/CPU submission samples per case.
Overlapping: all point lights at (0,0,-1), radius 8, intensity 0.1, quad at z=-3, scale 4.
Culled: same list at x=1000. Counts 0, 8, 32 and 128. The masks preserve draw batching.
CPU times exclude frame-slot waiting, readback and present. GPU values are whole-frame timestamps,
not isolated shader duration. Validation is disabled in the measured renderer.

Final measured executable SHA256:
`3a842b67ed8a1c9685e16ceae8a8a51cb436416955ed506744511a39b12bb560`.
The last subsequent source change adds only another synthetic per-instance pixel regression.
Measured point-light GLSL SHA256:
`cf71a29b5702de4e6800cdc56cfe364028c92d27f8f55f318d0571d1288d9ae8`.
Generated shader header SHA256:
`cd7020865174fc5490193c8e7594b0900e685446a26534a8447549ebfd2a5900`.
Hardware counters/power are unavailable; only Vulkan timestamps and CPU wall time were collected.

| Material | Placement | Lights | GPU median ms | GPU p95 ms | CPU median ms | CPU p95 ms |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| PBR-lite | overlapping | 0 | 2.565760 | 22.567328 | 0.072000 | 0.148100 |
| PBR-lite | overlapping | 8 | 4.104416 | 27.913152 | 0.099800 | 0.151500 |
| PBR-lite | overlapping | 32 | 9.325312 | 41.667552 | 0.129800 | 0.176200 |
| PBR-lite | overlapping | 128 | 31.586944 | 56.443808 | 0.168300 | 0.257800 |
| PBR-lite | culled | 0 | 2.686080 | 23.245376 | 0.066200 | 0.126000 |
| PBR-lite | culled | 8 | 2.629824 | 26.507936 | 0.067100 | 0.126600 |
| PBR-lite | culled | 32 | 2.440480 | 21.679712 | 0.073400 | 0.155700 |
| PBR-lite | culled | 128 | 2.611328 | 18.822752 | 0.112900 | 0.175000 |
| Paint | overlapping | 0 | 2.774912 | 22.705024 | 0.055700 | 0.110400 |
| Paint | overlapping | 8 | 4.209600 | 24.228032 | 0.076100 | 0.129800 |
| Paint | overlapping | 32 | 8.322432 | 33.180448 | 0.096000 | 0.193500 |
| Paint | overlapping | 128 | 32.853088 | 59.023008 | 0.164600 | 0.237400 |
| Paint | culled | 0 | 2.773248 | 18.738528 | 0.069400 | 0.136500 |
| Paint | culled | 8 | 2.772768 | 18.818976 | 0.066700 | 0.122000 |
| Paint | culled | 32 | 2.804256 | 19.309408 | 0.076500 | 0.120100 |
| Paint | culled | 128 | 2.915648 | 22.068512 | 0.105300 | 0.165800 |

These are noisy shared desktop/RDP observations, not an isolated performance ranking or guarantee.
No other validation/build command was started by this worker during the measurement.
An earlier run had lower zero-light medians (PBR-lite 1.443 ms, paint 1.615 ms), demonstrating host variability.
128 fully overlapping lights are deliberately pathological and take about 32 ms; do not infer cheap
full-screen lighting at that count. Spatially localized effects benefit from the node masks and range checks.

## Remaining and next

Optional spotlights, point-light shadows and clustered/tiled culling are deliberately not implemented.
The cap rejects excess lights rather than prioritizing or truncating them; hosts choose their visible budget.
Next: CI, merge latest main if needed, regenerate shaders after any source merge, bump the API once relative
to that final base, and merge only a green, mergeable PR. Remove the worktree only after unlinking its two junctions.
