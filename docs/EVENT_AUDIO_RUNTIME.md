# Timestamped event runtime (A04)

## Owner and lifetime

`SuAudioEvents` is a serialized presentation-owner object, not a simulation
system or a concurrent device adapter. Create it outside rendering with a
prepared graph, resident asset registry and optional spatial scene. It retains
the graph; the registry and scene must outlive the runtime. Destroy only after
rendering has stopped. Descriptors require the current `struct_size`.

The runtime owns a private legacy mixer, preserving existing applications and
preventing unexpected interaction with legacy voice stealing. Logical admission
reserves every expanded child, including delayed ones, against the configured
voice budget. A05 can replace this conservative policy with explicit physical
and virtual pools. There is no implicit virtualization in A04.

`su_audio_event_post` expands and validates outside rendering, acquires every
resident generation transactionally, and returns a generational instance handle.
Failed admission releases all newly acquired references and leaves the commit
watermark unchanged. Preparation and posting may allocate indirectly through
future adapters, so neither is a device-callback API. This implementation's post
scratch and instance storage are preallocated.

`su_audio_events_update` retires completed slots and releases their resident
references outside rendering. Post calls it automatically before capacity
admission. Stats include completed-but-not-collected slots until update runs.
Generation exhaustion retires a slot rather than making an old handle valid.

## Clock and ordering

Times and delays are output sample frames at the graph's explicit sample rate;
create rejects a different runtime rate. Internally, rendering always fills a
128-frame quantum and splits it at leaf starts and stop commands. External
reads consume the cached stereo float PCM. Splitting external reads therefore
does not change RNG, gain ramps, stop behavior or command boundaries.

`su_audio_events_clock` returns the first sample not yet rendered, not the next
sample to be delivered from the cache. Schedule new posts/stops at or after this
clock. Already-rendered commands are rejected and counted; audio is never
retroactively edited. Zero-length render is a no-op. End-of-clock overflow is
an explicit resource failure; a multi-quantum read may have delivered a prefix
before that failure. Callers needing transactional output must check their
requested horizon in advance.

At the same sample, stop commands apply in enqueue order before starts. Starts
follow successful admission order, then deterministic graph expansion order.
Scene/listener snapshots are sampled once at the beginning of each quantum.
Fresh child positions are resolved from those presentation transforms; live
children inherit camera/emitter movement without simulation reads. Destroyed
emitters cancel pending children and stop live ones at the next unrendered
quantum. Already-cached output is unaffected.

The integrated path uses a 256-entry, direct-mapped spatial-control cache local
to each quantum. Entries match the full emitter handle (including generation)
and the exact authored gain and sample-rate-adjusted pitch bits. Matching leaves
reuse scene resolution, curve evaluation, attenuation, pan and Doppler results.
Collisions simply recompute. Per-instance and per-bus parameter bindings still
run independently for every leaf, and every voice still updates controls and
audibility before ranking every quantum. No virtual cursor, filter, ranking or
wake policy changes. The cache uses bounded stack scratch, allocates nothing,
and is neither persistent nor serialized, so checkpoint format is unchanged.

`audio_controls_identical` compares every rendered PCM buffer and saved
checkpoint from the runtime test suite against a separately compiled uncached
reference. This includes shared emitters, cache collisions, mixed authored
gain/pitch, moving listeners/emitters, Doppler, filtering, all virtual modes,
parameter bindings, steals and partial-quantum checkpoint restoration.
For repeatable Release comparisons, build `bench_audio_system_reference` and
alternate it with `bench_audio_system` using the same arguments.

## Stop and release

`su_audio_instance_stop`, `su_audio_emitter_stop` and `su_audio_scope_stop` enqueue
bounded timestamped commands. Instance handles reject stale generations;
emitter/scope commands match instances present at their scheduled sample. They
remove every unstarted child, including a child starting at exactly that sample.
Unrelated instances continue.

Zero release frames hard-stop at the scheduled sample. Positive release frames
use an exact integer countdown in the private event-mixer path: the stop sample
retains the current gain and the voice is silent after the specified number of
frames. A later stop overrides the previous release starting from its current
gain. This does not change the legacy `su_audio_stop` ramp behavior. References stay pinned
until tails finish and the owner collects the completed instance. Queue-full
returns `SU_E_RESOURCE_LIMIT`, never successful silent command loss.

## Committed events and recovery

The caller provides a nonzero session generation and committed sequence.
Speculative events must not be posted. Successful admission advances `next_seq`
to `commit_seq + 1`; gaps are permitted, and older/out-of-order sequences return
`SU_E_STALE_REVISION`. Sequence `UINT64_MAX` is rejected to avoid watermark wrap.
There is one cursor per presentation session, shared across event/emitter IDs,
matching the existing committed-host-event cursor's sequence rule.

Snapshot the cursor outside speculative world state. Restore can move it
forward within the same session, never backward. Recreating a presentation
runtime for recovery requires restoring that cursor before consuming replayed
events. Restore is a watermark operation, not a PCM/filter checkpoint. A09 owns
full checkpoint continuation. An increasing session generation explicitly resets
the cursor only after instances, stop commands and cached output are drained.

## Boundedness and extension points

Creation bounds instances, leaves per instance, total admitted logical children
and queued stops. The instance/leaf arrays and render cache never grow. Runtime
stats report the exact allocated payload bytes excluding allocator overhead,
the private legacy mixer, graphs, scene and resident assets. Each component has
its own ownership and accounting; summing only runtime bytes is not a total
engine memory measurement.

Stable graph node IDs, authored bus IDs, emitter handles, instance handles,
scope IDs and admission sequences are handoff points. A06 routing, A05 voice
policies, A07 live parameters, A08 clustering and A09 full checkpoints are not
implemented by storing these identifiers. A10 should expose bounded snapshots
without writing files during rendering.

## Tests

`ctest --test-dir build-event -R '^audio_event$' --output-on-failure` checks
synthetic impulses at 0/1/127/128/129, nested offsets, same-sample cancellation,
independent emitter scopes, stale handles, finite pool/queue refusal, failed-post
watermarks, duplicate/out-of-order recovery, clock overflow, late commands,
release tails and bit-identical PCM under variable external chunks.

The test reports one 48,000-frame scalar render's CPU-clock observation and
runtime payload size. It is not a latency distribution or performance gate.
No new direct-device or cross-platform exact-PCM guarantee is claimed.

## Integration evidence (2026-10-20)

Windows 11 x64, Intel Core i5-12400F, Clang 23.1.3 and MSVC 19.44.35229.
The exact A04 index snapshot was exported with
`git checkout-index --all --force --prefix=build-pack/`. Its isolated strict
Clang Debug build passed all eleven audio CTests, including graph allocation
and callback side-effect instrumentation. Commands from the worktree:

```powershell
. ./tools/env-windows.ps1
cmake --build build-a01-accepted --target test_audio_event test_audio_graph test_audio_graph_alloc test_audio_spatial test_audio_asset test_audio_callback test_audio test_audio_stream test_audio_lowpass test_audio_adpcm test_audio_device -j 4
ctest --test-dir build-a01-accepted -R '^audio_' --output-on-failure
cmake --build build-event -j 4
ctest --test-dir build-event --output-on-failure -j 4
cmake --build build-event-cl --target test_audio_event test_audio_graph test_audio_graph_alloc test_audio_spatial test_audio_asset test_audio_callback test_audio test_audio_stream test_audio_lowpass test_audio_adpcm test_audio_device -j 4
ctest --test-dir build-event-cl -R '^audio_' --output-on-failure
cmake --build build-event-ogg --target test_audio_event test_audio_graph test_audio_graph_alloc test_audio_spatial test_audio_asset test_audio_callback test_audio test_audio_stream test_audio_lowpass test_audio_adpcm test_audio_device -j 4
ctest --test-dir build-event-ogg -R '^audio_' --output-on-failure
```

The build directories were configured with native and Vulkan rendering disabled;
`build-event-cl` uses MSVC Debug and `build-event-ogg` uses Clang Release with
pinned optional import/Ogg dependencies enabled. The MSVC compiler and Windows
SDK tools were added to PATH before its build. All eleven affected tests passed
in both configurations. The full Clang Debug run based on main `9971957` passed
111 tests, with the two POSIX-only tests explicitly skipped, zero failures.
Later incoming render work requires its own merged-tree CI; these local counts
are not claims about tests added after that build.

Local source SHA-256 values for this A04 checkpoint:

| Path | SHA-256 |
| --- | --- |
| `include/sutekh/audio_event.h` | `2F69F2B67FD4AED6F54240AB1B40310AD656CF7EAF649594A8DCB99BB37D21BB` |
| `src/audio/su_audio_event.c` | `01434B620E10361F87BAF9DDDD613593F96D7987A9AEB365443AE100691376A9` |
| `src/audio/su_audio_event_int.h` | `D88FDB51611D76A8AB28E4C12D6AA795AF77FFAC25FE9C48ECD61229AE33F4DB` |
| `src/audio/su_audio_mix.c` | `86165714D1A34D5A20F4E65F6CB49493BF3574E370D18E0AFC4ADD2319117C83` |
| `tests/audio/test_audio_event.c` | `59259D78F4807F52F70FF3A06CF17B6E01028C8540FE55A73A67522109C3D1D5` |

`build-a01-accepted/test_audio_event.exe` SHA-256:
`A5E37D2A2E5B02260337BEB07A030569019816CB0C5EF30F58EEEC961FB6EEF1`.
`build-event-cl/test_audio_event.exe` SHA-256:
`7C7229739C6E9B714F029AEEEDD6E0FFE8DC8901116559EE119F71B62621BD95`.
Source/artifact hashes identify these local observations, not reproducible-build
guarantees. After normal integration of main `29894fa`, the full strict Clang
Debug suite passed 112 tests with the same two POSIX skips and zero failures
(114 total, 75.22 seconds). Clang AddressSanitizer additionally passed spatial,
graph and event CTests, zero failures, using:

```powershell
cmake -S . -B build-event-asan -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug -DSUTEKH_NATIVE=OFF -DSUTEKH_RENDER=OFF -DSUTEKH_ASSET_IMPORT=OFF '-DCMAKE_C_FLAGS=-fsanitize=address'
cmake --build build-event-asan --target test_audio_event test_audio_graph test_audio_spatial -j 4
ctest --test-dir build-event-asan -R '^audio_(event|graph|spatial)$' --output-on-failure
```

The toolchain's `lib/clang/23/lib/windows` directory was added to PATH for its
ASan runtime DLL. `build-event-asan/test_audio_event.exe` SHA-256:
`0EED3DFCA7D1D770CE08B4B3608F0F7651CFA7482DC0A686EC77C5DA6AEB4372`.
This is address-safety test coverage, not exhaustive leak or race detection.
Concurrent adapter stress and long-duration device profiling remain unrun.
Cross-platform repository CI is a required merge gate.
