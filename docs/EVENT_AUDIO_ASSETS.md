# Prepared audio assets and async streams (A01)

## Resident event admission

`sutekh/audio_asset.h` provides a fixed-slot, caller-owned registry of copied,
immutable float PCM. IDs are authored nonzero `uint32_t` values, not slot indices
or runtime pointers. Prepare is a control-thread operation; event admission
acquires an already prepared `SuAudioAssetRef`, never decodes on demand. Event
registry admission remains explicitly resident-only: prepare accepts a clip, not
a path, encoded bytes, decoder, or stream. Opt-in async streaming is a separate
legacy mixer extension, not implicit stream admission through the asset registry.

All registry functions run on one game/control thread, never in an audio
callback. Acquisition returns a borrowed `const SuAudioClip *` and increments its
reference count. Although the legacy descriptor has a mutable sample pointer,
registry samples are read-only by contract. Do not mutate/free registry clips.
The source descriptor and PCM may be discarded after successful prepare.

Keep acquisitions alive across queued commands, active/fading voices, and graph
or event-runtime ownership. Release only after acknowledged render retirement;
submitting stop is not retirement. Release an unused acquisition after failed
play. The registry must outlive all graphs and event runtimes that hold it.
Stop/join the device or otherwise synchronize rendering before final teardown.
Destroy refuses outstanding references and leaves the registry intact. Registry
calls are not arbitrary-thread-safe; no callback registry lookup is required.

### Status and memory contracts

Exact signatures are in `include/sutekh/audio_asset.h`. All mutations and acquire
return `uint32_t`; stats returns `void`. `SuAudioAssetStats` holds `uint64_t`
`resident_bytes` and `refused_admissions`.

- Create requires nonzero capacity and PCM byte budget and preallocates slots.
- Prepare validates nonempty mono/stereo PCM at 1000 through 384000 Hz and rejects
  NaN and either infinity before copying/publication. Caller buffers must remain
  valid and unchanged for the duration of prepare.
- Replacing a referenced ID returns `SU_E_RESOURCE_LIMIT`. Unreferenced
  replacement is transactional; rejected input or failed admission preserves the
  original generation, descriptor, and PCM.
- Acquire returns `SU_E_STALE_HANDLE` for zero, absent, or retired references.
  Its scan is bounded by capacity and performs no allocation, I/O, or locking.
- Unmatched release returns `SU_E_INVALID_ARGUMENT`; referenced unload/destroy
  return `SU_E_RESOURCE_LIMIT`. Invalid release/unload references are stale.
- Slot/PCM-budget exhaustion, allocation failure, reference-count exhaustion,
  and generation exhaustion return `SU_E_RESOURCE_LIMIT`.
- Generations are registry-global, monotonically increasing across IDs and
  unload/reload, not independent per-ID counters. They never wrap; preparation
  after generation `UINT32_MAX` is refused. References belong to exactly one
  registry lifetime and cannot cross into another/recreated registry.
- Failed prepare clears its output reference; failed acquire clears its pointer.
  Null registry arguments are invalid except destroy (successful no-op) and
  stats (zero snapshot). Null stats output is a no-op.

Resident bytes count owned PCM payload, excluding allocator and fixed metadata.
Post-prepare payload cannot exceed the configured budget. Transactional
replacement briefly holds old and new copies, with PCM peak at most twice that
budget. Reserve that preparation headroom outside the callback. Each failed
prepare/acquire against a valid registry counts once as a refused admission,
including invalid inputs; the counter saturates at `UINT64_MAX`. Failed release,
unload, and destroy do not count as admissions.

## Opt-in worker-owned streams

Include `sutekh/audio_stream_async.h`:

```c
uint32_t su_audio_stream_mode(SuAudioStream *stream, uint32_t mode);
uint32_t su_audio_stream_prime(SuAudioStream *stream, uint64_t frame, uint32_t timeout_ms);
void su_audio_stream_stats_ex(const SuAudioStream *stream, SuAudioStreamStatsEx *out);
```

Streams default to `SU_STREAM_OFFLINE_SYNC`, preserving existing synchronous
WAV/Ogg decoding, seeking, loops, and deterministic offline PCM behavior.
`SU_STREAM_ASYNC` starts one worker using the existing thread abstraction. Mode
selection and destruction are control-thread operations with rendering quiescent
and no voice holding the stream; attempts while claimed return
`SU_E_INVALID_ARGUMENT`. Switching back or closing joins the worker before any
control-thread decoder access/free. Device lifetime must not race configuration.
Async activation returns `SU_E_UNSUPPORTED_METHOD` if callback atomics are not
lock-free and `SU_E_RESOURCE_LIMIT` if the worker cannot be created.

During async operation only the worker reads/seeks the decoder. The existing
PCM ring is divided into two fixed windows of `floor(ring_frames / 2)` frames.
For odd ring sizes the final allocated frame is unused. The worker fills the
requested aligned window and prefetches the next. A separate fixed four-frame
loop-head cache handles interpolation across arbitrary loop boundaries. No PCM
allocation grows with duration, playback position, seeks, or underruns.

Each window has atomic EMPTY/READY/HELD ownership. The callback makes one bounded
CAS attempt per window and never spins, waits, calls a decoder, performs file
I/O, allocates, locks, or wakes a condition variable. The worker cannot overwrite
PCM while the callback holds it. After reading, the callback release-publishes
READY. Worker fills are published only after PCM and bounds are complete.

The existing `su_stream_sample_begin`/`su_stream_sample_missed` pair bounds an
output-sample demand latch. The first source fetch publishes the primary demand;
later interpolation fetches in that sample cannot replace it, regardless of
whether either fetch succeeded. This keeps the primary aligned window and its
adjacent prefetch resident instead of letting a successful partner continually
evict a missing primary. Loop partners use the separately prepared loop-head
cache without redirecting the primary demand. Finishing the sample releases the
latch. Unguarded internal `su_stream_frame` calls retain their original behavior:
every fetch publishes its demand, including after a completed guarded sample.

Demands use one atomic latest-position mailbox, not a FIFO: rapid seeks and
misses coalesce, stale work is bounded to the worker's current pass, and there
is no allocating backlog. A `decode_queue_full` FIFO metric is not claimed.
The idle worker polls with a one-millisecond platform sleep; this is not a
low-latency scheduling guarantee. Large jumps can temporarily produce silence.

A missing source fetch returns zero immediately and publishes its demand. The
mixer consumes the per-sample miss latch after interpolation fetches, silencing
the entire output frame when either partner missed. Mixer time still advances,
including source-rate/pitch conversion and loop wrap; recovered data is used at
the current logical position, not replayed from the missed interval. Worker
seek/read failures are zero-padded and increment `decode_errors`.

`su_audio_stream_prime` is an optional bounded control-thread wait before voice
admission. It requests a source frame and waits for its window and any configured
loop head, up to `timeout_ms`; timeout returns `SU_E_RESOURCE_LIMIT`. It cannot
run while a voice claims the stream. Priming is a readiness snapshot, not a
promise against later seek/loop misses. Loop requests are published by existing
mixer commands and handled without decoder calls on rendering.

`SuAudioStreamStatsEx` reports mode, fixed window count/size, stream resident
bytes, lifetime `underrun_reads`, lifetime `underrun_frames`, and worker decode
errors. Reads count missing source-frame fetches (including interpolation);
frames count output sample frames suppressed by the mixer's miss-latch hook.
Two interpolation misses in one output frame count as one frame, not two.
Direct internal source-fetch callers must consume `su_stream_sample_missed`
once per logical output sample to participate in output-frame accounting.
Resident bytes include stream storage, ring, scratch, and decoder accounting;
OS thread-stack reservations and platform runtime overhead are excluded.
Async output is scheduling-dependent by design; use offline sync for repeatable
no-underrun rendering.

## Local validation

Owned paths: `include/sutekh/audio_asset.h`,
`include/sutekh/audio_stream_async.h`, `src/audio/su_audio_asset.c`,
`src/audio/su_audio_stream.c`, `src/audio/su_audio_int.h`,
`tests/audio/test_audio_asset.c`, and this document. The separately authorized
`voice_sample` region in `src/audio/su_audio_mix.c` has two A01 hunks only:
`su_stream_sample_begin` before source fetches and `su_stream_sample_missed`
after both fetches, returning stereo silence on a miss. Preserve unrelated mixer
changes when staging these hunks. Build registration belongs to the integrator.
No other sources, commits, issue
mutations, or public assets are part of this implementation handoff.

Platform: Windows 11 Pro 10.0.26200, x64, Intel Core i5-12400F; Clang 23.1.3
(x86_64-pc-windows-msvc). Validation outputs are isolated in `build-a01`.
Tests use generated PCM/WAV plus the existing synthetic Ogg tone only.

Reproduction from the prepared worktree:

```powershell
. ./tools/env-windows.ps1
New-Item -ItemType Directory -Force build-a01 | Out-Null
$flags=@('-std=c17','-Wall','-Wextra','-pedantic','-Werror',
  '-D_CRT_SECURE_NO_WARNINGS','-Iinclude','-Isrc')
$sources=@('src/audio/su_audio_asset.c','src/audio/su_audio_mix.c',
  'src/audio/su_audio_stream.c','src/audio/su_audio_adpcm.c',
  'src/audio/su_audio_noogg.c','src/render/su_render_util.c',
  'src/platform/su_platform.c')
& $env:CC @flags @sources tests/audio/test_audio_asset.c -o build-a01/test_audio_asset.exe
& ./build-a01/test_audio_asset.exe
& $env:CC @flags @sources tests/audio/test_audio_stream.c -o build-a01/test_audio_stream.exe
& ./build-a01/test_audio_stream.exe build-a01
& $env:CC @flags @sources tests/audio/test_audio.c -o build-a01/test_audio.exe
& ./build-a01/test_audio.exe
```

When the pinned decoder is present, compile `su_audio_thirdparty.c` separately
with `-std=c17 -w -D_CRT_SECURE_NO_WARNINGS -Ithird_party/stb`, replace
`su_audio_noogg.c` with `su_audio_ogg.c` and that object, and rerun the asset and
stream executables with `tests/fixtures/audio/tone440_stereo.ogg` as the optional
argument (after `build-a01` for the legacy stream executable). Only the pinned
third-party implementation suppresses warnings; project code stays strict.

The asset test covers independent copied PCM, NaN/Inf rejection and replacement
preservation, reference pinning, unload/destroy refusal, stale generations, slot
reuse, capacity/budget refusals, stats, resident mixer output, all samples of a
32768-frame async WAV, file-backed async WAV, forward/backward requests,
underrun silence with advancing mixer position, recovery, seek commands,
loop-boundary interpolation, mode transitions, and bounded storage. The optional
Ogg argument compares every async source sample against whole-clip decode.

Instrumentation additionally compiles the audio/render and pinned decoder units
with the following symbol remaps, leaving the test and platform unit unremapped:

```powershell
$hooks=@('-Dmalloc=su_test_malloc','-Dcalloc=su_test_calloc',
  '-Drealloc=su_test_realloc','-Dfree=su_test_free',
  '-Dfread=su_test_fread','-Dfopen=su_test_fopen','-Dfclose=su_test_fclose',
  '-Dfseek=su_test_fseek','-D_fseeki64=su_test_fseeki64',
  '-Dsu_mutex_lock=su_test_mutex_lock','-Dsu_cond_wait=su_test_cond_wait',
  '-Dsu_platform_sleep_ms=su_test_sleep_ms')
```

Link with `tests/audio/test_audio_asset.c` compiled using
`-DSU_AUDIO_INSTRUMENTED`. Thread-local guards attribute side effects to actual
rendering/source-fetch calls, not the worker. A synchronous file-stream positive
control must detect real seeks/reads before the test resets its counter; earlier
async checks must already have zero side effects. This is targeted API-level
instrumentation, not an OS-wide syscall tracer or a device-thread audit.

The instrumented test can pause the real decoder inside its remapped file read
while rendering continues. It forces an uncached fractional loop partner with
the first partner resident, requires both output channels to be zero, and
checks one output underrun frame, one missing read, and the advanced loop
position. The pause/ack flags exist only in the instrumented test translation
unit; no production pause API, seam header, or extra runtime source is needed.

### Measured evidence, 2026-10-20

The final project sources compiled with C17 `-Wall -Wextra -pedantic -Werror`.
Direct executable results, all exit zero:

| Executable | Checks | Failures |
| --- | ---: | ---: |
| Asset/async, no Ogg dependency | 33153 | 0 |
| Asset/async, synthetic Ogg reference | 77286 | 0 |
| Instrumented asset/async with forced decoder pause | 77297 | 0 |
| Existing synchronous WAV/Ogg stream | 339 | 0 |
| Existing mixer with Ogg | 245 | 0 |
| Existing lowpass | 865040 | 0 |
| Existing ADPCM | 135 | 0 |

Five consecutive final instrumented runs each passed all 77297 checks. Each
reported zero guarded allocation/free, file, mutex/condition-wait, and sleep
calls, while its legacy synchronous positive control detected two file calls.
The standard underrun scenarios reported exactly two missing source reads and
two silenced output frames, with positions advancing through both cold demand
and seek. The deliberately paused interpolation case separately reported
exactly one missing source read and one silenced stereo output frame.

Measured bounded storage: mono WAV ring 8192 bytes (two windows of 1024 frames),
stream resident 12680 bytes; stereo Ogg async stream resident 223800 bytes.
Neither changed during the tested requests, seeks, loops, or underruns. These
exclude the OS worker stack. Asset metadata layout measured by Clang is 40
bytes per registry plus 48 bytes per capacity slot, excluding allocator overhead.
Asset ABI sizes are ID 4, generation 4, reference 8, and stats 16 bytes. The
registry lifetime scenario measured 16 owned PCM bytes and three refusals.
Final asset/Ogg repair executable file size is 446464 bytes; instrumented repair
executable is 450048 bytes. These are local file sizes, not distribution/performance budgets.
No callback latency distribution or throughput benchmark is claimed.

Source and artifact SHA-256 evidence:

| Path | SHA-256 |
| --- | --- |
| `include/sutekh/audio_asset.h` | `3DFB955BE739B24863984B0B340E904B07EE4671CB2089DE9A3B3670FDB1B10B` |
| `include/sutekh/audio_stream_async.h` | `C5C29563147B259881842D0EA53F00175354305E0379A1C20A8B5D4CB7768BF1` |
| `src/audio/su_audio_asset.c` | `C37608ADD0D0030CC15631F8425B8AC587E93631F29ED0ECF1C05EABBBB3F5F5` |
| `src/audio/su_audio_stream.c` | `7312FEABF8029C96D83F5DEC6BDDF598B9583DEDBA344A733F7AA95BE8D19082` |
| `src/audio/su_audio_int.h` | `273527F86C72B3766A1FBF628207E689B18E30C7F03FD8C146F92562DB84F5E4` |
| `tests/audio/test_audio_asset.c` | `4B262C663A4E5EE240DB3721C80DA92AAA7E7B6099A2D5EF941D96FF3E937FDE` |
| `build-a01/test_audio_asset_repair.exe` | `20B831C433B002E1F8783D3796E989A78D9F44DDEEC9E697AFAAE3F22BA23352` |
| `build-a01/test_audio_asset_repair_instrumented.exe` | `06620271FD21217527643FF99F5E545AECD23584409E6857ECA7068BA94EAA04` |

### Fractional-boundary repair evidence

The review follow-up changes only `src/audio/su_audio_stream.c`,
`tests/audio/test_audio_asset.c`, and this document. No additional mixer/header
or build registration changes are required. Targeted validation used dependency
snapshot `bb2850f1a8314d6ca6c98dfad0d78669267db49a` plus these local edits and
the existing integration-worktree edits; it is not validation of a future merge.

The initial regression reproduced 25 failing assertions before the repair.
Its synthetic mono source contains 8192 constant nonzero frames with a 2048-frame
ring. Eight repeated guarded pairs fetch 1023 then 1024, allowing ten milliseconds
of worker progress between both fetches and pairs; eight more pairs fetch 3071
then loop-head frame 7. Both primary and partner must remain exactly 0.25 and
each pair must report no miss. A cold-primary case subsequently requires eight
consecutive recovered pairs without explicit re-priming, and a final unguarded
far fetch must independently request and recover its own window. This covers
successful partners overwriting primary misses, adjacent-window prefetch,
loop-wrap starvation, and direct-fetch compatibility.

All final targeted direct builds used the strict flags and sources documented
above. The final repair executables were run as:

```powershell
& ./build-a01/test_audio_asset_repair_noogg.exe
& ./build-a01/test_audio_asset_repair.exe tests/fixtures/audio/tone440_stereo.ogg
& ./build-a01/test_audio_asset_repair_instrumented.exe tests/fixtures/audio/tone440_stereo.ogg
& ./build-a01/test_audio_stream_repair.exe build-a01 tests/fixtures/audio/tone440_stereo.ogg
& ./build-a01/test_audio_repair.exe tests/fixtures/audio/tone440_stereo.ogg
& ./build-a01/test_audio_lowpass_repair.exe
& ./build-a01/test_audio_adpcm_repair.exe
```

All returned zero; the instrumented repair executable passed five consecutive
runs. The fixed latch fits existing stream-structure padding on this target,
so measured stream residency and ring bounds are unchanged. The follow-up did
not run device output, sanitizers, alternate-platform checks, or the integrator's
full-suite/merged-tree CI gates. Existing staging and unrelated implementation
files were not changed by this repair.

Remaining acceptance gaps: device output validation, OS-wide callback profiling,
forced thread/allocator/decoder errors, reference/generation exhaustion,
extended concurrent stress, alternate-platform validation, and exact integration
snapshot validation by the parent. No full issue-closure claim is made from the
local subset alone. Async FIFO-full statistics are intentionally absent because
the implementation uses a coalescing latest-demand mailbox, not a queue.
