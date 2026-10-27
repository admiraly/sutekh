# Worlds with several entity kinds — decision and design (issue 140)

Status: implemented in the two slices described at the end (slice 1: tables, per-table storage and identity;
slice 2: the table-aware entity tick, operator events, world-slot semantics, cross-table `space3d`, stateful
operators in any table). This note records the choice, the API, the formats and the identity rules before the
code, as the issue asks.

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

`SuHostWorldDesc` gains (appended fields, version 4, and `capsule_target_table` in version 5):

```c
uint32_t table_count;                 /* 0 or 1: the classic single-table world; 2..8: tables[] */
uint32_t table_reserved;              /* 0 */
const SuHostTableDesc *tables;        /* table_count entries; index = table index */
const uint32_t *capsule_table;        /* capsule_count entries; NULL = every capsule in table 0 */
const uint32_t *capsule_target_table; /* version 5: capsule_count entries; SU_HOST_TABLE_NONE = no target set */
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
  byte for byte. `tables`, `capsule_table` and `capsule_target_table` must then be NULL (a target set needs a
  second table).
- With `table_count >= 2`, `SuHostWorldDesc.schema` and `fixture_json` must be NULL: every table declares its
  schema in `tables[t].schema` (declared schemas are required, because a table may have no system that binds
  every lane, and a table with no systems cannot derive a schema). `capsule_table[i]` is the table of capsule
  i; a capsule's schema ID, version and layout generation must equal that table's declared ones.
- The capacities must sum to at most `SU_LIMIT_ROWS`; a table's capacity is at most `SU_LIMIT_ROWS`.
- A table with no systems is legal and costs nothing per tick (it is never begun or published).
- Host operators are table-local: the operator record's fields are checked against its table's schema, and its
  scratch is sized for that table's capacity (plus the target table's capacity when the capsule names a target
  set; section 8.2).
- `capsule_target_table[i]` is the table capsule i's operator reads as its target set (section 8.2), or
  `SU_HOST_TABLE_NONE`. It must differ from the capsule's own table and needs a host operator that declares
  target lanes; a v1..v4 descriptor has no such field (every capsule gets `SU_HOST_TABLE_NONE`).

### 3.2 Which table a system iterates

The capsule itself does not name a table. The world descriptor's `capsule_table` does, per capsule index, so
the IR capsule format is unchanged (an IR 0.3 capsule cannot carry an unknown top-level key, and a table is a
property of the world, not of the program). A system's reads and writes are that table's columns only; the
effect analysis runs over the whole schedule as before, and an edge between systems of different tables is
legal (they do not share columns, so it cannot conflict). A cross-table host entry (section 8.2) also reads
its target table's lanes; that read is not part of the effect analysis, which is sound because the host tick
is serial and ordered by the schedule. A future parallel tick would have to model it.

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
- **Every** event record carries a **world** slot in `slot`: host spawn/despawn records in `slot`/`a` (and
  the generation in `b`), IR `emit_event`/`despawn_self` records and host-operator `emit_event` records in
  `slot`. The record's table follows from its slot, so no event field names a table. `system_index` still names
  the system (and so the table) through the schedule, which orders the log.

The one slot rule, for IR and host records alike:

| operand | value |
|---|---|
| `slot_u32` | the row's world slot: the system's table base plus its local row |
| `gen_u32` | the row's generation in its own table |
| `handle_live(slot, gen)` | `slot` is a world slot of the system's own table; another table's slot is not live |
| `despawn_self` / `emit_event` `slot` | the emitting row's world slot |
| `run->table_base` | the operator's table base; an operator reports `table_base + local row` |
| `run->emit_event(slot, ...)` | `slot` is any world slot of the world (`0..world_slots-1`) |
| input record entity | a world handle (`generation << 32 \| world slot`) |

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
A table with no scheduled system is never begun, committed or published: its rows cost nothing per tick, and
only its tick counter is advanced with the world's (so its canonical state, and a snapshot of it, carries the
world tick and restores). Host spawn, despawn and input writes still work on such a table between ticks.

The entity tick is table-aware (slice 2). One command queue holds the tick's structural commands and events of
every table; one chunk cache holds one entry per scheduled system with that system's own table, rows, layout
generation and base; one event log holds the world's records. A system's chunk cache, entity view, alive and
generation views and event buffer are its own table's, so `despawn_self` and `emit_event` run in every table,
and the boundary applies each table's despawns to that table's working entity table before any table is
published. Merge order is unchanged: records are ordered by `(system_index, slot, seq)`, and `system_index`
names the system, so a tick's log is ordered by schedule position across tables. Host-operator events
(`max_events_per_tick`) join the same merge with the operator's schedule position and a world slot, so a table-1
operator's records interleave with table 0's IR records by schedule position. A table with no entity system
still pays nothing: its chunk cache is not built.

Cross-table reads (`space3d.query`, section 8) see the target table's **working** columns when that table is
begun (so a target system earlier in the schedule is visible), and its **committed** columns when it has no
system and is not in its tick (the tick-start state, which is also what its alive/generation views report).
Neither case allocates: the views are per-entry scratch allocated at world creation.

The effect analysis knows tables: two systems in different tables never share a column, so they cannot
conflict on a field whatever their field IDs; ordering edges and cycle detection still span the whole
schedule, which is what orders a tick.

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
                   per table in index order: 32-byte schema digest | u32 schema-ID length | schema ID |
                   u32 schema_version | u32 layout_generation | u64 capacity | u32 lane count |
                   per lane ascending ID: u32 id | u32 value type | u32 name length | name)
```

The per-table schema digest is the digest `su_world_schema_digest` computes for that table's world: it covers
the table's storage lanes, which is what the identity must bind beyond the declared names. A capsule that binds
a target table adds a trailing section (`SUHTARGET` 0x00 || previous identity || u32 bound entries || per
entry: u32 schedule index | u32 target table | u32 lane count | per lane: u32 bound field id), so a
cross-table world loads only where the same operator reads the same target lanes of the same table.

So a snapshot or replay of a multi-table world loads only into a world with the same tables, capacities,
schemas and order. The schedule's semantic hashes are already in `identity_as_before`; the fold adds the
per-table digest and descriptor the old identity could not express. Slice-1 multi-table identities are
unchanged by slice 2 (the target section appears only for a capsule that binds one), so slice-1 multi-table
snapshots and replays stay valid; a world that binds a target set is new and its snapshots are version 3 too.

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
staging columns, entity table, metadata) plus the host's own allocations: the input application buffers
(`input_storage`), the queued per-tick input records and their write arrays (`queued_inputs`), the entity-table
and event-log scratch of `su_host_snapshot_restore_inplace` (`restore_scratch`, zero until its first call) and
the world struct (`metadata`). `total` is their sum. The schedule's heap allocations (modules, VM contexts,
operator instances and their data, the event log) are not part of the world root; `su_evlog_memory_bytes`
reports the log separately. A multi-table world pays `fields_t * capacity_t` cells instead of
`all_fields * max_capacity`, which is the point of the change. The test compares 10,000 ship rows and 200,000
projectile rows as two tables against one union table of 210,000 rows, and checks that a queued record and a
first in-place restore move the categories and the total.

## 7. Future chunking (issue 142)

The chunked tick partitions rows inside one system's table. A chunk carries its table's world, so the partition
is still a function of that table's capacity and the merge order is still canonical chunk order; nothing in the
multi-table design depends on the worker count. The per-worker scratch of a host operator is sized from its
table's capacity, and a cross-table operator (section 8) would need one scratch per table it reads.

## 8. Slices, follow-ups and limits

### 8.1 Slice 1 (merged)

Multi-table creation, per-table schema, capacity and fixture, per-table spawn/despawn/handles, per-table
begin/commit/publish, multi-table identity, snapshot version 3, digest, input routing, memory accounting, and
the motivating benchmark. It refused IR entity operations and operator events in a multi-table world.

### 8.2 Slice 2 (this change)

- **Table-aware entity tick.** One command queue, one event log, one chunk cache with a per-system table, rows,
  layout generation and base, and per-table entity tables and event buffers. `despawn_self` and `emit_event`
  run in every table; the boundary applies each table's despawns to that table's working entity table, then
  every begun table is published. One table with no entity system pays nothing.
- **Operator events (#172) in a multi-table world.** A host operator's `emit_event` records join the same merge
  as the IR records, ordered by schedule position, with a world slot. `run->table_base` gives the operator's
  table base; `emit_event` accepts any world slot of the world.
- **World-slot semantics.** `slot_u32`, `gen_u32`, `handle_live`, `despawn_self`, `emit_event` and operator
  events all follow the one slot rule in section 3.3; a column handle stored by a system matches a host handle.
- **Cross-table `space3d`.** The operator descriptor declares target lanes (descriptor version 4), the world
  descriptor names each capsule's target table (version 5), and `SuHostOperatorRun` carries the target table,
  columns, rows, base and entity views. `space3d.query` takes its movers from its own table and its collidables
  and gates from the target table; the mover table's own non-mover rows are neither movers nor targets. The
  target table may have no system (its committed columns are read). A target row is static for the query: it is
  never swept (the target set has no previous position), so a moving ship is tested at its end position.
  Single-table `space3d` is unchanged. The host tick is serial, so a target read sees the schedule order; the
  effect analysis does not model the target reads (a parallel tick would need them, and the tick is serial
  today).
- **Stateful operators in any table.** The operator instance learns its table's base and the world's slot count
  at creation; its keys are world handles. The DROP_DEAD purge validates each key against the entity table of
  the table its slot belongs to, so an entry keyed by another table's live handle survives and one whose
  handle died in either table is erased. Covered by a test with an operator in table 1 holding table 0's
  handles.
- **Memory accounting** covers the categories section 6 lists, including the queued input records and the
  in-place restore scratch.

### 8.3 Deferred

1. **In-tick spawning by a host operator into a named table.** The structural command queue carries despawns
   only (`SU_STRUCT_SPAWN` is reserved and has no producer); a spawn needs a per-table free-slot allocation at
   the boundary and a canonical way to order it against the tick's other structural commands, plus a value
   payload that does not fit the current 32-byte command record. Doing it correctly is a command-queue format
   change, not a flag, so it is left out rather than half-defined. Host `su_host_spawn_in` between ticks works
   in every table today, and the entity tick's boundary is the place a future spawn command would apply.
2. **Table-level rollback.** `su_host_snapshot_restore_inplace` restores every table (scratch entity tables are
   allocated per table), but there is no "restore table t only" call.
3. **Chunked entity systems (issue 142).** The entity tick still runs one system serially per chunk; the
   per-table cache is ready for the chunked tick, which partitions rows inside one table.
