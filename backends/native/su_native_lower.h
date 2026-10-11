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
} SuNativeLowerInput;

typedef struct SuNativeLowered {
    uint8_t *bytes;        /* malloc-owned machine code */
    size_t size;
    uint32_t *insn_ranges; /* malloc-owned: 2 entries per instruction, begin then end */
    uint32_t range_count;  /* == insn_count */
    char *disasm;          /* malloc-owned, NUL-terminated AsmJit logger text */
    size_t disasm_len;
} SuNativeLowered;

#ifdef __cplusplus
extern "C" {
#endif

/* Lowers the input to one kernel. On error *out is left zeroed and msg holds a short text. */
uint32_t su_native_lower_x64(const SuNativeLowerInput *in, SuNativeLowered *out, char *msg,
                             size_t msg_cap, uint32_t *failed_insn);

#ifdef __cplusplus
}
#endif

#endif
