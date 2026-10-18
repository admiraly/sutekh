/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * VM tiers (su_vm.h): the planned tier against the reference tier on hand-built modules that reach the edges a
 * verified random corpus cannot (tests/ir03/ir03_diff.py covers verified IR 0.3 programs):
 *   - emissions and an emitter overflow: same records in the same order, same status and failed_insn;
 *   - a guarded module whose skipped rows emit nothing and store their own values;
 *   - read before write and a double store: no plan (with the reason), identical failures;
 *   - a register defined twice (not SSA): no hoisting of it, identical results;
 *   - the racer movement capsule: a plan with a one-condition guard and hoisted constants.
 * Run from the repository root. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_lir.h"
#include "sutekh/abi.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"
#include "vm/su_vm.h"

static int g_checks = 0, g_fail = 0;
#define CHECK(c)                                                                 \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(c)) {                                                              \
            g_fail++;                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);         \
        }                                                                        \
    } while (0)

#define ROWS 64u
#define MAXF 4u

typedef struct Rec {
    uint32_t kind, slot, seq, k, a, b;
    uint64_t handle;
} Rec;

typedef struct Sink {
    Rec rec[256];
    uint32_t n, cap;
} Sink;

static int on_despawn(void *user, uint32_t slot, uint32_t seq, uint64_t handle) {
    Sink *s = user;
    if (s->n >= s->cap) {
        return 1;
    }
    s->rec[s->n].kind = 1;
    s->rec[s->n].slot = slot;
    s->rec[s->n].seq = seq;
    s->rec[s->n].handle = handle;
    s->n++;
    return 0;
}

static int on_event(void *user, uint32_t slot, uint32_t seq, uint32_t k, uint32_t a, uint32_t b) {
    Sink *s = user;
    if (s->n >= s->cap) {
        return 1;
    }
    s->rec[s->n].kind = 2;
    s->rec[s->n].slot = slot;
    s->rec[s->n].seq = seq;
    s->rec[s->n].k = k;
    s->rec[s->n].a = a;
    s->rec[s->n].b = b;
    s->n++;
    return 0;
}

typedef struct Result {
    uint32_t st, failed, tier_ran, has_plan, reason, has_guard;
    uint64_t skipped;
    uint32_t out[MAXF][ROWS];
    Sink sink;
} Result;

/* Runs m once on the given tier over input columns in[f] (one per module field, by table index). */
static void run(const SuLirModule *m, uint32_t tier, uint32_t in[MAXF][ROWS], const uint8_t *alive, uint32_t cap,
                Result *res) {
    SuExecContext *ctx = NULL;
    SuColumnBinding ib[MAXF], ob[MAXF];
    SuInvocation inv;
    SuOutputs out;
    SuVmEmitter em;
    SuVmTierInfo info;
    uint32_t i, ni = 0, no = 0;
    memset(res, 0, sizeof *res);
    res->sink.cap = cap;
    res->st = su_vm_context_create(m, 1, &ctx);
    if (res->st != SU_OK) {
        return;
    }
    su_vm_context_set_tier(ctx, tier);
    em.user = &res->sink;
    em.emit_despawn = on_despawn;
    em.emit_event = on_event;
    su_vm_context_set_entity_view(ctx, alive, NULL, 100u, 7u, &em);
    for (i = 0; i < m->field_count; i++) {
        SuColumnBinding b;
        memset(&b, 0, sizeof b);
        b.struct_size = (uint32_t)sizeof b;
        b.field_id = m->fields[i].id;
        b.elem_type = SU_TYPE_U32;
        b.layout_generation = 1;
        b.stride_bytes = 4;
        b.row_count = ROWS;
        if ((m->fields[i].access & SU_ACCESS_READ) != 0u) {
            b.access = SU_ACCESS_READ;
            b.data = in[i];
            ib[ni++] = b;
        }
        if ((m->fields[i].access & SU_ACCESS_WRITE) != 0u) {
            b.access = SU_ACCESS_WRITE;
            b.data = res->out[i];
            ob[no++] = b;
        }
    }
    inv.struct_size = (uint32_t)sizeof inv;
    inv.abi_version = SU_ABI_VERSION;
    inv.layout_generation = 1;
    inv.binding_count = ni;
    inv.row_count = ROWS;
    inv.bindings = ib;
    out.struct_size = (uint32_t)sizeof out;
    out.binding_count = no;
    out.failed_insn = UINT32_MAX;
    out.reserved = 0;
    out.bindings = ob;
    res->st = su_vm_invoke_v1(ctx, &inv, &out);
    res->failed = out.failed_insn;
    CHECK(su_vm_context_last_failed_insn(ctx) == res->failed);
    su_vm_context_tier_info(ctx, &info);
    res->tier_ran = info.last_tier;
    res->has_plan = info.has_plan;
    res->reason = info.plan_reason;
    res->has_guard = info.has_guard;
    res->skipped = info.last_rows_skipped;
    su_vm_context_destroy(ctx);
}

/* Both tiers; the planned result must equal the reference result in status, failed_insn, emissions and (on
 * success) every staged lane. Returns the planned result in *p. */
static void both(const SuLirModule *m, uint32_t in[MAXF][ROWS], const uint8_t *alive, uint32_t cap, Result *p) {
    static Result r;
    uint32_t i;
    run(m, SU_VM_TIER_REFERENCE, in, alive, cap, &r);
    run(m, SU_VM_TIER_PLANNED, in, alive, cap, p);
    CHECK(r.tier_ran == SU_VM_TIER_REFERENCE || r.st != SU_OK);
    CHECK(p->st == r.st);
    CHECK(p->failed == r.failed);
    CHECK(p->sink.n == r.sink.n);
    CHECK(memcmp(p->sink.rec, r.sink.rec, sizeof r.sink.rec) == 0);
    if (r.st == SU_OK) {
        for (i = 0; i < m->field_count; i++) {
            if ((m->fields[i].access & SU_ACCESS_WRITE) != 0u) {
                CHECK(memcmp(p->out[i], r.out[i], sizeof r.out[i]) == 0);
            }
        }
    }
}

static SuLirModule module(uint32_t minor, SuLirField *f, uint32_t nf, SuInsn *ins, uint32_t ni, uint32_t regs) {
    SuLirModule m;
    memset(&m, 0, sizeof m);
    m.fields = f;
    m.field_count = nf;
    m.insns = ins;
    m.insn_count = ni;
    m.register_count = regs;
    m.ir_version_minor = minor;
    m.uses_entity_ops = 1;
    return m;
}

static SuInsn I(uint32_t op, uint32_t d, uint32_t a, uint32_t b, uint32_t c) {
    SuInsn x;
    x.op = op;
    x.dst = d;
    x.a = a;
    x.b = b;
    x.c = c;
    return x;
}

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    long sz;
    uint8_t *b;
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    rewind(f);
    b = malloc((size_t)sz + 1u);
    *n = b != NULL ? fread(b, 1, (size_t)sz, f) : 0u;
    fclose(f);
    return b;
}

int main(void) {
    static uint32_t in[MAXF][ROWS];
    static Result p;
    uint8_t alive[ROWS];
    uint32_t r;
    SuLirField f[2] = {{1, SU_TYPE_U32, SU_ACCESS_READ | SU_ACCESS_WRITE, "hp", 0, 0},
                       {2, SU_TYPE_U32, SU_ACCESS_READ, "gate", 0, 0}};

    for (r = 0; r < ROWS; r++) {
        in[0][r] = r * 37u % 101u;
        in[1][r] = (r % 5u == 0u) ? 1u : 0u;
        alive[r] = (uint8_t)(r % 7u != 3u);
    }

    /* 1. Guarded module with emissions: gate == 1 -> hp - 1 and an event and a despawn when hp < 10; otherwise
     *    hp unchanged and nothing emitted. The guard skips the gate == 0 rows. */
    {
        SuInsn ins[] = {
            I(SU_OP_LOAD_U32, 0, 0, 0, 0),  /* r0 = hp */
            I(SU_OP_LOAD_U32, 1, 1, 0, 0),  /* r1 = gate */
            I(SU_OP_CONST_U32, 2, 1, 0, 0), /* r2 = 1 */
            I(SU_OP_EQ_U32, 3, 1, 2, 0),    /* r3 = gate == 1 */
            I(SU_OP_SUB_U32, 4, 0, 2, 0),   /* r4 = hp - 1 */
            I(SU_OP_SELECT_U32, 5, 3, 4, 0),/* r5 = r3 ? r4 : r0 */
            I(SU_OP_EMIT_EVENT, 3, 9, 0, 4),/* if r3: event 9 (hp, hp - 1) */
            I(SU_OP_CONST_U32, 6, 10, 0, 0),
            I(SU_OP_LT_U32, 7, 0, 6, 0),    /* r7 = hp < 10 */
            I(SU_OP_AND_BOOL, 8, 3, 7, 0),  /* r8 = r3 && r7 (0.3 op in a 0.3 module) */
            I(SU_OP_DESPAWN_SELF, 8, 0, 0, 0),
            I(SU_OP_STORE_U32, 0, 0, 5, 0),
        };
        SuLirModule m = module(3, f, 2, ins, 12, 9);
        both(&m, in, NULL, 256, &p);
        CHECK(p.st == SU_OK && p.has_plan && p.has_guard && p.tier_ran == SU_VM_TIER_PLANNED);
        CHECK(p.skipped == ROWS - ROWS / 5u - 1u); /* rows 0, 5, ..., 60 run the body */
        CHECK(p.sink.n > 0u);
        both(&m, in, alive, 256, &p); /* dead rows */
        CHECK(p.st == SU_OK && p.has_guard);
        /* Emitter overflow at every capacity: same records kept, same failing instruction. */
        for (r = 0; r < 20u; r++) {
            both(&m, in, r % 2u ? alive : NULL, r, &p);
            CHECK(p.st == SU_OK || (p.st == SU_E_RESOURCE_LIMIT && (p.failed == 6u || p.failed == 10u)));
        }
    }

    /* 2. Not provable: the false side stores another field's value. A plan, no guard, same results. */
    {
        SuLirField g[2] = {{1, SU_TYPE_U32, SU_ACCESS_READ | SU_ACCESS_WRITE, "hp", 0, 0},
                           {2, SU_TYPE_U32, SU_ACCESS_READ, "gate", 0, 0}};
        SuInsn ins[] = {
            I(SU_OP_LOAD_U32, 0, 0, 0, 0), I(SU_OP_LOAD_U32, 1, 1, 0, 0), I(SU_OP_CONST_U32, 2, 1, 0, 0),
            I(SU_OP_EQ_U32, 3, 1, 2, 0),   I(SU_OP_SUB_U32, 4, 0, 2, 0),
            I(SU_OP_SELECT_U32, 5, 3, 4, 1), /* false side: gate, not hp */
            I(SU_OP_STORE_U32, 0, 0, 5, 0),
        };
        SuLirModule m = module(3, g, 2, ins, 7, 6);
        both(&m, in, NULL, 0, &p);
        CHECK(p.st == SU_OK && p.has_plan && !p.has_guard);
    }

    /* 3. Read before write: no plan (reason 1); the reference failure is reproduced. */
    {
        SuInsn ins[] = {I(SU_OP_ADD_U32, 0, 1, 1, 0), I(SU_OP_STORE_U32, 0, 0, 0, 0)};
        SuLirModule m = module(1, f, 2, ins, 2, 2);
        both(&m, in, NULL, 0, &p);
        CHECK(!p.has_plan && p.reason == 1u && p.st == SU_E_INTERNAL && p.failed == 0u);
    }

    /* 4. Double store: no plan (reason 2), same failure. Missing store: same. */
    {
        SuInsn ins[] = {I(SU_OP_LOAD_U32, 0, 0, 0, 0), I(SU_OP_STORE_U32, 0, 0, 0, 0), I(SU_OP_STORE_U32, 0, 0, 0, 0)};
        SuLirModule m = module(1, f, 2, ins, 3, 1);
        both(&m, in, NULL, 0, &p);
        CHECK(!p.has_plan && p.reason == 2u && p.st == SU_E_INTERNAL && p.failed == 2u);
        m.insn_count = 1;
        both(&m, in, NULL, 0, &p);
        CHECK(!p.has_plan && p.reason == 2u && p.st == SU_E_INTERNAL);
    }

    /* 5. A register defined twice (hand-built, not SSA): the constant is not hoisted; results equal. */
    {
        SuInsn ins[] = {
            I(SU_OP_CONST_U32, 1, 3, 0, 0), /* r1 = 3 */
            I(SU_OP_LOAD_U32, 0, 0, 0, 0),  /* r0 = hp */
            I(SU_OP_MUL_U32, 1, 0, 1, 0),   /* r1 = hp * 3 (redefines r1) */
            I(SU_OP_ADD_U32, 2, 1, 0, 0),   /* r2 = r1 + hp */
            I(SU_OP_STORE_U32, 0, 0, 2, 0),
        };
        SuLirModule m = module(1, f, 2, ins, 5, 3);
        both(&m, in, NULL, 0, &p);
        CHECK(p.st == SU_OK && p.has_plan && p.out[0][4] == in[0][4] * 4u);
    }

    /* 6. The racer movement capsule. */
    {
        size_t cn = 0, ln = 0;
        uint8_t *cb = slurp("capsules/space3d_racer/move.capsule.json", &cn);
        uint8_t *lb = slurp("capsules/space3d_racer/move.lir.json", &ln);
        SuLirModule m;
        SuDiag d;
        SuExecContext *ctx = NULL;
        SuVmTierInfo info;
        CHECK(cb != NULL && lb != NULL);
        if (cb != NULL && lb != NULL && su_lir_verify(cb, cn, lb, ln, &m, &d) == SU_OK) {
            CHECK(su_vm_context_create(&m, 1, &ctx) == SU_OK);
            su_vm_context_tier_info(ctx, &info);
            printf("racer.move: %u insns, body %u, hoisted %u, guard %u insns / %u requirements\n", info.insns,
                   info.body_insns, info.hoisted_insns, info.guard_insns, info.guard_requirements);
            CHECK(info.has_plan && info.has_guard && info.guard_requirements == 1u && info.guard_insns == 2u);
            CHECK(info.hoisted_insns + info.body_insns == info.insns && info.hoisted_insns >= 60u);
            su_vm_context_destroy(ctx);
            su_lir_module_free(&m);
        } else {
            CHECK(0);
        }
        free(cb);
        free(lb);
    }

    /* 7. Tier API. */
    CHECK(su_vm_context_set_tier(NULL, SU_VM_TIER_PLANNED) == SU_E_INVALID_ARGUMENT);
    su_vm_set_default_tier(SU_VM_TIER_REFERENCE);
    CHECK(su_vm_default_tier() == SU_VM_TIER_REFERENCE);
    su_vm_set_default_tier(0);

    printf("vm_tiers: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
