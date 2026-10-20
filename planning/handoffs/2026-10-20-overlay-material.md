# Mesh overlay materials: issue #67

Status: implemented and locally validated; cross-platform PR checks pending.
Date: 2026-10-20. Initial base: `478c736`. Branch: `feat/decal-overlay`.

## Implementation

- Append `overlay` to `SuRenderMaterialDesc` and `SuRenderPaintDesc`. Usage:
  `desc.overlay = 1;` before the existing material creation call. Zero retains
  existing behavior; applications recompile against the enlarged descriptors.
- Four additional startup pipelines: PBR-lite and layered paint, each with
  single-/double-sided culling. Existing shaders and generated headers unchanged.
- Straight-alpha blending, no depth writes, reversed-Z greater-or-equal depth
  testing, positive constant and slope bias factors of 1. Draw after opaque
  geometry and before ordinary transparency. Back-to-front overlay sorting and
  instancing use the existing batching machinery. No per-frame pipeline creation.
- Overlay factors use blend alpha mode regardless of the requested alpha mode,
  so the existing shadow exclusion also prevents overlay shadow casting.
  Lighting, texture alpha, tint alpha and shadow reception use existing shaders.
- Issue title/body rewritten to a generic mesh-overlay request. No asset format
  or glTF changes: the issue does not request import mapping.

## Modified paths

`include/sutekh/render.h`, `src/render/su_render_res.c`,
`src/render/su_render_scene.c`, `src/render/su_render_vk.c`,
`src/render/su_render_vk_int.h`, `tests/render/test_render_paint.c`,
`docs/RENDERING_AND_ASSETS.md`, `planning/leases/issue-67-overlay.json`,
`planning/handoffs/2026-10-20-overlay-material.md`, `PACK_VALIDATION.json`.

## Validation

Windows 11, Quadro P600 over RDP, LLVM clang 23.1.3, MSVC, C17 Release.
All commands run only in the isolated worktree after `tools/env-windows.ps1`.

- `cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release -DSUTEKH_RENDER=ON -DSUTEKH_NATIVE=OFF -DSUTEKH_ASSET_IMPORT=ON`
- `cmake --build build-win -j 4`: passed.
- `ctest --test-dir build-win --output-on-failure -j 4`: 112 tests, zero
  failures, 43.04 s; two POSIX-only tests skipped. Validation required and
  synchronization validation enabled for the renderer tests.
- `build-win/test_render_paint.exe`: 2776 checks, zero failures on both the
  P600 and Mesa lavapipe 25.0.7 (`VK_DRIVER_FILES` selecting the local software
  ICD). Vulkan validation and synchronization validation: zero errors/warnings.
- Pixel checks cover both material kinds, both culling variants, all original
  alpha modes, front-facing and sloped surfaces, coplanar blending, a slightly
  recessed decal, input permutation, texture alpha, tint alpha, ordinary
  transparency after overlays, no overlay depth writes, occlusion by nearer
  opaque geometry, and shadow caster exclusion. Existing paint tests unchanged.
- MSVC targeted renderer/test build with `/W4 /WX`: passed. Its pixel test:
  2776 checks, zero failures, zero validation errors/warnings.
- `python tools/compile_shaders.py --check --compiler <absolute glslang.exe>`:
  23 shader sources and one include recompiled and matched generated headers.
- `python tools/validate_pack.py --write-report`: passed; report regenerated.
- `git diff --check`: passed.

The POSIX build broker cannot run here (`ModuleNotFoundError: fcntl`). Bounded
direct validation was used instead; no broker job was created. The first MSVC
configure needed the Windows SDK bin directory added for `rc.exe`; the subsequent
configure/build succeeded. The shader compiler path must be absolute because the
compiler runs in a temporary directory.

## Cost observation

`SUTEKH_OVERLAY_BENCH=1 build-win/test_render_paint.exe --no-validation`, P600,
256 x 256 offscreen, one two-triangle lit base quad. Mean of 30 observations,
one observation every three frames, after 30 warmup frames per case. No competing
build/test workload in this worktree; system-wide contention is not controlled.

| Added overlays | CPU frame ms | GPU frame ms | Total draws |
| --- | ---: | ---: | ---: |
| 0 | 0.051990 | 0.053322 | 2 |
| 1, same coverage as base | 0.064447 | 0.075472 | 3 |
| 64, 8 x 8 nonoverlapping tiles | 0.070720 | 0.078319 | 3 |

One overlay costs approximately 12.5 microseconds CPU and 22.2 microseconds GPU
in this small synthetic scene. The tiled case adds 18.7/25.0 microseconds and
still batches all 64 overlays into one extra indexed draw. These are observations,
not production-resolution budgets or throughput guarantees. Four extra pipeline
objects are created at renderer startup; shaders introduce no extra instructions
relative to the equivalent existing material shader.

## Fingerprints

SHA-256 of the locally validated implementation and Clang test executable:

| Path | SHA-256 |
| --- | --- |
| `include/sutekh/render.h` | `db759821684c53d2f134970ccf96d996598ec52f96ac93352059b9f8faf3d5c0` |
| `src/render/su_render_res.c` | `3fb6c6a15b87a45ac8e45ac114c9b272cc1af8ab1d7dfbd68822a0b93ab921dc` |
| `src/render/su_render_scene.c` | `0182e252d312e138c709a45dd6b2c5a183eecb4954d3a2b1beddd452e4105631` |
| `src/render/su_render_vk.c` | `8f22eabec9af36c9d30771ec43ad2161a53a1e1aeb0063b732bf4b5cac4b883d` |
| `src/render/su_render_vk_int.h` | `373d44f88864b29223fd281698e3f046d79300cdd070721ced3deea7c7e871ef` |
| `tests/render/test_render_paint.c` | `14fd03c14fa92fce49c011e8c86618e702cd5f720739e955be17e153b993feb0` |
| `build-win/test_render_paint.exe` | `c590f93bd7731d91d1e9b1e3b6662becbcb6810c55ce8b1004d37d1d3f646c4b` |

## Remaining / next

- Rebase on current main, rerun affected gates, publish PR, resolve all CI jobs,
  merge only when green, then remove worktree junctions before worktree cleanup.
- Fixed bias is intended for coplanar geometry, not projected volume decals.
  Sorted-alpha intersections and same-depth overlapping decal order retain the
  normal renderer limitations. No custom bias controls or import extension added.
