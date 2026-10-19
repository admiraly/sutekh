/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * Planned execution tier of the checked VM (internal to src/vm). See su_vm.h ("VM tiers") for the contract.
 *
 * A plan is built once per context from a module that the context already shape-checked. It is built only
 * when one static pass over the straight-line body proves every per-row check of the reference interpreter
 * redundant:
 *   - every register operand is read after a write of the required type earlier in the same row (so the
 *     reference's per-read tag check can never fail and never observes a previous row);
 *   - every writable lane is stored exactly once per row (so the store-once and all-stored checks pass).
 * A module that fails the pass gets no plan and runs on the reference interpreter unchanged.
 *
 * Execution differs from the reference interpreter only in how it reaches the same results:
 *   - instructions are pre-decoded (register operands as word offsets, family ops already split);
 *   - row-invariant instructions (constants and pure operations on them, whose register is defined once)
 *     are evaluated once when the plan is built, with the same arithmetic;
 *   - an identity-row guard: when the store and emission values of the body are proven, symbolically, to
 *     equal the loaded values of the same fields (no emission) whenever a set of boolean definitions has
 *     given values, rows that meet those values are not executed: their writable lanes are copied from the
 *     input lanes, which is exactly what the body would have stored.
 * Rows are visited in ascending order and emissions happen in the same order, so the staged columns, the
 * emitted records and every status (including an emitter overflow and its failed_insn) are the reference's. */
#ifndef SU_VM_PLAN_H
#define SU_VM_PLAN_H

#include <stddef.h>
#include <stdint.h>

#include "ir/su_lir.h"

/* Pre-decoded instruction. Register operands are word offsets (register index * 4) into the plan's register
 * file; field operands are table indices; constants and axes are raw. */
typedef struct SuVmPlanInsn {
    uint32_t op;
    uint32_t idx; /* index in the module (failed_insn) */
    uint32_t d, a, b, c;
    uint32_t check; /* guard only: 0 none, 1 the defined bool must be false, 2 must be true */
} SuVmPlanInsn;

typedef struct SuVmPlan {
    uint32_t n_body;
    SuVmPlanInsn *body;
    uint32_t has_guard;
    uint32_t n_guard;
    SuVmPlanInsn *guard;
    uint32_t n_identity;
    uint32_t *identity; /* writable lane table indices copied on a guarded (skipped) row */
    uint32_t n_load;
    uint32_t *load_lanes; /* every lane a body or guard instruction reads (in_cols must be bound) */
    uint32_t n_store;
    uint32_t *store_lanes; /* every lane the body stores (out_cols must be bound) */
    int64_t *regs;          /* register_count * 4 words; hoisted registers hold their values */
    uint32_t hoisted;
    uint32_t guard_requirements;
} SuVmPlan;

/* Why a module has no plan (su_vm_plan_build reason). */
#define SU_VM_PLAN_OK 0u
#define SU_VM_PLAN_NO_READ_BEFORE_WRITE 1u /* a register read is not preceded by a write of its type */
#define SU_VM_PLAN_STORE_SHAPE 2u          /* a lane stored twice, or a writable lane never stored */
#define SU_VM_PLAN_UNSUPPORTED_OP 3u
#define SU_VM_PLAN_NO_MEMORY 4u

/* Builds a plan. Returns SU_VM_PLAN_OK with *out set, or a reason with *out NULL. The module must have
 * passed su_vm_context_create's shape checks. */
uint32_t su_vm_plan_build(const SuLirModule *m, SuVmPlan **out);
void su_vm_plan_free(SuVmPlan *p);

/* Runtime state the planned loop needs from the context. */
typedef struct SuVmPlanRun {
    uint32_t *const *in_cols;
    uint32_t *const *out_cols;
    const uint8_t *alive;
    const uint32_t *generations;
    uint32_t base_slot;
    uint32_t tick_low;
    int emits;
    void *emit_user;
    int (*emit_despawn)(void *user, uint32_t slot, uint32_t seq, uint64_t expected_handle);
    int (*emit_event)(void *user, uint32_t slot, uint32_t seq, uint32_t kind, uint32_t a, uint32_t b);
    /* IR 0.4: whole-table columns of gather_* / find_eq_u32 (per table index) and their row count. */
    uint32_t *const *gather_cols;
    uint64_t gather_rows;
    /* IR 0.4 row filter: NULL filter_col = none. A live row with (filter_col[r] & mask) != value copies the
     * writable lanes filter_lanes[0..n) from input to staging and runs nothing. */
    const uint32_t *filter_col;
    uint32_t filter_mask;
    uint32_t filter_value;
    const uint32_t *filter_lanes;
    uint32_t n_filter_lanes;
    /* statistics of the last run (rows executed by the body, rows skipped by the guard) */
    uint64_t rows_body;
    uint64_t rows_skipped;
    uint64_t rows_filtered; /* live rows the row filter skipped */
} SuVmPlanRun;

/* 1 when every lane the plan reads and stores is bound for this invocation. */
int su_vm_plan_bound(const SuVmPlan *p, uint32_t *const *in_cols, uint32_t *const *out_cols);
/* Runs rows [0, rows). Returns SU_OK, or SU_E_RESOURCE_LIMIT with *failed_insn on an emitter overflow. */
uint32_t su_vm_plan_run(SuVmPlan *p, SuVmPlanRun *run, uint64_t rows, uint32_t *failed_insn);

#endif
