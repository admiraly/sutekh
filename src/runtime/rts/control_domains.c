#include "runtime/rts/control_domains.h"

#include <stddef.h>
#include <string.h>

/* Bounds of the rts_u32_v1 profile (contract 2.2). Changing a bound is a profile change (OD-9). */
#define POS_MAX      UINT32_C(32767)
#define SPEED_MAX    UINT32_C(1024)
#define RANGE_SQ_MAX UINT32_C(262144)
#define RELOAD_MAX   UINT32_C(1024)
#define DAMAGE_MAX   UINT32_C(65535)
#define FLIGHT_MAX   UINT32_C(64)

static const char *const k_writable[] = {"pos_x",        "pos_y",        "speed",         "range_sq",
                                         "reload",       "damage",       "flight",        "order_mode",
                                         "order_x",      "order_y",      "formation_slot", "formation_gen",
                                         "formation_ox", "formation_oy"};

int su_rts_ctl_writable(const char *name) {
    size_t i;

    if (name == NULL) {
        return 0;
    }
    for (i = 0; i < sizeof k_writable / sizeof k_writable[0]; i++) {
        if (strcmp(name, k_writable[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

uint32_t su_rts_ctl_check_value(const char *name, uint32_t value) {
    if (name == NULL) {
        return SU_E_TYPE;
    }
    if (strcmp(name, "pos_x") == 0 || strcmp(name, "pos_y") == 0 || strcmp(name, "order_x") == 0 ||
        strcmp(name, "order_y") == 0 || strcmp(name, "formation_ox") == 0 || strcmp(name, "formation_oy") == 0) {
        return value <= POS_MAX ? SU_OK : SU_E_TYPE;
    }
    if (strcmp(name, "speed") == 0) {
        return value <= SPEED_MAX ? SU_OK : SU_E_TYPE;
    }
    if (strcmp(name, "range_sq") == 0) {
        return value == 0u || (value >= 1u && value <= RANGE_SQ_MAX) ? SU_OK : SU_E_TYPE;
    }
    if (strcmp(name, "reload") == 0) {
        return value >= 1u && value <= RELOAD_MAX ? SU_OK : SU_E_TYPE;
    }
    if (strcmp(name, "damage") == 0) {
        return value >= 1u && value <= DAMAGE_MAX ? SU_OK : SU_E_TYPE;
    }
    if (strcmp(name, "flight") == 0) {
        return value >= 1u && value <= FLIGHT_MAX ? SU_OK : SU_E_TYPE;
    }
    if (strcmp(name, "order_mode") == 0) {
        return value <= 2u ? SU_OK : SU_E_TYPE;
    }
    if (strcmp(name, "formation_slot") == 0) {
        /* NONE, or any slot index: the capacity bound (0..capacity-1) is checked by the caller against the world. */
        return SU_OK;
    }
    if (strcmp(name, "formation_gen") == 0) {
        /* u32 by type; the pairing with formation_slot is a resulting-row rule (su_rts_ctl_check_row). */
        return SU_OK;
    }
    return SU_E_TYPE;
}

uint32_t su_rts_ctl_check_formation_slot(uint32_t value, uint32_t capacity) {
    if (value == SU_RTS_NONE) {
        return SU_OK;
    }
    return value < capacity ? SU_OK : SU_E_TYPE;
}

uint32_t su_rts_ctl_check_row(uint32_t kind, uint32_t range_sq, uint32_t formation_slot,
                              uint32_t formation_gen) {
    if (kind == 2u && range_sq != 0u) {
        return SU_E_TYPE;
    }
    if (formation_slot == SU_RTS_NONE) {
        return formation_gen == 0u ? SU_OK : SU_E_TYPE;
    }
    return formation_gen != 0u ? SU_OK : SU_E_TYPE;
}

#define HP_MAX UINT32_C(65535)

/* System-owned fields whose spawn value must be the idle value 0 (contract 2.2). */
static const char *const k_idle_zero[] = {
    "cooldown",   "goal_x",       "goal_y",      "anchor_x",       "anchor_y",       "anchor_valid", "target_gen",
    "target_dist_sq", "target_valid", "target_x", "target_y",     "truncated",      "proj_active",  "proj_left",
    "proj_target_gen", "proj_damage", "dmg_out",  "dmg_target_gen", "incoming"};

/* Slot-valued fields whose spawn value is NONE; omitted = NONE (D1). */
static const char *const k_idle_none[] = {"target_slot", "dmg_target", "proj_target"};

/* Index of name in in[0..n), or -1. */
static int find_named(const SuRtsNamed *in, uint32_t n, const char *name) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (strcmp(in[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static uint32_t spawn_bad(const char **bad, const char *name) {
    if (bad != NULL) {
        *bad = name;
    }
    return SU_E_TYPE;
}

uint32_t su_rts_ctl_spawn_row(const SuRtsNamed *in, uint32_t n, uint32_t capacity,
                              int (*has)(const char *name, void *ctx), void *ctx, SuRtsNamed *add,
                              uint32_t *n_add, const char **bad) {
    static const char *const required[] = {"kind", "reload", "damage", "flight"};
    static const char *const checked[] = {"pos_x", "pos_y", "order_x", "order_y", "speed",
                                          "range_sq", "reload", "damage", "flight", "order_mode",
                                          "formation_ox", "formation_oy"};
    uint32_t kind = 0u;
    uint32_t fslot = SU_RTS_NONE;
    uint32_t fgen = 0u;
    uint32_t na = 0u;
    size_t i;
    int k;

    if ((in == NULL && n > 0u) || has == NULL || add == NULL || n_add == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *n_add = 0u;
    for (i = 0; i < sizeof k_idle_zero / sizeof k_idle_zero[0]; i++) {
        k = find_named(in, n, k_idle_zero[i]);
        if (k >= 0 && in[k].value != 0u) {
            return spawn_bad(bad, k_idle_zero[i]);
        }
    }
    for (i = 0; i < sizeof k_idle_none / sizeof k_idle_none[0]; i++) {
        k = find_named(in, n, k_idle_none[i]);
        if (k >= 0 && in[k].value != SU_RTS_NONE) {
            return spawn_bad(bad, k_idle_none[i]);
        }
    }
    for (i = 0; i < sizeof required / sizeof required[0]; i++) {
        if (has(required[i], ctx) && find_named(in, n, required[i]) < 0) {
            return spawn_bad(bad, required[i]);
        }
    }
    k = find_named(in, n, "kind");
    if (k >= 0) {
        kind = in[k].value;
        if (kind != 1u && kind != 2u) {
            return spawn_bad(bad, "kind");
        }
    }
    k = find_named(in, n, "team");
    if (k >= 0 && in[k].value > 1u) {
        return spawn_bad(bad, "team");
    }
    if (kind != 0u && has("hp", ctx)) {
        uint32_t hp = 0u;
        k = find_named(in, n, "hp");
        if (k >= 0) {
            hp = in[k].value;
        }
        if (kind == 1u ? (hp < 1u || hp > HP_MAX) : hp != 0u) {
            return spawn_bad(bad, "hp");
        }
    }
    for (i = 0; i < sizeof checked / sizeof checked[0]; i++) {
        k = find_named(in, n, checked[i]);
        if (k >= 0 && su_rts_ctl_check_value(checked[i], in[k].value) != SU_OK) {
            return spawn_bad(bad, checked[i]);
        }
    }
    k = find_named(in, n, "range_sq");
    if (kind == 2u && k >= 0 && in[k].value != 0u) {
        return spawn_bad(bad, "range_sq");
    }
    k = find_named(in, n, "formation_slot");
    if (k >= 0) {
        fslot = in[k].value;
    }
    if (su_rts_ctl_check_formation_slot(fslot, capacity) != SU_OK) {
        return spawn_bad(bad, "formation_slot");
    }
    k = find_named(in, n, "formation_gen");
    if (k >= 0) {
        fgen = in[k].value;
    }
    if (fslot == SU_RTS_NONE) {
        if (fgen != 0u) {
            return spawn_bad(bad, "formation_gen");
        }
    } else {
        if ((has("formation_gen", ctx) && k < 0) || fgen == 0u) {
            return spawn_bad(bad, "formation_gen");
        }
        if (has("formation_ox", ctx) && find_named(in, n, "formation_ox") < 0) {
            return spawn_bad(bad, "formation_ox");
        }
        if (has("formation_oy", ctx) && find_named(in, n, "formation_oy") < 0) {
            return spawn_bad(bad, "formation_oy");
        }
    }
    if (find_named(in, n, "formation_slot") < 0 && has("formation_slot", ctx)) {
        add[na].name = "formation_slot";
        add[na].value = SU_RTS_NONE;
        na++;
    }
    for (i = 0; i < sizeof k_idle_none / sizeof k_idle_none[0]; i++) {
        if (find_named(in, n, k_idle_none[i]) < 0 && has(k_idle_none[i], ctx)) {
            add[na].name = k_idle_none[i];
            add[na].value = SU_RTS_NONE;
            na++;
        }
    }
    *n_add = na;
    return SU_OK;
}
