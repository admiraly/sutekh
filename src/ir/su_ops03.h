/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * IR 0.3 operation table (LIVE_IR_SPEC "IR 0.3"). One row per opcode: JSON name, frozen code, the type of
 * the defined register (0 when none) and the operands with their JSON key, kind, type and SuInsn slot.
 * The verifier decodes JSON with it, the VM shape-checks hand-built modules with it, and the semantic hash
 * uses it to map field operands to field IDs. */
#ifndef SU_OPS03_H
#define SU_OPS03_H

#include <stddef.h>
#include <stdint.h>

/* Operand kinds. */
#define SU_O_REG    1u /* register; type = required register type */
#define SU_O_FREAD  2u /* field read; type = required field value type */
#define SU_O_FWRITE 3u /* field write; type = required field value type */
#define SU_O_CI32   4u /* JSON integer in the i32 range; stored as its 32 bits */
#define SU_O_CI64   5u /* JSON integer in the i64 range; stored low word in a, high word in b */
#define SU_O_CQNUM  6u /* const_q32 numerator, JSON integer in the i64 range */
#define SU_O_CQDEN  7u /* const_q32 denominator, JSON integer 1..2^64-1 */
#define SU_O_AXIS   8u /* JSON integer 0..type-1 (type is the component count, 3 or 4) */

/* SuInsn slots. SU_S_CONST: the operand is folded with others into a and b (const_q32). */
#define SU_S_DST   0u
#define SU_S_A     1u
#define SU_S_B     2u
#define SU_S_C     3u
#define SU_S_CONST 4u

#define SU_OP03_MAX_OPERANDS 4u

typedef struct SuOp03Operand {
    const char *key;
    uint8_t kind;
    uint8_t type;
    uint8_t slot;
} SuOp03Operand;

typedef struct SuOp03 {
    const char *name;
    uint32_t code;
    uint32_t dst_type;  /* type of the register defined by "dst"; 0 = defines none */
    uint32_t n;         /* operand count */
    SuOp03Operand o[SU_OP03_MAX_OPERANDS];
} SuOp03;

/* Lookup; NULL when the name or code is not an IR 0.3 operation. */
const SuOp03 *su_op03_by_name(const char *name, size_t len);
const SuOp03 *su_op03_by_code(uint32_t code);
/* Table access for exhaustive tests. */
size_t su_op03_count(void);
const SuOp03 *su_op03_at(size_t i);

/* Storage lanes of a field value type: 1 for u32 and i32, 2 for i64 and q32, 0 otherwise. */
uint32_t su_type_lanes(uint32_t value_type);
/* "u32", "bool", "i32", "i64", "q32", "v3", "quat" or "?". */
const char *su_type_name(uint32_t type);

#endif
