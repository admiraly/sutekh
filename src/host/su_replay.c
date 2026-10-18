/* Command-stream replay (include/sutekh/replay.h): recording through the host API and strict, digest-verified
 * playback with keyframe seeking. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include "sutekh/replay.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/su_host_internal.h"
#include "ir/su_lir.h"
#include "ir/su_sha256.h"
#include "world/su_world.h"

#ifndef SUTEKH_VERSION_STRING
#define SUTEKH_VERSION_STRING "unknown"
#endif

#define REPLAY_MAGIC "SUREPLAY"   /* 8 bytes + 0x00 */
#define REPLAY_MAGIC_BYTES 9u
#define REPLAY_TEXT_MAX 255u
#define INPUT_HEADER_BYTES 24u    /* su_host_input_encode header */
#define TAG_KEYFRAME 'K'
#define TAG_DIGEST 'D'
#define TAG_INPUT 'I'
#define TAG_END 'E'

/* ---------- small helpers ---------- */

static uint32_t set_err(SuHostError *err, uint32_t code, uint32_t index, uint64_t tick, const char *fmt, ...) {
    if (err != NULL) {
        va_list ap;
        err->code = code;
        err->index = index;
        err->tick = tick;
        va_start(ap, fmt);
        (void)vsnprintf(err->message, sizeof err->message, fmt, ap);
        va_end(ap);
    }
    return code;
}

static void clear_err(SuHostError *err) {
    if (err != NULL) {
        memset(err, 0, sizeof *err);
        err->index = UINT32_MAX;
        err->tick = UINT64_MAX;
    }
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_u64(uint8_t *p, uint64_t v) {
    put_u32(p, (uint32_t)v);
    put_u32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64(const uint8_t *p) {
    return (uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32);
}

/* Growable output buffer; failed is sticky. */
typedef struct Out {
    uint8_t *data;
    size_t len;
    size_t cap;
    int failed;
} Out;

static uint8_t *out_reserve(Out *o, size_t n) {
    if (o->failed || n > SIZE_MAX - o->len) {
        o->failed = 1;
        return NULL;
    }
    if (o->len + n > o->cap) {
        size_t cap = o->cap != 0u ? o->cap : 4096u;
        uint8_t *g;
        while (cap < o->len + n) {
            if (cap > SIZE_MAX / 2u) {
                o->failed = 1;
                return NULL;
            }
            cap *= 2u;
        }
        g = realloc(o->data, cap);
        if (g == NULL) {
            o->failed = 1;
            return NULL;
        }
        o->data = g;
        o->cap = cap;
    }
    o->len += n;
    return o->data + o->len - n;
}

static void out_bytes(Out *o, const void *p, size_t n) {
    uint8_t *d = out_reserve(o, n);
    if (d != NULL && n > 0u) {
        memcpy(d, p, n);
    }
}

static void out_u8(Out *o, uint8_t v) {
    out_bytes(o, &v, 1u);
}

static void out_u32(Out *o, uint32_t v) {
    uint8_t b[4];
    put_u32(b, v);
    out_bytes(o, b, 4u);
}

static void out_u64(Out *o, uint64_t v) {
    uint8_t b[8];
    put_u64(b, v);
    out_bytes(o, b, 8u);
}

static void out_text(Out *o, const char *s) {
    size_t n = strlen(s);
    out_u32(o, (uint32_t)n);
    out_bytes(o, s, n);
}

static void sha(const uint8_t *p, size_t n, uint8_t out[32]) {
    SuSha256 c;
    su_sha256_init(&c);
    su_sha256_update(&c, p, n);
    su_sha256_final(&c, out);
}

/* Snapshot bytes of w (malloc'd). */
static uint32_t snapshot_of(const SuHostWorld *w, uint8_t **bytes, size_t *len) {
    size_t size = 0;
    uint32_t st = su_host_snapshot_size(w, &size);
    *bytes = NULL;
    if (st != SU_OK) {
        return st;
    }
    *bytes = malloc(size > 0u ? size : 1u);
    if (*bytes == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    st = su_host_snapshot_write(w, *bytes, size, len);
    if (st != SU_OK) {
        free(*bytes);
        *bytes = NULL;
    }
    return st;
}

/* ---------- recording ---------- */

struct SuReplayWriter {
    const SuHostWorld *world;
    Out out;
    uint64_t start;
    uint64_t next;      /* tick the world must be at for the next step */
    uint64_t kf;
    uint64_t di;
    int finished;
};

/* Writes the keyframe or digest record of the state at tick t (the world's current state). */
static uint32_t record_state(SuReplayWriter *rw, const SuHostWorld *w, uint64_t t) {
    uint64_t off = t - rw->start;
    if (off % rw->kf == 0u) {
        uint8_t *snap = NULL;
        size_t len = 0;
        uint32_t st = snapshot_of(w, &snap, &len);
        if (st != SU_OK) {
            return st;
        }
        out_u8(&rw->out, TAG_KEYFRAME);
        out_u64(&rw->out, t);
        out_u64(&rw->out, (uint64_t)len);
        out_bytes(&rw->out, snap, len);
        free(snap);
    } else if (rw->di != 0u && off % rw->di == 0u) {
        uint8_t d[32];
        uint32_t st = su_host_digest(w, d);
        if (st != SU_OK) {
            return st;
        }
        out_u8(&rw->out, TAG_DIGEST);
        out_u64(&rw->out, t);
        out_bytes(&rw->out, d, 32u);
    }
    return rw->out.failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

uint32_t su_replay_writer_begin(SuHostWorld *w, const SuReplayOptions *opt, SuReplayWriter **out, SuHostError *err) {
    SuReplayWriter *rw;
    uint8_t schema[32];
    uint8_t d[32];
    uint8_t *snap = NULL;
    size_t snap_len = 0;
    uint32_t i;
    uint32_t st;

    clear_err(err);
    if (out == NULL) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX, "out is NULL");
    }
    *out = NULL;
    if (w == NULL || opt == NULL || opt->struct_size != sizeof *opt || opt->reserved0 != 0u ||
        opt->keyframe_interval == 0u) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX,
                       "options: struct_size, reserved0 or keyframe_interval (>= 1) is invalid");
    }
    rw = calloc(1, sizeof *rw);
    if (rw == NULL) {
        return set_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, UINT64_MAX, "allocation failed");
    }
    rw->world = w;
    rw->start = su_world_tick(w->w);
    rw->next = rw->start;
    rw->kf = opt->keyframe_interval;
    rw->di = opt->digest_interval;
    st = snapshot_of(w, &snap, &snap_len);
    if (st != SU_OK) {
        free(rw);
        return set_err(err, st, UINT32_MAX, rw->start, "initial snapshot failed");
    }
    sha(snap, snap_len, d);
    free(snap);
    memset(schema, 0, sizeof schema);
    (void)su_world_schema_digest(w->w, schema);

    out_bytes(&rw->out, REPLAY_MAGIC, REPLAY_MAGIC_BYTES);
    out_u32(&rw->out, SU_REPLAY_FORMAT_VERSION);
    out_u32(&rw->out, SU_HOST_API_VERSION);
    out_text(&rw->out, SUTEKH_VERSION_STRING);
    out_bytes(&rw->out, w->identity, 32u);
    out_bytes(&rw->out, schema, 32u);
    out_u32(&rw->out, w->sched.count);
    for (i = 0; i < w->sched.count; i++) {
        const SuSysEntry *e = &w->sched.sys[i];
        out_text(&rw->out, e->system_id);
        out_bytes(&rw->out, e->mod->semantic_hash, 32u);
        out_text(&rw->out, e->mod->numeric_profile != NULL ? e->mod->numeric_profile : "");
    }
    out_u64(&rw->out, w->seed);
    out_u32(&rw->out, w->hz);
    out_u64(&rw->out, rw->start);
    out_bytes(&rw->out, d, 32u);
    out_u64(&rw->out, rw->kf);
    out_u64(&rw->out, rw->di);
    st = record_state(rw, w, rw->start);
    if (st != SU_OK) {
        su_replay_writer_destroy(rw);
        return set_err(err, st, UINT32_MAX, UINT64_MAX, "recording header failed");
    }
    *out = rw;
    return SU_OK;
}

uint32_t su_replay_writer_step(SuReplayWriter *rw, SuHostWorld *w, uint64_t ticks, uint64_t *done, SuHostError *err) {
    uint64_t n = 0;
    uint32_t st = SU_OK;

    clear_err(err);
    if (done != NULL) {
        *done = 0;
    }
    if (rw == NULL || w == NULL || rw->world != w || rw->finished || su_world_tick(w->w) != rw->next) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX,
                       "writer and world disagree (another world, a finished writer, or a tick changed outside it)");
    }
    while (n < ticks) {
        uint64_t t = rw->next;
        uint8_t *rec = NULL;
        size_t rec_len = 0;
        uint64_t one = 0;

        st = su_host_input_encode(w, t, NULL, 0u, &rec_len);
        if (st == SU_OK && rec_len > INPUT_HEADER_BYTES) {
            rec = malloc(rec_len);
            st = rec == NULL ? SU_E_RESOURCE_LIMIT : su_host_input_encode(w, t, rec, rec_len, &rec_len);
        }
        if (st != SU_OK) {
            free(rec);
            set_err(err, st, UINT32_MAX, t, "input record of tick %llu could not be encoded", (unsigned long long)t);
            break;
        }
        st = su_host_step(w, 1u, &one, err);
        if (st != SU_OK) {
            free(rec);
            break;
        }
        if (rec != NULL) {
            out_u8(&rw->out, TAG_INPUT);
            out_u32(&rw->out, (uint32_t)rec_len);
            out_bytes(&rw->out, rec, rec_len);
            free(rec);
        }
        rw->next = t + 1u;
        st = record_state(rw, w, rw->next);
        if (st != SU_OK) {
            set_err(err, st, UINT32_MAX, rw->next, "keyframe or digest of tick %llu could not be recorded",
                    (unsigned long long)rw->next);
            break;
        }
        n++;
    }
    if (done != NULL) {
        *done = n;
    }
    return st;
}

uint32_t su_replay_writer_finish(SuReplayWriter *rw, const SuHostWorld *w, uint8_t **bytes, size_t *len) {
    uint8_t d[32];
    uint32_t st;
    if (rw == NULL || w == NULL || rw->world != w || rw->finished || bytes == NULL || len == NULL ||
        su_world_tick(w->w) != rw->next) {
        return SU_E_INVALID_ARGUMENT;
    }
    st = su_host_digest(w, d);
    if (st != SU_OK) {
        return st;
    }
    out_u8(&rw->out, TAG_END);
    out_u64(&rw->out, rw->next);
    out_bytes(&rw->out, d, 32u);
    if (rw->out.failed) {
        return SU_E_RESOURCE_LIMIT;
    }
    rw->finished = 1;
    *bytes = rw->out.data;
    *len = rw->out.len;
    memset(&rw->out, 0, sizeof rw->out);
    return SU_OK;
}

void su_replay_writer_destroy(SuReplayWriter *rw) {
    if (rw == NULL) {
        return;
    }
    free(rw->out.data);
    free(rw);
}

void su_replay_free(uint8_t *bytes) {
    free(bytes);
}

/* ---------- parsing ---------- */

typedef struct ReplaySys {
    char *system_id;
    char semantic_hex[65];
    char *numeric_profile;
} ReplaySys;

typedef struct Span {
    uint64_t tick;
    size_t off;
    size_t len;
} Span;

struct SuReplay {
    uint8_t *bytes;
    size_t len;
    SuReplayInfo info;
    char *engine_version;
    ReplaySys *sys;
    Span *keyframes;   /* tick ascending */
    Span *digests;     /* off -> 32-byte digest */
    Span *inputs;      /* tick ascending */
};

typedef struct Rd {
    const uint8_t *p;
    size_t len;
    size_t off;
    int bad;
} Rd;

static const uint8_t *rd_take(Rd *r, size_t n) {
    const uint8_t *p;
    if (r->bad || n > r->len - r->off) {
        r->bad = 1;
        return NULL;
    }
    p = r->p + r->off;
    r->off += n;
    return p;
}

static uint32_t rd32(Rd *r) {
    const uint8_t *p = rd_take(r, 4u);
    return p != NULL ? rd_u32(p) : 0u;
}

static uint64_t rd64(Rd *r) {
    const uint8_t *p = rd_take(r, 8u);
    return p != NULL ? rd_u64(p) : 0u;
}

/* A length-prefixed text of 0..REPLAY_TEXT_MAX bytes without NUL, copied. */
static char *rd_text(Rd *r) {
    uint32_t n = rd32(r);
    const uint8_t *p;
    char *s;
    if (n > REPLAY_TEXT_MAX) {
        r->bad = 1;
        return NULL;
    }
    p = rd_take(r, n);
    if (p == NULL || memchr(p, 0, n) != NULL) {
        r->bad = 1;
        return NULL;
    }
    s = malloc((size_t)n + 1u);
    if (s == NULL) {
        r->bad = 1;
        return NULL;
    }
    memcpy(s, p, n);
    s[n] = '\0';
    return s;
}

void su_replay_close(SuReplay *r) {
    uint32_t i;
    if (r == NULL) {
        return;
    }
    for (i = 0; r->sys != NULL && i < r->info.system_count; i++) {
        free(r->sys[i].system_id);
        free(r->sys[i].numeric_profile);
    }
    free(r->sys);
    free(r->engine_version);
    free(r->keyframes);
    free(r->digests);
    free(r->inputs);
    free(r->bytes);
    free(r);
}

static int is_keyframe_tick(const SuReplayInfo *in, uint64_t t) {
    return (t - in->start_tick) % in->keyframe_interval == 0u;
}

static int is_digest_tick(const SuReplayInfo *in, uint64_t t) {
    return !is_keyframe_tick(in, t) && in->digest_interval != 0u && (t - in->start_tick) % in->digest_interval == 0u;
}

uint32_t su_replay_open(const uint8_t *bytes, size_t len, SuReplay **out, SuHostError *err) {
    SuReplay *r = NULL;
    SuReplayInfo *in;
    Rd rd;
    const uint8_t *p;
    uint64_t t;
    uint32_t i;
    uint32_t nk = 0;
    uint32_t nd = 0;
    uint32_t ni = 0;
    size_t cap_k;
    size_t cap_d;
    size_t cap_i;
    int phase;     /* within the current tick: 0 nothing yet, 1 after K, 2 after D, 3 after I */
    int ended = 0;

    clear_err(err);
    if (out == NULL) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX, "out is NULL");
    }
    *out = NULL;
    if (bytes == NULL || len < REPLAY_MAGIC_BYTES + 8u || memcmp(bytes, REPLAY_MAGIC, REPLAY_MAGIC_BYTES) != 0) {
        return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX, "not a replay (magic)");
    }
    r = calloc(1, sizeof *r);
    if (r == NULL || (r->bytes = malloc(len)) == NULL) {
        free(r);
        return set_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, UINT64_MAX, "allocation failed");
    }
    memcpy(r->bytes, bytes, len);
    r->len = len;
    in = &r->info;
    memset(&rd, 0, sizeof rd);
    rd.p = r->bytes;
    rd.len = len;
    (void)rd_take(&rd, REPLAY_MAGIC_BYTES);
    in->format_version = rd32(&rd);
    in->host_api_version = rd32(&rd);
    if (in->format_version != SU_REPLAY_FORMAT_VERSION) {
        su_replay_close(r);
        return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX, "unsupported replay format version");
    }
    r->engine_version = rd_text(&rd);
    in->engine_version = r->engine_version;
    p = rd_take(&rd, 32u);
    if (p != NULL) {
        memcpy(in->identity, p, 32u);
    }
    p = rd_take(&rd, 32u);
    if (p != NULL) {
        memcpy(in->schema_digest, p, 32u);
    }
    in->system_count = rd32(&rd);
    if (in->system_count == 0u || in->system_count > SU_HOST_MAX_CAPSULES) {
        rd.bad = 1;
    }
    if (!rd.bad) {
        r->sys = calloc(in->system_count, sizeof *r->sys);
        if (r->sys == NULL) {
            rd.bad = 1;
        }
    }
    for (i = 0; !rd.bad && i < in->system_count; i++) {
        r->sys[i].system_id = rd_text(&rd);
        p = rd_take(&rd, 32u);
        if (p != NULL) {
            su_sha256_hex(p, r->sys[i].semantic_hex);
        }
        r->sys[i].numeric_profile = rd_text(&rd);
    }
    in->seed = rd64(&rd);
    in->tick_rate_hz = rd32(&rd);
    in->start_tick = rd64(&rd);
    p = rd_take(&rd, 32u);
    if (p != NULL) {
        memcpy(in->initial_digest, p, 32u);
    }
    in->keyframe_interval = rd64(&rd);
    in->digest_interval = rd64(&rd);
    if (rd.bad || in->keyframe_interval == 0u || in->tick_rate_hz == 0u || in->tick_rate_hz > SU_HOST_TICK_RATE_MAX) {
        su_replay_close(r);
        return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX, "replay header is malformed");
    }

    /* Records. Spans are bounded by the record count, which is bounded by the length. */
    cap_k = cap_d = cap_i = 16u;
    r->keyframes = malloc(cap_k * sizeof *r->keyframes);
    r->digests = malloc(cap_d * sizeof *r->digests);
    r->inputs = malloc(cap_i * sizeof *r->inputs);
    if (r->keyframes == NULL || r->digests == NULL || r->inputs == NULL) {
        su_replay_close(r);
        return set_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, UINT64_MAX, "allocation failed");
    }
    /* Ordering: records are in tick order. A state record (K or D) of tick u needs u > t, except the first
     * record, K of the start tick. An input record of tick u needs u > t, or u == t after that tick's state
     * record. The end record needs end > t after an input (its tick was stepped) and end >= t otherwise.
     * Which ticks carry K or D is fixed by the intervals and checked per record; the counts checked after the
     * loop then prove that none is missing. */
    t = in->start_tick;
    phase = 0;
    while (!rd.bad && !ended) {
        const uint8_t *tagp = rd_take(&rd, 1u);
        uint8_t tag = tagp != NULL ? *tagp : 0u;
        uint64_t rt;
        if (rd.bad) {
            break;
        }
        if (tag == TAG_END) {
            rt = rd64(&rd);
            p = rd_take(&rd, 32u);
            if (p == NULL || rt < t || (phase == 3 && rt == t) || phase == 0) {
                rd.bad = 1;
                break;
            }
            in->end_tick = rt;
            memcpy(in->final_digest, p, 32u);
            ended = 1;
            break;
        }
        if (tag == TAG_KEYFRAME || tag == TAG_DIGEST) {
            int first = phase == 0 && nk == 0u;
            rt = rd64(&rd);
            if (rd.bad || !(rt > t || (first && rt == t)) || (first && tag != TAG_KEYFRAME) ||
                (tag == TAG_KEYFRAME && !is_keyframe_tick(in, rt)) || (tag == TAG_DIGEST && !is_digest_tick(in, rt))) {
                rd.bad = 1;
                break;
            }
            t = rt;
            if (tag == TAG_KEYFRAME) {
                uint64_t klen = rd64(&rd);
                size_t at = rd.off;
                if (rd.bad || klen > (uint64_t)(len - rd.off) || rd_take(&rd, (size_t)klen) == NULL) {
                    rd.bad = 1;
                    break;
                }
                if (nk == cap_k) {
                    Span *g = realloc(r->keyframes, cap_k * 2u * sizeof *g);
                    if (g == NULL) {
                        rd.bad = 1;
                        break;
                    }
                    r->keyframes = g;
                    cap_k *= 2u;
                }
                r->keyframes[nk].tick = t;
                r->keyframes[nk].off = at;
                r->keyframes[nk].len = (size_t)klen;
                nk++;
                phase = 1;
            } else {
                size_t at = rd.off;
                if (rd_take(&rd, 32u) == NULL) {
                    break;
                }
                if (nd == cap_d) {
                    Span *g = realloc(r->digests, cap_d * 2u * sizeof *g);
                    if (g == NULL) {
                        rd.bad = 1;
                        break;
                    }
                    r->digests = g;
                    cap_d *= 2u;
                }
                r->digests[nd].tick = t;
                r->digests[nd].off = at;
                r->digests[nd].len = 32u;
                nd++;
                phase = 2;
            }
            continue;
        }
        if (tag == TAG_INPUT) {
            uint32_t ilen = rd32(&rd);
            size_t at = rd.off;
            const uint8_t *rec;
            uint64_t itick;
            if (rd.bad || phase == 0 || ilen <= INPUT_HEADER_BYTES || (rec = rd_take(&rd, ilen)) == NULL) {
                rd.bad = 1;
                break;
            }
            itick = rd_u64(rec + 12);
            if (!(itick > t || (itick == t && (phase == 1 || phase == 2)))) {
                rd.bad = 1;
                break;
            }
            t = itick;
            if (ni == cap_i) {
                Span *g = realloc(r->inputs, cap_i * 2u * sizeof *g);
                if (g == NULL) {
                    rd.bad = 1;
                    break;
                }
                r->inputs = g;
                cap_i *= 2u;
            }
            r->inputs[ni].tick = t;
            r->inputs[ni].off = at;
            r->inputs[ni].len = ilen;
            ni++;
            phase = 3;
            continue;
        }
        rd.bad = 1;
    }
    if (rd.bad || !ended || rd.off != len || nk == 0u || r->keyframes[0].tick != in->start_tick) {
        su_replay_close(r);
        return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX, "replay records are malformed or out of order");
    }
    /* Every state record the layout calls for between start and end is present. */
    if ((uint64_t)nk != (in->end_tick - in->start_tick) / in->keyframe_interval + 1u) {
        su_replay_close(r);
        return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX, "replay is missing keyframes");
    }
    if (in->digest_interval != 0u) {
        uint64_t want = 0;
        uint64_t u;
        for (u = in->start_tick; u <= in->end_tick; u++) {
            want += is_digest_tick(in, u) ? 1u : 0u;
        }
        if (want != nd) {
            su_replay_close(r);
            return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX, "replay is missing digest records");
        }
    } else if (nd != 0u) {
        su_replay_close(r);
        return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX, "replay has digest records it did not declare");
    }
    {
        uint8_t d[32];
        sha(r->bytes + r->keyframes[0].off, r->keyframes[0].len, d);
        if (memcmp(d, in->initial_digest, 32u) != 0) {
            su_replay_close(r);
            return set_err(err, SU_E_PARSE, UINT32_MAX, in->start_tick,
                           "initial keyframe does not match the header digest");
        }
    }
    in->keyframe_count = nk;
    in->digest_count = nd;
    in->input_count = ni;
    *out = r;
    return SU_OK;
}

uint32_t su_replay_info(const SuReplay *r, SuReplayInfo *out) {
    if (r == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = r->info;
    return SU_OK;
}

uint32_t su_replay_system(const SuReplay *r, uint32_t index, const char **system_id, const char **semantic_hash_hex,
                          const char **numeric_profile) {
    if (r == NULL || index >= r->info.system_count) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (system_id != NULL) {
        *system_id = r->sys[index].system_id;
    }
    if (semantic_hash_hex != NULL) {
        *semantic_hash_hex = r->sys[index].semantic_hex;
    }
    if (numeric_profile != NULL) {
        *numeric_profile = r->sys[index].numeric_profile;
    }
    return SU_OK;
}

/* ---------- playback ---------- */

static const Span *find_span(const Span *s, uint32_t n, uint64_t tick) {
    uint32_t lo = 0;
    uint32_t hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (s[mid].tick == tick) {
            return &s[mid];
        }
        if (s[mid].tick < tick) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

/* Compares w's state at tick t with the replay's record for t, if there is one. */
static uint32_t verify_at(const SuReplay *r, const SuHostWorld *w, uint64_t t, SuHostError *err) {
    const uint8_t *want = NULL;
    uint8_t kd[32];
    uint8_t have[32];
    const Span *s;
    uint32_t st;
    if ((s = find_span(r->keyframes, r->info.keyframe_count, t)) != NULL) {
        sha(r->bytes + s->off, s->len, kd);
        want = kd;
    } else if ((s = find_span(r->digests, r->info.digest_count, t)) != NULL) {
        want = r->bytes + s->off;
    } else if (t == r->info.end_tick) {
        want = r->info.final_digest;
    }
    if (want == NULL) {
        return SU_OK;
    }
    st = su_host_digest(w, have);
    if (st != SU_OK) {
        return set_err(err, st, UINT32_MAX, t, "digest of tick %llu could not be computed", (unsigned long long)t);
    }
    if (memcmp(have, want, 32u) != 0) {
        return set_err(err, SU_E_INTERNAL, SU_REPLAY_DESYNC, t, "desync: state at tick %llu differs from the recording",
                       (unsigned long long)t);
    }
    if (t == r->info.end_tick && memcmp(have, r->info.final_digest, 32u) != 0) {
        return set_err(err, SU_E_INTERNAL, SU_REPLAY_DESYNC, t, "desync: end state differs from the recording");
    }
    return SU_OK;
}

static uint32_t check_world(const SuReplay *r, const SuHostWorld *w, SuHostError *err) {
    if (memcmp(w->identity, r->info.identity, 32u) != 0) {
        return set_err(err, SU_E_SCHEMA_MISMATCH, UINT32_MAX, UINT64_MAX,
                       "world identity differs from the replay's (schema, systems, capacity or caps)");
    }
    return SU_OK;
}

uint32_t su_replay_play(const SuReplay *r, SuHostWorld *w, uint64_t ticks, uint64_t *done, SuHostError *err) {
    uint64_t n = 0;
    uint64_t t;
    uint32_t st;

    clear_err(err);
    if (done != NULL) {
        *done = 0;
    }
    if (r == NULL || w == NULL) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX, "invalid arguments");
    }
    st = check_world(r, w, err);
    if (st != SU_OK) {
        return st;
    }
    t = su_world_tick(w->w);
    if (t < r->info.start_tick || t > r->info.end_tick) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, t, "world tick %llu is outside the replay [%llu, %llu]",
                       (unsigned long long)t, (unsigned long long)r->info.start_tick,
                       (unsigned long long)r->info.end_tick);
    }
    su_host_input_clear(w, t);
    st = verify_at(r, w, t, err);
    while (st == SU_OK && n < ticks && t < r->info.end_tick) {
        const Span *in = find_span(r->inputs, r->info.input_count, t);
        uint64_t one = 0;
        if (in != NULL) {
            st = su_host_input_push_encoded(w, r->bytes + in->off, in->len, err);
            if (st != SU_OK) {
                break;
            }
        }
        st = su_host_step(w, 1u, &one, err);
        if (st != SU_OK) {
            break;
        }
        t++;
        n++;
        st = verify_at(r, w, t, err);
    }
    if (done != NULL) {
        *done = n;
    }
    return st;
}

uint32_t su_replay_seek(const SuReplay *r, SuHostWorld *w, uint64_t tick, SuHostError *err) {
    const Span *k;
    uint64_t done = 0;
    uint32_t idx;
    uint32_t st;

    clear_err(err);
    if (r == NULL || w == NULL) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX, "invalid arguments");
    }
    if (tick < r->info.start_tick || tick > r->info.end_tick) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, tick, "tick %llu is outside the replay [%llu, %llu]",
                       (unsigned long long)tick, (unsigned long long)r->info.start_tick,
                       (unsigned long long)r->info.end_tick);
    }
    st = check_world(r, w, err);
    if (st != SU_OK) {
        return st;
    }
    idx = (uint32_t)((tick - r->info.start_tick) / r->info.keyframe_interval);
    k = &r->keyframes[idx];
    st = su_host_snapshot_restore(w, r->bytes + k->off, k->len, err);
    if (st != SU_OK) {
        return st;
    }
    st = su_replay_play(r, w, tick - k->tick, &done, err);
    if (st == SU_OK && su_world_tick(w->w) != tick) {
        st = set_err(err, SU_E_INTERNAL, UINT32_MAX, tick, "seek stopped before tick %llu", (unsigned long long)tick);
    }
    return st;
}
