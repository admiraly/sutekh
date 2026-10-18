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
 * operator record instead, for an operator compiled into this library. */
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
 * with buf NULL; SU_E_RESOURCE_LIMIT when cap is too small (*written = size needed). */
SU_HOST_API uint32_t su_host_snapshot_size(const SuHostWorld *w, size_t *size);
SU_HOST_API uint32_t su_host_snapshot_write(const SuHostWorld *w, uint8_t *buf, size_t cap, size_t *written);
/* Replaces the world's state with the snapshot's (same world identity, else SU_E_SCHEMA_MISMATCH). Strict:
 * any malformed byte is SU_E_PARSE or SU_E_TYPE and nothing changes. Queued inputs are kept. */
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
 * SU_E_CAPABILITY_DENIED when no operator of the record's system_id is built into this library (err->index =
 * capsule index), else the loader's code (SU_E_SCHEMA_MISMATCH, SU_E_TYPE, SU_E_INVALID_ARGUMENT, SU_E_PARSE)
 * with its reason in err->message. The operator then runs in schedule order like any system.
 *
 * State: an operator's results are ordinary columns of the world (its declared writes), stored for every row
 * every tick, so they are in snapshots, su_host_digest, su_host_digest_fast, in-place restore and replays
 * exactly like IR results, and re-simulation after a restore reproduces them bit for bit. Events derived from
 * them are emitted by IR systems that read them. The record's semantic hash (covering the binding) is part of
 * the world identity. Each world owns its operator's scratch (allocated at creation for the world's capacity,
 * never in a tick), so distinct worlds stay independent. */

/* Operators built into this library, by index (0..count-1): system id (for example "space3d.query") and
 * operator name (for example "space3d_ops_v1"). */
SU_HOST_API uint32_t su_host_operator_count(void);
SU_HOST_API uint32_t su_host_operator_info(uint32_t index, const char **system_id, const char **operator_name);

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
