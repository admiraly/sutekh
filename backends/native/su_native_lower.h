/* Plain-data interface between su_native.c (C) and su_native_asmjit.cpp (C++).
 *
 * Deliberately includes nothing from include/ or src/: the frozen contract headers are C-only
 * (_Static_assert, _Alignof) and do not compile as C++. The C side copies the verified module into
 * these plain arrays and maps the returned ranges onto SuNativeSourceRange. */
#ifndef SU_NATIVE_LOWER_H
#define SU_NATIVE_LOWER_H

#include <stddef.h>
#include <stdint.h>

/* Must equal the frozen SU_OP_* numbers in include/sutekh/ir.h; checked in su_native.c. */
#define SU_NLOW_OP_CONST_U32 UINT32_C(1)
#define SU_NLOW_OP_LOAD_U32 UINT32_C(2)
#define SU_NLOW_OP_ADD_U32 UINT32_C(3)
#define SU_NLOW_OP_SUB_U32 UINT32_C(4)
#define SU_NLOW_OP_MUL_U32 UINT32_C(5)
#define SU_NLOW_OP_EQ_U32 UINT32_C(6)
#define SU_NLOW_OP_LT_U32 UINT32_C(7)
#define SU_NLOW_OP_SELECT_U32 UINT32_C(8)
#define SU_NLOW_OP_STORE_U32 UINT32_C(9)

/* IR 0.3 (fixed_q32_v1) opcodes used by su_native_lower_x64_03; equal to include/sutekh/ir.h (checked in
 * su_native.c). Scalar families are base + SU_NLOW_X_* offsets. */
#define SU_NLOW_OP_SLOT_U32 UINT32_C(10)
#define SU_NLOW_OP_EMIT_EVENT UINT32_C(14)
#define SU_NLOW_OP_CONST_I32 UINT32_C(32)
#define SU_NLOW_OP_CONST_I64 UINT32_C(33)
#define SU_NLOW_OP_CONST_Q32 UINT32_C(34)
#define SU_NLOW_OP_LOAD_I32 UINT32_C(35)
#define SU_NLOW_OP_STORE_I32 UINT32_C(36)
#define SU_NLOW_OP_LOAD_I64 UINT32_C(37)
#define SU_NLOW_OP_STORE_I64 UINT32_C(38)
#define SU_NLOW_OP_LOAD_Q32 UINT32_C(39)
#define SU_NLOW_OP_STORE_Q32 UINT32_C(40)
#define SU_NLOW_OP_LOAD_V3 UINT32_C(41)
#define SU_NLOW_OP_STORE_V3 UINT32_C(42)
#define SU_NLOW_OP_AND_BOOL UINT32_C(44)
#define SU_NLOW_OP_OR_BOOL UINT32_C(45)
#define SU_NLOW_OP_NOT_BOOL UINT32_C(46)
#define SU_NLOW_OP_I32_BASE UINT32_C(64)
#define SU_NLOW_OP_I64_BASE UINT32_C(96)
#define SU_NLOW_OP_Q32_BASE UINT32_C(128)
#define SU_NLOW_X_ADD 0u
#define SU_NLOW_X_SUB 1u
#define SU_NLOW_X_MUL 2u
#define SU_NLOW_X_DIV 3u
#define SU_NLOW_X_NEG 4u
#define SU_NLOW_X_ABS 5u
#define SU_NLOW_X_MIN 6u
#define SU_NLOW_X_MAX 7u
#define SU_NLOW_X_CLAMP 8u
#define SU_NLOW_X_EQ 9u
#define SU_NLOW_X_LT 10u
#define SU_NLOW_X_LE 11u
#define SU_NLOW_X_SELECT 12u
#define SU_NLOW_X_SHL 13u
#define SU_NLOW_X_SHR 14u
#define SU_NLOW_X_COUNT 15u
#define SU_NLOW_OP_SQRT_Q32 UINT32_C(160)
#define SU_NLOW_OP_RSQRT_Q32 UINT32_C(161)
#define SU_NLOW_OP_I32_FROM_U32 UINT32_C(176)
#define SU_NLOW_OP_U32_FROM_I32 UINT32_C(177)
#define SU_NLOW_OP_I64_FROM_I32 UINT32_C(178)
#define SU_NLOW_OP_I32_FROM_I64 UINT32_C(179)
#define SU_NLOW_OP_Q32_FROM_I32 UINT32_C(180)
#define SU_NLOW_OP_Q32_FROM_I64 UINT32_C(181)
#define SU_NLOW_OP_Q32_TO_I32_FLOOR UINT32_C(182)
#define SU_NLOW_OP_Q32_TO_I32_ROUND UINT32_C(183)
#define SU_NLOW_OP_Q32_FROM_RAW UINT32_C(184)
#define SU_NLOW_OP_Q32_RAW UINT32_C(185)
#define SU_NLOW_OP_Q32_TO_I64_FLOOR UINT32_C(186)
#define SU_NLOW_OP_MAKE_V3 UINT32_C(192)
#define SU_NLOW_OP_V3_GET UINT32_C(193)
#define SU_NLOW_OP_V3_ADD UINT32_C(194)
#define SU_NLOW_OP_V3_SUB UINT32_C(195)
#define SU_NLOW_OP_V3_SCALE UINT32_C(196)
#define SU_NLOW_OP_V3_NEG UINT32_C(197)
#define SU_NLOW_OP_SELECT_V3 UINT32_C(204)
#define SU_NLOW_OP_V3_DOT UINT32_C(198)
#define SU_NLOW_OP_V3_CROSS UINT32_C(199)
#define SU_NLOW_OP_V3_LEN UINT32_C(200)
#define SU_NLOW_OP_V3_LEN_SQ UINT32_C(201)
#define SU_NLOW_OP_V3_NORMALIZE UINT32_C(202)
#define SU_NLOW_OP_V3_CLAMP_LEN UINT32_C(203)
#define SU_NLOW_OP_QUAT_MUL UINT32_C(226)
#define SU_NLOW_OP_QUAT_CONJ UINT32_C(227)
#define SU_NLOW_OP_QUAT_ROTATE UINT32_C(228)
#define SU_NLOW_OP_QUAT_NORMALIZE UINT32_C(229)
#define SU_NLOW_OP_QUAT_INTEGRATE UINT32_C(230)
#define SU_NLOW_OP_MAKE_QUAT UINT32_C(224)
#define SU_NLOW_OP_QUAT_GET UINT32_C(225)
#define SU_NLOW_OP_SELECT_QUAT UINT32_C(231)
/* IR 0.4 u32 and i64 bit operations (inline). */
#define SU_NLOW_OP_AND_U32 UINT32_C(48)
#define SU_NLOW_OP_OR_U32 UINT32_C(49)
#define SU_NLOW_OP_XOR_U32 UINT32_C(50)
#define SU_NLOW_OP_NOT_U32 UINT32_C(51)
#define SU_NLOW_OP_SHL_U32 UINT32_C(52)
#define SU_NLOW_OP_SHR_U32 UINT32_C(53)
#define SU_NLOW_OP_DIV_U32 UINT32_C(54)
#define SU_NLOW_OP_REM_U32 UINT32_C(55)
#define SU_NLOW_OP_AND_I64 UINT32_C(56)
#define SU_NLOW_OP_OR_I64 UINT32_C(57)
#define SU_NLOW_OP_XOR_I64 UINT32_C(58)
#define SU_NLOW_OP_NOT_I64 UINT32_C(59)
#define SU_NLOW_HELPER_SLOTS 256u

/* field_access bits (same values as SU_ACCESS_READ / SU_ACCESS_WRITE). */
#define SU_NLOW_ACCESS_READ UINT32_C(1)
#define SU_NLOW_ACCESS_WRITE UINT32_C(2)

/* Status returned by su_native_lower_x64. */
#define SU_NLOW_OK UINT32_C(0)
#define SU_NLOW_RESOURCE UINT32_C(1)
#define SU_NLOW_INTERNAL UINT32_C(2)

typedef struct SuNativeLowerInput {
    uint32_t register_count;
    uint32_t field_count;
    uint32_t insn_count;
    const uint32_t *field_access; /* field_count entries, verified table order */
    const uint32_t *insn_words;   /* 5 entries per instruction: op, dst, a, b, c */
    /* IR 0.3 lowering only (su_native_lower_x64_03): the address of the C helper of each opcode
     * (backends/native/su_native_ops03.h), 0 when the operation is emitted inline. SU_NLOW_HELPER_SLOTS entries. */
    const uint64_t *helpers;
    /* IR 0.3 identity-row guard (from the VM plan builder, src/vm/su_vm_plan.h); counts 0 = no guard.
     * guard_insns: instruction indices of the guard slice in order; guard_checks: 0 none, 1 the defined bool must
     * be false, 2 must be true; identity_lanes: writable lane table indices copied on a guarded row. */
    uint32_t guard_count;
    const uint32_t *guard_insns;
    const uint8_t *guard_checks;
    uint32_t identity_count;
    const uint32_t *identity_lanes;
    /* Row-invariant instructions (the plan's hoisted set: constants and pure operations on them whose register
     * is defined once), emitted once before the row loop so the guard slice can read them; the body still
     * contains them. */
    uint32_t pre_count;
    const uint32_t *pre_insns;
    /* IR 0.4 row filter (has_filter 0 = none): a row runs only when (lane filter_field & filter_mask) ==
     * filter_value; every other row keeps its input values in the writable lanes filter_lanes[0..filter_count),
     * which are copied as whole columns before the loop (as the identity lanes are). */
    uint32_t has_filter;
    uint32_t filter_field;
    uint32_t filter_mask;
    uint32_t filter_value;
    uint32_t filter_count;
    const uint32_t *filter_lanes;
} SuNativeLowerInput;

typedef struct SuNativeLowered {
    uint8_t *bytes;        /* malloc-owned machine code */
    size_t size;
    uint32_t *insn_ranges; /* malloc-owned: 2 entries per instruction, begin then end */
    uint32_t range_count;  /* == insn_count */
    char *disasm;          /* malloc-owned, NUL-terminated AsmJit logger text */
    size_t disasm_len;
    /* Windows x64 unwind data (2026-10-18, backends/native/su_native_unwind.h): malloc-owned UNWIND_INFO bytes for
     * the code range [0, func_end) (the zero-row tail after it is a leaf); NULL / 0 when none was produced. */
    uint8_t *unwind;
    uint32_t unwind_size;
    uint32_t func_end;
} SuNativeLowered;

#ifdef __cplusplus
extern "C" {
#endif

/* Lowers the input to one kernel. On error *out is left zeroed and msg holds a short text. */
uint32_t su_native_lower_x64(const SuNativeLowerInput *in, SuNativeLowered *out, char *msg,
                             size_t msg_cap, uint32_t *failed_insn);

/* IR 0.3 scalar lowering (backends/native/su_native_lower03.cpp): every operation of a 0.3 module without
 * entity operations; heavy operations call in->helpers. Same kernel ABI and failure contract. */
uint32_t su_native_lower_x64_03(const SuNativeLowerInput *in, SuNativeLowered *out, char *msg,
                                size_t msg_cap, uint32_t *failed_insn);

#ifdef __cplusplus
}
#endif

#endif
