# Structured audio inspection and vertical-slice acceptance (A10)

Header `sutekh/audio_inspect.h`, source `src/audio/su_audio_inspect.c`, library
`su_audio_inspect` (links `su_audio_offline`), test `audio_inspect`.

## Snapshots

`su_audio_inspect(events, scene, assets, snapshot)` fills caller-owned, bounded
arrays: instances (handle, emitter, admission order, scope, state, voice range),
voices (instance, start sample, graph node, bus ID, asset ID/generation,
pending/playing/finished, resolved gain, pitch, low-pass), emitters referenced
by live instances (camera-relative distance and direction, curve gain, scale)
and per-bus pending/playing counts, plus budgets, runtime stats, resident asset
stats and the committed-event cursor. Short arrays set `truncated` bits and
never overflow. Output order is slot order then graph expansion order, so equal
state gives byte-identical JSON. It reads the runtime through a private
read-only view (`src/audio/su_audio_event_view.h`, one appended accessor in
`su_audio_event.c`); no runtime behaviour changes. Call it from the owner that
serializes rendering, not from a device callback.

Bus entries describe authored `bus_id` routing tags. No bus mixing, meters per
bus, voice virtualisation, parameters or clustering exist in this build; the
acceptance manifest records those as gaps until their components integrate.

## Telemetry

`su_audio_trace_begin(capacity)` creates a fixed ring. `su_audio_trace_record`
assigns a gap-free sequence and a stable reason code (`ok`, `pool_full`,
`duplicate`, `late`, `stale_handle`, `graph`, `invalid`). When full it refuses
the new entry and counts it; the next accepted entry carries
`dropped_before`, so backpressure is visible and queued entries are never
overwritten. `su_audio_trace_drain` moves entries out in record order.
`su_audio_inspect_post` posts and records; `su_audio_inspect_attach_offline`
records every submitted offline trace command with its sample timestamp and
the render clock it was submitted at (waveform event timestamps).

## Export

`su_audio_inspect_json`, `su_audio_trace_json` and `su_audio_trace_csv` write
into caller memory and report the required size when it is too small. They
contain numeric IDs and handles only, never pointers or file paths; the
offline report's compiler string is truncated before vendor/repository suffixes.
`su_audio_acceptance_manifest_json` links an offline session's source and graph
digests to a WAV artifact, verifies the WAV payload hashes to the session's PCM
digest (`SU_E_SCHEMA_MISMATCH` otherwise) and lists component capabilities and
compatibility gaps. The engine writes no files.

The control-plane methods `audio.inspect`, `audio.trace.begin/end` and
`audio.render.offline` are not registered; they remain proposals and are listed
as a gap in the manifest.

## Acceptance fixture and command

`test_audio_inspect [export_dir]` builds a synthetic skirmish (noise shots,
low impacts and a hum on three bus tags; three emitters with an attenuation
curve; emitter and camera moves; a scope stop), renders it offline with
telemetry, inspects it mid-render and exports `inspect.json`, `trace.json`,
`trace.csv`, `report.json`, `slice.wav` and `acceptance.json` into the given
directory. CTest passes a directory under the build tree. All audio is
synthesized by the test; no downstream profile or bank data is bundled.

Checks: deterministic trace and CSV across repeated runs, ring backpressure
accounting, diagnostics under instance-pool and stop-queue saturation (pool,
duplicate, unknown event and late reasons), truncated snapshots, matching
source/artifact manifests and tamper rejection, no pointer/path text in
serialized output, and one complete admitted event -> camera-spatialised ->
bus-tagged -> offline export path.

Measured on Windows 11 x64, Core i5-12400F, Clang 23.1.3 Debug: about 0.5 us
per snapshot with 4 instances, 5 voices, 3 emitters and 3 buses; 2.2 KB JSON.
One local observation, not a budget.
