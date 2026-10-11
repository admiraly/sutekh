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

/* Row limit used by the wrapper (default SU_LIMIT_ROWS). Returns
 * SU_E_INVALID_ARGUMENT on NULL ctx. */
uint32_t su_vm_context_set_row_limit(SuExecContext *ctx, uint64_t limit);
/* Failed instruction index of the most recent invocation, UINT32_MAX when the
 * failure was not attributable to one instruction or no failure occurred. */
uint32_t su_vm_context_last_failed_insn(const SuExecContext *ctx);

uint32_t su_vm_invoke_v1(SuExecContext *ctx, const SuInvocation *input,
                         SuOutputs *staged_output);

uint32_t su_vm_run_system(SuWorld *w, SuExecContext *ctx);

#endif
