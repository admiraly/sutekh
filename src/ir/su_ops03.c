/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * IR 0.3 and 0.4 operation table. See su_ops03.h. */
#include "ir/su_ops03.h"

#include <string.h>

#include "sutekh/ir.h"

#define R(k, t, s) {k, SU_O_REG, (uint8_t)(t), s}
#define U32 SU_TYPE_U32
#define BOOL SU_TYPE_BOOL
#define I32 SU_TYPE_I32
#define I64 SU_TYPE_I64
#define Q32 SU_TYPE_Q32
#define V3 SU_TYPE_V3
#define QUAT SU_TYPE_QUAT

/* The fifteen ops of one scalar family (i32, i64 or q32), in SU_OPX_* order. */
#define FAMILY(sfx, base, T)                                                                              \
    {"add_" sfx, (base) + SU_OPX_ADD, T, 2, {R("a", T, SU_S_A), R("b", T, SU_S_B)}},                      \
    {"sub_" sfx, (base) + SU_OPX_SUB, T, 2, {R("a", T, SU_S_A), R("b", T, SU_S_B)}},                      \
    {"mul_" sfx, (base) + SU_OPX_MUL, T, 2, {R("a", T, SU_S_A), R("b", T, SU_S_B)}},                      \
    {"div_" sfx, (base) + SU_OPX_DIV, T, 2, {R("a", T, SU_S_A), R("b", T, SU_S_B)}},                      \
    {"neg_" sfx, (base) + SU_OPX_NEG, T, 1, {R("a", T, SU_S_A)}},                                         \
    {"abs_" sfx, (base) + SU_OPX_ABS, T, 1, {R("a", T, SU_S_A)}},                                         \
    {"min_" sfx, (base) + SU_OPX_MIN, T, 2, {R("a", T, SU_S_A), R("b", T, SU_S_B)}},                      \
    {"max_" sfx, (base) + SU_OPX_MAX, T, 2, {R("a", T, SU_S_A), R("b", T, SU_S_B)}},                      \
    {"clamp_" sfx, (base) + SU_OPX_CLAMP, T, 3, {R("x", T, SU_S_A), R("lo", T, SU_S_B), R("hi", T, SU_S_C)}}, \
    {"eq_" sfx, (base) + SU_OPX_EQ, BOOL, 2, {R("a", T, SU_S_A), R("b", T, SU_S_B)}},                     \
    {"lt_" sfx, (base) + SU_OPX_LT, BOOL, 2, {R("a", T, SU_S_A), R("b", T, SU_S_B)}},                     \
    {"le_" sfx, (base) + SU_OPX_LE, BOOL, 2, {R("a", T, SU_S_A), R("b", T, SU_S_B)}},                     \
    {"select_" sfx, (base) + SU_OPX_SELECT, T, 3,                                                         \
     {R("cond", BOOL, SU_S_A), R("on_true", T, SU_S_B), R("on_false", T, SU_S_C)}},                       \
    {"shl_" sfx, (base) + SU_OPX_SHL, T, 2, {R("a", T, SU_S_A), R("n", U32, SU_S_B)}},                    \
    {"shr_" sfx, (base) + SU_OPX_SHR, T, 2, {R("a", T, SU_S_A), R("n", U32, SU_S_B)}}

static const SuOp03 OPS03[] = {
    /* constants */
    {"const_i32", SU_OP_CONST_I32, I32, 1, {{"value", SU_O_CI32, 0, SU_S_A}}},
    {"const_i64", SU_OP_CONST_I64, I64, 1, {{"value", SU_O_CI64, 0, SU_S_A}}},
    {"const_q32", SU_OP_CONST_Q32, Q32, 2, {{"num", SU_O_CQNUM, 0, SU_S_CONST}, {"den", SU_O_CQDEN, 0, SU_S_CONST}}},
    /* field access (a 64-bit field is two lanes; the operand names the lane-0 table index) */
    {"load_i32", SU_OP_LOAD_I32, I32, 1, {{"field", SU_O_FREAD, I32, SU_S_A}}},
    {"store_i32", SU_OP_STORE_I32, 0, 2, {{"field", SU_O_FWRITE, I32, SU_S_A}, R("src", I32, SU_S_B)}},
    {"load_i64", SU_OP_LOAD_I64, I64, 1, {{"field", SU_O_FREAD, I64, SU_S_A}}},
    {"store_i64", SU_OP_STORE_I64, 0, 2, {{"field", SU_O_FWRITE, I64, SU_S_A}, R("src", I64, SU_S_B)}},
    {"load_q32", SU_OP_LOAD_Q32, Q32, 1, {{"field", SU_O_FREAD, Q32, SU_S_A}}},
    {"store_q32", SU_OP_STORE_Q32, 0, 2, {{"field", SU_O_FWRITE, Q32, SU_S_A}, R("src", Q32, SU_S_B)}},
    {"load_v3", SU_OP_LOAD_V3, V3, 3,
     {{"x", SU_O_FREAD, Q32, SU_S_A}, {"y", SU_O_FREAD, Q32, SU_S_B}, {"z", SU_O_FREAD, Q32, SU_S_C}}},
    {"store_v3", SU_OP_STORE_V3, 0, 4,
     {{"x", SU_O_FWRITE, Q32, SU_S_A}, {"y", SU_O_FWRITE, Q32, SU_S_B}, {"z", SU_O_FWRITE, Q32, SU_S_C},
      R("src", V3, SU_S_DST)}},
    /* bool */
    {"and_bool", SU_OP_AND_BOOL, BOOL, 2, {R("a", BOOL, SU_S_A), R("b", BOOL, SU_S_B)}},
    {"or_bool", SU_OP_OR_BOOL, BOOL, 2, {R("a", BOOL, SU_S_A), R("b", BOOL, SU_S_B)}},
    {"not_bool", SU_OP_NOT_BOOL, BOOL, 1, {R("a", BOOL, SU_S_A)}},
    /* scalar families */
    FAMILY("i32", SU_OP_I32_BASE, I32),
    FAMILY("i64", SU_OP_I64_BASE, I64),
    FAMILY("q32", SU_OP_Q32_BASE, Q32),
    {"sqrt_q32", SU_OP_SQRT_Q32, Q32, 1, {R("a", Q32, SU_S_A)}},
    {"rsqrt_q32", SU_OP_RSQRT_Q32, Q32, 1, {R("a", Q32, SU_S_A)}},
    /* conversions */
    {"i32_from_u32", SU_OP_I32_FROM_U32, I32, 1, {R("a", U32, SU_S_A)}},
    {"u32_from_i32", SU_OP_U32_FROM_I32, U32, 1, {R("a", I32, SU_S_A)}},
    {"i64_from_i32", SU_OP_I64_FROM_I32, I64, 1, {R("a", I32, SU_S_A)}},
    {"i32_from_i64", SU_OP_I32_FROM_I64, I32, 1, {R("a", I64, SU_S_A)}},
    {"q32_from_i32", SU_OP_Q32_FROM_I32, Q32, 1, {R("a", I32, SU_S_A)}},
    {"q32_from_i64", SU_OP_Q32_FROM_I64, Q32, 1, {R("a", I64, SU_S_A)}},
    {"q32_to_i32_floor", SU_OP_Q32_TO_I32_FLOOR, I32, 1, {R("a", Q32, SU_S_A)}},
    {"q32_to_i32_round", SU_OP_Q32_TO_I32_ROUND, I32, 1, {R("a", Q32, SU_S_A)}},
    {"q32_from_raw", SU_OP_Q32_FROM_RAW, Q32, 1, {R("a", I64, SU_S_A)}},
    {"q32_raw", SU_OP_Q32_RAW, I64, 1, {R("a", Q32, SU_S_A)}},
    {"q32_to_i64_floor", SU_OP_Q32_TO_I64_FLOOR, I64, 1, {R("a", Q32, SU_S_A)}},
    /* vec3 */
    {"make_v3", SU_OP_MAKE_V3, V3, 3, {R("x", Q32, SU_S_A), R("y", Q32, SU_S_B), R("z", Q32, SU_S_C)}},
    {"v3_get", SU_OP_V3_GET, Q32, 2, {R("v", V3, SU_S_A), {"axis", SU_O_AXIS, 3, SU_S_B}}},
    {"v3_add", SU_OP_V3_ADD, V3, 2, {R("a", V3, SU_S_A), R("b", V3, SU_S_B)}},
    {"v3_sub", SU_OP_V3_SUB, V3, 2, {R("a", V3, SU_S_A), R("b", V3, SU_S_B)}},
    {"v3_scale", SU_OP_V3_SCALE, V3, 2, {R("v", V3, SU_S_A), R("s", Q32, SU_S_B)}},
    {"v3_neg", SU_OP_V3_NEG, V3, 1, {R("a", V3, SU_S_A)}},
    {"v3_dot", SU_OP_V3_DOT, Q32, 2, {R("a", V3, SU_S_A), R("b", V3, SU_S_B)}},
    {"v3_cross", SU_OP_V3_CROSS, V3, 2, {R("a", V3, SU_S_A), R("b", V3, SU_S_B)}},
    {"v3_len", SU_OP_V3_LEN, Q32, 1, {R("a", V3, SU_S_A)}},
    {"v3_len_sq", SU_OP_V3_LEN_SQ, Q32, 1, {R("a", V3, SU_S_A)}},
    {"v3_normalize", SU_OP_V3_NORMALIZE, V3, 1, {R("a", V3, SU_S_A)}},
    {"v3_clamp_len", SU_OP_V3_CLAMP_LEN, V3, 2, {R("v", V3, SU_S_A), R("max", Q32, SU_S_B)}},
    {"select_v3", SU_OP_SELECT_V3, V3, 3,
     {R("cond", BOOL, SU_S_A), R("on_true", V3, SU_S_B), R("on_false", V3, SU_S_C)}},
    /* quaternion (w, x, y, z) */
    {"make_quat", SU_OP_MAKE_QUAT, QUAT, 2, {R("w", Q32, SU_S_A), R("v", V3, SU_S_B)}},
    {"quat_get", SU_OP_QUAT_GET, Q32, 2, {R("q", QUAT, SU_S_A), {"axis", SU_O_AXIS, 4, SU_S_B}}},
    {"quat_mul", SU_OP_QUAT_MUL, QUAT, 2, {R("a", QUAT, SU_S_A), R("b", QUAT, SU_S_B)}},
    {"quat_conj", SU_OP_QUAT_CONJ, QUAT, 1, {R("a", QUAT, SU_S_A)}},
    {"quat_rotate", SU_OP_QUAT_ROTATE, V3, 2, {R("q", QUAT, SU_S_A), R("v", V3, SU_S_B)}},
    {"quat_normalize", SU_OP_QUAT_NORMALIZE, QUAT, 1, {R("a", QUAT, SU_S_A)}},
    {"quat_integrate", SU_OP_QUAT_INTEGRATE, QUAT, 3, {R("q", QUAT, SU_S_A), R("w", V3, SU_S_B), R("dt", Q32, SU_S_C)}},
    {"select_quat", SU_OP_SELECT_QUAT, QUAT, 3,
     {R("cond", BOOL, SU_S_A), R("on_true", QUAT, SU_S_B), R("on_false", QUAT, SU_S_C)}},
};

/* IR 0.4 rows (valid only in "0.4" modules). */
static const SuOp03 OPS04[] = {
    /* u32 bit operations and division */
    {"and_u32", SU_OP_AND_U32, U32, 2, {R("a", U32, SU_S_A), R("b", U32, SU_S_B)}},
    {"or_u32", SU_OP_OR_U32, U32, 2, {R("a", U32, SU_S_A), R("b", U32, SU_S_B)}},
    {"xor_u32", SU_OP_XOR_U32, U32, 2, {R("a", U32, SU_S_A), R("b", U32, SU_S_B)}},
    {"not_u32", SU_OP_NOT_U32, U32, 1, {R("a", U32, SU_S_A)}},
    {"shl_u32", SU_OP_SHL_U32, U32, 2, {R("a", U32, SU_S_A), R("n", U32, SU_S_B)}},
    {"shr_u32", SU_OP_SHR_U32, U32, 2, {R("a", U32, SU_S_A), R("n", U32, SU_S_B)}},
    {"div_u32", SU_OP_DIV_U32, U32, 2, {R("a", U32, SU_S_A), R("b", U32, SU_S_B)}},
    {"rem_u32", SU_OP_REM_U32, U32, 2, {R("a", U32, SU_S_A), R("b", U32, SU_S_B)}},
    /* i64 bit operations */
    {"and_i64", SU_OP_AND_I64, I64, 2, {R("a", I64, SU_S_A), R("b", I64, SU_S_B)}},
    {"or_i64", SU_OP_OR_I64, I64, 2, {R("a", I64, SU_S_A), R("b", I64, SU_S_B)}},
    {"xor_i64", SU_OP_XOR_I64, I64, 2, {R("a", I64, SU_S_A), R("b", I64, SU_S_B)}},
    {"not_i64", SU_OP_NOT_I64, I64, 1, {R("a", I64, SU_S_A)}},
    /* cross-row reads of the entry state */
    {"gather_u32", SU_OP_GATHER_U32, U32, 2, {{"field", SU_O_FGATHER, U32, SU_S_A}, R("slot", U32, SU_S_B)}},
    {"gather_i32", SU_OP_GATHER_I32, I32, 2, {{"field", SU_O_FGATHER, I32, SU_S_A}, R("slot", U32, SU_S_B)}},
    {"gather_i64", SU_OP_GATHER_I64, I64, 2, {{"field", SU_O_FGATHER, I64, SU_S_A}, R("slot", U32, SU_S_B)}},
    {"gather_q32", SU_OP_GATHER_Q32, Q32, 2, {{"field", SU_O_FGATHER, Q32, SU_S_A}, R("slot", U32, SU_S_B)}},
    {"find_eq_u32", SU_OP_FIND_EQ_U32, U32, 3,
     {{"field", SU_O_FGATHER, U32, SU_S_A}, R("key", U32, SU_S_B), {"span", SU_O_CSPAN, 0, SU_S_C}}},
    /* entity table read of the entry state (2026-10-26) */
    {"handle_live", SU_OP_HANDLE_LIVE, BOOL, 2, {R("slot", U32, SU_S_A), R("gen", U32, SU_S_B)}},
};

#define N03 (sizeof OPS03 / sizeof OPS03[0])
#define N04 (sizeof OPS04 / sizeof OPS04[0])

const SuOp03 *su_op03_at(size_t i) {
    if (i < N03) {
        return &OPS03[i];
    }
    return i < N03 + N04 ? &OPS04[i - N03] : NULL;
}

const SuOp03 *su_op03_by_name(const char *name, size_t len) {
    size_t i;
    for (i = 0; i < N03 + N04; i++) {
        const SuOp03 *o = su_op03_at(i);
        if (strlen(o->name) == len && memcmp(o->name, name, len) == 0) {
            return o;
        }
    }
    return NULL;
}

const SuOp03 *su_op03_by_code(uint32_t code) {
    size_t i;
    for (i = 0; i < N03 + N04; i++) {
        const SuOp03 *o = su_op03_at(i);
        if (o->code == code) {
            return o;
        }
    }
    return NULL;
}

size_t su_op03_count(void) {
    return N03 + N04;
}

uint32_t su_op_min_minor(const SuOp03 *op) {
    size_t i;
    for (i = 0; i < N04; i++) {
        if (op == &OPS04[i]) {
            return 4u;
        }
    }
    return 3u;
}

uint32_t su_type_lanes(uint32_t value_type) {
    switch (value_type) {
    case SU_TYPE_U32:
    case SU_TYPE_I32:
        return 1u;
    case SU_TYPE_I64:
    case SU_TYPE_Q32:
        return 2u;
    default:
        return 0u;
    }
}

const char *su_type_name(uint32_t type) {
    switch (type) {
    case SU_TYPE_U32: return "u32";
    case SU_TYPE_BOOL: return "bool";
    case SU_TYPE_I32: return "i32";
    case SU_TYPE_I64: return "i64";
    case SU_TYPE_Q32: return "q32";
    case SU_TYPE_V3: return "v3";
    case SU_TYPE_QUAT: return "quat";
    default: return "?";
    }
}
