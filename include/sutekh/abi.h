/* Sutekh M0 ABI contract, generation 1: execution boundary.
 *
 * C only; explicit-width integers; no enums, bitfields, variadics, exceptions
 * or longjmp across this boundary; sizes carried in every struct. Pointers
 * inside these structs are borrowed, valid for ONE call, and must not be
 * retained, serialized, or carried across a version boundary (constitution C05).
 * Only 64-bit targets are supported in M0. Changing any layout here requires a
 * new SU_ABI_VERSION and revalidation of every consumer. */
#ifndef SUTEKH_ABI_H
#define SUTEKH_ABI_H

#include <stddef.h>
#include <stdint.h>
#include "sutekh/status.h"
#include "sutekh/ir.h"

#define SU_ABI_VERSION UINT32_C(1)

_Static_assert(sizeof(void *) == 8u, "M0 ABI supports 64-bit targets only");
_Static_assert(sizeof(size_t) == 8u, "M0 ABI supports 64-bit targets only");

#define SU_ACCESS_READ   UINT32_C(1)
#define SU_ACCESS_WRITE  UINT32_C(2)

/* One u32 column view. 40 bytes, no implicit padding.
 * data points at row 0 of a contiguous array of row_count u32 values;
 * stride_bytes must equal 4 in M0 (SoA, no interleave); alignment of data is
 * at least 4 (wrappers may not assume more). */
typedef struct SuColumnBinding {
    uint32_t struct_size;        /* sizeof(SuColumnBinding) */
    uint32_t field_id;           /* stable schema field ID (not an index) */
    uint32_t elem_type;          /* SU_TYPE_U32 */
    uint32_t access;             /* SU_ACCESS_* bitmask */
    uint32_t layout_generation;  /* generation the binding was verified against */
    uint32_t stride_bytes;       /* 4 in M0 */
    uint64_t row_count;          /* rows readable/writable through data */
    void *data;                  /* borrowed; valid only for this call */
} SuColumnBinding;

/* Read-side inputs for one system over the whole table. Loads read these
 * views (state at system entry); the executor must never write through them.
 * bindings contains exactly the READABLE fields of the verified field table,
 * in verified-table order (ascending field_id); binding_count is their count.
 * The executor matches a load's field index to its binding by field_id, never
 * by position alone. */
typedef struct SuInvocation {
    uint32_t struct_size;        /* sizeof(SuInvocation) */
    uint32_t abi_version;        /* SU_ABI_VERSION */
    uint32_t layout_generation;  /* must equal each binding's generation */
    uint32_t binding_count;
    uint64_t row_count;          /* all bindings must have exactly this length */
    const SuColumnBinding *bindings;
} SuInvocation;

/* Write-side staging. One binding per WRITABLE field, in verified-table order,
 * matched by field_id. Staging columns never overlap (byte ranges) any input or
 * other staging column. Because every writable field is stored exactly once per
 * row, a successful call fully defines every staging column. The runtime, not
 * the executor, then COPIES staging into the working root; staging memory is
 * reused every tick. On any non-OK status staging contents are unspecified and
 * must be discarded. */
typedef struct SuOutputs {
    uint32_t struct_size;        /* sizeof(SuOutputs) */
    uint32_t binding_count;
    uint32_t failed_insn;        /* set on failure when attributable, else UINT32_MAX */
    uint32_t reserved;           /* must be zero */
    SuColumnBinding *bindings;
} SuOutputs;

/* Opaque executor context. Created, sized and destroyed by the executor
 * implementation behind the VM interface (M0-04); this header does not export
 * create/destroy. NULL ctx returns SU_E_INVALID_ARGUMENT. */
typedef struct SuExecContext SuExecContext;

/* Entry point shared by the reference VM and (M1) native variants.
 * Preconditions are CHECKED by the wrapper: struct sizes, abi_version,
 * generations, lengths equal to row_count, no aliasing between staging and
 * inputs, and row_count within SU_LIMIT_ROWS. Violations return
 * the status in M0_CONTRACT.md section 3 without touching any column.
 * row_count == 0 returns SU_OK and executes no instruction.
 * Rows run in ascending index order; at most one store per field per row. */
typedef uint32_t (*SuInvokeV1Fn)(SuExecContext *ctx, const SuInvocation *input,
                                 SuOutputs *staged_output);

_Static_assert(sizeof(SuColumnBinding) == 40u, "SuColumnBinding size");
_Static_assert(offsetof(SuColumnBinding, row_count) == 24u, "SuColumnBinding.row_count offset");
_Static_assert(offsetof(SuColumnBinding, data) == 32u, "SuColumnBinding.data offset");
_Static_assert(sizeof(SuInvocation) == 32u, "SuInvocation size");
_Static_assert(offsetof(SuInvocation, row_count) == 16u, "SuInvocation.row_count offset");
_Static_assert(offsetof(SuInvocation, bindings) == 24u, "SuInvocation.bindings offset");
_Static_assert(sizeof(SuOutputs) == 24u, "SuOutputs size");
_Static_assert(offsetof(SuOutputs, bindings) == 16u, "SuOutputs.bindings offset");

#endif
