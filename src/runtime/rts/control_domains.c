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
