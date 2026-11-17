# PR 186 HUD review — 2026-11-17

Reviewed main..be2c4a0addcdde5c2301d9b94b651030e7315ba7 in the existing wt-hud worktree.
Initial GitHub checks: 17/17 successful, no submitted reviews.

Findings and fixes:
- P2: shared hud_quad capacity addition could wrap near UINT32_MAX, bypass capacity checks and write out of bounds. Replaced with checked subtraction, also rejecting NULL storage and count > cap.
- Sprite helpers previously emitted NaN/Inf and zero-area quads. Ignore these inputs, including non-finite endpoints and UVs; retain negative dimensions for mirroring.
- Missing boundary coverage: added pixel checks for scissored-to-unscissored texture/font transitions, INT32_MIN/MAX and UINT32_MAX scissors, wrapping vertex ranges, 513 batches over four frames (pool reset/reuse), and a genuinely v12-sized descriptor allocation checked under ASan.
- Documented the existing 512-draw limit and API 13 native stats recompile requirement (SuRenderStats has no size field).

Audit: no remaining defect found in texture handle/generation/state/2D checks, zero-batch/v12 fallback, subtraction-based draw bounds, per-draw dynamic scissors, descriptor pool lifetime/reset after fences, pipeline destruction, shader interfaces or alpha write-mask/blending parity. Per-batch descriptor allocation/update during command recording inside the render pass is legal: each set is fresh and not modified after use. It is not the pre-pass allocation described in the PR prose. All submitted batches are scanned; after 512 drawn batches subsequent nonempty batches are counted skipped; there is no frame validation count cap.

Final verification (all passed):

```sh
python3 tools/compile_shaders.py --check --compiler glslangValidator
cmake --build "/mnt/levyrdisk/Games/Zelda Triforce Heroes/local/build-hud"
ctest --test-dir "/mnt/levyrdisk/Games/Zelda Triforce Heroes/local/build-hud" --output-on-failure -R "^(render_shaders_fresh|render_util|render_offscreen|render_assets|render_paint|demo_offscreen_rollback|render_hud|render_texfx|render_portable|host_api)$"
cmake -S . -B /tmp/build-hud-asan -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug -DSUTEKH_SANITIZE=address,undefined -DSUTEKH_WERROR=ON -DSUTEKH_VULKAN_SYSTEM=ON -DSUTEKH_DEPS_DIR="/mnt/levyrdisk/Games/Zelda Triforce Heroes/local/sutekh_deps"
cmake --build /tmp/build-hud-asan -j 8
ctest --test-dir /tmp/build-hud-asan --output-on-failure -R "^(render_shaders_fresh|render_util|render_offscreen|render_assets|render_paint|demo_offscreen_rollback|render_hud|render_texfx|render_portable|host_api)$"
python3 tools/validate_pack.py
python3 tools/check_repository_license.py
python3 tests/consumer/check_consumer.py --sutekh "/mnt/levyrdisk/Games/Zelda Triforce Heroes/local/worktrees/wt-hud" -- -G Ninja -DSUTEKH_VULKAN_SYSTEM=ON -DSUTEKH_DEPS_DIR="/mnt/levyrdisk/Games/Zelda Triforce Heroes/local/sutekh_deps"
git diff --check
```

Both test suites: 10/10 passed, 0 skipped; render_util 3790 checks and render_hud 180 checks, zero failures. GCC and Clang builds use C17 -Wall -Wextra -pedantic -Werror. Shader check recompiled all 32 sources. Pack validation checked 478 JSON files. Consumer integration passed. Temporary sanitizer build removed after verification.

Unrun: Vulkan validation-layer diagnostics (layers unavailable); Windows/MSVC and unrelated tests not run locally. Initial review attempts hit a GCC alloc-size warning in the new truncated-descriptor test and an incorrectly named sanitizer build target; corrected to void storage and a full build, then reran successfully.

Modified source SHA-256:

- include/sutekh/render.h: `e13cb5c898b04dee3d6d67de48cdcd45718c8496e09920986ad8e51dd34596c0`
- src/render/su_render_util.c: `74658166916c7cef7eaa92c70c9b4cac9b5f9d5fddb59bf6c3630f6628d7a499`
- tests/render/test_render_hud.c: `d6ce6b648d5ae76d36eab05c251b93240fe3a717a61de8864006474d8c9d01db`
- tests/render/test_render_util.c: `4647892a404321a63e4c9637fa4055c1b1a11a83e7a0a2d0f104dd6316b1cc01`

Next: push the verified fix commit to origin feat/hud-textured-batches; GitHub checks must rerun on the new head. No outstanding local verification debt.
