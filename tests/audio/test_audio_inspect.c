#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sutekh/audio_inspect.h"

static unsigned checks;
static unsigned failures;

#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        if (failures++ < 40u) \
        fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
    } \
} while (0)

#define RATE 48000u
#define LENGTH 48000u
#define CLIP 6000u

/* Synthetic skirmish: shots on bus 10, impacts on bus 20, ambience on bus 30. */
static const char graph_json[] =
    "{\"schema\":\"su_audio_events_v1\",\"numeric_profile\":\"audio_float_local_v1\","
    "\"seed_policy\":\"event_emitter_sequence_v1\",\"sample_rate\":48000,\"nodes\":["
    "{\"id\":1,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":1},\"bus_id\":10},"
    "{\"id\":2,\"type\":\"sound\",\"asset\":{\"id\":2,\"generation\":2},\"bus_id\":20},"
    "{\"id\":3,\"type\":\"sound\",\"asset\":{\"id\":3,\"generation\":3},\"bus_id\":30,\"gain\":0.4},"
    "{\"id\":4,\"type\":\"random\",\"children\":[{\"node\":1,\"weight\":3},{\"node\":2,\"weight\":1}]},"
    "{\"id\":5,\"type\":\"sequence\",\"children\":[{\"node\":1,\"offset_frames\":0},{\"node\":2,\"offset_frames\":900}]}],"
    "\"events\":[{\"id\":1,\"root\":4},{\"id\":2,\"root\":5},{\"id\":3,\"root\":3}]}";

static float shot[CLIP];
static float impact[CLIP];
static float ambience[CLIP * 4u];

typedef struct Slice {
    SuAudioAssets *assets;
    SuAudioGraph *graph;
    SuAudioAssetRef refs[3];
    SuAudioCurvePoint points[2];
    SuAudioCurveDesc curve;
    SuAudioEmitterSnapshot emitters[3];
    SuAudioOfflineManifest manifest;
    SuAudioTraceCommand trace[64];
    uint32_t trace_count;
} Slice;

static void synthesize(void) {
    uint32_t state = 0x9e3779b9u;
    uint32_t index;
    for (index = 0; index < CLIP; index++) {
        double env = exp(-(double)index / 700.0);
        state = state * 1664525u + 1013904223u;
        shot[index] = (float)(env * ((double)(state >> 8) / 8388608.0 - 1.0) * 0.5);
        impact[index] = (float)(env * sin(2.0 * 3.14159265358979 * 70.0 * index / RATE) * 0.7);
    }
    for (index = 0; index < CLIP * 4u; index++) {
        ambience[index] = (float)(0.2 * sin(2.0 * 3.14159265358979 * 110.0 * index / RATE));
    }
}

static SuWorldPos meters(double x, double z) {
    SuWorldPos pos;
    pos.x = (int64_t)(x * 4294967296.0);
    pos.y = 0;
    pos.z = (int64_t)(z * 4294967296.0);
    return pos;
}

static void command(Slice *s, uint32_t kind, uint64_t at, uint32_t event_id, uint32_t emitter, uint32_t scope) {
    SuAudioTraceCommand *c = &s->trace[s->trace_count];
    memset(c, 0, sizeof *c);
    c->kind = kind;
    c->sample_time = at;
    c->event_id = event_id;
    c->emitter_index = emitter;
    c->commit_seq = s->trace_count;
    c->scope_id = scope;
    c->rot.w = 1.0f;
    c->pos = meters(3.0, -2.0 - (double)s->trace_count);
    s->trace_count++;
}

static void slice_create(Slice *s) {
    SuAudioGraphLimits limits = {16u, 32u, 8u, 16u, 8192u};
    SuAudioGraphDiagnostic diagnostic;
    SuAudioClip clip = {shot, CLIP, 1u, RATE};
    uint32_t index;
    memset(s, 0, sizeof *s);
    CHECK(su_audio_assets_create(4u, 1u << 20, &s->assets) == SU_OK);
    CHECK(su_audio_asset_prepare(s->assets, 1u, &clip, &s->refs[0]) == SU_OK);
    clip.samples = impact;
    CHECK(su_audio_asset_prepare(s->assets, 2u, &clip, &s->refs[1]) == SU_OK);
    clip.samples = ambience;
    clip.frames = CLIP * 4u;
    CHECK(su_audio_asset_prepare(s->assets, 3u, &clip, &s->refs[2]) == SU_OK);
    CHECK(su_audio_graph_prepare(graph_json, sizeof graph_json - 1u, s->assets, &limits, &s->graph,
        &diagnostic) == SU_OK);
    s->points[0].distance_m = 1.0f;
    s->points[0].lowpass_hz = 20000.0f;
    s->points[0].scale = 1.0f;
    s->points[1].distance_m = 80.0f;
    s->points[1].gain_db = -30.0f;
    s->points[1].lowpass_hz = 1500.0f;
    s->points[1].scale = 1.0f;
    s->curve.struct_size = sizeof s->curve;
    s->curve.points = s->points;
    s->curve.point_count = 2u;
    for (index = 0; index < 3u; index++) {
        s->emitters[index].pos = meters(-20.0 + 20.0 * index, -5.0 - 10.0 * index);
        s->emitters[index].curve = 1u;
        s->emitters[index].scale = 1.0f;
    }
    s->manifest.struct_size = sizeof s->manifest;
    s->manifest.events.struct_size = sizeof s->manifest.events;
    s->manifest.events.sample_rate = RATE;
    s->manifest.events.max_instances = 12u;
    s->manifest.events.max_leaves_per_instance = 4u;
    s->manifest.events.max_logical_voices = 24u;
    s->manifest.events.command_capacity = 4u;
    s->manifest.events.project_seed = 4242u;
    s->manifest.events.session_generation = 1u;
    s->manifest.assets = s->assets;
    s->manifest.graph = s->graph;
    s->manifest.graph_source = graph_json;
    s->manifest.graph_source_bytes = sizeof graph_json - 1u;
    s->manifest.asset_refs = s->refs;
    s->manifest.asset_ref_count = 3u;
    s->manifest.curves = &s->curve;
    s->manifest.curve_count = 1u;
    s->manifest.emitters = s->emitters;
    s->manifest.emitter_count = 3u;
    s->manifest.listener.rot.w = 1.0f;
    command(s, SU_AUDIO_TRACE_POST, 0u, 3u, 0u, 1u);
    for (index = 0; index < 18u; index++) {
        uint64_t at = 100u + index * 2500u;
        command(s, SU_AUDIO_TRACE_POST, at, index % 4u == 3u ? 2u : 1u, 1u + index % 3u, 2u);
        if (index % 6u == 2u) {
            command(s, SU_AUDIO_TRACE_EMITTER_MOVE, at + 10u, 0u, 1u + index % 3u, 0u);
        }
        if (index == 9u) {
            command(s, SU_AUDIO_TRACE_LISTENER, at + 20u, 0u, 0u, 0u);
        }
    }
    command(s, SU_AUDIO_TRACE_STOP_SCOPE, 46000u, 0u, 0u, 1u);
}

static void slice_destroy(Slice *s) {
    su_audio_graph_release(s->graph);
    CHECK(su_audio_assets_destroy(s->assets) == SU_OK);
}

static int clean_text(const char *text) {
    return strstr(text, "0x") == NULL && strchr(text, '\\') == NULL && strstr(text, ":/") == NULL
        && strstr(text, "Users") == NULL && strstr(text, "home") == NULL;
}

static void write_file(const char *dir, const char *name, const void *data, size_t bytes) {
    char path[1024];
    FILE *file;
    if (dir == NULL) {
        return;
    }
    (void)snprintf(path, sizeof path, "%s/%s", dir, name);
    file = fopen(path, "wb");
    CHECK(file != NULL);
    if (file != NULL) {
        CHECK(fwrite(data, 1u, bytes, file) == bytes);
        CHECK(fclose(file) == 0);
    }
}

static float pcm[LENGTH * 2u];
static uint8_t wav[44u + LENGTH * 8u];
static char text[65536];
static char text2[65536];
static char csv_a[16384];
static char csv_b[16384];
static SuAudioTelemetryEntry drained[256];
static SuAudioTelemetryEntry drained2[256];

typedef struct SliceResult {
    uint32_t entries;
    size_t csv_bytes;
} SliceResult;

/* admitted event -> camera-spatialized -> bus-tagged -> offline export */
static SliceResult vertical_slice(const char *dir, SuAudioTelemetryEntry *entries, char *csv) {
    Slice s;
    SuAudioOffline *off = NULL;
    SuAudioTelemetry *telemetry = NULL;
    SuAudioInspectInstance instances[16];
    SuAudioInspectVoice voices[64];
    SuAudioInspectEmitter emitters[8];
    SuAudioInspectBus buses[8];
    SuAudioInspectSnapshot snap;
    SuAudioTelemetryStats tstats;
    SliceResult result = {0u, 0u};
    size_t written = 0u;
    uint32_t index;
    int spatialized = 0;
    slice_create(&s);
    CHECK(su_audio_trace_begin(256u, &telemetry) == SU_OK);
    CHECK(su_audio_offline_begin(&s.manifest, s.trace, s.trace_count, NULL, 0u, &off) == SU_OK);
    if (off == NULL || telemetry == NULL) {
        return result;
    }
    CHECK(su_audio_inspect_attach_offline(off, telemetry) == SU_OK);
    CHECK(su_audio_offline_render(off, pcm, 7700u) == SU_OK);
    memset(&snap, 0, sizeof snap);
    snap.struct_size = sizeof snap;
    snap.instances = instances;
    snap.instance_capacity = 16u;
    snap.voices = voices;
    snap.voice_capacity = 64u;
    snap.emitters = emitters;
    snap.emitter_capacity = 8u;
    snap.buses = buses;
    snap.bus_capacity = 8u;
    CHECK(su_audio_inspect(su_audio_offline_events(off), su_audio_offline_scene(off), s.assets, &snap) == SU_OK);
    CHECK(snap.truncated == 0u);
    CHECK(snap.instance_count >= 3u && snap.voice_count >= 3u && snap.emitter_count >= 2u);
    CHECK(snap.budget.max_instances == 12u && snap.stats.render_clock == 7808u);
    CHECK(snap.assets.resident_bytes > 0u && snap.cursor.next_seq > 0u);
    CHECK(snap.bus_count >= 2u && buses[0].bus_id < buses[snap.bus_count - 1u].bus_id);
    for (index = 0; index < snap.voice_count; index++) {
        CHECK(voices[index].bus_id == 10u || voices[index].bus_id == 20u || voices[index].bus_id == 30u);
        if (voices[index].bus_id != 30u && voices[index].gain < 0.99f && voices[index].lowpass_hz < 20000.0f) {
            spatialized = 1;
        }
    }
    CHECK(spatialized);
    for (index = 0; index < snap.emitter_count; index++) {
        CHECK(emitters[index].distance_m > 1.0 && emitters[index].curve == 1u);
    }
    CHECK(su_audio_inspect_json(&snap, NULL, 0u, &written) == SU_E_RESOURCE_LIMIT && written > 0u);
    CHECK(su_audio_inspect_json(&snap, text, sizeof text, &written) == SU_OK && written == strlen(text));
    CHECK(strstr(text, "\"bus_id\":10") != NULL && clean_text(text));
    if (dir != NULL) {
        struct timespec t0;
        struct timespec t1;
        uint32_t pass;
        (void)timespec_get(&t0, TIME_UTC);
        for (pass = 0; pass < 1000u; pass++) {
            CHECK(su_audio_inspect(su_audio_offline_events(off), su_audio_offline_scene(off), s.assets, &snap)
                == SU_OK);
        }
        (void)timespec_get(&t1, TIME_UTC);
        printf("inspect cost: %.3f us per snapshot (%u instances, %u voices, %u emitters, %u buses); json %zu bytes\n",
            ((double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec)) / 1000.0 / 1000.0,
            snap.instance_count, snap.voice_count, snap.emitter_count, snap.bus_count, written);
    }
    write_file(dir, "inspect.json", text, written);
    CHECK(su_audio_offline_render(off, pcm + 7700u * 2u, LENGTH - 7700u) == SU_OK);
    CHECK(su_audio_trace_drain(telemetry, entries, 256u, &result.entries) == SU_OK);
    CHECK(result.entries == s.trace_count);
    for (index = 0; index < result.entries; index++) {
        CHECK(entries[index].seq == index && entries[index].dropped_before == 0u);
        CHECK(entries[index].reason == SU_AUDIO_REASON_OK);
        CHECK(entries[index].sample_time >= entries[index].render_clock);
        CHECK(entries[index].sample_time - entries[index].render_clock < SU_AUDIO_EVENT_QUANTUM);
        CHECK(entries[index].kind != SU_AUDIO_TRACE_POST || entries[index].instance != 0u);
    }
    su_audio_trace_stats(telemetry, &tstats);
    CHECK(tstats.recorded == s.trace_count && tstats.dropped == 0u && tstats.queued == 0u);
    CHECK(su_audio_trace_csv(entries, result.entries, csv, sizeof csv_a, &result.csv_bytes) == SU_OK);
    write_file(dir, "trace.csv", csv, result.csv_bytes);
    CHECK(su_audio_trace_json(entries, result.entries, text2, sizeof text2, &written) == SU_OK);
    CHECK(clean_text(text2) && strstr(text2, "\"reason\":\"ok\"") != NULL);
    write_file(dir, "trace.json", text2, written);
    CHECK(su_audio_offline_report_json(off, text2, sizeof text2, &written) == SU_OK);
    CHECK(clean_text(text2));
    write_file(dir, "report.json", text2, written);
    CHECK(su_audio_wav_encode_f32(pcm, LENGTH, RATE, wav, sizeof wav, &written) == SU_OK);
    write_file(dir, "slice.wav", wav, written);
    CHECK(su_audio_acceptance_manifest_json(off, wav, sizeof wav, text2, sizeof text2, &written) == SU_OK);
    CHECK(clean_text(text2) && strstr(text2, "\"bus_tree_ducking\"") != NULL);
    write_file(dir, "acceptance.json", text2, written);
    wav[100] ^= 1u;
    CHECK(su_audio_acceptance_manifest_json(off, wav, sizeof wav, text2, sizeof text2, &written)
        == SU_E_SCHEMA_MISMATCH);
    wav[100] ^= 1u;
    CHECK(su_audio_acceptance_manifest_json(off, wav, sizeof wav - 8u, text2, sizeof text2, &written)
        == SU_E_SCHEMA_MISMATCH);
    {
        double energy = 0.0;
        for (index = 0; index < LENGTH * 2u; index++) {
            energy += (double)pcm[index] * pcm[index];
        }
        CHECK(energy > 1.0);
    }
    su_audio_offline_end(off);
    su_audio_trace_end(telemetry);
    slice_destroy(&s);
    return result;
}

static void deterministic_trace(void) {
    SliceResult a = vertical_slice(NULL, drained, csv_a);
    SliceResult b = vertical_slice(NULL, drained2, csv_b);
    CHECK(a.entries == b.entries && a.entries > 0u);
    CHECK(memcmp(drained, drained2, sizeof drained[0] * a.entries) == 0);
    CHECK(a.csv_bytes == b.csv_bytes && memcmp(csv_a, csv_b, a.csv_bytes) == 0);
}

static void backpressure(void) {
    SuAudioTelemetry *telemetry = NULL;
    SuAudioTelemetryEntry entry;
    SuAudioTelemetryEntry out[8];
    SuAudioTelemetryStats stats;
    uint32_t count = 0u;
    uint32_t index;
    CHECK(su_audio_trace_begin(0u, &telemetry) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_trace_begin(4u, &telemetry) == SU_OK);
    for (index = 0; index < 10u; index++) {
        memset(&entry, 0, sizeof entry);
        entry.kind = SU_AUDIO_TRACE_POST;
        entry.sample_time = index;
        CHECK(su_audio_trace_record(telemetry, &entry) == (index < 4u ? SU_OK : SU_E_RESOURCE_LIMIT));
    }
    CHECK(su_audio_trace_drain(telemetry, out, 2u, &count) == SU_OK && count == 2u);
    CHECK(out[0].seq == 0u && out[1].seq == 1u && out[1].sample_time == 1u);
    memset(&entry, 0, sizeof entry);
    entry.sample_time = 99u;
    entry.status = SU_E_RESOURCE_LIMIT;
    CHECK(su_audio_trace_record(telemetry, &entry) == SU_OK);
    CHECK(su_audio_trace_drain(telemetry, out, 8u, &count) == SU_OK && count == 3u);
    CHECK(out[0].seq == 2u && out[1].seq == 3u && out[2].seq == 4u);
    CHECK(out[2].dropped_before == 6u && out[2].reason == SU_AUDIO_REASON_POOL_FULL);
    su_audio_trace_stats(telemetry, &stats);
    CHECK(stats.recorded == 5u && stats.dropped == 6u && stats.drained == 5u && stats.queued == 0u);
    CHECK(stats.reasons[SU_AUDIO_REASON_POOL_FULL] == 1u);
    su_audio_trace_end(telemetry);
}

static void saturation(void) {
    Slice s;
    SuAudioScene *scene = NULL;
    SuAudioSceneDesc scene_desc = {sizeof scene_desc, 2u, 1u};
    SuAudioEvents *events = NULL;
    SuAudioEventsDesc desc;
    SuAudioTelemetry *telemetry = NULL;
    SuAudioTrigger trigger;
    SuAudioGraphDiagnostic diagnostic;
    SuAudioInstance instance;
    SuAudioInspectInstance instances[1];
    SuAudioInspectVoice voices[1];
    SuAudioInspectBus buses[1];
    SuAudioInspectSnapshot snap;
    SuAudioTelemetryEntry out[16];
    SuAudioTelemetryEntry entry;
    uint32_t count = 0u;
    size_t written = 0u;
    float buffer[256u * 2u];
    slice_create(&s);
    CHECK(su_audio_scene_create(&scene_desc, &scene) == SU_OK);
    desc = s.manifest.events;
    desc.max_instances = 2u;
    desc.command_capacity = 1u;
    CHECK(su_audio_events_create(&desc, s.assets, scene, s.graph, &events) == SU_OK);
    CHECK(su_audio_trace_begin(16u, &telemetry) == SU_OK);
    memset(&trigger, 0, sizeof trigger);
    trigger.struct_size = sizeof trigger;
    trigger.session_generation = 1u;
    trigger.event_id = 99u;
    trigger.sample_time = 10u;
    trigger.commit_seq = 1u;
    CHECK(su_audio_inspect_post(events, telemetry, &trigger, &instance, &diagnostic) != SU_OK);
    trigger.event_id = 2u;
    CHECK(su_audio_inspect_post(events, telemetry, &trigger, &instance, &diagnostic) == SU_OK);
    trigger.commit_seq = 2u;
    trigger.event_id = 3u;
    CHECK(su_audio_inspect_post(events, telemetry, &trigger, &instance, &diagnostic) == SU_OK);
    trigger.commit_seq = 3u;
    CHECK(su_audio_inspect_post(events, telemetry, &trigger, &instance, &diagnostic) == SU_E_RESOURCE_LIMIT);
    trigger.commit_seq = 1u;
    CHECK(su_audio_inspect_post(events, telemetry, &trigger, &instance, &diagnostic) == SU_E_STALE_REVISION);
    CHECK(su_audio_scope_stop(events, 0u, 5000u, 0u) == SU_OK);
    memset(&entry, 0, sizeof entry);
    entry.kind = SU_AUDIO_TRACE_STOP_SCOPE;
    entry.sample_time = 5001u;
    entry.status = su_audio_scope_stop(events, 0u, 5001u, 0u);
    CHECK(entry.status == SU_E_RESOURCE_LIMIT);
    CHECK(su_audio_trace_record(telemetry, &entry) == SU_OK);
    CHECK(su_audio_events_render(events, buffer, 256u) == SU_OK);
    trigger.commit_seq = 5u;
    trigger.event_id = 1u;
    trigger.sample_time = 3u;
    CHECK(su_audio_inspect_post(events, telemetry, &trigger, &instance, &diagnostic) == SU_E_INVALID_ARGUMENT);
    /* Snapshot under saturation with deliberately short arrays stays bounded. */
    memset(&snap, 0, sizeof snap);
    snap.struct_size = sizeof snap;
    snap.instances = instances;
    snap.instance_capacity = 1u;
    snap.voices = voices;
    snap.voice_capacity = 1u;
    snap.buses = buses;
    snap.bus_capacity = 1u;
    CHECK(su_audio_inspect(events, scene, s.assets, &snap) == SU_OK);
    CHECK(snap.instance_count == 1u && snap.voice_count == 1u && snap.bus_count == 1u);
    CHECK((snap.truncated & (SU_AUDIO_INSPECT_TRUNC_INSTANCES | SU_AUDIO_INSPECT_TRUNC_VOICES
        | SU_AUDIO_INSPECT_TRUNC_BUSES)) == (SU_AUDIO_INSPECT_TRUNC_INSTANCES | SU_AUDIO_INSPECT_TRUNC_VOICES
        | SU_AUDIO_INSPECT_TRUNC_BUSES));
    CHECK(snap.stats.refused >= 2u && snap.stats.duplicates == 1u && snap.stats.late_commands == 1u);
    CHECK(su_audio_inspect_json(&snap, text, sizeof text, &written) == SU_OK && clean_text(text));
    CHECK(su_audio_trace_drain(telemetry, out, 16u, &count) == SU_OK && count == 7u);
    CHECK(out[0].reason == SU_AUDIO_REASON_GRAPH && out[0].id == 99u);
    CHECK(out[1].reason == SU_AUDIO_REASON_OK && out[2].reason == SU_AUDIO_REASON_OK);
    CHECK(out[3].reason == SU_AUDIO_REASON_POOL_FULL);
    CHECK(out[4].reason == SU_AUDIO_REASON_DUPLICATE);
    CHECK(out[5].reason == SU_AUDIO_REASON_POOL_FULL && out[5].kind == SU_AUDIO_TRACE_STOP_SCOPE);
    CHECK(out[6].reason == SU_AUDIO_REASON_LATE && out[6].render_clock == 256u);
    CHECK(su_audio_trace_csv(out, count, text, sizeof text, &written) == SU_OK);
    CHECK(strstr(text, ",pool_full\n") != NULL && strstr(text, ",late\n") != NULL);
    su_audio_trace_end(telemetry);
    su_audio_events_destroy(events);
    su_audio_scene_destroy(scene);
    slice_destroy(&s);
}

int main(int argc, char **argv) {
    synthesize();
    backpressure();
    saturation();
    deterministic_trace();
    (void)vertical_slice(argc > 1 ? argv[1] : NULL, drained, csv_a);
    printf("audio_inspect: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
