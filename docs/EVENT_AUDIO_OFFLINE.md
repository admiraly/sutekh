# Deterministic offline render, checkpoints and measured export (A09)

Header `sutekh/audio_offline.h`, source `src/audio/su_audio_offline.c`, library
`su_audio_offline` (links `su_audio_events`), test `audio_offline`.

## Inputs

A render is defined by an `SuAudioOfflineManifest` (event-runtime descriptor,
borrowed resident registry and prepared graph, the exact graph JSON, the asset
generations to pin, curves, emitters and the initial camera) plus a sorted
trace of accepted presentation commands (`SuAudioTraceCommand`): posts,
instance/emitter/scope stops, emitter moves and listener moves. Emitters are
1-based manifest indices; instance stops name the trace index of their post.
Unsorted traces, dangling targets and missing emitters are rejected at begin.

The session owns its own scene and event runtime. It pins every listed asset
generation at begin; a missing or replaced generation returns
`SU_E_STALE_REVISION`.

## Clock and determinism

The session renders whole 128-frame event quanta into its own cache. Before a
quantum starting at clock `c` renders, every trace command with
`sample_time < c + 128` is submitted in trace order, so posts and stops keep
their exact sample timestamps (the runtime splits the quantum) while emitter
and listener moves take effect at the start of the containing quantum. Caller
read sizes only copy from the cache: scheduling, RNG seeds and mixer block
boundaries never depend on them. No worker threads are used, so thread count is
not an input.

Profile `audio_float_local_v1`: identical inputs give bit-identical PCM within
one build, decoder and floating-point mode. Cross-platform equality is not
claimed. Commands that the runtime refuses (for example late or over-budget
posts) are counted in `trace_failed` rather than silently dropped.

## Checkpoints

`su_audio_checkpoint_encode` writes a 256-byte little-endian header plus 8 bytes
per pinned asset (ID, generation): magic, version, delivered position, next
trace command, committed-event cursor, source and graph SHA-256, the running
PCM SHA-256 state, the profile name and an FNV-1a 64 integrity check. No
pointers or native views are encoded.

Version 1 reconstructs state by deterministic replay: begin with a checkpoint
verifies the source digest (manifest + trace + graph JSON + generations), pins
the generations, renders silently to the saved position and then requires the
recomputed PCM digest state, trace position and commit cursor to match exactly,
else `SU_E_SCHEMA_MISMATCH`. Event cursors, no-repeat histories, voice
positions, release ramps, low-pass state and any later bus/parameter/cluster
state are therefore restored exactly, including features added after this
step, without serializing private runtime structures. Restore cost is linear in
the saved position (measured below). A direct state blob would need hooks in
the shared runtime and is left to integrator coordination.

## Measurement and export

The session meters every delivered frame: BS.1770-4 K-weighted gated
integrated loudness (400 ms blocks, 75 % overlap, -70 LUFS absolute and -10 LU
relative gates; sample rates must be multiples of 10 Hz), sample peak and a
4x oversampled (48-tap windowed-sinc) true peak. `su_audio_measure_stereo`
runs the same meter on any buffer, so mastering/normalization stays a separate
step after rendering. Metrics also report peak instances and logical voices,
admitted/refused/late counts, trace results, start/end presentation timestamps
in nanoseconds for A/V muxing, wall-clock render seconds and source/graph/PCM
SHA-256.

`su_audio_offline_report_json` emits those fields plus profile, timing mode
(`offline_nonrealtime`), decoder (`prepared_pcm_float32`), compiler string and
hardware counters marked unavailable with a reason. `su_audio_wav_encode_f32`
writes a 44-byte-header IEEE float stereo WAV into caller memory; the engine
performs no file writes.

## Tests and cost

`ctest -R '^audio_offline$'` checks repeat renders, seven variable chunking
patterns, checkpoint continuation at 1/127/128/30001/62000/96000 frames
(PCM and digests equal to uninterrupted), the commit watermark after resume,
corrupted/truncated/version/source-mismatched checkpoint rejection, missing
generation rejection, a fixed sample-exact event-order fixture (quantum-edge
starts, equal-time summing, same-sample stop before start), EBU Tech 3341
cases 1-2 (1 kHz stereo sine at -23/-33 dBFS read within 0.1 LU), true peak of
an fs/4 sine at 45 degrees within 0.2 dB, silence gating, presentation
timestamps and WAV encoding. All fixture audio is synthesized in the test.

Measured on Windows 11 x64, Core i5-12400F, Clang 23.1.3 Debug: 2.0 s of the
synthetic 43-command fixture rendered and metered in about 59 ms (34x real
time); a 288-byte checkpoint restored at 96,000 frames by replay in about
62 ms. These are single local observations, not budgets.
