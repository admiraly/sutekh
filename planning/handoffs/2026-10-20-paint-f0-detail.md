# Paint specular-colour and tiled detail: issues #64 and #70

## State and scope

Implemented and locally verified; CI and integration are pending at this checkpoint.
Owner-requested scope: paint runtime API, descriptor plumbing, paint shader, synthetic paint tests and documentation.
Initial base: `478c736`. Work was isolated in `feat/paint-f0`; no shared checkout sources or assets were modified.

Modified implementation paths:
- `include/sutekh/render.h`
- `src/render/su_render_vk_int.h`
- `src/render/su_render_vk.c`
- `src/render/su_render_res.c`
- `src/render/su_render_scene.c`
- `src/render/shaders/paint.frag`
- `src/render/su_render_shaders.h` (regenerated with glslang 16.6.0)
- `tests/render/test_render_paint.c`
- `docs/RENDERING_AND_ASSETS.md`
- this handoff

The runtime has an independent RGB F0 map/factor/swizzle, fixed-Z base normals and tiled material or
normal/darkening detail. UV sets/wraps, scale, rotation, strength and layer-mask selection are explicit.
Absent extra options retain the existing metalness path and byte-identical legacy paint/PBR-lite equivalence.
The cooked `SuAssetPaint` layout, its eight-slot count and version-2 packs are unchanged. The runtime descriptor
is extended and consumers must recompile; older binary descriptor layouts are not supported. New slots are
runtime-only, not new glTF or cooked-pack fields. Detail material replacement operates on the base material-map
scalar channels in the unpainted region, not an added material-ID palette. Exact API semantics are in the rendering doc.

## Local validation

Windows 11, i5-12400F, Quadro P600 over RDP; clang 23.1.3, Ninja, Release, native specialization disabled.
Environment: dot-source `tools/env-windows.ps1`; local pinned Vulkan headers, glslang 16.6.0 and validation layer.

- `cmake -S . -B build-paint -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Release -DSUTEKH_NATIVE=OFF -DSUTEKH_RENDER=ON`
- `cmake --build build-paint -j 4`: successful.
- `SUTEKH_REQUIRE_VALIDATION=1 ctest --test-dir build-paint --output-on-failure -j 4`: 112 tests, zero failures;
  two existing POSIX-only tests skipped. This full run preceded the last additional paint assertions.
- Final targeted `test_render_paint.exe`: 1147 checks, zero failures on P600, validation zero errors/warnings.
- Same final paint tests with `VK_DRIVER_FILES` selecting the local Mesa 25.0.7 lavapipe ICD: 1147 checks,
  zero failures, validation zero errors/warnings. No new pixel assertions are skipped on the CPU device.
- `python tools/compile_shaders.py --check --compiler <absolute glslang path>`: all 23 sources and shared include match
  the embedded SPIR-V (also regenerated after the final neutral-detail-normal adjustment).
- `python tools/validate_pack.py --write-report`: passed; tracked report unchanged at this checkpoint.
- `git diff --check`: clean at this checkpoint.

Coverage includes independent F0 versus metallic, spatial F0, linear/sRGB decoding, RGB factor, channel swizzles and
constants, UV1, direct-light and procedural/cubemap ambient formulas, both normal conventions, detail normal rotation,
zero detail strength, repeated/rotated detail UVs, darkening strength, layer/unpainted selection, material replacement,
missing neutral maps, invalid parameters/colour spaces and existing pack upload/render equivalence.

Unrun locally: Linux/GCC 16.2 and MSVC jobs; these are CI obligations before merge. No new dependencies or content.

## P600 GPU timestamp observations

`SUTEKH_PAINT_BENCH=1 test_render_paint --no-validation`, 1920x1080 full-screen quad, no sky draw/shadows/bloom,
procedural ambient and direct light, one normal texture, all synthetic maps 1x1 RGBA8 (cache-resident lower-cost
case), detail UV scale 26, strength 0.5. Validation disabled for timing. Each mode: 40 warm-up frames and 300
completed-frame GPU timestamp samples; two interleaved old/new runs. These are whole-frame timings, not isolated
shader counters. Timestamp results are from the most recently completed frame, as exposed by `SuRenderStats`.
GPU clocks and exclusive GPU access were not controlled; RDP/shared-host scheduling causes large tail outliers.
Fine-grained qualification and large-map bandwidth costs remain unmeasured.

Old binary: initial base plus only the optional benchmark harness (the feature-guarded benchmark has one old mode).
New binary: final shader and implementation with the same harness and five modes. SHA-256:

- Old executable: `55147f59aef6ccee0f31a406c9fcef121bfef477ab2f5c1606d9f799bb4a6ba5`
- Measured new executable: `83e93a31e885ff9f443a8963af77d758f198d53093eccf36f4df1ccaecfa84c7`
- New `paint.frag`: `0550d210736a3f3e0ec3fa86504d96268a82cc5bac92caf1d64dfa334577e817`
- New embedded shader header: `e4609fe405fa6884d880933a51620cd8a7eaa42943f4db531d1b6f303ef3d495`

| Run | Mode | Median ms | p95 ms | Min ms | Max ms |
| --- | --- | ---: | ---: | ---: | ---: |
| 1 old | original paint | 4.077760 | 28.650336 | 3.477952 | 30.378784 |
| 1 new | options disabled | 4.036352 | 28.927552 | 3.595392 | 30.445312 |
| 1 new | F0 + fixed-Z base normal | 4.252608 | 29.183264 | 4.078752 | 31.232032 |
| 1 new | material detail | 4.201984 | 29.152640 | 3.672640 | 31.488256 |
| 1 new | normal/darkening detail | 4.176288 | 28.880384 | 3.707328 | 31.358624 |
| 1 new | F0 + fixed-Z + normal/darkening detail | 4.299904 | 29.033568 | 3.836288 | 31.224192 |
| 2 old | original paint | 3.905792 | 28.787584 | 3.472864 | 30.978080 |
| 2 new | options disabled | 4.091968 | 28.940704 | 3.601824 | 29.909248 |
| 2 new | F0 + fixed-Z base normal | 4.259040 | 28.946016 | 3.724608 | 30.979584 |
| 2 new | material detail | 4.154112 | 29.187424 | 3.668576 | 31.172480 |
| 2 new | normal/darkening detail | 4.178144 | 28.948480 | 3.710784 | 30.377312 |
| 2 new | F0 + fixed-Z + normal/darkening detail | 4.323424 | 29.061280 | 3.835616 | 31.432160 |

Average of the two medians (not pooled samples): original 3.992 ms, disabled 4.064 ms (+0.072 ms / 1.8%),
F0 4.256 ms (+0.264 ms / 6.6%), material detail 4.178 ms (+0.186 ms / 4.7%), normal/darkening detail 4.177 ms
(+0.185 ms / 4.6%), combined 4.312 ms (+0.320 ms / 8.0%). Treat small differences cautiously given the uncontrolled
host and tails; these observations are not a performance guarantee.

## Next

CI follow-up: the first Linux checked run exposed an invalid fixture assumption: constant all-zero UV1 did not give
portable image comparisons (the local CPU driver also produced black output with clamped zero-gradient sampling).
The quad fixture now has a meaningful nondegenerate UV1 mapping with swapped axes. Specular and detail UV1 tests
check the expected spatial pattern and compare unchanged detail samples to their corresponding reference pixels,
not different view angles. This also strengthens UV1 coverage. No pixel tolerance or CPU-device skip was added;
implementation and measured shader hashes are unchanged.

Rebase on latest upstream, regenerate shaders if needed, push the PR, repair CI findings, merge only when all required
checks are green, and remove the isolated worktree after unlinking its toolchain/dependency junctions.
