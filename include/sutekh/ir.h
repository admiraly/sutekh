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
#define SU_LIMIT_FIELDS             1024u /* 256 before 2026-10-19; no persisted format stores it */
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
/* IR 0.2 (M2-00 r2) opcodes. Valid only in modules with ir_version "0.2"; in a 0.1 module they are
 * SU_E_UNKNOWN_OPCODE. Operand mapping in SuInsn (dst a b c):
 *   SLOT_U32     dst=reg                                     current row's slot index
 *   GEN_U32      dst=reg                                     current row's generation
 *   TICK_U32     dst=reg                                     (committed_tick + 1) mod 2^32
 *   DESPAWN_SELF dst=cond reg (bool, a SOURCE here)  a,b,c = NONE
 *   EMIT_EVENT   dst=cond reg (bool, a SOURCE here)  a=kind (const u32, never an index)  b=reg A  c=reg B */
#define SU_OP_SLOT_U32      UINT32_C(10)
#define SU_OP_GEN_U32       UINT32_C(11)
#define SU_OP_TICK_U32      UINT32_C(12)
#define SU_OP_DESPAWN_SELF  UINT32_C(13)
#define SU_OP_EMIT_EVENT    UINT32_C(14)
#define SU_IR_VERSION_STRING_0_2        "0.2"
#define SU_ARTIFACT_FORMAT_VERSION_0_2  UINT32_C(2) /* hashed for 0.2 modules; 0.1 modules keep format 1 */

/* IR 0.3 (numeric profile fixed_q32_v1; LIVE_IR_SPEC "IR 0.3", docs/NUMERIC_FIXED_Q32.md). Valid only in
 * modules with ir_version "0.3"; in 0.1 and 0.2 modules they are SU_E_UNKNOWN_OPCODE. Operand mapping in
 * SuInsn is given by the operand table of src/ir/su_ops03.c (the single source for verifier, VM and hash).
 * Integer families are numbered base + offset; the bases and offsets are frozen like every opcode. */
#define SU_IR_VERSION_STRING_0_3        "0.3"
#define SU_ARTIFACT_FORMAT_VERSION_0_3  UINT32_C(3)
#define SU_NUMERIC_PROFILE_FIXED_Q32_V1 "fixed_q32_v1"
#define SU_OP_CONST_I32     UINT32_C(32)
#define SU_OP_CONST_I64     UINT32_C(33)
#define SU_OP_CONST_Q32     UINT32_C(34)
#define SU_OP_LOAD_I32      UINT32_C(35)
#define SU_OP_STORE_I32     UINT32_C(36)
#define SU_OP_LOAD_I64      UINT32_C(37)
#define SU_OP_STORE_I64     UINT32_C(38)
#define SU_OP_LOAD_Q32      UINT32_C(39)
#define SU_OP_STORE_Q32     UINT32_C(40)
#define SU_OP_LOAD_V3       UINT32_C(41)
#define SU_OP_STORE_V3      UINT32_C(42)
#define SU_OP_AND_BOOL      UINT32_C(44)
#define SU_OP_OR_BOOL       UINT32_C(45)
#define SU_OP_NOT_BOOL      UINT32_C(46)
#define SU_OP_I32_BASE      UINT32_C(64)
#define SU_OP_I64_BASE      UINT32_C(96)
#define SU_OP_Q32_BASE      UINT32_C(128)
#define SU_OPX_ADD    0u
#define SU_OPX_SUB    1u
#define SU_OPX_MUL    2u
#define SU_OPX_DIV    3u
#define SU_OPX_NEG    4u
#define SU_OPX_ABS    5u
#define SU_OPX_MIN    6u
#define SU_OPX_MAX    7u
#define SU_OPX_CLAMP  8u
#define SU_OPX_EQ     9u
#define SU_OPX_LT     10u
#define SU_OPX_LE     11u
#define SU_OPX_SELECT 12u
#define SU_OPX_SHL    13u
#define SU_OPX_SHR    14u
#define SU_OPX_COUNT  15u
#define SU_OP_SQRT_Q32          UINT32_C(160)
#define SU_OP_RSQRT_Q32         UINT32_C(161)
#define SU_OP_I32_FROM_U32      UINT32_C(176)
#define SU_OP_U32_FROM_I32      UINT32_C(177)
#define SU_OP_I64_FROM_I32      UINT32_C(178)
#define SU_OP_I32_FROM_I64      UINT32_C(179)
#define SU_OP_Q32_FROM_I32      UINT32_C(180)
#define SU_OP_Q32_FROM_I64      UINT32_C(181)
#define SU_OP_Q32_TO_I32_FLOOR  UINT32_C(182)
#define SU_OP_Q32_TO_I32_ROUND  UINT32_C(183)
#define SU_OP_Q32_FROM_RAW      UINT32_C(184)
#define SU_OP_Q32_RAW           UINT32_C(185)
#define SU_OP_Q32_TO_I64_FLOOR  UINT32_C(186)
#define SU_OP_MAKE_V3       UINT32_C(192)
#define SU_OP_V3_GET        UINT32_C(193)
#define SU_OP_V3_ADD        UINT32_C(194)
#define SU_OP_V3_SUB        UINT32_C(195)
#define SU_OP_V3_SCALE      UINT32_C(196)
#define SU_OP_V3_NEG        UINT32_C(197)
#define SU_OP_V3_DOT        UINT32_C(198)
#define SU_OP_V3_CROSS      UINT32_C(199)
#define SU_OP_V3_LEN        UINT32_C(200)
#define SU_OP_V3_LEN_SQ     UINT32_C(201)
#define SU_OP_V3_NORMALIZE  UINT32_C(202)
#define SU_OP_V3_CLAMP_LEN  UINT32_C(203)
#define SU_OP_SELECT_V3     UINT32_C(204)
#define SU_OP_MAKE_QUAT       UINT32_C(224)
#define SU_OP_QUAT_GET        UINT32_C(225)
#define SU_OP_QUAT_MUL        UINT32_C(226)
#define SU_OP_QUAT_CONJ       UINT32_C(227)
#define SU_OP_QUAT_ROTATE     UINT32_C(228)
#define SU_OP_QUAT_NORMALIZE  UINT32_C(229)
#define SU_OP_QUAT_INTEGRATE  UINT32_C(230)
#define SU_OP_SELECT_QUAT     UINT32_C(231)

/* IR 0.4 (2026-10-19; LIVE_IR_SPEC "IR 0.4"). Valid only in modules with ir_version "0.4" (numeric profile
 * fixed_q32_v1); in older modules they are SU_E_UNKNOWN_OPCODE. Operand mapping in SuInsn is the IR 0.4 rows of
 * the operation table in src/ir/su_ops03.c. Every 0.3 operation stays valid in 0.4.
 *   u32 bit and division ops (dst a b): shl/shr by n >= 32 give 0; div by 0 gives 0, rem by 0 gives a.
 *   i64 bit ops (dst a b).
 *   gather_u32/_i32/_i64/_q32 (dst = reg, a = field index, b = slot reg): the field of the row at slot b in the
 *     system's entry state; 0 when b is not a row of the table (b >= row count). A v3 is three gather_q32. In a
 *     world with tables (2026-10-27) b is a world slot of the system's own table: the base is subtracted first.
 *   find_eq_u32 (dst = reg, a = field index, b = key reg, c = span constant 1..4096): lowest slot s < span with
 *     field[s] == key in the entry state, else 0xFFFFFFFF. In a world with tables the result is a world slot.
 *   handle_live (dst = bool reg, a = slot reg, b = generation reg; 2026-10-26): true when slot a is a slot of the
 *     entity table, alive, and its generation equals b, in the system's entry state (the tick-start entity table);
 *     false otherwise, and always false for generation 0. Reads no field. */
#define SU_IR_VERSION_STRING_0_4        "0.4"
#define SU_ARTIFACT_FORMAT_VERSION_0_4  UINT32_C(4)
#define SU_OP_AND_U32       UINT32_C(48)
#define SU_OP_OR_U32        UINT32_C(49)
#define SU_OP_XOR_U32       UINT32_C(50)
#define SU_OP_NOT_U32       UINT32_C(51)
#define SU_OP_SHL_U32       UINT32_C(52)
#define SU_OP_SHR_U32       UINT32_C(53)
#define SU_OP_DIV_U32       UINT32_C(54)
#define SU_OP_REM_U32       UINT32_C(55)
#define SU_OP_AND_I64       UINT32_C(56)
#define SU_OP_OR_I64        UINT32_C(57)
#define SU_OP_XOR_I64       UINT32_C(58)
#define SU_OP_NOT_I64       UINT32_C(59)
#define SU_OP_GATHER_U32    UINT32_C(240)
#define SU_OP_GATHER_I32    UINT32_C(241)
#define SU_OP_GATHER_I64    UINT32_C(242)
#define SU_OP_GATHER_Q32    UINT32_C(243)
#define SU_OP_FIND_EQ_U32   UINT32_C(245)
#define SU_OP_HANDLE_LIVE   UINT32_C(246)
#define SU_FIND_SPAN_MAX    4096u
#define SU_FIND_NONE        UINT32_C(0xFFFFFFFF)

/* Register types. bool never coerces to u32. */
#define SU_TYPE_U32   UINT32_C(1)
#define SU_TYPE_BOOL  UINT32_C(2)
/* IR 0.3 register and field value types. Fields of type i64 and q32 occupy two u32 storage lanes. */
#define SU_TYPE_I32   UINT32_C(3)
#define SU_TYPE_I64   UINT32_C(4)
#define SU_TYPE_Q32   UINT32_C(5)
#define SU_TYPE_V3    UINT32_C(6)
#define SU_TYPE_QUAT  UINT32_C(7)

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
