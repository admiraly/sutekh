/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * Planned execution tier of the checked VM. See su_vm_plan.h. */
#include "vm/su_vm_plan.h"

#include <stdlib.h>
#include <string.h>

#include "ir/su_num.h"
#include "ir/su_num_fast.h"
#include "ir/su_ops03.h"
#include "sutekh/entities.h"
#include "sutekh/status.h"

#define NONE UINT32_MAX
#define PROOF_BUDGET 20000u

/* ---- operand shape of one instruction ---------------------------------- */

#define K_PURE 0u   /* computes from registers (and constants) only */
#define K_LOAD 1u   /* reads field lanes of the row */
#define K_STORE 2u  /* stores field lanes */
#define K_ROW 3u    /* slot_u32, gen_u32, tick_u32: per-row or per-invocation values */
#define K_EMIT 4u   /* emit_event, despawn_self */

typedef struct Shape {
    uint32_t nread;
    uint8_t slot[4]; /* SU_S_* of each register read */
    uint8_t type[4];
    uint32_t wtype; /* type written to dst; 0 = none */
    uint32_t kind;
} Shape;

static void rd(Shape *s, uint32_t slot, uint32_t type) {
    s->slot[s->nread] = (uint8_t)slot;
    s->type[s->nread] = (uint8_t)type;
    s->nread++;
}

/* 0 when the opcode is unknown. */
static int shape_of(uint32_t op, Shape *s) {
    const SuOp03 *o;
    uint32_t k;
    memset(s, 0, sizeof *s);
    switch (op) {
    case SU_OP_CONST_U32:
        s->wtype = SU_TYPE_U32;
        return 1;
    case SU_OP_LOAD_U32:
        s->wtype = SU_TYPE_U32;
        s->kind = K_LOAD;
        return 1;
    case SU_OP_ADD_U32:
    case SU_OP_SUB_U32:
    case SU_OP_MUL_U32:
        rd(s, SU_S_A, SU_TYPE_U32);
        rd(s, SU_S_B, SU_TYPE_U32);
        s->wtype = SU_TYPE_U32;
        return 1;
    case SU_OP_EQ_U32:
    case SU_OP_LT_U32:
        rd(s, SU_S_A, SU_TYPE_U32);
        rd(s, SU_S_B, SU_TYPE_U32);
        s->wtype = SU_TYPE_BOOL;
        return 1;
    case SU_OP_SELECT_U32:
        rd(s, SU_S_A, SU_TYPE_BOOL);
        rd(s, SU_S_B, SU_TYPE_U32);
        rd(s, SU_S_C, SU_TYPE_U32);
        s->wtype = SU_TYPE_U32;
        return 1;
    case SU_OP_STORE_U32:
        rd(s, SU_S_B, SU_TYPE_U32);
        s->kind = K_STORE;
        return 1;
    case SU_OP_SLOT_U32:
    case SU_OP_GEN_U32:
    case SU_OP_TICK_U32:
        s->wtype = SU_TYPE_U32;
        s->kind = K_ROW;
        return 1;
    case SU_OP_DESPAWN_SELF:
        rd(s, SU_S_DST, SU_TYPE_BOOL);
        s->kind = K_EMIT;
        return 1;
    case SU_OP_EMIT_EVENT:
        rd(s, SU_S_DST, SU_TYPE_BOOL);
        rd(s, SU_S_B, SU_TYPE_U32);
        rd(s, SU_S_C, SU_TYPE_U32);
        s->kind = K_EMIT;
        return 1;
    default:
        break;
    }
    o = su_op03_by_code(op);
    if (o == NULL) {
        return 0;
    }
    s->wtype = o->dst_type;
    for (k = 0; k < o->n; k++) {
        if (o->o[k].kind == SU_O_REG) {
            rd(s, o->o[k].slot, o->o[k].type);
        } else if (o->o[k].kind == SU_O_FREAD) {
            s->kind = K_LOAD;
        } else if (o->o[k].kind == SU_O_FWRITE) {
            s->kind = K_STORE;
        }
    }
    return 1;
}

static uint32_t slot_value(const SuInsn *ins, uint32_t slot) {
    switch (slot) {
    case SU_S_DST: return ins->dst;
    case SU_S_A: return ins->a;
    case SU_S_B: return ins->b;
    default: return ins->c;
    }
}

/* Field lanes (table indices) a load or store touches; returns the count (<= 6). */
static uint32_t lanes_of(const SuInsn *ins, const SuLirModule *m, uint32_t out[6]) {
    uint32_t n = 0;
    switch (ins->op) {
    case SU_OP_LOAD_U32:
    case SU_OP_STORE_U32:
    case SU_OP_LOAD_I32:
    case SU_OP_STORE_I32:
        out[n++] = ins->a;
        break;
    case SU_OP_LOAD_I64:
    case SU_OP_STORE_I64:
    case SU_OP_LOAD_Q32:
    case SU_OP_STORE_Q32:
        out[n++] = ins->a;
        out[n++] = ins->a + 1u;
        break;
    case SU_OP_LOAD_V3:
    case SU_OP_STORE_V3:
        out[n++] = ins->a;
        out[n++] = ins->a + 1u;
        out[n++] = ins->b;
        out[n++] = ins->b + 1u;
        out[n++] = ins->c;
        out[n++] = ins->c + 1u;
        break;
    default:
        break;
    }
    (void)m;
    return n;
}

/* ---- execution ---------------------------------------------------------- */

static int64_t col64(uint32_t *const *cols, uint32_t idx, size_t r) {
    return su_i64_from_bits(((uint64_t)cols[idx + 1u][r] << 32) | (uint64_t)cols[idx][r]);
}

static void st64(uint32_t *const *cols, uint32_t idx, size_t r, int64_t v) {
    cols[idx][r] = (uint32_t)((uint64_t)v & 0xFFFFFFFFu);
    cols[idx + 1u][r] = (uint32_t)((uint64_t)v >> 32);
}

static SuV3 v3_at(const int64_t *R, uint32_t o) {
    SuV3 v;
    v.x = R[o];
    v.y = R[o + 1u];
    v.z = R[o + 2u];
    return v;
}

static void v3_put(int64_t *R, uint32_t o, SuV3 v) {
    R[o] = v.x;
    R[o + 1u] = v.y;
    R[o + 2u] = v.z;
}

static SuQuat q_at(const int64_t *R, uint32_t o) {
    SuQuat q;
    q.w = R[o];
    q.x = R[o + 1u];
    q.y = R[o + 2u];
    q.z = R[o + 3u];
    return q;
}

static void q_put(int64_t *R, uint32_t o, SuQuat q) {
    R[o] = q.w;
    R[o + 1u] = q.x;
    R[o + 2u] = q.y;
    R[o + 3u] = q.z;
}

/* Scalar families: the arithmetic of the reference interpreter (su_vm.c su_family / su_family_shift). */
#define FAM(base, x) ((base) + (x))

/* Executes one pre-decoded instruction for row r. Only emissions can fail (SU_E_RESOURCE_LIMIT). */
static uint32_t exec1(const SuVmPlanInsn *p, int64_t *R, SuVmPlanRun *run, size_t r, uint32_t *seq) {
    switch (p->op) {
    /* ---- IR 0.1 / 0.2 ---- */
    case SU_OP_CONST_U32:
        R[p->d] = (int64_t)p->a;
        return SU_OK;
    case SU_OP_LOAD_U32:
        R[p->d] = (int64_t)run->in_cols[p->a][r];
        return SU_OK;
    case SU_OP_ADD_U32:
        R[p->d] = (int64_t)(uint32_t)((uint32_t)R[p->a] + (uint32_t)R[p->b]);
        return SU_OK;
    case SU_OP_SUB_U32:
        R[p->d] = (int64_t)(uint32_t)((uint32_t)R[p->a] - (uint32_t)R[p->b]);
        return SU_OK;
    case SU_OP_MUL_U32:
        R[p->d] = (int64_t)(uint32_t)((uint32_t)R[p->a] * (uint32_t)R[p->b]);
        return SU_OK;
    case SU_OP_EQ_U32:
        R[p->d] = R[p->a] == R[p->b];
        return SU_OK;
    case SU_OP_LT_U32:
        R[p->d] = R[p->a] < R[p->b];
        return SU_OK;
    case SU_OP_SELECT_U32:
        R[p->d] = R[p->a] != 0 ? R[p->b] : R[p->c];
        return SU_OK;
    case SU_OP_STORE_U32:
        run->out_cols[p->a][r] = (uint32_t)R[p->b];
        return SU_OK;
    case SU_OP_SLOT_U32:
        R[p->d] = (int64_t)(uint32_t)(run->base_slot + (uint32_t)r);
        return SU_OK;
    case SU_OP_GEN_U32:
        R[p->d] = (int64_t)(run->generations != NULL ? run->generations[r] : SU_GEN_FIRST);
        return SU_OK;
    case SU_OP_TICK_U32:
        R[p->d] = (int64_t)run->tick_low;
        return SU_OK;
    case SU_OP_DESPAWN_SELF: {
        uint32_t slot, gen;
        if (R[p->d] == 0) {
            return SU_OK;
        }
        slot = run->base_slot + (uint32_t)r;
        gen = run->generations != NULL ? run->generations[r] : SU_GEN_FIRST;
        if (run->emit_despawn(run->emit_user, slot, *seq, SU_ENTITY_MAKE(slot, gen)) != 0) {
            return SU_E_RESOURCE_LIMIT;
        }
        (*seq)++;
        return SU_OK;
    }
    case SU_OP_EMIT_EVENT:
        if (R[p->d] == 0) {
            return SU_OK;
        }
        if (run->emit_event(run->emit_user, run->base_slot + (uint32_t)r, *seq, p->a, (uint32_t)R[p->b],
                            (uint32_t)R[p->c]) != 0) {
            return SU_E_RESOURCE_LIMIT;
        }
        (*seq)++;
        return SU_OK;

    /* ---- IR 0.3: constants, fields, bool ---- */
    case SU_OP_CONST_I32:
        R[p->d] = su_i32_from_bits(p->a);
        return SU_OK;
    case SU_OP_CONST_I64:
    case SU_OP_CONST_Q32:
        R[p->d] = su_i64_from_bits(((uint64_t)p->b << 32) | (uint64_t)p->a);
        return SU_OK;
    case SU_OP_LOAD_I32:
        R[p->d] = su_i32_from_bits(run->in_cols[p->a][r]);
        return SU_OK;
    case SU_OP_LOAD_I64:
    case SU_OP_LOAD_Q32:
        R[p->d] = col64(run->in_cols, p->a, r);
        return SU_OK;
    case SU_OP_LOAD_V3:
        R[p->d] = col64(run->in_cols, p->a, r);
        R[p->d + 1u] = col64(run->in_cols, p->b, r);
        R[p->d + 2u] = col64(run->in_cols, p->c, r);
        return SU_OK;
    case SU_OP_STORE_I32:
        run->out_cols[p->a][r] = (uint32_t)((uint64_t)R[p->b] & 0xFFFFFFFFu);
        return SU_OK;
    case SU_OP_STORE_I64:
    case SU_OP_STORE_Q32:
        st64(run->out_cols, p->a, r, R[p->b]);
        return SU_OK;
    case SU_OP_STORE_V3:
        st64(run->out_cols, p->a, r, R[p->d]);
        st64(run->out_cols, p->b, r, R[p->d + 1u]);
        st64(run->out_cols, p->c, r, R[p->d + 2u]);
        return SU_OK;
    case SU_OP_AND_BOOL:
        R[p->d] = R[p->a] != 0 && R[p->b] != 0;
        return SU_OK;
    case SU_OP_OR_BOOL:
        R[p->d] = R[p->a] != 0 || R[p->b] != 0;
        return SU_OK;
    case SU_OP_NOT_BOOL:
        R[p->d] = R[p->a] == 0;
        return SU_OK;

    /* ---- i32 family ---- */
    case FAM(SU_OP_I32_BASE, SU_OPX_ADD): R[p->d] = su_i32_add((int32_t)R[p->a], (int32_t)R[p->b]); return SU_OK;
    case FAM(SU_OP_I32_BASE, SU_OPX_SUB): R[p->d] = su_i32_sub((int32_t)R[p->a], (int32_t)R[p->b]); return SU_OK;
    case FAM(SU_OP_I32_BASE, SU_OPX_MUL): R[p->d] = su_i32_mul((int32_t)R[p->a], (int32_t)R[p->b]); return SU_OK;
    case FAM(SU_OP_I32_BASE, SU_OPX_DIV): R[p->d] = su_i32_div((int32_t)R[p->a], (int32_t)R[p->b]); return SU_OK;
    case FAM(SU_OP_I32_BASE, SU_OPX_NEG): R[p->d] = su_i32_neg((int32_t)R[p->a]); return SU_OK;
    case FAM(SU_OP_I32_BASE, SU_OPX_ABS): R[p->d] = su_i32_abs((int32_t)R[p->a]); return SU_OK;
    case FAM(SU_OP_I32_BASE, SU_OPX_SHL): R[p->d] = su_i32_shl((int32_t)R[p->a], (uint32_t)R[p->b]); return SU_OK;
    case FAM(SU_OP_I32_BASE, SU_OPX_SHR): R[p->d] = su_i32_shr((int32_t)R[p->a], (uint32_t)R[p->b]); return SU_OK;
    /* ---- i64 family ---- */
    case FAM(SU_OP_I64_BASE, SU_OPX_ADD): R[p->d] = su_i64_add(R[p->a], R[p->b]); return SU_OK;
    case FAM(SU_OP_I64_BASE, SU_OPX_SUB): R[p->d] = su_i64_sub(R[p->a], R[p->b]); return SU_OK;
    case FAM(SU_OP_I64_BASE, SU_OPX_MUL): R[p->d] = su_i64_mul(R[p->a], R[p->b]); return SU_OK;
    case FAM(SU_OP_I64_BASE, SU_OPX_DIV): R[p->d] = su_i64_div(R[p->a], R[p->b]); return SU_OK;
    case FAM(SU_OP_I64_BASE, SU_OPX_NEG): R[p->d] = su_i64_neg(R[p->a]); return SU_OK;
    case FAM(SU_OP_I64_BASE, SU_OPX_ABS): R[p->d] = su_i64_abs(R[p->a]); return SU_OK;
    case FAM(SU_OP_I64_BASE, SU_OPX_SHL): R[p->d] = su_i64_shl(R[p->a], (uint32_t)R[p->b]); return SU_OK;
    case FAM(SU_OP_I64_BASE, SU_OPX_SHR): R[p->d] = su_i64_shr(R[p->a], (uint32_t)R[p->b]); return SU_OK;
    /* ---- q32 family ---- */
    case FAM(SU_OP_Q32_BASE, SU_OPX_ADD): R[p->d] = su_q32_addf(R[p->a], R[p->b]); return SU_OK;
    case FAM(SU_OP_Q32_BASE, SU_OPX_SUB): R[p->d] = su_q32_subf(R[p->a], R[p->b]); return SU_OK;
    case FAM(SU_OP_Q32_BASE, SU_OPX_MUL): R[p->d] = su_q32_mulf(R[p->a], R[p->b]); return SU_OK;
    case FAM(SU_OP_Q32_BASE, SU_OPX_DIV): R[p->d] = su_q32_divf(R[p->a], R[p->b]); return SU_OK;
    case FAM(SU_OP_Q32_BASE, SU_OPX_NEG): R[p->d] = su_q32_neg(R[p->a]); return SU_OK;
    case FAM(SU_OP_Q32_BASE, SU_OPX_ABS): R[p->d] = su_q32_abs(R[p->a]); return SU_OK;
    case FAM(SU_OP_Q32_BASE, SU_OPX_SHL): R[p->d] = su_q32_shl(R[p->a], (uint32_t)R[p->b]); return SU_OK;
    case FAM(SU_OP_Q32_BASE, SU_OPX_SHR): R[p->d] = su_q32_shr(R[p->a], (uint32_t)R[p->b]); return SU_OK;
    /* ---- family ops shared by the three types (values held as sign-extended int64) ---- */
    case FAM(SU_OP_I32_BASE, SU_OPX_MIN):
    case FAM(SU_OP_I64_BASE, SU_OPX_MIN):
    case FAM(SU_OP_Q32_BASE, SU_OPX_MIN):
        R[p->d] = R[p->a] < R[p->b] ? R[p->a] : R[p->b];
        return SU_OK;
    case FAM(SU_OP_I32_BASE, SU_OPX_MAX):
    case FAM(SU_OP_I64_BASE, SU_OPX_MAX):
    case FAM(SU_OP_Q32_BASE, SU_OPX_MAX):
        R[p->d] = R[p->a] > R[p->b] ? R[p->a] : R[p->b];
        return SU_OK;
    case FAM(SU_OP_I32_BASE, SU_OPX_CLAMP):
    case FAM(SU_OP_I64_BASE, SU_OPX_CLAMP):
    case FAM(SU_OP_Q32_BASE, SU_OPX_CLAMP): {
        int64_t t = R[p->a] > R[p->b] ? R[p->a] : R[p->b];
        R[p->d] = t < R[p->c] ? t : R[p->c];
        return SU_OK;
    }
    case FAM(SU_OP_I32_BASE, SU_OPX_EQ):
    case FAM(SU_OP_I64_BASE, SU_OPX_EQ):
    case FAM(SU_OP_Q32_BASE, SU_OPX_EQ):
        R[p->d] = R[p->a] == R[p->b];
        return SU_OK;
    case FAM(SU_OP_I32_BASE, SU_OPX_LT):
    case FAM(SU_OP_I64_BASE, SU_OPX_LT):
    case FAM(SU_OP_Q32_BASE, SU_OPX_LT):
        R[p->d] = R[p->a] < R[p->b];
        return SU_OK;
    case FAM(SU_OP_I32_BASE, SU_OPX_LE):
    case FAM(SU_OP_I64_BASE, SU_OPX_LE):
    case FAM(SU_OP_Q32_BASE, SU_OPX_LE):
        R[p->d] = R[p->a] <= R[p->b];
        return SU_OK;
    case FAM(SU_OP_I32_BASE, SU_OPX_SELECT):
    case FAM(SU_OP_I64_BASE, SU_OPX_SELECT):
    case FAM(SU_OP_Q32_BASE, SU_OPX_SELECT):
        R[p->d] = R[p->a] != 0 ? R[p->b] : R[p->c];
        return SU_OK;

    /* ---- sqrt, conversions ---- */
    case SU_OP_SQRT_Q32: R[p->d] = su_q32_sqrtf(R[p->a]); return SU_OK;
    case SU_OP_RSQRT_Q32: R[p->d] = su_q32_rsqrtf(R[p->a]); return SU_OK;
    case SU_OP_I32_FROM_U32: R[p->d] = su_i32_from_bits((uint32_t)R[p->a]); return SU_OK;
    case SU_OP_U32_FROM_I32: R[p->d] = (int64_t)(uint32_t)((uint64_t)R[p->a] & 0xFFFFFFFFu); return SU_OK;
    case SU_OP_I64_FROM_I32: R[p->d] = R[p->a]; return SU_OK;
    case SU_OP_Q32_FROM_I32: R[p->d] = su_q32_from_i32((int32_t)R[p->a]); return SU_OK;
    case SU_OP_I32_FROM_I64: R[p->d] = su_i32_from_bits((uint32_t)((uint64_t)R[p->a] & 0xFFFFFFFFu)); return SU_OK;
    case SU_OP_Q32_FROM_I64: R[p->d] = su_q32_from_i64(R[p->a]); return SU_OK;
    case SU_OP_Q32_FROM_RAW: R[p->d] = R[p->a]; return SU_OK;
    case SU_OP_Q32_TO_I32_FLOOR: R[p->d] = su_q32_to_i32_floor(R[p->a]); return SU_OK;
    case SU_OP_Q32_TO_I32_ROUND: R[p->d] = su_q32_to_i32_round(R[p->a]); return SU_OK;
    case SU_OP_Q32_RAW: R[p->d] = R[p->a]; return SU_OK;
    case SU_OP_Q32_TO_I64_FLOOR: R[p->d] = su_q32_to_i64_floor(R[p->a]); return SU_OK;

    /* ---- vec3 ---- */
    case SU_OP_MAKE_V3:
        R[p->d] = R[p->a];
        R[p->d + 1u] = R[p->b];
        R[p->d + 2u] = R[p->c];
        return SU_OK;
    case SU_OP_V3_GET: R[p->d] = R[p->a + p->b]; return SU_OK;
    case SU_OP_V3_ADD: v3_put(R, p->d, su_v3_addf(v3_at(R, p->a), v3_at(R, p->b))); return SU_OK;
    case SU_OP_V3_SUB: v3_put(R, p->d, su_v3_subf(v3_at(R, p->a), v3_at(R, p->b))); return SU_OK;
    case SU_OP_V3_CROSS: v3_put(R, p->d, su_v3_crossf(v3_at(R, p->a), v3_at(R, p->b))); return SU_OK;
    case SU_OP_V3_DOT: R[p->d] = su_v3_dotf(v3_at(R, p->a), v3_at(R, p->b)); return SU_OK;
    case SU_OP_V3_SCALE: v3_put(R, p->d, su_v3_scalef(v3_at(R, p->a), R[p->b])); return SU_OK;
    case SU_OP_V3_CLAMP_LEN: v3_put(R, p->d, su_v3_clamp_lenf(v3_at(R, p->a), R[p->b])); return SU_OK;
    case SU_OP_V3_NEG: v3_put(R, p->d, su_v3_neg(v3_at(R, p->a))); return SU_OK;
    case SU_OP_V3_NORMALIZE: v3_put(R, p->d, su_v3_normalizef(v3_at(R, p->a))); return SU_OK;
    case SU_OP_V3_LEN: R[p->d] = su_v3_lenf(v3_at(R, p->a)); return SU_OK;
    case SU_OP_V3_LEN_SQ: R[p->d] = su_v3_len_sqf(v3_at(R, p->a)); return SU_OK;
    case SU_OP_SELECT_V3: {
        uint32_t s = R[p->a] != 0 ? p->b : p->c;
        R[p->d] = R[s];
        R[p->d + 1u] = R[s + 1u];
        R[p->d + 2u] = R[s + 2u];
        return SU_OK;
    }

    /* ---- quaternion ---- */
    case SU_OP_MAKE_QUAT:
        R[p->d] = R[p->a];
        R[p->d + 1u] = R[p->b];
        R[p->d + 2u] = R[p->b + 1u];
        R[p->d + 3u] = R[p->b + 2u];
        return SU_OK;
    case SU_OP_QUAT_GET: R[p->d] = R[p->a + p->b]; return SU_OK;
    case SU_OP_QUAT_MUL: q_put(R, p->d, su_quat_mulf(q_at(R, p->a), q_at(R, p->b))); return SU_OK;
    case SU_OP_QUAT_CONJ: q_put(R, p->d, su_quat_conjf(q_at(R, p->a))); return SU_OK;
    case SU_OP_QUAT_NORMALIZE: q_put(R, p->d, su_quat_normalizef(q_at(R, p->a))); return SU_OK;
    case SU_OP_QUAT_ROTATE: v3_put(R, p->d, su_quat_rotatef(q_at(R, p->a), v3_at(R, p->b))); return SU_OK;
    case SU_OP_QUAT_INTEGRATE:
        q_put(R, p->d, su_quat_integratef(q_at(R, p->a), v3_at(R, p->b), R[p->c]));
        return SU_OK;
    case SU_OP_SELECT_QUAT: {
        uint32_t s = R[p->a] != 0 ? p->b : p->c;
        R[p->d] = R[s];
        R[p->d + 1u] = R[s + 1u];
        R[p->d + 2u] = R[s + 2u];
        R[p->d + 3u] = R[s + 3u];
        return SU_OK;
    }
    default:
        /* Unreachable: the builder admits only the opcodes above. */
        return SU_E_INTERNAL;
    }
}

int su_vm_plan_bound(const SuVmPlan *p, uint32_t *const *in_cols, uint32_t *const *out_cols) {
    uint32_t i;
    for (i = 0; i < p->n_load; i++) {
        if (in_cols[p->load_lanes[i]] == NULL) {
            return 0;
        }
    }
    for (i = 0; i < p->n_store; i++) {
        if (out_cols[p->store_lanes[i]] == NULL) {
            return 0;
        }
    }
    for (i = 0; i < p->n_identity; i++) {
        if (in_cols[p->identity[i]] == NULL || out_cols[p->identity[i]] == NULL) {
            return 0;
        }
    }
    return 1;
}

uint32_t su_vm_plan_run(SuVmPlan *p, SuVmPlanRun *run, uint64_t rows, uint32_t *failed_insn) {
    int64_t *R = p->regs;
    const uint8_t *alive = run->alive;
    uint64_t r;
    uint64_t body_rows = 0, skipped = 0;
    for (r = 0; r < rows; r++) {
        uint32_t seq = 0;
        uint32_t i;
        if (alive != NULL && alive[r] == 0u) {
            continue;
        }
        if (p->has_guard) {
            int skip = 1;
            for (i = 0; i < p->n_guard; i++) {
                const SuVmPlanInsn *g = &p->guard[i];
                (void)exec1(g, R, run, (size_t)r, &seq); /* pure: cannot fail */
                if (g->check != 0u && (R[g->d] != 0) != (g->check == 2u)) {
                    skip = 0;
                    break;
                }
            }
            if (skip) {
                for (i = 0; i < p->n_identity; i++) {
                    run->out_cols[p->identity[i]][r] = run->in_cols[p->identity[i]][r];
                }
                skipped++;
                continue;
            }
        }
        for (i = 0; i < p->n_body; i++) {
            uint32_t st = exec1(&p->body[i], R, run, (size_t)r, &seq);
            if (st != SU_OK) {
                *failed_insn = p->body[i].idx;
                run->rows_body = body_rows;
                run->rows_skipped = skipped;
                return st;
            }
        }
        body_rows++;
    }
    run->rows_body = body_rows;
    run->rows_skipped = skipped;
    return SU_OK;
}

/* ---- builder ------------------------------------------------------------ */

typedef struct Builder {
    const SuLirModule *m;
    uint32_t n;
    uint32_t *rdef;     /* n * 4: reaching definition (instruction index) of each slot's register, or NONE */
    uint8_t *hoist;     /* n */
    uint8_t *req;       /* n: 0 none, 1 must be false, 2 must be true */
    uint32_t *undo;     /* requirement stack */
    uint32_t nundo;
    uint32_t budget;
    int64_t *regs;
} Builder;

#define RDEF(b, i, slot) ((b)->rdef[(size_t)(i) * 4u + (slot)])

static int is_select(uint32_t op) {
    return op == SU_OP_SELECT_U32 || op == SU_OP_SELECT_V3 || op == SU_OP_SELECT_QUAT ||
           op == FAM(SU_OP_I32_BASE, SU_OPX_SELECT) || op == FAM(SU_OP_I64_BASE, SU_OPX_SELECT) ||
           op == FAM(SU_OP_Q32_BASE, SU_OPX_SELECT);
}

/* Requires definition def (a bool) to have value want (1 false, 2 true). A hoisted definition is a constant. */
static int need(Builder *b, uint32_t def, uint8_t want) {
    if (def == NONE) {
        return 0;
    }
    if (b->hoist[def]) {
        return (b->regs[(size_t)b->m->insns[def].dst * 4u] != 0) == (want == 2u);
    }
    /* Implied values: not(x) is v when x is !v; and(x, y) is false when x or y is already required false;
     * or(x, y) is true when x or y is already required true. */
    switch (b->m->insns[def].op) {
    case SU_OP_NOT_BOOL:
        return need(b, RDEF(b, def, SU_S_A), want == 1u ? 2u : 1u);
    case SU_OP_AND_BOOL:
    case SU_OP_OR_BOOL: {
        uint8_t implied = b->m->insns[def].op == SU_OP_AND_BOOL ? 1u : 2u;
        uint32_t x = RDEF(b, def, SU_S_A), y = RDEF(b, def, SU_S_B);
        if (want == implied && ((x != NONE && b->req[x] == implied) || (y != NONE && b->req[y] == implied))) {
            return 1;
        }
        break;
    }
    default:
        break;
    }
    if (b->req[def] == 0u) {
        b->req[def] = want;
        b->undo[b->nundo++] = def;
        return 1;
    }
    return b->req[def] == want;
}

static void rollback(Builder *b, uint32_t mark) {
    while (b->nundo > mark) {
        b->req[b->undo[--b->nundo]] = 0u;
    }
}

/* Component kinds of a wanted value: 0 scalar, 1 v3 component k, 2 quat component k. */
static int ident(Builder *b, uint32_t def, uint32_t comp_kind, uint32_t k, uint32_t field);

static int ident_select(Builder *b, uint32_t def, uint32_t comp_kind, uint32_t k, uint32_t field) {
    uint32_t mark = b->nundo;
    uint32_t cond = RDEF(b, def, SU_S_A);
    if (need(b, cond, 1u) && ident(b, RDEF(b, def, SU_S_C), comp_kind, k, field)) {
        return 1;
    }
    rollback(b, mark);
    if (need(b, cond, 2u) && ident(b, RDEF(b, def, SU_S_B), comp_kind, k, field)) {
        return 1;
    }
    rollback(b, mark);
    return 0;
}

/* 1 when the value of definition def (component k for v3/quat) equals the loaded content of field `field`
 * (lane-0 table index) under the requirements recorded so far (which this call may extend). */
static int ident(Builder *b, uint32_t def, uint32_t comp_kind, uint32_t k, uint32_t field) {
    const SuInsn *ins;
    if (def == NONE || b->budget == 0u) {
        return 0;
    }
    b->budget--;
    ins = &b->m->insns[def];
    if (is_select(ins->op)) {
        return ident_select(b, def, comp_kind, k, field);
    }
    if (comp_kind == 0u) {
        switch (ins->op) {
        case SU_OP_LOAD_U32:
        case SU_OP_LOAD_I32:
        case SU_OP_LOAD_I64:
        case SU_OP_LOAD_Q32:
            return ins->a == field;
        case SU_OP_V3_GET:
            return ident(b, RDEF(b, def, SU_S_A), 1u, ins->b, field);
        case SU_OP_QUAT_GET:
            return ident(b, RDEF(b, def, SU_S_A), 2u, ins->b, field);
        default:
            return 0;
        }
    }
    if (comp_kind == 1u) {
        switch (ins->op) {
        case SU_OP_LOAD_V3:
            return (k == 0u ? ins->a : (k == 1u ? ins->b : ins->c)) == field;
        case SU_OP_MAKE_V3:
            return ident(b, RDEF(b, def, k == 0u ? SU_S_A : (k == 1u ? SU_S_B : SU_S_C)), 0u, 0u, field);
        default:
            return 0;
        }
    }
    if (ins->op == SU_OP_MAKE_QUAT) {
        return k == 0u ? ident(b, RDEF(b, def, SU_S_A), 0u, 0u, field)
                       : ident(b, RDEF(b, def, SU_S_B), 1u, k - 1u, field);
    }
    return 0;
}

/* Tries to prove every store an identity and every emission off under one set of requirements. */
static int prove_identity(Builder *b) {
    const SuLirModule *m = b->m;
    uint32_t i;
    for (i = 0; i < b->n; i++) {
        const SuInsn *ins = &m->insns[i];
        switch (ins->op) {
        case SU_OP_STORE_U32:
        case SU_OP_STORE_I32:
        case SU_OP_STORE_I64:
        case SU_OP_STORE_Q32:
            if (!ident(b, RDEF(b, i, SU_S_B), 0u, 0u, ins->a)) {
                return 0;
            }
            break;
        case SU_OP_STORE_V3:
            if (!ident(b, RDEF(b, i, SU_S_DST), 1u, 0u, ins->a) || !ident(b, RDEF(b, i, SU_S_DST), 1u, 1u, ins->b) ||
                !ident(b, RDEF(b, i, SU_S_DST), 1u, 2u, ins->c)) {
                return 0;
            }
            break;
        case SU_OP_EMIT_EVENT:
        case SU_OP_DESPAWN_SELF:
            if (!need(b, RDEF(b, i, SU_S_DST), 1u)) {
                return 0;
            }
            break;
        default:
            break;
        }
    }
    return 1;
}

/* Register slots of an instruction pre-scaled to word offsets. */
static void decode(const SuInsn *ins, const Shape *s, SuVmPlanInsn *out, uint32_t idx) {
    uint32_t k;
    out->op = ins->op;
    out->idx = idx;
    out->d = ins->dst;
    out->a = ins->a;
    out->b = ins->b;
    out->c = ins->c;
    out->check = 0u;
    if (s->wtype != 0u) {
        out->d = ins->dst * 4u;
    }
    for (k = 0; k < s->nread; k++) {
        switch (s->slot[k]) {
        case SU_S_DST: out->d = ins->dst * 4u; break;
        case SU_S_A: out->a = ins->a * 4u; break;
        case SU_S_B: out->b = ins->b * 4u; break;
        default: out->c = ins->c * 4u; break;
        }
    }
}

void su_vm_plan_free(SuVmPlan *p) {
    if (p == NULL) {
        return;
    }
    free(p->body);
    free(p->guard);
    free(p->identity);
    free(p->load_lanes);
    free(p->store_lanes);
    free(p->regs);
    free(p);
}

uint32_t su_vm_plan_build(const SuLirModule *m, SuVmPlan **out) {
    Builder b;
    SuVmPlan *p = NULL;
    uint32_t rc = m->register_count, fc = m->field_count, n = m->insn_count;
    uint8_t *tag = NULL, *lane_stored = NULL, *lane_loaded = NULL, *ndef = NULL, *slice = NULL;
    uint32_t *last = NULL;
    uint32_t reason = SU_VM_PLAN_NO_MEMORY;
    uint32_t i, k;
    SuVmPlanRun none;

    *out = NULL;
    memset(&b, 0, sizeof b);
    memset(&none, 0, sizeof none);
    b.m = m;
    b.n = n;
    tag = calloc(rc > 0u ? rc : 1u, 1u);
    ndef = calloc(rc > 0u ? rc : 1u, 1u);
    last = malloc((rc > 0u ? rc : 1u) * sizeof *last);
    lane_stored = calloc(fc > 0u ? fc : 1u, 1u);
    lane_loaded = calloc(fc > 0u ? fc : 1u, 1u);
    b.rdef = malloc(((size_t)(n > 0u ? n : 1u)) * 4u * sizeof *b.rdef);
    b.hoist = calloc(n > 0u ? n : 1u, 1u);
    b.req = calloc(n > 0u ? n : 1u, 1u);
    b.undo = malloc((n > 0u ? n : 1u) * sizeof *b.undo);
    slice = calloc(n > 0u ? n : 1u, 1u);
    p = calloc(1, sizeof *p);
    if (tag == NULL || ndef == NULL || last == NULL || lane_stored == NULL || lane_loaded == NULL || b.rdef == NULL ||
        b.hoist == NULL || b.req == NULL || b.undo == NULL || slice == NULL || p == NULL) {
        goto done;
    }
    for (i = 0; i < rc; i++) {
        last[i] = NONE;
    }

    /* Pass 1: types, reaching definitions, store shape. */
    for (i = 0; i < n; i++) {
        const SuInsn *ins = &m->insns[i];
        Shape s;
        uint32_t lanes[6], nl;
        if (!shape_of(ins->op, &s)) {
            reason = SU_VM_PLAN_UNSUPPORTED_OP;
            goto done;
        }
        for (k = 0; k < 4u; k++) {
            RDEF(&b, i, k) = NONE;
        }
        for (k = 0; k < s.nread; k++) {
            uint32_t reg = slot_value(ins, s.slot[k]);
            if (reg >= rc || tag[reg] != s.type[k]) {
                reason = SU_VM_PLAN_NO_READ_BEFORE_WRITE;
                goto done;
            }
            RDEF(&b, i, s.slot[k]) = last[reg];
        }
        nl = lanes_of(ins, m, lanes);
        for (k = 0; k < nl; k++) {
            if (lanes[k] >= fc) {
                reason = SU_VM_PLAN_STORE_SHAPE;
                goto done;
            }
            if (s.kind == K_STORE) {
                if (lane_stored[lanes[k]] != 0u) {
                    reason = SU_VM_PLAN_STORE_SHAPE;
                    goto done;
                }
                lane_stored[lanes[k]] = 1u;
            } else {
                lane_loaded[lanes[k]] = 1u;
            }
        }
        if (s.wtype != 0u) {
            if (ins->dst >= rc) {
                reason = SU_VM_PLAN_NO_READ_BEFORE_WRITE;
                goto done;
            }
            tag[ins->dst] = (uint8_t)s.wtype;
            if (ndef[ins->dst] < 2u) {
                ndef[ins->dst]++;
            }
            last[ins->dst] = i;
        }
    }
    for (i = 0; i < fc; i++) {
        if ((m->fields[i].access & SU_ACCESS_WRITE) != 0u && lane_stored[i] == 0u) {
            reason = SU_VM_PLAN_STORE_SHAPE;
            goto done;
        }
    }

    /* Pass 2: row-invariant instructions, evaluated now with the same arithmetic. */
    p->regs = calloc(((size_t)(rc > 0u ? rc : 1u)) * 4u, sizeof *p->regs);
    if (p->regs == NULL) {
        goto done;
    }
    b.regs = p->regs;
    for (i = 0; i < n; i++) {
        const SuInsn *ins = &m->insns[i];
        Shape s;
        int inv;
        SuVmPlanInsn d;
        uint32_t seq = 0;
        shape_of(ins->op, &s);
        inv = s.kind == K_PURE && s.wtype != 0u && ndef[ins->dst] == 1u;
        for (k = 0; inv && k < s.nread; k++) {
            uint32_t def = RDEF(&b, i, s.slot[k]);
            inv = def != NONE && b.hoist[def];
        }
        if (inv) {
            decode(ins, &s, &d, i);
            (void)exec1(&d, p->regs, &none, 0u, &seq); /* pure: no columns, no emitter */
            b.hoist[i] = 1u;
            p->hoisted++;
        }
    }

    /* Pass 3: identity-row guard. */
    b.budget = PROOF_BUDGET;
    if (prove_identity(&b)) {
        uint32_t nslice = 0, nbody = n - p->hoisted;
        /* Backward slice of the requirement definitions over reaching definitions (hoisted ones excluded). */
        for (i = n; i-- > 0;) {
            if (b.req[i] != 0u) {
                slice[i] = 1u;
            }
            if (slice[i]) {
                Shape s;
                shape_of(m->insns[i].op, &s);
                for (k = 0; k < s.nread; k++) {
                    uint32_t def = RDEF(&b, i, s.slot[k]);
                    if (def != NONE && !b.hoist[def]) {
                        slice[def] = 1u;
                    }
                }
            }
        }
        for (i = 0; i < n; i++) {
            nslice += slice[i];
        }
        /* Worth it only when the guard is much shorter than the body it saves. */
        if (nslice * 3u <= nbody) {
            p->has_guard = 1u;
            p->n_guard = nslice;
            p->guard = malloc((nslice > 0u ? nslice : 1u) * sizeof *p->guard);
            p->identity = malloc((fc > 0u ? fc : 1u) * sizeof *p->identity);
            if (p->guard == NULL || p->identity == NULL) {
                goto done;
            }
            nslice = 0;
            for (i = 0; i < n; i++) {
                if (slice[i]) {
                    Shape s;
                    shape_of(m->insns[i].op, &s);
                    decode(&m->insns[i], &s, &p->guard[nslice], i);
                    p->guard[nslice].check = b.req[i];
                    if (b.req[i] != 0u) {
                        p->guard_requirements++;
                    }
                    nslice++;
                }
            }
            for (i = 0; i < fc; i++) {
                if ((m->fields[i].access & SU_ACCESS_WRITE) != 0u) {
                    p->identity[p->n_identity++] = i;
                }
            }
        }
    }

    /* Body: every instruction that was not hoisted, in order; lane lists for the per-invocation binding check. */
    p->body = malloc(((n - p->hoisted) > 0u ? (n - p->hoisted) : 1u) * sizeof *p->body);
    p->load_lanes = malloc((fc > 0u ? fc : 1u) * sizeof *p->load_lanes);
    p->store_lanes = malloc((fc > 0u ? fc : 1u) * sizeof *p->store_lanes);
    if (p->body == NULL || p->load_lanes == NULL || p->store_lanes == NULL) {
        goto done;
    }
    for (i = 0; i < n; i++) {
        if (!b.hoist[i]) {
            Shape s;
            shape_of(m->insns[i].op, &s);
            decode(&m->insns[i], &s, &p->body[p->n_body++], i);
        }
    }
    for (i = 0; i < fc; i++) {
        if (lane_loaded[i]) {
            p->load_lanes[p->n_load++] = i;
        }
        if (lane_stored[i]) {
            p->store_lanes[p->n_store++] = i;
        }
    }
    reason = SU_VM_PLAN_OK;

done:
    free(tag);
    free(ndef);
    free(last);
    free(lane_stored);
    free(lane_loaded);
    free(b.rdef);
    free(b.hoist);
    free(b.req);
    free(b.undo);
    free(slice);
    if (reason != SU_VM_PLAN_OK) {
        su_vm_plan_free(p);
        return reason;
    }
    *out = p;
    return SU_VM_PLAN_OK;
}
