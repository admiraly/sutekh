/* Entity-aware tick execution (M2-00c3, contract M2_ENTITIES_CONTRACT.md sections 1, 2, 5).
 *
 * A schedule with at least one system whose module uses entity ops (uses_entity_ops, IR 0.2) runs
 * through su_ent_run_ticks. Every system runs chunk-wise over SU_CHUNK_SLOTS storage chunks, with
 * one command/event buffer per (system, chunk). Each chunk sets the VM entity view (alive bytes and
 * generations from the working table, base_slot = chunk start, tick_low = committed tick + 1) and
 * emits despawn/event records through su_cmdq writers. Chunks of one system may run on several
 * workers; the output does not depend on the worker count because the merge is canonical.
 *
 * Per system: after its staging is committed, su_world_zero_dead zeroes dead rows of the working
 * root (contract B1). After every system succeeded, the boundary runs: su_cmdq_commit merges the
 * buffers in canonical order, applies despawns to the WORKING entity table (stale expected handles
 * are refused with SU_EVENT_REJECTED), appends the records to the event log, then su_world_zero_dead
 * clears the despawned slots and su_world_publish publishes table, columns and log together. Any
 * failure before publish aborts the tick: committed table, columns and event log are unchanged
 * (the log is appended only by su_cmdq_commit, which runs no callback when it refuses). */
#ifndef SU_RUNTIME_ENTITY_TICK_H
#define SU_RUNTIME_ENTITY_TICK_H

#include <stdint.h>

#include "sutekh/status.h"
#include "world/entities/su_cmdq.h"

typedef struct SuEntRt SuEntRt;
typedef struct SuSchedule SuSchedule;
typedef struct SuWorld SuWorld;
typedef struct SuSchedTickOpts SuSchedTickOpts;
typedef int (*SuSchedFault)(uint32_t system_index, void *ctx);

/* Per-tick caps and the event log capacity (world configuration). Ranges: caps 1..SU_CMDQ_MAX_TICK_CAP,
 * log 1..SU_CMDQ_MAX_LOG_CAPACITY; otherwise SU_E_RESOURCE_LIMIT. */
uint32_t su_ent_rt_create(uint64_t max_struct_tick, uint64_t max_events_tick, uint64_t event_log_max,
                          SuEntRt **out);
void su_ent_rt_destroy(SuEntRt *rt);
SuEventLog *su_ent_rt_log(SuEntRt *rt);
/* Current caps and log capacity. */
uint32_t su_ent_rt_caps(const SuEntRt *rt, uint64_t *max_struct_tick, uint64_t *max_events_tick,
                        uint64_t *event_log_max);
/* Runtime rebuilt from snapshot state (caps as configured by the world, log as recorded). */
uint32_t su_ent_rt_create_restored(uint64_t max_struct_tick, uint64_t max_events_tick, uint64_t event_log_max,
                                   uint64_t next_seq, uint64_t dropped, const SuEvent *ev, uint64_t n,
                                   SuEntRt **out);

/* Admission (contract B8): SU_E_RESOURCE_LIMIT when the command and event buffers of a tick over rows slots
 * exceed SU_CMDQ_MAX_ENTRIES per class. Allocates nothing. */
uint32_t su_ent_budget(const SuSchedule *s, uint64_t rows, uint64_t max_struct_tick, uint64_t max_events_tick);

/* Runs ticks like su_sched_run_ticks_opt for a schedule with entity state. rt must not be NULL.
 * Same done/failed_index contract as su_sched_run_ticks_opt. */
uint32_t su_ent_run_ticks(SuWorld *w, SuSchedule *s, SuEntRt *rt, uint64_t ticks, SuSchedFault fault,
                          void *fault_ctx, const SuSchedTickOpts *opt, uint64_t *done,
                          uint32_t *failed_index);

#endif
