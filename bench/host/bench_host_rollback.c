/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * Client-side rollback through the public host API (include/sutekh/host.h) on the 6dof_racer_microcosm world:
 * racer.race, racer.move (IR 0.3) and the space3d.query host record, created from capsule bytes and a fixture
 * written by `racer_microcosm --dump-fixture` (bench/host/run_host_rollback.py runs the matrix).
 *
 *   bench_host_rollback --root <repo> --fixture <json> --n <racers> [--tier reference|planned|native]
 *                       [--k 6,12] [--iters 200] [--warmup 120]
 *
 * After the warm-up the world is saved (su_host_snapshot_write into a preallocated buffer). Per K: restore in
 * place and step K ticks in one su_host_step call once to obtain the reference SHA-256 and fast digests, then
 * --iters times: su_host_snapshot_restore_inplace, su_host_step(K), su_host_digest_fast, each timed. Every
 * iteration's fast digest must equal the reference (exit 1 otherwise); the reference SHA-256 is printed so the
 * driver can compare tiers. Timing is an observation on the machine that ran it, not qualification. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/su_platform.h"
#include "sutekh/host.h"

static uint8_t *slurp(const char *root, const char *rel, size_t *len) {
    char path[1024];
    FILE *f;
    long sz;
    uint8_t *b = NULL;
    if (root != NULL) {
        snprintf(path, sizeof path, "%s/%s", root, rel);
    } else {
        snprintf(path, sizeof path, "%s", rel);
    }
    *len = 0;
    f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    if (fseek(f, 0, SEEK_END) == 0 && (sz = ftell(f)) >= 0) {
        rewind(f);
        b = malloc((size_t)sz + 1u);
        if (b != NULL && fread(b, 1, (size_t)sz, f) == (size_t)sz) {
            *len = (size_t)sz;
        } else {
            free(b);
            b = NULL;
        }
    }
    fclose(f);
    if (b == NULL) {
        exit(2);
    }
    return b;
}

static int cmp_u64(const void *x, const void *y) {
    uint64_t a = *(const uint64_t *)x, b = *(const uint64_t *)y;
    return (a > b) - (a < b);
}

static uint64_t pct(const uint64_t *sorted, uint32_t n, uint32_t p) {
    uint32_t k = (uint32_t)(((uint64_t)p * n + 99u) / 100u);
    return sorted[k == 0 ? 0 : k - 1u];
}

static void stats(const char *name, uint64_t *v, uint32_t n) {
    qsort(v, n, sizeof *v, cmp_u64);
    printf("\"%s\":{\"min\":%" PRIu64 ",\"p50\":%" PRIu64 ",\"p95\":%" PRIu64 ",\"p99\":%" PRIu64 ",\"max\":%" PRIu64
           "},",
           name, v[0], pct(v, n, 50), pct(v, n, 95), pct(v, n, 99), v[n - 1u]);
}

static uint64_t now_ns(void) {
    uint64_t t = 0;
    su_platform_monotonic_ns(&t);
    return t;
}

static void step_or_die(SuHostWorld *w, uint64_t n) {
    uint64_t done = 0;
    SuHostError err;
    uint32_t st = su_host_step(w, n, &done, &err);
    if (st != SU_OK || done != n) {
        fprintf(stderr, "step: %s (%s)\n", su_host_status_name(st), err.message);
        exit(1);
    }
}

int main(int argc, char **argv) {
    const char *root = ".", *fixture = NULL, *tier_name = "planned";
    uint32_t n = 8, iters = 200, warmup = 120, ks[8] = {6, 12}, nk = 2, i, k, it;
    uint32_t tier = SU_HOST_TIER_PLANNED;
    uint8_t *race_cap, *race_lir, *move_cap, *move_lir, *query, *fix;
    size_t race_cap_n, race_lir_n, move_cap_n, move_lir_n, query_n, fix_n;
    SuHostCapsule caps[3];
    SuHostWorldDesc d;
    SuHostWorld *w = NULL;
    SuHostError err;
    uint8_t *snap, *scratch;
    size_t snap_len = 0, got = 0;
    uint64_t *t_save, *t_restore, *t_resim, *t_digest, *t_total;
    int ok = 1;

    for (i = 1; i < (uint32_t)argc; i++) {
        const char *a = argv[i], *v = i + 1u < (uint32_t)argc ? argv[i + 1u] : "";
        if (strcmp(a, "--root") == 0) { root = v; i++; }
        else if (strcmp(a, "--fixture") == 0) { fixture = v; i++; }
        else if (strcmp(a, "--n") == 0) { n = (uint32_t)strtoul(v, NULL, 10); i++; }
        else if (strcmp(a, "--iters") == 0) { iters = (uint32_t)strtoul(v, NULL, 10); i++; }
        else if (strcmp(a, "--warmup") == 0) { warmup = (uint32_t)strtoul(v, NULL, 10); i++; }
        else if (strcmp(a, "--tier") == 0) {
            tier_name = v;
            tier = strcmp(v, "reference") == 0 ? SU_HOST_TIER_REFERENCE
                   : strcmp(v, "planned") == 0 ? SU_HOST_TIER_PLANNED
                   : strcmp(v, "native") == 0  ? SU_HOST_TIER_NATIVE
                                               : 0u;
            i++;
        } else if (strcmp(a, "--k") == 0) {
            const char *p = v;
            char *e = NULL;
            nk = 0;
            while (*p != '\0' && nk < 8u) {
                ks[nk++] = (uint32_t)strtoul(p, &e, 10);
                if (e == p) {
                    break;
                }
                p = *e == ',' ? e + 1 : e;
            }
            i++;
        } else {
            fprintf(stderr, "unknown argument %s\n", a);
            return 2;
        }
    }
    if (fixture == NULL || tier == 0u || iters == 0u || nk == 0u || n == 0u) {
        fprintf(stderr, "usage: bench_host_rollback --root <repo> --fixture <json> --n <racers> "
                        "[--tier reference|planned|native] [--k 6,12] [--iters 200] [--warmup 120]\n");
        return 2;
    }
    race_cap = slurp(root, "capsules/space3d_racer/race.capsule.json", &race_cap_n);
    race_lir = slurp(root, "capsules/space3d_racer/race.lir.json", &race_lir_n);
    move_cap = slurp(root, "capsules/space3d_racer/move.capsule.json", &move_cap_n);
    move_lir = slurp(root, "capsules/space3d_racer/move.lir.json", &move_lir_n);
    query = slurp(root, "profiles/space3d/space3d_query.capsule.json", &query_n);
    fix = slurp(NULL, fixture, &fix_n);
    memset(caps, 0, sizeof caps);
    caps[0].capsule_json = race_cap;
    caps[0].capsule_len = race_cap_n;
    caps[0].lir_json = race_lir;
    caps[0].lir_len = race_lir_n;
    caps[1].capsule_json = move_cap;
    caps[1].capsule_len = move_cap_n;
    caps[1].lir_json = move_lir;
    caps[1].lir_len = move_lir_n;
    caps[2].capsule_json = query;
    caps[2].capsule_len = query_n;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.capsule_count = 3;
    d.capsules = caps;
    d.fixture_json = fix;
    d.fixture_len = fix_n;
    d.tick_rate_hz = 120;
    d.max_struct_per_tick = 1;
    d.max_events_per_tick = 2u * n + 2u;
    d.event_log_max = 1024; /* a game keeps a bounded log; the snapshot carries the retained events */
    if (su_host_world_create(&d, &w, &err) != SU_OK) {
        fprintf(stderr, "world: %s\n", err.message);
        return 1;
    }
    if (tier == SU_HOST_TIER_NATIVE) {
        /* racer.move compiles; racer.race (emit_event) stays on the default VM tier. */
        if (su_host_system_set_tier(w, 1, SU_HOST_TIER_NATIVE, &err) != SU_OK) {
            fprintf(stderr, "native: %s\n", err.message);
            return 1;
        }
    } else {
        for (i = 0; i < 2u; i++) {
            if (su_host_system_set_tier(w, i, tier, &err) != SU_OK) {
                fprintf(stderr, "tier: %s\n", err.message);
                return 1;
            }
        }
    }
    step_or_die(w, warmup);
    su_host_snapshot_size(w, &snap_len);
    snap = malloc(snap_len);
    scratch = malloc(snap_len);
    t_save = calloc(iters, sizeof *t_save);
    t_restore = calloc(iters, sizeof *t_restore);
    t_resim = calloc(iters, sizeof *t_resim);
    t_digest = calloc(iters, sizeof *t_digest);
    t_total = calloc(iters, sizeof *t_total);
    if (snap == NULL || scratch == NULL || t_save == NULL || t_restore == NULL || t_resim == NULL ||
        t_digest == NULL || t_total == NULL || su_host_snapshot_write(w, snap, snap_len, &got) != SU_OK) {
        fprintf(stderr, "cannot save the anchor\n");
        return 1;
    }
    for (it = 0; it < iters; it++) {
        uint64_t t0 = now_ns();
        su_host_snapshot_write(w, scratch, snap_len, &got);
        t_save[it] = now_ns() - t0;
    }
    printf("{\"workload\":\"6dof_racer_rollback_host_api\",\"n\":%u,\"rows\":%" PRIu64 ",\"tier\":\"%s\","
           "\"warmup_ticks\":%u,\"anchor_tick\":%" PRIu64 ",\"iterations\":%u,\"snapshot_bytes\":%zu,",
           n, su_host_capacity(w), tier_name, warmup, su_host_tick(w), iters, snap_len);
    stats("save_ns", t_save, iters);
    printf("\"k\":[");
    for (k = 0; k < nk; k++) {
        uint32_t kt = ks[k], mismatches = 0;
        uint8_t sha[32];
        char hex[65];
        uint64_t ref_fast = 0;
        if (su_host_snapshot_restore_inplace(w, snap, snap_len, &err) != SU_OK) {
            fprintf(stderr, "restore: %s\n", err.message);
            return 1;
        }
        step_or_die(w, kt);
        su_host_digest(w, sha);
        su_host_hex(sha, hex);
        su_host_digest_fast(w, &ref_fast);
        for (it = 0; it < iters; it++) {
            uint64_t f = 0, t0, t1, t2, t3;
            t0 = now_ns();
            if (su_host_snapshot_restore_inplace(w, snap, snap_len, &err) != SU_OK) {
                fprintf(stderr, "restore: %s\n", err.message);
                return 1;
            }
            t1 = now_ns();
            step_or_die(w, kt);
            t2 = now_ns();
            su_host_digest_fast(w, &f);
            t3 = now_ns();
            t_restore[it] = t1 - t0;
            t_resim[it] = t2 - t1;
            t_digest[it] = t3 - t2;
            t_total[it] = t3 - t0;
            mismatches += f != ref_fast;
        }
        ok &= mismatches == 0u;
        printf("%s{\"k\":%u,\"snapshot_sha256\":\"%s\",\"mismatches\":%u,", k ? "," : "", kt, hex, mismatches);
        stats("restore_ns", t_restore, iters);
        stats("resim_ns", t_resim, iters);
        stats("digest_fast_ns", t_digest, iters);
        stats("total_ns", t_total, iters);
        printf("\"resim_per_tick_p50_ns\":%" PRIu64 ",\"total_p50_fraction_of_budget\":%.4f,"
               "\"total_p99_fraction_of_budget\":%.4f}",
               pct(t_resim, iters, 50) / kt, (double)pct(t_total, iters, 50) / 8333333.0,
               (double)pct(t_total, iters, 99) / 8333333.0);
    }
    printf("],\"budget_ns_120hz\":8333333,\"digests_identical\":%s,", ok ? "true" : "false");
#if defined(__clang__)
    printf("\"compiler\":\"clang %s\",", __clang_version__);
#elif defined(__GNUC__)
    printf("\"compiler\":\"gcc %d.%d.%d\",", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#endif
#ifdef NDEBUG
    printf("\"build\":\"optimized (NDEBUG)\"}\n");
#else
    printf("\"build\":\"debug\"}\n");
#endif
    su_host_world_destroy(w);
    free(snap);
    free(scratch);
    free(t_save);
    free(t_restore);
    free(t_resim);
    free(t_digest);
    free(t_total);
    free(race_cap);
    free(race_lir);
    free(move_cap);
    free(move_lir);
    free(query);
    free(fix);
    return ok ? 0 : 1;
}
