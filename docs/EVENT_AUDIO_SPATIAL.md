# Event audio: spatial scenes (A02)

## Contract

`sutekh/audio_spatial.h` is an additive C17 presentation API. `audio.h` and
`SuAudioVoiceDesc` are unchanged. All scene calls belong to one game thread;
scene storage is not shared with the render thread. Neither simulation state
nor committed-event ownership is part of this component. Fixtures are synthesized.

A scene has fixed capacities selected at creation: 1..65536 emitters and
0..4096 immutable curves. Allocation occurs only during scene creation;
subsequent operations allocate nothing. Creation reserves every curve's 64-point
storage. Emitter creation searches at most the emitter capacity; curve evaluation
searches at most 64 points. Allocation/capacity exhaustion returns
`SU_E_RESOURCE_LIMIT`. Zero curve capacity is supported; zero emitter capacity
is invalid. There are no implicit default capacities.

`SuAudioEmitter` is a scene-local `uint64_t`: upper 32 bits generation, lower
32 bits one-based slot. Zero is invalid. Destroy invalidates the handle;
reuse increments generation. A generation that wraps to zero permanently retires
the slot. Active queries return false for null scenes, invalid or stale handles;
other emitter operations return `SU_E_STALE_HANDLE` for a stale handle on a
valid scene. Handles must never be used with a different or destroyed scene:
they do not carry a scene identity and are not globally unique.

`SuAudioCurveId` is a scene-local stable `uint32_t`. Registration copies a table,
assigns consecutive IDs starting at 1, and never replaces or removes a table.
ID 0 means no curve. Authored external IDs require an adapter mapping; registration
order is not a cross-scene serialization identity. Invalid IDs return
`SU_E_INVALID_ARGUMENT`. Clear an emitter's curve by assigning 0.

## Units, interpolation and scale

Every point contains these channels, sharing a strictly increasing distance axis:

| Field | Unit and accepted range |
| --- | --- |
| `distance_m` | Metres, finite and nonnegative, up to `FLT_MAX` |
| `gain_db` | Amplitude decibels, -160..60 |
| `lowpass_hz` | Hertz, 0..384000; zero is bypass |
| `highpass_hz` | Hertz, 0..384000; zero is bypass metadata |
| `spread_rad` | Radians, 0..pi; metadata |
| `scale` | Dimensionless distance-radius multiplier, 0..64 |

One to 64 points are allowed. Every field must be finite. Duplicate or decreasing
distances are invalid. Each channel is linearly interpolated independently, with
endpoint clamping; a one-point curve is constant. Interpolation uses double
intermediates. Positive finite distances larger than `FLT_MAX` clamp to the
last point. Negative, NaN and infinite evaluation distances are invalid.
Zero-Hz bypass is a numeric endpoint, not a special interpolation mode:
interpolating from zero to 2000 Hz yields 1000 Hz at the midpoint. Use a positive
full-band endpoint if that is the desired transition.

Emitter `scale` is a dimensionless **distance-radius multiplier**, finite and in
(0, 1000000]. Set it explicitly to 1 for unscaled operation. The point `scale`
channel also scales distance, never amplitude. Resolution uses this explicit
non-recursive two-stage rule:

1. Compute `base_distance = physical_distance_m / emitter.scale`.
2. Interpolate the table's `scale` at `base_distance` once, giving `radius_scale`.
3. Interpolate gain/LPF/HPF/spread at `base_distance / radius_scale` once.
   Do not resample scale or iterate to a fixed point. A zero radius uses the
   last endpoint for positive distance, and the first endpoint at zero distance.

The scene snapshot's `curve.scale` reports the `radius_scale` actually applied,
not scale resampled at the adjusted distance. The standalone curve evaluator
returns raw table interpolation of every channel, with no secondary lookup.
This distinction prevents scale feedback. Both scales equal to 1 are neutral;
each applies to distance exactly once. Curved linear gain is
`authored_legacy_gain * pow(10, gain_db / 20)`, applied once.
The result saturates at `FLT_MAX` rather than becoming infinite. Legacy power
attenuation is disabled for a curve-resolved descriptor by setting
`min_distance = FLT_MAX`, `max_distance = 0`, `rolloff = 1`. Spatial panning and
Doppler remain active; no second distance attenuation is applied by the mixer.

With no curve, scale 1 preserves legacy gain/min/max/rolloff exactly, including
legacy zero-default semantics. A different emitter scale multiplies legacy
distance thresholds once (the default minimum is first interpreted as 1 metre).
An unlimited maximum remains zero. Overflow saturates at `FLT_MAX`; underflow
to zero for a positive threshold becomes `FLT_MIN`, preserving nonzero semantics.
No-curve samples return gain_db/LPF/HPF/spread zero and radius scale 1.

## Camera, transforms and numerical safety

The initial listener is at zero with identity rotation and zero velocity.
The camera setter copies world position, world-space orientation, and velocity.
Nonzero finite quaternions are normalized using double intermediates; zero or
nonfinite quaternions are rejected without changing the listener. World positions
use `SuWorldPos` Q32.32 metres, velocities use metres/second; each velocity
component must be finite with magnitude at most 1000000. Rejecting enormous finite
velocities avoids overflow in downstream float Doppler products.

World positions cannot encode NaN. Callers converting floating-point coordinates
must reject NaN, infinity and out-of-range inputs before calling `su_wpos`;
that existing conversion routine is not made safe by this API. Resolution checks
each source-listener coordinate difference fits signed Q32.32 before calling
`su_wpos_delta`, refusing an unrepresentable difference instead of wrapping a
distant source into a near one. All valid `int64_t` positions may be stored,
including endpoints; not all pairs can be resolved together. Nearby differences
at large anchors retain the existing integer-first precision.

`su_audio_scene_snapshot` copies emitter and listener state, physical distance,
the evaluated channels, and a camera-local direction in metres (not a unit vector).
The direction is inverse-camera-rotated once. Resolved legacy descriptors retain
**world-space** positions and velocities: the mixer performs its own listener
rotation, so rotating them here would apply orientation twice. Camera cuts and
emitter updates are immediate scene changes without implicit interpolation.
The caller supplies already-interpolated presentation transforms.

## Exact public signatures

```c
uint32_t su_audio_scene_create(const SuAudioSceneDesc *desc, SuAudioScene **out);
void su_audio_scene_destroy(SuAudioScene *scene);
uint32_t su_audio_scene_curve_add(SuAudioScene *scene,
    const SuAudioCurveDesc *desc, SuAudioCurveId *out);
uint32_t su_audio_scene_curve_evaluate(const SuAudioScene *scene,
    SuAudioCurveId curve, double distance_m, SuAudioCurveSample *out);
uint32_t su_audio_emitter_create(SuAudioScene *scene,
    const SuAudioEmitterSnapshot *desc, SuAudioEmitter *out);
uint32_t su_audio_emitter_update(SuAudioScene *scene, SuAudioEmitter emitter,
    SuWorldPos pos, SuVec3f vel);
uint32_t su_audio_emitter_destroy(SuAudioScene *scene, SuAudioEmitter emitter);
uint32_t su_audio_emitter_curve(SuAudioScene *scene,
    SuAudioEmitter emitter, SuAudioCurveId curve);
uint32_t su_audio_emitter_scale(SuAudioScene *scene,
    SuAudioEmitter emitter, float scale);
int su_audio_emitter_active(const SuAudioScene *scene, SuAudioEmitter emitter);
uint32_t su_audio_emitter_get(const SuAudioScene *scene,
    SuAudioEmitter emitter, SuAudioEmitterSnapshot *out);
uint32_t su_audio_scene_listener_set(SuAudioScene *scene,
    const SuAudioCameraListener *listener);
uint32_t su_audio_scene_listener_get(const SuAudioScene *scene,
    SuAudioCameraListener *out);
uint32_t su_audio_scene_listener_apply(const SuAudioScene *scene,
    SuAudioMixer *mixer);
uint32_t su_audio_scene_snapshot(const SuAudioScene *scene,
    SuAudioEmitter emitter, SuAudioSpatialSnapshot *out);
uint32_t su_audio_scene_resolve(const SuAudioScene *scene,
    SuAudioEmitter emitter, SuAudioVoiceDesc *in_out, float *lowpass_hz);
uint32_t su_audio_scene_resolve_v2(const SuAudioScene *scene,
    const SuAudioVoiceDescV2 *desc, SuAudioVoiceDesc *out,
    SuAudioCurveSample *effects);
```

`SuAudioVoiceDescV2` has exactly `uint32_t struct_size`,
`SuAudioVoiceDesc legacy`, `SuAudioEmitter emitter`, `SuAudioCurveId curve`,
in that order. `curve == 0` inherits the emitter curve; a nonzero ID overrides it
for this resolution only. There is no per-voice explicit no-curve override.
Scene/curve descriptors and V2 accept `struct_size >= sizeof(current_type)`;
unknown tail fields are ignored and short descriptors rejected. Initialize the
entire known structure. Emitter creation requires explicit valid scale.

The exact event-runtime resolver accepts an optional LPF output. V2 accepts an
optional full effects output. Except scene creation (which clears `*out` first),
failed calls do not mutate outputs or scene state. Resolving validates finite
nonnegative gain/distances/rolloff, pitch 0..64 and Doppler 0..1. It does not
validate clip/stream ownership, offsets or source PCM; `su_audio_play` retains
responsibility for validating the complete playback descriptor.

## Mixer integration and deliberately unsupported realization

Resolution alone enqueues nothing. Before rendering voices, send the scene's
listener with `su_audio_scene_listener_apply`, checking its bounded-queue status.
Then resolve a **fresh copy of the original authored descriptor**, call
`su_audio_play`, and send LPF with `su_audio_voice_set_lowpass`. Check every status.
Never repeatedly resolve a previously resolved descriptor: the input is authored
gain/distances, not a cache; repeated mutation compounds gain/scale.

For existing voices, resolve fresh authored data and send the returned position,
velocity and gain using `su_audio_voice_update`, and cutoff using the LPF setter.
Update the mixer listener after camera cuts. The caller owns the emitter-to-voice
binding and refresh schedule. Destroying an emitter invalidates future resolution
but does not implicitly stop mixer voices. Keep clips alive and stop voices
according to the caller's ownership policy.

The legacy update API cannot change a voice's attenuation thresholds. Changing
between curved and legacy mode, or changing legacy emitter scale while playing,
requires stopping/restarting that voice with a newly resolved descriptor.
Curve changes within curved mode need only refreshed gain and LPF. Multiple mixer
commands are not an atomic transaction: queue exhaustion may partially apply them.
No `scene_play` or `scene_update_voice` wrapper pretends otherwise; event runtime
integration must handle these statuses and ownership explicitly.

LPF is realized only when the caller sends the existing setter; zero or at/above
Nyquist bypasses it and existing smoothing applies. HPF and spread are **evaluated,
validated metadata only**. Neither HPF DSP nor stereo widening is implemented.
`su_audio_scene_resolve` exposes LPF only; V2 effects and scene snapshots expose
all channels for future DSP adapters. No unsupported channel is claimed audible.

## Integration checklist and validation

Only these component files are owned here:
`include/sutekh/audio_spatial.h`, `src/audio/su_audio_spatial.c`,
`tests/audio/test_audio_spatial.c`, and this document. No CMake edits are made.
The integrator must append `src/audio/su_audio_spatial.c` to `SU_AUDIO_SOURCES`
in `cmake/SutekhPresentation.cmake`, add a `test_audio_spatial` executable linked
to `su_audio`, apply project warnings, register an `audio_spatial` CTest, and
include the new header in any explicit distribution header manifest.

Targeted standalone validation, with no shared build directory:

```powershell
. tools/env-windows.ps1
New-Item -ItemType Directory -Force build-a02/spatial | Out-Null
$sources = @('src/audio/su_audio_spatial.c', 'src/audio/su_audio_mix.c',
    'src/audio/su_audio_stream.c', 'src/audio/su_audio_noogg.c',
    'src/render/su_render_util.c', 'src/platform/su_platform.c')
& $env:CC -std=c17 -Wall -Wextra -Werror -Wpedantic -D_CRT_SECURE_NO_WARNINGS `
    -Iinclude -Isrc tests/audio/test_audio_spatial.c @sources `
    -o build-a02/spatial/test_audio_spatial.exe
if ($LASTEXITCODE -ne 0) { throw 'spatial compile failed' }
& ./build-a02/spatial/test_audio_spatial.exe
if ($LASTEXITCODE -ne 0) { throw 'spatial tests failed' }
```

The test covers table endpoints, interpolation of all five channels, immutable
copies and stable IDs, capacity failures, malformed/nonfinite inputs, one/64-point
tables, stale/reused handles, default legacy descriptors, distance scale once,
gain conversion once, V2 overrides, normalized camera rotation, camera cuts,
moving emitters, millimetres at a large anchor, Q32.32 endpoints, and actual
offline stereo equal-power energy against a synthesized mono PCM signal.
It also compares no-curve output byte-for-byte with the legacy mixer.
The existing `test_audio.c` and `test_audio_lowpass.c` are separate regression
targets; this suite does not replace their Doppler/filter-response coverage.

Not qualified here: hardware output, concurrent scene access (unsupported),
generation exhaustion after billions of reuses, allocation fault injection,
cross-platform ABI/replay equality, performance budgets, event-runtime attachment
lifecycle, or future HPF/spread realization. This is correctness evidence, not a
performance qualification. No original assets or device access are required.

### Validation checkpoint: 2026-10-20

Component implementation and targeted native validation are complete; canonical
build/event-runtime integration remains the integrator's responsibility.
Modified paths are exactly the four component paths listed above. No commits,
pushes, pull requests, issues, CMake or shared-header edits were made by this
component author. The integration worktree is not the main checkout.

Toolchain: Windows x64, Clang 23.1.3 targeting `x86_64-pc-windows-msvc`, enabled
by dot-sourcing the repository environment script. Both default optimization and
`-O2` builds passed under `-std=c17 -Wall -Wextra -Werror -Wpedantic`:

| Executable | Checks | Failures |
| --- | ---: | ---: |
| `test_audio_spatial` | 4791 | 0 |
| `test_audio` | 236 | 0 |
| `test_audio_lowpass` | 865040 | 0 |

Measured x64 ABI sizes in bytes: emitter handle 8; curve ID 4; legacy voice
descriptor 136 (unchanged); V2 voice descriptor 160; emitter snapshot 48;
camera listener 56; spatial snapshot 152. These are platform observations, not
portable serialized sizes. C++17 header-only syntax checking also passed.

Exact additional validation commands, using the same dot-sourced environment:

```powershell
$sources = @('src/audio/su_audio_spatial.c', 'src/audio/su_audio_mix.c',
    'src/audio/su_audio_stream.c', 'src/audio/su_audio_noogg.c',
    'src/render/su_render_util.c', 'src/platform/su_platform.c')
foreach ($name in @('test_audio_spatial', 'test_audio', 'test_audio_lowpass')) {
    & $env:CC -std=c17 -Wall -Wextra -Werror -Wpedantic `
        -D_CRT_SECURE_NO_WARNINGS -Iinclude -Isrc "tests/audio/$name.c" `
        @sources -o "build-a02/spatial/$name.exe"
    if ($LASTEXITCODE -ne 0) { throw "$name compile failed" }
    & "./build-a02/spatial/$name.exe"
    if ($LASTEXITCODE -ne 0) { throw "$name failed" }
}
foreach ($name in @('test_audio_spatial', 'test_audio', 'test_audio_lowpass')) {
    & $env:CC -O2 -std=c17 -Wall -Wextra -Werror -Wpedantic `
        -D_CRT_SECURE_NO_WARNINGS -Iinclude -Isrc "tests/audio/$name.c" `
        @sources -o "build-a02/spatial/${name}_optimized.exe"
    if ($LASTEXITCODE -ne 0) { throw "$name optimized compile failed" }
    & "./build-a02/spatial/${name}_optimized.exe"
    if ($LASTEXITCODE -ne 0) { throw "$name optimized failed" }
}
'#include "sutekh/audio_spatial.h"' | & $env:CC -x c++ -std=c++17 `
    -Wall -Wextra -Werror -Wpedantic -Iinclude -fsyntax-only -
```

The standalone link includes `su_platform.c` because concurrent stream work
introduced thread/timing dependencies during validation. An intermediate link
without that source failed; the final commands above include it and passed.
Ogg decoding was not built in this direct-compiler lane. No hardware/device,
full-project CTest, sanitizer, or event-runtime integration test was run here.

SHA-256 source snapshot for the native runs (relative paths only):

```text
30A964CA480E26819EDAAB6BB99529359525A868F4CB5DC78239FE77444F72EB include/sutekh/audio_spatial.h
88C7B7801E5C6E8931732999330E5DFE39BE40238DC2091F79D8A4103742DFA8 src/audio/su_audio_spatial.c
CCE6DB80AB5097182B8CB9553F3687591D74EA90CBAF5A083F0BAD8549710AE2 tests/audio/test_audio_spatial.c
8EFA43B3767E6EB30DE70868F24602912DEB9EB668984E638B76C9043427269C src/audio/su_audio_mix.c
6F306075D1996B481F8904EA9366379B3CC4FFF9780AB36F9149201FAE3BD867 src/audio/su_audio_stream.c
C410D74C11C9C1FD0A87B836ACF6607702EFD0B6CF44E60A59C94E7EF22C9D5C src/audio/su_audio_noogg.c
E00704E06CB91956F019977678CA6B863769BFB3F1A8AF3A80296D3EAC10588F src/render/su_render_util.c
4B2ED59A5662D3A9C0C92797C6A28734A581E4AB17B97F93102EBC33B450F7A8 src/platform/su_platform.c
```

SHA-256 local validation artifacts (not distributed engine assets):

```text
2DD50ACB5AF92F2581048473C67B18D5ECF5CF315C583FC7BB876D6B4246471D build-a02/spatial/test_audio_spatial.exe
C0D3DE097263DC6616D1B4EEAE91231333EBEA8B19B8050D7238714138F4DAE6 build-a02/spatial/test_audio.exe
EAB93BAE4A602A3F0114F64CFA290C669D4B0ED2E940345B6843D58BC8D75013 build-a02/spatial/test_audio_lowpass.exe
411F3A9D4789EF98B7E1671C226844F68C729B313D6DB10BCDC6EB3341FE7A6F build-a02/spatial/test_audio_spatial_optimized.exe
F424F215AA69860B4B9F198869F6B4590C90B5191DEA4E4D0219E698430EA2C6 build-a02/spatial/test_audio_optimized.exe
B6D34B352C1AD4C134090E270F023F6DD72B8EA2FD40E76A70ED039B8ECD2CEF build-a02/spatial/test_audio_lowpass_optimized.exe
```

Next work: integrate the source/test into the canonical build, exercise A04
listener/resolve lifecycle and partial mixer-command failure handling, and
revalidate against the integrator's final dependency snapshot. HPF and spread
remain deliberately unrealized; no native scene-level play/update convenience
or autonomous voice attachment is provided.
