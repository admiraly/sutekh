# Event audio foundation

The event layer is additive to `sutekh/audio.h`; existing descriptors and mixer
behavior are unchanged. Clients include `sutekh/audio_event.h`, which includes
the asset, spatial and graph contracts. External bank-format compatibility
belongs in downstream adapters, not engine data types.

## Ownership and threading

Preparation, asset lifetime, graph compilation and event admission run outside
the callback. Render storage is bounded at creation. The event runtime is a
single-owner presentation object: calls, including rendering, are serialized by
the owner, never by a callback mutex. It does not directly attach to the legacy
device thread. A device adapter must marshal presentation commands to that
owner before rendering; concurrent calls are not supported by this first API.
The legacy mixer/device SPSC contract is unchanged.

Assets have authored integer IDs and generations. Preparation copies finite PCM
into a fixed-capacity, byte-budgeted registry. Graphs and admitted instances pin
generations. Unload/replacement while referenced is refused. The registry must
outlive its graphs and runtimes. Real-time event admission accepts only prepared
resident assets; streams remain a separate legacy facility.

Spatial handles are generational. Camera transforms are presentation data, not
simulation reads. Curves clamp endpoints and interpolate with explicit units.
Voice descriptor extensions are versioned rather than altering the legacy ABI.

Strict JSON compiles into immutable bounded arrays. IDs are exact unsigned
integers, never unchecked string hashes. Acyclic container expansion is bounded
before admission; random choices depend on the named seed policy, not worker
completion order. Parameters, switches and repeat histories use explicit scopes.

## Clock, admission and recovery

The initial numeric profile is `audio_float_local_v1`: exact PCM within the same
build, decoder and floating-point mode, not cross-platform equality. Scheduling
uses unsigned 64-bit output sample frames. A fixed 128-frame internal quantum is
cached and split at leaf and stop timestamps. External read chunking does not
change mixer block boundaries. The clock is the first unrendered sample, which
can lead delivered output by at most 128 frames. Commands cannot target rendered
audio. Equal-time starts follow admission order, then graph expansion order.
Stop commands precede starts at the same timestamp.

Instances own their delayed children and resident references through release
tails. Cancellation removes delayed children. Failed admission does not consume
the committed-event watermark. Successful posts advance a session-scoped
monotonic watermark; duplicates/out-of-order sequences are rejected. Recovery
restores external watermarks without speculative world rollback. Increasing the
session generation is explicit, never a side effect of graph replacement.

## Layer and write-scope map

| Step | Scope | Builds on |
| --- | --- | --- |
| A01 | `audio_asset.h`, `su_audio_asset.c`, stream internals | Prepared generations and resident admission |
| A02 | `audio_spatial.h`, `su_audio_spatial.c` | Emitters, camera, bounded authored curves |
| A03 | `audio_graph.h`, `su_audio_graph.c`, `audio_events.schema.json` | Strict immutable container data |
| A04 | `audio_event.h`, `su_audio_event.c` | Instance clock, bounded admission, stop, watermark |
| A05 | New `audio_voice_policy.h`, `su_audio_voice_policy.c` | Leaf node ID, emitter, instance and trigger sequence |
| A06 | New `audio_bus.h`, `su_audio_bus.c` | Stable leaf `bus_id`; prepared scratch and routing |
| A07 | New `audio_parameter.h`, `su_audio_parameter.c` | Graph expansion snapshots; timestamp clock |
| A08 | New `audio_cluster.h`, `su_audio_cluster.c` | A05 logical identity and A06 pre-master outputs |
| A09 | New `audio_offline.h`, `su_audio_offline.c` | Quantum renderer, generations, cursor snapshot |
| A10 | New `audio_inspect.h`, `su_audio_inspect.c` | Stats, stable IDs and reason codes |

One integrator owns this header set and schema. Later agents own their new files,
tests and component docs; edits to existing headers, schema, CMake or shared
runtime internals require integrator coordination. Routing identifiers are
extension points, not claims that buses, virtualization, clustering or music
already exist. Extensions use versioned descriptors/functions, not repurposed
padding or silent ABI changes. Checkpoints encode IDs, generations, numeric
cursors and filter state, never pointers from native prepared views.

## Acceptance boundaries

Component notes contain exact APIs and tests. Synthetic PCM is the only public
fixture source. Offline mastering is separate from the final safety limiter.
Device/decoder availability, unsupported DSP controls, remaining policy work
and measured costs must be explicit. Preparation tests alone do not establish
a real-device callback guarantee.
