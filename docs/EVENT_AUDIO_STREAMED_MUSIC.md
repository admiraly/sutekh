# Streamed music segments

`SuAudioMusicSegmentDesc.stream_stems` moves long tracks off the host bus input
path without preparing resident float PCM. It names `stem_count` seekable WAVE
or Ogg Vorbis sources (the same formats as `SuAudioStream`; Ogg requires the
optional pinned decoder). Set exactly one of `stems` and `stream_stems`.
Resident segments and streamed segments can coexist in one music player.

## Playlist API

Zero-initialize descriptors, then provide one source per track:

```c
SuAudioMusicStreamSource source = {0};
SuAudioMusicSegmentDesc segment = {0};
SuAudioTempoPoint tempo = {0, 120000};
source.path = "music/track.ogg";
source.ring_frames = 8192;
segment.struct_size = sizeof segment;
segment.segment_id = 1;
segment.stream_stems = &source;
segment.stem_count = 1;
segment.tempo = &tempo;
segment.tempo_count = 1;
segment.beats_per_bar = 4;
segment.length_frames = 8640000;
```

Register through `SuAudioSystemDesc.segments` / `segment_count`, with the usual
music descriptor and `music_bus_id`. For a playlist, populate one segment and
source per song. Use `su_audio_system_music_transition` to start or change
tracks: it journals the accepted command and its boundary. Beat, bar, cue,
segment-end, fades and resident stingers use the existing transition API,
including a crossfade back into the same segment. The standalone
`su_audio_music_segment_add` and transition/render functions also support these
sources, but standalone calls are not system-journal commands.

Instead of `path`, set `bytes` and `byte_count` for borrowed encoded data.
Exactly one of `path` and `bytes` is required. Memory is not copied; encoded
bytes must remain immutable and alive for every live/replay system. Files
must also remain available and immutable. System descriptor tables and their
source descriptors must outlive the system, just like resident configuration.
No game-owned `SuAudioStream` needs pumping, seeking, sharing or closing.
The music player opens and closes its own streams, including on failed adds.

Sources must match the music sample rate; mono duplicates into stereo.
`length_frames` defines the segment timeline independently of source length;
a shorter stem produces silence at its end. Looping wraps that timeline.

## Memory, execution and determinism

Each stem owns two independent synchronous seekable decoder windows, one for
the current player and one for the outgoing player. This avoids decoder
thrashing during self-crossfades and permits independent live, replay and
checkpoint-verifier systems. Decoded PCM residency depends on window size,
not song duration. `ring_frames = 0` selects 8192 frames per window; explicit
sizes follow `SuAudioStream` admission (2048 through 2^24 frames). Stereo ring
storage at the default is 128 KiB per stem for both windows combined, plus
fixed decode chunks, loop-head storage and decoder state. Encoded memory
sources remain caller-owned; files are not loaded wholesale.

Decoding/seeking is synchronous on the rendering owner, including a pull
device's rendering thread. This is deterministic bounded-memory streaming,
**not an asynchronous real-time I/O guarantee**: slow storage or decoder work
can stall rendering. There are no worker-timing-dependent underrun samples.
Use fast local immutable files or encoded memory and measure device headroom.
No steady-state allocations are introduced by the music layer itself; decoder
implementations retain their existing behavior.

Creation reads encoded sources once to fingerprint their complete content
with SHA-256, using bounded scratch space for files. Checkpoints bind segment
IDs, resident generations or encoded-source hashes, tempo, cues, lengths and
loop policy. Replacing content or configuration rejects direct checkpoint
load with `SU_E_SCHEMA_MISMATCH`, even when segment counts are unchanged.
Paths, pointers and decoder caches are never serialized. Restore reads from
the saved logical sample positions, reconstructing disposable decode windows.
Changing only the ring size or source location does not change source identity.

`audio.render.offline`, journal replay, direct restore and checkpoint
verification therefore include streamed music under `audio_float_local_v1`:
bit-identical PCM within the same build, decoder and floating-point mode.
Cross-platform decoder bit equality is not claimed. The standalone
`SuAudioOfflineManifest` event-trace API does not configure music; use the
integrated `SuAudioSystem` offline output or control method for music renders.

## Coverage

`audio_system` synthesizes asymmetric stereo WAVE sources in memory and on
disk, compares their playback sample-for-sample with resident PCM through
small non-power-of-two decoder windows, and covers loops, beat/bar/cue/end
transitions, same-segment fades, stingers, caller chunking, independent journal
replay, offline-render PCM hashes, direct checkpoint continuation during a
fade and replay verification. Changed encoded bytes reject checkpoint load.
It also renders an entire synthetic three-minute stereo file through a
3072-frame window without adding resident assets, and checks failed-add
cleanup, invalid sources and mismatched sample rates. `audio_music_stream`
checks mono WAVE and the pinned Ogg fixture (when enabled): file/memory
equivalence with different window sizes, loops and self-crossfades, per-sample
versus block rendering, and checkpoint continuation with already-used decoder
caches. All audio is synthetic.
