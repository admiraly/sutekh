/* Checked scalar reference VM (M0-04). See su_vm.h.
 *
 * Arithmetic is unsigned 32-bit (modulo 2^32). Registers carry a type tag
 * (SU_TYPE_U32 or SU_TYPE_BOOL, 0 = never written); every operand read checks
 * the tag, so a module that bypassed the verifier fails with SU_E_INTERNAL
 * instead of computing on the wrong type. Rows run in ascending order; within
 * a row, instructions run in ascending index order. */
#include "vm/su_vm.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/entities.h"

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
};

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
    default:
        return 0;
    }
}

/* Index-range and access checks for one instruction. */
static int su_insn_shape_ok(const SuInsn *ins, const SuLirModule *m) {
    uint32_t rc = m->register_count;
    uint32_t fc = m->field_count;
    if (!su_insn_known(ins->op, m->ir_version_minor)) {
        return 0;
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

/* ---- lifetime --------------------------------------------------------- */

void su_vm_context_destroy(SuExecContext *ctx) {
    if (ctx == NULL) {
        return;
    }
    free(ctx->reg_val);
    free(ctx->reg_tag);
    free(ctx->in_cols);
    free(ctx->out_cols);
    free(ctx->bound);
    free(ctx->stored);
    free(ctx->write_fields);
    free(ctx->write_ids);
    free(ctx->scr_in);
    free(ctx->scr_out);
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
    if (c->reg_val == NULL || c->reg_tag == NULL || c->in_cols == NULL || c->out_cols == NULL ||
        c->bound == NULL || c->stored == NULL || c->write_fields == NULL || c->write_ids == NULL) {
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
        return SU_E_INTERNAL;
    }
}

static uint32_t su_exec_rows(SuExecContext *ctx, uint64_t rows, SuOutputs *out) {
    const SuLirModule *m = ctx->module;
    /* NULL alive view (every M0 world): the dead-row test is skipped without a per-row load. row_seq is read only
     * by emitter calls, so it is reset only for modules that can emit. */
    const uint8_t *alive = ctx->alive;
    const int emits = ctx->needs_emitter;
    uint64_t r;
    for (r = 0; r < rows; r++) {
        uint32_t i;
        uint32_t k;
        /* A dead row is skipped entirely: no staged writes, no emissions, no store check. */
        if (alive != NULL && alive[r] == 0u) {
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
        uint32_t j;
        int found = 0;
        if ((acc & SU_ACCESS_READ) != 0u) {
            for (j = 0; j < wi.binding_count && !found; j++) {
                if (wi.bindings[j].field_id == id) {
                    ctx->scr_in[n_in++] = wi.bindings[j];
                    found = 1;
                }
            }
            if (!found) {
                return SU_E_SCHEMA_MISMATCH;
            }
        }
        if ((acc & SU_ACCESS_WRITE) != 0u) {
            found = 0;
            for (j = 0; j < wo.binding_count && !found; j++) {
                if (wo.bindings[j].field_id == id) {
                    ctx->scr_out[n_out++] = wo.bindings[j];
                    found = 1;
                }
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

    st = su_vm_invoke_v1(ctx, &inv, &outs);
    if (st != SU_OK) {
        return st;
    }
    return su_world_commit_staging(w, ctx->write_ids, n_out);
}
