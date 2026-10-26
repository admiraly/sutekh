/* Sutekh in-process host API (generation 1).
 *
 * A game host links this library (static `su_host`, or the DLL `sutekh_host` with SUTEKH_HOST_DLL defined
 * by the user) and drives one or more worlds from a single thread, without the supervisor process. The
 * out-of-process control plane (`sutekh serve`) stays the agent and development path; both run the same
 * verifier, schedule, VM and world code.
 *
 * Model
 *   A world is a schema (the union of the fields its capsules bind) plus an ordered schedule of verified IR
 *   systems (1..8 capsules, schedule order = array order, effect analysis as in world.create). Its state is
 *   the committed root: fixed-capacity 32-bit columns, the generational entity table, the event log and the
 *   tick counter. su_host_step runs whole ticks: a tick either publishes or leaves the committed root as it
 *   was.
 *
 *   Inputs are per-tick records of cell writes (entity handle, field id, 32-bit value). A record pushed for
 *   tick T is applied to the committed root immediately before the tick that advances the world from T to
 *   T + 1, then that tick runs. Records are canonical: writes are sorted by (slot, field id) and unique, so
 *   the same inputs give the same bytes (su_host_input_encode) and the same state on every run.
 *
 *   Snapshots are binary and canonical (format "SUHSNAP", see su_host_snapshot_write): the same state
 *   always gives the same bytes. The state digest is the SHA-256 of those bytes. Queued inputs are not
 *   state and are not in snapshots.
 *
 * Threading: a world must not be used from two threads at once. Distinct worlds are independent.
 * Memory: the library allocates with its own allocator; release what it returns only with its functions.
 * Errors: every call returns SU_OK or an SU_E_* code (sutekh/status.h). Calls that take a SuHostError fill
 *   it on failure (code, a related index or field id or tick, and a plain-language message); it may be NULL.
 *   A failed call leaves the world as it was unless its comment says otherwise. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef SUTEKH_HOST_H
#define SUTEKH_HOST_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sutekh/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(SUTEKH_HOST_DLL)
#  if defined(SUTEKH_HOST_BUILD)
#    define SU_HOST_API __declspec(dllexport)
#  else
#    define SU_HOST_API __declspec(dllimport)
#  endif
#elif defined(SUTEKH_HOST_DLL) && (defined(__GNUC__) || defined(__clang__))
#  define SU_HOST_API __attribute__((visibility("default")))
#else
#  define SU_HOST_API
#endif

#define SU_HOST_API_VERSION 1u

#define SU_HOST_MAX_CAPSULES 8u          /* systems per world */
#define SU_HOST_TICK_RATE_MAX 10000u     /* ticks per second */
#define SU_HOST_INPUT_MAX_WRITES 4096u   /* cell writes in one tick's record */
#define SU_HOST_INPUT_WINDOW 1024u       /* records may be queued this many ticks ahead of the world */
#define SU_HOST_ENTITY_NONE UINT64_C(0)

typedef struct SuHostError {
    uint32_t code;      /* SU_OK or SU_E_* */
    uint32_t index;     /* capsule index, write index or field id the message is about; UINT32_MAX if none */
    uint64_t tick;      /* tick the failure concerns (step and input calls); UINT64_MAX if none */
    char message[256];  /* NUL-terminated, plain words */
} SuHostError;

typedef struct SuHostWorld SuHostWorld;

/* One capsule: the exact bytes of the capsule JSON and of its .lir.json (as for su_lir_verify).
 * Host operators (additive, see "host operators" below): lir_json NULL and lir_len 0 make capsule_json a host
 * operator record instead, for an operator compiled into this library or registered with it. */
typedef struct SuHostCapsule {
    const uint8_t *capsule_json;
    size_t capsule_len;
    const uint8_t *lir_json;
    size_t lir_len;
} SuHostCapsule;

typedef struct SuHostWorldDesc {
    uint32_t struct_size;            /* sizeof(SuHostWorldDesc) */
    uint32_t capsule_count;          /* 1..SU_HOST_MAX_CAPSULES */
    const SuHostCapsule *capsules;   /* schedule order */
    /* Optional initial state in the world fixture format of world.create ({"schema_id", "tick": 0,
     * "entity_ids": [...], "columns": {name: [u32...]}}, every field of the schema present). NULL starts
     * an empty world: no live entity, all columns zero. */
    const uint8_t *fixture_json;
    size_t fixture_len;
    uint64_t max_entities;           /* capacity; 0 = the fixture's row count (an empty world needs >= 1) */
    uint32_t tick_rate_hz;           /* fixed simulation rate, 1..SU_HOST_TICK_RATE_MAX (recorded, see clock) */
    uint32_t reserved0;              /* 0 */
    uint64_t max_struct_per_tick;    /* entity runtime caps; 0 = engine defaults */
    uint64_t max_events_per_tick;
    uint64_t event_log_max;
    uint64_t seed;                   /* recorded in replays; no engine operation consumes it */
} SuHostWorldDesc;

/* Creates a world. Capsules are verified (any SU_E_* of su_lir_verify, err->index = capsule index), joined
 * into one schedule (SU_E_SCHEMA_MISMATCH when they disagree on schema or a field) and checked by the effect
 * analysis (SU_E_SCHEMA_MISMATCH or SU_E_INVALID_ARGUMENT for conflicts, cycles and edges against the
 * capsule order). On failure *out is NULL. */
SU_HOST_API uint32_t su_host_world_create(const SuHostWorldDesc *desc, SuHostWorld **out, SuHostError *err);
/* NULL-safe. */
SU_HOST_API void su_host_world_destroy(SuHostWorld *w);

/* ---- identity and schema ---- */

SU_HOST_API uint32_t su_host_api_version(void);
SU_HOST_API const char *su_host_status_name(uint32_t code);
SU_HOST_API const char *su_host_schema_id(const SuHostWorld *w);
SU_HOST_API uint64_t su_host_tick(const SuHostWorld *w);      /* committed tick (ticks published) */
SU_HOST_API uint64_t su_host_capacity(const SuHostWorld *w);  /* rows per column */
SU_HOST_API uint32_t su_host_tick_rate(const SuHostWorld *w);
SU_HOST_API uint64_t su_host_seed(const SuHostWorld *w);
/* Changes the recorded rate (1..SU_HOST_TICK_RATE_MAX). Simulation results do not depend on it. */
SU_HOST_API uint32_t su_host_set_tick_rate(SuHostWorld *w, uint32_t hz);

typedef struct SuHostFieldInfo {
    uint32_t id;
    uint32_t type;     /* SU_TYPE_* of sutekh/ir.h; every column holds one 32-bit cell per row */
    uint32_t access;   /* SU_ACCESS_* union over the schedule */
    const char *name;  /* owned by the world */
} SuHostFieldInfo;

SU_HOST_API uint32_t su_host_field_count(const SuHostWorld *w);
/* index in 0..field_count-1, ascending field id. */
SU_HOST_API uint32_t su_host_field_info(const SuHostWorld *w, uint32_t index, SuHostFieldInfo *out);
SU_HOST_API uint32_t su_host_field_id(const SuHostWorld *w, const char *name, uint32_t *id);

/* System identity in schedule order: system id and the lowercase hex SHA-256 semantic hash. */
SU_HOST_API uint32_t su_host_system_count(const SuHostWorld *w);
SU_HOST_API uint32_t su_host_system_info(const SuHostWorld *w, uint32_t index, const char **system_id,
                                         const char **semantic_hash_hex);
/* SHA-256 identifying what a snapshot or replay of this world must match: the schema descriptor digest and
 * the systems' semantic hashes in schedule order. */
SU_HOST_API uint32_t su_host_world_identity(const SuHostWorld *w, uint8_t out[32]);

/* ---- entities (between ticks) ---- */

typedef struct SuHostValue {
    uint32_t field_id;
    uint32_t value;
} SuHostValue;

/* Spawns one entity into the lowest free slot; fields not named get their default (0). Appends a spawn
 * record to the event log, as world.spawn does. *entity receives the handle (slot | generation << 32). */
SU_HOST_API uint32_t su_host_spawn(SuHostWorld *w, const SuHostValue *values, uint32_t count, uint32_t external_id,
                                   uint64_t *entity, SuHostError *err);
/* SU_E_STALE_HANDLE for a stale, dead or retired handle. Appends a despawn record. */
SU_HOST_API uint32_t su_host_despawn(SuHostWorld *w, uint64_t entity, SuHostError *err);
/* Handle of the entity alive in slot, or SU_E_STALE_HANDLE when the slot is not alive. */
SU_HOST_API uint32_t su_host_entity_at(const SuHostWorld *w, uint32_t slot, uint64_t *entity);
/* alive[i] = 1 when slot base + i is alive (committed root). */
SU_HOST_API uint32_t su_host_alive(const SuHostWorld *w, uint32_t base, uint32_t count, uint8_t *alive);

/* ---- per-tick inputs ---- */

typedef struct SuHostInput {
    uint64_t entity;    /* handle; must be alive when the record is applied */
    uint32_t field_id;  /* any field of the schema */
    uint32_t value;     /* the 32-bit cell */
} SuHostInput;

/* Queues writes for tick (the world's committed tick or later, at most SU_HOST_INPUT_WINDOW ahead). Several
 * pushes for one tick merge into one record; a (slot, field) written twice for the same tick is
 * SU_E_INVALID_ARGUMENT and nothing of that push is queued. Unknown fields are SU_E_SCHEMA_MISMATCH. */
SU_HOST_API uint32_t su_host_input_push(SuHostWorld *w, uint64_t tick, const SuHostInput *writes, uint32_t count,
                                        SuHostError *err);
/* Drops queued records for ticks >= from_tick (prediction correction). */
SU_HOST_API void su_host_input_clear(SuHostWorld *w, uint64_t from_tick);
/* Canonical record of tick (empty when nothing is queued): "SUINPUT" 0x00 | u32 version 1 | u64 tick |
 * u32 count | count x (u64 entity | u32 field_id | u32 value), little-endian, writes sorted by slot then
 * field id. Size query with buf NULL. */
SU_HOST_API uint32_t su_host_input_encode(const SuHostWorld *w, uint64_t tick, uint8_t *buf, size_t cap,
                                          size_t *written);
/* Queues a canonical record (as encoded above) for its tick, with the checks of su_host_input_push. Bytes
 * that are not canonical (unsorted, duplicate, wrong size) are SU_E_PARSE. */
SU_HOST_API uint32_t su_host_input_push_encoded(SuHostWorld *w, const uint8_t *buf, size_t len, SuHostError *err);

/* ---- stepping ---- */

/* Runs up to ticks whole ticks (each: apply that tick's record, run every system, publish). *done counts
 * published ticks. A failing tick (a system error, or an input naming a dead entity: SU_E_STALE_HANDLE)
 * leaves the committed root of that tick unchanged, input writes included, keeps its record queued, and
 * stops the call; err->tick and err->index (failing system, UINT32_MAX for inputs) say where. */
SU_HOST_API uint32_t su_host_step(SuHostWorld *w, uint64_t ticks, uint64_t *done, SuHostError *err);

/* ---- reading state (committed root) ---- */

/* Zero-copy column: *data points at capacity cells, valid until the next call that changes the world
 * (step, input application, spawn, despawn, restore) or destroys it. */
SU_HOST_API uint32_t su_host_column(const SuHostWorld *w, uint32_t field_id, const uint32_t **data, uint64_t *rows);
/* Copies count cells starting at row first. */
SU_HOST_API uint32_t su_host_read(const SuHostWorld *w, uint32_t field_id, uint64_t first, uint64_t count,
                                  uint32_t *out);

/* ---- snapshots and digest ---- */

/* Canonical snapshot: "SUHSNAP" 0x00 | u32 version 1 | world identity (32 bytes) | u64 tick |
 * u64 state length | world canonical state (SUWSTATE v2) | u64 log length | event log (SUEL). Size query
 * with buf NULL; SU_E_RESOURCE_LIMIT when cap is too small (*written = size needed).
 * A world with operator state (see "operator state" below; 2026-10-26) writes version 2 instead: the same parts,
 * then u64 operator state length | operator state section (SUOPSTAT). A world without operator state writes
 * exactly the version 1 bytes above, and each kind of world accepts only its own version. */
SU_HOST_API uint32_t su_host_snapshot_size(const SuHostWorld *w, size_t *size);
SU_HOST_API uint32_t su_host_snapshot_write(const SuHostWorld *w, uint8_t *buf, size_t cap, size_t *written);
/* Replaces the world's state with the snapshot's (same world identity, else SU_E_SCHEMA_MISMATCH). Strict:
 * any malformed byte is SU_E_PARSE or SU_E_TYPE and nothing changes. Queued inputs are kept.
 * Rollback note: queued (pushed, not yet applied) input records are not world state. They are not in snapshots,
 * digests or replays' keyframes, and both restores leave them queued as they are. Records queued for ticks that the
 * restore puts back in the future apply again when those ticks are stepped. A rollback user must therefore clear
 * (su_host_input_clear) or re-push exactly the records it wants re-simulated, for example the corrected predictions
 * and confirmed remote inputs. The replay functions drop queued records for the ticks they replay. */
SU_HOST_API uint32_t su_host_snapshot_restore(SuHostWorld *w, const uint8_t *buf, size_t len, SuHostError *err);
/* SHA-256 of the canonical snapshot bytes. */
SU_HOST_API uint32_t su_host_digest(const SuHostWorld *w, uint8_t out[32]);
/* Lowercase hex helper (out holds 65 bytes). */
SU_HOST_API void su_host_hex(const uint8_t digest[32], char out[65]);

/* ---- events (additive, API version 1) ----
 *
 * The world's event log is part of its state (it is in snapshots and the digest). Systems append events
 * with emit_event; the boundary of a tick appends one record per structural command (despawn applied or
 * refused); su_host_spawn and su_host_despawn append a record each. Every event has a sequence number
 * (event_seq) that increases by one per event for the life of the world, so (event_seq) orders events
 * exactly as the engine committed them: by tick, then system, then slot, then per-row emission order, with
 * host records where the host made them. The log is a ring of SuHostWorldDesc.event_log_max events: older
 * events are evicted, and readers are told how many they missed. Reading never changes the world.
 *
 * tick of an event: a system event or command record carries the tick that step published (a step from
 * tick T to T + 1 emits events with tick T + 1); a host spawn or despawn record carries the committed tick
 * at the moment of the call. Rollback: restoring a snapshot restores its log, so su_host_event_next_seq
 * goes back too, and re-simulating the same inputs re-emits the same events with the same sequence numbers.
 * A cursor beyond the restored log is SU_E_INVALID_ARGUMENT; clamp it to su_host_event_next_seq. */

/* Same layout as the engine's event record (40 bytes). kind is the IR constant, or an engine kind below. */
typedef struct SuHostEvent {
    uint64_t event_seq;
    uint64_t tick;
    uint32_t kind;
    uint32_t system_index;  /* schedule position of the emitting system; SU_HOST_EVENT_SYSTEM_HOST for host records */
    uint32_t slot;          /* emitting row */
    uint32_t seq;           /* emission order within the row */
    uint32_t a;
    uint32_t b;
} SuHostEvent;

#define SU_HOST_EVENT_SYSTEM_HOST UINT32_C(0xFFFFFFFF)
#define SU_HOST_EVENT_REJECTED    UINT32_C(0xFFFFFFFF) /* structural command refused (a = command kind, b = status) */
#define SU_HOST_EVENT_SPAWNED     UINT32_C(0xFFFFFFFE) /* host spawn (a = slot, b = generation) */
#define SU_HOST_EVENT_DESPAWNED   UINT32_C(0xFFFFFFFD) /* despawn applied (a = slot, b = old generation) */

/* Sequence number the next event will receive (= the cursor that is fully caught up). */
SU_HOST_API uint64_t su_host_event_next_seq(const SuHostWorld *w);
/* Copies up to max events with event_seq >= cursor, oldest first, into out. *count receives the number
 * copied, *next_cursor the cursor to pass next time, and *skipped (optional) how many events after cursor
 * were already evicted from the ring (reading then starts at the oldest retained event). */
SU_HOST_API uint32_t su_host_events_read(const SuHostWorld *w, uint64_t cursor, SuHostEvent *out, uint32_t max,
                                         uint32_t *count, uint64_t *next_cursor, uint64_t *skipped);
/* Copies up to max retained events whose tick field equals tick, in event_seq order. *total (optional)
 * receives how many retained events match (more than max means the copy was truncated); *complete
 * (optional) is 1 when no event of that tick can have been evicted (nothing was ever evicted, or the
 * oldest retained event is from an earlier tick), else 0. */
SU_HOST_API uint32_t su_host_events_for_tick(const SuHostWorld *w, uint64_t tick, SuHostEvent *out, uint32_t max,
                                             uint32_t *count, uint32_t *total, uint32_t *complete);

/* ---- rollback-grade state operations (additive, API version 1) ---- */

/* su_host_snapshot_restore without reallocation: the snapshot is validated completely and then written into
 * the world's existing columns, entity table and event log. It accepts exactly the snapshots
 * su_host_snapshot_restore accepts, with the same codes, leaves the world unchanged on failure, and keeps
 * queued inputs. The first call on a world allocates a scratch entity table and event log once (sized by
 * the capacity and event_log_max; SU_E_RESOURCE_LIMIT if that fails); later calls allocate nothing. */
SU_HOST_API uint32_t su_host_snapshot_restore_inplace(SuHostWorld *w, const uint8_t *buf, size_t len,
                                                      SuHostError *err);

/* FAST, NON-CANONICAL digest. This is not su_host_digest and the two are never interchangeable: it is the
 * 64-bit XXH64 (seed 0) of the same canonical snapshot bytes that su_host_digest hashes with SHA-256.
 * Identical states give identical values and different values prove different states, but it is not
 * collision resistant: use it for per-tick desync checks between cooperating peers or for rollback
 * self-checks, and use su_host_digest for replays, files and anything that crosses a trust boundary.
 * The value is defined by SU_HOST_FAST_DIGEST_VERSION. */
#define SU_HOST_FAST_DIGEST_VERSION 1u
SU_HOST_API uint32_t su_host_digest_fast(const SuHostWorld *w, uint64_t *out);

/* ---- IR 0.3 typed lanes (numeric profile fixed_q32_v1; LIVE_IR_SPEC "IR 0.3") ----
 * Every column is a 32-bit cell. An i64 or q32 field declared with id I occupies the lanes I ("<name>.lo",
 * bits 0..31) and I + 1 ("<name>.hi", bits 32..63); su_host_field_info reports both with type SU_TYPE_U32
 * (storage). These helpers read and write such a field as one 64-bit value (q32: raw Q32.32, value * 2^32). */

/* *value_type: SU_TYPE_U32, SU_TYPE_I32, SU_TYPE_I64 or SU_TYPE_Q32; *lane: 0, or 1 for the high lane of a
 * 64-bit field. SU_E_SCHEMA_MISMATCH for an unknown field. */
SU_HOST_API uint32_t su_host_field_value_type(const SuHostWorld *w, uint32_t field_id, uint32_t *value_type,
                                              uint32_t *lane);
/* As su_host_read for a 64-bit field named by its low lane id. SU_E_TYPE when field_id is not the low lane of an
 * i64 or q32 field, SU_E_SCHEMA_MISMATCH when it is unknown. */
SU_HOST_API uint32_t su_host_read_i64(const SuHostWorld *w, uint32_t field_id, uint64_t first, uint64_t count,
                                      int64_t *out);
/* Queues both lanes of a 64-bit field for tick, as one su_host_input_push of two cell writes (same checks,
 * same record). SU_E_TYPE / SU_E_SCHEMA_MISMATCH as su_host_read_i64. */
SU_HOST_API uint32_t su_host_input_push_i64(SuHostWorld *w, uint64_t tick, uint64_t entity, uint32_t field_id,
                                            int64_t value, SuHostError *err);

/* ---- host operators (additive, API version 1; 2026-10-18) ----
 *
 * A host operator is a system compiled into the engine (for example space3d.query: swept collisions and gate
 * crossings, docs/SPACE3D.md) that reads and writes only the fields its record declares. A world uses one by
 * listing its host record among the capsules: SuHostCapsule.capsule_json holds the exact bytes of the record,
 * lir_json is NULL and lir_len is 0. The record is checked as world.create checks it (su_host_load_bound):
 * operator name, version and constants hash equal the built-in descriptor's, and its field table is either the
 * operator's own schema exactly, or a schema-relative binding of every operator lane to a lane of the game's
 * schema ("binds"; the game's IDs, names and schema ID; see docs/SPACE3D.md section 2). Failures:
 * SU_E_CAPABILITY_DENIED when no operator of the record's system_id is built into this library or registered
 * with it (su_host_register_operator, below; err->index = capsule index), else the loader's code (SU_E_SCHEMA_MISMATCH, SU_E_TYPE, SU_E_INVALID_ARGUMENT, SU_E_PARSE)
 * with its reason in err->message. The operator then runs in schedule order like any system.
 *
 * State: an operator's results are ordinary columns of the world (its declared writes), stored for every row
 * every tick, so they are in snapshots, su_host_digest, su_host_digest_fast, in-place restore and replays
 * exactly like IR results, and re-simulation after a restore reproduces them bit for bit. Events derived from
 * them are emitted by IR systems that read them. The record's semantic hash (covering the binding) is part of
 * the world identity. Each world owns its operator's scratch (allocated at creation for the world's capacity,
 * never in a tick), so distinct worlds stay independent. */

/* Operators of this library, by index (0..count-1): system id (for example "space3d.query") and operator name (for
 * example "space3d_ops_v1"). The operators built into the library come first; operators registered with
 * su_host_register_operator (see "registered host operators") follow in registration order, so an index never
 * changes once assigned and count only grows. */
SU_HOST_API uint32_t su_host_operator_count(void);
SU_HOST_API uint32_t su_host_operator_info(uint32_t index, const char **system_id, const char **operator_name);
/* Operator data (additive, API version 1; 2026-10-20): immutable bytes an operator reads at run time, for example
 * the static mesh set of the space3d operators (docs/SPACE3D.md, "Static meshes"; blob format in
 * profiles/space3d/native/space3d_mesh.h), with the compound shapes of space3d.query_compound (S3MESH02,
 * "Compound movers"). Every host entry named system_id (NULL: every host entry whose operator
 * takes data; they must be of one family) parses the bytes (one parse, shared) and keeps them for the world's
 * life. Only before the world's first su_host_step, once per entry; the caller may free data on return. The data
 * is not world state (not in snapshots or digests) but it is part of the world identity: the identity becomes
 * SHA-256("SUOPDATA\0" || previous identity || u32 entry count || per entry: u32 schedule index ||
 * SHA-256(data)), so snapshots and replays only load into a world given the same data. Failures:
 * SU_E_INVALID_ARGUMENT (NULL/empty data, the world has stepped, no matching entry takes data, an entry already has
 * data), else the operator's parse status (SU_E_PARSE, SU_E_RESOURCE_LIMIT) with its reason in err->message. */
SU_HOST_API uint32_t su_host_set_operator_data(SuHostWorld *w, const char *system_id, const uint8_t *data,
                                               size_t len, SuHostError *err);

/* ---- registered host operators (additive, API version 1; 2026-10-26) ----
 *
 * A game can add host operators of its own (native cross-row systems that IR cannot express) without building
 * them into this library: it fills an SuHostOperatorDesc and calls su_host_register_operator before it creates a
 * world that lists the operator's record. From then on the operator is an operator of this library for that
 * process: su_host_world_create checks its record exactly as it checks an engine operator's (host.operator equals
 * desc->operator_name, operator_version and constants_hash equal the descriptor's, numeric_profile is
 * fixed_q32_v1 when any lane is i32/i64/q32 else u32_mod_v1, and host.fields is the descriptor's field table
 * exactly or a schema-relative binding of it, docs/SPACE3D.md section 2; refusals SU_E_SCHEMA_MISMATCH or SU_E_TYPE
 * with the reason), the schedule's union table checks every IR system that binds one of its lanes (ID, name,
 * storage and value type), the effect analysis uses its declared access, and it runs in schedule order. A record
 * whose system_id is neither built in nor registered is still SU_E_CAPABILITY_DENIED. Nothing changes for a
 * process that registers nothing: records, identities, snapshots and digests are those of earlier releases (only
 * the text of that refusal now says "built into or registered with this library").
 *
 * Contract of run (the built-in operators' contract; what the engine guarantees or checks is said so):
 *   - run sees only the lanes its descriptor declares (run->columns, descriptor order, under the descriptor's IDs
 *     even when the record binds them to other game IDs). It receives no world pointer and no other column.
 *   - Reads are frozen: `in` is the working value at entry (the committed tick plus the writes of systems earlier
 *     in the schedule) and must not be written. After run the engine re-hashes every column the operator may read
 *     (XXH64 before and after the call) and fails the tick with SU_E_UNDECLARED_WRITE, naming the lane in
 *     err->message, if one changed (unless SU_HOST_OPERATOR_TRUST_INPUTS). A change that keeps the 64-bit hash
 *     (probability about 2^-64) is missed. This covers writes through the views run is given; in-process native
 *     code can still write anywhere in the process (ENGINE_CONSTITUTION C07: native code in a process is trusted
 *     by that process).
 *     Cost (observations, ENGINE_CONSTITUTION C10; i7-14700K, one core, Release, 2026-10-26; not a budget): with a
 *     world stepping, about 0.55-0.62 ms per tick at 100,000 rows x 10 read lanes and about 34 us at 10,000 x 10
 *     (independent review measurement); the two hash passes alone, isolated, about 0.37 ms and 37 us (median).
 *     Method and samples: planning/handoffs/2026-10-26-registered-operators.md. SU_HOST_OPERATOR_TRUST_INPUTS
 *     removes the cost and the protection.
 *   - Write lanes are pre-filled before run: a read-write lane with its `in` values, a write-only lane with zeros
 *     (so a write-only lane leaks no undeclared read). Every row of every declared write is therefore defined and
 *     committed, also the rows run does not store; dead rows are zeroed by the world at publish.
 *   - run returns SU_OK, or any SU_E_* code: the tick then fails in this system (err->index, err->tick; the
 *     message names the code and, if run wrote one, its reason, see SuHostOperatorRun.reason), nothing of the
 *     tick is committed, and its input record stays queued. SuHostError.message holds 255 bytes, shared with the
 *     tick, the system ID and the code name, so a long reason (or a long system ID) is cut at the end; keep reasons
 *     short (well under 100 bytes) when they matter.
 *   - Determinism: the results must be a function of the columns, alive, generation, tick, operator data and the
 *     operator state (run->state) alone (no clock, no thread timing, nothing else kept across calls), so that a
 *     restore and re-simulation reproduces them. The engine zero-fills the scratch before every call (always; scratch is not world state and is not in
 *     snapshots, so nothing may survive in it from one tick to the next).
 *   - No allocation in the tick (ENGINE_CONSTITUTION C11): the scratch (scratch_size(capacity) bytes), the
 *     column table and the alive/generation views are allocated once at world creation. run must not allocate.
 *   - run is called serially, once per tick, from the thread that steps the world, with the whole row range
 *     (row_begin 0, row_end rows, chunk 0 of 1). Distinct worlds have distinct scratch and may step on different
 *     threads at once, so run and parse_data must be reentrant across worlds (user is shared). Use only
 *     run->scratch for temporary memory (never a global): it is private to the call.
 *
 * State and identity: the operator's results are ordinary columns (in snapshots, su_host_digest, the fast digest,
 * in-place restore and replays). The system's semantic hash (su_host_system_info) covers system ID, schema,
 * operator_name, operator_version, constants_hash, the field table, any binding, the descriptor flags and the
 * scratch size scratch_size returned for the world's capacity; it is part of su_host_world_identity, so snapshots
 * and replays of the world only load into a world whose process registered the same operator the same way.
 * Names are identifiers (below), so the hashed text is unambiguous. constants_hash identifies the operator's code
 * and constants: change it (or operator_version) whenever results can change. Operator data works as for engine
 * operators (su_host_set_operator_data; the data's hash joins the identity).
 *
 * Registry: process-wide (one per copy of the library: the static library su_host and the DLL sutekh_host each
 * have their own, so register with the copy that creates the worlds), append-only, at most
 * SU_HOST_MAX_REGISTERED_OPERATORS entries for the life of the process; there is no unregister. Registration and
 * lookups are serialized by a lock, so registering on one thread while worlds are created on others is safe; a
 * world keeps using the entry it was created with. Registration copies the descriptor (strings and field table);
 * the callbacks and user must stay valid for the life of the process. A registered operator is in-process only:
 * the out-of-process control plane (`sutekh serve`, the worker, world.create) does not see it and refuses its
 * record with SU_E_CAPABILITY_DENIED as before. A game that needs it in a worker must build it into a worker of its
 * own (not provided).
 *
 * Versioning: SuHostOperatorDesc.struct_size may be any size from SU_HOST_OPERATOR_DESC_V1_SIZE up; bytes beyond
 * the library's own sizeof(SuHostOperatorDesc) must be zero (a nonzero unknown byte is refused, so a request this
 * library cannot honour fails instead of being ignored), and only struct_size bytes are read. The check is
 * byte-based, so callers must zero the whole descriptor (memset, or SU_HOST_OPERATOR_DESC_INIT) before filling it,
 * padding included; the same holds for any later versioned struct of this section. Fields added later are laid out
 * without padding (each at an offset aligned for its type, the struct size a multiple of 8). The library fills
 * SuHostOperatorRun.struct_size; an operator reads only the fields that size covers. Fields are only ever appended.
 * SuHostOperatorColumn stays as it is (no stride, no growth): per-lane additions arrive as new trailing
 * SuHostOperatorRun fields (parallel arrays indexed like columns). Unknown flag bits are refused.
 *
 * Extension plan for a deterministic multithreaded tick (issue 142; not implemented): everything below is opt-in by
 * a new descriptor flag (and new trailing descriptor fields), and every operator without it keeps exactly the serial
 * whole-range call described above.
 *   - Chunked runs: run called once per canonical row chunk ([row_begin, row_end), chunk_index of chunk_count; the
 *     chunking depends only on the row count, as for chunked IR systems), storing only its rows' writes. Reads stay
 *     frozen for the whole tick.
 *   - Per-worker scratch: a new trailing callback scratch_size_ex(capacity, workers, chunk_rows, ...) sizes one
 *     scratch per worker; run->scratch is that worker's. scratch_size stays the serial path.
 *   - Writes to other rows (scatter: for example damage applied to target rows): each chunk writes into its own
 *     output buffer (declared lanes, per-chunk, engine-allocated at creation), then a declared merge/reduce
 *     callback combines the buffers in canonical chunk order (0, 1, ..., chunk_count - 1) into the staging columns,
 *     so the result does not depend on thread timing. The merge is part of the identity like run. */

#define SU_HOST_MAX_REGISTERED_OPERATORS 64u
#define SU_HOST_OPERATOR_TRUST_INPUTS UINT32_C(1) /* skip the post-run input re-hash: saves its cost (above), but an
                                                   * operator that writes a read lane then corrupts the world
                                                   * undetected. Part of the identity. */
/* Every other bit is reserved and refused (SU_E_INVALID_ARGUMENT) by this version. Bit 2 was never released. */

/* One declared lane. type: SU_TYPE_U32, SU_TYPE_I32, SU_TYPE_I64 or SU_TYPE_Q32 (sutekh/ir.h); every lane is one
 * 32-bit cell, and an i64/q32 field is two adjacent entries id I ("<name>.lo") and I + 1 ("<name>.hi") with the
 * same type and access. access: SU_ACCESS_READ, SU_ACCESS_WRITE or both (sutekh/abi.h). */
typedef struct SuHostOperatorField {
    uint32_t id;       /* nonzero, strictly ascending across the table */
    uint32_t type;
    uint32_t access;
    uint32_t reserved; /* 0 */
    const char *name;  /* identifier: 1..255 bytes of [A-Za-z0-9_.-], unique */
} SuHostOperatorField;

/* An operator's state (see "operator state" below): a handle passed in SuHostOperatorRun.state, or a read-only view
 * of the committed state (su_host_operator_state). */
typedef struct SuHostState SuHostState;

/* A declared lane as run sees it. Fixed layout (see "Versioning"). */
typedef struct SuHostOperatorColumn {
    uint32_t field_id;   /* the descriptor's ID */
    uint32_t access;     /* as declared */
    const uint32_t *in;  /* rows cells, frozen working values; NULL for a write-only lane */
    uint32_t *out;       /* rows cells, pre-filled (see the contract); NULL for a read-only lane */
} SuHostOperatorColumn;

typedef struct SuHostOperatorRun {
    uint32_t struct_size;                 /* sizeof(SuHostOperatorRun) of the library */
    uint32_t column_count;                /* the descriptor's field_count */
    const SuHostOperatorColumn *columns;  /* descriptor order */
    uint64_t rows;                        /* the world's capacity (every column has rows cells) */
    uint64_t row_begin;                   /* rows whose writes this call stores: always 0 .. rows in this version */
    uint64_t row_end;
    uint32_t chunk_index;                 /* always 0 of 1 in this version (see the extension plan) */
    uint32_t chunk_count;
    uint64_t tick;                        /* committed tick this step advances from (T for the step T -> T + 1) */
    const uint8_t *alive;                 /* rows entries: 1 alive, 0 dead (working entity table) */
    const uint32_t *generation;           /* rows entries: slot generations (handle = slot | generation << 32) */
    void *scratch;                        /* scratch_size(capacity) bytes (malloc alignment), zero at entry, or NULL
                                           * when 0; private to this call */
    size_t scratch_bytes;
    const void *data;                     /* parse_data's result for this world, or NULL when it has none */
    char *reason;                         /* reason_cap bytes, "" at entry: on failure run may write a NUL-terminated
                                           * reason, appended to the step's err->message */
    size_t reason_cap;
    /* Added 2026-10-26 (issue 139; present when struct_size covers it): the operator's state, mutable during this
     * call (see "operator state"), or NULL when the descriptor declares none. */
    SuHostState *state;
} SuHostOperatorRun;

typedef struct SuHostOperatorDesc {
    uint32_t struct_size;                 /* >= SU_HOST_OPERATOR_DESC_V1_SIZE; normally sizeof(SuHostOperatorDesc) */
    uint32_t flags;                       /* SU_HOST_OPERATOR_* above, or 0 */
    const char *system_id;                /* identifier, 1..127 bytes, unique ignoring letter case; the prefixes
                                           * "rts.", "space3d." and "sutekh." (any letter case) are reserved for
                                           * engine operators */
    const char *schema_id;                /* identifier: the operator's own schema ID, 1..255 bytes (records may
                                           * bind it to another schema) */
    const char *operator_name;            /* identifier: host.operator of the record, 1..255 bytes; names beginning
                                           * with "rts_ops", "space3d_ops" or "sutekh" (any case, '-' counted as
                                           * '_') are reserved */
    uint32_t operator_version;            /* >= 1 */
    uint32_t field_count;                 /* 1..1024 */
    const SuHostOperatorField *fields;
    uint8_t constants_hash[32];           /* SHA-256 identifying the operator's semantics (code and constants) */
    /* Scratch for a world of capacity rows: *bytes (0 = none). Called once per world at creation; the size is part
     * of the system's semantic hash. A non-SU_OK return (for example SU_E_RESOURCE_LIMIT for a capacity the
     * operator does not support) refuses the world. NULL: no scratch. */
    uint32_t (*scratch_size)(uint64_t capacity, size_t *bytes, void *user);
    /* The tick body; see the contract above. Required. */
    uint32_t (*run)(const SuHostOperatorRun *run, void *user);
    /* Operator data (su_host_set_operator_data). parse_data validates and converts the bytes (borrowed for the
     * call) into *parsed, which run receives as run->data; on failure it returns SU_E_PARSE or
     * SU_E_RESOURCE_LIMIT (any non-SU_OK code is passed through) and may write a reason into why. One parse is
     * shared by the world's entries of one call; release_data (may be NULL) frees it when the last of them is
     * destroyed. NULL parse_data: the operator takes no data. */
    uint32_t (*parse_data)(const uint8_t *data, size_t len, void **parsed, char *why, size_t why_cap, void *user);
    void (*release_data)(void *parsed, void *user);
    void *user;                           /* passed to every callback */
    /* Version 2 (2026-10-26, issue 139): operator state (see "operator state"). All four zero: no state, and the
     * operator, its identity and the world's snapshots are exactly those of version 1. state_max_entries and
     * state_max_bytes are both 0 or both nonzero. */
    uint32_t state_flags;                 /* SU_HOST_STATE_* below, or 0 */
    uint32_t state_max_entries;           /* 1..SU_HOST_STATE_MAX_ENTRIES: most entries the state can hold */
    uint32_t state_max_bytes;             /* 1..SU_HOST_STATE_MAX_BYTES: most value bytes (sum of lengths) */
    uint32_t state_reserved;              /* 0 */
} SuHostOperatorDesc;

/* Size of the first released SuHostOperatorDesc (through user): 120 bytes with 8-byte pointers (LP64, LLP64), 84
 * with 4-byte pointers (ILP32). The library pins these values at compile time. */
#define SU_HOST_OPERATOR_DESC_V1_SIZE (offsetof(SuHostOperatorDesc, user) + sizeof(void *))
/* Version 2 (through state_reserved): 136 bytes with 8-byte pointers, 100 with 4-byte pointers. */
#define SU_HOST_OPERATOR_DESC_V2_SIZE (offsetof(SuHostOperatorDesc, state_reserved) + sizeof(uint32_t))

/* Zeroes *d (padding included) and sets struct_size: use it before filling a descriptor. */
#define SU_HOST_OPERATOR_DESC_INIT(d) \
    (memset((d), 0, sizeof(SuHostOperatorDesc)), (d)->struct_size = (uint32_t)sizeof(SuHostOperatorDesc))

/* Identifiers: 1..max bytes of [A-Za-z0-9_.-] (ASCII; no quote, space, control character or other byte), with at
 * least one letter or digit.
 * Registers an operator for this process. Failures (nothing is registered): SU_E_INVALID_ARGUMENT for a malformed
 * descriptor (struct_size, a nonzero field this library does not know, NULL run/strings, unknown flags, a name
 * that is not an identifier, field table rules above; err->index = the offending field when there is one),
 * SU_E_DUPLICATE_KEY when system_id is already registered (ignoring letter case),
 * names an operator built into this library or uses a
 * reserved prefix, or operator_name is reserved, SU_E_RESOURCE_LIMIT when the registry is full or memory runs
 * out. */
SU_HOST_API uint32_t su_host_register_operator(const SuHostOperatorDesc *desc, SuHostError *err);

/* ---- operator state (additive, API version 1; 2026-10-26, issue 139) ----
 *
 * A registered operator may own authoritative variable-size state that does not fit fixed 32-bit columns (for
 * example sparse per-entity overlays keyed by entity handle). It declares it in its descriptor (state_max_entries,
 * state_max_bytes, state_flags) and the engine owns the bytes: a map from a key (entity, sub) to a value of 1 or
 * more bytes, kept sorted by key (entity, then sub, both unsigned). The operator reads and changes it only through
 * the functions below, on run->state during run.
 *
 * Canonical: the state's bytes are a function of its logical content alone (the set of keys and their value
 * bytes), never of the order or history of the calls that produced it or of the engine's memory layout. Encoding
 * (inside the snapshot's SUOPSTAT section, per system with state): u32 entry count | u64 value bytes | per entry in
 * key order: u64 entity | u64 sub | u32 length | then the values, concatenated in key order. Two runs that reach the
 * same entries give the same bytes, whatever the order of their puts and erases (tested).
 *
 * Where it is: in the canonical snapshot (version 2, see su_host_snapshot_write), so in su_host_digest,
 * su_host_digest_fast, both restores and replay keyframes, exactly like columns. A world without operator state
 * keeps version 1 snapshots, so its snapshots, digests and replays are byte-identical to earlier releases.
 * Section: "SUOPSTAT" 0x00 | u32 version 1 | u32 system count | per system with state, in schedule order:
 * u32 schedule index | the encoding above. Restores are strict: unsorted or duplicate keys, a zero length, lengths
 * that disagree with the totals, more than the declared maximum entries or bytes, a wrong count or index, a
 * truncated or oversized section are SU_E_PARSE, and the world is unchanged.
 *
 * Transactions: changes made during a tick become committed state only when the tick publishes. A failing tick (its
 * run, any later system, or the boundary) leaves the committed state exactly as it was, as for columns.
 *
 * Bounds (ENGINE_CONSTITUTION C11): every buffer is allocated at world creation for the declared maximum: two
 * arenas of 2 x state_max_bytes each (4 x state_max_bytes in all; the slack is what keeps compactions rare, below),
 * two entry tables of state_max_entries (24 bytes each) and 4 bytes per entry for compaction (calloc'd, so untouched
 * pages cost no physical memory on systems with lazy allocation; pages once written stay resident). Nothing is
 * allocated in a tick or by su_host_snapshot_restore_inplace. A change that would exceed either maximum is refused
 * with SU_E_RESOURCE_LIMIT and changes nothing; whether a change fits depends only on the logical state (entries and
 * the sum of value lengths), never on fragmentation (the engine compacts). run may handle the refusal or return it:
 * the tick then fails with SU_E_RESOURCE_LIMIT, nothing is committed, and err->message says the state cap was
 * reached.
 * Costs: lookup O(log n); inserting or erasing a key moves the entry table (O(n) entries of 24 bytes); the first
 * change in a tick copies the entry table (O(n)); value bytes are not copied per tick (a changed value is written to
 * free arena space, copy-on-write; a value already written in this tick and rewritten with the same length is reused
 * in place). When an arena's free space runs out, a compaction copies the live values (at most state_max_bytes) once
 * and leaves at least state_max_bytes free, so compactions copy at most as many bytes as were written since the
 * previous one: amortized O(1) per written byte, also with the live state at the cap. The tick that compacts pays
 * O(live bytes) at once (worst case: state_max_bytes copied in one change).
 * With SU_HOST_STATE_DROP_DEAD the publish scans the entries (O(n)) only when an entity may have died since the last
 * scan: su_host_despawn since then, a despawn applied by the tick (seen in the tick's event records, O(events of the
 * tick)), or a restore. Other ticks pay nothing for it.
 *
 * Keys and entities (ENGINE_CONSTITUTION C05): keys are stable IDs, never addresses. entity is normally an entity
 * handle (slot | generation << 32), which stays valid across snapshot, restore and replay because the entity table
 * is restored with the state; sub is the operator's (for example a brick index). Values are opaque bytes: an
 * operator that stores references in them must store handles or its own stable IDs, never pointers.
 * Despawned entities: with SU_HOST_STATE_DROP_DEAD the engine owns this: put and alloc refuse a key whose entity is
 * not alive in this tick's working entity table (SU_E_STALE_HANDLE), and when a tick publishes, every entry whose
 * entity is no longer alive in the committed entity table (despawned during the tick, or by su_host_despawn before
 * it) is erased, as part of that tick. Restored keys must have a generation in 1..0xFFFFFFFE. Without the flag,
 * entity is an opaque u64 and the operator is responsible (for example su_host_state_erase_entity when it sees a
 * handle die).
 * Either way the outcome is deterministic: the rule depends only on the state and the entity table.
 *
 * Pointers: data pointers returned by the functions below point into engine memory; they are valid until the next
 * change of that state (put, alloc, modify, erase, erase_entity) or the end of the run call (for views: until the
 * next call that changes the world). Only the pointers of su_host_state_alloc and su_host_state_modify may be
 * written. The pointers of su_host_state_get and su_host_state_entry (and everything read through a view) are const
 * for a reason: they may point at committed bytes shared with the last published state, so writing through them
 * changes committed state (and later snapshots, digests and rollbacks) without the engine noticing; it is a contract
 * violation the engine does not detect (ENGINE_CONSTITUTION C07). put's data must not point into the state
 * (SU_E_INVALID_ARGUMENT).
 *
 * Parallel chunked operators (issue 142; plan, not implemented). Lockstep peers may run with different thread counts,
 * so nothing below may depend on the worker count:
 *   - Opt-in by a descriptor flag (part of the identity). An operator without it always gets the serial call
 *     described above (one call, read-your-writes, the guarantees above), on any number of workers. An operator with
 *     it always gets the chunked semantics below, also when the engine runs every chunk on one thread and also when
 *     the world has a single chunk (chunk_count == 1).
 *   - The partition depends only on the world's capacity: fixed-size row chunks (the chunk size is a constant of the
 *     engine version, like SU_CHUNK_SLOTS for IR systems); the worker count only decides which thread runs which chunk.
 *   - Reads: every chunk sees the state as it was when the operator's run began in this tick (frozen; other chunks'
 *     changes are never visible). A chunk sees its own changes through su_host_state_get and su_host_state_modify
 *     (its journal is looked up first); su_host_state_count, su_host_state_lower_bound and su_host_state_entry
 *     enumerate the frozen state only.
 *   - Writes: put/alloc/modify/erase/erase_entity append to the chunk's own journal (modify copies the value into
 *     it). Each chunk's journal has a declared capacity (operations and bytes; new trailing descriptor fields) and
 *     is allocated at world creation for every chunk of the capacity. A change that does not fit in the chunk's
 *     journal is refused with SU_E_RESOURCE_LIMIT, modify included: the guarantee that modify is never refused for
 *     lack of space holds for the serial call only. The refusal depends only on that chunk's own calls, so it is
 *     deterministic. The journal capacities are declared caps and join the semantic hash like state_max_*.
 *   - Merge: after the last chunk the engine applies the journals in canonical chunk order (0 .. chunk_count - 1),
 *     each in call order. The same key changed by two chunks keeps the later chunk's result (or a declared merge
 *     callback combines the values in chunk order; the callback is part of the identity). The caps (state_max_*) are
 *     checked on the merged result; exceeding one fails the tick with SU_E_RESOURCE_LIMIT and nothing is committed.
 *   - Failures: when several chunks fail (their run returned an error, or a journal refusal was returned), the tick
 *     reports the status and reason of the lowest-indexed failing chunk, as chunked IR systems do, never whichever
 *     thread finished first; nothing of the tick is committed.
 *   - An operator keying per-entity data by handle and changing only its own rows' keys never conflicts. Because the
 *     signatures here return no memory that must survive a call, the journal needs no change to them.
 *
 * Identity: a declared state folds state_flags, state_max_entries and state_max_bytes into the system's semantic
 * hash (so into the world identity): snapshots and replays only load into a world with the same declaration.
 * Built-in operators have no state; their identities and snapshots are unchanged. */

#define SU_HOST_STATE_MAX_ENTRIES UINT32_C(16777216)  /* 2^24 entries per operator */
#define SU_HOST_STATE_MAX_BYTES UINT32_C(1073741824)  /* 2^30 value bytes per operator */
#define SU_HOST_STATE_DROP_DEAD UINT32_C(1)           /* entity is an entity handle; dead entities' entries are erased
                                                       * (above). Every other bit is reserved and refused. */

typedef struct SuHostStateEntry {
    uint64_t entity;
    uint64_t sub;
    const void *data;   /* len bytes (see "Pointers") */
    uint32_t len;
    uint32_t reserved;  /* 0 */
} SuHostStateEntry;

typedef struct SuHostStateInfo {
    uint32_t entries;      /* current */
    uint32_t max_entries;  /* declared */
    uint64_t bytes;        /* current sum of value lengths */
    uint64_t max_bytes;    /* declared */
    uint32_t flags;        /* declared state_flags */
    uint32_t writable;     /* 1 inside run (run->state), 0 for a view */
    uint64_t compactions;  /* diagnostic: compactions since the world was created (not state: not in snapshots or
                            * digests, not reset by restores) */
} SuHostStateInfo;

/* Read access, on run->state or a view. SU_E_INVALID_ARGUMENT for NULL arguments or an index out of range. */
SU_HOST_API uint32_t su_host_state_info(const SuHostState *s, SuHostStateInfo *out);
/* Number of entries (0 for NULL). */
SU_HOST_API uint32_t su_host_state_count(const SuHostState *s);
/* Index of the first entry whose key is >= (entity, sub), or su_host_state_count when none: for example all entries
 * of one entity are lower_bound(entity, 0) up to the first index whose entity differs. */
SU_HOST_API uint32_t su_host_state_lower_bound(const SuHostState *s, uint64_t entity, uint64_t sub);
/* Entry at index (0..count-1, key order). */
SU_HOST_API uint32_t su_host_state_entry(const SuHostState *s, uint32_t index, SuHostStateEntry *out);
/* Value of (entity, sub): SU_OK with *data and *len, or SU_OK with *data NULL and *len 0 when there is no entry. */
SU_HOST_API uint32_t su_host_state_get(const SuHostState *s, uint64_t entity, uint64_t sub, const void **data,
                                       uint32_t *len);

/* Changes: only on run->state during run (a view, or a handle used outside its run, is SU_E_INVALID_ARGUMENT).
 * len 1..state_max_bytes. Refusals change nothing: SU_E_RESOURCE_LIMIT at a cap (above), SU_E_STALE_HANDLE with
 * SU_HOST_STATE_DROP_DEAD for an entity that is not alive. */
/* Inserts or replaces (entity, sub) with a copy of data. */
SU_HOST_API uint32_t su_host_state_put(SuHostState *s, uint64_t entity, uint64_t sub, const void *data,
                                       uint32_t len);
/* Inserts or replaces (entity, sub) with len zero bytes and returns them writable in *data. */
SU_HOST_API uint32_t su_host_state_alloc(SuHostState *s, uint64_t entity, uint64_t sub, uint32_t len, void **data);
/* The existing value of (entity, sub), writable in place (copy-on-write: the committed bytes are not touched until
 * the tick publishes); *data NULL and *len 0 when there is no entry. Never refused for lack of space (in this
 * version's serial call; see the chunked plan above for issue 142). */
SU_HOST_API uint32_t su_host_state_modify(SuHostState *s, uint64_t entity, uint64_t sub, void **data,
                                          uint32_t *len);
/* Erases (entity, sub) if present; *erased (optional) receives 1 or 0. */
SU_HOST_API uint32_t su_host_state_erase(SuHostState *s, uint64_t entity, uint64_t sub, uint32_t *erased);
/* Erases every entry of entity; *erased (optional) receives how many. */
SU_HOST_API uint32_t su_host_state_erase_entity(SuHostState *s, uint64_t entity, uint32_t *erased);

/* Read-only view of the committed state of system index (between ticks; for rendering, tools and tests), valid until
 * the world is destroyed; what it shows changes with the world. SU_E_INVALID_ARGUMENT when the system has no state. */
SU_HOST_API uint32_t su_host_operator_state(const SuHostWorld *w, uint32_t index, const SuHostState **state);

/* ---- execution tiers (additive, API version 1; 2026-10-18) ----
 * Every tier gives bit-identical results (states, events, digests): the tiers are differential-tested against
 * the reference interpreter, and a native kernel is admitted only after a seeded differential check against
 * the VM. A tier choice is not world state: it is not in snapshots, digests or the world identity. */
#define SU_HOST_TIER_REFERENCE 1u /* checked reference interpreter */
#define SU_HOST_TIER_PLANNED   2u /* planned VM tier (the default unless SUTEKH_VM_TIER=reference) */
#define SU_HOST_TIER_NATIVE    3u /* native kernel (scalar x86-64), builds with the native backend only */
#define SU_HOST_TIER_OPERATOR  4u /* host operator (reported, never selectable) */

/* 1 when this library was built with the native backend (SU_HOST_TIER_NATIVE can be requested), else 0. */
SU_HOST_API uint32_t su_host_native_available(void);
/* Tier system index runs on. */
SU_HOST_API uint32_t su_host_system_tier(const SuHostWorld *w, uint32_t index, uint32_t *tier);
/* Selects the tier of IR system index. REFERENCE and PLANNED apply to every VM context of the system.
 * NATIVE compiles the system (scalar), admits the kernel by a seeded differential check against the VM and
 * installs it; SU_E_CAPABILITY_DENIED without the native backend, for a system with entity operations
 * (spawn, despawn, emit_event: those stay on the VM) or when the backend cannot lower the module (err->message
 * says why); SU_E_CANDIDATE_FAILED when admission finds a mismatch. A host operator's tier cannot be changed
 * (SU_E_INVALID_ARGUMENT). On failure the system keeps its tier.
 * Native kernels live in a process-wide registry (at most 32 live kernels) that is not thread-safe: in a
 * process where any world uses SU_HOST_TIER_NATIVE, create, step and destroy native worlds from one thread. */
SU_HOST_API uint32_t su_host_system_set_tier(SuHostWorld *w, uint32_t index, uint32_t tier, SuHostError *err);

#ifdef __cplusplus
}
#endif

#endif
