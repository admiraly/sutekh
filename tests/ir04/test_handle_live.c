/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * IR 0.4 handle_live (LIVE_IR_SPEC "IR 0.4", issue 162): verifier acceptance and rejection with exact codes and JSON
 * pointers, module flags and hash coverage; VM known answers on both tiers through su_vm_invoke_v1 (despawned slot,
 * older and newer generations, generation 0, retired and out-of-range slots), the entity-table defaults and wrapper
 * rule (a chunk at base_slot != 0 needs su_vm_context_set_entity_table), chunked invocations against the whole
 * table, and a seeded differential of the reference and planned tiers against an independent oracle, including a
 * program whose identity-row guard evaluates handle_live. With the native backend, su_native_compile refuses it. The
 * host-level test (despawn, slot reuse, snapshot/restore) is test_host_handle.c. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_lir.h"
#include "sutekh/abi.h"
#include "sutekh/entities.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"
#include "vm/su_vm.h"
#ifdef SU_TEST_NATIVE
#include "backends/native/su_native.h"
#endif

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        g_checks++;                                                                 \
        if (!(cond)) {                                                              \
            g_failures++;                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

static char g_cap[16384];
static char g_lir[16384];

static void make_capsule(const char *ver) {
    snprintf(g_cap, sizeof g_cap,
             "{\"capsule_format\":\"0.1\",\"ir_version\":\"%s\",\"system_id\":\"t.hl\",\"source\":\"t.lir.json\","
             "\"schema\":{\"id\":\"t.tab\",\"version\":1,\"layout_generation\":1},\"numeric_profile\":\"fixed_q32_v1\","
             "\"fields\":[{\"id\":1,\"name\":\"ts\",\"type\":\"u32\",\"read\":true,\"write\":false},"
             "{\"id\":2,\"name\":\"tg\",\"type\":\"u32\",\"read\":true,\"write\":false},"
             "{\"id\":3,\"name\":\"ti\",\"type\":\"i32\",\"read\":true,\"write\":false},"
             "{\"id\":10,\"name\":\"o0\",\"type\":\"u32\",\"read\":true,\"write\":true},"
             "{\"id\":11,\"name\":\"o1\",\"type\":\"u32\",\"read\":true,\"write\":true},"
             "{\"id\":12,\"name\":\"o2\",\"type\":\"u32\",\"read\":true,\"write\":true}],"
             "\"schedule\":{\"phase\":\"simulate\",\"after\":[],\"before\":[],\"parallel\":\"serial\"},"
             "\"limits\":{\"max_instructions\":4096,\"max_rows\":1000000,\"allocation\":\"forbidden\"},"
             "\"tests\":[\"t.test.json\"]}",
             ver);
}

static void make_lir(const char *ver, const char *insns) {
    snprintf(g_lir, sizeof g_lir,
             "{\"ir_version\":\"%s\",\"system_id\":\"t.hl\",\"numeric_profile\":\"fixed_q32_v1\",\"query\":\"t.tab\","
             "\"instructions\":[%s]}",
             ver, insns);
}

static uint32_t verify(SuLirModule *m, SuDiag *d) {
    return su_lir_verify((const uint8_t *)g_cap, strlen(g_cap), (const uint8_t *)g_lir, strlen(g_lir), m, d);
}

static void expect(int line, uint32_t want, const char *ptr) {
    SuLirModule m;
    SuDiag d;
    uint32_t st = verify(&m, &d);
    g_checks++;
    if (st != want || (ptr != NULL && strcmp(d.json_pointer, ptr) != 0)) {
        g_failures++;
        fprintf(stderr, "line %d: got %s at '%s' (%s), want %s at '%s'\n", line, su_status_name(st), d.json_pointer,
                d.message, su_status_name(want), ptr != NULL ? ptr : "*");
    }
    if (st == SU_OK) {
        su_lir_module_free(&m);
    }
}

#define LOADS "{\"op\":\"load_u32\",\"dst\":\"s\",\"field\":\"ts\"},{\"op\":\"load_u32\",\"dst\":\"g\",\"field\":\"tg\"},"
#define ONE_ZERO "{\"op\":\"const_u32\",\"dst\":\"one\",\"value\":1},{\"op\":\"const_u32\",\"dst\":\"zero\",\"value\":0},"

/* Values: o0 = handle_live(ts, tg); o1 = handle_live(own slot, own generation); o2 = handle_live(own slot, tg). */
#define PROG_VALUES                                                                                                \
    LOADS ONE_ZERO                                                                                                 \
    "{\"op\":\"handle_live\",\"dst\":\"h\",\"slot\":\"s\",\"gen\":\"g\"},"                                         \
    "{\"op\":\"slot_u32\",\"dst\":\"sl\"},{\"op\":\"gen_u32\",\"dst\":\"gn\"},"                                    \
    "{\"op\":\"handle_live\",\"dst\":\"me\",\"slot\":\"sl\",\"gen\":\"gn\"},"                                      \
    "{\"op\":\"handle_live\",\"dst\":\"mix\",\"slot\":\"sl\",\"gen\":\"g\"},"                                      \
    "{\"op\":\"select_u32\",\"dst\":\"v0\",\"cond\":\"h\",\"on_true\":\"one\",\"on_false\":\"zero\"},"             \
    "{\"op\":\"select_u32\",\"dst\":\"v1\",\"cond\":\"me\",\"on_true\":\"one\",\"on_false\":\"zero\"},"            \
    "{\"op\":\"select_u32\",\"dst\":\"v2\",\"cond\":\"mix\",\"on_true\":\"one\",\"on_false\":\"zero\"},"           \
    "{\"op\":\"store_u32\",\"field\":\"o0\",\"src\":\"v0\"},{\"op\":\"store_u32\",\"field\":\"o1\",\"src\":\"v1\"}," \
    "{\"op\":\"store_u32\",\"field\":\"o2\",\"src\":\"v2\"}"

/* Guarded: o_k = handle_live(ts, tg) ? o_k : o_k + k + 1. Every store is its own load when the handle is live, so
 * the planned tier builds an identity-row guard whose slice evaluates handle_live. */
#define GUARD_FIELD(k)                                                                                             \
    "{\"op\":\"load_u32\",\"dst\":\"x" #k "\",\"field\":\"o" #k "\"},"                                             \
    "{\"op\":\"const_u32\",\"dst\":\"c" #k "\",\"value\":" #k "},"                                                 \
    "{\"op\":\"add_u32\",\"dst\":\"y" #k "\",\"a\":\"x" #k "\",\"b\":\"c" #k "\"},"                                \
    "{\"op\":\"add_u32\",\"dst\":\"z" #k "\",\"a\":\"y" #k "\",\"b\":\"one\"},"                                    \
    "{\"op\":\"select_u32\",\"dst\":\"w" #k "\",\"cond\":\"h\",\"on_true\":\"x" #k "\",\"on_false\":\"z" #k "\"}," \
    "{\"op\":\"store_u32\",\"field\":\"o" #k "\",\"src\":\"w" #k "\"}"
#define PROG_GUARD                                                                                                 \
    LOADS ONE_ZERO "{\"op\":\"handle_live\",\"dst\":\"h\",\"slot\":\"s\",\"gen\":\"g\"}," GUARD_FIELD(0) ","       \
    GUARD_FIELD(1) "," GUARD_FIELD(2)

/* ---------- verifier ---------- */

static void test_verifier(void) {
    SuLirModule m;
    SuDiag d;
    char h0[65] = {0}, h1[65] = {0};
    make_capsule("0.4");
    make_lir("0.4", PROG_VALUES);
    expect(__LINE__, SU_OK, NULL);
    if (verify(&m, &d) == SU_OK) {
        CHECK(m.uses_handle_live == 1u);
        CHECK(m.uses_entity_ops == 1u); /* routes the schedule through the entity runtime */
        CHECK(m.uses_gather == 0u);
        CHECK(m.max_struct_per_row == 0u && m.max_events_per_row == 0u); /* needs no capability or cap */
        memcpy(h0, m.semantic_hash_hex, 65);
        su_lir_module_free(&m);
    }
    /* The operands are hashed: swapping slot and generation changes the semantic hash. */
    make_lir("0.4", LOADS ONE_ZERO "{\"op\":\"handle_live\",\"dst\":\"h\",\"slot\":\"g\",\"gen\":\"s\"},"
                    "{\"op\":\"slot_u32\",\"dst\":\"sl\"},{\"op\":\"gen_u32\",\"dst\":\"gn\"},"
                    "{\"op\":\"handle_live\",\"dst\":\"me\",\"slot\":\"sl\",\"gen\":\"gn\"},"
                    "{\"op\":\"handle_live\",\"dst\":\"mix\",\"slot\":\"sl\",\"gen\":\"g\"},"
                    "{\"op\":\"select_u32\",\"dst\":\"v0\",\"cond\":\"h\",\"on_true\":\"one\",\"on_false\":\"zero\"},"
                    "{\"op\":\"select_u32\",\"dst\":\"v1\",\"cond\":\"me\",\"on_true\":\"one\",\"on_false\":\"zero\"},"
                    "{\"op\":\"select_u32\",\"dst\":\"v2\",\"cond\":\"mix\",\"on_true\":\"one\",\"on_false\":\"zero\"},"
                    "{\"op\":\"store_u32\",\"field\":\"o0\",\"src\":\"v0\"},"
                    "{\"op\":\"store_u32\",\"field\":\"o1\",\"src\":\"v1\"},"
                    "{\"op\":\"store_u32\",\"field\":\"o2\",\"src\":\"v2\"}");
    if (verify(&m, &d) == SU_OK) {
        memcpy(h1, m.semantic_hash_hex, 65);
        su_lir_module_free(&m);
    }
    CHECK(h0[0] != '\0' && h1[0] != '\0' && strcmp(h0, h1) != 0);

    /* Unknown below 0.4 (the instruction index is that of the first handle_live). */
    make_capsule("0.3");
    make_lir("0.3", PROG_VALUES);
    expect(__LINE__, SU_E_UNKNOWN_OPCODE, "/instructions/4/op");
    make_capsule("0.4");
    /* Operand register types. */
    make_lir("0.4", "{\"op\":\"load_i32\",\"dst\":\"s\",\"field\":\"ti\"},{\"op\":\"load_u32\",\"dst\":\"g\",\"field\":\"tg\"},"
                    "{\"op\":\"handle_live\",\"dst\":\"h\",\"slot\":\"s\",\"gen\":\"g\"}");
    expect(__LINE__, SU_E_TYPE, "/instructions/2/slot");
    make_lir("0.4", LOADS "{\"op\":\"eq_u32\",\"dst\":\"b\",\"a\":\"s\",\"b\":\"g\"},"
                          "{\"op\":\"handle_live\",\"dst\":\"h\",\"slot\":\"s\",\"gen\":\"b\"}");
    expect(__LINE__, SU_E_TYPE, "/instructions/3/gen");
    /* The result is a bool: storing it into a u32 field is a type error. */
    make_lir("0.4", LOADS "{\"op\":\"handle_live\",\"dst\":\"h\",\"slot\":\"s\",\"gen\":\"g\"},"
                          "{\"op\":\"store_u32\",\"field\":\"o0\",\"src\":\"h\"}");
    expect(__LINE__, SU_E_TYPE, "/instructions/3/src");
    /* Undefined and duplicate registers, missing and unknown keys. */
    make_lir("0.4", LOADS "{\"op\":\"handle_live\",\"dst\":\"h\",\"slot\":\"s\",\"gen\":\"nope\"}");
    expect(__LINE__, SU_E_UNDEFINED_REGISTER, "/instructions/2/gen");
    make_lir("0.4", LOADS "{\"op\":\"handle_live\",\"dst\":\"s\",\"slot\":\"s\",\"gen\":\"g\"}");
    expect(__LINE__, SU_E_DUPLICATE_REGISTER, "/instructions/2/dst");
    make_lir("0.4", LOADS "{\"op\":\"handle_live\",\"dst\":\"h\",\"slot\":\"s\"}");
    expect(__LINE__, SU_E_TYPE, "/instructions/2/gen");
    make_lir("0.4", LOADS "{\"op\":\"handle_live\",\"dst\":\"h\",\"slot\":\"s\",\"gen\":\"g\",\"field\":\"ts\"}");
    expect(__LINE__, SU_E_TYPE, "/instructions/2/field");
    make_lir("0.4", LOADS "{\"op\":\"handle_live\",\"dst\":\"h\",\"slot\":\"s\",\"gen\":7}");
    expect(__LINE__, SU_E_TYPE, "/instructions/2/gen");
}

/* ---------- direct invocation ---------- */

#define MAXR 64u

typedef struct Call {
    uint32_t ts[MAXR], tg[MAXR], ti[MAXR], o_in[3][MAXR], o_out[3][MAXR];
} Call;

static SuColumnBinding bind(uint32_t id, uint32_t access, uint32_t *data, uint64_t rows) {
    SuColumnBinding b;
    memset(&b, 0, sizeof b);
    b.struct_size = (uint32_t)sizeof b;
    b.field_id = id;
    b.elem_type = SU_TYPE_U32;
    b.access = access;
    b.layout_generation = 1u;
    b.stride_bytes = 4u;
    b.row_count = rows;
    b.data = data;
    return b;
}

/* Invokes rows [begin, begin + len) of call c (base_slot begin, entity view slices of the given arrays). */
static uint32_t invoke(SuExecContext *ctx, Call *c, uint32_t begin, uint32_t len, const uint8_t *alive,
                       const uint32_t *gens) {
    SuColumnBinding in[6], out[3];
    SuInvocation inv;
    SuOutputs outs;
    uint32_t k, st;
    in[0] = bind(1u, SU_ACCESS_READ, c->ts + begin, len);
    in[1] = bind(2u, SU_ACCESS_READ, c->tg + begin, len);
    in[2] = bind(3u, SU_ACCESS_READ, c->ti + begin, len);
    for (k = 0; k < 3u; k++) {
        in[3u + k] = bind(10u + k, SU_ACCESS_READ, c->o_in[k] + begin, len);
        out[k] = bind(10u + k, SU_ACCESS_WRITE, c->o_out[k] + begin, len);
    }
    memset(&inv, 0, sizeof inv);
    inv.struct_size = (uint32_t)sizeof inv;
    inv.abi_version = SU_ABI_VERSION;
    inv.layout_generation = 1u;
    inv.binding_count = 6u;
    inv.row_count = len;
    inv.bindings = in;
    memset(&outs, 0, sizeof outs);
    outs.struct_size = (uint32_t)sizeof outs;
    outs.binding_count = 3u;
    outs.failed_insn = UINT32_MAX;
    outs.bindings = out;
    st = su_vm_context_set_entity_view(ctx, alive != NULL ? alive + begin : NULL, gens != NULL ? gens + begin : NULL,
                                       begin, 0u, NULL);
    return st != SU_OK ? st : su_vm_invoke_v1(ctx, &inv, &outs);
}

/* Independent oracle (not su_handle_live). */
static uint32_t oracle(const uint8_t *alive, const uint32_t *gens, uint64_t slots, uint32_t s, uint32_t g) {
    uint32_t stored;
    if (g == 0u || s >= slots) {
        return 0u;
    }
    if (alive != NULL && !alive[s]) {
        return 0u;
    }
    stored = gens != NULL ? gens[s] : 1u;
    return (uint32_t)(stored == g);
}

static int load_module(SuLirModule *m, const char *prog) {
    SuDiag d;
    make_capsule("0.4");
    make_lir("0.4", prog);
    if (verify(m, &d) != SU_OK) {
        fprintf(stderr, "verify: %s %s\n", d.json_pointer, d.message);
        return 0;
    }
    return 1;
}

/* Expected output k of row r for the values program (sv/gv: own slot and generation). */
static uint32_t want_values(uint32_t k, const Call *c, uint32_t r, uint32_t own_slot, uint32_t own_gen,
                            const uint8_t *ta, const uint32_t *tgen, uint64_t slots) {
    switch (k) {
    case 0: return oracle(ta, tgen, slots, c->ts[r], c->tg[r]);
    case 1: return oracle(ta, tgen, slots, own_slot, own_gen);
    default: return oracle(ta, tgen, slots, own_slot, c->tg[r]);
    }
}

#define SENT 0xC0FFEE00u

static void reset_out(Call *c, uint32_t rows) {
    uint32_t k, r;
    for (k = 0; k < 3u; k++) {
        for (r = 0; r < rows; r++) {
            c->o_out[k][r] = SENT + r;
        }
    }
}

/* Known answers. Entity table of 10 slots: slot 2 dead at generation 3 (despawned), slot 4 retired, slot 1 reused
 * (generation 2: handle (1,1) is its earlier occupant), slot 3 at the last generation. */
static void test_known(void) {
    static const uint8_t ALIVE[10] = {1, 1, 0, 1, 0, 1, 1, 1, 1, 1};
    static const uint32_t GEN[10] = {1, 2, 3, 0xFFFFFFFEu, SU_GEN_RETIRED, 5, 1, 7, 1, 9};
    static const uint32_t TS[10] = {0, 1, 1, 2, 3, 4, 10, 0xFFFFFFFFu, 6, 9};
    static const uint32_t TG[10] = {1, 1, 2, 3, 0xFFFFFFFEu, SU_GEN_RETIRED, 1, 7, 0, 9};
    static const uint32_t WANT0[10] = {1, 0, 1, 0, 1, 0, 0, 0, 0, 1};
    SuLirModule m;
    Call c;
    uint32_t tier, r, k;
    memset(&c, 0, sizeof c);
    memcpy(c.ts, TS, sizeof TS);
    memcpy(c.tg, TG, sizeof TG);
    if (!load_module(&m, PROG_VALUES)) {
        CHECK(0);
        return;
    }
    for (tier = SU_VM_TIER_REFERENCE; tier <= SU_VM_TIER_PLANNED; tier++) {
        SuExecContext *ctx = NULL;
        SuVmTierInfo info;
        CHECK(su_vm_context_create(&m, 1u, &ctx) == SU_OK);
        CHECK(su_vm_context_set_tier(ctx, tier) == SU_OK);
        /* (a) no entity table: the call's view over its rows is the table. */
        reset_out(&c, 10u);
        CHECK(invoke(ctx, &c, 0u, 10u, ALIVE, GEN) == SU_OK);
        CHECK(su_vm_context_tier_info(ctx, &info) == SU_OK && info.last_tier == tier);
        for (r = 0; r < 10u; r++) {
            if (!ALIVE[r]) {
                CHECK(c.o_out[0][r] == SENT + r); /* dead rows do not run */
                continue;
            }
            CHECK(c.o_out[0][r] == WANT0[r]);
            CHECK(c.o_out[1][r] == 1u);
            CHECK(c.o_out[2][r] == oracle(ALIVE, GEN, 10u, r, TG[r]));
        }
        /* (b) no view at all: every slot of the call alive at generation 1. */
        reset_out(&c, 10u);
        CHECK(invoke(ctx, &c, 0u, 10u, NULL, NULL) == SU_OK);
        for (r = 0; r < 10u; r++) {
            CHECK(c.o_out[0][r] == (uint32_t)(TS[r] < 10u && TG[r] == 1u));
            CHECK(c.o_out[1][r] == 1u);
        }
        /* (c) chunks at base_slot != 0 need the table; with it every chunk answers from the whole table. */
        reset_out(&c, 10u);
        CHECK(invoke(ctx, &c, 3u, 4u, ALIVE, GEN) == SU_E_INVALID_ARGUMENT);
        CHECK(su_vm_context_set_entity_table(ctx, ALIVE, GEN, 10u) == SU_OK);
        CHECK(invoke(ctx, &c, 0u, 3u, ALIVE, GEN) == SU_OK);
        CHECK(invoke(ctx, &c, 3u, 4u, ALIVE, GEN) == SU_OK);
        CHECK(invoke(ctx, &c, 7u, 3u, ALIVE, GEN) == SU_OK);
        for (r = 0; r < 10u; r++) {
            for (k = 0; k < 3u && ALIVE[r]; k++) {
                CHECK(c.o_out[k][r] == want_values(k, &c, r, r, GEN[r], ALIVE, GEN, 10u));
            }
        }
        /* (d) a table smaller than the call: slots beyond it are not live. */
        reset_out(&c, 10u);
        CHECK(su_vm_context_set_entity_table(ctx, ALIVE, GEN, 4u) == SU_OK);
        CHECK(invoke(ctx, &c, 0u, 10u, ALIVE, GEN) == SU_OK);
        for (r = 0; r < 10u; r++) {
            if (ALIVE[r]) {
                CHECK(c.o_out[0][r] == oracle(ALIVE, GEN, 4u, TS[r], TG[r]));
                CHECK(c.o_out[1][r] == (uint32_t)(r < 4u));
            }
        }
        /* (e) NULL, NULL, 0 clears the table: back to the call's view, which a chunk at 3 cannot use. */
        CHECK(su_vm_context_set_entity_table(ctx, NULL, NULL, 0u) == SU_OK);
        CHECK(invoke(ctx, &c, 3u, 4u, ALIVE, GEN) == SU_E_INVALID_ARGUMENT);
        su_vm_context_destroy(ctx);
    }
    CHECK(su_vm_context_set_entity_table(NULL, NULL, NULL, 0u) == SU_E_INVALID_ARGUMENT);
#ifdef SU_TEST_NATIVE
    {
        SuNativeArtifact *a = NULL;
        SuNativeDiag nd;
        memset(&nd, 0, sizeof nd);
        CHECK(su_native_compile(&m, 1u, SU_NCAP_X86_64_SCALAR, &a, &nd) == SU_E_CAPABILITY_DENIED);
        CHECK(a == NULL && strcmp(nd.message, "ir_0_4_handle_live_not_lowered") == 0);
    }
#endif
    su_lir_module_free(&m);
}

/* ---------- differential: reference vs planned vs oracle ---------- */

static uint64_t g_rng = UINT64_C(0x9E3779B97F4A7C15);

static uint32_t rnd(uint32_t n) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return n == 0u ? (uint32_t)(g_rng >> 32) : (uint32_t)((g_rng >> 32) % n);
}

static void test_diff(void) {
    SuLirModule mv, mg;
    uint32_t iter, guarded_rows = 0, body_rows = 0, mism = 0;
    int has_guard = 0;
    if (!load_module(&mv, PROG_VALUES) || !load_module(&mg, PROG_GUARD)) {
        CHECK(0);
        return;
    }
    for (iter = 0; iter < 400u; iter++) {
        static const uint32_t far[4] = {MAXR, 0x7FFFFFFFu, 0xFFFFFFFEu, 0xFFFFFFFFu};
        uint8_t alive[MAXR];
        uint32_t gens[MAXR];
        Call c[2];
        uint32_t rows = 1u + rnd(MAXR), slots = rnd(4u) == 0u ? rnd(rows + 1u) : rows, r, k, prog;
        int explicit_table = slots != rows || rnd(2u) == 0u;
        for (r = 0; r < MAXR; r++) {
            uint32_t pick = rnd(6u);
            gens[r] = pick == 0u ? 1u : pick == 1u ? 2u : pick == 2u ? 0xFFFFFFFEu : pick == 3u ? SU_GEN_RETIRED : 1u + rnd(1000u);
            alive[r] = (uint8_t)(gens[r] != SU_GEN_RETIRED && rnd(4u) != 0u);
        }
        memset(&c[0], 0, sizeof c[0]);
        for (r = 0; r < rows; r++) {
            uint32_t s = rnd(rows + 2u), kind = rnd(8u);
            uint32_t g = s < MAXR ? gens[s] : 1u;
            if (kind == 3u) {
                g -= 1u;
            } else if (kind == 4u) {
                g += 1u;
            } else if (kind == 5u) {
                g = 0u;
            } else if (kind == 6u) {
                s = far[rnd(4u)];
            } else if (kind == 7u) {
                g = rnd(0u);
            }
            c[0].ts[r] = s;
            c[0].tg[r] = g;
            for (k = 0; k < 3u; k++) {
                c[0].o_in[k][r] = rnd(0u);
            }
        }
        c[1] = c[0];
        for (prog = 0; prog < 2u; prog++) {
            const SuLirModule *m = prog == 0u ? &mv : &mg;
            uint32_t tier;
            for (tier = 0; tier < 2u; tier++) {
                SuExecContext *ctx = NULL;
                uint32_t begin = 0, st = SU_OK;
                reset_out(&c[tier], rows);
                CHECK(su_vm_context_create(m, 1u, &ctx) == SU_OK);
                CHECK(su_vm_context_set_tier(ctx, tier == 0u ? SU_VM_TIER_REFERENCE : SU_VM_TIER_PLANNED) == SU_OK);
                if (explicit_table) {
                    CHECK(su_vm_context_set_entity_table(ctx, alive, gens, slots) == SU_OK);
                }
                /* Explicit table: random chunking; else one call over the table. */
                while (begin < rows && st == SU_OK) {
                    uint32_t len = explicit_table ? 1u + rnd(rows - begin) : rows;
                    st = invoke(ctx, &c[tier], begin, len, alive, gens);
                    begin += len;
                }
                CHECK(st == SU_OK);
                if (tier == 1u && prog == 1u) {
                    SuVmTierInfo info;
                    if (su_vm_context_tier_info(ctx, &info) == SU_OK && info.has_guard) {
                        has_guard = 1;
                    }
                }
                su_vm_context_destroy(ctx);
            }
            for (r = 0; r < rows; r++) {
                uint32_t h = oracle(alive, gens, explicit_table ? slots : rows, c[0].ts[r], c[0].tg[r]);
                for (k = 0; k < 3u; k++) {
                    uint32_t want;
                    if (!alive[r]) {
                        want = SENT + r;
                    } else if (prog == 0u) {
                        want = want_values(k, &c[0], r, r, gens[r], alive, gens, explicit_table ? slots : rows);
                    } else {
                        want = h ? c[0].o_in[k][r] : c[0].o_in[k][r] + k + 1u;
                    }
                    if (c[0].o_out[k][r] != want || c[1].o_out[k][r] != want) {
                        if (mism++ < 5u) {
                            fprintf(stderr, "diff iter %u prog %u row %u out %u: ref 0x%08X planned 0x%08X want 0x%08X\n",
                                    (unsigned)iter, (unsigned)prog, (unsigned)r, (unsigned)k,
                                    (unsigned)c[0].o_out[k][r], (unsigned)c[1].o_out[k][r], (unsigned)want);
                        }
                    }
                }
                if (prog == 1u && alive[r]) {
                    guarded_rows += h;
                    body_rows += h == 0u ? 1u : 0u;
                }
            }
        }
    }
    CHECK(mism == 0u);
    CHECK(has_guard); /* the guard path (handle_live in the guard slice) was exercised */
    CHECK(guarded_rows > 1000u && body_rows > 1000u);
    printf("test_handle_live: differential 400 tables, %u live-handle rows, %u stale rows, guard %s\n",
           (unsigned)guarded_rows, (unsigned)body_rows, has_guard ? "yes" : "no");
    su_lir_module_free(&mv);
    su_lir_module_free(&mg);
}

int main(void) {
    test_verifier();
    test_known();
    test_diff();
    printf("test_handle_live: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
