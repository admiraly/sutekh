# Issue 127: opt-in premultiplied-alpha output

## State and scope

Locally verified on `feat/issue-127-premul-alpha`, based on
`7aa23242a98fc29ebcd364b3a806129c69f7d1c1`. CI/integration remain pending.

Exact modified paths:

- `include/sutekh/render.h`
- `src/render/su_render_vk.c`
- `src/render/shaders/post.frag`
- `src/render/shaders/post_bloom.frag`
- `src/render/su_render_shaders.h`
- `tests/render/test_render_bloom.c`
- `tests/render/test_render_texfx.c`
- `tests/render/test_render_flame.c`
- `tests/render/test_render_haze.c`
- `docs/RENDERING_AND_ASSETS.md`
- `planning/handoffs/2026-10-21-premul-alpha.md`
- `planning/handoffs/2026-10-21-premul-alpha.state.json`
- `PACK_VALIDATION.json`

API version 11 appends `output_alpha`; zero preserves legacy output. The opt-in
mode clears transparent black, suppresses sky drawing without disabling environment
lighting, and carries HDR coverage through both post paths. Existing additive
blend states preserve coverage; overlay and HUD pipelines now mask alpha writes.
No new targets, descriptors, passes, or GPU readbacks. Shader headers regenerated
with glslang 16.6.0. Native consumers must recompile.

Conservative choices: separate opt-in output mode, unchanged RGBA8 readback, no
RGBA16F extension. Composite decoded sRGB RGB without multiplying by alpha again.
Nonlinear post processing does not commute with background composition; exact
direct-background comparisons use the unclipped unit-exposure clamp path without
bloom. Bloom/ACES/auto-exposure tests independently check RGB and preserved alpha.
The public rendering contract describes these limitations explicitly.

## Local validation

Windows 11 Pro 10.0.26200, Intel Core i5-12400F, Clang 23.1.3 Debug C17
(`-Wall -Wextra -Werror -pedantic`), MSVC 19.44.35207 Debug (`/W4 /WX`).
NVIDIA Quadro P600 driver 32.0.15.8216 and Mesa 25.0.7 lavapipe/LLVM 20.1.6.

Setup/build commands, all successful after resolving local tool invocation paths:

```powershell
. ./tools/env-windows.ps1
python tools/bootstrap_vulkan_headers.py
python tools/bootstrap_asset_deps.py
python tools/compile_shaders.py --compiler "$PWD/local/toolchains/glslang-16.6.0/bin/glslang.exe"
python tools/compile_shaders.py --check --compiler "$PWD/local/toolchains/glslang-16.6.0/bin/glslang.exe"
cmake -S . -B build-i127 -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug -DSUTEKH_NATIVE=OFF -DSUTEKH_RENDER=ON -DSUTEKH_ASSET_IMPORT=ON
cmake --build build-i127 -j 8
$env:VK_DRIVER_FILES = Join-Path $PWD 'local\toolchains\mesa-25.0.7\x64\lvp_icd.x86_64.json'
$env:SUTEKH_REQUIRE_VALIDATION = '1'
ctest --test-dir build-i127 --output-on-failure -j 4
```

Full suite: **138 tests, zero failures, two Windows platform skips** (`path_fifo`,
`containment`), 122.60 seconds. All render tests ran on CPU Vulkan with required
validation. Focused `render_(bloom|texfx|haze|flame)$` tests passed on both lavapipe
and the P600; zero validation errors/warnings. Bloom: 754 checks with validation;
textured FX: 199694 checks. Synthetic tests cover mask discard/pass, opaque and
stacked translucency, instance tint, overlays, HUD, additive and premultiplied FX
quads/meshes, flame, additive/over haze, bloom alpha, both tonemaps, automatic
exposure, invalid output mode, and full-image compositing over two backgrounds.

MSVC renderer-on build: add the discovered MSVC `bin/Hostx64/x64` and Windows SDK
`bin/<version>/x64` to PATH after the environment script, then:

```powershell
cmake -S . -B build-i127-msvc -G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_BUILD_TYPE=Debug -DSUTEKH_NATIVE=OFF -DSUTEKH_RENDER=ON -DSUTEKH_ASSET_IMPORT=ON -DCMAKE_MT=mt -DCMAKE_RC_COMPILER=rc
cmake --build build-i127-msvc -j 6 --target test_render_bloom test_render_texfx test_render_flame test_render_haze
ctest --test-dir build-i127-msvc -R 'render_(bloom|texfx|haze|flame)$' --output-on-failure -j 2
```

All four MSVC tests passed on lavapipe with required Vulkan validation. Pack report
regeneration/check, pack-validator regression tests, repository-license metadata
check, and `git diff --check` pass. Logs/executables remain in ignored task-local
build directories. Initial harness stdin/quoting and forward-slash ICD path errors
were corrected; a missing SDK bin PATH in the first MSVC configure was corrected.
No failed build or skipped renderer is counted as acceptance.

## Serial timing observations

`test_render_bloom.exe --no-validation`, once per device, after other validation
jobs completed. 256x256, one half-opacity emissive cube, bloom enabled; modes
alternate, 20 samples per mode after two warm-up pairs. Each sample renders three
frames then reads back; reported CPU excludes fence wait/readback, GPU uses device
timestamps. Median is the upper-middle sample; p95 is nearest-rank. Development
desktop observations, not a performance ranking or a CI budget.

| Device / output | CPU median / p95 (ms) | GPU median / p95 (ms) |
| --- | --- | --- |
| P600 / legacy | 0.0816 / 0.3241 | 0.1238 / 0.1278 |
| P600 / premultiplied | 0.0967 / 0.1593 | 0.1265 / 0.1279 |
| lavapipe / legacy | 0.0963 / 0.1275 | 3.7977 / 6.4327 |
| lavapipe / premultiplied | 0.1004 / 0.1163 | 4.2835 / 23.2457 |

P600 GPU median difference was +0.0027 ms in this small run. CPU Vulkan tails are
noisy; no general performance claim follows. Counter/energy measurements: not run.

## SHA-256 evidence

- Renderer source: `d6e53607e03ce3c639a4960344e9a403cfd914d5918de695305156b88c16d1b1`
- Generated shaders: `1a0b17207a9cbbbbd67d21943d53f86a2d4dadbebec94d0b8e543d3b1ec9bc50`
- Clang bloom test executable: `7b64fa101789506df600f0a6dbbdecf08caad5e9bbb0a6a5d72b0db0f904e3f4b`

## Unrun and next

Linux GCC 16.2 and native-tier matrices are left to CI; no CI result is claimed.
Window transparency and optional RGBA16F readback are not implemented. No blockers.
Next: publish the branch and closing PR, then integrate after normal review/CI.
