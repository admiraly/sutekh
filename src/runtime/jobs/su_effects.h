/* Effect analysis and schedule graph for one phase group (M1-04).
 *
 * Inputs are verified modules (su_lir.h) plus the scheduling facts of their
 * capsules: phase, after/before edges, and declared parallelism. The verified
 * module keeps the field access table but not the capsule schedule block, so
 * the caller passes those facts in SuEffectInput. The M0 verifier accepts only
 * parallel == "serial"; a parallel declaration therefore comes from the
 * scheduling layer (a capsule-level change is outside this module).
 *
 * Effects are derived from the IR: a LOAD of a field reads it, a STORE writes it.
 * su_effects_check_declared requires the capsule's declared READ/WRITE set to
 * equal the derived set exactly.
 *
 * su_effects_build checks, in order:
 *   1. duplicate system IDs, unknown edge targets, edges across phases:
 *      SU_E_INVALID_ARGUMENT. One cross-phase edge is admitted (post-query
 *      ordering, 2026-10-18): an `after` edge from an IR system to a host
 *      operator (host_declared) whose phase sorts before the IR system's
 *   2. declared effects differ from IR effects: SU_E_SCHEMA_MISMATCH
 *   3. ordering cycles (self edges included): SU_E_INVALID_ARGUMENT, with the
 *      cycle written to err as "ordering cycle: a -> b -> ... -> a"
 *   4. two systems declared parallel, in the same phase, with no ordering path
 *      between them (direct or transitive), whose derived effects conflict on a
 *      field (write/write or read/write): SU_E_SCHEMA_MISMATCH, with the pair and
 *      the field named in err
 * Systems not declared parallel run in the canonical serial order, so conflicts
 * among them are allowed.
 *
 * Canonical serial order: phases in ascending strcmp order. Inside a phase, a
 * Kahn topological sort picks, among ready systems, the smallest (priority,
 * system_id by strcmp). The order is deterministic and independent of input order. */
#ifndef SU_RUNTIME_JOBS_SU_EFFECTS_H
#define SU_RUNTIME_JOBS_SU_EFFECTS_H

#include <stddef.h>
#include <stdint.h>

#include "ir/su_lir.h"
#include "sutekh/status.h"

#define SU_EFFECTS_MAX_SYSTEMS 256u

typedef struct SuEffectInput {
    const char *system_id;          /* NUL-terminated, 1..SU_DIAG_SYSTEM_BYTES-1 bytes */
    const SuLirModule *mod;         /* verified, borrowed for the call only; the graph copies its effects */
    const char *phase;              /* capsule schedule.phase */
    uint32_t priority;              /* lower runs earlier among ready systems */
    uint32_t declared_parallel;     /* nonzero: declared safe to run alongside unordered systems */
    const char *const *after;       /* system IDs that must run before this system */
    uint32_t after_count;
    const char *const *before;      /* system IDs that must run after this system */
    uint32_t before_count;
    uint32_t host_declared;         /* M3-04a: nonzero for a host entry; its read/write sets are the
                                       declared access bits of mod->fields (no instructions to derive),
                                       and su_effects_check_declared is not applied to it */
    uint32_t table;                 /* issue 140: the world table this system iterates (0 for every classic
                                       schedule). Two systems in different tables never share columns, so they
                                       cannot conflict on a field, whatever the field IDs; edges and cycles still
                                       span the whole schedule. */
} SuEffectInput;

/* Derives the read/write sets from the IR. reads[i] and writes[i] (i is a field
 * table index, field_count entries each) are set to 1 when some LOAD or STORE
 * touches field i. */
uint32_t su_effects_derive(const SuLirModule *m, uint8_t *reads, uint8_t *writes);
/* SU_E_SCHEMA_MISMATCH when the declared access of a field differs from the derived
 * effect (declared READ without a load, or LOAD without declared READ; likewise
 * WRITE). The message is written to err when err_cap > 0. */
uint32_t su_effects_check_declared(const SuLirModule *m, char *err, size_t err_cap);

typedef struct SuEffectGraph SuEffectGraph;

/* Builds the graph and the canonical order. The graph copies every string and
 * the derived sets, so the inputs need not outlive the call. On failure *out is
 * NULL and err holds a message (when err_cap > 0). */
uint32_t su_effects_build(const SuEffectInput *in, uint32_t n, SuEffectGraph **out, char *err,
                          size_t err_cap);
void su_effects_free(SuEffectGraph *g);

uint32_t su_effects_count(const SuEffectGraph *g);
/* Writes the canonical serial order as input indices. out must hold count entries. */
uint32_t su_effects_serial_order(const SuEffectGraph *g, uint32_t *out, uint32_t cap);
/* 1 when a and b are ordered (a path exists in either direction), 0 otherwise. */
int su_effects_ordered(const SuEffectGraph *g, uint32_t a, uint32_t b);
/* 1 when systems a and b conflict on some field (write/write or read/write). */
int su_effects_conflict(const SuEffectGraph *g, uint32_t a, uint32_t b);

#endif
