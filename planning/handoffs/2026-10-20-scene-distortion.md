# Scene-colour distortion (#84)

## Implementation state

Implemented: a full-resolution signed displacement field, depth-tested FX
quads and resident FX meshes, normal-map/procedural vortex/ring modes, strength,
radial falloff, optional independent chromatic offsets, soft/angular fade,
per-frame extra cube shading, HDR resolve before transparent effects and
bloom/tonemapping. Zero-initialized descriptors preserve ordinary FX.
The public issue description was rewritten using generic rendering terms.

The work started from `origin/main` at `7e3c6eb` in an isolated worktree.
Rendering API generation 8 enlarges the batch, FX material, frame and stats
structures; native consumers must recompile. No simulation state changes.

Modified paths:

- `include/sutekh/render.h`
- `src/render/su_render_vk_int.h`
- `src/render/su_render_vk.c`
- `src/render/su_render_res.c`
- `src/render/su_render_scene.c`
- `src/render/shaders/fx_layers.glsl`
- `src/render/shaders/distortion.frag`
- `src/render/shaders/distortion_resolve.frag`
- `src/render/su_render_shaders.h` (generated, not hand edited)
- `tools/compile_shaders.py`
- `cmake/SutekhPresentation.cmake`
- `tests/render/test_render_distortion.c`
- `tests/render/test_render_offscreen.c`
- `tests/render/test_render_assets.c`
- `docs/SCENE_DISTORTION.md`
- `docs/RENDERING_AND_ASSETS.md`
- this handoff and `PACK_VALIDATION.json`

## Validation

Windows 11, Clang 23.1.3 release build, MSVC 19.44.35229 `/W4 /WX`, SDK
10.0.26100, Python 3.13.15, glslang 16.6.0 targeting Vulkan 1.2.
The environment was loaded with `tools/env-windows.ps1`; MSVC additionally
requires the SDK's x64 `rc.exe` directory on PATH for manifest linking.

Completed targeted commands:

- `python tools/compile_shaders.py --compiler <glslang-16.6.0 executable>`:
  regenerated the embedded header.
- `cmake --build build-distortion --target test_render_distortion test_render_assets`:
  passed; the initial implementation added a real resolve draw, so two old
  draw-count assertions were updated (not suppressed).
- `SUTEKH_REQUIRE_VALIDATION=1 ctest --test-dir build-distortion -R '^render_(distortion|assets)$' --output-on-failure`:
  2/2 passed, no validation errors or warnings.
- `SUTEKH_REQUIRE_VALIDATION=1 ctest --test-dir build-distortion-msvc -R '^(render_distortion|render_texfx|render_haze|render_flame|render_bloom)$' --output-on-failure`:
  5/5 passed, no validation errors or warnings, synchronization validation on.

The new synthetic pixel suite covers exact shifted pixels, signed strength,
zero strength/opacity, falloff, screen-edge clamping, chromatic channels,
foreground rejection at both source and destination, no effect depth writes,
transparency ordering, bloom, ring radius/width, vortex, both mesh culling
pipelines, quad/mesh parity, cubes/intensity/stale/wrong-kind handles, sorted
batches, overlap ordering, invalid fields and skipped draws.

`SUTEKH_REQUIRE_VALIDATION=1 ctest --test-dir build-distortion --output-on-failure -j 2`:
117 passed, 2 platform skips (`path_fifo`, `containment`), zero failures, 62.03 s.
All render tests passed with zero validation errors/warnings.
`python tools/compile_shaders.py --check`: 30 shader sources and 2 includes
fresh. `git diff --check`: passed. Merging fetched `origin/main` before the
final API version check reported already up to date; generation 8 is one bump
over generation 7 on that base.

Source SHA-256:

- `distortion.frag`: `5506f932f3d35b59d933cc319123ccc012b8838b4ddddd513ef887d0c59ea21f`
- `distortion_resolve.frag`: `c60de59174e9121ee471516760827f4fe331c778b6109e474d278cfbee4809d9`
- generated shader header: `d1afac9cb4ba1e9656eed1459113fb877c2bc3fd75c47866524c28cf6bad31a7`

## Measurement protocol

Quadro P600, Windows 11 remote desktop, Vulkan API reported as 4211000,
driver version 2441347072. Shared machine: there is no GPU exclusivity or
stable-clock guarantee. Use these as observations, not budgets or isolated
pass-cost qualification. The first attempt had nonmonotonic medians and was
discarded; a repeat is retained with its high p95 values, not hidden.

`test_render_distortion --benchmark 1280 720` and
`test_render_distortion --benchmark 1920 1080` disable validation/readback.
Each case warms 30 frames and collects 180 timestamp/CPU samples. The workload
is a procedural sky and 0, 1 or 32 fully overlapping large vortex quads,
falloff 1, strength 16 pixels, chromatic split 0.1, no bloom or cube. Quad
half-extents are (3.2,1.8) at depth 4; vertical FOV 60 degrees, aspect 16:9.
The off case includes the fixed resolve and all reported GPU times are whole
frames, not isolated pass timestamps. See `docs/SCENE_DISTORTION.md`.

Final repeat, GPU/CPU milliseconds:

| Resolution | Effects | GPU p50 | GPU p95 | CPU p50 | CPU p95 |
|---|---:|---:|---:|---:|---:|
| 1280x720 | 0 | 1.680128 | 14.417984 | 0.047200 | 0.100100 |
| 1280x720 | 1 | 2.250144 | 26.838528 | 0.074900 | 0.129800 |
| 1280x720 | 32 | 7.304704 | 33.675200 | 0.104700 | 0.183700 |
| 1920x1080 | 0 | 3.363648 | 29.112000 | 0.074000 | 0.118000 |
| 1920x1080 | 1 | 4.773920 | 30.367776 | 0.082600 | 0.151500 |
| 1920x1080 | 32 | 15.071104 | 41.823296 | 0.128500 | 0.213300 |

Median differences from off: +0.570016 / +5.624576 ms at 720p (1/32 effects),
+1.410272 / +11.707456 ms at 1080p. These are differences of whole-frame
medians, not directly measured per-pass costs. Both benchmark invocations
reported zero failures and validation disabled as requested.
The immediately preceding repeat was 1.693/2.316/7.318 ms at 720p and
3.392/4.839/15.220 ms at 1080p, supporting the median trend but not the tails.

Measured release executable SHA-256:
`11e2584f70012462cb583b83b6f29315603b16aa80fd3be9c41daf4463567561`.
It was built from this patch with Clang 23.1.3 in release mode; sources are
identified by the committed patch and shader hashes above. No downloaded
assets or external material code were used.

## Limitations and next work

No correctness blockers identified locally. CI on Linux GCC/lavapipe, native
tier and the checked runtimes remains required before merge.
The full-resolution field/snapshot allocate 24 extra bytes/pixel, and the
resolve runs even when the field is empty. Lazy targets and an off-path bypass
are possible follow-up optimizations. Overlap intentionally uses last nonzero
writer, not additive refraction or multiple refracted layers. Nearest scene
sampling avoids silhouette leakage but can alias with subpixel animation.
The extra cube is frame-wide and view-aligned, not per-material physical
reflection. Refraction cannot reconstruct hidden geometry.
