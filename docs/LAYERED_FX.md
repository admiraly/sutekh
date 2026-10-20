# Layered unlit effects

Rendering API generation 4 adds source-compatible fields to `SuFxBatch` and
`SuRenderFrameDesc`, and `su_render_fx_material_create`. Recompile native
callers: structures are larger. Zero-initialize descriptors as before; all new
fields zero preserve the previous single-texture quad behavior. Analytic
untextured effects and lit material shading are unchanged.

## Quad layers

`SuFxBatch.layers` is a `SuFxLayers` value. Its three texture samples are
multiplied component-wise, including alpha. Each slot is a normal renderer 2D
texture, with its format controlling linear/sRGB decoding. A zero slot is white;
for quads only, zero slot 0 instead uses the existing `batch.texture` handle.
There must still be a base texture for a quad. Nonzero slots must all be READY
2D textures; a pending, stale, released, or cube slot skips the entire draw and
increments `fx_batches_skipped`. No borrowed resource is retained: keep textures
and materials alive while their meshes are in use.

Each layer has independent `uv[index].scale`, `.offset`, and `.speed`, all two
floats. The coordinate is `uv * scale + offset + speed * time_seconds`. A zero
scale component means 1, preserving identity transforms for zero initialization;
negative scales mirror. Offsets and speeds default to zero. Set
`SuRenderFrameDesc.time_seconds` once per frame; time need not advance or be
monotonic. Time, transforms and angular strength must be finite. Invalid quad
transforms skip the batch; invalid material descriptions or frame time are
rejected. Transform offset arithmetic uses double intermediates and saturates
at +/-1e30 before uploading floats.

```c
SuFxBatch batch = su_fx_batch(base, SU_FX_BLEND_ADD, SU_FX_WRAP_REPEAT,
                             SU_FX_WRAP_REPEAT, first_vertex, &buffer);
batch.layers.textures[1] = detail;
batch.layers.textures[2] = mask;
batch.layers.uv[0].speed[0] = 0.2f;
batch.layers.uv[1].speed[1] = -0.4f;
batch.layers.uv[1].scale[0] = 2.0f;
frame.time_seconds = elapsed_seconds;
```

The existing batch wrap modes apply to each layer. `atlas_cols`, `atlas_rows`,
and `atlas_frame` optionally select a cell of **slot 2 only**. Zero dimensions
mean 1. Frames run left-to-right, top-to-bottom and wrap modulo the grid size.
This is per batch/material, not per vertex; use multiple batches/materials for
different cells. Scrolling/scale happen in cell-local coordinates; clamp,
repeat, and mirror address within that cell before the atlas transform. The
sample center is clamped to a half-texel inset, with unwrapped gradients retained
for mip selection. Pad atlas cells for mipmapped/anisotropic filtering: this is
not a separate texture array and broad footprints may still reach neighboring
cells. Zero-gradient coordinates explicitly sample mip 0 instead of entering
the anisotropic gradient path (also covered on the software Vulkan driver).

`angular_fade` in [0,1] multiplies the effect opacity/intensity by
`lerp(1, abs(N dot V), angular_fade)`. N is the geometric triangle normal from
camera-relative position derivatives, V points to the camera; a planar quad
therefore uses its plane normal, not its UVs. Zero disables the computation.
Mesh triangles use geometric normals too, independent of lighting normals.

Both existing blend equations remain intact, substituting the product of
texture samples for the old single sample. Additive output is straight texture
RGB times texture alpha and vertex/object alpha; premultiplied output assumes
each input texture is already premultiplied. `soft_distance` and
`SU_FX_BATCH_SORT` still work for layered quads; sorted templates carry their
complete layer, atlas and fade state.

## Resident FX meshes

Create an FX material, supply its handle in `SuRenderMeshDesc.materials`, then
use the resulting model with ordinary `SuRenderObject` instances:

```c
SuRenderFxMaterialDesc material = {0};
material.layers.textures[0] = base;
material.layers.textures[1] = detail;
material.layers.textures[2] = mask;
material.layers.uv[0].speed[0] = 0.2f;
material.layers.uv[1].speed[1] = -0.4f;
material.blend = SU_FX_BLEND_ADD;
material.two_sided = 1;
material.wrap_u = material.wrap_v = SU_FX_WRAP_REPEAT;
material.soft_distance = 0.5f;
su_render_fx_material_create(renderer, &material, &handle);
```

All three mesh slots zero produce an untextured white multiplier. Mesh UV0 is
shared by the independently transformed layers; `SuRenderObject.tint` supplies
linear HDR RGB and opacity. There is no lighting, material PBR factor,
object-emissive multiplier, or CPU triangle reconstruction. HDR values remain
unclamped in the HDR scene target and feed normal exposure/bloom.

Meshes use reversed-Z depth testing against opaque/masked geometry, never
write depth, and default to back-face culling. Any nonzero `two_sided` disables
culling. `blend` selects additive (zero) or premultiplied. `soft_distance` uses
the same world-unit intersection fade and background behavior as quad batches;
it must be finite and nonnegative. Wrap values and blend must be valid enum
values. Unknown/cube textures are rejected at creation; pending textures may be
supplied, but the mesh draw waits for every nonzero slot to become READY.

FX meshes automatically participate in the existing stable back-to-front node
distance sorting **with PBR and paint translucent meshes**. Contiguous compatible
instances still batch into indexed GPU draws. They never cast opaque shadows.
Quad batches retain their separate stream after transparent meshes; there is no
global mesh/quad sort or per-triangle sorting of intersecting transparent meshes.
No extra mesh sort flag is required; `SU_FX_BATCH_SORT` remains a quad-only flag.

## Verification and measurement

`test_render_texfx` covers exact blend math, three-layer multiplication,
HDR object intensity, independent scale/offset/positive and negative scrolling
of every slot, atlas selection and scrolling with all wrap modes, angular fade,
mesh/quad pixel equivalence, soft intersections, depth rejection, no depth writes,
both cull modes, mesh sorting across material families, stale/invalid resources,
invalid parameters and zero-initialized compatibility. Vulkan validation must
report zero errors and warnings; no software-driver skips are added.

Run `test_render_texfx --no-validation --bench-layered` for GPU timestamp and CPU
min/median/p95 observations: 1024x1024, 16 full-screen overlapping quads or resident
mesh instances, one versus three independently scrolling 128x128 textures,
20 warmup frames and 120 samples per mode, no explicit readback. The offscreen
renderer still includes its normal output copy. These are whole-frame costs,
not isolated shader timers; shared GPU load and remote desktop affect results.
See `planning/handoffs/2026-10-20-layered-fx.md` for measured evidence and caveats.
