/* Control-plane domains for world.spawn_batch and world.set_fields (M3 contract 2.2, D6, OD-3, OD-9).
 * Pure checks: no world state. Every failure is SU_E_TYPE, the code the contract names for domain
 * violations. Field names that are not in the schema are the caller's SU_E_SCHEMA_MISMATCH. */
#ifndef SUTEKH_RUNTIME_RTS_CONTROL_DOMAINS_H
#define SUTEKH_RUNTIME_RTS_CONTROL_DOMAINS_H

#include <stdint.h>

#include "sutekh/status.h"

/* NONE for slot-valued fields (contract section 2). */
#define SU_RTS_NONE UINT32_C(0xFFFFFFFF)

/* Fourteen writable fields of world.set_fields (contract 2.2). Returns 1 when name is in that set. */
int su_rts_ctl_writable(const char *name);

/* Domain check of one written value for a writable field. Returns SU_E_TYPE for a value outside the
 * domain of the named field, SU_OK otherwise. The caller has checked name with su_rts_ctl_writable. */
uint32_t su_rts_ctl_check_value(const char *name, uint32_t value);

/* formation_slot range against the world capacity (contract 2.2): NONE, or an index in 0..capacity-1.
 * su_rts_ctl_check_value cannot do this because it has no capacity. Returns SU_E_TYPE otherwise. */
uint32_t su_rts_ctl_check_formation_slot(uint32_t value, uint32_t capacity);

/* Resulting-row rules (contract 2.2, D2): applied to the committed row with the payload applied.
 * kind 2 rows keep range_sq 0; formation_gen is 0 exactly when formation_slot is NONE. */
uint32_t su_rts_ctl_check_row(uint32_t kind, uint32_t range_sq, uint32_t formation_slot,
                              uint32_t formation_gen);

/* One named u32 value of a spawn row. name is NUL-terminated and borrowed. */
typedef struct SuRtsNamed {
    const char *name;
    uint32_t value;
} SuRtsNamed;

/* Most defaults su_rts_ctl_spawn_row adds: formation_slot, target_slot, dmg_target, proj_target. */
#define SU_RTS_SPAWN_ADD_MAX 4u

/* Spawn rules of one row (contract 2.2 and D1), mirroring validate_spawn in bench/rts/ref/model.py.
 * in holds the named values of the row; every name is a field of the world schema (the caller has
 * refused unknown names with SU_E_SCHEMA_MISMATCH). has(name, ctx) tells whether the world schema has a
 * field; a rule on a field the schema lacks is skipped, so a partial RTS schema is checked on the fields
 * it has. On SU_OK, add[0..*n_add) holds the NONE defaults for omitted slot-valued fields the schema has;
 * the caller spawns in followed by add. On SU_E_TYPE, *bad names the field at fault. */
uint32_t su_rts_ctl_spawn_row(const SuRtsNamed *in, uint32_t n, uint32_t capacity,
                              int (*has)(const char *name, void *ctx), void *ctx, SuRtsNamed *add,
                              uint32_t *n_add, const char **bad);

#endif
