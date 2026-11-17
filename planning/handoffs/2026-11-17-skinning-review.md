# PR 187 adversarial review — 2026-11-17

Reviewed main..1d04a502b8af9563936f6977e3892a97f39d485f in the existing worktree.

Fixed findings:
- v12 frame descriptors rejected: zero-extend the exact old prefix, tested with an exact-sized heap allocation under ASan.
- Bind-pose culling and shadow bounds: enclose joint-transformed spheres for every LOD with a conservative Frobenius norm; preserve bind-pose bounds for zero palettes/weights. Tight-sphere pixel regression moves an object from outside the camera into view. Node bounds also drive LOD and point-light masks.
- Weight-sum overflow and tiny positive sums: double accumulation in glTF, power-of-two scaling in both shaders. FLT_MAX regression catches GPU subnormal reciprocal flushing too.
- Reflected skin transform: flip tangent handedness with the blended determinant sign.
- Storage buffer descriptor range: validate maxStorageBufferRange and prevent capacity doubling past it.
- Unsupported glTF component types: reject invalid joint/weight encodings and normalized joints. Added malformed-component and flat-normal-skinned fixtures.
- v1 compatibility: continue ignoring the old reserved words, tested with a rehashed v1 payload.

Other audit observations: mesh lengths use uint64_t products and exact comparisons before traversal; 64-byte mesh headers and 24-byte skin vertices are preserved. Supported targets are 64-bit. Slice checks use subtraction to avoid addition overflow. Shader indices clamp to the available slice. Frame descriptor replacement follows the frame fence and each frame owns its buffers. Three-buffer uploads retain four-byte alignment, transfer barriers, source ownership, and deferred retirement. FX draws invalidate scene binding caches; scene/shadow batches compare actual mesh/submesh/skinning state. Raw all-zero weights deliberately render bind pose; glTF sanitizes these to joint 0. Generated SPIR-V was regenerated with installed glslang 16.2.0.

Final checks, all passed (run from the worktree):
```sh
python3 tools/compile_shaders.py --check --compiler glslangValidator
cmake --build "/mnt/levyrdisk/Games/Zelda Triforce Heroes/local/build-skinning"
ctest --test-dir "/mnt/levyrdisk/Games/Zelda Triforce Heroes/local/build-skinning" --output-on-failure -R '^(render_|asset_|host_api|demo_offscreen_rollback)'
cmake -S . -B /tmp/build-skinning-asan -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug -DSUTEKH_SANITIZE=address,undefined -DSUTEKH_VULKAN_SYSTEM=ON '-DSUTEKH_DEPS_DIR=/mnt/levyrdisk/Games/Zelda Triforce Heroes/local/sutekh_deps'
cmake --build /tmp/build-skinning-asan
ctest --test-dir /tmp/build-skinning-asan --output-on-failure -R '^(asset_|render_)'
python3 tools/validate_pack.py
python3 tools/check_repository_license.py
python3 tests/consumer/check_consumer.py --sutekh "/mnt/levyrdisk/Games/Zelda Triforce Heroes/local/worktrees/wt-skinning" -- -G Ninja -DSUTEKH_VULKAN_SYSTEM=ON '-DSUTEKH_DEPS_DIR=/mnt/levyrdisk/Games/Zelda Triforce Heroes/local/sutekh_deps'
git diff --check
```
Normal suite: 23/23 passed. Clang ASan+UBSan: 20/20 passed. Both builds use C17 with -Wall -Wextra -pedantic -Werror. Consumer checks passed. Temporary sanitizer build removed after verification.

Limitations: Vulkan validation layers unavailable; GPU pixel tests ran without layer diagnostics. Windows/MSVC not rerun locally. No formal GitHub review submitted. Original PR CI green; the pushed revision needs its own CI. Conservative sphere computation costs O(nodes * LODs * joints) and overestimates bounds. No unresolved blocker in reviewed paths.

Modified paths and SHA-256 before this report:
- `src/asset/su_asset_gltf.c`: `2d803bd4f9123da8e02744f42bc9275e5d5f0fd840d05bda5e0ad6e4ca4553d3`
- `src/asset/su_asset_pak.c`: `21b752e8f773355682063f2cfdac04603761583348a6f7c84b489c9c81e2bd4d`
- `src/render/shaders/pbr_skinned.vert`: `8e03ce30e0949e720829203591e0a6553736592f89ff4fb9409cab242e2c9604`
- `src/render/shaders/shadow_skinned.vert`: `94384c167fa792bef5c2cae930f2ee15733787c49c5af7910d4cbd63d95ccabc`
- `src/render/su_render_scene.c`: `c531e3dbce126e6633fc45d9ffe4c82975c50ac8bbf1bf51299090810cd6ffda`
- `src/render/su_render_shaders.h`: `e3bd0b00bb74f482e9dc127dbb66d68d82c91aa887c44b61be5044edd2eb1f8b`
- `src/render/su_render_vk.c`: `74f8398b60377030498fd87062846eb61ffab46aadf3a91f1ad9a96546a42831`
- `tests/asset/test_asset_gltf.c`: `892d91f20fb39b15c027b3cd122fbe924920d61ece6fef8b1a992b14fe6265c6`
- `tests/asset/test_asset_pak.c`: `47b4960fdcfcc4cdefd00f69b5e574ea521475f15b2b0b06a58cb480a3050a23`
- `tests/render/test_render_skinning.c`: `c4f5284cd97438ebdc8c2e129ebda163836d7828d37bcd82542c30566e86b8aa`
- `/tmp/build-skinning-asan/test_render_skinning`: `23b340ea3a543649266f06582b6337720809b19e7cdcea56b374be599b868308`
- `/tmp/build-skinning-asan/test_asset_pak`: `c2c0fccf93201c865e81e9e690fba7a3e3365fa10640fa1f722a49634dc8f7bf`
- `/tmp/build-skinning-asan/test_asset_gltf`: `d5a535d44fff70408fe16295111348285349fb66f623a57515b68291640a344b`
