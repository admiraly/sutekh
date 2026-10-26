/* Operator state (include/sutekh/host.h, "operator state"; issue 139): the engine-owned, canonical key/value store
 * of one registered host operator in one world. Internal; the public functions su_host_state_* are in
 * su_host_state.c too.
 *
 * Two sides: the committed state C (what snapshots, digests and views see) and the working state W of the running
 * tick. W is created lazily by the first change in a tick (a copy of C's entry table; values are shared with C and
 * copied on write), becomes C when the tick publishes (su_hstate_commit, an O(1) swap) and is dropped when it fails
 * (su_hstate_abort). Between ticks W is free and receives a snapshot's state during a restore (su_hstate_load),
 * which su_hstate_swap then publishes, or undoes when called again. Nothing here allocates after
 * su_hstate_create. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef SU_HOST_STATE_H
#define SU_HOST_STATE_H

#include <stddef.h>
#include <stdint.h>

#include "sutekh/host.h"
#include "world/entities/su_ents.h"

/* Allocates everything for the declared maximum (SU_E_RESOURCE_LIMIT when that fails). */
uint32_t su_hstate_create(uint32_t max_entries, uint32_t max_bytes, uint32_t flags, SuHostState **out);
void su_hstate_destroy(SuHostState *s);

/* Around the operator's run: changes are allowed in between. alive and gen are the run's working entity views
 * (rows entries), used by SU_HOST_STATE_DROP_DEAD. */
void su_hstate_begin_run(SuHostState *s, const uint8_t *alive, const uint32_t *gen, uint64_t rows);
void su_hstate_end_run(SuHostState *s);
/* Refusals at a cap since the last begin_run (for the step's error message). */
uint32_t su_hstate_cap_refusals(const SuHostState *s);

/* An entity may have died (su_host_despawn, or a despawn applied by a tick): the next commit scans for dead entries.
 * Restores (su_hstate_swap) set the same mark. */
void su_hstate_note_despawn(SuHostState *s);
/* The tick published: with SU_HOST_STATE_DROP_DEAD, and only when an entity may have died since the last purge
 * (su_hstate_note_despawn), erase entries whose entity is not alive in ents (the committed table after the tick);
 * then W becomes C. Cannot fail. */
void su_hstate_commit(SuHostState *s, const SuEnts *ents);
/* The tick failed (or a loaded state is discarded): C stays, W is dropped. */
void su_hstate_abort(SuHostState *s);

/* Committed state's encoding (host.h: u32 count | u64 bytes | entries | values). */
size_t su_hstate_encoded_size(const SuHostState *s);
void su_hstate_encode(const SuHostState *s, uint8_t *out);
/* Between ticks: parses an encoding at p (at most avail bytes) strictly into W, *used = its length. SU_E_PARSE
 * with a reason in why on any malformed or oversized byte; C is never touched. */
uint32_t su_hstate_load(SuHostState *s, const uint8_t *p, size_t avail, size_t *used, char *why, size_t why_cap);
/* Exchanges C and the loaded W (a second call undoes the first). */
void su_hstate_swap(SuHostState *s);

#endif
