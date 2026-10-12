/* Shared fixture for the RTS host-operator tests (M3-04): a 39-field rts world (profiles/rts/schema, section 2),
 * a schedule holding the two production host entries, and one-tick execution through the real scheduler
 * (run_host, views, staging, commit and zero-dead). Header-only, static functions. */
#ifndef SU_TESTS_RTS_NATIVE_RTS_FIXTURE_H
#define SU_TESTS_RTS_NATIVE_RTS_FIXTURE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "profiles/rts/native/rts_ops.h"
#include "runtime/rts/host_decl.h"
#include "runtime/schedule.h"
#include "sutekh/abi.h"
#include "sutekh/status.h"
#include "world/su_world.h"

#define RTS_F_COUNT 39u
#define RTS_NONE 0xFFFFFFFFu
#define RTS_CAP_MAX_TEST 65536u

/* Field names by id (index = id), section 2 table. */
static const char *const k_rts_names[RTS_F_COUNT + 1u] = {
    "", "kind", "team", "hp", "pos_x", "pos_y", "speed", "range_sq", "reload", "cooldown", "damage",
    "flight", "order_mode", "order_x", "order_y", "formation_slot", "formation_gen", "formation_ox",
    "formation_oy", "goal_x", "goal_y", "anchor_x", "anchor_y", "anchor_valid", "target_slot", "target_gen",
    "target_dist_sq", "target_valid", "target_x", "target_y", "truncated", "proj_active", "proj_left",
    "proj_target", "proj_target_gen", "proj_damage", "dmg_out", "dmg_target", "dmg_target_gen", "incoming",
};

/* Field ids in the order used by the fixtures (1..39). */
enum {
    F_KIND = 1, F_TEAM = 2, F_HP = 3, F_POS_X = 4, F_POS_Y = 5, F_SPEED = 6, F_RANGE_SQ = 7, F_RELOAD = 8,
    F_COOLDOWN = 9, F_DAMAGE = 10, F_FLIGHT = 11, F_ORDER_MODE = 12, F_ORDER_X = 13, F_ORDER_Y = 14,
    F_FSLOT = 15, F_FGEN = 16, F_FOX = 17, F_FOY = 18, F_GOAL_X = 19, F_GOAL_Y = 20, F_ANCHOR_X = 21,
    F_ANCHOR_Y = 22, F_ANCHOR_VALID = 23, F_TARGET_SLOT = 24, F_TARGET_GEN = 25, F_TARGET_DIST = 26,
    F_TARGET_VALID = 27, F_TARGET_X = 28, F_TARGET_Y = 29, F_TRUNCATED = 30, F_PROJ_ACTIVE = 31,
    F_PROJ_LEFT = 32, F_PROJ_TARGET = 33, F_PROJ_TARGET_GEN = 34, F_PROJ_DAMAGE = 35, F_DMG_OUT = 36,
    F_DMG_TARGET = 37, F_DMG_TARGET_GEN = 38, F_INCOMING = 39
};

/* A row as values by field id (index = id; index 0 unused). */
typedef struct RtsRow {
    uint32_t v[RTS_F_COUNT + 1u];
} RtsRow;

/* Row with the spawn defaults of 2.2 (D1): the four NONE-valued fields, everything else 0. */
static inline RtsRow rts_row_blank(void) {
    RtsRow r;
    memset(&r, 0, sizeof(r));
    r.v[F_FSLOT] = RTS_NONE;
    r.v[F_TARGET_SLOT] = RTS_NONE;
    r.v[F_DMG_TARGET] = RTS_NONE;
    r.v[F_PROJ_TARGET] = RTS_NONE;
    return r;
}

/* Unit (kind 1) with weapon fields in domain: reload 1, damage 1, flight 1 are required by spawn (D1). */
static inline RtsRow rts_unit(uint32_t team, uint32_t x, uint32_t y, uint32_t range_sq) {
    RtsRow r = rts_row_blank();
    r.v[F_KIND] = 1u;
    r.v[F_TEAM] = team;
    r.v[F_HP] = 10u;
    r.v[F_POS_X] = x;
    r.v[F_POS_Y] = y;
    r.v[F_RANGE_SQ] = range_sq;
    r.v[F_RELOAD] = 1u;
    r.v[F_DAMAGE] = 1u;
    r.v[F_FLIGHT] = 1u;
    return r;
}

/* Formation anchor (kind 2): hp 0, range_sq 0, weapon fields at the 7.3 idle value 1. */
static inline RtsRow rts_anchor(uint32_t team, uint32_t x, uint32_t y) {
    RtsRow r = rts_row_blank();
    r.v[F_KIND] = 2u;
    r.v[F_TEAM] = team;
    r.v[F_POS_X] = x;
    r.v[F_POS_Y] = y;
    r.v[F_RELOAD] = 1u;
    r.v[F_DAMAGE] = 1u;
    r.v[F_FLIGHT] = 1u;
    return r;
}

/* World of fields 1..39, all readable and writable (the schema of profiles/rts). */
static inline SuWorld *rts_world_create(uint64_t capacity) {
    SuWorldFieldDesc fd[RTS_F_COUNT];
    SuWorldDesc d;
    SuWorld *w = NULL;
    uint32_t i;
    for (i = 0; i < RTS_F_COUNT; i++) {
        fd[i].id = i + 1u;
        fd[i].type = SU_TYPE_U32;
        fd[i].access = SU_ACCESS_READ | SU_ACCESS_WRITE;
        fd[i].name = k_rts_names[i + 1u];
    }
    memset(&d, 0, sizeof(d));
    d.schema_id = "rts";
    d.schema_version = 1u;
    d.layout_generation = 1u;
    d.row_count = 0u;
    d.entity_ids = NULL;
    d.fields = fd;
    d.field_count = RTS_F_COUNT;
    if (su_world_create_ex(&d, capacity, &w) != SU_OK) {
        return NULL;
    }
    return w;
}

/* Spawns one row with every field named (so no spawn default is implied). Returns SU_OK and the handle. */
static inline uint32_t rts_spawn(SuWorld *w, const RtsRow *row, SuEntity *handle) {
    SuWorldSpawnValue vals[RTS_F_COUNT];
    uint32_t i;
    for (i = 0; i < RTS_F_COUNT; i++) {
        vals[i].field_id = i + 1u;
        vals[i].value = row->v[i + 1u];
    }
    return su_world_spawn(w, vals, RTS_F_COUNT, 0u, handle);
}

/* Declaration module of d on the heap (the schedule takes ownership on success). */
static inline SuLirModule *rts_decl_of(const SuHostOpDesc *d) {
    SuLirModule *m = calloc(1u, sizeof *m);
    if (m == NULL) {
        return NULL;
    }
    if (su_host_decl_build(d, m) != SU_OK) {
        free(m);
        return NULL;
    }
    return m;
}

/* Schedule with the two production host entries (spatial first, scatter second). */
static inline uint32_t rts_schedule_build(SuSchedule *s) {
    SuLirModule *dm;
    uint32_t st;
    su_sched_init(s);
    dm = rts_decl_of(&g_rts_spatial_desc);
    if (dm == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    st = su_sched_add_host(s, &g_rts_spatial_desc, dm, 1u, 1u, 0u, 1u, 0u);
    if (st != SU_OK) {
        su_host_decl_free(dm);
        return st;
    }
    dm = rts_decl_of(&g_rts_scatter_desc);
    if (dm == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    st = su_sched_add_host(s, &g_rts_scatter_desc, dm, 1u, 1u, 0u, 1u, 0u);
    if (st != SU_OK) {
        su_host_decl_free(dm);
        return st;
    }
    return su_sched_finalize(s);
}

/* One tick through the scheduler. */
static inline uint32_t rts_tick(SuWorld *w, SuSchedule *s) {
    uint64_t done = 0;
    uint32_t failed = UINT32_MAX;
    return su_sched_run_ticks(w, s, 1u, NULL, NULL, &done, &failed);
}

/* Committed value of one field at slot (0 when the read fails). */
static inline uint32_t rts_read(const SuWorld *w, uint32_t field, uint32_t slot) {
    const uint32_t *data = NULL;
    uint64_t rows = 0;
    if (su_world_read_committed(w, field, &data, &rows) != SU_OK || data == NULL || slot >= rows) {
        return 0xDEADBEEFu;
    }
    return data[slot];
}

#endif
