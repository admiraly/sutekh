# Analytic haze implementation, 2026-10-20

Task: issue #69, `M5-69`, branch `feat/haze-volume`. Initial base:
`478c7366c2cab7c85b8b1983a4b501d53af2aec9`. Local implementation and validation
are complete; remote CI and merge are integration gates, not implied by these
local results. The issue description was generalized before implementation.

## Implemented

- Public `SuHazeVolume`, `frame.haze/haze_count`, `stats.haze_volumes_drawn`,
  and portable `su_haze_optical_depth` reference.
- Affine spheres/ellipsoids and boxes; uniform-to-polynomial density falloff;
  exact chord integration with Beer-Lambert extinction and source in-scattering.
- Scene-depth clipping, camera-inside handling, additive and premultiplied-over
  output into HDR before bloom. Up to 1024 volumes, caller order, conservative
  projected scissor, no added resource uploads or buffers.
- Optional stylized chord-power response, including the issue's local
  source-brightness/diameter lobe, separate from physical scattering mode.
- Fixed-point world anchors, camera-relative transform inversion, and a
  closest-point ellipsoid intersection avoiding the distant quadratic
  discriminant subtraction. Byte-identical local versus 50-70 km world shifts;
  a separate 50 km distant ellipsoid is pixel-tested too.
- Optional isotropic directional sun and midpoint shadow-map visibility,
  existing single-map/cascaded PCF, including haze bounds in receiver fitting.

Usage with an initialized volume: `frame.haze = &volume; frame.haze_count = 1;`.
Full field semantics and limitations: `docs/ANALYTIC_HAZE.md`.

## Exact modified paths

`include/sutekh/render.h`, `src/render/su_render_haze.h`,
`src/render/su_render_haze.c`, `src/render/shaders/haze.frag`,
`src/render/su_render_shaders.h`, `src/render/su_render_vk_int.h`,
`src/render/su_render_vk.c`, `src/render/su_render_scene.c`,
`tests/render/test_haze_math.c`, `tests/render/test_render_haze.c`,
`cmake/SutekhPresentation.cmake`, `tools/compile_shaders.py`,
`docs/ANALYTIC_HAZE.md`, `docs/RENDERING_AND_ASSETS.md`,
`planning/tasks.json`, `PACK_VALIDATION.json`, this handoff,
`planning/evidence/haze/p600.txt`.

## Verification

Windows 11 Pro 10.0.26200, Intel i5-12400F, Quadro P600, Vulkan 1.4.312,
driver integer 2441347072; RDP session. Toolchain: clang 23.1.3, Ninja, C17
Release (`-O3 -DNDEBUG -Wall -Wextra -pedantic -Werror`); glslang 16.6.0,
Vulkan-Headers 1.4.365 and validation layers 1.4.363.0. Environment initialized
with `. ./tools/env-windows.ps1` in the isolated worktree.

- `python tools/compile_shaders.py --compiler <absolute glslang.exe>`:
  regenerated embedded SPIR-V, not hand-edited.
- `cmake -S . -B build-haze -G Ninja -DCMAKE_C_COMPILER=clang
  -DCMAKE_BUILD_TYPE=Release -DSUTEKH_NATIVE=OFF -DSUTEKH_RENDER=ON
  -DSUTEKH_ASSET_IMPORT=OFF` and `cmake --build build-haze -j 4`: pass.
- `ctest --test-dir build-haze --output-on-failure -j 4`: 113 tests,
  zero failures; POSIX-only `path_fifo` and `containment` skipped on Windows.
- `test_haze_math`: known chords, truncated chords, inside rays, two shapes,
  100 independent dense-integration comparisons, reflected/sheared nonuniform
  transforms at large anchors, invalid inputs: zero failures.
- `test_render_haze`: whole-image CPU-reference comparisons, zero-field
  regression, two shapes/blends, density falloff, rotated/sheared ellipsoid/box,
  camera rotation, large anchors, distant ellipsoid, eye-inside, partially and
  fully depth-clipped volumes, 32 additive/over overlaps, scissor rejection,
  input validation, actual opaque shadow caster and three shadow cascades:
  zero failures. Center red: sun-lit 226, shadowed 144.
- Direct P600 and Mesa 25.0.7 lavapipe runs, also with
  `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`: validation enabled, **0 errors,
  0 warnings**. Mesa selected with `VK_DRIVER_FILES=<local lvp_icd.x86_64.json>`.
- MSVC 19.44.35229 C17 Release `/W4 /WX`: `test_haze_math` and
  `test_render_haze` build and pass, renderer included. MSVC configuration also
  needs the SDK `bin/<version>/x64` on PATH (for rc/mt), in addition to the
  compiler binary directory and the env script's INCLUDE/LIB.
- `python tools/validate_pack.py --write-report`, then `--check-report`, and
  `python tools/check_repository_license.py`: pack/license gates required
  before submission. Remote Linux/GCC/native/Windows gates are tracked by CI.

## P600 observation, not a budget guarantee

Command: `build-haze/test_render_haze.exe --bench`. Raw output:
`planning/evidence/haze/p600.txt`. 1280x720 offscreen RGBA16F scene, output and
readback copy; no meshes/sky/bloom/shadows, validation **off**, unlit additive
ellipsoids all centered four units ahead, radii two units, density 0.01, falloff
one. All volumes overlap identically; each local-box scissor covers the screen,
while the analytic sphere covers only its projected disc. 20 warmup frames and
120 measured frames **per count**, GPU timestamps and renderer CPU timer;
readback waits after each submitted frame. This is whole-frame GPU time,
including output/readback transfer, not an isolated haze-pass counter.

| Volumes | GPU median ms | GPU p95 ms | GPU min/max ms | CPU median/p95 ms |
| --- | ---: | ---: | ---: | ---: |
| 0 | 0.358848 | 0.374080 | 0.357984 / 0.883456 | 0.104350 / 0.166900 |
| 1 | 0.996352 | 1.007072 | 0.606688 / 27.540672 | 0.113100 / 0.194100 |
| 8 | 2.096384 | 2.777856 | 1.686720 / 3.968160 | 0.114800 / 0.156300 |
| 32 | 5.793664 | 30.604640 | 5.209920 / 33.923680 | 0.140250 / 0.190400 |

Baseline-subtracted median observations: **0.638 / 1.738 / 5.435 ms** for
1/8/32. RDP/shared-machine tails reached 34 ms: medians are not worst-case
latency guarantees. GPU clocks/power, independent haze-only timestamps, hardware
occupancy and an isolated-machine measurement are **unavailable**, not measured.

SHA-256 of the measured Release `test_render_haze.exe`:
`d68327735052062c20bb6a8427f6d629d4ad931b2f62d0d7c8d2b47e1eef1bea`.
Shader source `haze.frag`:
`1632c33c8b8c9a56bacbaa05c4035e36213d109aa08f9c85697d93fcf904e452`.
Generated shader header:
`a72f7b8264b18343617781ae484c44d6a58d44cc5b7d90b35fc98983c63646aa`.
Measurement used the initial base above plus the haze implementation, before
integration rebasing. The haze sources are in the commit introducing this
handoff; `compile_shaders.py --check` verifies source/header hash mapping.
Unrelated integration-base changes are not additional timing measurements.

## Open scope and next work

No implementation blocker. Optional shadowing is one visibility estimate at
the chord midpoint; no light extinction along the sun path, multiple scattering,
textured density, arbitrary physical density falloff powers,
transparent-depth segmentation or order-independent intersecting media. None
is silently approximated as a full volumetric solution. Sorting over volumes
is the caller's responsibility. Benchmark tails warrant isolated profiling if
this becomes a frame-time-critical effect. Integration still requires latest
main rebase, regenerated shaders if needed, green remote CI and merge.
