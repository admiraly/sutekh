# Scene-colour distortion (#84)

## Implementation state

Implemented: a full-resolution signed displacement field, depth-tested FX
quads and resident FX meshes, normal-map/procedural vortex/ring modes, strength,
radial falloff, optional independent chromatic offsets, soft/angular fade,
per-frame extra cube shading, HDR resolve before transparent effects and
bloom/tonemapping. Zero-initialized descriptors preserve ordinary FX.
The public issue description was rewritten using generic rendering terms.

The work started from `origin/main` at `7e3c6eb` in an isolated worktree.
Rendering API generation 10 enlarges the batch, FX material, frame and stats
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
After merging main including GPU particles: 120 passed, 2 platform skips
(`path_fifo`, `containment`), zero failures, 64.90 s. MSVC's integrated
render/point-light/particle subset passed 9/9, again with required
synchronization validation and no messages.
All render tests passed with zero validation errors/warnings.
`python tools/compile_shaders.py --check`: 31 shader sources and 3 includes
fresh after integration. `git diff --check`: passed. Merging fetched `origin/main` before the
initial API version check reported already up to date. During CI, point lights
merged into main (`9971957`), advancing the render API to 8. That main revision
was merged (not rebased); the frame fields from both features are retained,
instance stride/vertex inputs were checked, and distortion advances the
integrated API once to generation 9. GPU particles subsequently merged into
main (`29894fa`), advancing its API to 9. This main revision was also merged;
particle frame/stats fields and the compute shader are retained, particle
draws remain in the ordinary transparency subpass, and distortion's final
API generation is 10, one above current main. The shader header was regenerated from
the merged sources rather than resolving generated words by hand.

Source SHA-256:

- `distortion.frag`: `5506f932f3d35b59d933cc319123ccc012b8838b4ddddd513ef887d0c59ea21f`
- `distortion_resolve.frag`: `c60de59174e9121ee471516760827f4fe331c778b6109e474d278cfbee4809d9`
- generated shader header: `cf3bc1ed049c412370bbedccaf8cf29cb375521f698e724f2e41a0a354102a94`

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
| 1280x720 | 0 | 1.693184 | 26.438592 | 0.067700 | 0.127600 |
| 1280x720 | 1 | 2.276608 | 27.195840 | 0.082800 | 0.142800 |
| 1280x720 | 32 | 7.455392 | 33.161728 | 0.157900 | 0.201000 |
| 1920x1080 | 0 | 3.287200 | 28.634368 | 0.095500 | 0.153700 |
| 1920x1080 | 1 | 4.743840 | 30.894144 | 0.118600 | 0.188600 |
| 1920x1080 | 32 | 15.137312 | 41.948320 | 0.163300 | 0.277500 |

Median differences from off: +0.583424 / +5.762208 ms at 720p (1/32 effects),
+1.456640 / +11.850112 ms at 1080p. These are differences of whole-frame
medians, not directly measured per-pass costs. Both benchmark invocations
reported zero failures and validation disabled as requested.
A prior repeat was 1.693/2.316/7.318 ms at 720p and
3.392/4.839/15.220 ms at 1080p, supporting the median trend but not the tails.

Measured release executable SHA-256:
`5f313c5c19af3273f7e23c915a3f316e7ff19fee14cd1b16c392ce0194251e8c`.
It was built from this patch with Clang 23.1.3 in release mode; sources are
identified by the committed patch and shader hashes above. No downloaded
assets or external material code were used.

## Limitations and next work

No correctness blockers identified locally. CI on Linux GCC/lavapipe, native
tier and the checked runtimes remains required before merge.
The initial CI run passed GCC 16.2/lavapipe, the native tier and Windows
checked runtime, but Ubuntu's older synchronization validator reported a
store-after-sampled-read hazard for the snapshot and field at subpass 2's end.
Both images now remain input attachments through subpass 3, placing their
stores after the resolve-read dependency. This is an explicit resource-lifetime
fix, not a validator suppression; the current local layer had not reported it.
The next checked Linux run (39463091219) passed in 8m49s, confirming zero
messages with that older validator before the final GPU-particle integration.
The full-resolution field/snapshot allocate 24 extra bytes/pixel, and the
resolve runs even when the field is empty. Lazy targets and an off-path bypass
are possible follow-up optimizations. Overlap intentionally uses last nonzero
writer, not additive refraction or multiple refracted layers. Nearest scene
sampling avoids silhouette leakage but can alias with subpixel animation.
The extra cube is frame-wide and view-aligned, not per-material physical
reflection. Refraction cannot reconstruct hidden geometry.
