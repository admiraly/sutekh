#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sutekh/audio_offline.h"
#include "ir/su_sha256.h"

#define TP_PHASES 4u
#define TP_TAPS 12u
#define CHECKPOINT_MAGIC 0x43415553u /* "SUAC" little-endian */

typedef struct Biquad {
    double b0, b1, b2, a1, a2;
} Biquad;

typedef struct Meter {
    Biquad pre;
    Biquad rlb;
    double state[2][4];
    double fir[TP_PHASES][TP_TAPS];
    float history[2][TP_TAPS];
    uint32_t history_pos;
    uint32_t sub_len;
    uint32_t sub_pos;
    double sub_energy;
    double recent[4];
    uint64_t sub_count;
    double *blocks;
    size_t block_count;
    size_t block_capacity;
    double sample_peak;
    double true_peak;
    uint32_t failed;
} Meter;

struct SuAudioOffline {
    SuAudioOfflineManifest manifest;
    SuAudioScene *scene;
    SuAudioEvents *events;
    SuAudioTraceCommand *trace;
    SuAudioInstance *handles;
    SuAudioEmitter *emitters;
    SuAudioAssetRef *refs;
    uint32_t trace_count;
    uint32_t trace_next;
    uint32_t pinned;
    float cache[SU_AUDIO_EVENT_QUANTUM * 2u];
    uint32_t cached;
    uint32_t consumed;
    uint64_t position;
    SuSha256 pcm;
    uint8_t source[32];
    uint8_t graph[32];
    Meter meter;
    uint32_t peak_instances;
    uint32_t peak_voices;
    uint32_t applied;
    uint32_t failed;
    uint32_t replayed;
    double render_seconds;
    SuAudioOfflineObserver observer;
    void *observer_user;
};

static double now_seconds(void) {
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) != TIME_UTC) {
        return 0.0;
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ---- little-endian serialization ---- */

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get64(const uint8_t *p) {
    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

static uint32_t float_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static void hash32(SuSha256 *ctx, uint32_t v) {
    uint8_t b[4];
    put32(b, v);
    su_sha256_update(ctx, b, sizeof b);
}

static void hash64(SuSha256 *ctx, uint64_t v) {
    uint8_t b[8];
    put64(b, v);
    su_sha256_update(ctx, b, sizeof b);
}

static void hashf(SuSha256 *ctx, float v) {
    hash32(ctx, float_bits(v));
}

static void hash_pos(SuSha256 *ctx, const SuWorldPos *pos) {
    hash64(ctx, (uint64_t)pos->x);
    hash64(ctx, (uint64_t)pos->y);
    hash64(ctx, (uint64_t)pos->z);
}

static void hash_vec(SuSha256 *ctx, const SuVec3f *v) {
    hashf(ctx, v->x);
    hashf(ctx, v->y);
    hashf(ctx, v->z);
}

static uint64_t fnv64(const uint8_t *data, size_t len, uint64_t h) {
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= data[i];
        h *= UINT64_C(0x100000001b3);
    }
    return h;
}

/* ---- BS.1770-4 K-weighting, gating blocks and 4x true peak ---- */

static void meter_init(Meter *m, uint32_t rate) {
    const double pi = 3.14159265358979323846;
    double k = tan(pi * 1681.974450955533 / (double)rate);
    double q = 0.7071752369554196;
    double vh = pow(10.0, 3.999843853973347 / 20.0);
    double vb = pow(vh, 0.4996667741545416);
    double a0 = 1.0 + k / q + k * k;
    uint32_t phase;
    uint32_t tap;
    memset(m, 0, sizeof *m);
    m->pre.b0 = (vh + vb * k / q + k * k) / a0;
    m->pre.b1 = 2.0 * (k * k - vh) / a0;
    m->pre.b2 = (vh - vb * k / q + k * k) / a0;
    m->pre.a1 = 2.0 * (k * k - 1.0) / a0;
    m->pre.a2 = (1.0 - k / q + k * k) / a0;
    k = tan(pi * 38.13547087602444 / (double)rate);
    q = 0.5003270373238773;
    a0 = 1.0 + k / q + k * k;
    m->rlb.b0 = 1.0;
    m->rlb.b1 = -2.0;
    m->rlb.b2 = 1.0;
    m->rlb.a1 = 2.0 * (k * k - 1.0) / a0;
    m->rlb.a2 = (1.0 - k / q + k * k) / a0;
    m->sub_len = rate / 10u;
    for (phase = 0; phase < TP_PHASES; phase++) {
        for (tap = 0; tap < TP_TAPS; tap++) {
            /* Phase p yields the point p/4 of a sample after x[n - TP_TAPS/2]. */
            double t = (double)tap - (double)(TP_TAPS / 2u) + (double)phase / (double)TP_PHASES;
            double sinc = t == 0.0 ? 1.0 : sin(pi * t) / (pi * t);
            double window = 0.5 + 0.5 * cos(pi * t / ((double)TP_TAPS / 2.0 + 0.5));
            m->fir[phase][TP_TAPS - 1u - tap] = sinc * window;
        }
    }
}

static double biquad(const Biquad *f, double *z, double x) {
    double y = f->b0 * x + z[0];
    z[0] = f->b1 * x - f->a1 * y + z[1];
    z[1] = f->b2 * x - f->a2 * y;
    return y;
}

static void meter_push(Meter *m, float left, float right) {
    float in[2];
    uint32_t channel;
    in[0] = left;
    in[1] = right;
    for (channel = 0; channel < 2u; channel++) {
        double x = in[channel];
        double y = biquad(&m->rlb, m->state[channel] + 2, biquad(&m->pre, m->state[channel], x));
        uint32_t phase;
        m->sub_energy += y * y;
        if (fabs(x) > m->sample_peak) {
            m->sample_peak = fabs(x);
        }
        m->history[channel][m->history_pos] = in[channel];
        for (phase = 0; phase < TP_PHASES; phase++) {
            double acc = 0.0;
            uint32_t tap;
            for (tap = 0; tap < TP_TAPS; tap++) {
                /* tap 0 = oldest sample in the ring. */
                acc += m->fir[phase][tap] * m->history[channel][(m->history_pos + 1u + tap) % TP_TAPS];
            }
            if (fabs(acc) > m->true_peak) {
                m->true_peak = fabs(acc);
            }
        }
    }
    m->history_pos = (m->history_pos + 1u) % TP_TAPS;
    if (++m->sub_pos == m->sub_len) {
        m->recent[m->sub_count % 4u] = m->sub_energy;
        m->sub_count++;
        m->sub_energy = 0.0;
        m->sub_pos = 0u;
        if (m->sub_count >= 4u) {
            double z = (m->recent[0] + m->recent[1] + m->recent[2] + m->recent[3]) / (4.0 * (double)m->sub_len);
            if (m->block_count == m->block_capacity) {
                size_t capacity = m->block_capacity ? m->block_capacity * 2u : 256u;
                double *grown = (double *)realloc(m->blocks, capacity * sizeof *grown);
                if (grown == NULL) {
                    m->failed = 1u;
                    return;
                }
                m->blocks = grown;
                m->block_capacity = capacity;
            }
            m->blocks[m->block_count++] = z;
        }
    }
}

static double loudness(double z) {
    return -0.691 + 10.0 * log10(z);
}

static double meter_integrated(const Meter *m) {
    double sum = 0.0;
    double gate;
    size_t count = 0u;
    size_t index;
    for (index = 0; index < m->block_count; index++) {
        if (m->blocks[index] > 0.0 && loudness(m->blocks[index]) > -70.0) {
            sum += m->blocks[index];
            count++;
        }
    }
    if (count == 0u) {
        return -HUGE_VAL;
    }
    gate = loudness(sum / (double)count) - 10.0;
    sum = 0.0;
    count = 0u;
    for (index = 0; index < m->block_count; index++) {
        if (m->blocks[index] > 0.0 && loudness(m->blocks[index]) > -70.0 && loudness(m->blocks[index]) > gate) {
            sum += m->blocks[index];
            count++;
        }
    }
    return count == 0u ? -HUGE_VAL : loudness(sum / (double)count);
}

uint32_t su_audio_measure_stereo(const float *stereo, uint64_t frames, uint32_t sample_rate,
    double *integrated_lufs, double *true_peak) {
    Meter m;
    uint64_t index;
    if ((stereo == NULL && frames != 0u) || sample_rate < 8000u || sample_rate % 10u != 0u
        || integrated_lufs == NULL || true_peak == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    meter_init(&m, sample_rate);
    for (index = 0; index < frames; index++) {
        meter_push(&m, stereo[index * 2u], stereo[index * 2u + 1u]);
    }
    *integrated_lufs = meter_integrated(&m);
    *true_peak = m.true_peak;
    free(m.blocks);
    return m.failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* ---- manifest/trace validation and hashing ---- */

static uint32_t validate(const SuAudioOfflineManifest *manifest, const SuAudioTraceCommand *trace, uint32_t count) {
    uint32_t index;
    if (manifest == NULL || manifest->struct_size != sizeof *manifest || manifest->assets == NULL
        || manifest->graph == NULL || (manifest->graph_source == NULL && manifest->graph_source_bytes != 0u)
        || (manifest->asset_refs == NULL && manifest->asset_ref_count != 0u)
        || manifest->asset_ref_count > SU_AUDIO_OFFLINE_MAX_ASSET_REFS
        || (manifest->curves == NULL && manifest->curve_count != 0u) || manifest->curve_count > SU_AUDIO_SCENE_MAX_CURVES
        || (manifest->emitters == NULL && manifest->emitter_count != 0u)
        || manifest->emitter_count > SU_AUDIO_SCENE_MAX_EMITTERS
        || manifest->events.sample_rate % 10u != 0u || manifest->events.sample_rate < 8000u
        || (trace == NULL && count != 0u) || count > SU_AUDIO_OFFLINE_MAX_TRACE) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (index = 0; index < count; index++) {
        const SuAudioTraceCommand *command = &trace[index];
        if (command->kind < SU_AUDIO_TRACE_POST || command->kind > SU_AUDIO_TRACE_LISTENER
            || command->emitter_index > manifest->emitter_count
            || (index > 0u && command->sample_time < trace[index - 1u].sample_time)) {
            return SU_E_INVALID_ARGUMENT;
        }
        if ((command->kind == SU_AUDIO_TRACE_STOP_EMITTER || command->kind == SU_AUDIO_TRACE_EMITTER_MOVE)
            && command->emitter_index == 0u) {
            return SU_E_INVALID_ARGUMENT;
        }
        if (command->kind == SU_AUDIO_TRACE_STOP_INSTANCE
            && (command->target >= index || trace[command->target].kind != SU_AUDIO_TRACE_POST)) {
            return SU_E_INVALID_ARGUMENT;
        }
    }
    return SU_OK;
}

static void hash_source(const SuAudioOfflineManifest *m, const SuAudioTraceCommand *trace, uint32_t count,
    uint8_t source[32], uint8_t graph[32]) {
    SuSha256 ctx;
    uint32_t index;
    su_sha256_init(&ctx);
    if (m->graph_source_bytes != 0u) {
        su_sha256_update(&ctx, m->graph_source, m->graph_source_bytes);
    }
    su_sha256_final(&ctx, graph);
    su_sha256_init(&ctx);
    su_sha256_update(&ctx, SU_AUDIO_OFFLINE_PROFILE, sizeof SU_AUDIO_OFFLINE_PROFILE - 1u);
    su_sha256_update(&ctx, graph, 32u);
    hash32(&ctx, m->events.sample_rate);
    hash32(&ctx, m->events.max_instances);
    hash32(&ctx, m->events.max_leaves_per_instance);
    hash32(&ctx, m->events.max_logical_voices);
    hash32(&ctx, m->events.command_capacity);
    hash64(&ctx, m->events.project_seed);
    hash64(&ctx, m->events.session_generation);
    hash64(&ctx, m->events.start_sample);
    hash32(&ctx, m->asset_ref_count);
    for (index = 0; index < m->asset_ref_count; index++) {
        hash32(&ctx, m->asset_refs[index].id);
        hash32(&ctx, m->asset_refs[index].generation);
    }
    hash32(&ctx, m->curve_count);
    for (index = 0; index < m->curve_count; index++) {
        uint32_t point;
        hash32(&ctx, m->curves[index].point_count);
        for (point = 0; point < m->curves[index].point_count && m->curves[index].points != NULL; point++) {
            const SuAudioCurvePoint *p = &m->curves[index].points[point];
            hashf(&ctx, p->distance_m);
            hashf(&ctx, p->gain_db);
            hashf(&ctx, p->lowpass_hz);
            hashf(&ctx, p->highpass_hz);
            hashf(&ctx, p->spread_rad);
            hashf(&ctx, p->scale);
        }
    }
    hash32(&ctx, m->emitter_count);
    for (index = 0; index < m->emitter_count; index++) {
        hash_pos(&ctx, &m->emitters[index].pos);
        hash_vec(&ctx, &m->emitters[index].vel);
        hash32(&ctx, m->emitters[index].curve);
        hashf(&ctx, m->emitters[index].scale);
    }
    hash_pos(&ctx, &m->listener.pos);
    hashf(&ctx, m->listener.rot.x);
    hashf(&ctx, m->listener.rot.y);
    hashf(&ctx, m->listener.rot.z);
    hashf(&ctx, m->listener.rot.w);
    hash_vec(&ctx, &m->listener.vel);
    hash32(&ctx, count);
    for (index = 0; index < count; index++) {
        const SuAudioTraceCommand *c = &trace[index];
        hash32(&ctx, c->kind);
        hash32(&ctx, c->event_id);
        hash64(&ctx, c->sample_time);
        hash64(&ctx, c->commit_seq);
        hash32(&ctx, c->emitter_index);
        hash32(&ctx, c->scope_id);
        hash32(&ctx, c->target);
        hash32(&ctx, c->release_frames);
        hash_pos(&ctx, &c->pos);
        hash_vec(&ctx, &c->vel);
        hashf(&ctx, c->rot.x);
        hashf(&ctx, c->rot.y);
        hashf(&ctx, c->rot.z);
        hashf(&ctx, c->rot.w);
    }
    su_sha256_final(&ctx, source);
}

/* ---- session ---- */

void su_audio_offline_end(SuAudioOffline *offline) {
    uint32_t index;
    if (offline == NULL) {
        return;
    }
    su_audio_events_destroy(offline->events);
    for (index = 0; index < offline->pinned; index++) {
        (void)su_audio_asset_release(offline->manifest.assets, offline->refs[index]);
    }
    su_audio_scene_destroy(offline->scene);
    free(offline->meter.blocks);
    free(offline->trace);
    free(offline->handles);
    free(offline->emitters);
    free(offline->refs);
    free(offline);
}

static void apply_command(SuAudioOffline *off, uint32_t index) {
    const SuAudioTraceCommand *c = &off->trace[index];
    SuAudioEmitter emitter = c->emitter_index ? off->emitters[c->emitter_index - 1u] : 0u;
    uint32_t status = SU_E_INVALID_ARGUMENT;
    switch (c->kind) {
    case SU_AUDIO_TRACE_POST: {
        SuAudioTrigger trigger;
        SuAudioGraphDiagnostic diagnostic;
        memset(&trigger, 0, sizeof trigger);
        trigger.struct_size = sizeof trigger;
        trigger.event_id = c->event_id;
        trigger.emitter = emitter;
        trigger.sample_time = c->sample_time;
        trigger.commit_seq = c->commit_seq;
        trigger.session_generation = off->manifest.events.session_generation;
        trigger.scope_id = c->scope_id;
        status = su_audio_event_post(off->events, &trigger, &off->handles[index], &diagnostic);
        break;
    }
    case SU_AUDIO_TRACE_STOP_INSTANCE:
        status = su_audio_instance_stop(off->events, off->handles[c->target], c->sample_time, c->release_frames);
        break;
    case SU_AUDIO_TRACE_STOP_EMITTER:
        status = su_audio_emitter_stop(off->events, emitter, c->sample_time, c->release_frames);
        break;
    case SU_AUDIO_TRACE_STOP_SCOPE:
        status = su_audio_scope_stop(off->events, c->scope_id, c->sample_time, c->release_frames);
        break;
    case SU_AUDIO_TRACE_EMITTER_MOVE:
        status = su_audio_emitter_update(off->scene, emitter, c->pos, c->vel);
        break;
    default: {
        SuAudioCameraListener listener;
        listener.pos = c->pos;
        listener.rot = c->rot;
        listener.vel = c->vel;
        status = su_audio_scene_listener_set(off->scene, &listener);
        break;
    }
    }
    if (status == SU_OK) {
        off->applied++;
    } else {
        off->failed++;
    }
    if (off->observer != NULL) {
        off->observer(off->observer_user, index, c, status, su_audio_events_clock(off->events),
            c->kind == SU_AUDIO_TRACE_POST ? off->handles[index]
            : c->kind == SU_AUDIO_TRACE_STOP_INSTANCE ? off->handles[c->target] : 0u);
    }
}

static uint32_t refill(SuAudioOffline *off) {
    uint64_t clock = su_audio_events_clock(off->events);
    SuAudioEventStats stats;
    uint64_t horizon = clock > UINT64_MAX - SU_AUDIO_EVENT_QUANTUM ? UINT64_MAX : clock + SU_AUDIO_EVENT_QUANTUM;
    uint32_t status;
    /* Everything due before the end of the next quantum is submitted before it
     * renders; late commands are submitted too and counted as rejected. */
    while (off->trace_next < off->trace_count && off->trace[off->trace_next].sample_time < horizon) {
        apply_command(off, off->trace_next++);
    }
    status = su_audio_events_render(off->events, off->cache, SU_AUDIO_EVENT_QUANTUM);
    if (status != SU_OK) {
        return status;
    }
    su_audio_events_stats(off->events, &stats);
    if (stats.instances > off->peak_instances) {
        off->peak_instances = stats.instances;
    }
    if (stats.logical_voices > off->peak_voices) {
        off->peak_voices = stats.logical_voices;
    }
    su_audio_events_update(off->events);
    off->cached = SU_AUDIO_EVENT_QUANTUM;
    off->consumed = 0u;
    return SU_OK;
}

static uint32_t render_frames(SuAudioOffline *off, float *out, uint32_t frames) {
    uint32_t done = 0u;
    while (done < frames) {
        uint32_t take;
        uint32_t index;
        if (off->consumed == off->cached) {
            uint32_t status = refill(off);
            if (status != SU_OK) {
                return status;
            }
        }
        take = off->cached - off->consumed;
        if (take > frames - done) {
            take = frames - done;
        }
        for (index = 0; index < take; index++) {
            const float *frame = off->cache + (size_t)(off->consumed + index) * 2u;
            uint8_t bytes[8];
            put32(bytes, float_bits(frame[0]));
            put32(bytes + 4, float_bits(frame[1]));
            su_sha256_update(&off->pcm, bytes, sizeof bytes);
            meter_push(&off->meter, frame[0], frame[1]);
        }
        if (out != NULL) {
            memcpy(out + (size_t)done * 2u, off->cache + (size_t)off->consumed * 2u, (size_t)take * 2u * sizeof *out);
        }
        off->consumed += take;
        off->position += take;
        done += take;
    }
    return off->meter.failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

static uint32_t decode_ctx(const uint8_t *p, SuSha256 *ctx) {
    uint32_t index;
    for (index = 0; index < 8u; index++) {
        ctx->h[index] = get32(p + index * 4u);
    }
    ctx->total_bytes = get64(p + 32);
    ctx->buf_len = get32(p + 40);
    memcpy(ctx->buf, p + 44, 64u);
    return ctx->buf_len < 64u ? SU_OK : SU_E_SCHEMA_MISMATCH;
}

uint32_t su_audio_offline_begin(const SuAudioOfflineManifest *manifest, const SuAudioTraceCommand *trace,
    uint32_t trace_count, const void *checkpoint, size_t checkpoint_bytes, SuAudioOffline **out) {
    SuAudioOffline *off;
    SuAudioSceneDesc scene_desc;
    SuAudioCheckpointInfo info;
    uint32_t status;
    uint32_t index;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    memset(&info, 0, sizeof info);
    status = validate(manifest, trace, trace_count);
    if (status != SU_OK) {
        return status;
    }
    if (checkpoint != NULL) {
        status = su_audio_checkpoint_decode(checkpoint, checkpoint_bytes, &info);
        if (status != SU_OK) {
            return status;
        }
    }
    off = (SuAudioOffline *)calloc(1u, sizeof *off);
    if (off == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    off->manifest = *manifest;
    off->trace_count = trace_count;
    off->trace = (SuAudioTraceCommand *)calloc(trace_count ? trace_count : 1u, sizeof *off->trace);
    off->handles = (SuAudioInstance *)calloc(trace_count ? trace_count : 1u, sizeof *off->handles);
    off->emitters = (SuAudioEmitter *)calloc(manifest->emitter_count ? manifest->emitter_count : 1u, sizeof *off->emitters);
    off->refs = (SuAudioAssetRef *)calloc(manifest->asset_ref_count ? manifest->asset_ref_count : 1u, sizeof *off->refs);
    if (off->trace == NULL || off->handles == NULL || off->emitters == NULL || off->refs == NULL) {
        su_audio_offline_end(off);
        return SU_E_RESOURCE_LIMIT;
    }
    if (trace_count != 0u) {
        memcpy(off->trace, trace, (size_t)trace_count * sizeof *trace);
    }
    hash_source(manifest, trace, trace_count, off->source, off->graph);
    if (checkpoint != NULL && (memcmp(info.source_sha256, off->source, 32u) != 0
        || memcmp(info.graph_sha256, off->graph, 32u) != 0 || info.asset_ref_count != manifest->asset_ref_count
        || info.trace_next > trace_count)) {
        su_audio_offline_end(off);
        return SU_E_SCHEMA_MISMATCH;
    }
    /* Pin every declared generation; a missing or replaced one rejects the render. */
    for (index = 0; index < manifest->asset_ref_count; index++) {
        const SuAudioClip *clip = NULL;
        if (su_audio_asset_acquire(manifest->assets, manifest->asset_refs[index], &clip) != SU_OK) {
            su_audio_offline_end(off);
            return SU_E_STALE_REVISION;
        }
        off->refs[off->pinned++] = manifest->asset_refs[index];
    }
    scene_desc.struct_size = sizeof scene_desc;
    scene_desc.max_emitters = manifest->emitter_count ? manifest->emitter_count : 1u;
    scene_desc.max_curves = manifest->curve_count ? manifest->curve_count : 1u;
    status = su_audio_scene_create(&scene_desc, &off->scene);
    for (index = 0; status == SU_OK && index < manifest->curve_count; index++) {
        SuAudioCurveId id;
        status = su_audio_scene_curve_add(off->scene, &manifest->curves[index], &id);
    }
    for (index = 0; status == SU_OK && index < manifest->emitter_count; index++) {
        status = su_audio_emitter_create(off->scene, &manifest->emitters[index], &off->emitters[index]);
    }
    if (status == SU_OK) {
        status = su_audio_scene_listener_set(off->scene, &manifest->listener);
    }
    if (status == SU_OK) {
        status = su_audio_events_create(&manifest->events, manifest->assets, off->scene, manifest->graph, &off->events);
    }
    if (status != SU_OK) {
        su_audio_offline_end(off);
        return status;
    }
    su_sha256_init(&off->pcm);
    meter_init(&off->meter, manifest->events.sample_rate);
    if (checkpoint != NULL) {
        const uint8_t *bytes = (const uint8_t *)checkpoint;
        SuSha256 saved;
        SuAudioCommitCursor cursor;
        uint64_t remaining = info.position;
        double started = now_seconds();
        status = decode_ctx(bytes + 112, &saved);
        while (status == SU_OK && remaining != 0u) {
            uint32_t step = remaining > 65536u ? 65536u : (uint32_t)remaining;
            status = render_frames(off, NULL, step);
            remaining -= step;
        }
        off->render_seconds = now_seconds() - started;
        off->replayed = (uint32_t)(info.position > UINT32_MAX ? UINT32_MAX : info.position);
        if (status == SU_OK) {
            (void)su_audio_commit_cursor_snapshot(off->events, &cursor);
            if (memcmp(saved.h, off->pcm.h, sizeof saved.h) != 0 || saved.total_bytes != off->pcm.total_bytes
                || saved.buf_len != off->pcm.buf_len || memcmp(saved.buf, off->pcm.buf, saved.buf_len) != 0
                || off->trace_next != info.trace_next || cursor.session_generation != info.cursor.session_generation
                || cursor.next_seq != info.cursor.next_seq) {
                status = SU_E_SCHEMA_MISMATCH;
            }
        }
        if (status != SU_OK) {
            su_audio_offline_end(off);
            return status;
        }
    }
    *out = off;
    return SU_OK;
}

uint32_t su_audio_offline_render(SuAudioOffline *offline, float *out, uint32_t frames) {
    double started;
    uint32_t status;
    if (offline == NULL || (out == NULL && frames != 0u)) {
        return SU_E_INVALID_ARGUMENT;
    }
    started = now_seconds();
    status = render_frames(offline, out, frames);
    offline->render_seconds += now_seconds() - started;
    return status;
}

uint64_t su_audio_offline_position(const SuAudioOffline *offline) {
    return offline != NULL ? offline->position : 0u;
}

const SuAudioEvents *su_audio_offline_events(const SuAudioOffline *offline) {
    return offline != NULL ? offline->events : NULL;
}

SuAudioScene *su_audio_offline_scene(const SuAudioOffline *offline) {
    return offline != NULL ? offline->scene : NULL;
}

uint32_t su_audio_offline_observe(SuAudioOffline *offline, SuAudioOfflineObserver observer, void *user) {
    if (offline == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    offline->observer = observer;
    offline->observer_user = user;
    return SU_OK;
}

/* ---- checkpoints ----
 * 0 magic, 4 version, 8 header bytes, 12 asset ref count, 16 position,
 * 24 trace_next, 28 reserved, 32 cursor (generation, next_seq), 48 source
 * SHA-256, 80 graph SHA-256, 112 PCM digest state (h[8], total, buf_len, buf),
 * 220 profile (24 bytes, NUL padded), 244 FNV-1a 64 of all other bytes, 252 pad.
 * Asset references (id, generation) follow the header. */

uint32_t su_audio_checkpoint_encode(const SuAudioOffline *offline, void *out, size_t capacity, size_t *written) {
    uint8_t *p = (uint8_t *)out;
    size_t need;
    SuAudioCommitCursor cursor;
    uint32_t index;
    uint64_t check;
    if (written != NULL) {
        *written = 0u;
    }
    if (offline == NULL || written == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    need = SU_AUDIO_CHECKPOINT_HEADER_BYTES + (size_t)offline->pinned * 8u;
    *written = need;
    if (out == NULL || capacity < need) {
        return SU_E_RESOURCE_LIMIT;
    }
    memset(p, 0, need);
    (void)su_audio_commit_cursor_snapshot(offline->events, &cursor);
    put32(p, CHECKPOINT_MAGIC);
    put32(p + 4, SU_AUDIO_CHECKPOINT_VERSION);
    put32(p + 8, SU_AUDIO_CHECKPOINT_HEADER_BYTES);
    put32(p + 12, offline->pinned);
    put64(p + 16, offline->position);
    put32(p + 24, offline->trace_next);
    put64(p + 32, cursor.session_generation);
    put64(p + 40, cursor.next_seq);
    memcpy(p + 48, offline->source, 32u);
    memcpy(p + 80, offline->graph, 32u);
    for (index = 0; index < 8u; index++) {
        put32(p + 112 + index * 4u, offline->pcm.h[index]);
    }
    put64(p + 144, offline->pcm.total_bytes);
    put32(p + 152, (uint32_t)offline->pcm.buf_len);
    memcpy(p + 156, offline->pcm.buf, 64u);
    memcpy(p + 220, SU_AUDIO_OFFLINE_PROFILE, sizeof SU_AUDIO_OFFLINE_PROFILE - 1u);
    for (index = 0; index < offline->pinned; index++) {
        put32(p + SU_AUDIO_CHECKPOINT_HEADER_BYTES + index * 8u, offline->refs[index].id);
        put32(p + SU_AUDIO_CHECKPOINT_HEADER_BYTES + index * 8u + 4u, offline->refs[index].generation);
    }
    check = fnv64(p, 244u, UINT64_C(0xcbf29ce484222325));
    check = fnv64(p + SU_AUDIO_CHECKPOINT_HEADER_BYTES, need - SU_AUDIO_CHECKPOINT_HEADER_BYTES, check);
    put64(p + 244, check);
    return SU_OK;
}

uint32_t su_audio_checkpoint_decode(const void *data, size_t bytes, SuAudioCheckpointInfo *out) {
    const uint8_t *p = (const uint8_t *)data;
    uint8_t profile[24];
    uint64_t check;
    uint32_t refs;
    if (out == NULL || data == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    if (bytes < SU_AUDIO_CHECKPOINT_HEADER_BYTES || get32(p) != CHECKPOINT_MAGIC) {
        return SU_E_PARSE;
    }
    if (get32(p + 4) != SU_AUDIO_CHECKPOINT_VERSION || get32(p + 8) != SU_AUDIO_CHECKPOINT_HEADER_BYTES) {
        return SU_E_UNSUPPORTED_MIGRATION;
    }
    refs = get32(p + 12);
    if (refs > SU_AUDIO_OFFLINE_MAX_ASSET_REFS || bytes != SU_AUDIO_CHECKPOINT_HEADER_BYTES + (size_t)refs * 8u) {
        return SU_E_PARSE;
    }
    check = fnv64(p, 244u, UINT64_C(0xcbf29ce484222325));
    check = fnv64(p + SU_AUDIO_CHECKPOINT_HEADER_BYTES, bytes - SU_AUDIO_CHECKPOINT_HEADER_BYTES, check);
    memset(profile, 0, sizeof profile);
    memcpy(profile, SU_AUDIO_OFFLINE_PROFILE, sizeof SU_AUDIO_OFFLINE_PROFILE - 1u);
    if (check != get64(p + 244) || memcmp(profile, p + 220, sizeof profile) != 0 || get32(p + 152) >= 64u) {
        return SU_E_SCHEMA_MISMATCH;
    }
    out->version = SU_AUDIO_CHECKPOINT_VERSION;
    out->asset_ref_count = refs;
    out->position = get64(p + 16);
    out->trace_next = get32(p + 24);
    out->cursor.session_generation = get64(p + 32);
    out->cursor.next_seq = get64(p + 40);
    memcpy(out->source_sha256, p + 48, 32u);
    memcpy(out->graph_sha256, p + 80, 32u);
    return SU_OK;
}

/* ---- metrics, report and WAV ---- */

static double to_db(double linear) {
    return linear > 0.0 ? 20.0 * log10(linear) : -HUGE_VAL;
}

static uint64_t frames_to_ns(uint64_t frames, uint32_t rate) {
    return (frames / rate) * UINT64_C(1000000000) + ((frames % rate) * UINT64_C(1000000000)) / rate;
}

uint32_t su_audio_offline_metrics(const SuAudioOffline *offline, SuAudioOfflineMetrics *out) {
    SuAudioEventStats stats;
    SuSha256 pcm;
    uint8_t digest[32];
    if (offline == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    su_audio_events_stats(offline->events, &stats);
    out->sample_rate = offline->manifest.events.sample_rate;
    out->start_sample = offline->manifest.events.start_sample;
    out->frames = offline->position;
    out->start_pts_ns = frames_to_ns(out->start_sample, out->sample_rate);
    out->end_pts_ns = out->start_pts_ns + frames_to_ns(out->frames, out->sample_rate);
    out->sample_peak = offline->meter.sample_peak;
    out->true_peak = offline->meter.true_peak > offline->meter.sample_peak
        ? offline->meter.true_peak : offline->meter.sample_peak;
    out->sample_peak_dbfs = to_db(out->sample_peak);
    out->true_peak_dbtp = to_db(out->true_peak);
    out->integrated_lufs = meter_integrated(&offline->meter);
    out->loudness_blocks = offline->meter.block_count;
    out->peak_instances = offline->peak_instances;
    out->peak_logical_voices = offline->peak_voices;
    out->admitted = stats.admitted;
    out->refused = stats.refused;
    out->late_commands = stats.late_commands;
    out->trace_applied = offline->applied;
    out->trace_failed = offline->failed;
    out->replayed_frames_on_begin = offline->replayed;
    out->render_seconds = offline->render_seconds;
    su_sha256_hex(offline->source, out->source_sha256);
    su_sha256_hex(offline->graph, out->graph_sha256);
    pcm = offline->pcm;
    su_sha256_final(&pcm, digest);
    su_sha256_hex(digest, out->pcm_sha256);
    return SU_OK;
}

static const char *toolchain(void) {
#if defined(__clang__)
    return "clang " __clang_version__;
#elif defined(__GNUC__)
    return "gcc " __VERSION__;
#elif defined(_MSC_VER)
#define SU_STR2(x) #x
#define SU_STR(x) SU_STR2(x)
    return "msvc " SU_STR(_MSC_FULL_VER);
#else
    return "unknown";
#endif
}

static void json_number(char *buf, size_t cap, double value) {
    if (value == -HUGE_VAL || value != value) {
        (void)snprintf(buf, cap, "null");
    } else {
        (void)snprintf(buf, cap, "%.6f", value);
    }
}

uint32_t su_audio_offline_report_json(const SuAudioOffline *offline, char *out, size_t capacity, size_t *written) {
    SuAudioOfflineMetrics m;
    char lufs[48];
    char peak[48];
    char tpeak[48];
    char tool[160];
    size_t index;
    size_t j = 0u;
    int n;
    if (written != NULL) {
        *written = 0u;
    }
    if (offline == NULL || written == NULL || su_audio_offline_metrics(offline, &m) != SU_OK) {
        return SU_E_INVALID_ARGUMENT;
    }
    json_number(lufs, sizeof lufs, m.integrated_lufs);
    json_number(peak, sizeof peak, m.sample_peak_dbfs);
    json_number(tpeak, sizeof tpeak, m.true_peak_dbtp);
    for (index = 0; toolchain()[index] != '\0' && j + 1u < sizeof tool; index++) {
        char c = toolchain()[index];
        if (c == '(') {
            break; /* drop vendor/repository suffixes */
        }
        if (c != '"' && c != '\\' && (unsigned char)c >= 0x20u) {
            tool[j++] = c;
        }
    }
    while (j > 0u && tool[j - 1u] == ' ') {
        j--;
    }
    tool[j] = '\0';
    n = snprintf(out, out != NULL ? capacity : 0u,
        "{\"schema\":\"su_audio_offline_report_v1\",\"numeric_profile\":\"%s\","
        "\"timing_mode\":\"offline_nonrealtime\",\"decoder\":\"prepared_pcm_float32\",\"toolchain\":\"%s\","
        "\"hardware_counters\":{\"available\":false,\"reason\":\"no portable performance-counter interface\"},"
        "\"source_sha256\":\"%s\",\"graph_sha256\":\"%s\",\"pcm_sha256\":\"%s\","
        "\"sample_rate\":%u,\"start_sample\":%llu,\"frames\":%llu,\"start_pts_ns\":%llu,\"end_pts_ns\":%llu,"
        "\"sample_peak_dbfs\":%s,\"true_peak_dbtp\":%s,\"true_peak_oversampling\":4,\"integrated_lufs\":%s,"
        "\"loudness_blocks\":%llu,\"peak_instances\":%u,\"peak_logical_voices\":%u,\"admitted\":%llu,"
        "\"refused\":%llu,\"late_commands\":%llu,\"trace_applied\":%u,\"trace_failed\":%u,"
        "\"replayed_frames_on_begin\":%u,\"render_seconds\":%.6f}",
        SU_AUDIO_OFFLINE_PROFILE, tool, m.source_sha256, m.graph_sha256, m.pcm_sha256, m.sample_rate,
        (unsigned long long)m.start_sample, (unsigned long long)m.frames, (unsigned long long)m.start_pts_ns,
        (unsigned long long)m.end_pts_ns, peak, tpeak, lufs, (unsigned long long)m.loudness_blocks,
        m.peak_instances, m.peak_logical_voices, (unsigned long long)m.admitted, (unsigned long long)m.refused,
        (unsigned long long)m.late_commands, m.trace_applied, m.trace_failed, m.replayed_frames_on_begin,
        m.render_seconds);
    if (n < 0) {
        return SU_E_INTERNAL;
    }
    *written = (size_t)n;
    return out != NULL && (size_t)n < capacity ? SU_OK : SU_E_RESOURCE_LIMIT;
}

uint32_t su_audio_wav_encode_f32(const float *stereo, uint64_t frames, uint32_t sample_rate,
    void *out, size_t capacity, size_t *written) {
    uint8_t *p = (uint8_t *)out;
    uint64_t data;
    uint64_t index;
    if (written != NULL) {
        *written = 0u;
    }
    if (written == NULL || (stereo == NULL && frames != 0u) || sample_rate == 0u || frames > UINT32_MAX / 8u - 64u) {
        return SU_E_INVALID_ARGUMENT;
    }
    data = frames * 8u;
    *written = (size_t)(44u + data);
    if (out == NULL || capacity < 44u + data) {
        return SU_E_RESOURCE_LIMIT;
    }
    memcpy(p, "RIFF", 4u);
    put32(p + 4, (uint32_t)(36u + data));
    memcpy(p + 8, "WAVEfmt ", 8u);
    put32(p + 16, 16u);
    p[20] = 3u; /* WAVE_FORMAT_IEEE_FLOAT */
    p[21] = 0u;
    p[22] = 2u;
    p[23] = 0u;
    put32(p + 24, sample_rate);
    put32(p + 28, sample_rate * 8u);
    p[32] = 8u;
    p[33] = 0u;
    p[34] = 32u;
    p[35] = 0u;
    memcpy(p + 36, "data", 4u);
    put32(p + 40, (uint32_t)data);
    for (index = 0; index < frames * 2u; index++) {
        put32(p + 44 + index * 4u, float_bits(stereo[index]));
    }
    return SU_OK;
}
