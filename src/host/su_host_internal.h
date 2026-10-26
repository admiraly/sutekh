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

typedef struct HostRegOp HostRegOp; /* a registered operator (su_host_registered.c) */

/* A host operator the in-process API can run: built in (su_host_operators.c) or registered
 * (su_host_registered.c, su_host_register_operator). Its descriptor, the record's operator name, and the
 * per-world scratch constructor and destructor (rows = the world's capacity). Every callback receives the operator
 * it belongs to (registered operators share one set of callbacks). */
typedef struct HostOperator {
    const SuHostOpDesc *desc;
    const char *operator_name;
    uint32_t (*create)(const struct HostOperator *op, uint64_t rows, void **inst);
    void (*destroy)(void *inst);
    /* Operator data (su_host_set_operator_data; NULL: the operator takes none). *shared is NULL for the first
     * instance of one call and keeps whatever that call parsed for the next ones (released by data_release after
     * the call); why receives a reason on failure. */
    uint32_t (*set_data)(const struct HostOperator *op, void *inst, const uint8_t *data, size_t len, void **shared,
                         char *why, size_t why_cap);
    void (*data_release)(void *shared);
    /* Registered operators: the entry (NULL for built-in operators), and the reason of the instance's last failed
     * run ("" when none; NULL callback for built-in operators). */
    const HostRegOp *reg;
    const char *(*last_error)(const void *inst);
    /* Registered operators: folds the world-specific part of the identity (flags, scratch size for the world's
     * capacity) into the entry's semantic hash at world creation. NULL for built-in operators. */
    void (*identity)(const void *inst, uint8_t semantic[32]);
    /* SuHostOpDesc.operator_name override for the record check and the semantic hash: NULL for built-in operators
     * (their name is derived from the descriptor, host_decl.h), operator_name for registered ones. */
    const char *record_name;
} HostOperator;

/* Built-in operators first, then registered ones. NULL when system_id names neither. */
const HostOperator *su_host_operator_find(const char *system_id);
/* Built-in operators only (su_host_operators.c). */
const HostOperator *su_host_builtin_find(const char *system_id);
uint32_t su_host_builtin_count(void);
/* Registered operators (su_host_registered.c): lookup by system ID, count, and entry by registration index. */
const HostOperator *su_host_registered_find(const char *system_id);
uint32_t su_host_registered_count(void);
const HostOperator *su_host_registered_at(uint32_t index);

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
    uint32_t stepped;              /* su_host_step ran at least one tick */
    uint32_t opdata_mask;          /* bit i: schedule entry i received operator data */
};

#endif
