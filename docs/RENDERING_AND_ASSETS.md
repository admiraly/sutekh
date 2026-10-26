# Rendering, assets, and the shipping player

Deterministic compute emitters and their CPU reference: [GPU particles](GPU_PARTICLES.md).

Layered unlit quad and resident mesh effects are implemented in
[Layered FX](LAYERED_FX.md), including API defaults, sorting and validation.
**Later-stage contracts. Do not implement these before the headless gates.**

## 1. Simulation/presentation boundary

The renderer consumes committed presentation snapshots and effect events. It cannot mutate authoritative simulation state through entity pointers or GPU readbacks. Interpolation, cosmetic particles, camera effects, and animation presentation can run at a different rate from the semantic simulation tick.

GPU scene data uses stable object/asset IDs and explicit lifetime generations. The renderer owns GPU buffers, descriptors, pipelines, fences, and retirement. Their handles never appear in canonical simulation snapshots.

## 2. Initial rendering profile

Use Vulkan with a tested baseline capability profile; Vulkan 1.3 is a candidate initial desktop baseline, subject to actual device/driver support. Record required versus optional features. Headless builds never depend on Vulkan. Introduce a minimal instanced diagnostic scene before adding a GPU-driven frame graph, compute culling, animation, shadows, or large particles.

Do not promise that every unit type collapses into one draw. Material state, mesh variants, skinning, visibility, shadows, transparency, and rendering passes determine batching. The engine should reduce per-object CPU work while measuring actual GPU and CPU cost.

### Presentation targets: Win32, Wayland, X11, offscreen

`SuRenderDesc` selects the target (API 12): an `hwnd` (Windows, `VK_KHR_win32_surface`); a `window_system` with
native handles (Linux: `SU_RENDER_WINDOW_WAYLAND` with `wl_display`/`wl_surface` through `VK_KHR_wayland_surface`,
or `SU_RENDER_WINDOW_XLIB` with `Display`/`Window` through `VK_KHR_xlib_surface`, which also covers XWayland); or
neither, for the offscreen target with readback. The renderer enables only the instance extensions of the chosen
target, creates and destroys the surface itself, and needs no Wayland or X11 headers or libraries (the loader
provides the entry points). `include/sutekh/window.h` (`su_window`, SDL 3 opened at run time; decision in
[BACKENDS_AND_DEPENDENCIES.md](BACKENDS_AND_DEPENDENCIES.md) section 8) creates such a window and fills the fields
with `su_window_render_desc`; any other windowing layer can pass the same handles. An API 11 descriptor
(`SU_RENDER_DESC_V2_SIZE`) still works.

Size and pausing are the host's report, as on Win32: a Wayland surface does not report its extent, so the
swapchain takes the size from `SuRenderDesc.width/height` and `su_render_resize` (pixels, clamped to the surface
limits). `su_render_resize(r, 0, 0)` pauses presentation: frame calls return `SU_E_RESOURCE_LIMIT` without
acquiring an image until a non-zero size arrives, or until the surface reports a non-zero extent different from
the one at the pause (a Win32 window restored from a minimize resumes without a resize call; an iconified X11
window keeps its extent and stays paused). Image acquisition waits at most 100 ms; a timeout skips the frame
(`SU_E_RESOURCE_LIMIT`), so a compositor that stops releasing images without reporting the window as suspended
cannot stall the host loop and its simulation ticks. `su_window_poll` reports a minimized, hidden or occluded window
(including a Wayland window the compositor suspends) as 0 x 0, so the same `resized` forwarding pauses and resumes
it, and no frame acquires an image of a surface the compositor no longer shows. Wayland has no
client request to unminimize a window.

Observed on the integration machine (KDE Plasma Wayland, Intel Arc A770, Mesa 26.2.4): FIFO on Wayland and on
XWayland, swapchain recreation on resize; under bare Xvfb (no DRI3) the hardware drivers cannot present, the
renderer picks lavapipe/llvmpipe, which presents through the X11 software path (`window_swapchain_xvfb`).

### 2.1 HDR scene, tonemap, bloom and exposure

The scene (sky, opaque, masked and blended meshes, debug items, additive effects) renders into an `R16G16B16A16_SFLOAT` target; a final output pass applies exposure and the ACES-fitted tonemap (or a clamp) and writes the sRGB swapchain or offscreen image. The HDR format is part of the baseline: a device that cannot render to and sample it does not meet the baseline (`su_render_enumerate`, `why_not`). Bloom additionally needs linear filtering of that format (`SuRenderDeviceInfo.bloom`); without it `SuRenderPost` requests are ignored and the frame is tonemapped plain.

`SuRenderFrameDesc.post` (`SuRenderPost`) is zero for "off", so frames that do not set it render exactly as before. With `SU_POST_BLOOM`: a half-resolution mip chain, 13-tap downsample (Jimenez 2014) whose first level applies the threshold with a soft knee and Karis weights, tent upsample with a `radius` scale, added to the scene before exposure. Threshold is scene-linear (before exposure). The chain is energy preserving per level and divided by the level count at the output, so `bloom_intensity` 1 adds back about the energy that passed the threshold; intensity 0 gives byte-identical output to no bloom. Emissive meshes (material `emissive` x `emissive_strength` x per-object `emissive`), additive billboards and ribbon trails all write values above 1 and bleed through it. `SU_POST_AUTO_EXPOSURE` sets exposure from the log-average luminance of the frame (read from the same chain, no readback), `key / average`, clamped to `[exposure_min, exposure_max]`, times `light.exposure`; it keeps no state between frames, so it is deterministic but not temporally smoothed. `su_fx_glare` is an optional CPU helper that adds a core, halo and two streaks for very bright points. None of this reads or writes simulation state. Cost and test evidence: `planning/handoffs/2026-10-18-hdr-bloom.md`.

### Premultiplied-alpha compositing output

Render API version 11 appends `SuRenderFrameDesc.output_alpha`; recompile native consumers.
`SU_RENDER_OUTPUT_ALPHA_OPAQUE` (zero, including zero-initialized frames) preserves the previous output.
Set `SU_RENDER_OUTPUT_ALPHA_PREMULTIPLIED` to render over transparent black, ignoring both procedural/cube
backgrounds and `light.clear_color`. Environment lighting, exposure, and the selected tonemap still apply.
Unknown modes reject the frame with `SU_E_INVALID_ARGUMENT`.

RGBA8 readback contains sRGB-encoded RGB and linear coverage alpha. Opaque and surviving masked fragments
write coverage one; discarded masks leave it untouched. Blended surfaces, premultiplied textured effects,
and over-blended haze accumulate coverage with `a_src + a_dst * (1 - a_src)`. Translucent instance tints
participate in the same rule. Decal/overlay materials and the HUD change RGB without changing coverage.
Additive quads, meshes, flames, additive haze, emissive light and bloom do not add coverage; bloom's
log-luminance alpha is never substituted for the scene coverage. Glow can therefore have nonzero RGB at
zero alpha. This is an emission-bearing premultiplied representation, not straight-alpha image data.

Decode readback RGB from sRGB, then composite in linear output-light space:
`result = foreground_rgb + background_rgb * (1 - coverage)`. Do not multiply foreground RGB by alpha
again, and do not discard RGB at zero alpha. Encode the result for the destination afterward. An image
tool that assumes straight alpha or erases transparent RGB will lose glow. This mode also works for
the window output attachment, but does not enable operating-system window transparency.

RGB is exposed and tonemapped over black exactly as in the ordinary post pipeline, without dividing by
coverage or multiplying by it again. Nonlinear ACES/clipping, bloom thresholding, and scene-dependent auto
exposure do not commute with background composition: output-space compositing cannot generally equal a
scene-linear render with the background included before those operations. Direct-background equivalence
is tested with clamp tonemapping, unit exposure, no clipping and bloom disabled; bloom/ACES/auto-exposure
tests separately verify RGB output and unchanged coverage. Use fixed exposure for predictable footage
matching. No new render targets, passes, or readbacks are needed; optional RGBA16F readback is not added.

The synthetic coverage/compositing tests run in `render_bloom`, with textured quad/mesh coverage in
`render_texfx`, flame coverage in `render_flame`, and additive/over haze coverage in `render_haze`.
`render_bloom` reports alternating legacy/premultiplied CPU and GPU median/p95 samples at 256x256 with
bloom enabled; GPU values of -1 mean timestamps are unavailable. These are observations, not CI budgets.

### Dynamic point lights

Render API version 8 adds `SuRenderPointLight` and the appended
`SuRenderFrameDesc.point_lights` / `point_light_count` fields. Recompile native consumers after upgrading.
Populate a caller-owned array for each `su_render_frame_ex` call; the renderer copies it during the call,
so it need not survive submission. No handles, resource creation, persistent allocations or light shadows
are involved. Zero count accepts a null pointer and preserves the previous image byte for byte.

Each light has a Q32.32 world `pos`, linear RGB `color`, nonnegative `intensity`, and positive finite
`radius` in world units. RGB and intensity must be finite and nonnegative, including their product.
An invalid entry, null pointer with nonzero count, or count exceeding `SU_RENDER_MAX_POINT_LIGHTS` (128)
rejects the frame with `SU_E_INVALID_ARGUMENT`; nothing is silently truncated. Zero intensity disables a light.

At distance `d < radius`, attenuation is `(1 - (d / radius)^4)^2 / max(d*d, 1)`;
it is zero at and beyond the radius. The squared window has zero slope at its outer edge, and the
one-unit inverse-square soft core bounds near-field energy. At the exact light position the contribution
is zero because the incoming direction is undefined. Intensity is the radiant-strength multiplier,
not the brightness at the edge of the sphere. Choose radius independently to bound influence.

PBR-lite and paint use the same GGX distribution, correlated Smith visibility, Schlick Fresnel and
Lambert diffuse as their sun term, including paint layers, normal/detail maps and RGB specular-colour F0.
Point light is added after sun shadowing and before ambient/emission; AO affects ambient only.
Unlit materials, debug primitives, volumes, sky and additive effects do not receive it.
Opaque, masked, blended and overlay mesh materials share this lighting path.

Positions are made camera-relative with integer subtraction before conversion to float. CPU sphere
intersection builds a conservative 128-bit mask per visible model node (including its scale and transforms),
shared by its submeshes. The fragment shader iterates only set bits and skips out-of-radius/back-facing
fragments. This keeps the existing instanced batching and descriptor bindings: the frame UBO grows by
4096 bytes and each internal draw instance by 16 bytes. Worst case remains linear in overlapping lights;
this is a bounded forward path, not a clustered renderer.

```c
SuRenderPointLight flash = {0};
flash.pos = su_wpos(10.0, 2.0, -20.0);
flash.color[0] = 1.0f;
flash.color[1] = 0.3f;
flash.color[2] = 0.1f;
flash.intensity = 20.0f;
flash.radius = 15.0f;
frame.point_lights = &flash;
frame.point_light_count = 1;
su_render_frame_ex(renderer, &frame);
```

Run `test_point_lights` for portable validation/culling checks and `test_render_paint` for synthetic pixel
regressions on hardware or lavapipe. Set `SUTEKH_POINT_LIGHT_BENCH=1` when running `test_render_paint`
for the 1280x720 full-screen PBR-lite/paint benchmark: 0/8/32/128 coincident overlapping lights and
the same counts wholly culled, 40 warm-up frames and 100 GPU timestamp/CPU submission samples per case.
It reports median and p95 in milliseconds with validation disabled for the measured renderer.

### 2.2 Layered paint material and image-based ambient

An optional second material model (`SuRenderPaintDesc`, or a material entry of `type_version` 2 in a pack). The
surface blend is specified exactly in `include/sutekh/render.h` ("layered paint material"); in short: base albedo,
roughness and metalness are blended toward each layer by `mask[k] * opacity`, in `layer_order`, by lerp or multiply;
glow = glow map x glow colour x intensity x `SuRenderObject.emissive`, layers may tint it; dirt darkens and roughens;
AO multiplies ambient only. Colour space per slot is fixed (albedo and glow sRGB, everything else linear) and checked.
Ambient and reflections come from `SuRenderFrameDesc.env`, a cube with a CPU-prefiltered GGX mip chain (RGBA16F,
split-sum scale and bias; diffuse from the roughest mip), or from the procedural sky when none is given.

**glTF mapping** (`SUTEKH_materials_layered_paint` on a material; list it in `extensionsUsed`). Core fields keep their
meaning: `baseColorTexture` albedo (sRGB), `normalTexture` normal (cooked as BC5), `emissiveTexture` glow (sRGB),
`occlusionTexture` AO, `baseColorFactor`, `metallicFactor`, `roughnessFactor` (factors multiplying the maps),
`emissiveFactor` x `KHR_materials_emissive_strength` the base glow colour and intensity, `normalTexture.scale`,
`occlusionTexture.strength`, alpha and `doubleSided`. The extension object adds:

| key | meaning |
| --- | --- |
| `roughnessTexture`, `materialTexture`, `maskTexture`, `dirtTexture` | `{index, texCoord}` (linear) |
| `glowMaskTexture` | `{index, texCoord}` (linear): a one-channel glow mask used instead of `emissiveTexture`; glow = mask x `emissiveFactor` x strength |
| `channels` | `{roughness, metal, specular, dirt, ao, glow}`: 0..3 = r g b a, -1 = do not read the map (`glow` applies to `glowMaskTexture`) |
| `specular` | dielectric specular scale, F0 = 0.08 x specular (default 0.5) |
| `dirt` | `{amount, darken, roughen}` in 0..1 |
| `layers` | up to four `{color [r,g,b] linear, roughness, metallic, opacity, blend "lerp"/"multiply"/"tint", glowColor [r,g,b], glowStrength}`; layer k is mask channel k; `glowColor` present enables the glow tint |
| `order` | permutation of the layer indices, bottom to top (default identity) |

Malformed fields produce an import warning and keep the default; a material without the extension cooks exactly as
before. The cooked layout is `SuAssetMaterial` + `SuAssetPaint` (`include/sutekh/asset.h`). Tests: `test_render_paint`.

#### Runtime specular-colour and tiled detail options

Zero-initialize `SuRenderPaintDesc`, then initialize `factors` and `paint` with their existing default functions.
The extra fields are runtime-only: existing version-2 packs and the glTF extension are unchanged. Applications must
recompile against the extended header; binary ABI compatibility with an older descriptor is not promised.
`SU_PAINT_SLOT_COUNT` remains eight for the cooked format; the runtime `textures` array now has
`SU_RENDER_PAINT_SLOT_COUNT` (12) entries. Zero extra fields and absent extra textures retain the old metalness path.

- Bind `textures[SU_PAINT_SLOT_SPECULAR]` to an RGB F0 map (linear or sRGB). Sampling decodes sRGB before multiplying
  by `specular_factor[3]` (linear RGB, each in 0..1; explicitly set to 1 for an unscaled map). The result is clamped
  to 0..1. A bound map selects this workflow even when its factor is zero. F0 is independent of base colour, tint,
  layers and dirt. Diffuse uses the fully blended albedo without a metalness multiplier. Direct diffuse is
  `(1 - Schlick(F0, V.H)) * A / pi`; the existing GGX specular normalization is unchanged. Ambient becomes
  `diffEnv * A * (1 - envBRDF(F0)) + specEnv * envBRDF(F0)`, multiplied by ambient intensity and AO.
- `specular_channels` packs one byte per output RGB component: 0 uses the corresponding input component;
  1, 2, 3, 4 select R, G, B, A; 5 supplies 1. Thus zero is identity, and `1 | (1 << 8) | (1 << 16)` replicates R.
- Set `flags |= SU_PAINT_NORMAL_Z_ONE` for `normalize(x * scale * T + y * scale * B + N)` instead of reconstructing
  tangent-space Z from XY. Zero retains reconstructed Z. This option applies to the base normal.
- The extra slots use `extra_tex_info[slot - SU_PAINT_SLOT_COUNT]`, the existing `SU_ASSET_TEX_INFO` encoding
  (UV0/UV1 and independent repeat/clamp/mirror wrap). Their zero value is UV0 and repeat. The detail slots must be linear.
- `detail_uv_scale[2]` scales each detail slot's selected UV before `detail_rotation` (radians, counterclockwise around
  the UV origin). A zero scale component means 1; negative scales mirror. Detail normals rotate back into base UV
  tangent axes. These controls do not change any base texture coordinates.
- `detail_strength` is 0..1; zero bypasses all detail reads. `detail_layer_mask` selects base mask channels with bits
  0..3, and the unpainted remainder with bit 4. Zero selects the whole surface; selected weights sum and clamp to 1.
  The remainder is `1 - clamp(sum(base mask RGBA), 0, 1)`, before layer opacity/order. Weight = strength times selector.
- `flags |= SU_PAINT_DETAIL_MATERIAL` blends `textures[SU_PAINT_SLOT_DETAIL_MATERIAL]` into the base material-map
  texel by weight times the unpainted remainder. The existing metal/specular channel selectors consume this texel;
  subsequent paint layers retain their existing overrides. This is a tiled replacement of the base material scalar,
  not a new material-ID palette system. A missing map is white, like the base material map.
- `flags |= SU_PAINT_DETAIL_NORMAL_DARKEN` adds the weighted XY of `textures[SU_PAINT_SLOT_DETAIL_NORMAL]` to the
  base tangent normal before normalization, and multiplies albedo by
  `1 - weight * textures[SU_PAINT_SLOT_DETAIL_MASK].r`. Missing normal/mask are neutral flat/black. The mask darkens
  diffuse only, not glow or independent F0. Both detail modes may be combined.

Unknown flags, invalid channels/UV selectors/wraps, out-of-range strengths/factors, nonfinite controls, invalid
texture handles and sRGB detail maps are rejected. Texture uploads and release use the existing deferred resource
lifetime rules. Synthetic pixel tests cover the new workflows on hardware and CPU Vulkan devices.
For an optional whole-frame GPU timestamp observation, run `SUTEKH_PAINT_BENCH=1 test_render_paint --no-validation`:
1920x1080 full-screen paint, 40 warm-up frames then 300 samples per mode (legacy options disabled, F0 with fixed-Z
normal, material detail, normal/darkening detail, F0 plus normal/darkening detail). This is not a shipping budget.
### 2.2 Directional sun shadows (optional)

`SuRenderFrameDesc.shadow` (`SuRenderShadow`) is zero for "off": nothing extra is allocated or drawn and frames render
exactly as before. With `SU_SHADOW_ON` a single shadow map (`size`, default 2048) is fitted each frame in the sun's frame
to the visible objects (and, with `distance` > 0, to the view frustum slice up to that distance, for close-ups of huge
hulls), with texels snapped to a world-space grid so it does not shimmer as the camera moves. Casters are the opaque
objects without `SU_RENDER_OBJECT_NO_SHADOW` reaching into that box from the sun's side, drawn depth-only at the LOD the
camera selects; blended materials and translucent objects do not cast. PBR-lite and paint materials multiply the sun's
direct light (not ambient or glow) by a 3 x 3 percentage-closer filter of the map, scaled by `strength`. Cost: one
depth-only pass over the casters and nine shadow taps per lit pixel. `cascades` 2..4 split the view (uniform/log blend
`split_lambda`, default 0.75) into one map layer each, fitted by the slice's bounding sphere and snapped to world texels;
0/1 keeps the single map. Measured on a Quadro P600 at 1920 x 1080 with 400 sphere casters (300 k triangles): scene
4.73 ms; one 2048 map +0.66 ms; 2 cascades of 2048 +1.25 ms; 4 of 2048 +2.64 ms; 4 of 1024 +1.49 ms
(`SUTEKH_SHADOW_BENCH=1 test_render_paint`). Tests: `test_render_paint` (`test_shadows`, `test_shadow_cascades`).

### Vulkan sampler portability

Mesa 25.0.7 lavapipe returns NaNs when an anisotropic sampler reads a texture with constant UVs (zero implicit
derivatives). This includes the renderer's white 1x1 fallback, so even an untextured, unlit opaque material can
render black; it is not an alpha-blending or lighting failure. The same draw works with nonconstant UVs, without
anisotropy, and on the tested hardware driver. Synchronization validation reports no warnings or errors.

The backend disables anisotropy only for `VK_DRIVER_ID_MESA_LLVMPIPE` with version 25.0.7 parsed from
`VkPhysicalDeviceDriverProperties.driverInfo`. Its legacy `driverVersion` is 1, not the Mesa version. Linear
minification/magnification, trilinear mip filtering and wrap modes are unchanged. Other drivers and Mesa versions
retain anisotropy; do not widen the workaround without a failing reproduction. An upstream Mesa report with the
reduced case would be appropriate; a newer-driver result has not been established here.

`ctest --test-dir build -R '^render_(ghost|sampler)$' --output-on-failure` covers the original translucent boxes and
a reduced case, also available as `test_render_ghost WORKDIR --sampler-repro`: one unlit opaque triangle, no pack,
no lighting, no shadows, no sky or bloom, then the same triangle at half alpha. It checks constant, varying and
one-axis UVs against both the white 1x1 fallback and a white 2x2 texture. Select lavapipe with `VK_DRIVER_FILES`
(or `VK_ICD_FILENAMES`) pointing to its ICD manifest; `SUTEKH_REQUIRE_VALIDATION=1` requires the validation layer.
To reproduce the driver defect rather than the workaround, temporarily remove `!broken_anisotropy(r)` from the
sampler's anisotropy condition and rebuild. Only constant-UV cases fail on the affected driver; the same binary
passes on the tested hardware. Diagnostic bisect and validation evidence: `planning/handoffs/2026-10-20-lavapipe-ghost.md`.

### 2.3 Analytic haze

Scene-colour distortion is available on FX quads and resident FX meshes, with
procedural vortex/ring primitives, depth rejection and optional chromatic/cube
shading. See [Scene-colour distortion](SCENE_DISTORTION.md).

`SuRenderFrameDesc.haze` submits depth-clipped ellipsoid and box media with exact
polynomial density integration, additive or extinction-over composition, and
optional sun/shadow lighting. No volumes means no change to existing frames.
See [Analytic haze volumes](ANALYTIC_HAZE.md) for parameters, ordering, precision,
reference integration and the repeatable offscreen benchmark.

## 3. Shader/pipeline edits

Shader source is an asset with explicit include dependencies and compiler/target/resource-layout fingerprints. Compile only affected variants using an established compiler adapter. Prepare driver pipelines away from the render-critical path. Keep the last compatible pipeline active until the new one is ready; use a documented fallback when an interface change makes that impossible.

Khronos documents internal shader compilation during pipeline creation and its potential frame-time cost [S11]. Pipeline cache-control features can avoid unexpectedly compiling in a guarded creation attempt on supporting devices [S12]. These mechanisms help schedule work; they do not guarantee a zero-cost first-use pipeline.

Changing descriptor/resource layouts is an interface migration, not just replacing a shader pointer. Revalidate bindings and dependent materials. Retire old GPU resources only after relevant device work completes. Pipeline caches include implementation/device compatibility metadata and are never trusted across arbitrary driver changes.

## 4. Asset pipeline

Asset identities are content- and dependency-addressed. Each importer declares source formats, dependency discovery, output version, target settings, and importer/tool version. Changing one model invalidates that model and its affected derived artifacts—not the entire project. Changes to importer logic correctly invalidate all affected outputs.

Imports run in isolated bounded workers. Malformed or hostile files cannot write outside the artifact store or execute arbitrary scripts. Large texture/mesh conversion can remain pending while the game uses the previous artifact. A viewport must expose stale/loading/error state rather than pretending an import completed.

Budget CPU staging, upload bandwidth, GPU memory peak, and resource retirement. Replacing a large texture can temporarily need both old and new allocations. A live asset edit is allowed to be rejected or deferred for insufficient resources.

## 5. BFME assets

The compatibility adapter reads original assets from a configured local installation. Importer tests use synthetic/minimal redistributable fixtures in public CI. Do not commit original archives, extracted textures/models/audio, or user-installation paths. Unknown format behavior is tracked as a compatibility gap with evidence.

Preserve semantic separation between visual upgrades and original gameplay. Changing unit mesh/particle density is not permission to change target selection, collision, attack timing, or authoritative visibility.

## 6. Audio

Audio consumes committed effect events. Use an established device/mixing backend initially; do not rewrite codecs in assembly. Real-time audio callbacks cannot block on the agent control plane, memory allocation, asset import, or GPU work. Asset updates and voice lifetime changes are prepared outside the audio callback.

Speculative candidates do not play real sounds. Development effect IDs and commit indices prevent duplicate externally visible effects after controlled recovery where supported.

## Depth-biased mesh overlays

Set `SuRenderMaterialDesc.overlay = 1` or `SuRenderPaintDesc.overlay = 1` before
creating a material for coplanar decal geometry. Zero preserves the existing
material behavior. Both paths use their normal lit fragment shader and texture
slots, including texture alpha and per-object tint alpha. Overlay mode overrides
the material alpha mode with straight-alpha blending (it does not alpha-test).

Overlays render after opaque and masked geometry, including diagnostic meshes,
and before ordinary transparent geometry. They use reversed-Z greater-or-equal
depth testing, positive constant and slope-scaled raster depth bias (both factors
1), and no depth writes. They receive the normal material lighting and shadows
but never cast shadows. Single- and double-sided variants retain material culling.
Overlays are sorted back-to-front within their phase and batch like other meshes.

Use the base surface's vertex positions with a dedicated decal index buffer and
material; no CPU vertex displacement or special shader is needed. This is a mesh
overlay, not a screen-space projected decal. Bias is intentionally small and fixed;
geometry meaningfully behind another surface remains occluded. Avoid intersecting
transparent surfaces and ambiguous same-depth overlapping decals: this is the
existing sorted-alpha renderer, not order-independent transparency.

No glTF extension or cooked asset format changes are introduced: import continues
to preserve its existing material semantics. Applications select overlay materials
through the renderer API when constructing their mesh materials. Issue #67 does
not request an import mapping.

## 7. Release player

The shipped player/server excludes agent RPC, source watchers, untrusted compilation, test imports, and development secrets. It loads a pinned semantic/content manifest and validated platform artifacts. JIT may be disabled where unnecessary or disallowed. Offline native optimization and shader preparation are packaging operations, not normal gameplay edit operations.

Maintain debug/source-map artifacts separately for profiling and crash diagnosis. Distribution size and startup time are measured alongside runtime performance. Dependency notices and the final license are owner-approved release requirements.

Sources: [SOURCE_NOTES.md](SOURCE_NOTES.md).

### 2.3 Textured effects, DDS textures and the cube-map sky

Effects can carry textures: `SuRenderFrameDesc.fx_tex` holds camera-relative triangles (`SuFxVertex`, with real uv) and
`fx_batches` names, per run of vertices, a 2D texture, a blend (`SU_FX_BLEND_ADD`: `dst + t.rgb * t.a * c.rgb * c.a`;
`SU_FX_BLEND_PREMUL`: `t.rgb * c.rgb * c.a + dst * (1 - t.a * c.a)`) and wrap modes. `c` is the per-sprite colour (linear,
HDR, so intensities above 1 feed bloom). The texture's format decides sRGB or linear. Helpers: `su_fx_sprite`,
`su_fx_sprite_axis` (a quad along a world axis that turns about it to face the camera: flames and plumes; end-on it
degrades to a sliver), `su_fx_atlas_rect` (frame to uv; keep a texel of padding between atlas cells, sampling is
bilinear), `su_trail_build_tex` (U along the trail by age or by distance, with a scroll offset and wrap `REPEAT`; V across).
Batches can fade softly into the geometry behind them (`SuFxBatch.soft_distance`: the scene depth is read as an input
attachment in a second subpass of the scene pass) and can be sorted back to front across batches (`SU_FX_BATCH_SORT`,
per quad).

`include/sutekh/dds.h` parses DDS files in memory (2D and cube, mips, BC1 to BC7 including BC6H, RGBA8, RGBA16F) and
`su_render_texture_create_dds` uploads them natively when the device supports BCn (`SuRenderDeviceInfo.bc_textures`);
a cube gets a cube image view. Trailing bytes after the pixel data are ignored. `su_render_env_from_dds` builds a
prefiltered environment from an HDR or LDR cube (BC1 to BC5 and BC7 decode on the CPU with `su_bc_decode_block`, in the
colour space chosen at parse); cubes above 1024 (or above the cap given to `su_render_env_from_dds_ex`) are
box-downsampled on the CPU; `SuRenderFrameDesc.sky_cube` draws any cube (raw or prefiltered) as the sky
background, with intensity, rotation and lod. The BC6H decoder (`su_bc6h_decode_block`) is bit-exact to the D3D11 /
OpenGL decode; it was checked against an independent decoder on four million random blocks and against the GPU on
every mode. The BC7 decoder matches the GPU within one output level on random blocks of every mode. Tests: `test_render_texfx`, `test_asset_dds`.
