# M2-00 contract draft r1: generational entities, chunked storage, structural and event queues, IR 0.2

Status: DRAFT (contingent on M1 owner acceptance; additive and disabled by default until integrated). Headers: `include/sutekh/entities.h`, `include/sutekh/status.h` (adds `SU_E_STALE_HANDLE` = 21). Consumers: `src/world/entities/*` (table, queues), world/worker integration, IR 0.2 verifier + reference VM, snapshot v0.2, control plane, harness. A change after acceptance needs a new generation.

## 1. Identity and storage
- `SuEntity` = `(generation << 32) | slot` (header macros). `0` is invalid. Generations run 1..0xFFFFFFFE. Despawn increments the slot generation; a slot whose next generation would be 0xFFFFFFFF is **retired** (never reissued, never wrapped).
- A handle is valid iff its slot is in range, alive, and generations are equal. Everything else is `SU_E_STALE_HANDLE` (never a lookup by slot alone, never a silent fallback). A handle is never an address and never crosses a version boundary except as this 64-bit value.
- Storage: fixed capacity `max_entities` chosen at world creation, rounded up to whole chunks of `SU_CHUNK_SLOTS` (1024) slots, `<= SU_LIMIT_ROWS`. Columns are SoA **per chunk**. Chunks are the unit of parallel work (M1-04 sliced views). Growth is a later out-of-tick transaction, not M2-00.
- Allocation is deterministic: the **lowest free slot index** is reused first. A per-slot external id (u32, the M0 fixture `entity_ids`) is stored beside the generation; spawned entities carry the id given in the request or 0.
- Dead slots: `alive = 0`, all column values canonical zero, never visited by system row loops (reference VM skips them; natively compiled IR 0.1 kernels may process them but staged writes to dead slots are discarded at commit).
- Iteration order of every system: ascending slot over alive slots. Existing M0/M1 fixtures map to slots `0..n-1` all alive with generation 1, so IR 0.1 behaviour and canonical state dumps stay identical (capacity exactly `n` rounded to chunks; slots `>= n` are dead).

## 2. Structural requests and events
- Sources: control plane `world.spawn` / `world.despawn` (applied at a tick boundary, not mid-tick) and IR 0.2 ops (section 3).
- Systems never mutate the entity table. They append `SuStructCmd` / `SuEvent` records to **per-system, per-chunk bounded buffers** with capacities declared in the capsule `limits` (`max_struct_per_row`, `max_events_per_row`, defaults 1) and a global per-tick cap (`max_struct_per_tick`, `max_events_per_tick`). Exceeding any cap aborts the tick with `SU_E_RESOURCE_LIMIT`: nothing is published, the committed root, entity table and event log are unchanged.
- At the tick boundary, after all systems succeeded and before publication, requests are merged and applied in **canonical order** `(system_index, slot, seq)` (lexicographic), independent of worker count, chunking and completion order. Each applied command either takes effect or is *rejected* deterministically (despawn with a stale expected handle, spawn when no free slot, unknown template): a rejected command emits a `SU_EVENT_REJECTED`-kind record in the same canonical position and never fails the tick.
- Events are assigned `event_seq` in canonical order at commit (monotonic u64). The host reads them with `events.read` {cursor, max} -> bounded ordered list + next cursor. M2: events are host-visible only; systems cannot consume events (relationships/consumption are later). The event log is bounded (`event_log_max`, default 65,536 entries, oldest evicted with a `dropped` counter reported to readers; eviction is deterministic and part of canonical state).
- A tick commit is all-or-nothing: entity table changes, event log appends and column data publish together or not at all (M0/M1 committed/working roots; abort discards the working copy of the entity table too).

## 3. IR 0.2 (versioned extension; IR 0.1 modules keep working unchanged)
`ir_version` `"0.2"` adds, all total and bounded, none with host callbacks:

| Opcode | Arguments | Effect |
|---|---|---|
| `slot_u32` | `dst` | Define u32 = current row's slot index |
| `gen_u32` | `dst` | Define u32 = current row's generation |
| `tick_u32` | `dst` | Define u32 = low 32 bits of the tick being computed |
| `despawn_self` | `cond` (bool) | If cond: queue `SU_STRUCT_DESPAWN` for the current entity (expected handle = its handle) |
| `emit_event` | `cond` (bool), `kind` (const u32), `a`, `b` (u32 regs) | If cond: append an event with the current slot |

`spawn` from IR and entity-valued registers are explicitly **not** in M2-00 (M3 will need them; they require an entity-handle type and template contracts and get their own versioned step). Verifier rules: new ops only when `ir_version` is `"0.2"`; the capsule declares `limits.max_struct_per_row` / `max_events_per_row` and the verifier counts the worst-case emissions per row (straight-line IR, so exact) and rejects over-declaration (`SU_E_RESOURCE_LIMIT`); `emit_event` kind constants are validated; first-error stage order of M0_CONTRACT section 1 is extended with these checks in stage 6 (per-instruction). The semantic hash encoding gains the new opcode numbers (10..14: `slot_u32`, `gen_u32`, `tick_u32`, `despawn_self`, `emit_event`) and the capsule queue limits; the artifact format version is bumped to 2 only for 0.2 modules (0.1 hashes unchanged).
Native backends: modules using any 0.2 op execute on the **reference VM only**; `world.specialize` for them returns `SU_E_CAPABILITY_DENIED` with reason `ir_0_2_not_lowered` (scalar/AVX2 lowering of side-effecting ops is a later task). Modules that are IR 0.1 are unaffected.

## 4. Snapshot and replay
Snapshot format `"0.2"`: M0 fields plus `capacity`, `slots` (per slot: generation, alive, external id), `free_policy: "lowest_slot"`, `event_log` (bounded, with `dropped`, `next_event_seq`), and columns for ALL slots (dead = 0). Restore must reproduce handles, generations, retired slots, event log and next sequence exactly; a 0.1 snapshot restores into a 0.2 world as all-alive generation-1 slots. Replay comparison (`replay.compare`, later) compares canonical state and event logs across permitted backends and worker counts.

## 5. Tests required for acceptance (M2-00)
Entity churn (spawn/despawn thousands of cycles, lowest-slot reuse), stale-handle rejection (reuse, retired slot, out of range, zero), generation exhaustion retirement (fault-inject near the limit), canonical merge equality for worker counts 1/2/4 and shuffled completion orders, per-row/per-tick cap overflow aborting the tick with unchanged state, dead slots excluded, snapshot/restore round trip incl. event log, IR 0.1 fixtures bit-identical, new-op verifier negatives, reference VM known-answer cases for each new op, and mutation proofs for each.

## 6. Open points for the owner
Event consumption by systems, entity-typed registers, spawn from IR, relationship tables, dynamic growth, and numeric profile for any future float: all deferred, none required by M2-00 acceptance.
