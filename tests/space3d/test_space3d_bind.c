/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * Schema-relative binding of the space3d.query host operator (src/runtime/rts/host_decl.h, su_host_load_bound;
 * src/runtime/schedule.h, su_sched_add_host_bound). Records are written by this test from the operator
 * descriptor: a valid record binds every operator lane to a lane of a game schema ("racer_game", IDs + 1000,
 * names prefixed "g_"); each altered record breaks one rule of the contract and must be refused with
 * SU_E_SCHEMA_MISMATCH. The schedule refuses a declaration whose lanes do not match the binding.
 * Run from the repository root (the base record supplies the operator identity). Usage: test_space3d_bind <scratch> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_ops03.h"
#include "runtime/rts/host_decl.h"
#include "runtime/schedule.h"
#include "space3d_ops.h"
#include "sutekh/status.h"

static int g_checks = 0, g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        g_checks++;                                                      \
        if (!(c)) {                                                      \
            g_fail++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        }                                                                \
    } while (0)

/* Alterations of the generated record. */
enum {
    V_VALID = 0,
    V_MISSING_BINDS,  /* one entry without binds */
    V_DUP_BINDS,      /* two entries bind the same lane */
    V_UNKNOWN_BINDS,  /* binds names no operator lane */
    V_TYPE,           /* an entry's type differs from its lane */
    V_ACCESS,         /* an entry's access differs from its lane */
    V_ORDER,          /* IDs not ascending */
    V_GAP,            /* the lanes of a 64-bit field are not at J, J + 1 */
    V_SWAP,           /* lane 1 bound before lane 0 */
    V_DUP_NAME,       /* two entries share a name */
    V_OPERATOR,       /* wrong operator family */
    V_COUNT
};

static const char *access_name(uint32_t a) {
    return a == (SU_ACCESS_READ | SU_ACCESS_WRITE) ? "read_write" : (a == SU_ACCESS_WRITE ? "write" : "read");
}

static int write_record(const char *path, int variant, const char *schema_id) {
    const SuHostOpDesc *d = &g_space3d_query_desc;
    FILE *f = fopen(path, "wb");
    char hex[65];
    uint32_t k;
    if (f == NULL) {
        return 0;
    }
    for (k = 0; k < 32u; k++) {
        snprintf(hex + 2u * k, 3, "%02x", d->constants_hash[k]);
    }
    fprintf(f, "{\"capsule_format\":\"0.1\",\"system_id\":\"%s\",\"system_rev\":1,\"capabilities\":[],", d->system_id);
    fprintf(f, "\"schema\":{\"id\":\"%s\",\"version\":1,\"layout_generation\":1},", schema_id);
    fprintf(f, "\"numeric_profile\":\"fixed_q32_v1\",\"host\":{\"operator\":\"%s\",\"operator_version\":%u,"
               "\"constants_hash\":\"%s\",\"fields\":[",
            variant == V_OPERATOR ? "rts_ops_v1" : "space3d_ops_v1", (unsigned)d->operator_version, hex);
    for (k = 0; k < d->field_count; k++) {
        const SuHostField *x = &d->fields[k];
        uint32_t id = x->id + 1000u;
        const char *binds = x->name;
        const char *type = su_type_name(x->type);
        const char *acc = access_name(x->access);
        char name[96];
        snprintf(name, sizeof name, "g_%s", x->name);
        if (variant == V_DUP_BINDS && k == 1u) binds = d->fields[0].name;
        if (variant == V_UNKNOWN_BINDS && k == 2u) binds = "no_such_lane";
        if (variant == V_TYPE && k == 0u) type = "i32";
        if (variant == V_ACCESS && k == 0u) acc = "read_write";
        if (variant == V_ORDER && k == 1u) id = 1000u;
        if (variant == V_GAP && x->id > SPACE3D_F_POS_X) id += 1u; /* pos_x lanes at J, J + 2; later pairs intact */
        if (variant == V_DUP_NAME && k == 1u) snprintf(name, sizeof name, "g_%s", d->fields[0].name);
        if (variant == V_SWAP && x->id == SPACE3D_F_POS_X) binds = d->fields[k + 1u].name;
        if (variant == V_SWAP && x->id == SPACE3D_F_POS_X + 1u) binds = d->fields[k - 1u].name;
        fprintf(f, "%s{\"id\":%u,\"name\":\"%s\",\"type\":\"%s\",\"access\":\"%s\"", k ? "," : "", (unsigned)id, name, type,
                acc);
        if (!(variant == V_MISSING_BINDS && k == 3u)) {
            fprintf(f, ",\"binds\":\"%s\"", binds);
        }
        fprintf(f, "}");
    }
    fprintf(f, "]},\"schedule\":{\"phase\":\"simulate\",\"after\":[],\"before\":[],\"parallel\":\"serial\"}}");
    return fclose(f) == 0;
}

int main(int argc, char **argv) {
    const SuHostOpDesc *d = &g_space3d_query_desc;
    char path[1024], msg[256];
    uint32_t bind[SU_LIMIT_FIELDS], sv = 0, gen = 0, k, st;
    int bound = 0, v;
    SuLirModule *decl = NULL, *plain = NULL;
    const char *scratch = argc > 1 ? argv[1] : ".";

    /* The committed record: unbound, identity binding, unchanged semantic hash. */
    CHECK(su_host_load_bound("profiles/space3d/space3d_query.capsule.json", NULL, d, &plain, &sv, &gen, bind, &bound,
                             msg, sizeof msg) == SU_OK);
    CHECK(plain != NULL && bound == 0);
    for (k = 0; plain != NULL && k < d->field_count; k++) {
        CHECK(bind[k] == d->fields[k].id);
    }

    /* A valid bound record. */
    snprintf(path, sizeof path, "%s/bound_valid.capsule.json", scratch);
    CHECK(write_record(path, V_VALID, "racer_game"));
    st = su_host_load_bound(path, NULL, d, &decl, &sv, &gen, bind, &bound, msg, sizeof msg);
    if (st != SU_OK) {
        fprintf(stderr, "valid bound record refused: %s\n", msg);
    }
    CHECK(st == SU_OK && decl != NULL && bound == 1);
    if (decl != NULL) {
        CHECK(strcmp(decl->query, "racer_game") == 0);
        CHECK(decl->field_count == d->field_count);
        for (k = 0; k < d->field_count; k++) {
            char want[96];
            snprintf(want, sizeof want, "g_%s", d->fields[k].name);
            CHECK(bind[k] == d->fields[k].id + 1000u);
            CHECK(decl->fields[k].id == d->fields[k].id + 1000u && strcmp(decl->fields[k].name, want) == 0);
            CHECK(SU_LIR_VALUE_TYPE(&decl->fields[k]) == d->fields[k].type && decl->fields[k].access == d->fields[k].access);
        }
        CHECK(plain != NULL && strcmp(decl->semantic_hash_hex, plain->semantic_hash_hex) != 0);
        {
            /* The schedule accepts it under the binding and takes the game's schema; it refuses a wrong table. */
            SuSchedule s;
            uint32_t wrong[SU_LIMIT_FIELDS];
            su_sched_init(&s);
            memcpy(wrong, bind, sizeof wrong);
            wrong[0] = wrong[1];
            CHECK(su_sched_add_host_bound(&s, d, decl, wrong, sv, gen, 0, 1, 0) == SU_E_SCHEMA_MISMATCH);
            CHECK(su_sched_add_host(&s, d, decl, sv, gen, 0, 1, 0) == SU_E_SCHEMA_MISMATCH);
            CHECK(su_sched_add_host_bound(&s, d, decl, bind, sv, gen, 0, 1, 0) == SU_OK);
            CHECK(s.count == 1u && strcmp(s.schema_id, "racer_game") == 0 && s.sys[0].host_bind != NULL);
            CHECK(su_sched_finalize(&s) == SU_OK);
            su_sched_free(&s); /* frees decl and the copied binding */
            decl = NULL;
        }
    }
    /* The unbound API refuses a bound record. */
    CHECK(su_host_load(path, NULL, d, &decl, &sv, &gen, msg, sizeof msg) == SU_E_SCHEMA_MISMATCH && decl == NULL);

    /* Every altered record is refused. */
    for (v = V_MISSING_BINDS; v < V_COUNT; v++) {
        snprintf(path, sizeof path, "%s/bound_%d.capsule.json", scratch, v);
        CHECK(write_record(path, v, "racer_game"));
        st = su_host_load_bound(path, NULL, d, &decl, &sv, &gen, bind, &bound, msg, sizeof msg);
        printf("variant %d: %s (%s)\n", v, su_status_name(st), msg);
        CHECK(st == SU_E_SCHEMA_MISMATCH && decl == NULL);
        if (decl != NULL) {
            su_host_decl_free(decl);
            decl = NULL;
        }
    }
    /* An unbound record must still name the operator's schema. */
    {
        FILE *in = fopen("profiles/space3d/space3d_query.capsule.json", "rb");
        static char buf[65536];
        size_t n = in != NULL ? fread(buf, 1, sizeof buf - 1u, in) : 0u;
        char *p;
        FILE *o;
        if (in != NULL) {
            fclose(in);
        }
        buf[n] = '\0';
        p = strstr(buf, "\"id\": \"space3d\"");
        CHECK(p != NULL);
        snprintf(path, sizeof path, "%s/unbound_other_schema.capsule.json", scratch);
        o = fopen(path, "wb");
        if (p != NULL && o != NULL) {
            fwrite(buf, 1, (size_t)(p - buf), o);
            fputs("\"id\": \"racer_game\"", o);
            fputs(p + strlen("\"id\": \"space3d\""), o);
            fclose(o);
            CHECK(su_host_load_bound(path, NULL, d, &decl, &sv, &gen, bind, &bound, msg, sizeof msg) ==
                      SU_E_SCHEMA_MISMATCH &&
                  decl == NULL);
        }
    }
    su_host_decl_free(plain);
    printf("space3d_bind: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
