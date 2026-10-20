# Bounded event audio graphs

## Contract

`include/sutekh/audio_graph.h` defines an opaque prepared `SuAudioGraph`.
Preparation is a control-thread operation, uses the strict shared `su_json`
reader from `su_ir`, and never touches mixer/device state. Link the graph
implementation with `su_ir` and the audio asset registry implementation.
`su_audio_graph_prepare(json, len, assets, limits, &out, diagnostic)` returns a
status and leaves `out` null on every failure. Diagnostics contain status,
node ID when applicable, input byte offset when available, failed bound, and
message. Structural graph diagnostics may have offset zero.

Preparation acquires one pinned asset reference per sound node, including
unreachable sound nodes. Failed preparation releases all acquired references.
Successful graphs release these pins only when their final reference is released.
The caller-owned `SuAudioAssets` registry must outlive all graphs.
`su_audio_graph_retain` and `su_audio_graph_release` let event instances retain
the exact graph generation that produced their leaves. Retain/release and asset
registry operations are serialized on the control thread, not thread-safe
reference counting. The graph's compiled content is immutable. Concurrent
expansions are safe with independent output buffers and caller state while a
retained graph remains alive. Expansion leaves are borrowed asset references:
keep the graph retained until playback finishes, or acquire independent asset pins.

## Strict format

`schemas/audio_events.schema.json` describes the structural format. Native
preparation additionally enforces exact integer-literal syntax (no fraction or
exponent in IDs, weights, offsets, or sample rate), duplicate property rejection,
UTF-8 validity, finite local float conversion, graph references, and caller limits.
Every object rejects unknown properties. IDs are exact nonzero u32 integers,
not string hashes. Node/event namespaces are distinct. Source node/event order
does not affect lookup. Each namespace rejects duplicates, including unreachable
nodes. Case values, flags and bus IDs permit zero. All nodes are validated,
not only the event-reachable subgraph.

```json
{
  "schema": "su_audio_events_v1",
  "numeric_profile": "audio_float_local_v1",
  "seed_policy": "event_emitter_sequence_v1",
  "sample_rate": 48000,
  "nodes": [
    {"id": 1, "type": "sound", "asset": {"id": 1, "generation": 1}},
    {"id": 2, "type": "sound", "asset": {"id": 2, "generation": 2}},
    {"id": 3, "type": "random", "children": [
      {"node": 1, "weight": 1}, {"node": 2, "weight": 3}
    ]}
  ],
  "events": [{"id": 7, "root": 3}]
}
```

- `sound`: required asset `{id,generation}`; optional `gain` defaults to 1
  (finite, nonnegative), `pitch` defaults to 1 (finite, greater than 0 and at most
  64), `bus_id` defaults to 0, `flags` defaults to 0. Only `SU_AUDIO_LOOP` and
  `SU_AUDIO_SPATIAL` bits are accepted; flags do not infer asset loop metadata.
- `random`: nonempty `children` of `{node,weight}`, positive u32 integer
  weights, unique child IDs. Weight sums use u64. Exactly one child is chosen.
- `sequence`: nonempty `children` of `{node,offset_frames}`. Each offset is an
  exact u64 delay relative to this node's invocation, in graph output frames,
  not source clip frames or accumulated clip durations. Nested offsets add.
  Output preserves authored child order, even when offsets are not sorted.
- `parallel`: nonempty `children` of node IDs, expanded in authored order at
  the same invocation delay. Repeated child IDs deliberately repeat expansion.
- `switch`: numeric `switch_id`, required `default` node ID, and `cases` of
  `{value,node}`. Case values are unique u32 integers. Empty cases are allowed.
  Missing switch snapshots and unmatched values choose the explicit default.
- `blend`: numeric `parameter_id`, nonempty `points` of `{value,node}`.
  Points must be finite and strictly increasing after local float conversion.
  Values clamp at endpoints; an exact point expands only that child; intermediate
  values expand the two adjacent children with linear complementary gains.
  Each selected subtree retains its delay. A reached blend with no matching
  parameter snapshot fails explicitly; there is no guessed parameter default.

`sample_rate` is required (1000..384000). `su_audio_graph_sample_rate` exposes
it; the event runtime must match its output clock or reject the graph. Asset
sample rates may differ because mixer resampling is separate.

`audio_float_local_v1` uses finite native float leaves and local decimal conversion:
up to 64 significand digits, at most 96 token bytes, explicit exponent magnitude
at most 10000. Integer fields retain the shared parser's exact u64 handling instead.
Fractional conversion accumulates decimal digits in double, applies a double
power of ten, then narrows to float. Underflow may become zero (thus invalid for
pitch). This is a named local presentation profile, not cross-platform bit equality.
Extreme exponent overflow is rejected. Preparation never changes process locale.

## Bounds and admission

All five `SuAudioGraphLimits` fields are mandatory and positive. Implementation
ceilings are 65536 nodes, 262144 edges, 128 node levels, 65536 expanded leaves,
and 16777216 JSON bytes; larger limits are invalid arguments. Event count is
also bounded by `max_nodes`; switch defaults count as edges. JSON container depth
is separately capped at 8. Cycles, undefined roots/children, duplicate IDs,
excessive depth (including shared subgraph paths), and nested u64 delay overflow
are rejected before publication. Worst-case leaf expansion sums sequence/parallel
children, takes the largest random/switch child, and takes the largest adjacent
blend pair. Shared nodes count once per invocation, not once per ID.

Preparation memory is bounded by input bytes, node/event count, and configured
edge capacity. DAG validation memoizes depth, expansion, and maximum delay.
Expansion uses a bounded recursive stack (at most 128 levels), scans bounded
child/snapshot arrays, and performs no heap allocation, registry operations,
I/O, or shared RNG mutation. This is a bounded reference implementation, not a
claim of constant-time callback execution or a measured real-time budget.

## Expansion and transactional caller state

`su_audio_graph_expand(graph, event_id, seed, emitter, commit_seq, leaves,
capacity, &count, diagnostic)` uses `size_t` capacity/count. Its leaf contains
`asset`, `delay_frames`, `gain`, `pitch`, `node_id`, `bus_id`, and `flags`.
It preflights the complete selected expansion before writing. Any error returns
count zero and leaves output untouched, including insufficient capacity and
missing parameters. `leaves == NULL` is valid only with zero capacity; this is
not a required-capacity query. Caller buffers, count, diagnostic, graph, and
snapshot/history storage must not overlap. Snapshots and histories must remain
stable for the entire call and must not be mutated concurrently.

`su_audio_graph_expand_ex` inserts `const SuAudioGraphExpandState *state` after
`commit_seq`. Switch entries are `{switch_id,value}`; parameter entries are
`{parameter_id,value}`. Duplicate snapshot IDs and nonfinite parameters fail.
Each snapshot/history array count is bounded by graph node count. Snapshot order
is irrelevant; unused snapshots are permitted.

Optional history entries are `{node_id,last_child_id,next_child_id}`. Start
`last_child_id` at zero. A history entry must name a random node with at least
two distinct children, and a nonzero prior child must belong to that node.
`next_child_id` is caller-owned scratch, ignored on input. A successful expansion
updates it and commits `last_child_id`; failures leave both fields unchanged.
Unreached histories preserve `last_child_id`. The prior child is excluded and
remaining weights are renormalized. Repeated invocations of the same random
node inside one trigger use the same choice and commit one history transition.

History scope is explicitly caller-owned: normally one set per graph generation
and emitter, reset on graph replacement. Sharing a set across emitters produces
global history and requires serialized calls. The API does not track the emitter
for the caller. No history entries means no repeat avoidance, including the plain
`expand` wrapper. This implements one-previous-child exclusion, not arbitrary
repeat windows, shuffle bags, or persistent sequence cursors.

## RNG version

`event_emitter_sequence_v1` freezes the following unsigned-u64 algorithm;
all arithmetic wraps modulo 2^64. Define `mix(x)` by:

```text
x = (x XOR (x >> 30)) * 0xbf58476d1ce4e5b9
x = (x XOR (x >> 27)) * 0x94d049bb133111eb
return x XOR (x >> 31)
r = mix(seed XOR mix(emitter) XOR mix(commit_seq + 0x9e3779b97f4a7c15) XOR mix(node_id))
pick = r modulo sum(eligible weights)
```

Walk eligible children in authored order, subtracting weight until `pick` falls
inside a child interval. Modulo mapping has its specified small modulo bias; it
does not promise rejection-sampled uniformity. There is no graph-global state,
worker completion order input, or platform hash. DAG reuse of a random node in
one trigger deliberately gives the same choice. Replay with history also requires
the same caller history and snapshots. The synthetic golden fixture uses seed 77,
emitter 13, random node 10, weights 2/2, and commit sequences 0..15; expected node
IDs are `1,2,2,1,1,2,2,2,2,2,2,2,2,2,2,2`.

## Validation and integration boundary

The synthetic test is `tests/audio/test_audio_graph.c`; it exercises strict
parsing, exact IDs, cycles, bounds, stale assets, rollback of partial pinning,
deterministic variation, switch defaults, blend endpoints/crossfades, repeat
avoidance, and atomic capacity errors. Direct local Windows command:

```powershell
. tools/env-windows.ps1
& $env:CC -std=c17 -Wall -Wextra -Werror -Iinclude tests/audio/test_audio_graph.c src/audio/su_audio_graph.c src/audio/su_audio_asset.c src/ir/su_json.c -o local/test-a03-graph.exe
& local/test-a03-graph.exe
```

This component does not implement playback, scheduling, mixer admission,
sample-rate conversion, parameter declaration registries, device callbacks, or
an asset authoring importer. Missing blend snapshots are diagnosed at expansion,
not preparation: there is no parameter registry in the prepare API. The integrator
owns build target wiring and event-runtime retention/admission validation.

## A03 validation record

Task state: submitted for integration, 2026-10-20. Base commit:
`7e3c6eb`. Owned paths modified: `include/sutekh/audio_graph.h`,
`src/audio/su_audio_graph.c`, `tests/audio/test_audio_graph.c`,
`docs/EVENT_AUDIO_GRAPH.md`, and `schemas/audio_events.schema.json`.
No build configuration, commit, publication, or issue state was changed by this
component implementation.

Windows x64, Clang 23.1.3, C17, `-Wall -Wextra -Werror`: the direct command
above passed with **850 checks, zero failures**, exit 0. The invalid UTF-8 corpus
preserves byte `0xff` through an unsigned-byte view; it is not weakened to ASCII.
The 128-level valid DAG, 129-level rejection, shared-subgraph expansion exhaustion,
u64 maximum delay, delayed overflow rejection, high u32 IDs, and 64-bit weight
sums are exercised synthetically.

Allocation-instrumented build (same shell environment):

```powershell
& $env:CC -std=c17 -Wall -Wextra -Werror -Iinclude -Dmalloc=a03_malloc -Dcalloc=a03_calloc -Dfree=a03_free -c src/audio/su_audio_graph.c -o local/a03-graph-alloc.obj
& $env:CC -std=c17 -Wall -Wextra -Werror -DA03_TEST_ALLOCATORS -Iinclude tests/audio/test_audio_graph.c local/a03-graph-alloc.obj src/audio/su_audio_asset.c src/ir/su_json.c -o local/test-a03-graph-alloc.exe
& local/test-a03-graph-alloc.exe
```

Result: **851 checks, zero failures**, exit 0; the extra check asserts zero graph
heap allocation/free operations during expansion, including snapshots, repeats,
crossfades, and error paths.

AddressSanitizer build and run:

```powershell
& $env:CC -std=c17 -Wall -Wextra -Werror -fsanitize=address -g -Iinclude tests/audio/test_audio_graph.c src/audio/su_audio_graph.c src/audio/su_audio_asset.c src/ir/su_json.c -o local/test-a03-graph-asan.exe
$env:PATH = "$(Get-Location)/local/toolchains/llvm-23.1.3/LLVM/lib/clang/23/lib/windows;$env:PATH"
& local/test-a03-graph-asan.exe
```

Result: **850 checks, zero failures**, exit 0, no sanitizer reports. The first
launch without the sanitizer runtime directory on PATH failed to start; the
reported successful result is the corrected invocation above.

MSVC 19.44, with the same environment's SDK headers/libraries and `cl` selected
from the installed x64 compiler directory:

```powershell
New-Item -ItemType Directory -Force local/a03-msvc | Out-Null
& $cl.FullName /nologo /std:c17 /W4 /WX /Iinclude tests/audio/test_audio_graph.c src/audio/su_audio_graph.c src/audio/su_audio_asset.c src/ir/su_json.c /Folocal/a03-msvc/ /Felocal/test-a03-graph-msvc.exe
& local/test-a03-graph-msvc.exe
```

Result: **850 checks, zero failures**, exit 0. Native graph and test warnings are
errors in both compiler runs. Python `jsonschema.Draft202012Validator.check_schema`
and validation of this document's synthetic JSON example both passed. The owned
paths passed `git diff --check`.

SHA-256 fingerprints of the tested source/dependency snapshot and artifacts
(documentation intentionally excludes its own recursive fingerprint):

```text
18a2cb64544b077fa0c90136bb7590c385c7c95068d193967428156fcf69c9ef  include/sutekh/audio_graph.h
da0f197880309ae078196d6206ee4fbd9cd9c5fcf0ec4df889defc7ad54d87f9  src/audio/su_audio_graph.c
4a92ecc3c988fb656c7bf4daef97c32fae551092e546ca725ec73a015584d3aa  tests/audio/test_audio_graph.c
091de18e3169a08b01f0581dd6e2209092c6c766325cdcbd952ac2bdd091b04b  schemas/audio_events.schema.json
c37608add0d0030cc15631f8425b8ac587e93631f29ed0ecf1c05eabbbb3f5f5  src/audio/su_audio_asset.c
3dfb955be739b24863984b0b340e904b07ee4671cb2089de9a3b3670fdb1b10b  include/sutekh/audio_asset.h
9d7b7ddd45393cc197fb0c346b02fffe41df142cc888fe9617a1fb3287e6a417  src/ir/su_json.c
2e7841f36543403978518c045c588506d27b14676d200aba0916550f8262964d  src/ir/su_json.h
5169f8bf4f56c9424049f31080c5feee3668b86b1470082f91c7131f5dafdf1b  local/test-a03-graph.exe
e5bbfd4d5900b7ae85ff4c5bd361c6a35f861553e789be21dd6b83f8010c7cbd  local/test-a03-graph-alloc.exe
e41c50827d03a9bad097abe34f28ddfcf3a555bcb6e81f99e1a3fe340aa66942  local/test-a03-graph-asan.exe
21897eba464f33e126dc15bf06f989bd12f2a0a28e36b0a752e460f9f1c0b351  local/test-a03-graph-msvc.exe
```

Not run here: cross-platform compilers, device playback, full engine suite, and
independent acceptance review. No blocking component issue remains. Next work is
integrator-owned dependent runtime validation and build wiring; performance
budgets and broader repeat-history policies remain explicitly outside this slice.
