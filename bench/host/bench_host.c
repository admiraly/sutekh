/* Observation bench for the in-process host API: per-call wall time of su_host_step (1 tick, with a 4-write
 * input record), su_host_snapshot_write, su_host_snapshot_restore, su_host_digest, and the rollback
 * operations su_host_snapshot_restore_inplace, su_host_digest_fast, su_host_events_for_tick and a composite
 * rollback (in-place restore, 8 re-simulated ticks, fast digest), on the examples/ movement capsule
 * (2 fields) with N live entities. Output is one JSON object on stdout.
 *
 * These are observations on the machine that ran it, not qualification against any target (constitution
 * C10). The workload is the 2-field demo capsule; costs scale with fields x capacity and the event log.
 *
 * Usage: bench_host [iterations]   (run from the repository root) */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/su_platform.h"
#include "sutekh/host.h"

#define OPS 8u

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long n;
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    b = malloc((size_t)n + 1u);
    if (b == NULL || fread(b, 1, (size_t)n, f) != (size_t)n) {
        exit(2);
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static uint64_t now_ns(void) {
    uint64_t t = 0;
    (void)su_platform_monotonic_ns(&t);
    return t;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* Median and p95 (nearest rank) in microseconds. */
static void stats(uint64_t *s, uint32_t n, double *median_us, double *p95_us) {
    qsort(s, n, sizeof *s, cmp_u64);
    *median_us = (double)s[n / 2u] / 1000.0;
    *p95_us = (double)s[(uint32_t)((uint64_t)n * 95u / 100u)] / 1000.0;
}

int main(int argc, char **argv) {
    static const uint64_t sizes[] = {64u, 1024u, 16384u};
    static const char *const names[OPS] = {"step_us", "snapshot_write_us", "snapshot_restore_us", "digest_us",
                                           "snapshot_restore_inplace_us", "digest_fast_us", "events_for_tick_us",
                                           "rollback_8_ticks_us"};
    uint32_t iters = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 1000u;
    SuHostCapsule cap;
    uint32_t si;

    if (iters < 20u) {
        iters = 20u;
    }
    cap.capsule_json = slurp("examples/movement.capsule.json", &cap.capsule_len);
    cap.lir_json = slurp("examples/movement.lir.json", &cap.lir_len);
    printf("{\"bench\":\"host_api\",\"kind\":\"observation\",\"iterations\":%u,\"cpus\":%u,\"results\":[",
           (unsigned)iters, (unsigned)su_platform_cpu_count());
    for (si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
        SuHostWorldDesc d;
        SuHostWorld *w = NULL;
        SuHostError e;
        uint64_t *t[OPS];
        uint8_t *snap;
        size_t snap_len = 0;
        uint64_t ents[4];
        uint64_t k;
        uint32_t i;
        uint32_t op;

        memset(&d, 0, sizeof d);
        d.struct_size = sizeof d;
        d.capsule_count = 1;
        d.capsules = &cap;
        d.max_entities = sizes[si];
        d.tick_rate_hz = 120;
        d.event_log_max = 64;
        d.max_struct_per_tick = sizes[si];
        d.max_events_per_tick = sizes[si];
        for (op = 0; op < OPS; op++) {
            t[op] = malloc(iters * sizeof *t[op]);
            if (t[op] == NULL) {
                return 1;
            }
        }
        if (su_host_world_create(&d, &w, &e) != SU_OK) {
            fprintf(stderr, "setup failed: %s\n", e.message);
            return 1;
        }
        for (k = 0; k < sizes[si]; k++) {
            SuHostValue v[2] = {{1u, (uint32_t)k}, {2u, 1u + (uint32_t)(k % 7u)}};
            uint64_t h = 0;
            if (su_host_spawn(w, v, 2, 0, &h, &e) != SU_OK) {
                fprintf(stderr, "spawn failed: %s\n", e.message);
                return 1;
            }
            if (k < 4u) {
                ents[k] = h;
            }
        }
        (void)su_host_snapshot_size(w, &snap_len);
        snap = malloc(snap_len);
        if (snap == NULL) {
            return 1;
        }
        for (i = 0; i < iters; i++) {
            SuHostInput in[4];
            SuHostEvent ev[16];
            uint64_t t0;
            uint64_t fast = 0;
            uint32_t n = 0;
            uint32_t j;
            for (j = 0; j < 4u; j++) {
                in[j].entity = ents[j];
                in[j].field_id = 2u;
                in[j].value = i + j;
            }
            (void)su_host_input_push(w, su_host_tick(w), in, 4, &e);
            t0 = now_ns();
            (void)su_host_step(w, 1, NULL, &e);
            t[0][i] = now_ns() - t0;
            t0 = now_ns();
            (void)su_host_snapshot_write(w, snap, snap_len, &snap_len);
            t[1][i] = now_ns() - t0;
            t0 = now_ns();
            if (su_host_snapshot_restore(w, snap, snap_len, &e) != SU_OK) {
                fprintf(stderr, "restore failed: %s\n", e.message);
                return 1;
            }
            t[2][i] = now_ns() - t0;
            {
                uint8_t dg[32];
                t0 = now_ns();
                (void)su_host_digest(w, dg);
                t[3][i] = now_ns() - t0;
            }
            t0 = now_ns();
            if (su_host_snapshot_restore_inplace(w, snap, snap_len, &e) != SU_OK) {
                fprintf(stderr, "in-place restore failed: %s\n", e.message);
                return 1;
            }
            t[4][i] = now_ns() - t0;
            t0 = now_ns();
            (void)su_host_digest_fast(w, &fast);
            t[5][i] = now_ns() - t0;
            t0 = now_ns();
            (void)su_host_events_for_tick(w, su_host_tick(w), ev, 16, &n, NULL, NULL);
            t[6][i] = now_ns() - t0;
            /* A rollback frame: restore in place, re-run 8 ticks (4 input writes each), fast digest. The
             * world ends 8 ticks ahead of the snapshot; the next iteration snapshots there. */
            t0 = now_ns();
            (void)su_host_snapshot_restore_inplace(w, snap, snap_len, &e);
            for (j = 0; j < 8u; j++) {
                uint32_t q;
                for (q = 0; q < 4u; q++) {
                    in[q].value = i + j + q;
                }
                (void)su_host_input_push(w, su_host_tick(w), in, 4, &e);
                (void)su_host_step(w, 1, NULL, &e);
            }
            (void)su_host_digest_fast(w, &fast);
            t[7][i] = now_ns() - t0;
        }
        printf("%s{\"entities\":%llu,\"snapshot_bytes\":%zu", si > 0u ? "," : "", (unsigned long long)sizes[si],
               snap_len);
        for (op = 0; op < OPS; op++) {
            double m = 0.0;
            double p = 0.0;
            stats(t[op], iters, &m, &p);
            printf(",\"%s\":{\"median\":%.2f,\"p95\":%.2f}", names[op], m, p);
            free(t[op]);
        }
        printf("}");
        free(snap);
        su_host_world_destroy(w);
    }
    printf("]}\n");
    free((void *)cap.capsule_json);
    free((void *)cap.lir_json);
    return 0;
}
