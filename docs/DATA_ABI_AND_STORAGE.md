# Data, storage, and native ABI

## 1. Identity is not address

M0 uses fixed rows and stable fixture entity IDs. Later entity identity is a generational handle with explicitly sized index and generation fields; zero is reserved as invalid. Exhausted generations are retired rather than silently wrapped into a potentially live stale ID. Physical rows, chunk indices, addresses, and GPU instance indices are not entity identity.

Schema and field IDs remain stable across names and layouts. A field's type, default, numeric interpretation, and persistence role belong in schema metadata. A capsule binds to stable IDs once during admission; lookup does not occur by string on every entity.

## 2. Storage progression

M0 is one fixed-row SoA table with u32 columns. Each table has a row count, stable entity ordering, immutable schema generation, and allocated column capacities. All hot-path sizes are validated before pointer arithmetic.

M1 may add chunks and parallel current-row iteration. M2 introduces generational entities, tombstones or swap/compaction policies, relationships, structural command buffers, and schema migration. Choose a single deterministic storage policy per profile before benchmarking alternatives. A physical reorder must not change an order-sensitive gameplay result.

Do not implement a general archetype engine before the live-loop gate. Do not automatically pack every horde's members into consecutive global entity IDs; deaths, replacements, attachments, and upgrades make identity and locality different problems. Use explicit membership ranges/indirection and measured packing policies in the RTS profile.

## 3. ABI boundary

The public ABI is C with explicit-width integers, opaque context handles, versioned structures, structure-size fields, status returns, and borrowed spans. Avoid compiler-dependent enums/bitfields, STL types, variadic calls, ownership across allocators, and implicit exceptions. Internal private C structs may evolve without becoming a public interface.

The native invocation receives validated column bindings and row spans. It does not own those buffers or retain their addresses. Each binding includes field ID, element type/stride, access permissions, length, and layout generation. Native code must either check supplied generations in its wrapper or be callable only through a wrapper that does.

Illustrative signature, to be finalized by the contract owner before workers implement it:

```c
typedef struct SuExecContext SuExecContext;
typedef struct SuInvocation SuInvocation;
typedef struct SuOutputs SuOutputs;
/* uint32_t status; no longjmp/exception propagation through this boundary. */
uint32_t su_invoke_v1(SuExecContext *ctx,
                      const SuInvocation *input,
                      SuOutputs *staged_output);
```

This is an interface sketch, not a completed header. M0-00 owns freezing the actual fields, alignment assertions, error codes, and lifetime rules. Changes after that point require a new generation and dependent tests.

## 4. Calling conventions

Use the host platform's C ABI, selected explicitly by the backend. Windows x64 register/stack rules differ from Unix System V conventions; the Windows ABI includes register arguments and caller-provided shadow space [S04]. Do not reuse earlier conversational `rdi/rsi` examples as a universal ABI.

Assembly kernels must preserve required nonvolatile registers, stack alignment, and floating-point control state. Add platform-specific ABI tests and unwind/debug metadata where required. Never claim Windows support solely because Linux-generated x86-64 bytes execute in one test.

## 5. Alignment, aliasing, and SIMD

Aligned fast paths require proven alignment. Provide an unaligned path or explicit rejection at a trusted internal boundary. Handle zero length, short arrays, nonmultiple vector tails, and overlapping ranges. The verifier or wrapper establishes nonaliasing assumptions; source annotations alone do not authorize out-of-bounds vector loads.

SIMD dispatch is per capability and supported OS register state, not merely CPU marketing name. A baseline scalar path always exists. AVX-512 and architecture-specific tuning are deferred until measurements justify their maintenance cost.

## 6. Serialization

Snapshots and network/replay data use a specified canonical encoding: versioned headers, explicit widths, little-endian integer values, length-prefixed byte/string data, stable entity/field ordering, and maximum lengths before allocation. Never serialize raw C structs or padding. The normalizer rejects duplicate field IDs and inconsistent counts.

M0 fixtures remain JSON for transparency. Binary snapshot encoding becomes necessary only when M1/M2 workload measurements justify it. The representation version and numeric profile are included in every state fingerprint. State equality compares canonical bytes or fields, not hash output alone when reporting a divergence.

### 6.1 Entity table in the world (M2-00c1)

The world owns a committed and a working generational entity table (`src/world/entities/su_ents.h`). Capacity is `max(initial rows, max_entities)`, fixed at creation through `su_world_create_ex`; `su_world_row_count` reports the capacity, which equals the initial row count for every fixture world. Dead rows are canonically zero in the working and committed roots: `su_world_zero_dead` runs after each `commit_staging` in a tick, and `su_world_publish` zeroes dead rows before it publishes.

Canonical state, version 2 (`SUWSTATE`, all integers little-endian): `"SUWSTATE" 0x00 | u32 2 | u64 tick | u32 field_count | entity table | per field in ascending id: u32 id | u32 type | u32 values[high_water]`. The entity table is the `SUENTABL` encoding of section 1 of the M2 entities contract (capacity, high_water, alive and retired counts, free policy `lowest_slot`, then a generation, external id and alive byte per slot below high_water). Version 1 (the M0 layout, all slots alive) still restores through `su_world_restore_canonical`.

Snapshot format `"0.2"` is what capture writes. After the 0.1 keys, in fixed order: `capacity`, `high_water`, `free_policy`, `retired_count`, `slots` (`[generation, alive, external_id]` per slot below high_water), `event_next_seq`, `event_dropped`, `event_log_max`, `events` (`[event_seq, tick, kind, system_index, slot, seq_in_row, a, b]` per retained event). Columns and `entity_ids` hold high_water rows. Format `"0.1"` still parses and restores; its capacity must equal its row count. The worked example is `tests/fixtures/snapshot02/worked_0_2.json` (sha256 in `worked_0_2.sha256`). The descriptor's entity IDs are checked only for slots still at their first generation, because a reused slot legitimately carries a different identity.

## 7. Layout specialization

A later release optimizer may explore SoA, AoSoA, hot/cold splits, and system fusion. It works on a snapshot of the access graph and produces a new layout artifact plus validated bindings. It cannot change public field IDs or invalidate live addresses because capsules never retain them.

Layout candidates are compared on representative end-to-end workloads, not one kernel. Count migration/copy costs, rendering reads, snapshotting, networking, and memory peak. Dynamic data-layout retuning during a match is off by default.

## 8. Allocation model

Use lifetime-based arenas for prepared plans, world columns, tick scratch, and candidate state. Capacities are explicit. Growth is an out-of-tick transaction or a recorded resource failure. The first implementation favors easy-to-audit ownership over a custom allocator zoo.

Reference snapshot staging may use more memory than release mode. Report committed state bytes, reserved capacity, working copies, snapshots, retired code, candidate workers, and asset/device allocations separately.

Sources: [SOURCE_NOTES.md](SOURCE_NOTES.md).

### 6.2 Schema history, migration and snapshot 0.3 (M2-01a)

Each field has a `u32` default (0 unless a migration sets one). `su_world_spawn` writes the default for every field the request does not name. The world owns the retired field-ID list (ascending, never shrunk within a lineage) and the migration lineage (raw SHA-256 migration hashes, oldest first); both are set at creation through `su_world_create_ext` and grow only by migration.

Digests. `su_world_schema_digest` is SHA-256 of `"SUWSCHEMA" 0x00 | u32 1 | u32 field_count | per field in ascending id: u32 id | u32 type | u32 default | u32 name_length | name bytes` (integers little-endian; access is not covered). `su_world_recovery_digest` is SHA-256 of `canonical_state || u32 layout_generation (LE) || schema_digest`, where canonical_state is the version 2 encoding of section 6.1. The version 2 canonical encoding itself is unchanged.

Snapshot format `"0.3"` (`SU_SNAPSHOT_FORMAT_03`). Capture writes it only when the lineage or the retired list is non-empty; otherwise it writes the 0.2 bytes unchanged. 0.3 is the 0.2 key set with `fields` entries `{"id","name","type","default"}` and two required top-level keys appended after `events`, in this order: `migration_lineage` (64-hex migration hashes) and `retired_field_ids` (ascending). Parsing accepts 0.1, 0.2 and 0.3. A 0.3 snapshot must carry a non-empty lineage or retired list. The golden is `tests/fixtures/snapshot03/worked_0_3.json` (sha256 in `worked_0_3.sha256`), produced by the migration `m_mixed` over `worked_0_2`.

Restore. `su_snapshot_restore_world(bytes, len, desc, allow_schema_change, ...)`: with `allow_schema_change` 0 the snapshot must match `desc` (schema identity, field table, layout generation) or the call returns `SU_E_SCHEMA_MISMATCH`. With 1 (rollback and recovery only) the schema comes from the snapshot header and the world is built without the original fixture; its fields carry `SU_ACCESS_READ`, because access is not stored, and the installed systems re-establish access.

Migration. The migration document and transform document are strict (limits, syntax, duplicate keys, shape, semantics, in that order; at most 64 ops). Ops: `add_field` (u32 default; `read`/`write` rejected), `rename_field`, `remove_field` (the ID is retired), and one `transform`. The transform is a `transform_format` 0.1 document with `writes`; its IR is verified by the unchanged `su_lir_verify` against a synthesized capsule whose table is the union of the source and destination schemas. `su_migrate_plan` computes the destination schema, per-system impact and resource admission; `su_migrate_prepare` builds the successor world aside (same entity table, capacity, tick and generations); `su_migrate_swap` publishes it with two pointer writes after every check. The migration hash is SHA-256 of `"SUMIGRATE" 0x00` followed by length-prefixed migration, transform and IR bytes. Resource figures in a plan are estimates from row and field counts; they are not measurements. Source: `src/world/migration/su_migrate.[ch]`. The post-commit checkpoint, native retirement and the runtime reply are owned by the runtime worker and are not part of this layer.
