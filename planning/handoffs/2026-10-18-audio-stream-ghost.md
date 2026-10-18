# Handoff: streamed audio, voice offsets and loop points, translucent instances

Branch `feat/audio-stream-render` from `origin/main` c7584c8. Scope: the third extension of
`planning/amendments/2026-10-18-downstream-host-work.md` (items 14 to 16). Machine as in
`2026-10-18-presentation-assets.md` (clang 23.1.3 with the MSVC libraries, NVIDIA Quadro P600). Tools reused from
the previous stream outside the repository: glslang 16.6.0 (not needed, no shader changed) and the Khronos
validation layer built from source (`C:\Users\dev\sutekh-scratch\vvl-build\layers`, `VK_ADD_LAYER_PATH`,
`SUTEKH_RENDER_VALIDATION=1`).

## 1. Streamed audio (`include/sutekh/audio.h`, `src/audio/su_audio_stream.c`)

- `su_audio_stream_open_file(path, ring_frames, &s, msg)` / `su_audio_stream_open_memory(bytes, len, ...)`: format
  detected from the first bytes (RIFF WAVE or Ogg). WAV reads the PCM from the file on demand; Ogg uses the same
  pinned stb_vorbis pull API (`STB_VORBIS_NO_STDIO` dropped from the implementation unit so it can read a FILE).
  `su_audio_stream_info` reports frames, window, decoder and total resident bytes, frames decoded and decoder seeks;
  `su_audio_stream_close` refuses while a voice uses the stream.
- Bounded memory: a window of `ring_frames` decoded frames (default 16384, minimum 2048) plus decoder state.
  Decoding is incremental, in 1024-frame chunks, **on the thread that calls `su_audio_mixer_render`**, so offline
  output is a pure function of the commands and block sizes (no worker timing). Consequence: the device thread does
  file reads and Vorbis decoding; keep short sounds as clips. A worker-thread prefetch is the obvious next step if a
  device build shows underruns; it would need the window to be filled ahead of the read position.
- One stream serves one voice at a time (`su_audio_play` refuses an in-use stream); a finished, stolen or destroyed
  voice (and a mixer destroyed with the play still queued) releases it.
- Ogg conversion is the same short-to-float as `su_audio_clip_from_ogg` and WAV uses the shared
  `su_wav_sample`, so a streamed render equals the decoded one **bit for bit** (no tolerance needed).

## 2. Start offset, loop points, seek

`SuAudioVoiceDesc` gained (appended; zero keeps the old behaviour) `stream`, `start_frame`, `loop_start`,
`loop_end`, and `start_seconds`, `loop_start_seconds`, `loop_end_seconds` (seconds are used while the frames field
is 0; both set is an error; conversion `llround(seconds * clip rate)`). Looping continues at `loop_start` on
reaching `loop_end` (0 = the clip's end); a non-looping voice ignores the loop points. Invalid combinations return
`SU_E_INVALID_ARGUMENT`. New: `su_audio_seek`, `su_audio_seek_seconds` (exact: the next output sample is that
frame), `su_audio_voice_position` (source frames after the last render). For streams, the first frames of the loop
region are cached so the interpolation partner at the wrap needs no seek; the wrap itself repositions the decoder
once per cycle when the loop start lies outside the window.

## 3. Translucent instances (`SuRenderObject.tint[3]`)

The tint already multiplied base alpha, but only blend-mode materials honoured it. Now `tint[3] < 1` promotes the
instance's draws to the blend pipeline (opaque -> blend, opaque two-sided -> blend two-sided, same for masked) and
the material push constant selects alpha mode 2 for it, so a masked material's cutoff does not discard the ghost.
Blended items stay depth-tested, no depth write, drawn after the opaque pass, sorted far to near by node distance;
ties now keep submission order (deterministic). `tint[3] <= 0` or NaN draws nothing and counts in `objects_culled`.
`tint[3] == 1` takes the old path unchanged. No shader and no SPIR-V changed. Not done: order-independent
transparency, and sorting is per node (not per triangle), so intersecting translucent meshes can show order
artifacts.

## Tests

| Command | Result |
|---|---|
| `build-win\test_audio.exe tests\fixtures\audio\tone440_stereo.ogg` | 220 checks, 0 failures (unchanged count and values) |
| `build-win\test_audio_stream.exe build-win\audio_stream_work tests\fixtures\audio\tone440_stereo.ogg` | 339 checks, 0 failures. Streamed (file and memory) equals the whole decode byte for byte for WAV 20 s stereo and for Ogg in 6 configurations each (plain, 333-frame blocks, pitch 1.37, spatial with Doppler, loop with start offset and fractional pitch, whole-clip loop); a 2048-frame window looping a 6000-frame region at pitch 1.9 (46 seeks in 4 s); mono 8 kHz source; start offset and loop region in frames and seconds against exact expected samples; position readback; seeks (5 targets per source incl. Ogg, seconds, bounds, stale handle); refusals; ownership (in use, close refused, freed by stop, steal, destroy) |
| same, memory section | 300 s stereo 44.1 kHz WAV (53 MB file) through the 48 kHz mixer: stream resident 143,616 bytes (window 131,072, decoder 4,160) = 0.14% of the 105,840,000-byte whole decode; 0 seeks; 13,230,000 frames decoded; FNV-1a of the 14.4M-frame output equal: `11494b1d62fe80f1`. Process peak working set (observation): start 20.5 to 21.0 MiB, after streaming 5 minutes 20.5 to 21.0 MiB (no growth), after decoding it whole 161 MiB. Ogg fixture: decoder state 198,896 bytes (stb_vorbis setup + temp), resident 338,352 bytes |
| `build-win\test_render_ghost.exe build-win\render_assets_work` (validation layer on) | 79 checks, 0 failures, validation 0 errors 0 warnings. Exact pixels: opaque tint unchanged (0,255,0); ghost 0.5 = 188; tint colour with alpha 0.25; ghost over an opaque backdrop mixes (188,188,0); opaque occluder hides a ghost behind it; two and three ghosts back to front independent of submission order (188,137,0) and (188,137,99); alpha 0, NaN, negative not drawn; masked material ghost under its cutoff; blend material x tint alpha = 0.25; natural blend object sorted with a ghost; two frames pixel-identical. Run against the code without the scene change: 14 failures (the test detects the feature) |
| `build-win\test_render_assets.exe`, `test_render_offscreen.exe` (validation on) | 114 checks / 23 checks, 0 failures, 0 validation errors |
| `ctest --test-dir build-win -j 4` | 104 tests, 100% passed; 2 skipped (`path_fifo`, `containment`, as before) |

Unrun: MSVC `cl /W4 /WX` and the Linux GCC `-pedantic` build (CI covers them); `audio_device` was not rerun (device
thread path unchanged apart from the stream decode now possible inside it).

## Answer to the downstream "collision immunity after respawn" request (no engine feature)

This is game logic. Add a counter field (for example `immune_ticks`, u32) to the entity; the respawn rule sets it to
the window length; a system decrements it by one per tick down to zero; the collision-response system (which runs
after the proximity/query operator, see the post-query phase handoff) reads it and skips the response and damage
when it is non-zero. It is deterministic, snapshot/rollback-safe because it is ordinary entity state, and needs no
operator, IR or host change.

## Next work

Worker-thread prefetch for device builds if the render thread shows decode hiccups; per-triangle or
order-independent transparency if intersecting ghosts matter; streamed-clip loop crossfade.
