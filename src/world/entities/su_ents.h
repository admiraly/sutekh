/* Sutekh M2-00a generational entity table (contract r2, planning/contracts/M2_ENTITIES_CONTRACT.md
 * sections 1, 4, 5).
 *
 * Slot states (exactly one holds for every slot in [0, capacity)):
 *   alive    alive=1, gen in [1, 0xFFFFFFFE]; external id any u32 (0 allowed)
 *   free     alive=0, gen in [1, 0xFFFFFFFE] = generation the next spawn issues; external id 0
 *   retired  alive=0, gen = SU_GEN_RETIRED (0xFFFFFFFF); never reissued; external id 0
 * Despawn increments gen; a despawn of gen 0xFFFFFFFE stores 0xFFFFFFFF and retires the slot.
 *
 * Capacity = max(initial_alive, max_entities), at most SU_LIMIT_ROWS, NOT rounded to chunks. Chunk c
 * covers [c*SU_CHUNK_SLOTS, min((c+1)*SU_CHUNK_SLOTS, capacity)); the last chunk may be short.
 *
 * high_water = 1 + the highest slot ever allocated (0 if none). Slots >= high_water are implicitly
 * dead, generation 1, external id 0, and no entry point here can touch them.
 *
 * Allocation takes the lowest free slot (deterministic). Free slots are tracked by an avail bitmap,
 * per-chunk alive/free counts, a per-chunk summary bitmap of chunks with a free slot, and a summary-
 * word hint (all summary words below the hint are zero). alloc scans at most ceil(chunks/64) summary
 * words plus at most 16 bitmap words of one chunk: a constant bound for SU_LIMIT_ROWS, and amortized
 * O(1) under churn. despawn, validate, external id get/set and handle helpers are O(1). copy and
 * canonical write/parse are O(capacity).
 *
 * All arrays are allocated once in su_ents_create / su_ents_clone / su_ents_canonical_parse. alloc,
 * despawn, validate, copy and iteration never allocate. Capacity exhaustion returns
 * SU_E_RESOURCE_LIMIT. No internal locking. */
#ifndef SUTEKH_WORLD_ENTITIES_SU_ENTS_H
#define SUTEKH_WORLD_ENTITIES_SU_ENTS_H

#include <stddef.h>
#include <stdint.h>
#include "sutekh/entities.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"

/* Free-slot policy name written into the canonical dump and required by the parser. */
#define SU_ENTS_FREE_POLICY "lowest_slot"

typedef struct SuEnts SuEnts;

typedef struct SuEntsDesc {
    uint64_t max_entities;        /* requested capacity; 0 means n_initial */
    uint64_t initial_alive;       /* slots [0, initial_alive) are spawned alive with generation 1 */
    const uint32_t *external_ids; /* initial_alive entries, or NULL meaning all zero */
} SuEntsDesc;

/* Creation. On failure *out is NULL and nothing is retained.
 * SU_E_INVALID_ARGUMENT: NULL desc or out.
 * SU_E_RESOURCE_LIMIT: max(initial_alive, max_entities) > SU_LIMIT_ROWS, or allocation failure. */
uint32_t su_ents_create(const SuEntsDesc *desc, SuEnts **out);
/* NULL-safe. Frees all storage. */
void su_ents_destroy(SuEnts *e);

/* Read-only accessors. NULL e reports zero. */
uint32_t su_ents_capacity(const SuEnts *e);
uint32_t su_ents_chunk_count(const SuEnts *e);
uint32_t su_ents_high_water(const SuEnts *e);
uint32_t su_ents_alive_count(const SuEnts *e);
uint32_t su_ents_retired_count(const SuEnts *e);
uint32_t su_ents_free_count(const SuEnts *e);
/* chunk must be < su_ents_chunk_count. SU_E_INVALID_ARGUMENT otherwise. */
uint32_t su_ents_chunk_alive_count(const SuEnts *e, uint32_t chunk, uint32_t *count);

/* Allocation: lowest free slot, alive with external id ext_id (0 allowed); handle in *out.
 * SU_E_RESOURCE_LIMIT when no free slot exists (retired slots are not free). */
uint32_t su_ents_alloc(SuEnts *e, uint32_t ext_id, SuEntity *out);

/* Despawn with expected handle. Valid iff su_ents_validate(expected) is OK; otherwise returns
 * SU_E_STALE_HANDLE and changes nothing. */
uint32_t su_ents_despawn(SuEnts *e, SuEntity expected);

/* SU_OK iff the handle's generation is nonzero, its slot is < capacity, the slot is alive and the
 * generation equals the stored one. Every other handle returns SU_E_STALE_HANDLE. */
uint32_t su_ents_validate(const SuEnts *e, SuEntity h);

/* Handle for the live entity at slot. SU_E_STALE_HANDLE if the slot is not alive. */
uint32_t su_ents_handle_for_slot(const SuEnts *e, uint32_t slot, SuEntity *out);
int su_ents_slot_alive(const SuEnts *e, uint32_t slot);
int su_ents_slot_retired(const SuEnts *e, uint32_t slot);
/* Stored generation (current for alive slots, next for free, RETIRED for retired). */
uint32_t su_ents_slot_generation(const SuEnts *e, uint32_t slot, uint32_t *gen);

/* External id of a live entity. SU_E_STALE_HANDLE if the handle is not valid. */
uint32_t su_ents_get_external(const SuEnts *e, SuEntity h, uint32_t *ext_id);
uint32_t su_ents_set_external(SuEnts *e, SuEntity h, uint32_t ext_id);

/* Alive iteration (ascending slot). su_ents_next_alive returns 1 and sets *slot to the lowest alive
 * slot >= from, or 0 if none. su_ents_chunk_alive_bits gives the chunk's alive bitmap:
 * *word_count = ceil(chunk_len/64) words; bit k of word w is slot chunk*SU_CHUNK_SLOTS + w*64 + k. */
int su_ents_next_alive(const SuEnts *e, uint32_t from, uint32_t *slot);
uint32_t su_ents_chunk_alive_bits(const SuEnts *e, uint32_t chunk, const uint64_t **words, uint32_t *word_count);

/* Copy into an existing table of identical capacity. No allocation. dst == src is a no-op.
 * SU_E_INVALID_ARGUMENT on NULL or capacity mismatch (dst unchanged). */
uint32_t su_ents_copy(SuEnts *dst, const SuEnts *src);
/* Allocates a new table equal to src. */
uint32_t su_ents_clone(const SuEnts *src, SuEnts **out);

/* Canonical encoding, all integers little-endian, no padding:
 *   "SUENTABL" 0x00 | u32 encoding_version (1) | u32 policy_len (11) | "lowest_slot" |
 *   u64 capacity | u64 high_water | u64 alive_count | u64 retired_count |
 *   for slot in [0, high_water): u32 generation | u32 external_id | u8 alive (0 or 1)
 * Header is 60 bytes; size = 60 + 9 * high_water. */
uint32_t su_ents_canonical_size(const SuEnts *e, size_t *size);
/* Sets *written to the required size; SU_E_INVALID_ARGUMENT if buf is NULL or cap is too small. */
uint32_t su_ents_canonical_write(const SuEnts *e, uint8_t *buf, size_t cap, size_t *written);
/* Strict parser. On failure *out is NULL and nothing is retained.
 * SU_E_INVALID_ARGUMENT: NULL buf or out.
 * SU_E_RESOURCE_LIMIT: capacity > SU_LIMIT_ROWS (checked before any size arithmetic or allocation).
 * SU_E_PARSE: bad magic/version/policy, high_water > capacity, length not exactly the expected size
 *   (truncated or trailing bytes), alive byte not 0/1, generation 0 or 0xFFFFFFFF on an alive slot,
 *   generation 0 on a dead slot, nonzero external id on a dead slot, slot high_water-1 that was never
 *   allocated (dead with generation 1), counts that disagree with the records, or allocation failure. */
uint32_t su_ents_canonical_parse(const uint8_t *buf, size_t len, SuEnts **out);
/* Strict parse into an existing table of the same capacity, without allocating (rollback restore).
 * Same checks and codes as su_ents_canonical_parse, plus SU_E_SCHEMA_MISMATCH when the encoded
 * capacity differs from e's. On failure e holds unspecified (memory-safe) contents: callers parse
 * into a scratch table and swap it in only on SU_OK. */
uint32_t su_ents_canonical_parse_into(SuEnts *e, const uint8_t *buf, size_t len);
/* Exchanges the contents of two tables of the same capacity (O(1), no allocation).
 * SU_E_INVALID_ARGUMENT on NULL or capacity mismatch. */
uint32_t su_ents_swap(SuEnts *a, SuEnts *b);
/* Compares logical state. *equal is 1 or 0. */
uint32_t su_ents_state_equal(const SuEnts *a, const SuEnts *b, int *equal);

/* TEST SEAM (not for production use). Sets the stored generation of a slot below high_water that is
 * alive or free, to gen in [1, 0xFFFFFFFE]. SU_E_INVALID_ARGUMENT for: slot >= high_water, a retired
 * slot, gen 0 or 0xFFFFFFFF. An alive slot's handle changes: re-read it with su_ents_handle_for_slot. */
uint32_t su_ents_fault_set_generation(SuEnts *e, uint32_t slot, uint32_t gen);

/* Exact heap bytes of one entity table: the struct and every array it allocates, computed from the
 * same sizes the allocator is asked for (M3-08a, contract 9.3). NULL e gives 0. */
uint64_t su_ents_memory_bytes(const SuEnts *e);

#endif
