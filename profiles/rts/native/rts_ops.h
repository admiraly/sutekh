/* RTS host operators rts_ops_v1 (M3-04, contract M3_RTS_CONTRACT r4 sections 4.1 to 4.4, 3.1).
 *
 * Two phase operators run as host entries (src/runtime/schedule.h, SU_SYS_KIND_HOST):
 *   rts.spatial  gather of formation anchors, grid build, ring-ordered target query with budget K = 128
 *   rts.scatter  saturating add of damage into the incoming field of target rows
 *
 * Contract of every operator here:
 *   - integer only; no allocation in the tick (static scratch sized by RTS_OPS_MAX_ROWS, admitted at
 *     process start); no mutable state other than the scratch and the per-run fault counter;
 *   - reads only the views it was given (declared reads) and the entity table through the const world
 *     (alive and generation), never the staging of another system;
 *   - stores every declared write for every row (write-only fields) or the rows it changes (read-write
 *     fields, whose working copy is seeded into staging by run_host);
 *   - returns SU_OK only after every store; any SU_E_* code aborts the tick with no commit.
 *
 * Not reentrant: the scratch is shared by every world in the process. The scheduler runs host entries
 * serially (OD-14), and each worker process runs one world, so this is sufficient for M3.
 *
 * Instrumentation (contract 4.4): every store goes through rts_ops_note. A test attaches an
 * RtsOpsInstr to the operator context to record which (declared write, row) pairs were stored. With
 * instr == NULL (product use) the hook costs one branch. */
#ifndef SU_PROFILES_RTS_NATIVE_RTS_OPS_H
#define SU_PROFILES_RTS_NATIVE_RTS_OPS_H

#include <stddef.h>
#include <stdint.h>

#include "runtime/schedule.h"

#define RTS_OPS_VERSION 1u
#define RTS_OPS_NONE 0xFFFFFFFFu
#define RTS_OPS_DOMAIN_MAX 32767u
#define RTS_OPS_RANGE_SQ_MAX 262144u
#define RTS_OPS_KIND_UNIT 1u
#define RTS_OPS_BUDGET_K 128u
#define RTS_OPS_GRID 512u
#define RTS_OPS_CELL_SHIFT 6u
#define RTS_OPS_CELLS (RTS_OPS_GRID * RTS_OPS_GRID)
#define RTS_OPS_RC_MAX 8u
/* Scratch is sized for this many rows. Larger capacities are refused with SU_E_RESOURCE_LIMIT. */
#define RTS_OPS_MAX_ROWS 1000000u
/* No-fault value of the fault seam. */
#define RTS_OPS_NO_FAULT 0xFFFFFFFFu

/* Store recorder (test use). seen holds rows * n_write bytes, indexed [write_index * rows + row]. */
typedef struct RtsOpsInstr {
    uint64_t rows;        /* row count (capacity) of the world under test */
    uint8_t *seen;        /* test-owned buffer, or NULL to count stores only */
    uint32_t n_write;     /* number of declared writes of the operator */
    uint32_t violations;  /* stores that named an undeclared write or an out-of-range row (must stay 0) */
    uint64_t stores;      /* store events recorded */
} RtsOpsInstr;

/* Per-operator context (the host_ctx of the descriptor). */
typedef struct RtsOpsCtx {
    RtsOpsInstr *instr;   /* NULL in product use */
    uint32_t fault_at;    /* RTS_OPS_NO_FAULT, or the 0-based fault point of a run that returns SU_E_INTERNAL */
    uint32_t fault_seen;  /* fault points passed by the current run (reset at the start of each run) */
} RtsOpsCtx;

/* Descriptors registered by host_registry.c (exactly these two production entries). */
extern const SuHostOpDesc g_rts_spatial_desc;
extern const SuHostOpDesc g_rts_scatter_desc;

/* Contexts of the two descriptors (their host_ctx). */
RtsOpsCtx *rts_spatial_ctx(void);
RtsOpsCtx *rts_scatter_ctx(void);

/* Canonical constants text of each operator (4.4). The descriptors carry the SHA-256 of the text as a
 * literal; a test checks that the literal equals the hash of the text. */
const char *rts_spatial_constants_text(void);
const char *rts_scatter_constants_text(void);

/* Exact floor square root of a u32 (bit method). */
uint32_t rts_ops_isqrt(uint32_t n);

/* Saturating u32 add (4.3): min(a + b, 0xFFFFFFFF). */
uint32_t rts_ops_sat_add(uint32_t a, uint32_t b);

/* Records one store of declared write k at row. k is the index of the field in the descriptor's ascending
 * write list. */
static inline void rts_ops_note(RtsOpsCtx *c, uint32_t k, uint64_t row) {
    RtsOpsInstr *i = c->instr;
    if (i == NULL) {
        return;
    }
    i->stores++;
    if (k >= i->n_write || row >= i->rows) {
        i->violations++;
        return;
    }
    if (i->seen != NULL) {
        i->seen[(uint64_t)k * i->rows + row] = 1u;
    }
}

#endif
