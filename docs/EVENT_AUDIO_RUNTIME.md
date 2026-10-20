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
