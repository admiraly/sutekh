# Layered FX implementation: issues 66 and 68

## Task state

Implemented and locally verified on 20 October 2026. Isolated branch
`feat/fx-scroll`, initial base `478c7366c2cab7c85b8b1983a4b501d53af2aec9`.
Owner-authorized scope: layered quad effects and resident unlit FX mesh
materials, their public API, shaders, tests and documentation. Both issue bodies
were edited to describe only generic rendering features. CI/integration pending
at this checkpoint; no unchecked merge is claimed here.

## Exact modified paths

- `include/sutekh/render.h`
- `src/render/su_render_vk_int.h`
- `src/render/su_render_vk.c`
- `src/render/su_render_scene.c`
- `src/render/su_render_res.c`
- `src/render/su_render_util.c`
- `src/render/shaders/fx_layer.vert`
- `src/render/shaders/fx_mesh.vert`
- `src/render/shaders/fx_layers.glsl`
- `src/render/shaders/fx_tex_add.frag`
- `src/render/shaders/fx_tex_pm.frag`
- `src/render/su_render_shaders.h` (regenerated, never hand-merged)
- `tools/compile_shaders.py`
- `tests/render/test_render_texfx.c`
- `docs/RENDERING_AND_ASSETS.md`
- `docs/LAYERED_FX.md`
- `planning/evidence/layered-fx/source.sha256`
- `planning/evidence/layered-fx/p600-bench.log`
- `planning/handoffs/2026-10-20-layered-fx.md`
- `PACK_VALIDATION.json` (regenerated for package changes)

## Implemented

API generation 5 (after flame volumes took 4): `SuFxLayers`, `SuFxUvTransform`, `SuFxBatch.layers`,
`SuRenderFrameDesc.time_seconds`, `SuRenderFxMaterialDesc`,
`su_render_fx_material_create`. Source-compatible zero defaults; native callers
must recompile for larger structures. Three samples multiply RGBA; transforms
are independent. Third-slot atlas selection is per batch/material, with local
wrap and angular fade. Existing ADD/PREMUL equations are retained.

Meshes retain GPU vertices/indices, object transforms, HDR tint, culling and
LOD. They are unlit, depth-tested, depth-write-disabled, optionally two-sided,
soft against opaque depth, and stably sorted with other transparent mesh
material families. Regular translucent pipelines moved to the read-only-depth
subpass so FX and normal transparent draws interleave correctly. Analytic FX
retain their position after mesh transparency and before textured quads.

Per-frame descriptor sets carry all three texture bindings and retire with
their frame slot. Invalid/not-ready layers skip the whole effect rather than
silently dropping masks. Unused layers avoid sampling. Atlas grid multiplication
uses 64 bits to avoid overflow/modulo-by-zero. Explicit mip-0 sampling for zero
UV gradients avoids the software driver's zero-gradient anisotropic black
sampling observed during the new pixel tests.

## Validation actually run

Toolchain: Windows 11 x64, PowerShell 5.1, LLVM clang 23.1.3, MSVC 19.44.35229,
Windows SDK 10.0.26100.0, Python 3.13.15, glslang 16.6.0. Environment loaded
with `. .\tools\env-windows.ps1`. Build directories are worktree-local.

- `python tools/compile_shaders.py --compiler <absolute glslang.exe>` regenerated
  the header; `--check --compiler <absolute glslang.exe>` verified all 25 shader
  sources and both includes, including identical SPIR-V words.
- `cmake -S . -B build/fx-scroll -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Release`
  with pinned Vulkan headers and asset dependencies, then
  `cmake --build build/fx-scroll -j 6`: passed under strict C17 warnings.
- `SUTEKH_REQUIRE_VALIDATION=1 ctest --test-dir build/fx-scroll --output-on-failure -j 4`:
  112/112 tests successful (110 passed, two pre-existing platform skips:
  `path_fifo` and `containment`). P600 render tests: zero validation errors and
  warnings; layered suite 230929 checks, zero failures.
- With `VK_DRIVER_FILES` selecting local Mesa 25.0.7 lavapipe x64,
  `ctest --test-dir build/fx-scroll --output-on-failure -R '^render_'`:
  9/10 passed; new layered suite 101314 checks, zero failures/validation messages.
  Existing `render_ghost` still returns black frames (tracked separately in
  issue 61). No assertions or software-driver exclusions were added here.
- MSVC C17 `/W4 /WX`, renderer **enabled**: configured
  `build/fx-scroll-msvc` with `cl`, SDK tools on PATH and native tier off;
  built `test_render_texfx`, `test_render_util`, `test_render_portable` and ran
  `ctest --test-dir build/fx-scroll-msvc --output-on-failure -R '^(render_texfx|render_util|render_portable)$'`:
  3/3 passed, P600 validation clean.
- `python tools/check_repository_license.py`: passed.
- Pack report regeneration/check and remote CI are integration gates, not
  inferred from these native tests.

Independent read-only review found two issues (mixed mesh transparency and
atlas-local scrolling). Both were fixed with specific pixel regressions.
Follow-up review was requested against the fixes; its outcome is recorded in
the PR before integration.

## P600 cost observation

`test_render_texfx --no-validation --bench-layered`, Windows 11 over remote
desktop, NVIDIA Quadro P600, driver 32.0.15.8216. Release clang build, GPU
timestamp queries, no explicit readback; the normal offscreen output copy is
included. Each workload has 20 warmup frames plus 120 samples. Resolution is
asserted at 1024x1024. Sixteen overlapping full-screen quads or resident mesh
instances, 128x128 RGBA8 textures, additive blending, independent scrolling,
no softness/angle/atlas/bloom. Compatible objects instance into one draw.

| Workload | GPU min / median / p95, ms | CPU median / p95, ms |
| --- | --- | --- |
| Empty | 0.525 / 0.963 / 2.874 | 0.056 / 0.125 |
| Quads, one layer | 3.628 / 4.058 / 29.591 | 0.099 / 0.166 |
| Quads, three layers | 5.446 / 6.158 / 31.223 | 0.127 / 0.184 |
| Resident meshes, one layer | 3.634 / 4.051 / 29.582 | 0.116 / 0.190 |
| Resident meshes, three layers | 5.435 / 6.111 / 31.508 | 0.117 / 0.181 |

Observed median increment for two extra samples: 2.100 ms (quad workload),
2.060 ms (mesh workload), approximately 0.13 ms per full-screen layer stack at
this extreme 16x overdraw. This is not a performance guarantee or a general
scene budget. Shared GPU activity/remote desktop causes large p95 excursions;
do not interpret the tails as isolated shader execution. The benchmark is
repeatable and contains no timing assertions. Raw output is committed alongside
the source SHA-256 manifest in `planning/evidence/layered-fx/`.

Measured executable SHA-256:
`eda6c6157472be6e881c7be02d9f9a209d227d181f447007a02f8eae53255109`.
Generated shader header SHA-256:
`5530e683ca399b9545c67808a29ccd1e43560559a641673728472a4a4bc15720`.
Source hashes describe the exact measured local code before integration;
unrelated rebased changes require their own rebuild/CI verification.

## Limits and next work

- No global quad/mesh transparency sort or per-triangle transparent sorting.
- Atlas cells need padding for broad mip/anisotropic footprints.
- Material descriptions are native-only; no asset-pack schema change.
- Linux/GCC 16.2, Linux native tier, and remote Windows jobs are unrun locally;
  CI must be green before merging. The independently tracked software-driver
  `render_ghost` failure is not expanded into this feature's scope.
- Next: finish review, rebase onto latest `origin/main`, regenerate generated
  headers if required, push, resolve CI and merge with a merge commit.
