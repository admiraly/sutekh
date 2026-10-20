# Issue 85: deterministic GPU particle emitters

## Implemented

Public API generation 8 adds `SuParticleEmitter`, `SuParticleAttractor`,
`SuParticleState`, deterministic count/evaluate/validate CPU helpers,
frame emitter submission, optional device capability and per-frame statistics.
An offscreen-only diagnostic reads actual GPU states in draw order on request.
No emitters preserves existing rendering and graphics-only device admission.

Counter-based uint32 RNG and bounded 60 Hz replay reconstruct every particle
without frame history. Rate/burst, four shapes, four-key size/colour curves,
drag, gravity, turbulence, four smoothed attractors, anchored world/local
simulation and explicitly specified moving affine transforms are implemented.
Compute generates device-local billboard/axis geometry for existing soft,
layered, textured/atlas, additive/premultiplied FX pipelines. Stable GPU sorting
uses compact keys, shared 64-lane blocks and global bitonic merges.

The issue description was rewritten generically, retaining its technical request.
All fixture textures and geometry are synthetic; no external content was used.
API usage and exact bounds/semantics: `docs/GPU_PARTICLES.md`.

## Modified paths

- `include/sutekh/render.h`
- `src/render/su_particles.c`
- `src/render/su_render_particles.c`
- `src/render/su_render_vk.c`
- `src/render/su_render_vk_int.h`
- `src/render/shaders/particles.comp`
- `src/render/su_render_shaders.h` (generated, never hand-edited)
- `tools/compile_shaders.py`
- `cmake/SutekhPresentation.cmake`
- `tests/render/test_particles.c`
- `tests/render/test_render_particles.c`
- `docs/GPU_PARTICLES.md`
- `docs/RENDERING_AND_ASSETS.md`
- `planning/evidence/gpu-particles/` (state, hashes, validation and timing)
- This handoff and regenerated `PACK_VALIDATION.json`.

## Local verification

Windows 11, Clang 23.1.3, MSVC 19.44.35229, C17, glslang 16.6.0;
Quadro P600 over remote desktop and Mesa 25.0.7 lavapipe/LLVM 20.1.6.
The project-local Windows environment was sourced for all native commands.

- `python tools/compile_shaders.py --compiler <absolute local glslang.exe>`:
  generated the embedded header; `--check`: all 29 sources/2 includes current.
- `cmake -S . -B build-particles -G Ninja -DCMAKE_BUILD_TYPE=Release`, then
  `cmake --build build-particles -j 4`: full default build passed.
- `ctest --test-dir build-particles --output-on-failure -j 3` with
  `SUTEKH_REQUIRE_VALIDATION=1`, `VK_LAYER_VALIDATE_SYNC=1`: 120 tests,
  0 failures, 2 existing Windows skips (path FIFO, containment), 44.97 seconds.
- Targeted reference/render checks repeated on final source. Both hardware and
  lavapipe report validation enabled, zero errors/warnings and zero failures.
  Includes actual 131072-slot GPU sorting, padding/workgroup boundaries,
  state/pixel oracle comparisons, standalone seeks, endpoint ages, curves,
  moving transforms, forces, atlas/layered blends, known soft fade and opaque
  occlusion pixels, multiple emitters, combined-capacity rejection and bad input.
- MSVC `/W4 /WX`: renderer and new tests compiled; both targeted tests passed.
  Initial configuration lacked SDK `rc`/`mt` on PATH; adding the installed SDK
  binaries resolved it without source changes.
- `python tools/check_repository_license.py`: passed.
- `git diff --check`: passed.

Independent read-only review found two allocation recovery errors; fixed
preparation ordering before swapchain acquisition and partial diagnostic-buffer
cleanup. A fresh second review of the complete context found no actionable
correctness issues. These reviews did not independently rerun the runtime tests.

## Cost and evidence

`planning/evidence/gpu-particles/p600-benchmark.txt` contains complete-frame
GPU/CPU median/p95 timestamps at 1280x720, 20 warmups and 60 samples, 0/10k/100k
particles both unsorted and sorted, age 60 ticks, lifetime 600 ticks. Validation
and diagnostic readback are disabled; ordinary offscreen output copy remains.
Exact workload is documented in `docs/GPU_PARTICLES.md`. RDP/shared GPU activity
causes large tails; these are observations, not a performance guarantee.
Source and executable SHA-256 values are in the adjacent fingerprints file.

## Remaining scope and integration

- Sorting is within each emitter, not global across emitters or other FX streams.
- Replay cost scales with live particle age. No temporal cache or analytic-force
  specialization is claimed; caller-selected bounds limit work.
- Floating-point CPU/GPU and cross-device bit equality is not claimed.
- Two affine transform endpoints represent replayable motion; arbitrary transform
  histories and mesh-surface emission are not implemented or required by the issue.
- Windowed allocation-failure recovery was reviewed, not fault-injected interactively.
- Remote CI and merge status are recorded by the PR; do not infer them from this
  local verification snapshot. No canonical-checkout changes or builds were made.
