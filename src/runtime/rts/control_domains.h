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

/* Resulting-row rules (contract 2.2, D2): applied to the committed row with the payload applied.
 * kind 2 rows keep range_sq 0; formation_gen is 0 exactly when formation_slot is NONE. */
uint32_t su_rts_ctl_check_row(uint32_t kind, uint32_t range_sq, uint32_t formation_slot,
                              uint32_t formation_gen);

#endif
