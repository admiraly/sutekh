# Deterministic GPU particles

`SuRenderFrameDesc.particle_emitters` submits up to 16 bounded cosmetic emitters.
API generation 8 appends fields; recompile native callers and continue to
zero-initialize descriptors. No emitters means the previous rendering behavior.
Headless programs can use the CPU reference from `su_render_util` without Vulkan.

```c
SuParticleEmitter emitter = {0};
emitter.seed = 42;
emitter.capacity = 10000;
emitter.rate = 500;
emitter.burst = 100;
emitter.lifetime_ticks = 180;
emitter.velocity.y = 0.5f;
emitter.velocity_random.x = 0.2f;
emitter.gravity.y = -0.1f;
emitter.transform_start = su_mat4f_identity();
emitter.transform_start.m[14] = -4;
emitter.material.texture = sprite_texture;
emitter.material.soft_distance = 0.2f;
for (unsigned key = 0; key < 4; key++) {
    emitter.size[key] = 0.05f * (1.0f - (float)key / 3.0f);
    emitter.colour[key][0] = 1;
    emitter.colour[key][3] = 1.0f - (float)key / 3.0f;
}
emitter.time_tick = 90;
frame.particle_emitters = &emitter;
frame.particle_emitter_count = 1;
su_render_frame_ex(renderer, &frame);
```

## Time and deterministic replay

Time is an explicit unsigned **60 Hz effect-local tick**, not an accumulated
frame delta. Convert an absolute effect time once with `floor(seconds * 60)`;
do not repeatedly add rounded frame deltas. Tick zero includes the burst.
At tick `t`, total births are `burst + floor(rate * t / 60)`. Continuous IDs
follow burst IDs and are born at `ceil((id - burst + 1) * 60 / rate)`.
Lifetime is half-open: alive at birth, dead at age `lifetime_ticks`.
`su_particle_count` returns the admitted live count, first admitted ID and live
births dropped for capacity. The newest live IDs win; no free-list or historical
overflow state affects subsequent frames. IDs never recycle within the effect.

Each invocation reconstructs initial position/velocity from a counter-based
32-bit integer hash of `(seed, particle ID, random counter)`, then replays
semi-implicit Euler integration from its birth in exactly 1/60-second steps:

`v = (v + acceleration / 60) / (1 + drag / 60); p += v / 60`.

Gravity, up to four attractors, and independently hashed XYZ turbulence samples
form acceleration. An attractor contributes
`(target - p) * strength / max(distance, radius, 0.001)`; negative strength repels.
It is constant-strength attraction outside its smoothing radius, not inverse
square gravity. Turbulence is bounded per-tick acceleration, not a sampled
external noise texture. Seed and counters use wrapping uint32 arithmetic;
the high 24 hash bits map exactly into `[-1,1)` before float simulation.

There is **no persistent simulation state**, warm-up requirement or frame cache.
Seeking backward, rendering an isolated timestamp, changing frame cadence and
repeating a frame produce identical particle identities and same-device output.
This is a cosmetic floating-point profile: CPU/GPU results are compared with
tolerances, not claimed bit-identical across drivers, devices or compilers.
Do not use it for authoritative simulation. Evaluation cost scales with live
count times age in ticks; long-lived effects are not constant-cost simulations.

## Shapes, curves and moving emitters

- `shape`: point (zero), box, uniform-volume sphere/ellipsoid, or uniform-volume
  cone along local +Z. `extent` is box half-extents, ellipsoid radii, or cone
  base X/Y radii and height. Zero extents reduce to a point.
- `velocity` plus independent signed `velocity_random` amplitudes initializes
  velocity. `drag` is nonnegative; gravity/turbulence/attractors apply per step.
- Four `size` half-extents and four linear RGBA `colour` keys are interpolated
  at normalized ages 0, 1/3, 2/3, 1. Zero size/colour really means invisible;
  there is no implicit white colour or nonzero size. RGB can be HDR.
- `origin` is the fixed Q32.32 anchor. CPU state positions are relative to it;
  rendering adds its camera-relative displacement after simulation.
- `transform_start` is the spawn affine transform; all-zero means identity.
  All-zero `transform_end` means start. If `transform_ticks` is nonzero,
  interpolate matrix components from start to end over those absolute ticks,
  clamping afterward. This explicitly specified motion is replayable, including
  translation and changing orientation/scale; matrix interpolation is not slerp.
- World mode (zero flags) transforms positions/velocities **at birth**, then
  integrates in anchor-relative world coordinates. `SU_PARTICLE_LOCAL` instead
  integrates in local coordinates and transforms at the requested tick, so
  surviving particles follow the emitter. Forces/attractor targets use the
  chosen simulation coordinates. Arbitrary animated transform history must be
  represented by separate effect segments; supplying only a mutable current
  transform cannot reconstruct past world-space births.

## Rendering and ordering

Compute writes camera-facing triangle vertices directly into a device-local
buffer; no per-frame CPU quad reconstruction, GPU simulation readback, or indirect
draw count is needed. `SU_PARTICLE_AXIS` instead makes cylindrical sprites
around world-direction `axis` (zero means +Y); `aspect` scales half-length
relative to half-width (zero means 1). Parallel camera/axis uses the same
perpendicular fallback as `su_fx_sprite_axis`.

`material` uses `SuFxBatch` texture, three layers, UV transforms, additive or
premultiplied blending, wrap, angular fade and soft opaque-depth intersections.
No depth writes occur. Texture handles must remain alive and READY; unavailable
textures skip that emitter draw and increment `fx_batches_skipped`. All-zero
texture slots sample the built-in white texture. Its vertex range fields are
ignored: capacity determines geometry. Layer animation still uses explicit
`frame.time_seconds`, independently of emitter ticks.

`atlas_cols/rows` select a per-particle sprite atlas (zero dimensions mean 1).
`atlas_ticks` advances the cell by particle age, wrapping; zero holds cell zero.
This operates on base vertex UVs and therefore every layer, before their usual
UV transformations. The existing material layer-2 atlas remains independent.
Use clamp and padded cells for filtered sprite atlases.

`material.flags = SU_FX_BATCH_SORT` enables stable GPU bitonic distance sorting
**within that emitter**, far to near, equal distances by ascending birth ID.
Sorting moves only compact 8-byte keys, not particle state or triangles.
Emitter draws stay in submission order after the existing textured-quad stream,
before haze/flame volumes. There is no global sort across emitters, meshes or
CPU FX batches. Use a single emitter for particles requiring mutual alpha
ordering; additive emitters generally do not need sorting.

## Bounds, statistics and diagnostics

Maximum 131072 slots across emitters, with each capacity rounded up to a power
of two for admission; maximum 16 emitters. Zero capacity is disabled. Invalid
count/pointers or combined capacity rejects the frame before GPU recording.
Tick and transform duration are at most 65535; lifetime is 1..4096 ticks;
rate is 0..60000 births/s; burst is at most 131072. These limits prevent
uint32 scheduling overflow and bound replay work. Shape/flag/wrap/blend IDs,
finite vectors/matrices/curves and scalar ranges are validated; vectors,
matrix entries, colour, size, turbulence and attractor strengths/radii are
bounded in magnitude by 10000. Drag is 0..60. Atlas dimensions are at most 1024.

GPU buffers are allocated lazily per in-flight frame, independent of ordinary
FX vertex budgets. Maximum storage is about 37 MiB per slot plus small parameter
buffers; diagnostics add a separate host staging buffer only when requested.
`SuRenderDeviceInfo.gpu_particles` reports optional compute support. The renderer
prefers a combined graphics/compute queue but retains the previous baseline on
graphics-only queues; submitting emitters there returns `SU_E_UNSUPPORTED_METHOD`.
Explicit
compute/compute, compute/vertex, and diagnostic transfer/host barriers protect
all accesses; normal frames never block on particle readback.

- `su_particle_validate(&emitter)`: validate simulation and scalar material inputs.
- `su_particle_count(&emitter, &first_id, &dropped)`: inspect deterministic admission.
- `su_particle_evaluate(&emitter, id, &state)`: CPU oracle, including zeroed dead output.
- `su_render_frame_ex(renderer, &frame)`: simulate and render the submitted emitters.
- `su_render_particles_readback(renderer, emitter_index, states, capacity, &count)`:
  offscreen-only diagnostic of the last frame, in actual draw order, with
  camera-relative positions; synchronizes and copies only when explicitly called.
- `su_render_stats(renderer, &stats)`: admitted `particles_alive`, capacity-only
  `particles_dropped`, `particle_emitters_drawn` and `particle_draws` each frame.

## Verification and cost

`test_particles` checks scheduling, boundaries, overflow limits, known integration,
seek/history invariance and moving-transform semantics without Vulkan.
`test_render_particles` compares compute state and pixels with CPU sprites for
all shapes, forces, curves, local/world transforms, both blends, stable sorting,
axis sprites, per-particle atlases and layered materials. It also checks known
soft-fade/occlusion pixels, multiple emitters, replay image identity, capacity,
empty frames and invalid input. Vulkan/synchronization validation must report
zero errors and warnings; software drivers use the same tests.

`test_render_particles --benchmark`: 1280x720 offscreen complete-frame CPU/GPU
timestamps, 20 warmup frames and 60 samples per case; 0/10k/100k particles,
unsorted and sorted. A box of half-extents (4,2,.5) at Z=-4, random velocity
amplitude .4, half-size .006, tick 60, lifetime 600, drag .2, Y gravity -.1 and
turbulence .1. No validation or explicit diagnostic/readback; the renderer's
ordinary offscreen output copy is included. Timings are observations, not a
universal bound. See the dated handoff and evidence directory for measurements.
