# Bus tree, sidechain ducking and dynamic-range window (A06)

`sutekh/audio_bus.h` adds a fixed mixing tree keyed by the authored graph leaf
`bus_id`. It is a separate single-owner presentation component; every call,
including processing, is serialized by its owner. Creation validates the tree,
assigns the execution order and allocates every buffer in one block, so
`su_audio_bus_mix`, `su_audio_bus_process` and the setters never allocate. The
legacy `su_audio` mixer is unchanged. Feeding A04/A05 voices into buses is the
owner's (or a later integrator's) job: render each source into its bus with
`su_audio_bus_mix`, then call `su_audio_bus_process` once per block.

## API

| Call | Purpose |
| --- | --- |
| `su_audio_bus_tree_create/destroy` | Buses (`id`, `parent`, `gain_db`, `max_duck_db`, `hdr`), duck rules and window policy. Exactly one root (`parent = SU_AUDIO_BUS_ROOT`). Failures fill `SuAudioBusDiagnostic` (descriptor, duplicate, unknown bus, root count, cycle). |
| `su_audio_bus_order` | Execution order: children and undelayed sidechain sources precede consumers; ready buses go in ascending ID. |
| `su_audio_bus_mix` | Adds stereo input to a bus for the next block. |
| `su_audio_bus_set_loudness` | Authored loudness (dB) of the loudest current source on a bus, for the window; cleared every block. |
| `su_audio_bus_set_gain` | Fader target, ramped linearly in linear gain over `ramp_frames`, across block boundaries. |
| `su_audio_bus_process` | Runs one block (<= `max_block_frames`) into interleaved stereo output; clears inputs. |
| `su_audio_bus_gain_at` | Total linear gain (fader x duck x window) of the last processed sample. |
| `su_audio_bus_meter_read` | Peak, RMS, 4x inter-sample peak estimate, fader/duck/window dB; optional reset. |
| `su_audio_bus_tree_bytes` | Prepared allocation size. |

## Processing

Each block, in execution order, a bus multiplies its summed input by
`fader x 10^(-duck/20) x window` per sample, meters the result, records the
sidechain detector level for rules whose source it is, and adds it to its
parent (the root writes the output). Summation order is fixed, so output is
deterministic under the `audio_float_local_v1` profile.

## Ducking

A rule's detector is the stereo-linked sample peak (`max(|L|, |R|)`) of the
source bus after its own gain. The target reduction is
`clamp((level_dB - threshold_db) * (1 - 1/ratio), 0, max_attenuation_db)`.
The envelope moves in dB, linearly: rising by `max_attenuation_db` takes
`attack_ms`, falling by it takes `release_ms` (0 = instant), and a fall starts
only after `hold_ms` without a rise. Because the detector is a raw sample peak,
use a hold of at least half the lowest expected period for tonal sidechains.
Reductions of all rules on a target are summed in dB and clamped by the
target's `max_duck_db` (0 = no clamp).

Parent edges and undelayed sidechain edges must form an acyclic graph; a
sidechain that would feed back is rejected (`SU_E_SCHEMA_MISMATCH`, cycle
diagnostic) unless it carries `SU_AUDIO_DUCK_DELAY_QUANTUM`. A delayed rule
reads its source exactly `max_block_frames` samples late through a ring, for
any processed block sizes.

## Dynamic-range window

Optional and off by default. Participating buses report authored source
loudness, not measured PCM, so the window never acts as a master hard clip.
The top `T` is the loudest current participating bus, held and falling by at
most `window_db` per `release_ms`. Every participating bus is attenuated by
`max(0, T - ceiling_db)`; a bus with a source quieter than `T - window_db` is
culled. Window gains change once per block and ramp linearly across it.
Fields stored with `enabled = 0` have no effect; no compatibility profile
enables the window implicitly.

## Metering

Peak and RMS cover both channels since the last reset. `true_peak` is a 4x
Catmull-Rom inter-sample estimate, not an ITU-R BS.1770 oversampling filter; for
an fs/4 sine sampled at +/-0.7071 it reports 0.8839 against an analytic 1.0.

## Tests

`ctest --test-dir build-a05 -R '^audio_bus$' --output-on-failure` covers
diagnostics (duplicate, unknown parent/rule bus, roots, parent cycle, sidechain
feedback, bad ratio, enabled window without a width) and accepted delayed
feedback; execution order; disable-all equivalence with the legacy mixer
(per-voice legacy renders routed through unity buses equal a single legacy
mixer of the same voices, measured difference 0, and the tree's own sum is
exact); gain staging, metering and a click-free 300-frame ramp across blocks;
exact attack/hold/release timing from a synthetic 600-frame sidechain burst;
stereo-linked detection; silence passthrough; additive duck clamp; cascade
order; one-block delayed feedback; window culling, ramping and release; and the
true-peak estimate.

## Measured cost

Windows 11 x64, Intel Core i5-12400F, Clang 23.1.3 Debug: 16 buses on three
levels, 8 duck rules, window enabled, 48,000 frames in 128-frame blocks: 38 ms
CPU (`clock()`), prepared allocation 23,840 bytes. One observation in an
unoptimized build, not a latency distribution or a performance gate; per-sample
`powf`/`log10f` in active duck rules dominates.
