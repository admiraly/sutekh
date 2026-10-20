# Migrating from the legacy mixer to the audio system

`sutekh/audio.h` (mixer, clips, streams, WASAPI device, `su_audio_event_new`)
is unchanged and stays supported: existing games keep working without edits.
This note is for a game that wants the integrated runtime
(`sutekh/audio_system.h`, see [EVENT_AUDIO_INTEGRATION.md](EVENT_AUDIO_INTEGRATION.md)).
The two can coexist in one process; they share nothing but the clip format.

## What moves where

| Legacy game code | Audio system |
| --- | --- |
| `su_audio_clip_from_wav/ogg` kept in game tables | `su_audio_asset_prepare(assets, id, clip, &ref)`: the registry copies PCM and gives `(id, generation)` references; unload is refused while referenced |
| Choosing a clip, gain and pitch per gameplay event in code | Strict JSON event graph (`su_audio_graph_prepare`): sound, random, sequence, parallel, switch and blend nodes with authored gain, pitch, loop flag and `bus_id` |
| `su_audio_mixer_create` + `su_audio_device_open(mixer)` | `su_audio_system_create(&desc, ...)` with `output = SU_AUDIO_OUTPUT_DEVICE` and `ring_frames` (render-ahead latency); call `su_audio_system_update` once per game frame |
| `su_audio_play(mixer, &voice_desc, &voice)` | `su_audio_system_post(system, &trigger, &instance, NULL)`: event ID, emitter, sample time and commit sequence; `su_audio_event_new` deduplication is replaced by the commit watermark |
| `su_audio_stop(mixer, voice, fade_s)` | `su_audio_system_stop(system, SU_AUDIO_SYSTEM_CMD_STOP_INSTANCE, instance, time, release_frames)`; also per emitter or scope |
| `su_audio_voice_update(pos, vel, gain)` per voice | `su_audio_system_emitter_update(emitter, pos, vel)` once per emitter; every voice on it follows |
| `su_audio_listener(pos, rot, vel)` | `su_audio_system_listener_set(&camera_listener)` |
| `su_audio_master_limiter(knee)` | `desc.limiter_knee` (same law) at the end of the bus chain |
| `su_audio_voice_set_pitch/gain` driven by game state | Parameters (`desc.params`) with `PITCH`/`GAIN_DB` bindings, set with `su_audio_system_param_set` (scoped global, per emitter or per instance, smoothed); a binding's `bus_id` limits it to one bus's voices |
| `SuAudioVoiceDesc.doppler` | `desc.doppler` for every spatial voice |
| Streams (`SuAudioVoiceDesc.stream`) | Stay legacy; mix them in through a host bus input (`desc.inputs`) |

## Replacing a game-side voice budget

A game that counts its own voices and refuses or kills plays when over budget
should drop that code and describe the policy instead:

- `desc.voices.max_physical` is the rendered-voice budget;
  `desc.voices.max_logical` (at least `events.max_logical_voices`) is how many
  sounds may exist. Over-budget sounds become virtual instead of being refused,
  keep their position (`SU_AUDIO_VIRTUAL_ADVANCE`) and come back with a fade-in
  when they rank again.
- Per-category caps become `SuAudioVoicePolicyRule` entries (match a graph
  node or bus) with `physical_limit` and `limit_scope` (`GLOBAL`, `EVENT`,
  `EMITTER`, `BUS`, `NODE`), `priority`, `HERO` flags plus `hero_reserve` for
  sounds that must never be dropped, and `release_frames` for the steal fade.
- Ranking uses priority, then audibility (gain after distance and curve), so
  "loudest/closest wins" needs no game code. `su_audio_voice_pool_stats` and
  `audio.inspect` show why a voice is virtual (stable reason codes).

## Replacing game-side ducking

A game that lowers music or ambience gain while other sounds play should route
those sounds to buses instead (`bus_id` on graph sound nodes, `desc.buses`) and
declare `SuAudioDuckRule`s: source bus, target bus, threshold, ratio, attack,
hold, release, maximum attenuation. The detector reads the real post-fader
signal of the source bus, so ducking follows what is actually audible. Fixed
volume settings become bus faders (`su_audio_system_bus_gain`) or
`BUS_GAIN_DB` parameter bindings.

## Replacing a game-side distance low-pass

A game that computes a low-pass cut-off from distance and calls
`su_audio_voice_set_lowpass` every frame should author a distance curve
instead (`desc.curves`, `SuAudioCurvePoint` with `gain_db` and `lowpass_hz` per
distance) and give emitters that curve. The runtime evaluates it per voice per
128-frame quantum and glides the filter coefficient, so there is no zipper
noise. Occlusion-style extra filtering maps to a `LOWPASS_HZ` parameter binding
(the lower cut-off wins).

## Long streamed tracks, Doppler and layered engines

- The music player plays prepared resident segments only. Long tracks can stay
  on the legacy `SuAudioStream` path: render them with a device-less legacy
  mixer from a host bus input (`desc.inputs`, `SuAudioBusInput`) into a music
  bus, so they still get bus faders (crossfades between playlists), duck rules
  and the master limiter. Host input is not journaled.
- Legacy per-voice `doppler` becomes `desc.doppler` (one amount for every
  spatial voice; the listener velocity comes from `listener_set`).
- Engine sounds layered from several loops, each answering the same game
  inputs differently, get one bus per layer and voice bindings filtered by
  `bus_id`, so a single "throttle" parameter can open one layer's filter and
  raise another's gain.

## Things to keep in mind

- Leave `desc.voices.sample_rate` at 0 when clips have other rates than the
  output: a nonzero pool rate makes the pool refuse every clip of another
  rate (the leaf then never plays), while the runtime resamples by pitch.

- Timing is in output sample frames: post at `su_audio_system_clock(system)`
  or later. With device output that clock leads the speaker by the ring fill.
- Everything the system references (assets, graph, descriptor tables) must
  outlive it. Calls are single-owner: do not call from the device thread.
- Use the journaled `su_audio_system_*` commands rather than component calls if
  you want `audio.render.offline` and checkpoint verification to reproduce the
  session.
- Save games: `su_audio_system_checkpoint_save/load` restore the audio state
  directly within the same build; keep your instance handles and commit
  sequence with the save.
