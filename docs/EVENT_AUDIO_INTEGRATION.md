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

## Tests

`ctest -R '^audio_runtime$'` (`tests/audio/test_audio_runtime.c`, synthetic
PCM only): hook validation; legacy equivalence under 37- versus 1000-frame
chunking; exact pitch-0.5 interpolation; curve low-pass on a far voice; a
one-physical-voice steal and resume with the largest sample step bounded by
the tone slope plus one envelope increment; exact stop timing, stop-before-start
cancellation and instance retirement.
