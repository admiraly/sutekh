# Worlds with several entity kinds — decision and design (issue 140)

Status: implemented in the first slice described at the end. This note records the choice, the API, the
formats and the identity rules before the code, as the issue asks.

## 1. The problem

A world is one schema (the union of every bound field) at one capacity. Every system of the schedule pays the
begin/commit/publish cost of every column of that union, for every row of that capacity, whether it binds the
field or not: `su_world_begin_tick` copies committed to working, `su_world_commit_staging` copies the system's
staging columns back, and `su_world_publish` copies working to committed. Row filters inside a system cannot
help, because the cost is paid before and after the system runs, not inside it.

A world of very different entity kinds therefore pays for its largest table in every system. A downstream game
measured a world of 2,000 ships (about 60 fields) and a fixed projectile pool of about 27,500 rows (about 15
fields): 93% of the pool rows were empty, and the idle tick spent most of its time hashing and committing those
rows. One table cannot fix that; the fix is per-kind storage with per-kind commit cost.

## 2. Decision

**Several tables per world (option a), not linked worlds (option b).**

Each table is an independent `SuWorld`: its own declared schema, its own capacity, its own entity table and
generational handle space. One host world owns 1..`SU_HOST_MAX_TABLES` tables, one schedule, one tick counter,
one event log, one input record stream, one snapshot and one digest. A schedule entry names the table it
iterates; it pays the begin/commit/publish cost of that table only.

Why (a):

- **One identity and one commit point.** (b) would have to define which child world's tick, log and event
  sequence is canonical, and every cross-child read would need a merge rule. (a) keeps the existing
  "a tick either publishes or leaves the committed root as it was" rule, applied to all tables together.
- **Cost is already per table.** `SuWorld` already has committed/working/staging roots and an entity table at a
  fixed capacity. A table is exactly that object; the host layer already owns one, so the change is a loop over
  tables, not a second world implementation.
- **Determinism.** Table order and schedule order are both fixed arrays. No new scheduling, no new merge.
- **(b) is not cheaper in this code.** A linked world still needs a container that steps children in order,
  routes inputs by handle, concatenates snapshots and folds identities — the same container work — plus a rule
  for cross-child handles. (a) reuses the handle space for that routing (section 4).

## 3. API

### 3.1 Tables at creation

`SuHostWorldDesc` gains (appended fields, version 4):

```c
uint32_t table_count;                 /* 0 or 1: the classic single-table world; 2..8: tables[] */
uint32_t table_reserved;              /* 0 */
const SuHostTableDesc *tables;        /* table_count entries; index = table index */
const uint32_t *capsule_table;        /* capsule_count entries; NULL = every capsule in table 0 */
```

```c
typedef struct SuHostTableDesc {
    uint32_t struct_size;
    uint32_t reserved;                 /* 0 */
    const SuHostSchema *schema;        /* required: the table's declared schema (the issue 144 rules) */
    uint64_t max_entities;             /* capacity; 0 = the table fixture's row count (an empty table needs >= 1) */
    const uint8_t *fixture_json;       /* optional: initial state of this table, same fixture format */
    size_t fixture_len;
} SuHostTableDesc;
```

Rules:

- `table_count` 0 or 1 keeps every earlier descriptor, schema, fixture, identity, snapshot, digest and replay
  byte for byte. `tables` and `capsule_table` must then be NULL.
- With `table_count >= 2`, `SuHostWorldDesc.schema` and `fixture_json` must be NULL: every table declares its
  schema in `tables[t].schema` (declared schemas are required, because a table may have no system that binds
  every lane, and a table with no systems cannot derive a schema). `capsule_table[i]` is the table of capsule
  i; a capsule's schema ID, version and layout generation must equal that table's declared ones.
- The capacities must sum to at most `SU_LIMIT_ROWS`; a table's capacity is at most `SU_LIMIT_ROWS`.
- A table with no systems is legal and costs nothing per tick (it is never begun or published).
- Host operators are table-local: the operator record's fields are checked against its table's schema, and its
  scratch is sized for that table's capacity.

### 3.2 Which table a system iterates

The capsule itself does not name a table. The world descriptor's `capsule_table` does, per capsule index, so
the IR capsule format is unchanged (an IR 0.3 capsule cannot carry an unknown top-level key, and a table is a
property of the world, not of the program). A system's reads and writes are that table's columns only; the
effect analysis runs over the whole schedule as before, and an edge between systems of different tables is
legal (they do not share columns, so it cannot conflict).

### 3.3 Cross-table handles

A world handle keeps the existing 64-bit layout `generation << 32 | world_slot`, and `world_slot` is the
table's local slot **plus the table's base**, where the base of table 0 is 0 and the base of table t is the sum
of the capacities of tables `0..t-1`. Consequences:

- **Single-table handles are unchanged**, because table 0's base is 0 and the slot is the local slot.
- **A handle carries its table**: the slot range of each table is disjoint and fixed at creation, so the table
  is a function of the handle alone. No table bits are stolen from the slot or the generation; the full 32-bit
  generation range and `SU_GEN_RETIRED` semantics are unchanged.
- **A handle of another table is rejected**: every table-qualified call checks that the handle's slot lies in
  that table's range and returns `SU_E_STALE_HANDLE` when it does not (documented as "stale handle", the same
  code a dead or retired handle gets). Table-local calls (`su_host_despawn`, input records) route by the
  handle's table, so a stale handle from any table is rejected by the table it belongs to.
- Host spawn/despawn event records carry the **world** slot in `slot`/`a` (and the generation in `b`), so a
  record identifies its table without a new event field. IR events carry the emitting row's local slot and
  their `system_index`, which names the table through the schedule.

New and extended calls (all additive; the old names keep their meaning as table 0):

| call | meaning |
|---|---|
| `su_host_table_count` | number of tables (1 for a classic world) |
| `su_host_table_capacity(w, t)` | capacity of table t |
| `su_host_table_schema_id(w, t)` | declared schema ID of table t |
| `su_host_table_base(w, t)` | first world slot of table t |
| `su_host_table_of_handle(w, h, *t)` | the table a handle belongs to (`SU_E_STALE_HANDLE` when out of range) |
| `su_host_spawn_in(w, t, ...)` | spawn into table t |
| `su_host_despawn_in(w, t, h, err)` | despawn, rejecting a handle of another table |
| `su_host_entity_at_in`, `su_host_alive_in` | table-qualified reads |
| `su_host_column_in`, `su_host_read_in`, `su_host_field_count_in`, `su_host_field_info_in`, `su_host_field_id_in` | table-qualified column and schema reads |
| `su_host_field_value_type_in`, `su_host_read_i64_in` | table-qualified 64-bit helpers |
| `su_host_memory` | exact per-table and total heap accounting |

## 4. Tick semantics

A tick is: apply the tick's input record to the committed roots (routing each write by its handle's table),
begin every table that has at least one scheduled system, run the schedule in order with each entry on its
table, zero the dead rows of the table a system just committed to, publish every begun table, increment the
tick. A failure in any system aborts every open table, so the committed root of the whole world is unchanged.

Order is canonical: table order is the descriptor array order and schedule order is the schedule array order.
Neither depends on a thread count, and the future chunked tick (issue 142) splits rows inside one table without
changing which table an entry runs on.

An input record stays the same bytes: writes are sorted by (world slot, field id), and world slots are
table-disjoint, so the canonical order is table-grouped without a table field in the record. Record version 1
is unchanged, including for multi-table worlds.

## 5. Identity, snapshot and replay

**Identity.** A single-table world's identity is exactly as before. A multi-table world folds the table
descriptor into it:

```
identity = SHA-256("SUHTABLES" 0x00 || identity_as_before_without_tables || u32 table_count ||
                   per table in index order: u32 schema-ID length | schema ID | u32 schema_version |
                   u32 layout_generation | u64 capacity | u32 lane count |
                   per lane ascending ID: u32 id | u32 value type | u32 name length | name)
```

So a snapshot or replay of a multi-table world loads only into a world with the same tables, capacities,
schemas and order. The schema digest of table 0 and the schedule's semantic hashes are already in
`identity_as_before`, so the fold adds only what the old identity could not express.

**Snapshot.** A single-table world keeps version 1 (no operator state) or version 2 (operator state) byte for
byte. A multi-table world writes version 3:

```
"SUHSNAP" 0x00 | u32 version 3 | world identity (32) | u64 tick | u32 table_count |
per table in index order: u64 state length | canonical world state (SUWSTATE) of that table |
u64 log length | event log (SUEL) | [u64 operator state length | operator state (SUOPSTAT) when the world has state]
```

The state digest (`su_host_digest`) is the SHA-256 of the snapshot bytes, so it covers every table and the
shared log by construction. The fast digest is the same function over the same bytes. Restores are strict and
re-encode the restored world to require canonical bytes, as version 1 and 2 restores already do.

**Replay.** The replay header's schema digest field stays table 0's schema digest; the world identity in the
same header covers every table, and the reader already refuses a world whose identity differs, so replays of
multi-table worlds are bound to the same tables. Keyframes are snapshots (version 3), input records are version
1, and the digest chain is unchanged.

**Single-table byte identity** is therefore structural, not a promise: the table count is 1, the new code
paths branch on `table_count > 1` only for identity, snapshot version and the tick loop's table count, and the
tick loop with one table calls the same schedule function as before. The tests compare a classic world's
snapshot bytes and digests against a recording made on the parent revision.

## 6. Memory

`su_host_memory` reports, per table, the exact bytes `su_world_memory` already accounts (committed, working and
staging columns, entity table, metadata) plus the host's own per-table and shared allocations. A multi-table
world pays `fields_t * capacity_t` cells instead of `all_fields * max_capacity`, which is the point of the
change. The test compares 10,000 ship rows and 200,000 projectile rows as two tables against one union table of
210,000 rows.

## 7. Future chunking (issue 142)

The chunked tick partitions rows inside one system's table. A chunk carries its table's world, so the partition
is still a function of that table's capacity and the merge order is still canonical chunk order; nothing in the
multi-table design depends on the worker count. The per-worker scratch of a host operator is sized from its
table's capacity, and a cross-table operator (section 8) would need one scratch per table it reads.

## 8. First slice and follow-ups

Implemented in this change: multi-table creation, per-table schema, capacity and fixture, per-table
spawn/despawn/handles, per-table begin/commit/publish, multi-table identity, snapshot version 3, digest, input
routing, memory accounting, and the motivating benchmark.

Deferred, each independently useful:

1. **Cross-table host operators.** `space3d.query` takes its movers from one table and its targets from
   another: the operator descriptor needs a second table selector and `SuHostOperatorRun` needs the target
   columns (a trailing field, so existing operators keep their layout). The single-table operator path is
   unchanged in this slice, and a multi-table world cannot yet bind an operator to a table other than its own
   with a cross-table target set.
2. **IR entity operations in multi-table worlds.** `spawn`, `despawn_self` and `emit_event` run through the
   entity tick, which today owns one world's chunk cache and one entity table. Multi-table worlds refuse a
   capsule that uses entity operations (`SU_E_CAPABILITY_DENIED` at creation, named in the message) until the
   entity tick is table-aware. Host spawn/despawn works in every table today.
3. **Per-table operator state.** State keys are world handles already; the DROP_DEAD purge is per table in this
   slice, and a stateful operator may only be scheduled in table 0.
4. **Table-level rollback.** `su_host_snapshot_restore_inplace` restores every table (scratch entity tables are
   allocated per table), but there is no "restore table t only" call.
