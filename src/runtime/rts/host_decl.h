/* Host-operator declarations and capsule loading (M3-04a, contract M3_RTS_CONTRACT sections 3.1, 3.2, 5.5).
 *
 * A host entry (schedule.h, SU_SYS_KIND_HOST) is an operator compiled into the engine. Its schedule
 * identity and its declared effects come from a host capsule record (schema host variant, M3-03a):
 * phase and ordering from the schedule block, and the field table from the host object. su_host_load
 * checks the record against the operator descriptor and builds the declaration module that the schedule
 * stores in place of IR. The declaration module has no instructions and is never verified, lowered or
 * given a VM context.
 *
 * Constants of the operators are native semantics (contract 4.4): changing them needs a new
 * descriptor, a new operator_version and a new constants_hash. The M3-04a records carry the hashes of the
 * canonical constants texts written in the M3-04a handoff.
 *
 * No production descriptor is registered in M3-04a: su_host_find (host_registry.h) returns NULL for every
 * system, so a world.create item with host_operator is refused (SU_E_CAPABILITY_DENIED) until M3-04
 * registers the RTS descriptors. Tests build descriptors of their own. */
#ifndef SU_RUNTIME_RTS_HOST_DECL_H
#define SU_RUNTIME_RTS_HOST_DECL_H

#include <stddef.h>
#include <stdint.h>

#include "ir/su_lir.h"
#include "runtime/schedule.h"

/* The operator name a host capsule must declare in its host.operator key. */
#define SU_HOST_OPERATOR_NAME "rts_ops_v1"

/* Checks a descriptor: system and schema IDs in range, operator_version >= 1, run set, 1..SU_LIMIT_FIELDS
 * fields in strictly ascending ID order, type SU_TYPE_U32, access nonzero and within READ|WRITE, names
 * in range. Returns SU_OK or SU_E_INVALID_ARGUMENT. */
uint32_t su_host_desc_validate(const SuHostOpDesc *d);

/* Builds the declaration module of d into out (a caller-owned struct, zeroed here). The module carries
 * the system ID, the query (schema ID), numeric_profile "u32_mod_v1" and the declared field table; it
 * has no instructions, register or limits, and its hashes are empty. Owned members are freed by
 * su_lir_module_free. Returns SU_OK, SU_E_INVALID_ARGUMENT (bad descriptor) or SU_E_RESOURCE_LIMIT. On
 * failure out holds nothing that must be freed. */
uint32_t su_host_decl_build(const SuHostOpDesc *d, SuLirModule *out);

/* Frees a declaration module returned by su_host_load (heap) and its members. NULL-safe. */
void su_host_decl_free(SuLirModule *m);

/* Loads the host capsule at capsule_path for descriptor d. Checks, in order: the optional capsule_sha256
 * pin (SU_E_CANDIDATE_FAILED on mismatch), strict JSON, the IR-only keys source, fields and ir_version
 * (refused: SU_E_SCHEMA_MISMATCH), capsule_format "0.1", system_id equal to d->system_id (SU_E_INVALID_ARGUMENT),
 * schema.id equal to d->schema_id (SU_E_SCHEMA_MISMATCH), numeric_profile "u32_mod_v1" (SU_E_TYPE), a
 * schedule block with a phase (SU_E_SCHEMA_MISMATCH when missing), and the host object: operator
 * SU_HOST_OPERATOR_NAME, operator_version and constants_hash equal to the descriptor's, and fields equal to
 * the descriptor's table exactly (count, ID, name, type u32, access). Any of those is SU_E_SCHEMA_MISMATCH.
 * On SU_OK *out is a heap declaration module (free with su_host_decl_free unless it is passed to
 * su_sched_add_host), and schema_version and layout_generation are the record's schema values.
 * msg receives the reason on failure. */
uint32_t su_host_load(const char *capsule_path, const char *expect_capsule_hash, const SuHostOpDesc *d,
                      SuLirModule **out, uint32_t *schema_version, uint32_t *layout_generation, char *msg,
                      size_t msg_cap);

/* 1 when the file at path is strict JSON with a host object (a host capsule record), else 0. Used to refuse
 * a candidate or an admission that names a host system before any reload (contract 3.1, r4). */
int su_host_capsule_is_host(const char *path);

#endif
