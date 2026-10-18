/* Internal view of a host world, shared by su_host.c and su_replay.c. Not installed; not part of the ABI. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef SU_HOST_INTERNAL_H
#define SU_HOST_INTERNAL_H

#include <stdint.h>

#include "runtime/schedule.h"
#include "sutekh/host.h"
#include "world/su_world.h"

/* One tick's input record: writes sorted by (slot, field id), unique. */
typedef struct HostRecord {
    uint64_t tick;
    uint32_t count;
    uint32_t cap;
    SuHostInput *writes;
} HostRecord;

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
};

#endif
