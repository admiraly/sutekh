/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * C helpers called by IR 0.3 kernels (backends/native/su_native_lower03.cpp) for the operations that are not
 * emitted inline. One signature for all: pointers to the 32-byte register slots of dst and of operands a, b, c
 * (unused operands point at a valid slot and are ignored). A slot holds a u32 or bool zero-extended, an i32
 * sign-extended, an i64 or q32 in word 0, a v3 in words 0..2, a quat (w, x, y, z) in words 0..3, exactly as the
 * VM's register file does. Every helper computes with the functions of src/ir/su_num_fast.h, which return the
 * reference bits of src/ir/su_num.c (tests/num/test_num_fast.c), so a kernel and the VM agree by construction;
 * tests/ir03/ir03_diff.py --native checks the kernels end to end. */
#include "backends/native/su_native_ops03.h"

#include <stddef.h>
#include <stdint.h>

#include "ir/su_num.h"
#include "ir/su_num_fast.h"
#include "sutekh/ir.h"

typedef int64_t I64;

static SuV3 v3(const I64 *p) {
    SuV3 v;
    v.x = p[0];
    v.y = p[1];
    v.z = p[2];
    return v;
}

static void put3(I64 *d, SuV3 v) {
    d[0] = v.x;
    d[1] = v.y;
    d[2] = v.z;
}

static SuQuat qt(const I64 *p) {
    SuQuat q;
    q.w = p[0];
    q.x = p[1];
    q.y = p[2];
    q.z = p[3];
    return q;
}

static void put4(I64 *d, SuQuat q) {
    d[0] = q.w;
    d[1] = q.x;
    d[2] = q.y;
    d[3] = q.z;
}

#define H(name) static void name(I64 *d, const I64 *a, const I64 *b, const I64 *c)

H(h_i32_div) { (void)c; d[0] = su_i32_div((int32_t)a[0], (int32_t)b[0]); }
H(h_i32_shl) { (void)c; d[0] = su_i32_shl((int32_t)a[0], (uint32_t)b[0]); }
H(h_i32_shr) { (void)c; d[0] = su_i32_shr((int32_t)a[0], (uint32_t)b[0]); }
H(h_i64_div) { (void)c; d[0] = su_i64_div(a[0], b[0]); }
H(h_i64_shl) { (void)c; d[0] = su_i64_shl(a[0], (uint32_t)b[0]); }
H(h_i64_shr) { (void)c; d[0] = su_i64_shr(a[0], (uint32_t)b[0]); }
H(h_q32_div) { (void)c; d[0] = su_q32_divf(a[0], b[0]); }
H(h_q32_shl) { (void)c; d[0] = su_q32_shl(a[0], (uint32_t)b[0]); }
H(h_q32_shr) { (void)c; d[0] = su_q32_shr(a[0], (uint32_t)b[0]); }
H(h_q32_sqrt) { (void)b; (void)c; d[0] = su_q32_sqrtf(a[0]); }
H(h_q32_rsqrt) { (void)b; (void)c; d[0] = su_q32_rsqrtf(a[0]); }
H(h_q32_from_i64) { (void)b; (void)c; d[0] = su_q32_from_i64(a[0]); }
H(h_q32_to_i32_round) { (void)b; (void)c; d[0] = su_q32_to_i32_round(a[0]); }
H(h_v3_dot) { (void)c; d[0] = su_v3_dotf(v3(a), v3(b)); }
H(h_v3_cross) { (void)c; put3(d, su_v3_crossf(v3(a), v3(b))); }
H(h_v3_len) { (void)b; (void)c; d[0] = su_v3_lenf(v3(a)); }
H(h_v3_len_sq) { (void)b; (void)c; d[0] = su_v3_len_sqf(v3(a)); }
H(h_v3_normalize) { (void)b; (void)c; put3(d, su_v3_normalizef(v3(a))); }
H(h_v3_clamp_len) { (void)c; put3(d, su_v3_clamp_lenf(v3(a), b[0])); }
H(h_quat_mul) { (void)c; put4(d, su_quat_mulf(qt(a), qt(b))); }
H(h_quat_conj) { (void)b; (void)c; put4(d, su_quat_conjf(qt(a))); }
H(h_quat_rotate) { (void)c; put3(d, su_quat_rotatef(qt(a), v3(b))); }
H(h_quat_normalize) { (void)b; (void)c; put4(d, su_quat_normalizef(qt(a))); }
H(h_quat_integrate) { put4(d, su_quat_integratef(qt(a), v3(b), c[0])); }

SuNativeHelper03 su_native_helper03(uint32_t op) {
    switch (op) {
    case SU_OP_I32_BASE + SU_OPX_DIV: return h_i32_div;
    case SU_OP_I32_BASE + SU_OPX_SHL: return h_i32_shl;
    case SU_OP_I32_BASE + SU_OPX_SHR: return h_i32_shr;
    case SU_OP_I64_BASE + SU_OPX_DIV: return h_i64_div;
    case SU_OP_I64_BASE + SU_OPX_SHL: return h_i64_shl;
    case SU_OP_I64_BASE + SU_OPX_SHR: return h_i64_shr;
    case SU_OP_Q32_BASE + SU_OPX_DIV: return h_q32_div;
    case SU_OP_Q32_BASE + SU_OPX_SHL: return h_q32_shl;
    case SU_OP_Q32_BASE + SU_OPX_SHR: return h_q32_shr;
    case SU_OP_SQRT_Q32: return h_q32_sqrt;
    case SU_OP_RSQRT_Q32: return h_q32_rsqrt;
    case SU_OP_Q32_FROM_I64: return h_q32_from_i64;
    case SU_OP_Q32_TO_I32_ROUND: return h_q32_to_i32_round;
    case SU_OP_V3_DOT: return h_v3_dot;
    case SU_OP_V3_CROSS: return h_v3_cross;
    case SU_OP_V3_LEN: return h_v3_len;
    case SU_OP_V3_LEN_SQ: return h_v3_len_sq;
    case SU_OP_V3_NORMALIZE: return h_v3_normalize;
    case SU_OP_V3_CLAMP_LEN: return h_v3_clamp_len;
    case SU_OP_QUAT_MUL: return h_quat_mul;
    case SU_OP_QUAT_CONJ: return h_quat_conj;
    case SU_OP_QUAT_ROTATE: return h_quat_rotate;
    case SU_OP_QUAT_NORMALIZE: return h_quat_normalize;
    case SU_OP_QUAT_INTEGRATE: return h_quat_integrate;
    default: return NULL;
    }
}
