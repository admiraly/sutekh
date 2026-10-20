# Integrated event-audio runtime

The A01-A10 components are wired into one runtime here. Without a hook, the
event runtime still mixes through its private legacy mixer exactly as before
(`audio_event`, `audio_offline` and `audio_inspect` tests are unchanged). The
legacy `sutekh/audio.h` mixer and device API are untouched.

## Voices and spatial filtering

`su_audio_events_attach(events, &hook)` (hook version
`SU_AUDIO_EVENTS_HOOK_VERSION`, appended fields selected by `struct_size`)
replaces the private mixer of an idle runtime with an A05 voice pool. The pool
must be idle, at the runtime's sample rate (or 0) and hold at least
`max_logical_voices` logical voices, so an admitted leaf always gets a logical
voice. Each admitted leaf becomes one logical voice:

- Start and stop are sample exact. `su_audio_voice_add_at` starts a voice at an
  absolute pool frame inside a quantum; `su_audio_voice_stop_at` begins its
  release at an exact frame. A stop at or before a leaf's start cancels it
  (stops still precede starts at equal timestamps). In integrated mode a stop
  with `release_frames == 0` uses the voice policy release instead of a hard
  cut.
- Policies come from the hook: the first `SuAudioVoicePolicyRule` whose
  `node_id` and `bus_id` (0 = any) match the graph leaf, else
  `default_policy`. Looping leaves (`flags` bit `SU_AUDIO_LOOP`) get
  `SU_AUDIO_VOICE_LOOP`. Trigger sequence is admission order, then expansion
  order.
- Every quantum the runtime evaluates each live leaf with the same law as the
  legacy mixer: scene resolve (curve gain, scale, curve low-pass), distance
  attenuation, listener-relative equal-power pan and mono downmix for spatial
  voices; leaf pitch times the asset/runtime rate ratio. The result goes to the
  pool through `su_audio_voice_set_controls` and ranks the voice through
  `su_audio_voice_set_audibility(gain, 0)` (the curve already includes
  distance).
- The pool's controlled render path uses a double-precision fractional cursor
  with linear interpolation, a one-pole low-pass whose coefficient glides over
  10 ms, per-channel gains ramped across each quantum and a pitch glide across
  the quantum. Steal fade tails carry the full cursor, filter and gain state,
  and a woken voice fades in from a cleared filter, so virtualise, steal and
  resume do not click (unless a policy asks for `release_frames = 0`).
- A leaf ends when its logical voice dies; its asset pin is released only
  after no steal tail still reads the clip.

For unpitched, pitched, looping, delayed and statically spatialised voices
without a curve low-pass the integrated output is bit-identical to the legacy
path (`test_legacy_equivalence`).

## Parameters, buses, music and the master limiter

The hook's appended fields (read only when `struct_size` covers them,
`SU_AUDIO_EVENTS_HOOK_HAS`) attach optional components; all must outlive the
runtime and are then driven only by it:

- `params`: `su_audio_event_post` builds `su_audio_param_expand_state` for the
  `(emitter, new instance)` target at the trigger time and expands through
  `su_audio_graph_expand_ex`, so switch and blend nodes follow scoped values
  (`FIRST_ONLY` latches at the trigger time). Every quantum the runtime
  advances the parameter clock to the quantum start and evaluates `GAIN_DB`
  (added to the voice gain), `PITCH` (multiplies the pitch) and `LOWPASS_HZ`
  (the lower of curve and binding cut-off) for each voice's
  `(emitter, instance)` target at the quantum end; the gains then ramp across
  the quantum. `BUS_GAIN_DB` bindings, evaluated globally at the quantum end,
  drive their bus faders with a one-quantum ramp; those faders belong to the
  bindings. A retired instance's scoped values are cleared. Graph snapshots may
  now hold up to 4096 parameters and switches regardless of graph size.
- `buses`: routes are the tree's buses in execution order; each voice mixes
  into its leaf `bus_id`, unknown or zero IDs into the root. The runtime mixes
  every route, sets the bound faders and processes the tree once per quantum,
  so sidechain ducking detects the real post-fader bus signals. The tree's
  `max_block_frames` must be at least 128. The authored-loudness window has no
  automatic input yet (`su_audio_bus_set_loudness` is per block).
- `music` / `music_bus_id`: a music player on the runtime clock renders one
  quantum per quantum into its bus (0: the root; without buses it is added to
  the voices).
- `limiter_knee`: the master output passes the legacy soft limiter
  (`knee + (1 - knee) tanh((|x| - knee) / (1 - knee))`, never beyond full
  scale) when the knee is in [0.1, 1); 0 keeps the hard clamp.

## Tests

`ctest -R '^audio_runtime$'` (`tests/audio/test_audio_runtime.c`, synthetic
PCM only): hook validation; legacy equivalence under 37- versus 1000-frame
chunking; exact pitch-0.5 interpolation; curve low-pass on a far voice; a
one-physical-voice steal and resume with the largest sample step bounded by
the tone slope plus one envelope increment; exact stop timing, stop-before-start
cancellation and instance retirement. Parameter cases: switch selection before
and after a timed change, exact blend weights, `GAIN_DB` and `PITCH` bindings
on every voice, sidechain ducking of an ambience bus by shots on another bus,
a `BUS_GAIN_DB` binding reaching -12 dB on its fader, music metered on its own
bus, and the soft limiter bending an 18 dB overdrive to at most full scale.
