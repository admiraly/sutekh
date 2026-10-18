/* Observation bench for the in-process host API: per-call wall time of su_host_step (1 tick, with a 4-write
 * input record), su_host_snapshot_write, su_host_snapshot_restore and su_host_digest, on the examples/
 * movement capsule (2 fields) with N live entities. Output is one JSON object on stdout.
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
        uint64_t *t_step = malloc(iters * sizeof *t_step);
        uint64_t *t_snap = malloc(iters * sizeof *t_snap);
        uint64_t *t_rest = malloc(iters * sizeof *t_rest);
        uint64_t *t_dig = malloc(iters * sizeof *t_dig);
        uint8_t *snap;
        size_t snap_len = 0;
        uint64_t ents[4];
        uint64_t k;
        uint32_t i;
        double m[4];
        double p[4];

        memset(&d, 0, sizeof d);
        d.struct_size = sizeof d;
        d.capsule_count = 1;
        d.capsules = &cap;
        d.max_entities = sizes[si];
        d.tick_rate_hz = 120;
        d.event_log_max = 64;
        d.max_struct_per_tick = sizes[si];
        d.max_events_per_tick = sizes[si];
        if (t_step == NULL || t_snap == NULL || t_rest == NULL || t_dig == NULL ||
            su_host_world_create(&d, &w, &e) != SU_OK) {
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
            uint64_t t0;
            uint32_t j;
            for (j = 0; j < 4u; j++) {
                in[j].entity = ents[j];
                in[j].field_id = 2u;
                in[j].value = i + j;
            }
            (void)su_host_input_push(w, su_host_tick(w), in, 4, &e);
            t0 = now_ns();
            (void)su_host_step(w, 1, NULL, &e);
            t_step[i] = now_ns() - t0;
            t0 = now_ns();
            (void)su_host_snapshot_write(w, snap, snap_len, &snap_len);
            t_snap[i] = now_ns() - t0;
            t0 = now_ns();
            if (su_host_snapshot_restore(w, snap, snap_len, &e) != SU_OK) {
                fprintf(stderr, "restore failed: %s\n", e.message);
                return 1;
            }
            t_rest[i] = now_ns() - t0;
            {
                uint8_t dg[32];
                t0 = now_ns();
                (void)su_host_digest(w, dg);
                t_dig[i] = now_ns() - t0;
            }
        }
        stats(t_step, iters, &m[0], &p[0]);
        stats(t_snap, iters, &m[1], &p[1]);
        stats(t_rest, iters, &m[2], &p[2]);
        stats(t_dig, iters, &m[3], &p[3]);
        printf("%s{\"entities\":%llu,\"snapshot_bytes\":%zu,"
               "\"step_us\":{\"median\":%.2f,\"p95\":%.2f},\"snapshot_write_us\":{\"median\":%.2f,\"p95\":%.2f},"
               "\"snapshot_restore_us\":{\"median\":%.2f,\"p95\":%.2f},\"digest_us\":{\"median\":%.2f,\"p95\":%.2f}}",
               si > 0u ? "," : "", (unsigned long long)sizes[si], snap_len, m[0], p[0], m[1], p[1], m[2], p[2], m[3],
               p[3]);
        free(snap);
        free(t_step);
        free(t_snap);
        free(t_rest);
        free(t_dig);
        su_host_world_destroy(w);
    }
    printf("]}\n");
    free((void *)cap.capsule_json);
    free((void *)cap.lir_json);
    return 0;
}
