# Scoped parameters, switches and interactive music (A07)

`include/sutekh/audio_parameter.h` is an additive component header. The
implementation lives in `src/audio/su_audio_parameter.c` and
`src/audio/su_audio_music.c` and links as the `su_audio_parameter` static
library on top of `su_audio_events`. No shared header, schema or event runtime
source changed; the legacy `su_audio` API is untouched.

Both objects are single-owner presentation state, serialized by their owner
like `SuAudioEvents`. All storage is allocated at creation.

## Parameters

`SuAudioParamsDesc` copies typed definitions (`SuAudioParamDef`: id, range,
default, smoothing frames, `CLAMP` or `REJECT` range policy), switch definitions
(`SuAudioSwitchDef`: id, default value, policy, fade frames) and control
bindings. Duplicate IDs, non-finite values and defaults outside their range are
refused at creation.

Values are written with `su_audio_param_set_at(params, scope, id, value,
sample_time)` and `su_audio_switch_set_at`. Scopes are `GLOBAL` (key zero),
`EMITTER` (emitter handle) or `INSTANCE` (instance handle). Lookups take a
`SuAudioParamTarget {emitter, instance}` and resolve instance, then emitter,
then global. A scoped value applies from its first command's timestamp; before
that the lookup falls through. Unknown IDs return `SU_E_INVALID_ARGUMENT` with
a zero output, never a guessed value.

Each slot holds up to `SU_AUDIO_PARAM_PENDING` (8) timestamped segments.
Parameters ramp linearly over `smoothing_frames` from the value at the command
time (the inherited global value for a first scoped write), so a mid-ramp
retarget never steps. At the command timestamp the value is the start value; at
`start + smoothing_frames` and later it is exactly the target. A command at the
same timestamp as the newest one replaces it. Commands earlier than the newest
segment, or than the clock set by `su_audio_params_advance`, are late and
return `SU_E_STALE_REVISION`. `advance` drops superseded segments (switch
segments are kept until their fade completes). Scoped storage is a fixed pool of
`max_scoped_values` slots with linear lookup; a full pool returns
`SU_E_RESOURCE_LIMIT`. `su_audio_param_scope_clear` frees a retired emitter or
instance.

Switch policies, reported by `su_audio_switch_mix(target, id, instance_start,
time)`:

- `FIRST_ONLY`: the value at `instance_start` is latched.
- `CONTINUE`: the current value, hard cut at the change timestamp.
- `FADE`: after a change later than `instance_start`, `old_value`/`new_value`
  with complementary linear gains over `fade_frames`. The old value is the one
  immediately before the newest change, so repeated changes fade from the most
  recent alternative.

`su_audio_param_expand_state` fills caller arrays and a
`SuAudioGraphExpandState` for `su_audio_graph_expand_ex`, so graph switch and
blend nodes select authored alternatives from scoped values (FIRST_ONLY
switches use the instance start).

### Live event branches

When parameters are attached to the integrated event runtime, posting an event
expands every switch case, its explicit default, and every blend point. Random
nodes still choose one deterministic child; sequences retain their authored
offsets and parallel nodes retain their authored order. The complete expansion,
including inaudible alternatives, must fit the graph's `max_expansion`, the
runtime's `max_leaves_per_instance`, and its available logical voices. A refusal
does not admit a partial instance. Preparation and the public
`su_audio_graph_expand[_ex]` snapshot APIs retain their existing selected-branch
bounds and behavior; the additional live bound is enforced at event admission.

Each quantum reevaluates the same bounded expansion without allocating. Switch
gains come from `su_audio_switch_mix`: CONTINUE selects the current case, FADE
weights the old and new cases, and unmatched values select the explicit default.
If both values resolve to that default, their weights add on the same branch.
FIRST_ONLY uses an instance-owned initial value, so parameter-history pruning
cannot change a latched choice. Blend gains use the current scoped parameter,
clamp at the endpoints, and weight only the adjacent points. Nested branch
weights multiply in root-to-leaf order before the authored sound gain.

Branch targets are sampled at the quantum start (or the instance start for a
pending instance); the voice pool retains its existing control interpolation and
virtualization policy. Inaudible alternatives are logical voices, not extra
physical voices. With ADVANCE virtualization their cursors advance with the
instance timeline, allowing a newly audible branch to resume in place rather
than retriggering its clip. No branch changes means unchanged PCM arithmetic
and authored ordering. Runtimes without attached parameters remain snapshot
consumers.

Direct checkpoints include the instance start, deterministic expansion sequence,
initial switch values, current leaf gains, and the existing voice-pool state and
cached partial quantum. The internal layout fingerprint distinguishes these
checkpoints from older builds; the public same-build checkpoint version is
unchanged. `audio_runtime` covers live switch policies, smoothed scoped blends,
nested alternatives, bounds, and byte-exact checkpoint and PCM continuation at
partial-quantum positions. `audio_graph_alloc` checks allocation-free live
expansion and atomic capacity refusal.

## Control bindings

`SuAudioParamBinding` maps a parameter through a clamped piecewise-linear curve
(up to 16 strictly increasing points) to `GAIN_DB` (added), `PITCH` (ratio,
multiplied, output in (0, 64]), `LOWPASS_HZ` (minimum wins, default Nyquist) or
`BUS_GAIN_DB` (first bound bus ID, gains on that bus added).
Sum groups: bindings with the same nonzero `sum_group` and control add their
outputs before joining the other bindings. `GAIN_DB` members output linear
amplitude, `PITCH` members ratio terms, and `LOWPASS_HZ` members hertz, all
>= 0. So `gain = 0.08 + 0.40 throttle + 0.35 strafe` is three bindings in one
group, and the sum is converted to dB once. At most
`SU_AUDIO_PARAM_SUM_GROUPS` (16) distinct groups are allowed; `BUS_GAIN_DB`
bindings cannot be grouped.
`su_audio_param_controls` evaluates them for a target at a sample time. In
the integrated runtime a voice-control binding with a nonzero `bus_id`
applies only to voices on that bus (see
[EVENT_AUDIO_INTEGRATION.md](EVENT_AUDIO_INTEGRATION.md));
`su_audio_param_controls` and `SuAudioParamVoice` have no voice bus and apply
every binding.

`SuAudioParamVoice` is the reference consumer: it renders one resident clip
through the controls with per-frame evaluation, a double-precision fractional
cursor (linear interpolation) and a one-pole low-pass. Pitch changes alter only
the cursor increment, so the cursor stays continuous through smoothed ramps.

## Interactive music

`SuAudioMusic` plays prepared resident or seekable streamed segments; see
[streamed music](EVENT_AUDIO_STREAMED_MUSIC.md) for long tracks and playlists.
A resident segment pins up to 16 stems
(mono or stereo, sample rate equal to the music clock; resampling is out of
scope), an integer tempo map (`SuAudioTempoPoint {beat, bpm_milli}`, first
point at beat 0, 1..1000 BPM), beats per bar, up to 256 cue frames, a length and
a loop flag. All stems read the same segment cursor, so they stay sample-locked.

Beat frames are `tempo_frame[k] + floor((beat - beat_k) * 60000 * rate /
bpm_milli)` with a 128-bit intermediate: each tempo point rounds once, and no
error accumulates over long durations. `su_audio_music_beat_frame` exposes it.

`su_audio_music_transition(music, SuMusicTransition, sample_time, &boundary)`
computes an absolute boundary from the current segment position:
`IMMEDIATE`, next `BEAT`, next `BAR` (bars count from beat 0), a `CUE`, or
`SEGMENT_END`. Boundaries past a looping segment's end land on the next loop
start. Late policy covers both requests before the music clock and cues already
passed: `REJECT` (`SU_E_STALE_REVISION`), `NEXT` (next loop occurrence of the
cue; otherwise starts at the clock) or `IMMEDIATE`. A new request replaces a
pending one, and boundaries never precede the end of an active crossfade, so a
third segment never overlaps one still fading.

At the boundary sample the old segment fades out over `fade_out_frames` and the
new one starts at frame 0 fading in over `fade_in_frames` (linear; equal
lengths are complementary). At the boundary sample itself the old gain is 1 and
the new gain 0. An optional stinger asset starts exactly at the boundary. Up to
four stingers are pinned; finished stingers are released by the next transition
request or destroy, never inside render. Rendering is per-sample and splits
exactly at boundaries regardless of the caller's block size.

## Tests and measured cost

`ctest -R '^audio_parameter$'` (`tests/audio/test_audio_parameter.c`) uses
synthetic PCM only and covers missing IDs and defaults, clamp and reject
policies, smoothing endpoints and mid-ramp retargets, late and out-of-order
commands, bounded pending segments and scoped slots, global/emitter/instance
isolation and fall-through, switch selection and blend weights through
`su_audio_graph_expand_ex`, FIRST_ONLY/CONTINUE/FADE with repeated changes,
binding validation, pitch-cursor continuity (cursor equals the integral of the
smoothed pitch), exact beat/bar/cue/segment-end boundaries across a tempo
change, drift-free beat frames up to beat 10^9, all late policies, a 2^40 start
clock, sample-exact crossfade and stinger PCM, and synchronized stems.

Measured on Windows 11 x64, Intel Core i5-12400F, Clang 23.1.3 Debug, `clock()`
CPU time over 10 s of audio (coarse; not a latency distribution or gate):
`SuAudioParamVoice` with three bindings about 3 ms per 48,000 frames; music
with three stems during a crossfade about 0.4 ms per 48,000 frames.
`SuAudioParams` storage is reported in `SuAudioParamStats.runtime_bytes`
(each scoped slot is under 300 bytes).

## Not covered

The integrated event runtime consumes these APIs (expansion snapshots, voice
and bus-gain bindings, music through a bus); see
[EVENT_AUDIO_INTEGRATION.md](EVENT_AUDIO_INTEGRATION.md). Switch and blend
choices are made when an instance is expanded: `CONTINUE`/`FADE` changes and
blend parameter moves do not re-select the branches of an instance that is
already playing. Music does not resample, time-stretch, or support entry
offsets other than frame 0 of the destination segment.
