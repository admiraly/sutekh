/* Internal view of a host world, shared by su_host.c and su_replay.c. Not installed; not part of the ABI. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef SU_HOST_INTERNAL_H
#define SU_HOST_INTERNAL_H

#include <stdint.h>

#include "runtime/schedule.h"
#include "sutekh/host.h"
#include "world/entities/su_cmdq.h"
#include "world/su_world.h"

/* One tick's input record: writes sorted by (slot, field id), unique. */
typedef struct HostRecord {
    uint64_t tick;
    uint32_t count;
    uint32_t cap;
    SuHostInput *writes;
} HostRecord;

/* A host operator the in-process API can run (su_host_operators.c): its static descriptor, the record's operator
 * name, and the per-world scratch constructor and destructor (rows = the world's capacity). */
typedef struct HostOperator {
    const SuHostOpDesc *desc;
    const char *operator_name;
    uint32_t (*create)(uint64_t rows, void **inst);
    void (*destroy)(void *inst);
} HostOperator;

const HostOperator *su_host_operator_find(const char *system_id);

struct SuHostWorld {
    SuSchedule sched;              /* owns the modules, VM contexts and the entity runtime */
    SuWorld *w;
    uint32_t hz;
    uint64_t seed;
    uint64_t max_struct;
    uint64_t max_events;
    uint64_t log_max;
    uint8_t identity[32];
    HostRecord *recs;              /* ascending tick */
    uint32_t rec_count;
    uint32_t rec_cap;
    SuEnts *rb_ents;               /* scratch for su_host_snapshot_restore_inplace (allocated on first use) */
    SuEventLog *rb_log;
};

#endif
