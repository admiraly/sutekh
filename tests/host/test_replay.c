/* Command-stream replay (include/sutekh/replay.h): recording, strict parsing, digest-verified playback,
 * keyframe seeking, desync and tamper detection.
 *
 * Usage (from the repository root):
 *   test_replay                 in-process checks
 *   test_replay record <file>   records the reference scenario to <file>
 *   test_replay verify <file>   plays <file> back in this process and checks every recorded digest; with the
 *                               record mode in another process (and another build of the library) this checks
 *                               that the same runtime reproduces the run across processes. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/host.h"
#include "sutekh/replay.h"

static long g_checks = 0;
static long g_fail = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        g_checks++;                                                                  \
        if (!(cond)) {                                                               \
            g_fail++;                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

#define TICKS 300u
#define KEYFRAME_EVERY 64u

typedef struct Bytes {
    uint8_t *data;
    size_t len;
} Bytes;

static Bytes slurp(const char *path) {
    Bytes b = {NULL, 0};
    FILE *f = fopen(path, "rb");
    long n;
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    b.data = malloc((size_t)n + 1u);
    if (b.data == NULL || fread(b.data, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    fclose(f);
    b.len = (size_t)n;
    return b;
}

/* Two systems over the entity fixture: despawn_move (entity ops, events) in the reference scenario. */
static Bytes g_cap;
static Bytes g_lir;
static Bytes g_fx;
static SuHostCapsule g_capsule;

static SuHostWorld *make_world(uint64_t capacity) {
    SuHostWorldDesc d;
    SuHostWorld *w = NULL;
    SuHostError e;
    uint32_t st;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.capsule_count = 1;
    d.capsules = &g_capsule;
    d.fixture_json = g_fx.data;
    d.fixture_len = g_fx.len;
    d.max_entities = capacity;
    d.tick_rate_hz = 120;
    d.seed = 20261018;
    d.event_log_max = 64;
    st = su_host_world_create(&d, &w, &e);
    if (st != SU_OK) {
        fprintf(stderr, "create: %s %s\n", su_host_status_name(st), e.message);
        exit(1);
    }
    return w;
}

/* Scenario inputs: every 7th tick rewrites a cell of each live entity from a fixed sequence. */
static void push_inputs(SuHostWorld *w, uint64_t tick) {
    SuHostInput in[8];
    uint32_t n = 0;
    uint32_t slot;
    uint32_t nf = su_host_field_count(w);
    if (tick % 7u != 0u || nf == 0u) {
        return;
    }
    for (slot = 0; slot < su_host_capacity(w) && n < 8u; slot++) {
        uint64_t ent = 0;
        SuHostFieldInfo fi;
        if (su_host_entity_at(w, slot, &ent) != SU_OK) {
            continue;
        }
        (void)su_host_field_info(w, (uint32_t)((tick / 7u + slot) % nf), &fi);
        in[n].entity = ent;
        in[n].field_id = fi.id;
        in[n].value = (uint32_t)(tick * 2654435761u + slot);
        n++;
    }
    if (n > 0u) {
        SuHostError e;
        CHECK(su_host_input_push(w, tick, in, n, &e) == SU_OK);
    }
}

/* Records the scenario; live[t] receives the digest of the state at tick t. */
static void record(uint8_t **bytes, size_t *len, uint8_t (*live)[32]) {
    SuHostWorld *w = make_world(0);
    SuReplayWriter *rw = NULL;
    SuReplayOptions o;
    SuHostError e;
    uint64_t t;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.keyframe_interval = KEYFRAME_EVERY;
    o.digest_interval = 1;
    CHECK(su_replay_writer_begin(w, &o, &rw, &e) == SU_OK);
    if (live != NULL) {
        CHECK(su_host_digest(w, live[0]) == SU_OK);
    }
    for (t = 0; t < TICKS; t++) {
        uint64_t done = 0;
        push_inputs(w, t);
        CHECK(su_replay_writer_step(rw, w, 1, &done, &e) == SU_OK && done == 1u);
        if (live != NULL) {
            CHECK(su_host_digest(w, live[t + 1u]) == SU_OK);
        }
    }
    CHECK(su_replay_writer_finish(rw, w, bytes, len) == SU_OK);
    CHECK(su_replay_writer_step(rw, w, 1, NULL, &e) == SU_E_INVALID_ARGUMENT); /* finished */
    su_replay_writer_destroy(rw);
    su_host_world_destroy(w);
}

static void test_record_and_play(void) {
    static uint8_t live[TICKS + 1u][32];
    uint8_t *bytes = NULL;
    uint8_t *again = NULL;
    size_t len = 0;
    size_t len2 = 0;
    SuReplay *r = NULL;
    SuReplayInfo info;
    SuHostWorld *w;
    SuHostError e;
    uint64_t done = 0;
    uint8_t d[32];
    const char *sid = NULL;
    const char *sem = NULL;
    const char *prof = NULL;
    uint64_t seeks[] = {0u, 1u, 63u, 64u, 65u, 128u, 199u, 256u, 299u, 300u};
    uint32_t i;

    record(&bytes, &len, live);
    record(&again, &len2, NULL);
    CHECK(len == len2 && memcmp(bytes, again, len) == 0); /* the recording is canonical and repeatable */
    su_replay_free(again);

    CHECK(su_replay_open(bytes, len, &r, &e) == SU_OK);
    CHECK(su_replay_info(r, &info) == SU_OK);
    CHECK(info.format_version == SU_REPLAY_FORMAT_VERSION && info.host_api_version == SU_HOST_API_VERSION);
    CHECK(info.start_tick == 0u && info.end_tick == TICKS);
    CHECK(info.keyframe_count == TICKS / KEYFRAME_EVERY + 1u);
    CHECK(info.digest_count == TICKS - TICKS / KEYFRAME_EVERY);
    CHECK(info.input_count > 0u && info.input_count <= (TICKS + 6u) / 7u);
    CHECK(info.tick_rate_hz == 120u && info.seed == 20261018u && info.system_count == 1u);
    CHECK(memcmp(info.initial_digest, live[0], 32) == 0);
    CHECK(memcmp(info.final_digest, live[TICKS], 32) == 0);
    CHECK(su_replay_system(r, 0, &sid, &sem, &prof) == SU_OK && strlen(sem) == 64u && strcmp(prof, "u32_mod_v1") == 0);
    CHECK(su_replay_system(r, 1, &sid, &sem, &prof) == SU_E_INVALID_ARGUMENT);

    /* Full playback on a fresh world, from the start keyframe. */
    w = make_world(0);
    CHECK(su_replay_seek(r, w, 0, &e) == SU_OK);
    CHECK(su_replay_play(r, w, UINT64_MAX, &done, &e) == SU_OK && done == TICKS);
    CHECK(su_host_digest(w, d) == SU_OK && memcmp(d, live[TICKS], 32) == 0);
    CHECK(su_replay_play(r, w, 5, &done, &e) == SU_OK && done == 0u); /* at the end */

    /* Seeking lands on the recorded state, from either direction. */
    for (i = 0; i < sizeof seeks / sizeof seeks[0]; i++) {
        CHECK(su_replay_seek(r, w, seeks[i], &e) == SU_OK);
        CHECK(su_host_tick(w) == seeks[i]);
        CHECK(su_host_digest(w, d) == SU_OK && memcmp(d, live[seeks[i]], 32) == 0);
    }
    CHECK(su_replay_seek(r, w, TICKS + 1u, &e) == SU_E_INVALID_ARGUMENT);

    /* Playback from a mid-run seek reaches the same end. */
    CHECK(su_replay_seek(r, w, 100, &e) == SU_OK);
    CHECK(su_replay_play(r, w, UINT64_MAX, &done, &e) == SU_OK && done == TICKS - 100u);

    /* A host mutation outside the input stream is caught as a desync at the next recorded digest. */
    CHECK(su_replay_seek(r, w, 10, &e) == SU_OK);
    {
        uint64_t ent = 0;
        uint64_t n;
        uint32_t slot = 0;
        SuHostFieldInfo fi;
        SuHostInput in;
        (void)su_host_field_info(w, 0, &fi);
        while (slot < su_host_capacity(w) && su_host_entity_at(w, slot, &ent) != SU_OK) {
            slot++;
        }
        CHECK(slot < su_host_capacity(w));
        in.entity = ent;
        in.field_id = fi.id;
        in.value = 0xABCDEFu;
        CHECK(su_host_input_push(w, 10, &in, 1, &e) == SU_OK);
        CHECK(su_host_step(w, 1, &n, &e) == SU_OK);
        CHECK(su_replay_play(r, w, 1, &n, &e) == SU_E_INTERNAL);
        CHECK(e.index == SU_REPLAY_DESYNC && e.tick == 11u);
    }
    su_host_world_destroy(w);

    /* A world with another identity is refused. */
    w = make_world(32);
    CHECK(su_replay_seek(r, w, 0, &e) == SU_E_SCHEMA_MISMATCH);
    su_host_world_destroy(w);
    su_replay_close(r);
    su_replay_free(bytes);
}

/* Tampering: every changed byte of a recording is refused at open or caught in playback. */
static void test_tamper(void) {
    uint8_t *bytes = NULL;
    uint8_t *bad;
    size_t len = 0;
    SuReplay *r = NULL;
    SuHostError e;
    size_t at;
    uint32_t caught = 0;
    uint32_t tried = 0;

    record(&bytes, &len, NULL);
    bad = malloc(len);
    CHECK(su_replay_open(bytes, len - 1u, &r, &e) == SU_E_PARSE && r == NULL);
    memcpy(bad, bytes, len);
    bad[len - 1u] ^= 1u; /* final digest */
    CHECK(su_replay_open(bad, len, &r, &e) == SU_OK);
    {
        SuHostWorld *w = make_world(0);
        uint64_t done = 0;
        CHECK(su_replay_seek(r, w, 0, &e) == SU_OK);
        CHECK(su_replay_play(r, w, UINT64_MAX, &done, &e) == SU_E_INTERNAL && e.index == SU_REPLAY_DESYNC &&
              e.tick == TICKS);
        su_host_world_destroy(w);
    }
    su_replay_close(r);
    r = NULL;
    /* Flip one bit at a spread of offsets past the header: open refuses it, or full playback reports an error. */
    for (at = 200u; at < len; at += len / 97u + 1u) {
        SuHostWorld *w;
        uint64_t done = 0;
        uint32_t st;
        memcpy(bad, bytes, len);
        bad[at] ^= 0x10u;
        tried++;
        st = su_replay_open(bad, len, &r, &e);
        if (st != SU_OK) {
            caught++;
            continue;
        }
        w = make_world(0);
        st = su_replay_seek(r, w, 0, &e);
        if (st == SU_OK) {
            st = su_replay_play(r, w, UINT64_MAX, &done, &e);
        }
        if (st != SU_OK) {
            caught++;
        } else {
            fprintf(stderr, "tamper at offset %zu was not detected\n", at);
        }
        su_host_world_destroy(w);
        su_replay_close(r);
        r = NULL;
    }
    CHECK(tried > 50u);
    CHECK(caught == tried);
    free(bad);
    su_replay_free(bytes);
}

/* Little-endian reads of the recording layout (src/host/su_replay.c), for locating records in the bytes. */
static uint64_t le(const uint8_t *p, unsigned n) {
    uint64_t v = 0;
    while (n-- > 0u) {
        v = (v << 8) | p[n];
    }
    return v;
}

/* Advances *at past a length-prefixed text; 0 when it runs past len. */
static int skip_text(const uint8_t *b, size_t len, size_t *at) {
    uint64_t n;
    if (*at + 4u > len) {
        return 0;
    }
    n = le(b + *at, 4u);
    if (n > len - *at - 4u) {
        return 0;
    }
    *at += 4u + (size_t)n;
    return 1;
}

/* Offset of the initial keyframe's snapshot bytes: the header (magic, versions, engine text, identity, schema
 * digest, systems, seed, rate, start tick, initial digest, intervals), then K, tick and length. 0 if it does not
 * walk. */
static size_t initial_keyframe_body(const uint8_t *b, size_t len, size_t *body_len) {
    size_t at = 9u + 4u + 4u;
    uint32_t sys;
    uint32_t i;
    if (!skip_text(b, len, &at) || at + 64u + 4u > len) {
        return 0;
    }
    at += 64u;
    sys = (uint32_t)le(b + at, 4u);
    at += 4u;
    for (i = 0; i < sys; i++) {
        if (!skip_text(b, len, &at) || at + 32u > len) {
            return 0;
        }
        at += 32u;
        if (!skip_text(b, len, &at)) {
            return 0;
        }
    }
    at += 8u + 4u + 8u + 32u + 8u + 8u;
    if (at + 17u > len || b[at] != (uint8_t)'K') {
        return 0;
    }
    *body_len = (size_t)le(b + at + 9u, 8u);
    at += 17u;
    return *body_len <= len - at ? at : 0u;
}

/* Regression (issue 148): a recording whose initial keyframe does not match the header digest is refused with
 * the start tick in the error. The refusal used to read the start tick through the replay it had just freed
 * (heap-use-after-free, reported by AddressSanitizer). */
static void test_initial_keyframe_mismatch(void) {
    uint8_t *bytes = NULL;
    uint8_t *bad;
    size_t len = 0;
    size_t body_len = 0;
    size_t body;
    SuReplay *r = NULL;
    SuHostError e;

    record(&bytes, &len, NULL);
    body = initial_keyframe_body(bytes, len, &body_len);
    CHECK(body != 0u && body_len > 0u);
    bad = malloc(len);
    CHECK(bad != NULL);
    if (body != 0u && body_len > 0u && bad != NULL) {
        memcpy(bad, bytes, len);
        bad[body + body_len - 1u] ^= 0x01u; /* last snapshot byte: framing intact, digest differs */
        CHECK(su_replay_open(bad, len, &r, &e) == SU_E_PARSE && r == NULL);
        CHECK(e.code == SU_E_PARSE && e.tick == 0u && e.index == UINT32_MAX);
        CHECK(strstr(e.message, "initial keyframe does not match") != NULL);
        /* The untouched recording still opens. */
        CHECK(su_replay_open(bytes, len, &r, &e) == SU_OK && r != NULL);
        su_replay_close(r);
    }
    free(bad);
    su_replay_free(bytes);
}

static void put_le(uint8_t *p, uint64_t v, unsigned n) {
    unsigned i;
    for (i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

/* Regression: a header whose keyframe interval spans nearly 2^64 ticks. The recording keeps the real header and
 * initial keyframe and sets keyframe_interval = UINT64_MAX and digest_interval = 1, then a second keyframe and the
 * end record at tick UINT64_MAX: two keyframes, as that layout calls for, and no digest record. Open must refuse
 * it for the missing digests; it used to count the digest ticks one by one over the whole span and never return. */
static void test_huge_span(void) {
    uint8_t *bytes = NULL;
    uint8_t *bad;
    size_t len = 0;
    size_t body_len = 0;
    size_t body;
    size_t k_at;
    size_t k_len;
    size_t n;
    SuReplay *r = NULL;
    SuHostError e;

    record(&bytes, &len, NULL);
    body = initial_keyframe_body(bytes, len, &body_len);
    CHECK(body != 0u);
    if (body == 0u) {
        su_replay_free(bytes);
        return;
    }
    k_at = body - 17u;     /* K, tick, length */
    k_len = 17u + body_len; /* the whole initial keyframe record */
    bad = malloc(k_at + 2u * k_len + 41u);
    CHECK(bad != NULL);
    if (bad != NULL) {
        memcpy(bad, bytes, k_at + k_len);           /* header and initial keyframe */
        put_le(bad + k_at - 16u, UINT64_MAX, 8u);   /* keyframe_interval */
        put_le(bad + k_at - 8u, 1u, 8u);            /* digest_interval */
        n = k_at + k_len;
        memcpy(bad + n, bytes + k_at, k_len);       /* a second keyframe ... */
        put_le(bad + n + 1u, UINT64_MAX, 8u);       /* ... at tick UINT64_MAX */
        n += k_len;
        bad[n] = (uint8_t)'E';
        put_le(bad + n + 1u, UINT64_MAX, 8u);       /* end tick */
        memset(bad + n + 9u, 0, 32u);               /* final digest (not reached) */
        n += 41u;
        CHECK(su_replay_open(bad, n, &r, &e) == SU_E_PARSE && r == NULL);
        CHECK(strstr(e.message, "missing digest records") != NULL);
    }
    free(bad);
    su_replay_free(bytes);
}

static void test_writer_errors(void) {
    SuHostWorld *w = make_world(0);
    SuHostWorld *other = make_world(0);
    SuReplayWriter *rw = NULL;
    SuReplayOptions o;
    SuHostError e;
    uint8_t *bytes = NULL;
    size_t len = 0;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    CHECK(su_replay_writer_begin(w, &o, &rw, &e) == SU_E_INVALID_ARGUMENT && rw == NULL); /* keyframe 0 */
    o.keyframe_interval = 10;
    CHECK(su_replay_writer_begin(w, &o, &rw, &e) == SU_OK);
    CHECK(su_replay_writer_step(rw, other, 1, NULL, &e) == SU_E_INVALID_ARGUMENT);
    CHECK(su_host_step(w, 1, NULL, &e) == SU_OK); /* stepped outside the writer */
    CHECK(su_replay_writer_step(rw, w, 1, NULL, &e) == SU_E_INVALID_ARGUMENT);
    CHECK(su_replay_writer_finish(rw, w, &bytes, &len) == SU_E_INVALID_ARGUMENT);
    su_replay_writer_destroy(rw);
    /* A recording with no ticks is valid: start keyframe and end record. */
    CHECK(su_replay_writer_begin(w, &o, &rw, &e) == SU_OK);
    CHECK(su_replay_writer_finish(rw, w, &bytes, &len) == SU_OK);
    {
        SuReplay *r = NULL;
        SuReplayInfo info;
        CHECK(su_replay_open(bytes, len, &r, &e) == SU_OK);
        CHECK(su_replay_info(r, &info) == SU_OK && info.start_tick == 1u && info.end_tick == 1u &&
              info.keyframe_count == 1u && info.digest_count == 0u);
        su_replay_close(r);
    }
    su_replay_free(bytes);
    su_replay_writer_destroy(rw);
    su_host_world_destroy(w);
    su_host_world_destroy(other);
}

static int write_file(const char *path, const uint8_t *b, size_t n) {
    FILE *f = fopen(path, "wb");
    int ok = f != NULL && fwrite(b, 1, n, f) == n;
    if (f != NULL && fclose(f) != 0) {
        ok = 0;
    }
    return ok;
}

int main(int argc, char **argv) {
    g_cap = slurp("tests/fixtures/entities/despawn_move.capsule.json");
    g_lir = slurp("tests/fixtures/entities/despawn_move.lir.json");
    g_fx = slurp("tests/fixtures/entities/actors_3.json");
    g_capsule.capsule_json = g_cap.data;
    g_capsule.capsule_len = g_cap.len;
    g_capsule.lir_json = g_lir.data;
    g_capsule.lir_len = g_lir.len;

    if (argc == 3 && strcmp(argv[1], "record") == 0) {
        uint8_t *bytes = NULL;
        size_t len = 0;
        record(&bytes, &len, NULL);
        CHECK(write_file(argv[2], bytes, len));
        su_replay_free(bytes);
    } else if (argc == 3 && strcmp(argv[1], "verify") == 0) {
        Bytes b = slurp(argv[2]);
        SuReplay *r = NULL;
        SuHostWorld *w = make_world(0);
        SuHostError e;
        uint64_t done = 0;
        CHECK(su_replay_open(b.data, b.len, &r, &e) == SU_OK);
        CHECK(su_replay_seek(r, w, 0, &e) == SU_OK);
        CHECK(su_replay_play(r, w, UINT64_MAX, &done, &e) == SU_OK && done == TICKS);
        if (g_fail != 0) {
            fprintf(stderr, "verify: %s (tick %llu)\n", e.message, (unsigned long long)e.tick);
        }
        su_replay_close(r);
        su_host_world_destroy(w);
        free(b.data);
    } else if (argc == 1) {
        test_record_and_play();
        test_initial_keyframe_mismatch();
        test_huge_span();
        test_tamper();
        test_writer_errors();
    } else {
        fprintf(stderr, "usage: %s [record <file> | verify <file>]\n", argv[0]);
        return 2;
    }
    free(g_cap.data);
    free(g_lir.data);
    free(g_fx.data);
    printf("test_replay: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
