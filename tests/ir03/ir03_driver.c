/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * IR 0.3 differential driver: verifies a capsule/IR pair, builds a world from the verified lane table, fills
 * every lane with seeded biased values, runs the module once through su_vm_run_system, and prints the inputs and
 * the committed outputs, one lane per line:
 *   STATUS <code> <semantic hash>
 *   IN <row> <lane name> <u32>
 *   OUT <row> <lane name> <u32>
 * tests/ir03/ir03_diff.py recomputes every OUT value with an independent interpreter.
 *
 *   ir03_driver <capsule> <lir> <rows> <seed>
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_lir.h"
#include "ir/su_num.h"
#include "sutekh/status.h"
#include "vm/su_vm.h"
#include "world/su_world.h"

static uint64_t g_rng = 0;
static uint64_t rng_next(void) {
    uint64_t z = (g_rng += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static int64_t rnd_q(void) {
    static const int64_t edges[] = {0, 1, -1, INT64_MAX, INT64_MIN, SU_Q32_ONE, -SU_Q32_ONE, SU_Q32_HALF,
                                    -SU_Q32_HALF, 3 * SU_Q32_HALF, INT64_C(0x7FFFFFFF)};
    uint64_t r = rng_next();
    switch (rng_next() % 6u) {
    case 0: return edges[r % (sizeof edges / sizeof edges[0])];
    case 1: return su_i64_from_bits(r);
    case 2: return su_i64_from_bits(r) >> 23; /* |x| < 256 */
    case 3: return su_i64_from_bits(r) >> 13; /* |x| < 262 km */
    case 4: return su_i64_from_bits(r) >> 30; /* |x| < 8 */
    default: return (su_i64_from_bits(r) >> 44) * SU_Q32_HALF;
    }
}

static uint32_t rnd_32(uint32_t vt) {
    uint64_t r = rng_next();
    if (vt == SU_TYPE_U32) {
        return (r & 3u) != 0u ? (uint32_t)(r >> 2) % 70u : (uint32_t)(r >> 32);
    }
    switch (r % 4u) {
    case 0: return (uint32_t)(r >> 32);
    case 1: return (uint32_t)(int32_t)((int64_t)(r >> 33) % 141 - 70);
    case 2: return (uint32_t)(int32_t)((int64_t)(r >> 33) % 2001 - 1000);
    default: {
        static const uint32_t e[] = {0u, 1u, 0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 2u, 0xFFFFFFFEu};
        return e[(r >> 8) % 7u];
    }
    }
}

static int slurp(const char *p, uint8_t **b, size_t *n) {
    FILE *f = fopen(p, "rb");
    long sz;
    if (f == NULL) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    rewind(f);
    *b = malloc((size_t)sz + 1u);
    *n = *b != NULL ? fread(*b, 1, (size_t)sz, f) : 0u;
    fclose(f);
    return *b != NULL && *n == (size_t)sz;
}

int main(int argc, char **argv) {
    uint8_t *cb = NULL, *lb = NULL;
    size_t cn = 0, ln = 0;
    SuLirModule m;
    SuDiag d;
    SuWorldFieldDesc *fd;
    SuWorldDesc wd;
    uint32_t *ids, **cols;
    SuWorld *w = NULL;
    SuExecContext *ctx = NULL;
    uint32_t st, i, rows, r;

    if (argc != 5 || !slurp(argv[1], &cb, &cn) || !slurp(argv[2], &lb, &ln)) {
        fprintf(stderr, "usage: ir03_driver <capsule> <lir> <rows> <seed>\n");
        return 2;
    }
    rows = (uint32_t)strtoul(argv[3], NULL, 10);
    g_rng = (uint64_t)strtoull(argv[4], NULL, 10);
    st = su_lir_verify(cb, cn, lb, ln, &m, &d);
    if (st != SU_OK) {
        printf("STATUS %u - %s %s\n", (unsigned)st, d.json_pointer, d.message);
        return 0;
    }
    fd = calloc(m.field_count, sizeof *fd);
    ids = calloc(rows + 1u, sizeof *ids);
    cols = calloc(m.field_count, sizeof *cols);
    for (i = 0; i < m.field_count; i++) {
        fd[i].id = m.fields[i].id;
        fd[i].type = SU_TYPE_U32;
        fd[i].access = m.fields[i].access;
        fd[i].name = m.fields[i].name;
        cols[i] = calloc(rows + 1u, sizeof **cols);
    }
    for (r = 0; r < rows; r++) {
        ids[r] = r + 1u;
    }
    memset(&wd, 0, sizeof wd);
    wd.schema_id = m.query;
    wd.schema_version = 1;
    wd.layout_generation = 1;
    wd.row_count = rows;
    wd.entity_ids = ids;
    wd.fields = fd;
    wd.field_count = m.field_count;
    st = su_world_create(&wd, &w);
    if (st != SU_OK) {
        printf("STATUS %u - world\n", (unsigned)st);
        return 1;
    }
    for (r = 0; r < rows; r++) {
        for (i = 0; i < m.field_count; i++) {
            uint32_t vt = SU_LIR_VALUE_TYPE(&m.fields[i]);
            if (vt == SU_TYPE_I64 || vt == SU_TYPE_Q32) {
                uint64_t v = (uint64_t)(vt == SU_TYPE_Q32 ? rnd_q() : su_i64_from_bits(rng_next()) >> (rng_next() % 64u));
                cols[i][r] = (uint32_t)(v & 0xFFFFFFFFu);
                cols[i + 1u][r] = (uint32_t)(v >> 32);
                i++;
            } else {
                cols[i][r] = rnd_32(vt);
            }
        }
    }
    for (i = 0; i < m.field_count; i++) {
        su_world_init_committed(w, m.fields[i].id, cols[i], rows);
    }
    st = su_vm_context_create(&m, 1, &ctx);
    if (st == SU_OK) st = su_world_begin_tick(w);
    if (st == SU_OK) st = su_vm_run_system(w, ctx);
    if (st == SU_OK) st = su_world_publish(w);
    else su_world_abort(w);
    printf("STATUS %u %s\n", (unsigned)st, m.semantic_hash_hex);
    for (r = 0; r < rows; r++) {
        for (i = 0; i < m.field_count; i++) {
            printf("IN %u %s %u\n", (unsigned)r, m.fields[i].name, (unsigned)cols[i][r]);
        }
    }
    if (st == SU_OK) {
        for (i = 0; i < m.field_count; i++) {
            const uint32_t *c;
            uint64_t n;
            if ((m.fields[i].access & SU_ACCESS_WRITE) == 0u) {
                continue;
            }
            su_world_read_committed(w, m.fields[i].id, &c, &n);
            for (r = 0; r < rows; r++) {
                printf("OUT %u %s %u\n", (unsigned)r, m.fields[i].name, (unsigned)c[r]);
            }
        }
    }
    su_vm_context_destroy(ctx);
    su_world_destroy(w);
    for (i = 0; i < m.field_count; i++) {
        free(cols[i]);
    }
    free(cols);
    free(ids);
    free(fd);
    su_lir_module_free(&m);
    free(cb);
    free(lb);
    return 0;
}
