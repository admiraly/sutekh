# Instanced exhaust flame volumes

`SuRenderFrameDesc.flames` / `flame_count` submit up to 4096
`SuRenderFlame` instances per frame. A zero count preserves existing rendering.
The presentation-only API version is 4; existing source callers that zero-initialize
descriptors keep their previous behaviour. Recompile consumers after updating the header.

```c
SuRenderFlame flame = {0};
flame.transform = su_mat4f_identity();
flame.transform.m[14] = -3.0f;
flame.colour[0] = 0.005f;
flame.colour[1] = 0.002f;
flame.colour[3] = 1.0f;
flame.gain = 1.0f;
flame.shape_atlas = shape_texture;
flame.gradient = gradient_texture;
flame.noise_volume = noise_texture;
frame.flames = &flame;
frame.flame_count = 1;
su_render_frame_ex(renderer, &frame);
```

## Instance parameters

| Field | Meaning |
| --- | --- |
| `transform` | Column-major affine local-to-camera-relative-world matrix. Unit box x,y in [-1,1], z in [-1,0]. Nonuniform scale, shear, rotation and reflection are supported. Translation is relative to `frame.camera.pos`, in world-oriented axes, like other effects. |
| `colour[4]` | Linear HDR RGB times intensity/alpha. Zero emits nothing. |
| `frequency` | Noise texture coordinate multiplier in local units. |
| `amplitude_start`, `amplitude_end` | Nonnegative local displacement amplitudes at the start/end of the gain-scaled length. Zero disables displacement. |
| `noise_velocity[3]` | Noise texture-coordinate velocity per second. |
| `time`, `phase` | Noise coordinates are `local_position * frequency + noise_velocity * time + phase`. Phase offsets all three axes. |
| `gain` | Clamped to [0,1]; length is `gain` local units, from z=0 toward negative z. Emission is gated by smoothstep(0.01,0.03,gain). |
| `warp` | Gradient row coordinate is clamp(gain + warp,0,1). |
| `scale` | Multiplier of the gradient alpha radius; 0 means 1. The outer unit box still clips the volume. |
| `soft_distance` | Nonnegative ray-distance fade against opaque scene depth in world units; 0 disables softness but not occlusion. |
| `atlas_rows`, `atlas_index` | Horizontal-strip atlas: rows are floored, minimum 1 (0 means 1); index is floored and clamped. Each row needs at least one texel of height. Sampling stays inside texel centres of the selected strip. |
| `steps` | Quality: 0 means 6; otherwise truncated after clamping to [2,12]. |
| `reserved` | Set to zero. |
| `shape_atlas`, `gradient`, `noise_volume` | Renderer texture handles. Missing, stale, failed, not-yet-ready or dimensionally incompatible handles skip the instance run; stats report the skip. |

All numeric fields must be finite and have magnitude <= 1e12; the transform
must be affine with absolute determinant >= 1e-12. Invalid numeric inputs,
negative amplitudes/scale/softness/atlas selection/step count, null instance arrays
with a nonzero count, and counts above 4096 reject the frame before submission.

The shape atlas and gradient are 2D textures. Gradient RGB supplies colour;
alpha supplies radius. Shape RGBA multiplies gradient RGB by shape RGB and alpha.
The texture format determines sRGB decoding, so use linear formats when authoring
numeric data. Gradient u is clamp(-displaced_z / gain,0,1). Noise RGB is decoded
from [0,1] to [-1,1], sampled with trilinear filtering and repeat on all axes.
The gradient and atlas clamp; all three use LOD 0.

`SuRenderTextureDesc.depth` extends texture creation: 0/1 retains 2D/cube
behaviour; 2..256 creates a 3D `SU_TEX_RGBA8_UNORM` image, with width/height
also <=256, one mip and no array/cube layers. `mip_data[0]` holds contiguous
z slices of tightly packed width*height*4 bytes. Both borrowed data and
`SU_RENDER_UPLOAD_COPY` follow existing asynchronous lifetime rules. Uploads
are staged a bounded number of rows/slices per frame, not a synchronous upload.

## Rendering and normalization

One instanced draw per contiguous run of the same three texture handles; group
instances by textures to keep descriptor and draw overhead low. A conservative
screen rectangle bounds each box. A ray/slab intersection clips to the near plane
and handles cameras inside or crossing the box. Depth testing is enabled,
depth writes are disabled, and the linear HDR contribution is additive, so it
also participates in bloom. No new render pass or scene-depth copy is needed.

Midpoint samples accumulate gradient RGB * shape RGB * shape alpha, with a
per-sample soft fade `clamp((surface_ray_distance - sample_ray_distance) /
soft_distance, 0, 1)`. Samples behind opaque depth contribute nothing. The original
box interval, not the occluded interval, determines spacing and normalization;
this prevents occlusion from increasing intensity. Scene background depth has
no soft fade. Transparent effects do not occlude flames.

The requested normalization is literal: divide the sum by `(local_path_length / N)`,
with a 1e-4 denominator floor for grazing rays; then multiply by colour and the
gain gate. This is **not an energy-invariant physical integral**: constant fields
scale as N squared / path length. To change quality without changing the brightness
of a constant field, scale colour intensity by `(old_steps / new_steps)^2`.
Gradient/radius/row mapping is the generic contract above, not a claim of matching
an external material. No external shader or content is used.

## Checks and timing

`test_render_flame` checks known linear-to-sRGB answers for steps 2..12,
default/clamped quality, additive batching and no depth writes, whole and partial
occlusion, analytic soft fade, unchanged empty frames, gain and length, near-plane
crossings, camera-inside, rotated cameras, affine transforms, noise/time/phase,
gradient row/warp, radius/scale, atlas selection, rejected inputs and 3D upload
restrictions. It runs under Vulkan validation and on Mesa lavapipe.

`test_render_flame --benchmark` runs at 1280x720 without validation or readback,
with timestamped complete frames, 20 warm-up frames and 120 samples per case.
It prints GPU and CPU median/p95 for 0, 32 and 128 instances at 2,6,12 steps.
Each flame is about 25x25 projected pixels: non-overlapping grid, x/y scale .12,
z scale .65 at z=-6, spacing .5/.42 units. Textures are synthetic 8x8 RGBA maps
and a patterned 16-cubed RGBA noise volume. These are small-engine workloads,
not a bound for full-screen overdraw. RDP and other GPU activity can affect tails.
See the dated handoff and evidence directory for actual measurements.
