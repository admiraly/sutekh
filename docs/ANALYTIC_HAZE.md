# Analytic haze volumes

`SuRenderFrameDesc.haze` and `haze_count` submit up to `SU_HAZE_MAX_VOLUMES`
(1024) volumes in caller order. Zero count preserves existing rendering; density
zero is invisible. This is a presentation-only effect, not simulation state.

```c
SuHazeVolume volume = {0};
volume.pos = su_wpos(50000.0, 0.0, -20.0);
volume.transform = su_mat4f_identity();
volume.transform.m[0] = 8.0f;
volume.transform.m[5] = 3.0f;
volume.transform.m[10] = 5.0f;
volume.color[0] = 0.3f;
volume.color[1] = 0.4f;
volume.color[2] = 0.5f;
volume.density = 0.08f;
volume.falloff = 1.0f;
volume.blend = SU_HAZE_OVER;
frame.haze = &volume; frame.haze_count = 1;
```

Keep the array alive through `su_render_frame_ex`. `pos` is a Q32.32 anchor;
`transform` is an invertible column-major affine local-to-anchor matrix. Put the
large absolute translation in `pos`, not in the float matrix. The renderer
subtracts the camera position before converting the difference to floats and
inverts the affine transform in double precision on the CPU. Rotation,
nonuniform scale, shear, reflection and local translation are supported.

## Parameters

- `shape`: zero/`SU_HAZE_ELLIPSOID` uses the local unit sphere; a nonuniform
  transform produces an ellipsoid. `SU_HAZE_BOX` uses local `[-1,1]^3`.
- `density`: nonnegative extinction coefficient per world unit. Zero disables
  that volume. `color[3]` contains nonnegative scene-linear source radiance;
  values above one feed the existing HDR/bloom path.
- `falloff`: `[0,1]`, mixing uniform density with a smooth polynomial profile.
  Ellipsoid profile is `1-dot(p,p)`; box profile is
  `(1-p.x*p.x)*(1-p.y*p.y)*(1-p.z*p.z)` inside the volume. At one the density
  vanishes at its boundary. This is not an arbitrary power exponent.
- `blend`: zero/`SU_HAZE_ADDITIVE` adds in-scattering without attenuating the
  existing image; `SU_HAZE_OVER` also applies extinction to that image. For
  overlapping over volumes submit far-to-near. There is no automatic sorting.
- `sun`: nonnegative multiplier of the frame's sun radiance added to the base
  source: `source = color * (1 + sun * light.sun_color * light.sun_intensity)`.
  Zero preserves the unlit source. This assumes isotropic single scattering;
  it does not solve light extinction within the volume.
- `flags`: optional `SU_HAZE_SHADOW` modulates only the sun contribution using
  the existing sun map and cascade selection at the visible chord midpoint.
  Enable `frame.shadow` as usual. Shadow-enabled haze bounds participate in
  receiver fitting, even without visible opaque receivers. Haze does not cast
  shadows. Midpoint visibility is an approximation, not a volumetric shadow
  integral; other portions of the chord can have different visibility.
- `response`: zero/`SU_HAZE_SCATTER` selects the physical density integral above.
  `SU_HAZE_CHORD` selects the stylized chord-power response requested by the
  issue. With `L` the visible chord length in normalized local coordinates,
  `d` the local distance from the origin to the closest point on that visible
  chord, and `p = chord_power` (zero defaults to one), its alpha is
  `clamp(pow(L*density,p) + source_brightness*pow(clamp(1-2*d/source_size,0,1),p/3),0,1)`.
  Set density to one for the unscaled chord formula. `source_size` is the local
  source diameter; zero disables its lobe. Source brightness and size are
  nonnegative, finite, zero by default. This mode uses the same intersection,
  depth clipping, blend and lighting paths, but deliberately does not apply
  the polynomial density profile or Beer-Lambert response. `chord_power` is a
  nonnegative finite value (positive values select the exponent explicitly).

Unknown shape/blend/response/flag values, nonfinite or negative radiometric parameters,
falloff outside `[0,1]`, nonfinite/non-affine/singular transforms, null arrays
with nonzero counts, and counts above the cap return `SU_E_INVALID_ARGUMENT`
before submission. A zero-initialized frame requests no volumes. Each enabled
volume needs an explicitly initialized affine transform, normally identity.

## Integration and composition

A screen-space triangle per volume is conservatively scissored to its projected
local bounding box. Boxes crossing the camera plane use the full viewport.
Each fragment reconstructs the camera ray from the inverse reversed-Z matrix,
intersects the local analytic volume and clips its chord to opaque/masked scene
depth. The camera can be inside the volume; integration starts at the eye, not
at the near plane. Neither depth testing of a proxy surface nor depth writes
are involved. Background depth zero leaves the entire analytic chord visible.

The integrated coefficient is `tau = density * integral(profile_mix ds)` in
world units, followed by `alpha = 1-exp(-tau)`. The ellipsoid polynomial is
integrated explicitly. The box's degree-six polynomial uses the fixed
four-node Gauss formula, exact for polynomials through degree seven: four
profile evaluations are an exact polynomial integral, not distance-dependent
ray marching. The CPU reference uses the same exact four-node formula for both
shapes. Floating-point roundoff and HDR attachment precision still apply.

Additive output is `dst + source*alpha`; over output is
`dst*(1-alpha) + source*alpha`. Volumes run after the existing textured effects,
before bloom and tonemapping. Transparent geometry does not contribute depth,
so transparent surfaces inside a chord are not separately integrated. Over
composition follows submission order; this is not an order-independent solver
for interpenetrating media. There are no textures, arbitrary density fields,
or multiple scattering. The optional source-size lobe is a stylized
screen-ray response, not a physical local light transport solution.

`su_haze_optical_depth(&volume, eye, direction, max_distance)` is a GPU-free
reference returning `tau`. Direction is normalized internally; distance must
be finite and positive. Invalid inputs and misses return zero. It is useful for
tests, not an authoritative simulation primitive. It always returns the density
integral, not the stylized chord-mode alpha. `SuRenderStats.haze_volumes_drawn`
counts submitted, nonzero-density, screen-intersecting draws, not covered pixels.

## Validation and cost

`haze_math` compares the analytic integral with independent dense integration
and known chords. `render_haze` compares complete readbacks to the CPU reference,
including falloff, both shapes/blends, affine transforms, clipping, eye-inside,
camera rotation, large-world anchoring, distant volumes, chord-power/source
responses and 32 overlapping
volumes. Synthetic opaque geometry exercises single-map and cascaded sun
occlusion. Both new tests run in the normal CTest suite, including software
Vulkan. `test_render_haze --bench` measures the offscreen frame at 1280x720 with
0/1/8/32 overlapping ellipsoids, 20 warmup and 120 measured frames per count.
It reports timestamp and CPU min/median/p95/max as applicable. GPU time covers
the whole frame, including output/readback copy; CPU time excludes readback wait.
Benchmark validation is off. See the dated implementation handoff for actual
hardware measurements and command results, not guaranteed timing budgets.
