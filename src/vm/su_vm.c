/* Checked scalar reference VM (M0-04). See su_vm.h.
 *
 * Arithmetic is unsigned 32-bit (modulo 2^32). Registers carry a type tag
 * (SU_TYPE_U32 or SU_TYPE_BOOL, 0 = never written); every operand read checks
 * the tag, so a module that bypassed the verifier fails with SU_E_INTERNAL
 * instead of computing on the wrong type. Rows run in ascending order; within
 * a row, instructions run in ascending index order.
 *
 * IR 0.3 (numeric profile fixed_q32_v1): i32, i64, q32, v3 and quat registers live in reg_w (four 64-bit
 * words per register, allocated only for 0.3 modules) with the same tag discipline; u32 and bool stay in
 * reg_val. The arithmetic is src/ir/su_num.c. A 64-bit field is two u32 lanes (low word first). */
#include "vm/su_vm.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_num.h"
#include "ir/su_ops03.h"
#include "sutekh/entities.h"
#include "vm/su_vm_plan.h"

#define SU_VM_ACCESS_MASK (SU_ACCESS_READ | SU_ACCESS_WRITE)
#define SU_VM_NO_FAILED_INSN UINT32_MAX

struct SuExecContext {
    const SuLirModule *module;     /* borrowed */
    uint32_t layout_generation;
    uint64_t row_limit;
    uint32_t readable_count;       /* module fields with SU_ACCESS_READ */
    uint32_t writable_count;       /* module fields with SU_ACCESS_WRITE */
    uint32_t last_failed_insn;
    /* IR 0.2 entity view (su_vm_context_set_entity_view). Defaults: all alive, gen 1, base 0. */
    const uint8_t *alive;          /* borrowed; NULL = all rows alive */
    const uint32_t *generations;   /* borrowed; NULL = 1 */
    uint32_t base_slot;
    uint32_t tick_low;
    SuVmEmitter emitter;           /* copied; valid when has_emitter */
    int has_emitter;
    int needs_emitter;             /* module contains despawn_self or emit_event */
    uint32_t row_seq;              /* per-row emission counter, reset for each live row */
    /* Register file, sized from module->register_count. */
    uint32_t *reg_val;
    uint8_t *reg_tag;
    uint64_t *reg_w;               /* IR 0.3 only: 4 words per register (i32/i64/q32 in word 0, v3 x,y,z, quat w,x,y,z) */
    /* Per verified-table index, filled per invocation. */
    uint32_t **in_cols;            /* NULL when not bound */
    uint32_t **out_cols;           /* NULL when not staged */
    uint8_t *bound;                /* wrapper scratch: id matched */
    uint8_t *stored;               /* per row: writable field stored yet */
    /* Writable fields, ascending table order. */
    uint32_t *write_fields;        /* table indices */
    uint32_t *write_ids;           /* field IDs, for commit */
    /* Scratch binding arrays for su_vm_run_system. */
    SuColumnBinding *scr_in;
    SuColumnBinding *scr_out;
    /* Execution tier (su_vm.h "VM tiers"). plan is NULL when the module has none (plan_reason says why). */
    uint32_t tier;
    SuVmPlan *plan;
    uint32_t plan_reason;
    uint32_t last_tier;
    uint64_t last_rows_body;
    uint64_t last_rows_skipped;
    /* IR 0.4 whole-table view for gather_* / find_eq_u32 (su_vm_context_set_table_view). */
    int has_table;
    SuInvocation table;            /* copied; bindings borrowed until replaced */
    uint32_t **gather_cols;        /* per table index, filled per invocation (whole-table columns) */
    uint64_t gather_rows;
    uint32_t *filter_copy;         /* writable lane indices copied on a filtered-out row */
    /* IR 0.4 entity table for handle_live (su_vm_context_set_entity_table), and the one bound per invocation. */
    int has_ents;
    const uint8_t *ents_alive;     /* borrowed; NULL = all slots alive */
    const uint32_t *ents_gen;      /* borrowed; NULL = generation 1 */
    uint64_t ents_slots;
    uint32_t ents_base;            /* world slot of the table's local slot 0 (issue 140; 0 for table 0) */
    const uint8_t *hl_alive;
    const uint32_t *hl_gen;
    uint64_t hl_slots;
    uint32_t hl_base;
};

/* Process default tier for new contexts: 0 = not set (environment SUTEKH_VM_TIER, else planned). */
static uint32_t g_default_tier = 0u;
/* The environment's answer, read once (2026-10-18: su_vm_default_tier is on the per-tick path of the entity
 * runtime, and getenv scans the environment block). 0 = not read yet. Idempotent, so a race between two first
 * reads stores the same value. */
static uint32_t g_env_tier = 0u;

void su_vm_set_default_tier(uint32_t tier) {
    g_default_tier = (tier == SU_VM_TIER_REFERENCE || tier == SU_VM_TIER_PLANNED) ? tier : 0u;
    if (g_default_tier == 0u) {
        g_env_tier = 0u; /* restoring the rule re-reads the environment */
    }
}

uint32_t su_vm_default_tier(void) {
    const char *e;
    if (g_default_tier != 0u) {
        return g_default_tier;
    }
    if (g_env_tier == 0u) {
        e = getenv("SUTEKH_VM_TIER");
        g_env_tier = (e != NULL && strcmp(e, "reference") == 0) ? SU_VM_TIER_REFERENCE : SU_VM_TIER_PLANNED;
    }
    return g_env_tier;
}

/* ---- helpers ---------------------------------------------------------- */

static void *su_vm_calloc(size_t n, size_t sz) {
    return calloc(n > 0 ? n : 1u, sz > 0 ? sz : 1u);
}

/* Records a failure on the staging struct when it is present. The struct is
 * never written through a column pointer. */
static uint32_t fail_out(SuExecContext *ctx, SuOutputs *out, uint32_t st) {
    if (ctx != NULL) {
        ctx->last_failed_insn = SU_VM_NO_FAILED_INSN;
    }
    if (out != NULL) {
        out->failed_insn = SU_VM_NO_FAILED_INSN;
    }
    return st;
}

static int su_reg_ok(uint32_t reg, uint32_t count) {
    return reg < count;
}

static int su_insn03_shape_ok(const SuInsn *ins, const SuLirModule *m);

/* IR 0.2 opcodes are known only in 0.2 modules (minor >= 2); hand-built modules that leave
 * ir_version_minor at 0 are treated as 0.1. */
static int su_insn_known(uint32_t op, uint32_t minor) {
    switch (op) {
    case SU_OP_CONST_U32:
    case SU_OP_LOAD_U32:
    case SU_OP_ADD_U32:
    case SU_OP_SUB_U32:
    case SU_OP_MUL_U32:
    case SU_OP_EQ_U32:
    case SU_OP_LT_U32:
    case SU_OP_SELECT_U32:
    case SU_OP_STORE_U32:
        return 1;
    case SU_OP_SLOT_U32:
    case SU_OP_GEN_U32:
    case SU_OP_TICK_U32:
    case SU_OP_DESPAWN_SELF:
    case SU_OP_EMIT_EVENT:
        return minor >= 2u;
    default: {
        const SuOp03 *o = su_op03_by_code(op);
        return minor >= 3u && o != NULL && su_op_min_minor(o) <= minor;
    }
    }
}

/* Index-range and access checks for one instruction. */
static int su_insn_shape_ok(const SuInsn *ins, const SuLirModule *m) {
    uint32_t rc = m->register_count;
    uint32_t fc = m->field_count;
    if (!su_insn_known(ins->op, m->ir_version_minor)) {
        return 0;
    }
    if (su_op03_by_code(ins->op) != NULL) {
        return su_insn03_shape_ok(ins, m);
    }
    switch (ins->op) {
    case SU_OP_SLOT_U32:
    case SU_OP_GEN_U32:
    case SU_OP_TICK_U32:
        return su_reg_ok(ins->dst, rc);
    case SU_OP_DESPAWN_SELF:
        return su_reg_ok(ins->dst, rc);
    case SU_OP_EMIT_EVENT:
        /* a is the kind constant (never an index); the verifier keeps it below the reserved range. */
        return su_reg_ok(ins->dst, rc) && su_reg_ok(ins->b, rc) && su_reg_ok(ins->c, rc) &&
               ins->a < SU_EVENT_KIND_RESERVED_MIN;
    case SU_OP_CONST_U32:
        return su_reg_ok(ins->dst, rc);
    case SU_OP_LOAD_U32:
        return su_reg_ok(ins->dst, rc) && ins->a < fc &&
               (m->fields[ins->a].access & SU_ACCESS_READ) != 0u;
    case SU_OP_ADD_U32:
    case SU_OP_SUB_U32:
    case SU_OP_MUL_U32:
    case SU_OP_EQ_U32:
    case SU_OP_LT_U32:
        return su_reg_ok(ins->dst, rc) && su_reg_ok(ins->a, rc) && su_reg_ok(ins->b, rc);
    case SU_OP_SELECT_U32:
        return su_reg_ok(ins->dst, rc) && su_reg_ok(ins->a, rc) &&
               su_reg_ok(ins->b, rc) && su_reg_ok(ins->c, rc);
    case SU_OP_STORE_U32:
        return ins->a < fc && (m->fields[ins->a].access & SU_ACCESS_WRITE) != 0u &&
               su_reg_ok(ins->b, rc);
    default:
        return 0;
    }
}

static int su_read_u32(const SuExecContext *c, uint32_t reg, uint32_t *v) {
    if (c->reg_tag[reg] != SU_TYPE_U32) {
        return 0;
    }
    *v = c->reg_val[reg];
    return 1;
}

static int su_read_bool(const SuExecContext *c, uint32_t reg, uint32_t *v) {
    if (c->reg_tag[reg] != SU_TYPE_BOOL) {
        return 0;
    }
    *v = c->reg_val[reg];
    return 1;
}

static void su_write_reg(SuExecContext *c, uint32_t reg, uint32_t type, uint32_t v) {
    c->reg_val[reg] = v;
    c->reg_tag[reg] = (uint8_t)type;
}

/* ---- IR 0.3 (numeric profile fixed_q32_v1) ----------------------------- */

/* A field operand of a 0.3 op: lane-0 index with the value type and access the op needs; a 64-bit type also
 * needs the adjacent lane-1 entry (id + 1, same value type and access). */
static int su_field03_ok(const SuLirModule *m, uint32_t idx, uint32_t vtype, uint32_t access) {
    const SuLirField *f;
    if (idx >= m->field_count) {
        return 0;
    }
    f = &m->fields[idx];
    if ((f->access & access) == 0u || SU_LIR_VALUE_TYPE(f) != vtype || f->lane != 0u) {
        return 0;
    }
    if (su_type_lanes(vtype) == 2u) {
        const SuLirField *h;
        if (idx + 1u >= m->field_count) {
            return 0;
        }
        h = &m->fields[idx + 1u];
        if (h->id != f->id + 1u || SU_LIR_VALUE_TYPE(h) != vtype || h->lane != 1u || h->access != f->access) {
            return 0;
        }
    }
    return 1;
}

static int su_insn03_shape_ok(const SuInsn *ins, const SuLirModule *m) {
    const SuOp03 *op = su_op03_by_code(ins->op);
    uint32_t slot[4];
    uint32_t k;
    if (op == NULL) {
        return 0;
    }
    slot[SU_S_DST] = ins->dst;
    slot[SU_S_A] = ins->a;
    slot[SU_S_B] = ins->b;
    slot[SU_S_C] = ins->c;
    if (op->dst_type != 0u && !su_reg_ok(ins->dst, m->register_count)) {
        return 0;
    }
    for (k = 0; k < op->n; k++) {
        const SuOp03Operand *o = &op->o[k];
        switch (o->kind) {
        case SU_O_REG:
            if (!su_reg_ok(slot[o->slot], m->register_count)) {
                return 0;
            }
            break;
        case SU_O_FREAD:
        case SU_O_FGATHER:
            if (!su_field03_ok(m, slot[o->slot], o->type, SU_ACCESS_READ)) {
                return 0;
            }
            break;
        case SU_O_CSPAN:
            if (slot[o->slot] < 1u || slot[o->slot] > SU_FIND_SPAN_MAX) {
                return 0;
            }
            break;
        case SU_O_FWRITE:
            if (!su_field03_ok(m, slot[o->slot], o->type, SU_ACCESS_WRITE)) {
                return 0;
            }
            break;
        case SU_O_AXIS:
            if (slot[o->slot] >= o->type) {
                return 0;
            }
            break;
        default:
            break;
        }
    }
    return 1;
}

static int su_rd_w(const SuExecContext *c, uint32_t reg, uint32_t type, int64_t *v) {
    if (c->reg_tag[reg] != type) {
        return 0;
    }
    *v = su_i64_from_bits(c->reg_w[(size_t)reg * 4u]);
    return 1;
}

static int su_rd_v3(const SuExecContext *c, uint32_t reg, SuV3 *v) {
    const uint64_t *w = &c->reg_w[(size_t)reg * 4u];
    if (c->reg_tag[reg] != SU_TYPE_V3) {
        return 0;
    }
    v->x = su_i64_from_bits(w[0]);
    v->y = su_i64_from_bits(w[1]);
    v->z = su_i64_from_bits(w[2]);
    return 1;
}

static int su_rd_quat(const SuExecContext *c, uint32_t reg, SuQuat *q) {
    const uint64_t *w = &c->reg_w[(size_t)reg * 4u];
    if (c->reg_tag[reg] != SU_TYPE_QUAT) {
        return 0;
    }
    q->w = su_i64_from_bits(w[0]);
    q->x = su_i64_from_bits(w[1]);
    q->y = su_i64_from_bits(w[2]);
    q->z = su_i64_from_bits(w[3]);
    return 1;
}

static void su_wr_w(SuExecContext *c, uint32_t reg, uint32_t type, int64_t v) {
    c->reg_w[(size_t)reg * 4u] = (uint64_t)v;
    c->reg_tag[reg] = (uint8_t)type;
}

static void su_wr_v3(SuExecContext *c, uint32_t reg, SuV3 v) {
    uint64_t *w = &c->reg_w[(size_t)reg * 4u];
    w[0] = (uint64_t)v.x;
    w[1] = (uint64_t)v.y;
    w[2] = (uint64_t)v.z;
    c->reg_tag[reg] = (uint8_t)SU_TYPE_V3;
}

static void su_wr_quat(SuExecContext *c, uint32_t reg, SuQuat q) {
    uint64_t *w = &c->reg_w[(size_t)reg * 4u];
    w[0] = (uint64_t)q.w;
    w[1] = (uint64_t)q.x;
    w[2] = (uint64_t)q.y;
    w[3] = (uint64_t)q.z;
    c->reg_tag[reg] = (uint8_t)SU_TYPE_QUAT;
}

/* 64-bit value of a two-lane field at row r (lane 0 low word). */
static int64_t su_col64(uint32_t *const *cols, uint32_t idx, size_t r) {
    return su_i64_from_bits(((uint64_t)cols[idx + 1u][r] << 32) | (uint64_t)cols[idx][r]);
}

/* Stages a 64-bit store into the two lanes; SU_E_INTERNAL when unbound or already stored this row. */
static uint32_t su_store64(SuExecContext *ctx, uint32_t idx, size_t r, int64_t v) {
    uint32_t *lo = ctx->out_cols[idx];
    uint32_t *hi = ctx->out_cols[idx + 1u];
    if (lo == NULL || hi == NULL || ctx->stored[idx] != 0u || ctx->stored[idx + 1u] != 0u) {
        return SU_E_INTERNAL;
    }
    lo[r] = (uint32_t)((uint64_t)v & 0xFFFFFFFFu);
    hi[r] = (uint32_t)((uint64_t)v >> 32);
    ctx->stored[idx] = 1u;
    ctx->stored[idx + 1u] = 1u;
    return SU_OK;
}

/* One scalar-family op (i32, i64 or q32). x, y, z are the operands already read with the family type. */
static int64_t su_family(uint32_t type, uint32_t off, int64_t x, int64_t y, int64_t z) {
    if (type == SU_TYPE_I32) {
        int32_t a = (int32_t)x, b = (int32_t)y;
        switch (off) {
        case SU_OPX_ADD: return su_i32_add(a, b);
        case SU_OPX_SUB: return su_i32_sub(a, b);
        case SU_OPX_MUL: return su_i32_mul(a, b);
        case SU_OPX_DIV: return su_i32_div(a, b);
        case SU_OPX_NEG: return su_i32_neg(a);
        case SU_OPX_ABS: return su_i32_abs(a);
        default: break;
        }
    } else if (type == SU_TYPE_I64) {
        switch (off) {
        case SU_OPX_ADD: return su_i64_add(x, y);
        case SU_OPX_SUB: return su_i64_sub(x, y);
        case SU_OPX_MUL: return su_i64_mul(x, y);
        case SU_OPX_DIV: return su_i64_div(x, y);
        case SU_OPX_NEG: return su_i64_neg(x);
        case SU_OPX_ABS: return su_i64_abs(x);
        default: break;
        }
    } else {
        switch (off) {
        case SU_OPX_ADD: return su_q32_add(x, y);
        case SU_OPX_SUB: return su_q32_sub(x, y);
        case SU_OPX_MUL: return su_q32_mul(x, y);
        case SU_OPX_DIV: return su_q32_div(x, y);
        case SU_OPX_NEG: return su_q32_neg(x);
        case SU_OPX_ABS: return su_q32_abs(x);
        default: break;
        }
    }
    switch (off) {
    case SU_OPX_MIN: return x < y ? x : y;
    case SU_OPX_MAX: return x > y ? x : y;
    case SU_OPX_CLAMP: {
        int64_t t = x > y ? x : y; /* max(x, lo) */
        return t < z ? t : z;      /* min(., hi) */
    }
    default: return 0;
    }
}

/* Shift of a family value by a u32 count. */
static int64_t su_family_shift(uint32_t type, int left, int64_t x, uint32_t n) {
    if (type == SU_TYPE_I32) {
        return left ? su_i32_shl((int32_t)x, n) : su_i32_shr((int32_t)x, n);
    }
    if (type == SU_TYPE_I64) {
        return left ? su_i64_shl(x, n) : su_i64_shr(x, n);
    }
    return left ? su_q32_shl(x, n) : su_q32_shr(x, n);
}

/* Executes one 0.3 instruction for row r. Operand types were checked by the verifier; every read checks the
 * register tag again (SU_E_INTERNAL on a module that bypassed the verifier). */
static uint32_t su_exec_insn03(SuExecContext *ctx, const SuInsn *ins, size_t r) {
    int64_t x = 0, y = 0, z = 0;
    uint32_t u = 0, bb = 0;
    SuV3 va, vb;
    SuQuat qa, qb;
    uint32_t op = ins->op;

    switch (op) {
    case SU_OP_AND_U32:
    case SU_OP_OR_U32:
    case SU_OP_XOR_U32:
    case SU_OP_SHL_U32:
    case SU_OP_SHR_U32:
    case SU_OP_DIV_U32:
    case SU_OP_REM_U32: {
        uint32_t ua = 0, ub = 0, ur = 0;
        if (!su_read_u32(ctx, ins->a, &ua) || !su_read_u32(ctx, ins->b, &ub)) {
            return SU_E_INTERNAL;
        }
        switch (op) {
        case SU_OP_AND_U32: ur = ua & ub; break;
        case SU_OP_OR_U32: ur = ua | ub; break;
        case SU_OP_XOR_U32: ur = ua ^ ub; break;
        case SU_OP_SHL_U32: ur = su_u32_shl(ua, ub); break;
        case SU_OP_SHR_U32: ur = su_u32_shr(ua, ub); break;
        case SU_OP_DIV_U32: ur = su_u32_div(ua, ub); break;
        default: ur = su_u32_rem(ua, ub); break;
        }
        su_write_reg(ctx, ins->dst, SU_TYPE_U32, ur);
        return SU_OK;
    }
    case SU_OP_NOT_U32:
        if (!su_read_u32(ctx, ins->a, &u)) {
            return SU_E_INTERNAL;
        }
        su_write_reg(ctx, ins->dst, SU_TYPE_U32, ~u);
        return SU_OK;
    case SU_OP_AND_I64:
    case SU_OP_OR_I64:
    case SU_OP_XOR_I64:
        if (!su_rd_w(ctx, ins->a, SU_TYPE_I64, &x) || !su_rd_w(ctx, ins->b, SU_TYPE_I64, &y)) {
            return SU_E_INTERNAL;
        }
        z = su_i64_from_bits(op == SU_OP_AND_I64   ? ((uint64_t)x & (uint64_t)y)
                             : op == SU_OP_OR_I64 ? ((uint64_t)x | (uint64_t)y)
                                                  : ((uint64_t)x ^ (uint64_t)y));
        su_wr_w(ctx, ins->dst, SU_TYPE_I64, z);
        return SU_OK;
    case SU_OP_NOT_I64:
        if (!su_rd_w(ctx, ins->a, SU_TYPE_I64, &x)) {
            return SU_E_INTERNAL;
        }
        su_wr_w(ctx, ins->dst, SU_TYPE_I64, su_i64_from_bits(~(uint64_t)x));
        return SU_OK;
    case SU_OP_GATHER_U32:
    case SU_OP_GATHER_I32:
    case SU_OP_GATHER_I64:
    case SU_OP_GATHER_Q32: {
        uint32_t s = 0;
        int two = op == SU_OP_GATHER_I64 || op == SU_OP_GATHER_Q32;
        uint64_t bits = 0;
        if (!su_read_u32(ctx, ins->b, &s) || ctx->gather_cols[ins->a] == NULL ||
            (two && ctx->gather_cols[ins->a + 1u] == NULL)) {
            return SU_E_INTERNAL;
        }
        if ((uint64_t)s < ctx->gather_rows) {
            bits = ctx->gather_cols[ins->a][s];
            if (two) {
                bits |= (uint64_t)ctx->gather_cols[ins->a + 1u][s] << 32;
            }
        }
        if (op == SU_OP_GATHER_U32) {
            su_write_reg(ctx, ins->dst, SU_TYPE_U32, (uint32_t)bits);
        } else if (op == SU_OP_GATHER_I32) {
            su_wr_w(ctx, ins->dst, SU_TYPE_I32, su_i32_from_bits((uint32_t)bits));
        } else {
            su_wr_w(ctx, ins->dst, op == SU_OP_GATHER_I64 ? SU_TYPE_I64 : SU_TYPE_Q32, su_i64_from_bits(bits));
        }
        return SU_OK;
    }
    case SU_OP_HANDLE_LIVE: {
        uint32_t g = 0;
        if (!su_read_u32(ctx, ins->a, &u) || !su_read_u32(ctx, ins->b, &g)) {
            return SU_E_INTERNAL;
        }
        su_write_reg(ctx, ins->dst, SU_TYPE_BOOL,
                     su_handle_live_base(ctx->hl_alive, ctx->hl_gen, ctx->hl_slots, ctx->hl_base, u, g));
        return SU_OK;
    }
    case SU_OP_FIND_EQ_U32: {
        uint64_t n = ins->c, s;
        uint32_t found = SU_FIND_NONE;
        const uint32_t *col = ctx->gather_cols[ins->a];
        if (!su_read_u32(ctx, ins->b, &u) || col == NULL) {
            return SU_E_INTERNAL;
        }
        if (n > ctx->gather_rows) {
            n = ctx->gather_rows;
        }
        for (s = 0; s < n; s++) {
            if (col[s] == u) {
                found = (uint32_t)s;
                break;
            }
        }
        su_write_reg(ctx, ins->dst, SU_TYPE_U32, found);
        return SU_OK;
    }
    default:
        break;
    }

    if (op >= SU_OP_I32_BASE && op < SU_OP_Q32_BASE + SU_OPX_COUNT &&
        (op - SU_OP_I32_BASE) % 32u < SU_OPX_COUNT) {
        uint32_t base = op >= SU_OP_Q32_BASE ? SU_OP_Q32_BASE : (op >= SU_OP_I64_BASE ? SU_OP_I64_BASE : SU_OP_I32_BASE);
        uint32_t type = base == SU_OP_Q32_BASE ? SU_TYPE_Q32 : (base == SU_OP_I64_BASE ? SU_TYPE_I64 : SU_TYPE_I32);
        uint32_t off = op - base;
        switch (off) {
        case SU_OPX_NEG:
        case SU_OPX_ABS:
            if (!su_rd_w(ctx, ins->a, type, &x)) {
                return SU_E_INTERNAL;
            }
            su_wr_w(ctx, ins->dst, type, su_family(type, off, x, 0, 0));
            return SU_OK;
        case SU_OPX_CLAMP:
            if (!su_rd_w(ctx, ins->a, type, &x) || !su_rd_w(ctx, ins->b, type, &y) ||
                !su_rd_w(ctx, ins->c, type, &z)) {
                return SU_E_INTERNAL;
            }
            su_wr_w(ctx, ins->dst, type, su_family(type, off, x, y, z));
            return SU_OK;
        case SU_OPX_EQ:
        case SU_OPX_LT:
        case SU_OPX_LE:
            if (!su_rd_w(ctx, ins->a, type, &x) || !su_rd_w(ctx, ins->b, type, &y)) {
                return SU_E_INTERNAL;
            }
            su_write_reg(ctx, ins->dst, SU_TYPE_BOOL,
                         off == SU_OPX_EQ ? (x == y) : (off == SU_OPX_LT ? (x < y) : (x <= y)));
            return SU_OK;
        case SU_OPX_SELECT:
            if (!su_read_bool(ctx, ins->a, &bb) || !su_rd_w(ctx, ins->b, type, &x) ||
                !su_rd_w(ctx, ins->c, type, &y)) {
                return SU_E_INTERNAL;
            }
            su_wr_w(ctx, ins->dst, type, bb != 0u ? x : y);
            return SU_OK;
        case SU_OPX_SHL:
        case SU_OPX_SHR:
            if (!su_rd_w(ctx, ins->a, type, &x) || !su_read_u32(ctx, ins->b, &u)) {
                return SU_E_INTERNAL;
            }
            su_wr_w(ctx, ins->dst, type, su_family_shift(type, off == SU_OPX_SHL, x, u));
            return SU_OK;
        default:
            if (!su_rd_w(ctx, ins->a, type, &x) || !su_rd_w(ctx, ins->b, type, &y)) {
                return SU_E_INTERNAL;
            }
            su_wr_w(ctx, ins->dst, type, su_family(type, off, x, y, 0));
            return SU_OK;
        }
    }

    switch (op) {
    case SU_OP_CONST_I32:
        su_wr_w(ctx, ins->dst, SU_TYPE_I32, su_i32_from_bits(ins->a));
        return SU_OK;
    case SU_OP_CONST_I64:
    case SU_OP_CONST_Q32:
        su_wr_w(ctx, ins->dst, op == SU_OP_CONST_I64 ? SU_TYPE_I64 : SU_TYPE_Q32,
                su_i64_from_bits(((uint64_t)ins->b << 32) | (uint64_t)ins->a));
        return SU_OK;
    case SU_OP_LOAD_I32:
        if (ctx->in_cols[ins->a] == NULL) {
            return SU_E_INTERNAL;
        }
        su_wr_w(ctx, ins->dst, SU_TYPE_I32, su_i32_from_bits(ctx->in_cols[ins->a][r]));
        return SU_OK;
    case SU_OP_LOAD_I64:
    case SU_OP_LOAD_Q32:
        if (ctx->in_cols[ins->a] == NULL || ctx->in_cols[ins->a + 1u] == NULL) {
            return SU_E_INTERNAL;
        }
        su_wr_w(ctx, ins->dst, op == SU_OP_LOAD_I64 ? SU_TYPE_I64 : SU_TYPE_Q32, su_col64(ctx->in_cols, ins->a, r));
        return SU_OK;
    case SU_OP_LOAD_V3:
        if (ctx->in_cols[ins->a] == NULL || ctx->in_cols[ins->a + 1u] == NULL || ctx->in_cols[ins->b] == NULL ||
            ctx->in_cols[ins->b + 1u] == NULL || ctx->in_cols[ins->c] == NULL || ctx->in_cols[ins->c + 1u] == NULL) {
            return SU_E_INTERNAL;
        }
        va.x = su_col64(ctx->in_cols, ins->a, r);
        va.y = su_col64(ctx->in_cols, ins->b, r);
        va.z = su_col64(ctx->in_cols, ins->c, r);
        su_wr_v3(ctx, ins->dst, va);
        return SU_OK;
    case SU_OP_STORE_I32: {
        uint32_t *col = ctx->out_cols[ins->a];
        if (col == NULL || ctx->stored[ins->a] != 0u || !su_rd_w(ctx, ins->b, SU_TYPE_I32, &x)) {
            return SU_E_INTERNAL;
        }
        col[r] = (uint32_t)((uint64_t)x & 0xFFFFFFFFu);
        ctx->stored[ins->a] = 1u;
        return SU_OK;
    }
    case SU_OP_STORE_I64:
    case SU_OP_STORE_Q32:
        if (!su_rd_w(ctx, ins->b, op == SU_OP_STORE_I64 ? SU_TYPE_I64 : SU_TYPE_Q32, &x)) {
            return SU_E_INTERNAL;
        }
        return su_store64(ctx, ins->a, r, x);
    case SU_OP_STORE_V3: {
        uint32_t st;
        if (!su_rd_v3(ctx, ins->dst, &va)) {
            return SU_E_INTERNAL;
        }
        st = su_store64(ctx, ins->a, r, va.x);
        if (st == SU_OK) {
            st = su_store64(ctx, ins->b, r, va.y);
        }
        if (st == SU_OK) {
            st = su_store64(ctx, ins->c, r, va.z);
        }
        return st;
    }
    case SU_OP_AND_BOOL:
    case SU_OP_OR_BOOL:
        if (!su_read_bool(ctx, ins->a, &u) || !su_read_bool(ctx, ins->b, &bb)) {
            return SU_E_INTERNAL;
        }
        su_write_reg(ctx, ins->dst, SU_TYPE_BOOL, op == SU_OP_AND_BOOL ? (u != 0u && bb != 0u) : (u != 0u || bb != 0u));
        return SU_OK;
    case SU_OP_NOT_BOOL:
        if (!su_read_bool(ctx, ins->a, &u)) {
            return SU_E_INTERNAL;
        }
        su_write_reg(ctx, ins->dst, SU_TYPE_BOOL, u == 0u);
        return SU_OK;
    case SU_OP_SQRT_Q32:
    case SU_OP_RSQRT_Q32:
        if (!su_rd_w(ctx, ins->a, SU_TYPE_Q32, &x)) {
            return SU_E_INTERNAL;
        }
        su_wr_w(ctx, ins->dst, SU_TYPE_Q32, op == SU_OP_SQRT_Q32 ? su_q32_sqrt(x) : su_q32_rsqrt(x));
        return SU_OK;
    case SU_OP_I32_FROM_U32:
        if (!su_read_u32(ctx, ins->a, &u)) {
            return SU_E_INTERNAL;
        }
        su_wr_w(ctx, ins->dst, SU_TYPE_I32, su_i32_from_bits(u));
        return SU_OK;
    case SU_OP_U32_FROM_I32:
        if (!su_rd_w(ctx, ins->a, SU_TYPE_I32, &x)) {
            return SU_E_INTERNAL;
        }
        su_write_reg(ctx, ins->dst, SU_TYPE_U32, (uint32_t)((uint64_t)x & 0xFFFFFFFFu));
        return SU_OK;
    case SU_OP_I64_FROM_I32:
    case SU_OP_Q32_FROM_I32:
        if (!su_rd_w(ctx, ins->a, SU_TYPE_I32, &x)) {
            return SU_E_INTERNAL;
        }
        su_wr_w(ctx, ins->dst, op == SU_OP_I64_FROM_I32 ? SU_TYPE_I64 : SU_TYPE_Q32,
                op == SU_OP_I64_FROM_I32 ? x : su_q32_from_i32((int32_t)x));
        return SU_OK;
    case SU_OP_I32_FROM_I64:
        if (!su_rd_w(ctx, ins->a, SU_TYPE_I64, &x)) {
            return SU_E_INTERNAL;
        }
        su_wr_w(ctx, ins->dst, SU_TYPE_I32, su_i32_from_bits((uint32_t)((uint64_t)x & 0xFFFFFFFFu)));
        return SU_OK;
    case SU_OP_Q32_FROM_I64:
    case SU_OP_Q32_FROM_RAW:
        if (!su_rd_w(ctx, ins->a, SU_TYPE_I64, &x)) {
            return SU_E_INTERNAL;
        }
        su_wr_w(ctx, ins->dst, SU_TYPE_Q32, op == SU_OP_Q32_FROM_I64 ? su_q32_from_i64(x) : x);
        return SU_OK;
    case SU_OP_Q32_TO_I32_FLOOR:
    case SU_OP_Q32_TO_I32_ROUND:
        if (!su_rd_w(ctx, ins->a, SU_TYPE_Q32, &x)) {
            return SU_E_INTERNAL;
        }
        su_wr_w(ctx, ins->dst, SU_TYPE_I32, op == SU_OP_Q32_TO_I32_FLOOR ? su_q32_to_i32_floor(x) : su_q32_to_i32_round(x));
        return SU_OK;
    case SU_OP_Q32_RAW:
    case SU_OP_Q32_TO_I64_FLOOR:
        if (!su_rd_w(ctx, ins->a, SU_TYPE_Q32, &x)) {
            return SU_E_INTERNAL;
        }
        su_wr_w(ctx, ins->dst, SU_TYPE_I64, op == SU_OP_Q32_RAW ? x : su_q32_to_i64_floor(x));
        return SU_OK;
    case SU_OP_MAKE_V3:
        if (!su_rd_w(ctx, ins->a, SU_TYPE_Q32, &va.x) || !su_rd_w(ctx, ins->b, SU_TYPE_Q32, &va.y) ||
            !su_rd_w(ctx, ins->c, SU_TYPE_Q32, &va.z)) {
            return SU_E_INTERNAL;
        }
        su_wr_v3(ctx, ins->dst, va);
        return SU_OK;
    case SU_OP_V3_GET:
        if (!su_rd_v3(ctx, ins->a, &va) || ins->b > 2u) {
            return SU_E_INTERNAL;
        }
        su_wr_w(ctx, ins->dst, SU_TYPE_Q32, ins->b == 0u ? va.x : (ins->b == 1u ? va.y : va.z));
        return SU_OK;
    case SU_OP_V3_ADD:
    case SU_OP_V3_SUB:
    case SU_OP_V3_CROSS:
        if (!su_rd_v3(ctx, ins->a, &va) || !su_rd_v3(ctx, ins->b, &vb)) {
            return SU_E_INTERNAL;
        }
        su_wr_v3(ctx, ins->dst, op == SU_OP_V3_ADD ? su_v3_add(va, vb)
                                : (op == SU_OP_V3_SUB ? su_v3_sub(va, vb) : su_v3_cross(va, vb)));
        return SU_OK;
    case SU_OP_V3_DOT:
        if (!su_rd_v3(ctx, ins->a, &va) || !su_rd_v3(ctx, ins->b, &vb)) {
            return SU_E_INTERNAL;
        }
        su_wr_w(ctx, ins->dst, SU_TYPE_Q32, su_v3_dot(va, vb));
        return SU_OK;
    case SU_OP_V3_SCALE:
    case SU_OP_V3_CLAMP_LEN:
        if (!su_rd_v3(ctx, ins->a, &va) || !su_rd_w(ctx, ins->b, SU_TYPE_Q32, &x)) {
            return SU_E_INTERNAL;
        }
        su_wr_v3(ctx, ins->dst, op == SU_OP_V3_SCALE ? su_v3_scale(va, x) : su_v3_clamp_len(va, x));
        return SU_OK;
    case SU_OP_V3_NEG:
    case SU_OP_V3_NORMALIZE:
        if (!su_rd_v3(ctx, ins->a, &va)) {
            return SU_E_INTERNAL;
        }
        su_wr_v3(ctx, ins->dst, op == SU_OP_V3_NEG ? su_v3_neg(va) : su_v3_normalize(va));
        return SU_OK;
    case SU_OP_V3_LEN:
    case SU_OP_V3_LEN_SQ:
        if (!su_rd_v3(ctx, ins->a, &va)) {
            return SU_E_INTERNAL;
        }
        su_wr_w(ctx, ins->dst, SU_TYPE_Q32, op == SU_OP_V3_LEN ? su_v3_len(va) : su_v3_len_sq(va));
        return SU_OK;
    case SU_OP_SELECT_V3:
        if (!su_read_bool(ctx, ins->a, &bb) || !su_rd_v3(ctx, ins->b, &va) || !su_rd_v3(ctx, ins->c, &vb)) {
            return SU_E_INTERNAL;
        }
        su_wr_v3(ctx, ins->dst, bb != 0u ? va : vb);
        return SU_OK;
    case SU_OP_MAKE_QUAT:
        if (!su_rd_w(ctx, ins->a, SU_TYPE_Q32, &qa.w) || !su_rd_v3(ctx, ins->b, &va)) {
            return SU_E_INTERNAL;
        }
        qa.x = va.x;
        qa.y = va.y;
        qa.z = va.z;
        su_wr_quat(ctx, ins->dst, qa);
        return SU_OK;
    case SU_OP_QUAT_GET:
        if (!su_rd_quat(ctx, ins->a, &qa) || ins->b > 3u) {
            return SU_E_INTERNAL;
        }
        su_wr_w(ctx, ins->dst, SU_TYPE_Q32, ins->b == 0u ? qa.w : (ins->b == 1u ? qa.x : (ins->b == 2u ? qa.y : qa.z)));
        return SU_OK;
    case SU_OP_QUAT_MUL:
        if (!su_rd_quat(ctx, ins->a, &qa) || !su_rd_quat(ctx, ins->b, &qb)) {
            return SU_E_INTERNAL;
        }
        su_wr_quat(ctx, ins->dst, su_quat_mul(qa, qb));
        return SU_OK;
    case SU_OP_QUAT_CONJ:
    case SU_OP_QUAT_NORMALIZE:
        if (!su_rd_quat(ctx, ins->a, &qa)) {
            return SU_E_INTERNAL;
        }
        su_wr_quat(ctx, ins->dst, op == SU_OP_QUAT_CONJ ? su_quat_conj(qa) : su_quat_normalize(qa));
        return SU_OK;
    case SU_OP_QUAT_ROTATE:
        if (!su_rd_quat(ctx, ins->a, &qa) || !su_rd_v3(ctx, ins->b, &va)) {
            return SU_E_INTERNAL;
        }
        su_wr_v3(ctx, ins->dst, su_quat_rotate(qa, va));
        return SU_OK;
    case SU_OP_QUAT_INTEGRATE:
        if (!su_rd_quat(ctx, ins->a, &qa) || !su_rd_v3(ctx, ins->b, &va) || !su_rd_w(ctx, ins->c, SU_TYPE_Q32, &x)) {
            return SU_E_INTERNAL;
        }
        su_wr_quat(ctx, ins->dst, su_quat_integrate(qa, va, x));
        return SU_OK;
    case SU_OP_SELECT_QUAT:
        if (!su_read_bool(ctx, ins->a, &bb) || !su_rd_quat(ctx, ins->b, &qa) || !su_rd_quat(ctx, ins->c, &qb)) {
            return SU_E_INTERNAL;
        }
        su_wr_quat(ctx, ins->dst, bb != 0u ? qa : qb);
        return SU_OK;
    default:
        return SU_E_INTERNAL;
    }
}

/* ---- lifetime --------------------------------------------------------- */

void su_vm_context_destroy(SuExecContext *ctx) {
    if (ctx == NULL) {
        return;
    }
    free(ctx->reg_val);
    free(ctx->reg_tag);
    free(ctx->reg_w);
    free(ctx->in_cols);
    free(ctx->out_cols);
    free(ctx->bound);
    free(ctx->stored);
    free(ctx->write_fields);
    free(ctx->write_ids);
    free(ctx->scr_in);
    free(ctx->scr_out);
    free(ctx->gather_cols);
    free(ctx->filter_copy);
    su_vm_plan_free(ctx->plan);
    free(ctx);
}

uint32_t su_vm_context_create(const SuLirModule *m, uint32_t layout_generation,
                              SuExecContext **out) {
    SuExecContext *c;
    uint32_t i;
    uint32_t nw = 0;

    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (m == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (m->field_count > 0 && m->fields == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (m->insn_count > 0 && m->insns == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (m->register_count > SU_LIMIT_REGISTERS || m->field_count > SU_LIMIT_FIELDS ||
        m->insn_count > SU_LIMIT_INSTRUCTIONS) {
        return SU_E_RESOURCE_LIMIT;
    }
    for (i = 0; i < m->field_count; i++) {
        uint32_t acc = m->fields[i].access;
        if (acc == 0u || (acc & ~SU_VM_ACCESS_MASK) != 0u || m->fields[i].type != SU_TYPE_U32) {
            return SU_E_INVALID_ARGUMENT;
        }
        if ((acc & SU_ACCESS_WRITE) != 0u) {
            nw++;
        }
    }
    for (i = 0; i < m->insn_count; i++) {
        if (!su_insn_shape_ok(&m->insns[i], m)) {
            return SU_E_INVALID_ARGUMENT;
        }
    }
    /* IR 0.4 row filter of a hand-built module: a readable u32 lane-0 field, and every writable lane readable. */
    if (m->has_row_filter) {
        if (m->ir_version_minor < 4u || !su_field03_ok(m, m->row_filter_field, SU_TYPE_U32, SU_ACCESS_READ)) {
            return SU_E_INVALID_ARGUMENT;
        }
        for (i = 0; i < m->field_count; i++) {
            if ((m->fields[i].access & SU_ACCESS_WRITE) != 0u && (m->fields[i].access & SU_ACCESS_READ) == 0u) {
                return SU_E_INVALID_ARGUMENT;
            }
        }
    }

    c = calloc(1, sizeof *c);
    if (c == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    c->module = m;
    c->layout_generation = layout_generation;
    c->row_limit = (m->max_rows != 0 && m->max_rows < SU_LIMIT_ROWS) ? m->max_rows : SU_LIMIT_ROWS;
    c->last_failed_insn = SU_VM_NO_FAILED_INSN;
    for (i = 0; i < m->insn_count; i++) {
        if (m->insns[i].op == SU_OP_DESPAWN_SELF || m->insns[i].op == SU_OP_EMIT_EVENT) {
            c->needs_emitter = 1;
        }
    }

    c->reg_val = su_vm_calloc(m->register_count, sizeof *c->reg_val);
    c->reg_tag = su_vm_calloc(m->register_count, sizeof *c->reg_tag);
    c->in_cols = su_vm_calloc(m->field_count, sizeof *c->in_cols);
    c->out_cols = su_vm_calloc(m->field_count, sizeof *c->out_cols);
    c->bound = su_vm_calloc(m->field_count, sizeof *c->bound);
    c->stored = su_vm_calloc(m->field_count, sizeof *c->stored);
    c->write_fields = su_vm_calloc(nw, sizeof *c->write_fields);
    c->write_ids = su_vm_calloc(nw, sizeof *c->write_ids);
    c->gather_cols = su_vm_calloc(m->field_count, sizeof *c->gather_cols);
    c->filter_copy = su_vm_calloc(nw, sizeof *c->filter_copy);
    if (m->ir_version_minor >= 3u) {
        c->reg_w = su_vm_calloc((size_t)m->register_count * 4u, sizeof *c->reg_w);
    }
    if (c->reg_val == NULL || c->reg_tag == NULL || c->in_cols == NULL || c->out_cols == NULL ||
        c->bound == NULL || c->stored == NULL || c->write_fields == NULL || c->write_ids == NULL ||
        c->gather_cols == NULL || c->filter_copy == NULL ||
        (m->ir_version_minor >= 3u && c->reg_w == NULL)) {
        su_vm_context_destroy(c);
        return SU_E_RESOURCE_LIMIT;
    }

    /* Verified table is ascending by id; writable subset keeps that order. */
    for (i = 0; i < m->field_count; i++) {
        uint32_t acc = m->fields[i].access;
        if ((acc & SU_ACCESS_READ) != 0u) {
            c->readable_count++;
        }
        if ((acc & SU_ACCESS_WRITE) != 0u) {
            c->write_fields[c->writable_count] = i;
            c->write_ids[c->writable_count] = m->fields[i].id;
            c->writable_count++;
        }
    }

    c->scr_in = su_vm_calloc(c->readable_count, sizeof *c->scr_in);
    c->scr_out = su_vm_calloc(c->writable_count, sizeof *c->scr_out);
    if (c->scr_in == NULL || c->scr_out == NULL) {
        su_vm_context_destroy(c);
        return SU_E_RESOURCE_LIMIT;
    }
    c->tier = su_vm_default_tier();
    c->plan_reason = su_vm_plan_build(m, &c->plan);
    if (c->plan_reason == SU_VM_PLAN_NO_MEMORY) {
        su_vm_context_destroy(c);
        return SU_E_RESOURCE_LIMIT;
    }

    *out = c;
    return SU_OK;
}

uint32_t su_vm_context_set_entity_view(SuExecContext *ctx, const uint8_t *alive,
                                       const uint32_t *generations, uint32_t base_slot,
                                       uint32_t tick_low, const SuVmEmitter *emitter) {
    if (ctx == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (emitter != NULL && (emitter->emit_despawn == NULL || emitter->emit_event == NULL)) {
        return SU_E_INVALID_ARGUMENT;
    }
    ctx->alive = alive;
    ctx->generations = generations;
    ctx->base_slot = base_slot;
    ctx->tick_low = tick_low;
    ctx->has_emitter = emitter != NULL;
    if (emitter != NULL) {
        ctx->emitter = *emitter;
    } else {
        memset(&ctx->emitter, 0, sizeof ctx->emitter);
    }
    return SU_OK;
}

uint32_t su_vm_context_set_table_view(SuExecContext *ctx, const SuInvocation *table) {
    if (ctx == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (table == NULL) {
        ctx->has_table = 0;
        memset(&ctx->table, 0, sizeof ctx->table);
        return SU_OK;
    }
    ctx->table = *table;
    ctx->has_table = 1;
    return SU_OK;
}

uint32_t su_vm_context_set_entity_table(SuExecContext *ctx, const uint8_t *alive, const uint32_t *generations,
                                        uint64_t slot_count) {
    return su_vm_context_set_entity_table_base(ctx, alive, generations, slot_count, 0u);
}

uint32_t su_vm_context_set_entity_table_base(SuExecContext *ctx, const uint8_t *alive, const uint32_t *generations,
                                             uint64_t slot_count, uint32_t base_slot) {
    if (ctx == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    ctx->has_ents = !(alive == NULL && generations == NULL && slot_count == 0u);
    ctx->ents_alive = alive;
    ctx->ents_gen = generations;
    ctx->ents_slots = slot_count;
    ctx->ents_base = base_slot;
    return SU_OK;
}

uint32_t su_vm_context_set_row_limit(SuExecContext *ctx, uint64_t limit) {
    if (ctx == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    ctx->row_limit = limit;
    return SU_OK;
}

uint32_t su_vm_context_last_failed_insn(const SuExecContext *ctx) {
    return ctx != NULL ? ctx->last_failed_insn : SU_VM_NO_FAILED_INSN;
}

uint32_t su_vm_context_set_tier(SuExecContext *ctx, uint32_t tier) {
    if (ctx == NULL || (tier != SU_VM_TIER_REFERENCE && tier != SU_VM_TIER_PLANNED)) {
        return SU_E_INVALID_ARGUMENT;
    }
    ctx->tier = tier;
    return SU_OK;
}

uint32_t su_vm_context_tier_info(const SuExecContext *ctx, SuVmTierInfo *info) {
    if (ctx == NULL || info == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(info, 0, sizeof *info);
    info->tier = ctx->tier;
    info->plan_reason = ctx->plan_reason;
    info->insns = ctx->module->insn_count;
    if (ctx->plan != NULL) {
        info->has_plan = 1u;
        info->body_insns = ctx->plan->n_body;
        info->hoisted_insns = ctx->plan->hoisted;
        info->guard_insns = ctx->plan->has_guard ? ctx->plan->n_guard : 0u;
        info->guard_requirements = ctx->plan->guard_requirements;
        info->has_guard = ctx->plan->has_guard;
    }
    info->last_tier = ctx->last_tier;
    info->last_rows_body = ctx->last_rows_body;
    info->last_rows_skipped = ctx->last_rows_skipped;
    return SU_OK;
}

/* ---- wrapper checks --------------------------------------------------- */

/* Index of the field with this ID that carries the access bit, or -1. */
static long su_find_field(const SuLirModule *m, uint32_t id, uint32_t access) {
    uint32_t i;
    for (i = 0; i < m->field_count; i++) {
        if (m->fields[i].id == id && (m->fields[i].access & access) != 0u) {
            return (long)i;
        }
    }
    return -1;
}

/* Stage 1 (per binding, no cross-checks): INVALID_ARGUMENT. */
static uint32_t su_check_binding_shape(const SuColumnBinding *b, uint32_t access) {
    if (b->struct_size != (uint32_t)sizeof(SuColumnBinding)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (b->elem_type != SU_TYPE_U32 || b->access != access || b->stride_bytes != 4u) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (b->data == NULL) {
        return b->row_count == 0u ? SU_OK : SU_E_INVALID_ARGUMENT;
    }
    if (((uintptr_t)b->data % 4u) != 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    return SU_OK;
}

/* Stage 2 (per binding): SCHEMA_MISMATCH. Records the column in cols[] and
 * marks the table index bound, so duplicates and unknown IDs are caught. */
static uint32_t su_check_binding_schema(SuExecContext *ctx, const SuColumnBinding *b,
                                        uint64_t rows, uint32_t access, uint32_t **cols) {
    long idx;
    if (b->layout_generation != ctx->layout_generation) {
        return SU_E_SCHEMA_MISMATCH;
    }
    if (b->row_count != rows) {
        return SU_E_SCHEMA_MISMATCH;
    }
    idx = su_find_field(ctx->module, b->field_id, access);
    if (idx < 0 || ctx->bound[idx] != 0u) {
        return SU_E_SCHEMA_MISMATCH;
    }
    ctx->bound[idx] = 1u;
    cols[idx] = (uint32_t *)b->data;
    return SU_OK;
}

static int su_ranges_overlap(uintptr_t a0, uintptr_t a1, uintptr_t b0, uintptr_t b1) {
    return a0 < b1 && b0 < a1;
}

/* Stage 4: no staging range may overlap any input or another staging range. */
static uint32_t su_check_alias(const SuColumnBinding *in, uint32_t nin,
                               const SuColumnBinding *out, uint32_t nout, uint64_t rows) {
    uintptr_t bytes;
    uint32_t j;
    uint32_t i;
    uint32_t k;
    if (rows == 0u) {
        return SU_OK;
    }
    bytes = (uintptr_t)rows * 4u;
    for (j = 0; j < nout; j++) {
        uintptr_t os = (uintptr_t)out[j].data;
        uintptr_t oe;
        if (os > UINTPTR_MAX - bytes) {
            return SU_E_INVALID_ARGUMENT;
        }
        oe = os + bytes;
        for (i = 0; i < nin; i++) {
            uintptr_t is = (uintptr_t)in[i].data;
            if (is > UINTPTR_MAX - bytes) {
                return SU_E_INVALID_ARGUMENT;
            }
            if (su_ranges_overlap(os, oe, is, is + bytes)) {
                return SU_E_INVALID_ARGUMENT;
            }
        }
        for (k = 0; k < nout; k++) {
            uintptr_t ks;
            if (k == j) {
                continue;
            }
            ks = (uintptr_t)out[k].data;
            if (ks > UINTPTR_MAX - bytes) {
                return SU_E_INVALID_ARGUMENT;
            }
            if (su_ranges_overlap(os, oe, ks, ks + bytes)) {
                return SU_E_INVALID_ARGUMENT;
            }
        }
    }
    return SU_OK;
}

/* Validates one direction (inputs or outputs). cols[] receives the bound
 * column per table index on success. */
static uint32_t su_check_set(SuExecContext *ctx, const SuColumnBinding *b, uint32_t n,
                             uint32_t access, uint64_t rows, uint32_t **cols) {
    uint32_t i;
    uint32_t st;
    for (i = 0; i < n; i++) {
        st = su_check_binding_shape(&b[i], access);
        if (st != SU_OK) {
            return st;
        }
    }
    for (i = 0; i < n; i++) {
        st = su_check_binding_schema(ctx, &b[i], rows, access, cols);
        if (st != SU_OK) {
            return st;
        }
    }
    return SU_OK;
}

/* IR 0.4: binds the whole-table view for gather_* / find_eq_u32. Without su_vm_context_set_table_view the
 * invocation is the table, which is only meaningful when it starts at slot 0 (else SU_E_INVALID_ARGUMENT, so a
 * chunked caller cannot silently gather from its chunk). A set view is checked like the inputs: same readable
 * fields and generation, one row count, and no overlap with any staging column. */
static uint32_t su_bind_table(SuExecContext *ctx, const SuOutputs *out, uint64_t rows) {
    const SuInvocation *t = &ctx->table;
    size_t fc = ctx->module->field_count > 0 ? ctx->module->field_count : 1u;
    uint32_t st, j, i;
    uintptr_t tb, ob;
    if (!ctx->module->uses_gather) {
        return SU_OK;
    }
    memset(ctx->gather_cols, 0, sizeof *ctx->gather_cols * fc);
    if (!ctx->has_table) {
        if (ctx->base_slot != 0u) {
            return SU_E_INVALID_ARGUMENT;
        }
        memcpy(ctx->gather_cols, ctx->in_cols, sizeof *ctx->gather_cols * fc);
        ctx->gather_rows = rows;
        return SU_OK;
    }
    if (t->struct_size != (uint32_t)sizeof(SuInvocation) || t->abi_version != SU_ABI_VERSION ||
        t->binding_count != ctx->readable_count || (t->binding_count > 0u && t->bindings == NULL)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (t->layout_generation != ctx->layout_generation) {
        return SU_E_SCHEMA_MISMATCH;
    }
    if (t->row_count > SIZE_MAX / sizeof(uint32_t)) {
        return SU_E_RESOURCE_LIMIT;
    }
    memset(ctx->bound, 0, fc);
    st = su_check_set(ctx, t->bindings, t->binding_count, SU_ACCESS_READ, t->row_count, ctx->gather_cols);
    if (st != SU_OK) {
        return st;
    }
    tb = (uintptr_t)t->row_count * 4u;
    ob = (uintptr_t)rows * 4u;
    for (j = 0; tb > 0u && ob > 0u && j < out->binding_count; j++) {
        uintptr_t os = (uintptr_t)out->bindings[j].data;
        for (i = 0; i < t->binding_count; i++) {
            uintptr_t ts = (uintptr_t)t->bindings[i].data;
            if (os > UINTPTR_MAX - ob || ts > UINTPTR_MAX - tb || su_ranges_overlap(os, os + ob, ts, ts + tb)) {
                return SU_E_INVALID_ARGUMENT;
            }
        }
    }
    ctx->gather_rows = t->row_count;
    return SU_OK;
}

/* IR 0.4 handle_live: binds the entity table for this invocation. Without su_vm_context_set_entity_table the
 * call's entity view is the table, which is only meaningful when it starts at slot 0 (else SU_E_INVALID_ARGUMENT,
 * so a chunk cannot silently answer from its own slots). */
static uint32_t su_bind_ents(SuExecContext *ctx, uint64_t rows) {
    if (!ctx->module->uses_handle_live) {
        return SU_OK;
    }
    if (ctx->has_ents) {
        ctx->hl_alive = ctx->ents_alive;
        ctx->hl_gen = ctx->ents_gen;
        ctx->hl_slots = ctx->ents_slots;
        ctx->hl_base = ctx->ents_base;
        return SU_OK;
    }
    if (ctx->base_slot != 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    ctx->hl_alive = ctx->alive;
    ctx->hl_gen = ctx->generations;
    ctx->hl_slots = rows;
    ctx->hl_base = 0u;
    return SU_OK;
}

/* ---- execution -------------------------------------------------------- */

/* Executes one instruction for row r. Returns SU_OK or SU_E_INTERNAL. */
static uint32_t su_exec_insn(SuExecContext *ctx, const SuInsn *ins, size_t r) {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t z = 0;
    switch (ins->op) {
    case SU_OP_CONST_U32:
        su_write_reg(ctx, ins->dst, SU_TYPE_U32, ins->a);
        return SU_OK;
    case SU_OP_LOAD_U32: {
        const uint32_t *col = ctx->in_cols[ins->a];
        if (col == NULL) {
            return SU_E_INTERNAL;
        }
        su_write_reg(ctx, ins->dst, SU_TYPE_U32, col[r]);
        return SU_OK;
    }
    case SU_OP_ADD_U32:
    case SU_OP_SUB_U32:
    case SU_OP_MUL_U32:
        if (!su_read_u32(ctx, ins->a, &x) || !su_read_u32(ctx, ins->b, &y)) {
            return SU_E_INTERNAL;
        }
        if (ins->op == SU_OP_ADD_U32) {
            z = x + y;
        } else if (ins->op == SU_OP_SUB_U32) {
            z = x - y;
        } else {
            z = x * y;
        }
        su_write_reg(ctx, ins->dst, SU_TYPE_U32, z);
        return SU_OK;
    case SU_OP_EQ_U32:
    case SU_OP_LT_U32:
        if (!su_read_u32(ctx, ins->a, &x) || !su_read_u32(ctx, ins->b, &y)) {
            return SU_E_INTERNAL;
        }
        z = (ins->op == SU_OP_EQ_U32) ? (x == y) : (x < y);
        su_write_reg(ctx, ins->dst, SU_TYPE_BOOL, z);
        return SU_OK;
    case SU_OP_SELECT_U32:
        if (!su_read_bool(ctx, ins->a, &x) || !su_read_u32(ctx, ins->b, &y) ||
            !su_read_u32(ctx, ins->c, &z)) {
            return SU_E_INTERNAL;
        }
        su_write_reg(ctx, ins->dst, SU_TYPE_U32, x != 0u ? y : z);
        return SU_OK;
    case SU_OP_STORE_U32: {
        uint32_t *col = ctx->out_cols[ins->a];
        if (col == NULL || ctx->stored[ins->a] != 0u) {
            return SU_E_INTERNAL;
        }
        if (!su_read_u32(ctx, ins->b, &x)) {
            return SU_E_INTERNAL;
        }
        col[r] = x;
        ctx->stored[ins->a] = 1u;
        return SU_OK;
    }
    case SU_OP_SLOT_U32:
        su_write_reg(ctx, ins->dst, SU_TYPE_U32, ctx->base_slot + (uint32_t)r);
        return SU_OK;
    case SU_OP_GEN_U32:
        su_write_reg(ctx, ins->dst, SU_TYPE_U32,
                     ctx->generations != NULL ? ctx->generations[r] : SU_GEN_FIRST);
        return SU_OK;
    case SU_OP_TICK_U32:
        su_write_reg(ctx, ins->dst, SU_TYPE_U32, ctx->tick_low);
        return SU_OK;
    case SU_OP_DESPAWN_SELF: {
        uint32_t go = 0;
        uint32_t slot = ctx->base_slot + (uint32_t)r;
        uint32_t gen = ctx->generations != NULL ? ctx->generations[r] : SU_GEN_FIRST;
        if (!su_read_bool(ctx, ins->dst, &go)) {
            return SU_E_INTERNAL;
        }
        if (go == 0u) {
            return SU_OK;
        }
        if (ctx->emitter.emit_despawn(ctx->emitter.user, slot, ctx->row_seq,
                                      SU_ENTITY_MAKE(slot, gen)) != 0) {
            return SU_E_RESOURCE_LIMIT;
        }
        ctx->row_seq++;
        return SU_OK;
    }
    case SU_OP_EMIT_EVENT: {
        uint32_t go = 0;
        uint32_t pa = 0;
        uint32_t pb = 0;
        uint32_t slot = ctx->base_slot + (uint32_t)r;
        if (!su_read_bool(ctx, ins->dst, &go)) {
            return SU_E_INTERNAL;
        }
        if (go == 0u) {
            return SU_OK;
        }
        if (!su_read_u32(ctx, ins->b, &pa) || !su_read_u32(ctx, ins->c, &pb)) {
            return SU_E_INTERNAL;
        }
        if (ctx->emitter.emit_event(ctx->emitter.user, slot, ctx->row_seq, ins->a, pa, pb) != 0) {
            return SU_E_RESOURCE_LIMIT;
        }
        ctx->row_seq++;
        return SU_OK;
    }
    default:
        return ctx->reg_w != NULL ? su_exec_insn03(ctx, ins, r) : SU_E_INTERNAL;
    }
}

/* Planned tier (su_vm_plan.h): used when the context's tier is planned, the module has a plan and every lane
 * the plan touches is bound; otherwise the reference loop below runs. Both give the same results. */
static int su_exec_rows_planned(SuExecContext *ctx, uint64_t rows, SuOutputs *out, uint32_t *st) {
    SuVmPlanRun run;
    uint32_t failed = SU_VM_NO_FAILED_INSN;
    if (ctx->tier != SU_VM_TIER_PLANNED || ctx->plan == NULL || !su_vm_plan_bound(ctx->plan, ctx->in_cols, ctx->out_cols)) {
        return 0;
    }
    memset(&run, 0, sizeof run);
    run.in_cols = ctx->in_cols;
    run.out_cols = ctx->out_cols;
    run.alive = ctx->alive;
    run.generations = ctx->generations;
    run.base_slot = ctx->base_slot;
    run.tick_low = ctx->tick_low;
    run.emits = ctx->needs_emitter;
    run.gather_cols = ctx->gather_cols;
    run.gather_rows = ctx->gather_rows;
    run.ents_alive = ctx->hl_alive;
    run.ents_gen = ctx->hl_gen;
    run.ents_slots = ctx->hl_slots;
    run.ents_base = ctx->hl_base;
    if (ctx->module->has_row_filter) {
        run.filter_col = ctx->in_cols[ctx->module->row_filter_field];
        run.filter_mask = ctx->module->row_filter_mask;
        run.filter_value = ctx->module->row_filter_value;
        run.filter_lanes = ctx->filter_copy;
        run.n_filter_lanes = ctx->writable_count;
    }
    if (ctx->has_emitter) {
        run.emit_user = ctx->emitter.user;
        run.emit_despawn = ctx->emitter.emit_despawn;
        run.emit_event = ctx->emitter.emit_event;
    }
    *st = su_vm_plan_run(ctx->plan, &run, rows, &failed);
    ctx->last_tier = SU_VM_TIER_PLANNED;
    ctx->last_rows_body = run.rows_body;
    ctx->last_rows_skipped = run.rows_skipped;
    if (*st != SU_OK) {
        ctx->last_failed_insn = failed;
        out->failed_insn = failed;
    }
    return 1;
}

static uint32_t su_exec_rows(SuExecContext *ctx, uint64_t rows, SuOutputs *out) {
    const SuLirModule *m = ctx->module;
    /* NULL alive view (every M0 world): the dead-row test is skipped without a per-row load. row_seq is read only
     * by emitter calls, so it is reset only for modules that can emit. */
    const uint8_t *alive = ctx->alive;
    const int emits = ctx->needs_emitter;
    uint64_t r;
    uint32_t pst = SU_OK;
    if (su_exec_rows_planned(ctx, rows, out, &pst)) {
        return pst;
    }
    ctx->last_tier = SU_VM_TIER_REFERENCE;
    ctx->last_rows_body = 0u;
    ctx->last_rows_skipped = 0u;
    for (r = 0; r < rows; r++) {
        uint32_t i;
        uint32_t k;
        /* A dead row is skipped entirely: no staged writes, no emissions, no store check. */
        if (alive != NULL && alive[r] == 0u) {
            continue;
        }
        /* IR 0.4 row filter: a live row that does not match keeps its entry values and runs nothing. */
        if (m->has_row_filter && (ctx->in_cols[m->row_filter_field][r] & m->row_filter_mask) != m->row_filter_value) {
            for (k = 0; k < ctx->writable_count; k++) {
                uint32_t f = ctx->write_fields[k];
                ctx->out_cols[f][r] = ctx->in_cols[f][r];
            }
            continue;
        }
        if (emits) {
            ctx->row_seq = 0u;
        }
        for (k = 0; k < ctx->writable_count; k++) {
            ctx->stored[ctx->write_fields[k]] = 0u;
        }
        for (i = 0; i < m->insn_count; i++) {
            uint32_t st = su_exec_insn(ctx, &m->insns[i], (size_t)r);
            if (st != SU_OK) {
                ctx->last_failed_insn = i;
                out->failed_insn = i;
                return st;
            }
        }
        for (k = 0; k < ctx->writable_count; k++) {
            if (ctx->stored[ctx->write_fields[k]] == 0u) {
                /* Verifier guarantees one store per writable field per row. */
                ctx->last_failed_insn = SU_VM_NO_FAILED_INSN;
                out->failed_insn = SU_VM_NO_FAILED_INSN;
                return SU_E_INTERNAL;
            }
        }
    }
    return SU_OK;
}

uint32_t su_vm_invoke_v1(SuExecContext *ctx, const SuInvocation *input, SuOutputs *out) {
    uint32_t st;
    uint64_t rows;
    if (ctx == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    ctx->last_failed_insn = SU_VM_NO_FAILED_INSN;
    if (input == NULL || out == NULL) {
        return fail_out(ctx, out, SU_E_INVALID_ARGUMENT);
    }
    if (input->struct_size != (uint32_t)sizeof(SuInvocation) ||
        input->abi_version != SU_ABI_VERSION) {
        return fail_out(ctx, out, SU_E_INVALID_ARGUMENT);
    }
    if (out->struct_size != (uint32_t)sizeof(SuOutputs) || out->reserved != 0u) {
        return fail_out(ctx, out, SU_E_INVALID_ARGUMENT);
    }
    if (input->binding_count != ctx->readable_count ||
        out->binding_count != ctx->writable_count) {
        return fail_out(ctx, out, SU_E_INVALID_ARGUMENT);
    }
    if ((input->binding_count > 0u && input->bindings == NULL) ||
        (out->binding_count > 0u && out->bindings == NULL)) {
        return fail_out(ctx, out, SU_E_INVALID_ARGUMENT);
    }
    if (ctx->needs_emitter && !ctx->has_emitter) {
        return fail_out(ctx, out, SU_E_INVALID_ARGUMENT);
    }

    /* Schema stage. */
    if (input->layout_generation != ctx->layout_generation) {
        return fail_out(ctx, out, SU_E_SCHEMA_MISMATCH);
    }
    memset(ctx->in_cols, 0, sizeof *ctx->in_cols * (ctx->module->field_count > 0 ? ctx->module->field_count : 1u));
    memset(ctx->out_cols, 0, sizeof *ctx->out_cols * (ctx->module->field_count > 0 ? ctx->module->field_count : 1u));
    memset(ctx->bound, 0, ctx->module->field_count > 0 ? ctx->module->field_count : 1u);

    rows = input->row_count;
    /* Shape and schema checks run before any column pointer is used. */
    st = su_check_set(ctx, input->bindings, input->binding_count, SU_ACCESS_READ, rows, ctx->in_cols);
    if (st != SU_OK) {
        return fail_out(ctx, out, st);
    }
    memset(ctx->bound, 0, ctx->module->field_count > 0 ? ctx->module->field_count : 1u);
    st = su_check_set(ctx, out->bindings, out->binding_count, SU_ACCESS_WRITE, rows, ctx->out_cols);
    if (st != SU_OK) {
        return fail_out(ctx, out, st);
    }
    if (rows > ctx->row_limit || rows > SIZE_MAX / sizeof(uint32_t)) {
        return fail_out(ctx, out, SU_E_RESOURCE_LIMIT);
    }
    st = su_check_alias(input->bindings, input->binding_count, out->bindings, out->binding_count, rows);
    if (st != SU_OK) {
        return fail_out(ctx, out, st);
    }
    if (ctx->module->has_row_filter) {
        uint32_t k;
        for (k = 0; k < ctx->writable_count; k++) {
            if (ctx->in_cols[ctx->write_fields[k]] == NULL) {
                return fail_out(ctx, out, SU_E_INVALID_ARGUMENT);
            }
            ctx->filter_copy[k] = ctx->write_fields[k];
        }
    }
    st = su_bind_table(ctx, out, rows);
    if (st == SU_OK) {
        st = su_bind_ents(ctx, rows);
    }
    if (st != SU_OK) {
        return fail_out(ctx, out, st);
    }

    if (rows == 0u) {
        return SU_OK;
    }
    st = su_exec_rows(ctx, rows, out);
    if (st != SU_OK) {
        return st;
    }
    return SU_OK;
}

uint32_t su_vm_run_system(SuWorld *w, SuExecContext *ctx) {
    SuInvocation wi;
    SuOutputs wo;
    SuInvocation inv;
    SuOutputs outs;
    const SuLirModule *m;
    uint32_t i;
    uint32_t n_in = 0;
    uint32_t n_out = 0;
    uint32_t cur_in = 0;
    uint32_t cur_out = 0;
    uint32_t st;

    if (w == NULL || ctx == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    m = ctx->module;
    st = su_world_build_invocation(w, &wi);
    if (st != SU_OK) {
        return st;
    }
    st = su_world_build_outputs(w, &wo);
    if (st != SU_OK) {
        return st;
    }

    /* Select the module's declared fields from the world's views, in table
     * order. A declared field the world lacks is a schema mismatch. */
    for (i = 0; i < m->field_count; i++) {
        uint32_t acc = m->fields[i].access;
        uint32_t id = m->fields[i].id;
        int found = 0;
        if ((acc & SU_ACCESS_READ) != 0u) {
            const SuColumnBinding *b = su_world_find_binding_from(wi.bindings, wi.binding_count, id, &cur_in);
            if (b != NULL) {
                ctx->scr_in[n_in++] = *b;
                found = 1;
            }
            if (!found) {
                return SU_E_SCHEMA_MISMATCH;
            }
        }
        if ((acc & SU_ACCESS_WRITE) != 0u) {
            found = 0;
            const SuColumnBinding *b = su_world_find_binding_from(wo.bindings, wo.binding_count, id, &cur_out);
            if (b != NULL) {
                ctx->scr_out[n_out++] = *b;
                found = 1;
            }
            if (!found) {
                return SU_E_SCHEMA_MISMATCH;
            }
        }
    }

    inv.struct_size = (uint32_t)sizeof(SuInvocation);
    inv.abi_version = SU_ABI_VERSION;
    inv.layout_generation = wi.layout_generation;
    inv.binding_count = n_in;
    inv.row_count = wi.row_count;
    inv.bindings = ctx->scr_in;

    outs.struct_size = (uint32_t)sizeof(SuOutputs);
    outs.binding_count = n_out;
    outs.failed_insn = SU_VM_NO_FAILED_INSN;
    outs.reserved = 0u;
    outs.bindings = ctx->scr_out;

    if (m->uses_gather) {
        /* The invocation is the whole table: it is also the gather view for this call (IR 0.4). */
        int had = ctx->has_table;
        SuInvocation prev = ctx->table;
        ctx->table = inv;
        ctx->has_table = 1;
        st = su_vm_invoke_v1(ctx, &inv, &outs);
        ctx->table = prev;
        ctx->has_table = had;
    } else {
        st = su_vm_invoke_v1(ctx, &inv, &outs);
    }
    if (st != SU_OK) {
        return st;
    }
    return su_world_commit_staging(w, ctx->write_ids, n_out);
}
