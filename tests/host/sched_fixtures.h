/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * Shared builders for the schedule-size and declared-schema tests (issue 144): test_host_schedule.c (new API) and
 * test_host_compat.c (first-release API only, so the same source also builds against an older library).
 *
 * The chain world (schema "t.chain", n systems, fields v00..v<n>, IDs 1..n+1, u32): system k ("chain.sKK") reads
 * v<k> and v<k+1> and stores v<k+1> = v<k> * 3 + v<k+1> + (k + 1) (mod 2^32), with an `after` edge to system k - 1,
 * so a tick is correct only when the systems run in capsule order. */
#ifndef SU_TEST_SCHED_FIXTURES_H
#define SU_TEST_SCHED_FIXTURES_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Buf {
    char *p;
    size_t n, cap;
} Buf;

static void put(Buf *b, const char *fmt, ...) {
    va_list ap;
    int k;
    for (;;) {
        va_start(ap, fmt);
        k = vsnprintf(b->p != NULL ? b->p + b->n : NULL, b->p != NULL ? b->cap - b->n : 0u, fmt, ap);
        va_end(ap);
        if (k < 0) {
            fprintf(stderr, "vsnprintf failed\n");
            exit(2);
        }
        if (b->p != NULL && b->n + (size_t)k < b->cap) {
            b->n += (size_t)k;
            return;
        }
        b->cap = (b->cap + (size_t)k + 256u) * 2u;
        b->p = realloc(b->p, b->cap);
        if (b->p == NULL) {
            fprintf(stderr, "out of memory\n");
            exit(2);
        }
    }
}

static void buf_free(Buf *b) {
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0u;
}

#define CHAIN_MAX 72u

/* edge: the `after` target of system k ("" for none, NULL for the default chain.s<k-1>). */
static Buf chain_capsule(uint32_t k, const char *schema, const char *edge) {
    Buf b = {NULL, 0, 0};
    put(&b, "{\"capsule_format\":\"0.1\",\"system_id\":\"chain.s%02u\",\"source\":\"inline.lir.json\",", (unsigned)k);
    put(&b, "\"schema\":{\"id\":\"%s\",\"version\":1,\"layout_generation\":1},\"numeric_profile\":\"u32_mod_v1\",",
        schema);
    put(&b, "\"fields\":[{\"id\":%u,\"name\":\"v%02u\",\"type\":\"u32\",\"read\":true,\"write\":false},", (unsigned)(k + 1u),
        (unsigned)k);
    put(&b, "{\"id\":%u,\"name\":\"v%02u\",\"type\":\"u32\",\"read\":true,\"write\":true}],", (unsigned)(k + 2u),
        (unsigned)(k + 1u));
    if (edge == NULL && k > 0u) {
        put(&b, "\"schedule\":{\"phase\":\"simulate\",\"after\":[\"chain.s%02u\"],\"before\":[],\"parallel\":\"serial\"},",
            (unsigned)(k - 1u));
    } else if (edge != NULL && edge[0] != '\0') {
        put(&b, "\"schedule\":{\"phase\":\"simulate\",\"after\":[\"%s\"],\"before\":[],\"parallel\":\"serial\"},", edge);
    } else {
        put(&b, "\"schedule\":{\"phase\":\"simulate\",\"after\":[],\"before\":[],\"parallel\":\"serial\"},");
    }
    put(&b, "\"limits\":{\"max_instructions\":4096,\"max_rows\":1000000,\"allocation\":\"forbidden\"},");
    put(&b, "\"tests\":[\"tests/host/test_host_schedule.c\"]}");
    return b;
}

static Buf chain_lir(uint32_t k, const char *schema) {
    Buf b = {NULL, 0, 0};
    put(&b, "{\"ir_version\":\"0.1\",\"system_id\":\"chain.s%02u\",\"numeric_profile\":\"u32_mod_v1\",\"query\":\"%s\",",
        (unsigned)k, schema);
    put(&b, "\"instructions\":[{\"op\":\"load_u32\",\"dst\":\"a\",\"field\":\"v%02u\"},", (unsigned)k);
    put(&b, "{\"op\":\"load_u32\",\"dst\":\"b\",\"field\":\"v%02u\"},", (unsigned)(k + 1u));
    put(&b, "{\"op\":\"const_u32\",\"dst\":\"three\",\"value\":3},{\"op\":\"mul_u32\",\"dst\":\"m\",\"a\":\"a\",\"b\":\"three\"},");
    put(&b, "{\"op\":\"add_u32\",\"dst\":\"s\",\"a\":\"m\",\"b\":\"b\"},{\"op\":\"const_u32\",\"dst\":\"kk\",\"value\":%u},",
        (unsigned)(k + 1u));
    put(&b, "{\"op\":\"add_u32\",\"dst\":\"r\",\"a\":\"s\",\"b\":\"kk\"},");
    put(&b, "{\"op\":\"store_u32\",\"field\":\"v%02u\",\"src\":\"r\"}]}", (unsigned)(k + 1u));
    return b;
}

static uint32_t chain_init(uint32_t row, uint32_t field) {
    return row * 7u + field * 13u + 1u;
}

/* Fixture: rows entities (IDs 1..rows), columns v00..v<n>. */
static Buf chain_fixture(uint32_t n, uint32_t rows, const char *schema) {
    Buf b = {NULL, 0, 0};
    uint32_t i, f;
    put(&b, "{\"fixture_format\":\"0.1\",\"schema_id\":\"%s\",\"schema_version\":1,\"tick\":0,\"entity_ids\":[", schema);
    for (i = 0; i < rows; i++) {
        put(&b, "%s%u", i > 0u ? "," : "", (unsigned)(i + 1u));
    }
    put(&b, "],\"columns\":{");
    for (f = 0; f <= n; f++) {
        put(&b, "%s\"v%02u\":[", f > 0u ? "," : "", (unsigned)f);
        for (i = 0; i < rows; i++) {
            put(&b, "%s%u", i > 0u ? "," : "", (unsigned)chain_init(i, f));
        }
        put(&b, "]");
    }
    put(&b, "}}");
    return b;
}

/* Reference model of one tick for one row: v has n + 1 entries. */
static void chain_model_tick(uint32_t *v, uint32_t n) {
    uint32_t k;
    for (k = 0; k < n; k++) {
        v[k + 1u] = v[k] * 3u + v[k + 1u] + (k + 1u);
    }
}

/* Owns the bytes of an n-system chain world description. */
typedef struct Chain {
    uint32_t n;
    Buf cap[CHAIN_MAX], lir[CHAIN_MAX], fixture;
    SuHostCapsule caps[CHAIN_MAX];
} Chain;

/* edges: NULL for the default chain, else edges[k] as for chain_capsule. */
static void chain_build(Chain *c, uint32_t n, uint32_t rows, const char *const *edges) {
    uint32_t k;
    memset(c, 0, sizeof *c);
    c->n = n;
    for (k = 0; k < n; k++) {
        c->cap[k] = chain_capsule(k, "t.chain", edges != NULL ? edges[k] : NULL);
        c->lir[k] = chain_lir(k, "t.chain");
        c->caps[k].capsule_json = (const uint8_t *)c->cap[k].p;
        c->caps[k].capsule_len = c->cap[k].n;
        c->caps[k].lir_json = (const uint8_t *)c->lir[k].p;
        c->caps[k].lir_len = c->lir[k].n;
    }
    c->fixture = chain_fixture(n, rows, "t.chain");
}

static void chain_free(Chain *c) {
    uint32_t k;
    for (k = 0; k < c->n; k++) {
        buf_free(&c->cap[k]);
        buf_free(&c->lir[k]);
    }
    buf_free(&c->fixture);
}

#endif
