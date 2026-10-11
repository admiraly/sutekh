/* Sutekh M0 ABI contract, generation 1: SU-LIR 0.1 normalized form and limits. */
#ifndef SUTEKH_IR_H
#define SUTEKH_IR_H

#include <stddef.h>
#include <stdint.h>

#define SU_IR_VERSION_STRING        "0.1"
#define SU_NUMERIC_PROFILE_U32_MOD_V1 "u32_mod_v1"
#define SU_ARTIFACT_FORMAT_VERSION  UINT32_C(1) /* bump if normalization or hash encoding changes */
#define SU_HASH_BYTES               32u         /* SHA-256 digest */

/* Default defensive limits (LIVE_IR_SPEC section 7). Configurable, checked
 * before any proportional allocation or size multiplication. */
#define SU_LIMIT_IR_SOURCE_BYTES    (256u * 1024u)
#define SU_LIMIT_JSON_DEPTH         32u
#define SU_LIMIT_INSTRUCTIONS       4096u
#define SU_LIMIT_REGISTERS          4096u
#define SU_LIMIT_FIELDS             256u
#define SU_LIMIT_ROWS               UINT64_C(1000000) /* default; runtime may configure it lower or higher */
#define SU_LIMIT_PROTOCOL_MESSAGE   (1024u * 1024u)

/* Opcode numbers are part of the semantic-hash encoding: frozen. */
#define SU_OP_CONST_U32   UINT32_C(1)
#define SU_OP_LOAD_U32    UINT32_C(2)
#define SU_OP_ADD_U32     UINT32_C(3)
#define SU_OP_SUB_U32     UINT32_C(4)
#define SU_OP_MUL_U32     UINT32_C(5)
#define SU_OP_EQ_U32      UINT32_C(6)
#define SU_OP_LT_U32      UINT32_C(7)
#define SU_OP_SELECT_U32  UINT32_C(8)
#define SU_OP_STORE_U32   UINT32_C(9)

/* Register types. bool never coerces to u32. */
#define SU_TYPE_U32   UINT32_C(1)
#define SU_TYPE_BOOL  UINT32_C(2)

/* Sentinel for an unused operand slot. Register/field indices are bounded by
 * SU_LIMIT_REGISTERS/SU_LIMIT_FIELDS, so this value is never a valid index;
 * const_u32 values live in operand a only for SU_OP_CONST_U32, where a may be
 * any u32 (including 0xFFFFFFFF) and is never interpreted as an index. */
#define SU_OPERAND_NONE UINT32_C(0xFFFFFFFF)

/* Verified instruction. Registers are numbered by definition order (this is
 * also the register normalization used by the semantic hash). Field operands
 * are indices into the VERIFIED FIELD TABLE: the capsule's declared fields
 * sorted ascending by field_id (declaration order is not identity). Never names.
 * eq/lt operands must both be u32 registers; results are bool.
 *
 *  op            dst        a              b          c
 *  CONST_U32     reg        value          NONE       NONE
 *  LOAD_U32      reg        field index    NONE       NONE
 *  ADD/SUB/MUL   reg        reg            reg        NONE
 *  EQ/LT         reg(bool)  reg            reg        NONE
 *  SELECT_U32    reg        cond (bool)    on_true    on_false
 *  STORE_U32     NONE       field index    src reg    NONE
 */
typedef struct SuInsn {
    uint32_t op;
    uint32_t dst;
    uint32_t a;
    uint32_t b;
    uint32_t c;
} SuInsn;

_Static_assert(sizeof(SuInsn) == 20u, "SuInsn must be 20 bytes");
_Static_assert(_Alignof(SuInsn) == 4u, "SuInsn alignment");

#endif
