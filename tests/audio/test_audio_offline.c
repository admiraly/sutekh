#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sutekh/audio_offline.h"

static unsigned checks;
static unsigned failures;

#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        failures++; \
        fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
    } \
} while (0)

#define RATE 48000u
#define LENGTH 96000u
#define BURST 4800u
#define TONE 9600u

static const char graph_json[] =
    "{\"schema\":\"su_audio_events_v1\",\"numeric_profile\":\"audio_float_local_v1\","
    "\"seed_policy\":\"event_emitter_sequence_v1\",\"sample_rate\":48000,\"nodes\":["
    "{\"id\":1,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":1}},"
    "{\"id\":2,\"type\":\"sound\",\"asset\":{\"id\":2,\"generation\":2},\"gain\":0.5},"
    "{\"id\":3,\"type\":\"sound\",\"asset\":{\"id\":3,\"generation\":3}},"
    "{\"id\":4,\"type\":\"random\",\"children\":[{\"node\":1,\"weight\":1},{\"node\":3,\"weight\":1}]},"
    "{\"id\":5,\"type\":\"sequence\",\"children\":[{\"node\":1,\"offset_frames\":0},{\"node\":3,\"offset_frames\":300}]},"
    "{\"id\":6,\"type\":\"sound\",\"asset\":{\"id\":4,\"generation\":4}}],"
    "\"events\":[{\"id\":1,\"root\":4},{\"id\":2,\"root\":2},{\"id\":3,\"root\":5},{\"id\":4,\"root\":6}]}";

static float burst[BURST];
static float tone[TONE];
static float thud[BURST];
static float impulse[4] = {0.5f, 0.0f, 0.0f, 0.0f};

typedef struct Fixture {
    SuAudioAssets *assets;
    SuAudioGraph *graph;
    SuAudioAssetRef refs[4];
    SuAudioCurvePoint points[2];
    SuAudioCurveDesc curve;
    SuAudioEmitterSnapshot emitters[2];
    SuAudioOfflineManifest manifest;
    SuAudioTraceCommand trace[96];
    uint32_t trace_count;
} Fixture;

static void synthesize(void) {
    uint32_t state = 0x12345678u;
    uint32_t index;
    for (index = 0; index < BURST; index++) {
        double env = exp(-(double)index / 900.0);
        state = state * 1664525u + 1013904223u;
        burst[index] = (float)(env * ((double)(state >> 8) / 8388608.0 - 1.0) * 0.4);
        thud[index] = (float)(env * sin(2.0 * 3.14159265358979 * 55.0 * index / RATE) * 0.6);
    }
    for (index = 0; index < TONE; index++) {
        tone[index] = (float)(0.3 * sin(2.0 * 3.14159265358979 * 440.0 * index / RATE));
    }
}

static SuWorldPos meters(double x, double z) {
    SuWorldPos pos;
    pos.x = (int64_t)(x * 4294967296.0);
    pos.y = 0;
    pos.z = (int64_t)(z * 4294967296.0);
    return pos;
}

static void add(Fixture *f, uint32_t kind, uint64_t at, uint32_t event_id, uint32_t emitter, uint32_t extra) {
    SuAudioTraceCommand *c = &f->trace[f->trace_count];
    memset(c, 0, sizeof *c);
    c->kind = kind;
    c->sample_time = at;
    c->event_id = event_id;
    c->emitter_index = emitter;
    c->commit_seq = f->trace_count;
    c->scope_id = kind == SU_AUDIO_TRACE_POST ? event_id : extra;
    c->target = extra;
    c->release_frames = kind == SU_AUDIO_TRACE_STOP_INSTANCE ? 480u : 0u;
    c->rot.w = 1.0f;
    c->pos = meters((double)extra - 20.0, -5.0);
    f->trace_count++;
}

static void fixture(Fixture *f, uint64_t start) {
    SuAudioGraphLimits limits = {16u, 32u, 8u, 16u, 8192u};
    SuAudioGraphDiagnostic diagnostic;
    SuAudioClip clip = {burst, BURST, 1u, RATE};
    uint32_t index;
    memset(f, 0, sizeof *f);
    CHECK(su_audio_assets_create(8u, 1u << 20, &f->assets) == SU_OK);
    CHECK(su_audio_asset_prepare(f->assets, 1u, &clip, &f->refs[0]) == SU_OK);
    clip.samples = tone;
    clip.frames = TONE;
    CHECK(su_audio_asset_prepare(f->assets, 2u, &clip, &f->refs[1]) == SU_OK);
    clip.samples = thud;
    clip.frames = BURST;
    CHECK(su_audio_asset_prepare(f->assets, 3u, &clip, &f->refs[2]) == SU_OK);
    clip.samples = impulse;
    clip.frames = 4u;
    CHECK(su_audio_asset_prepare(f->assets, 4u, &clip, &f->refs[3]) == SU_OK);
    CHECK(su_audio_graph_prepare(graph_json, sizeof graph_json - 1u, f->assets, &limits, &f->graph,
        &diagnostic) == SU_OK);
    if (f->graph == NULL) {
        fprintf(stderr, "graph: %s (%s)\n", diagnostic.message, diagnostic.bound);
    }
    f->points[0].distance_m = 1.0f;
    f->points[0].lowpass_hz = 20000.0f;
    f->points[0].scale = 1.0f;
    f->points[1].distance_m = 60.0f;
    f->points[1].gain_db = -24.0f;
    f->points[1].lowpass_hz = 2000.0f;
    f->points[1].scale = 1.0f;
    f->curve.struct_size = sizeof f->curve;
    f->curve.points = f->points;
    f->curve.point_count = 2u;
    for (index = 0; index < 2u; index++) {
        f->emitters[index].pos = meters(index ? 15.0 : -8.0, -10.0);
        f->emitters[index].curve = 1u;
        f->emitters[index].scale = 1.0f;
    }
    f->manifest.struct_size = sizeof f->manifest;
    f->manifest.events.struct_size = sizeof f->manifest.events;
    f->manifest.events.sample_rate = RATE;
    f->manifest.events.max_instances = 16u;
    f->manifest.events.max_leaves_per_instance = 4u;
    f->manifest.events.max_logical_voices = 32u;
    f->manifest.events.command_capacity = 8u;
    f->manifest.events.project_seed = 77u;
    f->manifest.events.session_generation = 1u;
    f->manifest.events.start_sample = start;
    f->manifest.assets = f->assets;
    f->manifest.graph = f->graph;
    f->manifest.graph_source = graph_json;
    f->manifest.graph_source_bytes = sizeof graph_json - 1u;
    f->manifest.asset_refs = f->refs;
    f->manifest.asset_ref_count = 4u;
    f->manifest.curves = &f->curve;
    f->manifest.curve_count = 1u;
    f->manifest.emitters = f->emitters;
    f->manifest.emitter_count = 2u;
    f->manifest.listener.rot.w = 1.0f;
    /* A synthetic skirmish: random shots, a sustained hum, layered impacts,
     * moving emitters/camera and every stop flavour. */
    add(f, SU_AUDIO_TRACE_POST, start + 0u, 2u, 0u, 0u);
    for (index = 0; index < 40u; index++) {
        uint64_t at = start + 37u + index * 2111u;
        add(f, SU_AUDIO_TRACE_POST, at, index % 5u == 4u ? 3u : 1u, 1u + index % 2u, 0u);
        if (index % 8u == 3u) {
            add(f, SU_AUDIO_TRACE_EMITTER_MOVE, at + 64u, 0u, 1u + index % 2u, index);
        }
        if (index % 10u == 6u) {
            add(f, SU_AUDIO_TRACE_LISTENER, at + 129u, 0u, 0u, index);
        }
    }
    add(f, SU_AUDIO_TRACE_STOP_INSTANCE, start + 5000u, 0u, 0u, 0u);
    add(f, SU_AUDIO_TRACE_POST, start + 7000u, 2u, 2u, 0u);
    add(f, SU_AUDIO_TRACE_STOP_EMITTER, start + 8000u, 0u, 2u, 0u);
    add(f, SU_AUDIO_TRACE_STOP_SCOPE, start + 90000u, 0u, 0u, 1u);
    /* Restore sorted order for the commands appended out of time order. */
    {
        uint32_t i;
        uint32_t j;
        for (i = 1u; i < f->trace_count; i++) {
            for (j = i; j > 0u && f->trace[j - 1u].sample_time > f->trace[j].sample_time; j--) {
                SuAudioTraceCommand tmp = f->trace[j];
                f->trace[j] = f->trace[j - 1u];
                f->trace[j - 1u] = tmp;
            }
        }
        for (i = 0; i < f->trace_count; i++) {
            f->trace[i].commit_seq = i;
            if (f->trace[i].kind == SU_AUDIO_TRACE_STOP_INSTANCE) {
                f->trace[i].target = 0u; /* the hum post stays first */
            }
        }
    }
}

static void cleanup(Fixture *f) {
    su_audio_graph_release(f->graph);
    CHECK(su_audio_assets_destroy(f->assets) == SU_OK);
}

static double seconds(void) {
    struct timespec ts;
    (void)timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static float whole[LENGTH * 2u];
static float other[LENGTH * 2u];
static float tone_buf[20u * RATE * 2u];

static void render_whole(Fixture *f, float *out, SuAudioOfflineMetrics *metrics) {
    SuAudioOffline *off = NULL;
    CHECK(su_audio_offline_begin(&f->manifest, f->trace, f->trace_count, NULL, 0u, &off) == SU_OK);
    if (off == NULL) {
        return;
    }
    CHECK(su_audio_offline_render(off, out, LENGTH) == SU_OK);
    CHECK(su_audio_offline_metrics(off, metrics) == SU_OK);
    su_audio_offline_end(off);
}

static void repeat_and_chunks(void) {
    Fixture f;
    SuAudioOfflineMetrics a;
    SuAudioOfflineMetrics b;
    SuAudioOffline *off = NULL;
    static const uint32_t sizes[] = {1u, 7u, 127u, 128u, 129u, 1000u, 4099u};
    uint32_t pass;
    double energy = 0.0;
    uint32_t index;
    fixture(&f, 0u);
    render_whole(&f, whole, &a);
    for (index = 0; index < LENGTH * 2u; index++) {
        energy += (double)whole[index] * whole[index];
    }
    CHECK(energy > 1.0);
    CHECK(a.trace_failed == 0u && a.trace_applied == f.trace_count);
    CHECK(a.admitted == 42u && a.refused == 0u && a.late_commands == 0u);
    render_whole(&f, other, &b);
    CHECK(memcmp(whole, other, sizeof whole) == 0);
    CHECK(strcmp(a.pcm_sha256, b.pcm_sha256) == 0);
    CHECK(strcmp(a.source_sha256, b.source_sha256) == 0);
    for (pass = 0; pass < sizeof sizes / sizeof sizes[0]; pass++) {
        uint32_t done = 0u;
        uint32_t step = 0u;
        memset(other, 0, sizeof other);
        CHECK(su_audio_offline_begin(&f.manifest, f.trace, f.trace_count, NULL, 0u, &off) == SU_OK);
        while (off != NULL && done < LENGTH) {
            uint32_t count = sizes[pass] + (step++ * 13u) % (sizes[pass] + 1u);
            if (count > LENGTH - done) {
                count = LENGTH - done;
            }
            CHECK(su_audio_offline_render(off, other + (size_t)done * 2u, count) == SU_OK);
            done += count;
        }
        CHECK(memcmp(whole, other, sizeof whole) == 0);
        CHECK(su_audio_offline_metrics(off, &b) == SU_OK);
        CHECK(strcmp(a.pcm_sha256, b.pcm_sha256) == 0);
        CHECK(b.integrated_lufs == a.integrated_lufs && b.true_peak == a.true_peak);
        su_audio_offline_end(off);
    }
    /* Session meters equal the standalone measurement of the same PCM. */
    {
        double lufs = 0.0;
        double peak = 0.0;
        CHECK(su_audio_measure_stereo(whole, LENGTH, RATE, &lufs, &peak) == SU_OK);
        CHECK(lufs == a.integrated_lufs && peak == a.true_peak);
        CHECK(a.integrated_lufs > -60.0 && a.integrated_lufs < 0.0);
        CHECK(a.true_peak >= a.sample_peak);
        CHECK(a.peak_instances >= 2u && a.peak_logical_voices >= 2u);
    }
    /* A different trace changes the source digest. */
    f.trace[3].event_id ^= 2u;
    render_whole(&f, other, &b);
    CHECK(strcmp(a.source_sha256, b.source_sha256) != 0);
    CHECK(strcmp(a.graph_sha256, b.graph_sha256) == 0);
    cleanup(&f);
}

static void checkpoints(void) {
    static const uint32_t splits[] = {1u, 127u, 128u, 30001u, 62000u, LENGTH};
    Fixture f;
    SuAudioOfflineMetrics full;
    SuAudioOfflineMetrics resumed;
    uint8_t blob[512];
    size_t size = 0u;
    uint32_t index;
    fixture(&f, 0u);
    render_whole(&f, whole, &full);
    for (index = 0; index < sizeof splits / sizeof splits[0]; index++) {
        SuAudioOffline *first = NULL;
        SuAudioOffline *second = NULL;
        SuAudioCheckpointInfo info;
        SuAudioCommitCursor cursor;
        uint32_t split = splits[index];
        memset(other, 0, sizeof other);
        CHECK(su_audio_offline_begin(&f.manifest, f.trace, f.trace_count, NULL, 0u, &first) == SU_OK);
        CHECK(su_audio_offline_render(first, other, split) == SU_OK);
        CHECK(su_audio_checkpoint_encode(first, NULL, 0u, &size) == SU_E_RESOURCE_LIMIT);
        CHECK(size == SU_AUDIO_CHECKPOINT_HEADER_BYTES + 4u * 8u);
        CHECK(su_audio_checkpoint_encode(first, blob, sizeof blob, &size) == SU_OK);
        CHECK(su_audio_commit_cursor_snapshot(su_audio_offline_events(first), &cursor) == SU_OK);
        su_audio_offline_end(first);
        CHECK(su_audio_checkpoint_decode(blob, size, &info) == SU_OK);
        CHECK(info.position == split && info.asset_ref_count == 4u);
        CHECK(info.cursor.next_seq == cursor.next_seq && info.cursor.session_generation == 1u);
        CHECK(su_audio_offline_begin(&f.manifest, f.trace, f.trace_count, blob, size, &second) == SU_OK);
        if (second == NULL) {
            continue;
        }
        CHECK(su_audio_offline_position(second) == split);
        /* The committed-event watermark resumes, so replayed posts are not re-admitted. */
        CHECK(su_audio_commit_cursor_snapshot(su_audio_offline_events(second), &cursor) == SU_OK);
        CHECK(cursor.next_seq == info.cursor.next_seq);
        CHECK(su_audio_offline_render(second, other + (size_t)split * 2u, LENGTH - split) == SU_OK);
        CHECK(memcmp(whole, other, sizeof whole) == 0);
        CHECK(su_audio_offline_metrics(second, &resumed) == SU_OK);
        CHECK(strcmp(full.pcm_sha256, resumed.pcm_sha256) == 0);
        CHECK(resumed.admitted == full.admitted && resumed.integrated_lufs == full.integrated_lufs);
        CHECK(resumed.replayed_frames_on_begin == split);
        su_audio_offline_end(second);
    }
    /* Corruption, truncation, version and source mismatches are refused. */
    {
        SuAudioOffline *off = NULL;
        SuAudioCheckpointInfo info;
        blob[60] ^= 1u;
        CHECK(su_audio_checkpoint_decode(blob, size, &info) == SU_E_SCHEMA_MISMATCH);
        CHECK(su_audio_offline_begin(&f.manifest, f.trace, f.trace_count, blob, size, &off) == SU_E_SCHEMA_MISMATCH);
        blob[60] ^= 1u;
        CHECK(su_audio_checkpoint_decode(blob, size - 1u, &info) == SU_E_PARSE);
        blob[4] = 9u;
        CHECK(su_audio_checkpoint_decode(blob, size, &info) == SU_E_UNSUPPORTED_MIGRATION);
        blob[4] = 1u;
        CHECK(su_audio_checkpoint_decode(blob, size, &info) == SU_OK);
        f.trace[5].sample_time++;
        CHECK(su_audio_offline_begin(&f.manifest, f.trace, f.trace_count, blob, size, &off) == SU_E_SCHEMA_MISMATCH);
        CHECK(off == NULL);
        f.trace[5].sample_time--;
    }
    cleanup(&f);
}

static void rejection_and_order(void) {
    Fixture f;
    SuAudioOffline *off = NULL;
    SuAudioAssetRef missing[1] = {{3u, 9u}};
    SuAudioTraceCommand saved;
    float pcm[512u * 2u];
    uint32_t index;
    fixture(&f, 0u);
    f.manifest.asset_refs = missing;
    f.manifest.asset_ref_count = 1u;
    CHECK(su_audio_offline_begin(&f.manifest, f.trace, f.trace_count, NULL, 0u, &off) == SU_E_STALE_REVISION);
    CHECK(off == NULL);
    f.manifest.asset_refs = f.refs;
    f.manifest.asset_ref_count = 4u;
    saved = f.trace[2];
    f.trace[2].sample_time = 0u;
    f.trace[2].sample_time = f.trace[1].sample_time - 1u;
    CHECK(su_audio_offline_begin(&f.manifest, f.trace, f.trace_count, NULL, 0u, &off) == SU_E_INVALID_ARGUMENT);
    f.trace[2] = saved;
    f.manifest.struct_size = 0u;
    CHECK(su_audio_offline_begin(&f.manifest, f.trace, f.trace_count, NULL, 0u, &off) == SU_E_INVALID_ARGUMENT);
    f.manifest.struct_size = sizeof f.manifest;
    /* Fixed event-order fixture: impulses at quantum edges, same-sample stop before start. */
    f.manifest.emitter_count = 0u;
    f.trace_count = 0u;
    add(&f, SU_AUDIO_TRACE_POST, 0u, 4u, 0u, 0u);
    add(&f, SU_AUDIO_TRACE_POST, 127u, 4u, 0u, 0u);
    add(&f, SU_AUDIO_TRACE_POST, 128u, 4u, 0u, 0u);
    add(&f, SU_AUDIO_TRACE_POST, 300u, 4u, 0u, 0u);
    add(&f, SU_AUDIO_TRACE_POST, 300u, 4u, 0u, 0u);
    add(&f, SU_AUDIO_TRACE_POST, 400u, 4u, 0u, 0u);
    add(&f, SU_AUDIO_TRACE_STOP_INSTANCE, 400u, 0u, 0u, 5u);
    CHECK(su_audio_offline_begin(&f.manifest, f.trace, f.trace_count, NULL, 0u, &off) == SU_OK);
    CHECK(su_audio_offline_render(off, pcm, 512u) == SU_OK);
    for (index = 0; index < 512u; index++) {
        float expected = (index == 0u || index == 127u || index == 128u) ? 0.5f : index == 300u ? 1.0f : 0.0f;
        CHECK(pcm[index * 2u] == expected && pcm[index * 2u + 1u] == expected);
    }
    su_audio_offline_end(off);
    cleanup(&f);
}

static void loudness_and_peak(void) {
    static const double levels[] = {-23.0, -33.0};
    double lufs = 0.0;
    double peak = 0.0;
    uint32_t level;
    uint32_t index;
    /* EBU Tech 3341 cases 1/2: 1 kHz stereo sine at -23/-33 dBFS reads -23/-33 LUFS. */
    for (level = 0; level < 2u; level++) {
        double amplitude = pow(10.0, levels[level] / 20.0);
        for (index = 0; index < 20u * RATE; index++) {
            float s = (float)(amplitude * sin(2.0 * 3.14159265358979323846 * 1000.0 * index / RATE));
            tone_buf[index * 2u] = s;
            tone_buf[index * 2u + 1u] = s;
        }
        CHECK(su_audio_measure_stereo(tone_buf, 20u * RATE, RATE, &lufs, &peak) == SU_OK);
        CHECK(fabs(lufs - levels[level]) < 0.1);
    }
    /* fs/4 sine at 45 degrees: samples peak at 0.707 A, the true peak is A. */
    for (index = 0; index < RATE; index++) {
        float s = (float)(0.5 * sin(3.14159265358979323846 * (0.5 * index + 0.25)));
        tone_buf[index * 2u] = s;
        tone_buf[index * 2u + 1u] = 0.0f;
    }
    CHECK(su_audio_measure_stereo(tone_buf, RATE, RATE, &lufs, &peak) == SU_OK);
    CHECK(fabs(20.0 * log10(peak / 0.5)) < 0.2);
    /* Silence is fully gated. */
    memset(tone_buf, 0, (size_t)RATE * 2u * sizeof tone_buf[0]);
    CHECK(su_audio_measure_stereo(tone_buf, RATE, RATE, &lufs, &peak) == SU_OK);
    CHECK(lufs == -HUGE_VAL && peak == 0.0);
    CHECK(su_audio_measure_stereo(tone_buf, RATE, 44101u, &lufs, &peak) == SU_E_INVALID_ARGUMENT);
}

static void export_and_report(void) {
    Fixture f;
    SuAudioOffline *off = NULL;
    SuAudioOfflineMetrics m;
    static uint8_t wav[44u + 4800u * 8u];
    char report[2048];
    size_t written = 0u;
    float value;
    uint32_t bits;
    fixture(&f, RATE);
    CHECK(su_audio_offline_begin(&f.manifest, f.trace, f.trace_count, NULL, 0u, &off) == SU_OK);
    CHECK(su_audio_offline_render(off, other, LENGTH) == SU_OK);
    CHECK(su_audio_offline_metrics(off, &m) == SU_OK);
    CHECK(m.start_pts_ns == UINT64_C(1000000000) && m.end_pts_ns == UINT64_C(3000000000));
    CHECK(su_audio_offline_report_json(off, report, 16u, &written) == SU_E_RESOURCE_LIMIT);
    CHECK(su_audio_offline_report_json(off, report, sizeof report, &written) == SU_OK);
    CHECK(written == strlen(report));
    CHECK(strstr(report, m.pcm_sha256) != NULL);
    CHECK(strstr(report, "\"numeric_profile\":\"audio_float_local_v1\"") != NULL);
    CHECK(strstr(report, "\"hardware_counters\":{\"available\":false") != NULL);
    CHECK(strstr(report, "\"end_pts_ns\":3000000000") != NULL);
    su_audio_offline_end(off);
    CHECK(su_audio_wav_encode_f32(other, 4800u, RATE, wav, sizeof wav - 1u, &written) == SU_E_RESOURCE_LIMIT);
    CHECK(su_audio_wav_encode_f32(other, 4800u, RATE, wav, sizeof wav, &written) == SU_OK);
    CHECK(written == sizeof wav && memcmp(wav, "RIFF", 4u) == 0 && memcmp(wav + 8, "WAVEfmt ", 8u) == 0);
    CHECK(wav[20] == 3u && wav[22] == 2u && wav[34] == 32u);
    bits = (uint32_t)wav[44 + 100 * 4] | ((uint32_t)wav[45 + 100 * 4] << 8) | ((uint32_t)wav[46 + 100 * 4] << 16)
        | ((uint32_t)wav[47 + 100 * 4] << 24);
    memcpy(&value, &bits, sizeof value);
    CHECK(memcmp(&value, &other[100], sizeof value) == 0);
    cleanup(&f);
}

static void cost(void) {
    Fixture f;
    SuAudioOffline *off = NULL;
    SuAudioOffline *resumed = NULL;
    uint8_t blob[512];
    size_t size = 0u;
    double started;
    double render;
    double restore;
    uint32_t pass;
    fixture(&f, 0u);
    started = seconds();
    for (pass = 0; pass < 10u; pass++) {
        CHECK(su_audio_offline_begin(&f.manifest, f.trace, f.trace_count, NULL, 0u, &off) == SU_OK);
        CHECK(su_audio_offline_render(off, whole, LENGTH) == SU_OK);
        if (pass + 1u < 10u) {
            su_audio_offline_end(off);
        }
    }
    render = (seconds() - started) / 10.0;
    CHECK(su_audio_checkpoint_encode(off, blob, sizeof blob, &size) == SU_OK);
    started = seconds();
    CHECK(su_audio_offline_begin(&f.manifest, f.trace, f.trace_count, blob, size, &resumed) == SU_OK);
    restore = seconds() - started;
    printf("offline cost: %.3f ms per %.1f s rendered+measured (%.1fx realtime); checkpoint %zu bytes, "
        "replay restore at %u frames %.3f ms\n", render * 1000.0, (double)LENGTH / RATE,
        render > 0.0 ? (double)LENGTH / RATE / render : 0.0, size, LENGTH, restore * 1000.0);
    su_audio_offline_end(resumed);
    su_audio_offline_end(off);
    cleanup(&f);
}

int main(void) {
    synthesize();
    repeat_and_chunks();
    checkpoints();
    rejection_and_order();
    loudness_and_peak();
    export_and_report();
    cost();
    printf("audio_offline: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
