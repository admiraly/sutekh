# Scene-colour distortion

The rendering API adds `SuFxDistortion` to `SuFxBatch` and
`SuRenderFxMaterialDesc`. Recompile native callers after the API version bump.
Zero-initialized fields retain ordinary FX rendering. `mode` selects a
distortion material instead of the ordinary additive/premultiplied material;
within that mode, zero strength is invisible, not an additive normal map.

## Usage

Quad: build ordinary camera-relative vertices with `su_fx_sprite`, then submit
the batch in `frame.fx_batches` and vertices in `frame.fx_tex`:

```c
batch.distortion.mode = SU_DISTORT_NORMAL;
batch.distortion.strength = 16.0f;
batch.texture = normal_map;
```

Mesh: create a resident FX material and use its handle in a mesh's material
array. Ordinary `SuRenderObject` transforms, tint alpha, camera-relative
positioning, culling and instancing apply:

```c
SuRenderFxMaterialDesc material = {0};
material.layers.textures[0] = normal_map;
material.distortion.mode = SU_DISTORT_NORMAL;
material.distortion.strength = 12.0f;
su_render_fx_material_create(renderer, &material, &handle);
```

Vortex: `batch.distortion.mode = SU_DISTORT_VORTEX;` uses the tangential
direction around UV `(0.5,0.5)` without requiring a texture. Keep strength set.

Shockwave/ring: `batch.distortion.mode = SU_DISTORT_RING;` uses the radial
direction in a procedural annulus. Animate `ring_radius` to expand it and
strength or opacity to fade it. This works on the same sprite/mesh primitive;
no tessellation or uploaded ring texture is needed.

Optional extra cube: `frame.distortion_cube = cube;` and
`frame.distortion_cube_intensity = 0.2f;` add a cube sample only at displaced
pixels. The direction is the normalized view-aligned `(offset_x,offset_y,16)`;
this is a stylized vortex highlight, not physically based reflection. This
extra cube is independent of the sky and lighting environment. Zero intensity
disables it. Missing, pending, stale or non-cube handles contribute nothing.

## Parameters

| Field | Contract |
|---|---|
| `mode` | `SU_DISTORT_NONE` (zero): ordinary FX. `SU_DISTORT_NORMAL`, `SU_DISTORT_VORTEX`, `SU_DISTORT_RING`: field output. |
| `strength` | Signed screen pixels, finite in `[-4096,4096]`. Independent of render resolution. Zero produces no field output. |
| `falloff` | Finite `[0,64]` power of `max(1-length(2*uv-1),0)`; zero is a uniform multiplier of one. |
| `chromatic_split` | Finite `[0,1]`; green samples at offset, red at `offset*(1+split)`, blue at `offset*(1-split)`. Zero uses one colour sample. |
| `ring_radius` | Finite `[0,1]` in normalized UV radius. Zero defaults to `0.75`. |
| `ring_width` | Finite `[0,1]` half-width; zero defaults to `0.15`. The annulus weight is `max(1-abs(radius-ring_radius)/ring_width,0)`. |

Normal maps must use a linear format, not sRGB: clamped RG encodes screen-space XY in
`[0,1]`, decoded to `[-1,1]`; `(0.5,0.5)` is neutral. Texture alpha and vertex/
object alpha scale strength. RGB tint does not colour the refracted scene.
The existing layer UV transforms, mask product, atlas, `soft_distance`, and
angular fade are shared with layered FX. Normal mode samples the product of
the configured layers; use the base slot for RG and alpha for masking. The
procedural modes ignore texture samples but still validate any supplied
texture handles and apply geometric angular/soft fades.

Invalid material parameters return `SU_E_INVALID_ARGUMENT`. Invalid quad
parameters or unavailable/wrong-dimensional textures skip that batch and
increment `fx_batches_skipped`. Existing capacity limits and upload lifetimes
apply. A nonfinite/negative cube intensity rejects the frame before submission.

## Ordering, depth and overlap

Four scene subpasses draw opaque/masked geometry and overlays, write the
distortion field, resolve HDR scene colour, then draw normal transparent FX,
blended meshes, haze and flames. Bloom, exposure and tonemapping consume that
resolved result. No simultaneous scene-colour read/write feedback is used.

The field is full-resolution RGBA32F: XY pixels, chromatic split and reversed-Z
effect depth. Full float depth avoids half-precision silhouette rejection
errors. Distortion depth-tests against opaque geometry, never writes depth,
and may fade softly at intersections. Each chromatic sample independently
rejects coordinates whose opaque depth is nearer than the effect, falling back
to the original pixel. Coordinates clamp to pixel centres at screen edges.
Scene colour and depth use nearest sampling to prevent bilinear taps leaking
foreground colour across a silhouette. This is depth-aware screen-space
refraction, not ray tracing: hidden surfaces cannot be reconstructed.

Overlapping fields use last nonzero writer, not additive displacement:
resident mesh runs first, then quads in their existing batch/sort order. A
zero-offset fragment does not erase an earlier field. Normal transparency is
not refracted and does not occlude the field. `SuRenderStats.distortion_draws`
counts field draws only; the fullscreen resolve also contributes to `draws`.
The resolve currently runs even with no distortion, copying scene HDR exactly;
there is a fixed fullscreen cost and two extra full-resolution images (24
bytes/pixel, about 21.1 MiB at 720p / 47.5 MiB at 1080p). Lazy allocation and
an off-path bypass are future optimizations, not zero-cost claims.

## Verification and measurement

`test_render_distortion` checks zero strength, signed normal offsets, independent
chromatic offsets, falloff, opacity, vortex, ring width/radius, opaque rejection
at both original and displaced coordinates, ordinary transparency ordering,
no depth writes, bloom participation, quad/mesh equivalence, optional cubes,
stale handles, sorted batches, last-writer overlap, and invalid input.
It uses synthetic data and runs on both hardware Vulkan and CI's lavapipe.
Set `SUTEKH_REQUIRE_VALIDATION=1` to require the validation layer, including
synchronization validation, with zero warnings and errors.

```
test_render_distortion --benchmark 1280 720
test_render_distortion --benchmark 1920 1080
```

The benchmark disables validation and readback, warms each case for 30 frames,
then records 180 whole-frame GPU timestamp and CPU submission samples (p50,
p95). Cases are off, one large effect, and 32 fully overlapping effects. Each
quad is 3.2 by 1.8 half-extents at camera distance 4 with a 60-degree vertical
FOV: approximately 78% of each viewport dimension at 16:9 before the circular
vortex mask. It uses procedural sky, vortex falloff 1 and chromatic split 0.1,
no bloom, and no optional cube. The off case includes the fixed resolve.
Measurements on a shared remote desktop are observational, not isolated GPU
performance qualification; retain raw distributions and do not infer speedup
from noisy cross-case differences. Recorded results and artifact hashes are
in `planning/handoffs/2026-10-20-scene-distortion.md`.
