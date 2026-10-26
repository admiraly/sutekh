/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * Worlds without a declared schema are unchanged by issue 144 (64-system schedules, declared schemas). This file
 * uses only the first-release host API (SuHostWorldDesc through seed, no schema), so the same source builds against
 * a library from before the change.
 *
 * The chain worlds of sched_fixtures.h with 1, 3 and 8 systems (8 was the old maximum), 6 rows, inputs at ticks 2
 * and 5, 12 ticks recorded with keyframes every 4 ticks and digests every tick:
 *   (no arguments)  checks every column against the reference model after every tick, and the world identity and
 *                   final digest against values pinned from the library before the change (origin/main 31192d9);
 *   dump <dir>      writes, per world, the replay bytes, the final snapshot bytes and a text file with the identity
 *                   and every tick's SHA-256 and fast digest, for a byte comparison between two builds. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/host.h"
#include "sutekh/replay.h"
#include "sched_fixtures.h"

static unsigned g_checks, g_failures;

static void check(int cond, const char *what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

#define ROWS 6u
#define TICKS 12u

/* Pinned on origin/main 31192d9 (before issue 144): world identity and SHA-256 digest at tick 12. */
typedef struct Pin {
    uint32_t n;
    const char *identity;
    const char *digest;
} Pin;

static const Pin k_pins[] = {
    {1u,
     "c6e97bf4ebf7ca50347fc0601ccf83ab1c2896e29b974db40cc635bf66759a58",
     "69ffbf1bb372aa74d972c2e2308fcc8ded01ba38dd79c1a6239d984430f8c49b"},
    {3u,
     "a05f51e91e998d57bd293b561514165624e877e3906f023c5c292fc49ab0fd7a",
     "51edd268d0f62b34ef8b307167ab13949622c68a7cec4a7c81339511e8926448"},
    {8u,
     "29ced9f5de8597f4ae20c6373e3a14008f53390b1b9e6da71f7c7fc206c22c0e",
     "c1ac922453baab6300577f6fa0e3d48ce35d1efbb8f6675962dd665a94f11ecb"},
};

static int write_file(const char *dir, const char *name, const void *p, size_t n) {
    char path[1024];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (f == NULL) {
        return 0;
    }
    if (n > 0u && fwrite(p, 1, n, f) != n) {
        fclose(f);
        return 0;
    }
    return fclose(f) == 0;
}

static int run_chain(uint32_t n, const char *dump_dir) {
    Chain c;
    SuHostWorldDesc d;
    SuHostWorld *w = NULL;
    SuHostError err;
    SuReplayWriter *rw = NULL;
    SuReplayOptions ro;
    uint32_t model[ROWS][CHAIN_MAX + 1u];
    uint8_t id[32], dg[32];
    char hex[65], name[64];
    Buf text = {NULL, 0, 0};
    uint8_t *rep = NULL, *snap = NULL;
    size_t rep_len = 0, snap_len = 0, got = 0;
    uint32_t r, f, t;
    uint64_t done = 0;
    int ok = 1;

    chain_build(&c, n, ROWS, NULL);
    memset(&d, 0, sizeof d);
    d.struct_size = (uint32_t)sizeof d;
    d.capsule_count = n;
    d.capsules = c.caps;
    d.fixture_json = (const uint8_t *)c.fixture.p;
    d.fixture_len = c.fixture.n;
    d.tick_rate_hz = 60u;
    d.event_log_max = 64u;
    d.seed = 144u;
    if (su_host_world_create(&d, &w, &err) != SU_OK) {
        fprintf(stderr, "chain %u: create failed: %s\n", (unsigned)n, err.message);
        chain_free(&c);
        return 0;
    }
    for (r = 0; r < ROWS; r++) {
        for (f = 0; f <= n; f++) {
            model[r][f] = chain_init(r, f);
        }
    }
    su_host_world_identity(w, id);
    su_host_hex(id, hex);
    put(&text, "identity %s\n", hex);
    memset(&ro, 0, sizeof ro);
    ro.struct_size = (uint32_t)sizeof ro;
    ro.keyframe_interval = 4u;
    ro.digest_interval = 1u;
    ok &= su_replay_writer_begin(w, &ro, &rw, &err) == SU_OK;
    for (t = 0; ok && t < TICKS; t++) {
        uint64_t fast = 0;
        if (t == 2u || t == 5u) {
            SuHostInput in[ROWS];
            for (r = 0; r < ROWS; r++) {
                uint64_t ent = 0;
                su_host_entity_at(w, r, &ent);
                in[r].entity = ent;
                in[r].field_id = 1u; /* v00 */
                in[r].value = t * 100u + r;
                model[r][0] = in[r].value;
            }
            ok &= su_host_input_push(w, t, in, ROWS, &err) == SU_OK;
        }
        ok &= su_replay_writer_step(rw, w, 1u, &done, &err) == SU_OK && done == 1u;
        for (r = 0; r < ROWS; r++) {
            chain_model_tick(model[r], n);
        }
        for (f = 0; f <= n; f++) {
            uint32_t col[ROWS];
            ok &= su_host_read(w, f + 1u, 0, ROWS, col) == SU_OK;
            for (r = 0; r < ROWS; r++) {
                ok &= col[r] == model[r][f];
            }
        }
        su_host_digest(w, dg);
        su_host_digest_fast(w, &fast);
        su_host_hex(dg, hex);
        put(&text, "tick %u %s %016llx\n", (unsigned)(t + 1u), hex, (unsigned long long)fast);
    }
    check(ok, "chain world matches the reference model every tick");
    ok &= su_replay_writer_finish(rw, w, &rep, &rep_len) == SU_OK;
    su_replay_writer_destroy(rw);
    ok &= su_host_snapshot_size(w, &snap_len) == SU_OK;
    snap = malloc(snap_len);
    ok &= snap != NULL && su_host_snapshot_write(w, snap, snap_len, &got) == SU_OK && got == snap_len;
    if (dump_dir != NULL) {
        snprintf(name, sizeof name, "chain%u.replay", (unsigned)n);
        ok &= write_file(dump_dir, name, rep, rep_len);
        snprintf(name, sizeof name, "chain%u.snapshot", (unsigned)n);
        ok &= write_file(dump_dir, name, snap, snap_len);
        snprintf(name, sizeof name, "chain%u.digests", (unsigned)n);
        ok &= write_file(dump_dir, name, text.p, text.n);
    } else {
        size_t i;
        for (i = 0; i < sizeof k_pins / sizeof k_pins[0]; i++) {
            char msg[256];
            if (k_pins[i].n != n) {
                continue;
            }
            su_host_hex(id, hex);
            snprintf(msg, sizeof msg, "chain %u: identity %s equals the pinned %s", (unsigned)n, hex,
                     k_pins[i].identity);
            check(strcmp(hex, k_pins[i].identity) == 0, msg);
            su_host_digest(w, dg);
            su_host_hex(dg, hex);
            snprintf(msg, sizeof msg, "chain %u: digest %s equals the pinned %s", (unsigned)n, hex, k_pins[i].digest);
            check(strcmp(hex, k_pins[i].digest) == 0, msg);
        }
    }
    check(ok, "chain world: replay, snapshot and outputs");
    su_replay_free(rep);
    free(snap);
    buf_free(&text);
    su_host_world_destroy(w);
    chain_free(&c);
    return ok;
}

int main(int argc, char **argv) {
    const char *dir = NULL;
    if (argc == 3 && strcmp(argv[1], "dump") == 0) {
        dir = argv[2];
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [dump <dir>]\n", argv[0]);
        return 2;
    }
    run_chain(1u, dir);
    run_chain(3u, dir);
    run_chain(8u, dir);
    printf("test_host_compat: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
