/* Sutekh M2 entity/structural ABI contract, generation 1 (DRAFT for M2-00 review).
 * Owner: contract owner / integrator. Consumers: src/world/entities (table, queues), world
 * integration, IR 0.2 VM ops, snapshot v0.2, control plane. C only; fixed-width integers; no enums
 * or bitfields. Handles are IDENTITY, not addresses (constitution C05); rows/slots are storage. */
#ifndef SUTEKH_ENTITIES_H
#define SUTEKH_ENTITIES_H

#include <stddef.h>
#include <stdint.h>
#include "sutekh/status.h"

#define SU_ENTITIES_ABI_VERSION UINT32_C(1)

/* Handle = (generation << 32) | slot_index. 0 is reserved invalid. Generations run 1..0xFFFFFFFE;
 * a slot whose generation would reach 0xFFFFFFFF on despawn is RETIRED forever (never reissued,
 * never wrapped) so a stale handle can never alias a live entity. */
typedef uint64_t SuEntity;
#define SU_ENTITY_INVALID           UINT64_C(0)
#define SU_ENTITY_MAKE(slot, gen)   ((((uint64_t)(uint32_t)(gen)) << 32) | (uint64_t)(uint32_t)(slot))
#define SU_ENTITY_SLOT(h)           ((uint32_t)((h) & UINT64_C(0xFFFFFFFF)))
#define SU_ENTITY_GEN(h)            ((uint32_t)((h) >> 32))
#define SU_GEN_FIRST                UINT32_C(1)
#define SU_GEN_RETIRED              UINT32_C(0xFFFFFFFF)

/* Storage is chunked: chunk c holds slots [c*SU_CHUNK_SLOTS, min((c+1)*SU_CHUNK_SLOTS, capacity)).
 * Capacity is fixed at world creation, NOT rounded: capacity = max(initial entities, requested
 * max_entities) <= SU_LIMIT_ROWS, and the last chunk may be short. Growth is an out-of-tick
 * transaction (later). Chunks are the unit of structural/event buffering; scheduler job slicing
 * (src/runtime/jobs chunk_rows) is independent of them. */
#define SU_CHUNK_SLOTS              UINT32_C(1024)

/* Slot allocation is deterministic: the LOWEST free slot index is reused first. Iteration order of
 * every system is ascending slot index over ALIVE slots only. A dead slot never contributes staged
 * writes, structural requests or events, and its column values are canonical zero. */

/* Structural request kinds (queued, applied at the tick boundary in canonical order). */
#define SU_STRUCT_DESPAWN UINT32_C(1) /* arg = expected handle of the target entity */
#define SU_STRUCT_SPAWN   UINT32_C(2) /* RESERVED: no producer in M2-00 (control-plane spawn is
                                         applied directly between ticks; IR/template spawn is a later,
                                         separately versioned step). arg/init[] unused. */

/* Control-plane commands use this system_index and sort BEFORE every IR system in the canonical
 * order (they are applied directly at a quiescent boundary, so they never share a tick with IR
 * requests in M2-00; the value is reserved for event provenance). */
#define SU_SYSTEM_INDEX_HOST UINT32_C(0xFFFFFFFF)

/* Event kinds >= SU_EVENT_KIND_RESERVED_MIN are engine-generated and rejected in IR constants. */
#define SU_EVENT_KIND_RESERVED_MIN UINT32_C(0xFFFFFF00)
#define SU_EVENT_REJECTED          UINT32_C(0xFFFFFFFF) /* a structural request that was refused */
#define SU_EVENT_SPAWNED           UINT32_C(0xFFFFFFFE) /* host spawn applied (a = slot, b = generation) */
#define SU_EVENT_DESPAWNED         UINT32_C(0xFFFFFFFD) /* despawn applied (a = slot, b = old generation) */

/* One queued structural request. 32 bytes, no implicit padding. Canonical commit order is the
 * lexicographic order of (system_index, slot, seq): independent of worker count, chunking and
 * physical completion order (the same rule as the M1 scheduler's canonical merge). */
typedef struct SuStructCmd {
    uint32_t kind;          /* SU_STRUCT_* */
    uint32_t system_index;  /* position of the emitting system in the schedule */
    uint32_t slot;          /* emitting row's slot */
    uint32_t seq;           /* 0-based order of emission within that row in that system */
    uint64_t arg;           /* see kinds */
    uint32_t init[2];
} SuStructCmd;

/* One host-visible event. 40 bytes. Events are OUTPUT only in M2 (read through events.read);
 * systems cannot consume them yet. `event_seq` is assigned at commit in canonical order, is
 * monotonic for the life of the world (u64) and is independent of simulation ticks. */
typedef struct SuEvent {
    uint64_t event_seq;
    uint64_t tick;          /* tick that produced it */
    uint32_t kind;          /* IR-supplied constant */
    uint32_t system_index;
    uint32_t slot;
    uint32_t seq;           /* per-row emission order */
    uint32_t a;
    uint32_t b;
} SuEvent;

_Static_assert(sizeof(SuStructCmd) == 32u, "SuStructCmd size");
_Static_assert(offsetof(SuStructCmd, arg) == 16u, "SuStructCmd.arg offset");
_Static_assert(sizeof(SuEvent) == 40u, "SuEvent size");
_Static_assert(offsetof(SuEvent, kind) == 16u, "SuEvent.kind offset");

#endif
