# Issue 61: lavapipe constant-UV anisotropic sampling

## Task state and scope

State: implementation and local validation complete; CI and integration pending.
Owner-directed isolated scope: `fix/lavapipe-ghost`, based on `478c736`, in its own worktree.
Modified paths:

- `.github/workflows/repository-checks.yml`: remove the GCC 16.2 render_ghost exclusion.
- `cmake/SutekhPresentation.cmake`: register the reduced render_sampler regression.
- `src/render/su_render_vk_int.h`: load the Vulkan 1.1/core entry point for properties2.
- `src/render/su_render_vk.c`: identify the affected driver and disable sampler anisotropy only there.
- `tests/render/test_render_ghost.c`: add the pack-free, single-triangle sampler reproduction mode.
- `docs/RENDERING_AND_ASSETS.md`: document the driver defect, scope, trade-off and reproduction.
- `planning/handoffs/2026-10-20-lavapipe-ghost.md`: durable task state and evidence.

No public interfaces, gameplay, shader source or embedded SPIR-V changed. No third-party source was copied.

## Root-cause bisect

Windows 11, Clang 23.1.3 Release, Mesa 25.0.7 lavapipe from the local Windows toolchain;
`driverID=VK_DRIVER_ID_MESA_LLVMPIPE`, `driverInfo="Mesa 25.0.7 (git-35721f1986) (LLVM 20.1.6)"`,
legacy `driverVersion=1`. Hardware comparison: the default Quadro P600 driver on this machine.

1. Original render_assets passes; render_ghost fails all 15 expected-colour comparisons on lavapipe.
   The identical executable passes render_ghost on hardware. Validation is silent.
2. Replace the PBR fragment output with constant green: geometry reaches the framebuffer on lavapipe.
3. Output only `mat.base_color * v_tint`: all original ghost comparisons pass.
4. Multiply that output by `texture(t_base, uv_of(0u))`: black returns, even without lighting/emission.
5. Output red when `any(isnan(texture(t_base, uv_of(0u))))`: the triangle/box pixels turn red.
   Thus the sampler returns NaNs from finite constant UVs and a valid white texture.
6. Restore the original shader. Give box faces nonconstant UV0: all ghost comparisons pass with anisotropy on.
7. Restore zero UVs. Disable sampler anisotropy: all ghost comparisons pass, including opaque/mask/blend paths.
8. Reduce to one triangle, public resource API, no pack, origin camera, unlit, no sky/bloom/shadows. With the
   workaround temporarily bypassed, render_sampler fails exactly 4 of 87 checks: opaque and half-alpha with
   constant UVs for each of the 1x1 fallback and explicit 2x2 white textures. Varying and one-axis UV cases pass.
   The same unmodified-anisotropy executable passes all 87 checks on hardware.

All temporary shader, box-UV and anisotropy-bypass edits were removed. The workaround is scoped to the exact
observed Mesa release using driver properties rather than treating all CPU devices or all Mesa releases as broken.
The evidence locates the defect at degenerate-footprint anisotropic texture sampling, not at any particular Mesa
source line or internal arithmetic operation. An upstream Mesa report with this reduced case is appropriate.

## Reproducible validation

All commands run inside the isolated worktree. Source `. .\tools\env-windows.ps1` first.
Dependencies fetched with `python tools/bootstrap_vulkan_headers.py` and `python tools/bootstrap_asset_deps.py`.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSUTEKH_NATIVE=OFF -DSUTEKH_RENDER=ON -DSUTEKH_ASSET_IMPORT=ON
cmake --build build -j 4
```

Clang build passes. Set `VK_DRIVER_FILES` to the absolute path of
`local/toolchains/mesa-25.0.7/x64/lvp_icd.x86_64.json` and `SUTEKH_REQUIRE_VALIDATION=1`;
the environment script supplies `VK_ADD_LAYER_PATH` for VVL 1.4.363.0 (synchronization validation enabled).

- `ctest --test-dir build -R '^render_' --output-on-failure -j 2`: 11/11 pass on Mesa 25.0.7, with validation required.
- Original negative control: render_ghost fails 15 pixel checks on lavapipe before the workaround.
- Reduced negative control: render_sampler fails 4/87 checks when only the workaround is bypassed; hardware 87/87.
- `test_render_ghost build --sampler-repro`: 87/87 pass with the workaround and required validation.

- `ctest --test-dir build --output-on-failure -j 4`: 111 pass, 2 expected platform skips (`path_fifo`, `containment`),
  no failures, on hardware with validation required (113 registered tests).
- `python tools/validate_pack.py`: passes, including JSON schemas.
- `python tools/check_repository_license.py`: passes (CLA acceptances are not machine-checked).
- `git diff --check`: passes.

SHA-256 at the local validation checkpoint:

| Source/artifact | SHA-256 |
| --- | --- |
| `src/render/su_render_vk.c` | `1211a503d1d913669cd2b178a6f49a751a16a137a8db792ea13343a2310e6515` |
| `src/render/su_render_vk_int.h` | `006f8d47538c0d87e76671469c0e089e7b5868093378461c91c0f86bfe97fb7a` |
| `tests/render/test_render_ghost.c` | `fba9092ae4a9a3175d4022688c05b0e52e6f8007e881dde0a844f3d4709d6798` |
| `build/test_render_ghost.exe` | `f98067e889f204c95e7abd1367c6bddce150fbdf7e03d343175ccf2154bb6268` |

Unrun at this checkpoint: CI Linux/GCC/MSVC jobs and newer Mesa versions. The Windows environment cannot run the
Linux-only broker; bounded direct validation was used here. Next: rebase on origin/main, open the fixing PR,
inspect checks, integrate and remove the worktree after unlinking only its shared-toolchain junctions.
