/* Vertical-slice acceptance: committed events -> camera-spatialised event
 * runtime (offline, inspected and traced) -> owner-composed voice pools,
 * parameter-driven bus faders, bus tree with sidechain ducking -> measured
 * export. Every sample is synthesized here. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#define BLOCK 128u
#define MAX_VOICES 128u

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
static float ambience[CLIP * 8u];
static SuAudioClip clips[3];

typedef struct Voice {
    uint64_t instance;
    uint64_t start;
    uint32_t node_id;
    uint32_t bus_id;
    uint32_t asset_id;
    float gain;
    float distance;
} Voice;

static Voice voices[MAX_VOICES];
static uint32_t voice_count;
static float event_pcm[LENGTH * 2u];
static float mix_a[LENGTH * 2u];
static float mix_b[LENGTH * 2u];
static uint8_t wav[44u + LENGTH * 8u];
static char text[65536];

static void synthesize(void) {
    uint32_t state = 0x2545f491u;
    uint32_t index;
    for (index = 0; index < CLIP; index++) {
        double env = exp(-(double)index / 700.0);
        state = state * 1664525u + 1013904223u;
        shot[index] = (float)(env * ((double)(state >> 8) / 8388608.0 - 1.0) * 0.5);
        impact[index] = (float)(env * sin(2.0 * 3.14159265358979 * 70.0 * index / RATE) * 0.7);
    }
    for (index = 0; index < CLIP * 8u; index++) {
        ambience[index] = (float)(0.2 * sin(2.0 * 3.14159265358979 * 110.0 * index / RATE));
    }
    clips[0].samples = shot;
    clips[0].frames = CLIP;
    clips[1].samples = impact;
    clips[1].frames = CLIP;
    clips[2].samples = ambience;
    clips[2].frames = CLIP * 8u;
    for (index = 0; index < 3u; index++) {
        clips[index].channels = 1u;
        clips[index].sample_rate = RATE;
    }
}

static SuWorldPos meters(double x, double z) {
    SuWorldPos pos;
    pos.x = (int64_t)(x * 4294967296.0);
    pos.y = 0;
    pos.z = (int64_t)(z * 4294967296.0);
    return pos;
}

static int clean_text(const char *value) {
    return strstr(value, "0x") == NULL && strchr(value, '\\') == NULL && strstr(value, ":/") == NULL
        && strstr(value, "Users") == NULL && strstr(value, "home") == NULL;
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

/* Stage 1: the event runtime decides what plays, where and when. */
static void event_stage(const char *dir) {
    SuAudioAssets *assets = NULL;
    SuAudioGraph *graph = NULL;
    SuAudioGraphLimits limits = {16u, 32u, 8u, 16u, 8192u};
    SuAudioGraphDiagnostic diagnostic;
    SuAudioAssetRef refs[3];
    SuAudioCurvePoint points[2];
    SuAudioCurveDesc curve;
    SuAudioEmitterSnapshot emitters[3];
    SuAudioOfflineManifest manifest;
    SuAudioTraceCommand trace[48];
    SuAudioOffline *off = NULL;
    SuAudioTelemetry *telemetry = NULL;
    SuAudioTelemetryEntry entries[64];
    SuAudioInspectInstance instances[16];
    SuAudioInspectVoice live[64];
    SuAudioInspectEmitter live_emitters[8];
    SuAudioInspectBus buses[8];
    SuAudioInspectSnapshot snap;
    uint32_t trace_count = 0u;
    uint32_t drained = 0u;
    uint32_t index;
    uint32_t done;
    size_t written = 0u;
    memset(refs, 0, sizeof refs);
    memset(points, 0, sizeof points);
    memset(emitters, 0, sizeof emitters);
    memset(&manifest, 0, sizeof manifest);
    CHECK(su_audio_assets_create(4u, 1u << 20, &assets) == SU_OK);
    for (index = 0; index < 3u; index++) {
        CHECK(su_audio_asset_prepare(assets, index + 1u, &clips[index], &refs[index]) == SU_OK);
    }
    CHECK(su_audio_graph_prepare(graph_json, sizeof graph_json - 1u, assets, &limits, &graph, &diagnostic) == SU_OK);
    points[0].distance_m = 1.0f;
    points[0].lowpass_hz = 20000.0f;
    points[0].scale = 1.0f;
    points[1].distance_m = 80.0f;
    points[1].gain_db = -30.0f;
    points[1].lowpass_hz = 1500.0f;
    points[1].scale = 1.0f;
    curve.struct_size = sizeof curve;
    curve.points = points;
    curve.point_count = 2u;
    for (index = 0; index < 3u; index++) {
        emitters[index].pos = meters(-20.0 + 20.0 * index, -5.0 - 10.0 * index);
        emitters[index].curve = 1u;
        emitters[index].scale = 1.0f;
    }
    manifest.struct_size = sizeof manifest;
    manifest.events.struct_size = sizeof manifest.events;
    manifest.events.sample_rate = RATE;
    manifest.events.max_instances = 12u;
    manifest.events.max_leaves_per_instance = 4u;
    manifest.events.max_logical_voices = 24u;
    manifest.events.command_capacity = 4u;
    manifest.events.project_seed = 4242u;
    manifest.events.session_generation = 1u;
    manifest.assets = assets;
    manifest.graph = graph;
    manifest.graph_source = graph_json;
    manifest.graph_source_bytes = sizeof graph_json - 1u;
    manifest.asset_refs = refs;
    manifest.asset_ref_count = 3u;
    manifest.curves = &curve;
    manifest.curve_count = 1u;
    manifest.emitters = emitters;
    manifest.emitter_count = 3u;
    manifest.listener.rot.w = 1.0f;
    memset(trace, 0, sizeof trace);
    trace[trace_count].kind = SU_AUDIO_TRACE_POST;
    trace[trace_count].event_id = 3u;
    trace[trace_count].scope_id = 1u;
    trace[trace_count].commit_seq = trace_count;
    trace_count++;
    for (index = 0; index < 18u; index++) {
        SuAudioTraceCommand *c = &trace[trace_count];
        c->kind = SU_AUDIO_TRACE_POST;
        c->sample_time = 100u + index * 2500u;
        c->event_id = index % 4u == 3u ? 2u : 1u;
        c->emitter_index = 1u + index % 3u;
        c->scope_id = 2u;
        c->commit_seq = trace_count++;
        if (index == 9u) {
            c = &trace[trace_count];
            c->kind = SU_AUDIO_TRACE_LISTENER;
            c->sample_time = 100u + index * 2500u + 20u;
            c->pos = meters(5.0, -8.0);
            c->rot.w = 1.0f;
            c->commit_seq = trace_count++;
        }
    }
    CHECK(su_audio_trace_begin(64u, &telemetry) == SU_OK);
    CHECK(su_audio_offline_begin(&manifest, trace, trace_count, NULL, 0u, &off) == SU_OK);
    if (off == NULL || telemetry == NULL) {
        return;
    }
    CHECK(su_audio_inspect_attach_offline(off, telemetry) == SU_OK);
    memset(&snap, 0, sizeof snap);
    snap.struct_size = sizeof snap;
    snap.instances = instances;
    snap.instance_capacity = 16u;
    snap.voices = live;
    snap.voice_capacity = 64u;
    snap.emitters = live_emitters;
    snap.emitter_capacity = 8u;
    snap.buses = buses;
    snap.bus_capacity = 8u;
    voice_count = 0u;
    for (done = 0u; done < LENGTH; done += BLOCK) {
        uint32_t v;
        CHECK(su_audio_offline_render(off, event_pcm + (size_t)done * 2u, BLOCK) == SU_OK);
        CHECK(su_audio_inspect(su_audio_offline_events(off), su_audio_offline_scene(off), assets, &snap) == SU_OK);
        CHECK(snap.truncated == 0u);
        for (v = 0; v < snap.voice_count; v++) {
            uint32_t known;
            uint32_t i;
            int found = 0;
            for (known = 0; known < voice_count; known++) {
                if (voices[known].instance == live[v].instance && voices[known].node_id == live[v].node_id
                    && voices[known].start == live[v].start_sample) {
                    found = 1;
                    break;
                }
            }
            if (found || voice_count == MAX_VOICES) {
                continue;
            }
            voices[voice_count].instance = live[v].instance;
            voices[voice_count].start = live[v].start_sample;
            voices[voice_count].node_id = live[v].node_id;
            voices[voice_count].bus_id = live[v].bus_id;
            voices[voice_count].asset_id = live[v].asset_id;
            voices[voice_count].gain = live[v].gain;
            voices[voice_count].distance = 1.0f;
            for (i = 0; i < snap.instance_count; i++) {
                uint32_t e;
                for (e = 0; instances[i].handle == live[v].instance && e < snap.emitter_count; e++) {
                    if (live_emitters[e].handle == instances[i].emitter) {
                        voices[voice_count].distance = (float)live_emitters[e].distance_m;
                    }
                }
            }
            voice_count++;
        }
    }
    CHECK(voice_count >= 20u && voice_count < MAX_VOICES);
    CHECK(su_audio_trace_drain(telemetry, entries, 64u, &drained) == SU_OK && drained == trace_count);
    for (index = 0; index < drained; index++) {
        CHECK(entries[index].reason == SU_AUDIO_TRACE_REASON_OK && entries[index].seq == index);
    }
    CHECK(su_audio_trace_csv(entries, drained, text, sizeof text, &written) == SU_OK && clean_text(text));
    write_file(dir, "event_trace.csv", text, written);
    CHECK(su_audio_wav_encode_f32(event_pcm, LENGTH, RATE, wav, sizeof wav, &written) == SU_OK);
    write_file(dir, "event_stage.wav", wav, written);
    CHECK(su_audio_acceptance_manifest_json(off, wav, written, text, sizeof text, &written) == SU_OK);
    CHECK(clean_text(text) && strstr(text, "owner-composed") != NULL);
    write_file(dir, "acceptance.json", text, written);
    su_audio_offline_end(off);
    su_audio_trace_end(telemetry);
    su_audio_graph_release(graph);
    CHECK(su_audio_assets_destroy(assets) == SU_OK);
}

typedef struct Composition {
    double min_ambience_gain_early;
    float final_ambience_fader_db;
    uint64_t stolen;
    uint32_t virtual_peak;
} Composition;

/* Stage 2: voice pools per bus, a parameter on the ambience fader, and a
 * bus tree with shots ducking ambience. block is the external chunk size. */
static Composition compose(float *out, uint32_t block, const char *dir) {
    static const uint32_t bus_ids[3] = {10u, 20u, 30u};
    static const uint32_t physical[3] = {2u, 3u, 2u};
    SuAudioBusDesc bus_desc[4];
    SuAudioDuckRule duck;
    SuAudioBusTreeDesc tree_desc;
    SuAudioBusTree *tree = NULL;
    SuAudioBusDiagnostic bus_diag;
    SuAudioVoicePool *pools[3] = {NULL, NULL, NULL};
    SuAudioParams *params = NULL;
    SuAudioParamsDesc params_desc;
    SuAudioParamDef param;
    SuAudioBindingPoint binding_points[2];
    SuAudioParamBinding binding;
    SuAudioParamScope global_scope = {SU_AUDIO_SCOPE_GLOBAL, 0u};
    SuAudioParamTarget target = {0u, 0u};
    Composition result;
    float scratch[BLOCK * 2u];
    uint32_t index;
    uint32_t done;
    uint32_t next_voice = 0u;
    size_t written = 0u;
    memset(&result, 0, sizeof result);
    result.min_ambience_gain_early = 1.0;
    memset(bus_desc, 0, sizeof bus_desc);
    bus_desc[0].id = 1u;
    bus_desc[0].parent = SU_AUDIO_BUS_ROOT;
    for (index = 0; index < 3u; index++) {
        bus_desc[index + 1u].id = bus_ids[index];
        bus_desc[index + 1u].parent = 1u;
    }
    memset(&duck, 0, sizeof duck);
    duck.source = 10u;
    duck.target = 30u;
    duck.threshold_db = -30.0f;
    duck.ratio = 4.0f;
    duck.attack_ms = 5.0f;
    duck.hold_ms = 50.0f;
    duck.release_ms = 200.0f;
    duck.max_attenuation_db = 9.0f;
    memset(&tree_desc, 0, sizeof tree_desc);
    tree_desc.struct_size = sizeof tree_desc;
    tree_desc.sample_rate = RATE;
    tree_desc.max_block_frames = BLOCK;
    tree_desc.buses = bus_desc;
    tree_desc.bus_count = 4u;
    tree_desc.rules = &duck;
    tree_desc.rule_count = 1u;
    CHECK(su_audio_bus_tree_create(&tree_desc, &tree, &bus_diag) == SU_OK);
    for (index = 0; index < 3u; index++) {
        SuAudioVoicePoolDesc pool_desc;
        memset(&pool_desc, 0, sizeof pool_desc);
        pool_desc.struct_size = sizeof pool_desc;
        pool_desc.sample_rate = RATE;
        pool_desc.max_logical = 32u;
        pool_desc.max_physical = physical[index];
        pool_desc.transition_reserve = 2u;
        CHECK(su_audio_voice_pool_create(&pool_desc, &pools[index]) == SU_OK);
    }
    memset(&param, 0, sizeof param);
    param.id = 1u;
    param.max_value = 1.0f;
    param.smoothing_frames = 480u;
    binding_points[0].value = 0.0f;
    binding_points[0].output = 0.0f;
    binding_points[1].value = 1.0f;
    binding_points[1].output = -12.0f;
    memset(&binding, 0, sizeof binding);
    binding.param_id = 1u;
    binding.control = SU_AUDIO_CONTROL_BUS_GAIN_DB;
    binding.bus_id = 30u;
    binding.points = binding_points;
    binding.point_count = 2u;
    memset(&params_desc, 0, sizeof params_desc);
    params_desc.struct_size = sizeof params_desc;
    params_desc.sample_rate = RATE;
    params_desc.params = &param;
    params_desc.param_count = 1u;
    params_desc.bindings = &binding;
    params_desc.binding_count = 1u;
    params_desc.max_scoped_values = 8u;
    CHECK(su_audio_params_create(&params_desc, &params) == SU_OK);
    CHECK(su_audio_param_set_at(params, global_scope, 1u, 1.0f, 24000u) == SU_OK);
    if (tree == NULL || params == NULL || pools[0] == NULL || pools[1] == NULL || pools[2] == NULL) {
        return result;
    }
    for (done = 0u; done < LENGTH; done += BLOCK) {
        SuAudioParamControls controls;
        float gain = 1.0f;
        while (next_voice < voice_count && voices[next_voice].start < (uint64_t)done + BLOCK) {
            const Voice *v = &voices[next_voice++];
            SuAudioLogicalDesc desc;
            SuAudioLogicalVoice handle = 0u;
            uint32_t pool = v->bus_id == 10u ? 0u : v->bus_id == 20u ? 1u : 2u;
            memset(&desc, 0, sizeof desc);
            desc.struct_size = sizeof desc;
            desc.policy.virtual_mode = SU_AUDIO_VIRTUAL_ADVANCE;
            desc.policy.release_frames = 64u;
            desc.clip = &clips[v->asset_id - 1u];
            desc.gain = v->gain;
            desc.node_id = v->node_id;
            desc.bus_id = v->bus_id;
            desc.instance = v->instance;
            desc.trigger_seq = next_voice;
            CHECK(su_audio_voice_add(pools[pool], &desc, &handle) == SU_OK);
            CHECK(su_audio_voice_set_audibility(pools[pool], handle, 1.0f, v->distance) == SU_OK);
        }
        CHECK(su_audio_param_controls(params, target, done, &controls) == SU_OK);
        if (controls.bus_id == 30u) {
            CHECK(su_audio_bus_set_gain(tree, 30u, controls.bus_gain_db, BLOCK) == SU_OK);
        }
        CHECK(su_audio_params_advance(params, done) == SU_OK);
        for (index = 0; index < 3u; index++) {
            uint32_t part;
            /* External chunking of each pool must not change its quanta. */
            for (part = 0u; part < BLOCK; part += block) {
                uint32_t count = block < BLOCK - part ? block : BLOCK - part;
                CHECK(su_audio_voice_pool_render(pools[index], scratch + (size_t)part * 2u, count) == SU_OK);
            }
            CHECK(su_audio_bus_mix(tree, bus_ids[index], scratch, BLOCK) == SU_OK);
        }
        CHECK(su_audio_bus_process(tree, out + (size_t)done * 2u, BLOCK) == SU_OK);
        CHECK(su_audio_bus_gain_at(tree, 30u, &gain) == SU_OK);
        if (done < 24000u && gain < result.min_ambience_gain_early) {
            result.min_ambience_gain_early = gain;
        }
    }
    {
        SuAudioBusMeter meter;
        SuAudioVoiceStats stats;
        CHECK(su_audio_bus_meter_read(tree, 30u, &meter, 0) == SU_OK);
        result.final_ambience_fader_db = meter.fader_db;
        su_audio_voice_pool_stats(pools[0], &stats);
        result.stolen = stats.stolen;
        result.virtual_peak = stats.virtual_peak;
    }
    CHECK(su_audio_inspect_bus_json(tree, 1, text, sizeof text, &written) == SU_OK && clean_text(text));
    CHECK(strstr(text, "\"bus_id\":30") != NULL);
    write_file(dir, "bus_meters.json", text, written);
    CHECK(su_audio_inspect_voice_pool_json(pools[0], text, sizeof text, &written) == SU_OK && clean_text(text));
    CHECK(strstr(text, "\"physical\":") != NULL);
    write_file(dir, "voice_pool_shots.json", text, written);
    CHECK(su_audio_inspect_params_json(params, text, sizeof text, &written) == SU_OK && clean_text(text));
    CHECK(strstr(text, "\"sets\":1") != NULL);
    write_file(dir, "params.json", text, written);
    su_audio_params_destroy(params);
    for (index = 0; index < 3u; index++) {
        su_audio_voice_pool_destroy(pools[index]);
    }
    su_audio_bus_tree_destroy(tree);
    return result;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : NULL;
    Composition a;
    Composition b;
    double lufs = 0.0;
    double peak = 0.0;
    double energy = 0.0;
    size_t written = 0u;
    uint32_t index;
    synthesize();
    event_stage(dir);
    a = compose(mix_a, BLOCK, dir);
    b = compose(mix_b, 37u, NULL);
    CHECK(memcmp(mix_a, mix_b, sizeof mix_a) == 0);
    CHECK(a.stolen == b.stolen && a.virtual_peak == b.virtual_peak);
    /* Shots exceed two physical voices, so virtualisation is exercised. */
    CHECK(a.virtual_peak > 0u);
    /* Shots duck the ambience bus before the parameter moves its fader. */
    CHECK(a.min_ambience_gain_early < 0.95);
    CHECK(fabs(a.final_ambience_fader_db + 12.0f) < 0.01);
    for (index = 0; index < LENGTH * 2u; index++) {
        energy += (double)mix_a[index] * mix_a[index];
    }
    CHECK(energy > 1.0);
    CHECK(su_audio_measure_stereo(mix_a, LENGTH, RATE, &lufs, &peak) == SU_OK);
    CHECK(lufs > -60.0 && lufs < 0.0 && peak > 0.0);
    CHECK(su_audio_wav_encode_f32(mix_a, LENGTH, RATE, wav, sizeof wav, &written) == SU_OK);
    write_file(dir, "composed.wav", wav, written);
    (void)snprintf(text, sizeof text, "{\"schema\":\"su_audio_composed_v1\",\"frames\":%u,\"integrated_lufs\":%.3f,"
        "\"true_peak_dbtp\":%.3f,\"voices\":%u,\"shot_virtual_peak\":%u,\"shot_stolen\":%llu,"
        "\"min_ambience_gain_early\":%.4f}\n", LENGTH, lufs, 20.0 * log10(peak), voice_count, a.virtual_peak,
        (unsigned long long)a.stolen, a.min_ambience_gain_early);
    write_file(dir, "composed.json", text, strlen(text));
    printf("%s", text);
    printf("audio_acceptance: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
