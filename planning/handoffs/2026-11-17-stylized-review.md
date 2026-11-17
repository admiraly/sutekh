<!-- SPDX-FileCopyrightText: 2026 admiraly -->
<!-- SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 -->
# PR 188 adversarial review — 2026-11-17

Reviewed base `main` through `7eb76ce4596d163ca256113b7cbee9004ed74098`, using the existing wt-stylized worktree.
GitHub at review start: OPEN, UNSTABLE, 16 successful checks, one failed clang ASan+UBSan native-OFF check,
no submitted reviews. The supplied CI timeout diagnosis was not independently rerun for space3d_geom.

## Findings and fixes

- Toon reader/writer accepted nonzero reserved words. Reject all four words; malformed packs with recomputed
  hashes and writer rejection tests cover this. This preserves the reserved encoding for future extensions.
- Direct toon material creation accepted invalid double_sided/unlit modes and high tex_info bits, unlike the
  asset reader/writer. Reject these before allocating Vulkan resources.
- Toon materials accepted textures with the wrong slot color space, including sRGB normals/occlusion.
  Enforce the documented SU_ASSET_SLOT_* spaces on every supplied texture; upload_pak uses this same path
  and its existing rollback releases partial resources. Test all five slots.
- Rim's normalized dot product could round above one, making `pow(1-nv, fractional_power)` undefined.
  Clamp nv to [0,1] and regenerate the embedded SPIR-V. Positive glossiness/rim-power validation already
  excludes the zero/negative-exponent cases for pow(0,p).
- Added missing importer coverage: uncolored primitives before AND after colored primitives, plus mirrored
  colored meshes retaining their color records while triangle winding flips.

Before the implementation fixes, the initial new regression tests failed in render_stylized (7 checks) and
asset_pak (5 checks). Final runs below pass.

## Audit notes

Reviewed all 19 changed paths, including the generated shader header through compiler regeneration/check.
New layouts are additive: existing vertex/material layouts remain unchanged; render API version is 13.
Mesh payload sizes use 64-bit count arithmetic on read and are bounded by the entry's validated byte range;
entry offset+size cannot wrap after the subtraction-based range test. Supported targets are 64-bit.
Colored stream offsets require float alignment (4 bytes), which the index-tail layout satisfies.
The new cb buffer participates in staged upload, release, immediate failure cleanup and deferred retirement;
ToonUbo shares the material UBO cleanup path. Descriptor pool capacities cover paint's larger sampler count
and one UBO per material; pipeline/layout destruction includes partial initialization.
Sort-key material/mesh/submesh fields truncate large indices, so keys are not unique across the entire handle
range. This can reduce batching at large capacities, but draws still compare full identity before instancing;
blend order uses distance/submission order, so this does not misbind resources or merge distinct draws.
Colored vertex binding state is invalidated after FX draws and rebound across colored/uncolored transitions.
UV order is scale, rotation, offset; toon vertex-color and rim-lit-mask flags are independent.
Alpha cutoff, forced per-instance blend and overlay use the existing material push/pipeline semantics.
No additional blocking defects identified in those paths.

## Verification (final sources)

All commands run from the worktree unless otherwise specified:

```sh
python3 tools/compile_shaders.py --check --compiler glslangValidator
cmake --build "/mnt/levyrdisk/Games/Zelda Triforce Heroes/local/build-stylized"
ctest --test-dir "/mnt/levyrdisk/Games/Zelda Triforce Heroes/local/build-stylized" --output-on-failure -R "^(render_|asset_|host_api|demo_offscreen_rollback)"
cmake -S . -B /tmp/build-stylized-asan -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug -DSUTEKH_SANITIZE=address,undefined -DSUTEKH_NATIVE=OFF -DSUTEKH_WERROR=ON -DSUTEKH_VULKAN_SYSTEM=ON -DSUTEKH_DEPS_DIR="/mnt/levyrdisk/Games/Zelda Triforce Heroes/local/sutekh_deps"
cmake --build /tmp/build-stylized-asan --parallel 6 --target test_render_util test_render_offscreen test_render_assets test_render_texfx test_render_particles test_render_flame test_render_distortion test_render_haze test_render_bloom test_render_paint test_render_stylized test_asset_pak test_asset_gltf test_asset_bc test_asset_dds test_render_ghost test_render_portable
ctest --test-dir /tmp/build-stylized-asan --output-on-failure -R "^(render_|asset_)"
python3 tools/validate_pack.py
python3 tools/check_repository_license.py
python3 tests/consumer/check_consumer.py --sutekh "/mnt/levyrdisk/Games/Zelda Triforce Heroes/local/worktrees/wt-stylized" -- -G Ninja -DSUTEKH_VULKAN_SYSTEM=ON -DSUTEKH_DEPS_DIR="/mnt/levyrdisk/Games/Zelda Triforce Heroes/local/sutekh_deps"
git diff --check
```

Results: shader sources/includes/SPIR-V match; full preconfigured build passed; 23/23 selected normal tests
passed; 20/20 targeted sanitizer tests passed with no skips; pack validation, license and consumer checks passed.
Normal and sanitizer builds enable C17 -Wall -Wextra -pedantic -Werror.
Vulkan pixel tests ran on Intel Arc A770 (DG2); validation layers were unavailable, so validation-layer error
coverage is unverified. No full unrelated sanitizer suite was run. The initial consumer attempt coincided with
shader regeneration and failed its tree-unchanged guard; the final stable-source rerun passed every check.
Removed /tmp/build-stylized-asan after verification. Logs are outside the repo in /tmp/stylized-*.log.
No watch or GitHub review submission requested/performed. Next: new-head CI and maintainer review.

## Modified source hashes (SHA-256)

- `include/sutekh/asset.h`: `af261c10e94f70a53717c533d1fe0039d93f5bd8c4af255eb2f3b2a5e88ae89b`
- `include/sutekh/render.h`: `9ff7d850919f7ef05362b0615e5bf68a0102968530fcd88249cb274268448f2b`
- `src/asset/su_asset_pak.c`: `5fdad1ed1124ed2b7fb5199b663f2c7447087e82aa0d4f8f338ab7286ccb5c02`
- `src/render/shaders/toon.frag`: `0596b7f62d8dde681bf48e4c9d5d5cb7ac9270e140ad53e2717723d8977406a6`
- `src/render/su_render_res.c`: `a84e3af8f7020cab0999280785ea4dc9df77697f33874c34962174e322459524`
- `src/render/su_render_shaders.h`: `3fb4d2b4337bbda483c2501e4fc07025fed48ceea1b23fdc5b349c708dc20838`
- `tests/asset/test_asset_gltf.c`: `eeef2dd3fd9ad40a54a31c508e64b0283c5caa99e7020895ca393c5854e1f006`
- `tests/asset/test_asset_pak.c`: `9ad1c0003d4c99c3ee9be5ff7654519dfcfb440d72bb0928238b9798e11ce335`
- `tests/render/test_render_stylized.c`: `a14aa7e63bc6d0fb346f1f13dc3bc6c8dcea61f08a592072634e97aa16f7f5c0`
