/* Checked scalar reference VM (M0-04).
 *
 * An SuExecContext is bound to one verified SuLirModule and the layout
 * generation the system was verified against. The module is borrowed and must
 * outlive the context. su_vm_invoke_v1 matches SuInvokeV1Fn (abi.h) and performs
 * every wrapper check of M0_CONTRACT.md section 3 before touching a column.
 *
 * su_vm_run_system is the world-facing helper: it builds views over the open
 * tick, selects the bindings the module declares, invokes, and on SU_OK commits
 * the module's writable staging columns. It does not begin, publish or abort a
 * tick; the caller owns that protocol (su_world.h). */
#ifndef SU_VM_H
#define SU_VM_H

#include <stdint.h>

#include "ir/su_lir.h"
#include "sutekh/abi.h"
#include "sutekh/status.h"
#include "world/su_world.h"

uint32_t su_vm_context_create(const SuLirModule *module, uint32_t layout_generation,
                              SuExecContext **out);
/* NULL-safe. Does not free the module. */
void su_vm_context_destroy(SuExecContext *ctx);

/* IR 0.2 (M2-00c2). The emitter receives every despawn_self / emit_event that fires, in row order
 * and, within a row, in instruction order. seq is the 0-based per-row counter shared by both ops.
 * A callback returns 0 when the record was buffered, nonzero on buffer overflow; the VM then fails
 * the call with SU_E_RESOURCE_LIMIT and failed_insn set. The records already emitted are NOT
 * retracted: the caller discards its buffers when the call fails. */
typedef struct SuVmEmitter {
    void *user;
    int (*emit_despawn)(void *user, uint32_t slot, uint32_t seq, uint64_t expected_handle);
    int (*emit_event)(void *user, uint32_t slot, uint32_t seq, uint32_t kind, uint32_t a, uint32_t b);
} SuVmEmitter;

/* Sets the entity view used by the NEXT invocations until replaced. Defaults (never set): every row
 * alive, generation 1, base_slot 0 (so slot_u32 = row index), tick_u32 = 0.
 *   alive        NULL = all rows alive; otherwise one byte per row of the call (nonzero = alive). A
 *                dead row is skipped entirely: no staged writes, no emissions. The caller guarantees
 *                the array covers the call's row_count (the VM cannot check it).
 *   generations  NULL = 1 for every row; otherwise one u32 per row of the call.
 *   base_slot    slot_u32 = base_slot + row index (chunk offset), wrapping u32.
 *   tick_low     the value tick_u32 yields: committed_tick + 1 (mod 2^32), supplied by the caller.
 *   emitter      NULL = none. Required (else SU_E_INVALID_ARGUMENT) by a module that contains
 *                despawn_self or emit_event. Both callbacks must be non-NULL when emitter is given.
 * Returns SU_OK or SU_E_INVALID_ARGUMENT (NULL ctx or incomplete emitter). */
uint32_t su_vm_context_set_entity_view(SuExecContext *ctx, const uint8_t *alive,
                                       const uint32_t *generations, uint32_t base_slot,
                                       uint32_t tick_low, const SuVmEmitter *emitter);

/* IR 0.4 (2026-10-19). Whole-table view for the cross-row reads gather_* and find_eq_u32, used by the NEXT
 * invocations until replaced (NULL clears it). table holds exactly the module's READABLE fields, like an
 * SuInvocation over the whole table (row index = slot), and is checked at each invocation of a module that
 * gathers: shape, generation, one row count, no overlap with staging. The struct is copied; its bindings stay
 * borrowed. Without a view the invocation itself is the table, which requires base_slot 0 (a chunk at another
 * offset fails with SU_E_INVALID_ARGUMENT). Gathers read entry state only, so results do not depend on chunking or
 * worker count. Returns SU_E_INVALID_ARGUMENT on NULL ctx. */
uint32_t su_vm_context_set_table_view(SuExecContext *ctx, const SuInvocation *table);

/* IR 0.4 (2026-10-26). Whole entity table for handle_live, used by the NEXT invocations until replaced: alive (one
 * byte per slot, nonzero = alive; NULL = all alive) and generations (one u32 per slot; NULL = 1) of slots
 * [0, slot_count), indexed by slot. Pass the system's entry state (the tick-start entity table); the arrays are
 * borrowed. NULL, NULL, 0 clears it. Without a table the call's entity view (su_vm_context_set_entity_view) over
 * the call's rows is the table, which requires base_slot 0 (otherwise a module with handle_live fails with
 * SU_E_INVALID_ARGUMENT). Returns SU_E_INVALID_ARGUMENT on NULL ctx. */
uint32_t su_vm_context_set_entity_table(SuExecContext *ctx, const uint8_t *alive, const uint32_t *generations,
                                        uint64_t slot_count);

/* Row limit used by the wrapper (default SU_LIMIT_ROWS). Returns
 * SU_E_INVALID_ARGUMENT on NULL ctx. */
uint32_t su_vm_context_set_row_limit(SuExecContext *ctx, uint64_t limit);
/* Failed instruction index of the most recent invocation, UINT32_MAX when the
 * failure was not attributable to one instruction or no failure occurred. */
uint32_t su_vm_context_last_failed_insn(const SuExecContext *ctx);

/* VM tiers (2026-10-18). Both tiers give bit-identical staged columns, emissions, statuses and failed_insn.
 *   SU_VM_TIER_REFERENCE  the checked interpreter: per-read register tag checks, per-row store bookkeeping.
 *   SU_VM_TIER_PLANNED    (default) a plan built at context creation when one static pass over the body proves
 *                         those per-row checks redundant (src/vm/su_vm_plan.h): pre-decoded dispatch,
 *                         row-invariant instructions evaluated once, and rows proven to store their own loaded
 *                         values (no emission) copied instead of executed. A module without a plan, or an
 *                         invocation that leaves a lane the plan uses unbound, runs on the reference tier.
 * The default for new contexts is planned unless su_vm_set_default_tier chose otherwise or the environment
 * variable SUTEKH_VM_TIER is "reference" (read once, at the first call that needs it; su_vm_set_default_tier(0)
 * restores the rule and reads it again). It is a plain
 * process-wide setting: call it before contexts are created on other threads. */
#define SU_VM_TIER_REFERENCE 1u
#define SU_VM_TIER_PLANNED 2u
void su_vm_set_default_tier(uint32_t tier);
uint32_t su_vm_default_tier(void);
/* SU_E_INVALID_ARGUMENT on NULL ctx or an unknown tier. */
uint32_t su_vm_context_set_tier(SuExecContext *ctx, uint32_t tier);

typedef struct SuVmTierInfo {
    uint32_t tier;               /* requested tier of the context */
    uint32_t has_plan;           /* 1 when the module has a plan */
    uint32_t plan_reason;        /* 0 = plan built; else why not (su_vm_plan.h SU_VM_PLAN_*) */
    uint32_t insns;              /* module instructions */
    uint32_t body_insns;         /* instructions executed per row by the plan */
    uint32_t hoisted_insns;      /* row-invariant instructions evaluated at plan build */
    uint32_t has_guard;          /* 1 when the plan has an identity-row guard */
    uint32_t guard_insns;        /* guard instructions per row */
    uint32_t guard_requirements; /* boolean definitions the guard checks */
    uint32_t last_tier;          /* tier that ran the last invocation's rows (0 before any) */
    uint64_t last_rows_body;     /* planned tier: live rows the body executed in the last invocation */
    uint64_t last_rows_skipped;  /* planned tier: live rows the guard copied in the last invocation */
} SuVmTierInfo;
uint32_t su_vm_context_tier_info(const SuExecContext *ctx, SuVmTierInfo *info);

uint32_t su_vm_invoke_v1(SuExecContext *ctx, const SuInvocation *input,
                         SuOutputs *staged_output);

uint32_t su_vm_run_system(SuWorld *w, SuExecContext *ctx);

#endif
