/* Vertical-slice acceptance: committed events -> camera-spatialised event
 * runtime (offline, inspected and traced) -> owner-composed voice pools,
 * parameter-driven bus faders, bus tree with sidechain ducking -> measured
 * export. Every sample is synthesized here. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/audio_system.h"
#include "ir/su_sha256.h"

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

#define BLOCKS (LENGTH / BLOCK)
#define MAX_EMITTERS 4u

typedef struct EmitterFrame {
    uint64_t handle[MAX_EMITTERS];
    SuVec3f direction[MAX_EMITTERS];
    float distance[MAX_EMITTERS];
    uint32_t count;
} EmitterFrame;

static EmitterFrame emitter_frames[BLOCKS];
static const float silence[BLOCK] = {0.0f};
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
        {
            EmitterFrame *frame = &emitter_frames[done / BLOCK];
            uint32_t e;
            frame->count = 0u;
            for (e = 0; e < snap.emitter_count && e < MAX_EMITTERS; e++) {
                frame->handle[frame->count] = live_emitters[e].handle;
                frame->direction[frame->count] = live_emitters[e].direction_listener;
                frame->distance[frame->count] = (float)live_emitters[e].distance_m;
                frame->count++;
            }
        }
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
    CHECK(clean_text(text) && strstr(text, "integrated:") != NULL);
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
    uint32_t max_cluster_outputs;
    uint64_t cluster_joins;
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
    SuAudioCluster *cluster = NULL;
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
    {
        SuAudioClusterDesc cluster_desc;
        memset(&cluster_desc, 0, sizeof cluster_desc);
        cluster_desc.struct_size = sizeof cluster_desc;
        cluster_desc.width_rad = 1.2f;
        cluster_desc.leave_ratio = 1.25f;
        cluster_desc.hold_frames = 1024u;
        cluster_desc.crossfade_frames = 256u;
        cluster_desc.output_limit = 2u;
        cluster_desc.max_objects = MAX_EMITTERS;
        CHECK(su_audio_cluster_create(&cluster_desc, &cluster) == SU_OK);
    }
    if (tree == NULL || params == NULL || cluster == NULL || pools[0] == NULL || pools[1] == NULL || pools[2] == NULL) {
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
        {
            /* Emitters of the live instances are clustered for spatial output metadata. */
            const EmitterFrame *frame = &emitter_frames[done / BLOCK];
            SuAudioClusterObject objects[MAX_EMITTERS];
            SuAudioClusterStats cluster_stats;
            uint32_t e;
            memset(objects, 0, sizeof objects);
            for (e = 0; e < frame->count; e++) {
                objects[e].id = frame->handle[e];
                objects[e].direction = frame->direction[e];
                objects[e].distance_m = frame->distance[e];
                objects[e].rms = 0.1f;
                objects[e].channels = 1u;
                objects[e].pcm = silence;
            }
            CHECK(su_audio_cluster_update(cluster, objects, frame->count) == SU_OK);
            CHECK(su_audio_cluster_render(cluster, objects, frame->count, BLOCK, scratch) == SU_OK);
            su_audio_cluster_stats(cluster, &cluster_stats);
            CHECK(cluster_stats.outputs <= 2u || cluster_stats.infeasible != 0u);
            if (cluster_stats.outputs > result.max_cluster_outputs) {
                result.max_cluster_outputs = cluster_stats.outputs;
            }
            result.cluster_joins = cluster_stats.joins;
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
    CHECK(su_audio_inspect_cluster_json(cluster, text, sizeof text, &written) == SU_OK && clean_text(text));
    CHECK(strstr(text, "\"schema\":\"su_audio_cluster_v1\"") != NULL);
    write_file(dir, "clusters.json", text, written);
    CHECK(su_audio_inspect_params_json(params, text, sizeof text, &written) == SU_OK && clean_text(text));
    CHECK(strstr(text, "\"sets\":1") != NULL);
    write_file(dir, "params.json", text, written);
    su_audio_cluster_destroy(cluster);
    su_audio_params_destroy(params);
    for (index = 0; index < 3u; index++) {
        su_audio_voice_pool_destroy(pools[index]);
    }
    su_audio_bus_tree_destroy(tree);
    return result;
}

/* Stage 3: the same slice through the integrated runtime owned by one SuAudioSystem. The real-time path
 * (per game frame: commands, render-ahead ring update, device-sized reads) and the offline path (journal
 * replay into a fresh offline system) must produce the same PCM. */
static float live_pcm[LENGTH * 2u];
static float offline_pcm[LENGTH * 2u];
static char report[1u << 18];

typedef struct Integrated {
    double min_ambience_gain_early;
    float final_ambience_fader_db;
    uint64_t scope_limited;
    uint32_t cluster_outputs;
    uint64_t cluster_joins;
} Integrated;

static Integrated integrated_stage(const char *dir) {
    static const SuAudioBindingPoint fader_points[2] = {{0.0f, 0.0f}, {1.0f, -12.0f}};
    SuAudioAssets *assets = NULL;
    SuAudioGraph *graph = NULL;
    SuAudioGraphLimits limits = {16u, 32u, 8u, 16u, 8192u};
    SuAudioGraphDiagnostic diagnostic;
    SuAudioAssetRef refs[3];
    SuAudioCurvePoint points[2];
    SuAudioCurveDesc curve;
    SuAudioBusDesc bus_desc[4];
    SuAudioDuckRule duck;
    SuAudioBusTreeDesc tree_desc;
    SuAudioParamDef param;
    SuAudioParamBinding binding;
    SuAudioParamsDesc params_desc;
    SuAudioClusterDesc cluster_desc;
    SuAudioVoicePolicyRule rules[3];
    SuAudioSystemDesc desc;
    SuAudioSystem *live = NULL;
    SuAudioSystem *offline = NULL;
    SuAudioEmitter emitters[3];
    SuAudioInstance handles[32];
    SuAudioTelemetryEntry entries[64];
    Integrated result;
    char message[256];
    uint32_t index;
    uint32_t posted = 0u;
    uint32_t next_command = 0u;
    uint64_t consumed = 0u;
    uint32_t chunk = 441u;
    size_t written = 0u;
    memset(&result, 0, sizeof result);
    result.min_ambience_gain_early = 1.0;
    CHECK(su_audio_assets_create(4u, 1u << 20, &assets) == SU_OK);
    for (index = 0; index < 3u; index++) {
        CHECK(su_audio_asset_prepare(assets, index + 1u, &clips[index], &refs[index]) == SU_OK);
    }
    CHECK(su_audio_graph_prepare(graph_json, sizeof graph_json - 1u, assets, &limits, &graph, &diagnostic) == SU_OK);
    memset(points, 0, sizeof points);
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
    memset(bus_desc, 0, sizeof bus_desc);
    bus_desc[0].id = 1u;
    bus_desc[0].parent = SU_AUDIO_BUS_ROOT;
    bus_desc[1].id = 10u;
    bus_desc[2].id = 20u;
    bus_desc[3].id = 30u;
    for (index = 1u; index < 4u; index++) {
        bus_desc[index].parent = 1u;
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
    memset(&param, 0, sizeof param);
    param.id = 1u;
    param.max_value = 1.0f;
    param.smoothing_frames = 480u;
    memset(&binding, 0, sizeof binding);
    binding.param_id = 1u;
    binding.control = SU_AUDIO_CONTROL_BUS_GAIN_DB;
    binding.bus_id = 30u;
    binding.points = fader_points;
    binding.point_count = 2u;
    memset(&params_desc, 0, sizeof params_desc);
    params_desc.struct_size = sizeof params_desc;
    params_desc.sample_rate = RATE;
    params_desc.params = &param;
    params_desc.param_count = 1u;
    params_desc.bindings = &binding;
    params_desc.binding_count = 1u;
    params_desc.max_scoped_values = 8u;
    memset(&cluster_desc, 0, sizeof cluster_desc);
    cluster_desc.struct_size = sizeof cluster_desc;
    cluster_desc.width_rad = 1.2f;
    cluster_desc.leave_ratio = 1.25f;
    cluster_desc.hold_frames = 1024u;
    cluster_desc.crossfade_frames = 256u;
    cluster_desc.output_limit = 2u;
    cluster_desc.max_objects = 32u;
    memset(rules, 0, sizeof rules);
    for (index = 0; index < 3u; index++) {
        rules[index].bus_id = index == 0u ? 10u : index == 1u ? 20u : 30u;
        rules[index].policy.physical_limit = index == 1u ? 3u : 2u;
        rules[index].policy.limit_scope = SU_AUDIO_LIMIT_BUS;
        rules[index].policy.virtual_mode = SU_AUDIO_VIRTUAL_ADVANCE;
        rules[index].policy.release_frames = 64u;
    }
    memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.output = SU_AUDIO_OUTPUT_RING;
    desc.ring_frames = 2048u;
    desc.assets = assets;
    desc.graph = graph;
    desc.events.struct_size = sizeof desc.events;
    desc.events.sample_rate = RATE;
    desc.events.max_instances = 12u;
    desc.events.max_leaves_per_instance = 4u;
    desc.events.max_logical_voices = 24u;
    desc.events.command_capacity = 4u;
    desc.events.project_seed = 4242u;
    desc.events.session_generation = 1u;
    desc.scene.struct_size = sizeof desc.scene;
    desc.scene.max_emitters = 4u;
    desc.scene.max_curves = 1u;
    desc.curves = &curve;
    desc.curve_count = 1u;
    desc.voices.struct_size = sizeof desc.voices;
    desc.voices.sample_rate = RATE;
    desc.voices.max_logical = 32u;
    desc.voices.max_physical = 7u;
    desc.voices.transition_reserve = 6u;
    desc.policies = rules;
    desc.policy_count = 3u;
    desc.default_policy.release_frames = 64u;
    desc.buses = &tree_desc;
    desc.params = &params_desc;
    desc.cluster = &cluster_desc;
    desc.cluster_inputs = SU_AUDIO_EVENTS_CLUSTER_VOICES | SU_AUDIO_EVENTS_CLUSTER_BUSES;
    desc.journal_capacity = 256u;
    desc.telemetry_capacity = 64u;
    CHECK(su_audio_system_create(&desc, &live, message) == SU_OK);
    if (live == NULL) {
        return result;
    }
    {
        SuAudioEmitterSnapshot emitter;
        SuAudioCameraListener listener;
        SuAudioParamScope global = {SU_AUDIO_SCOPE_GLOBAL, 0u};
        memset(&emitter, 0, sizeof emitter);
        emitter.curve = 1u;
        emitter.scale = 1.0f;
        for (index = 0; index < 3u; index++) {
            emitter.pos = meters(-20.0 + 20.0 * index, -5.0 - 10.0 * index);
            CHECK(su_audio_system_emitter_create(live, &emitter, &emitters[index]) == SU_OK);
        }
        memset(&listener, 0, sizeof listener);
        listener.rot.w = 1.0f;
        CHECK(su_audio_system_listener_set(live, &listener) == SU_OK);
        CHECK(su_audio_system_param_set(live, global, 1u, 1.0f, 24000u) == SU_OK);
    }
    /* The A10 trace of stage 1, issued by a game loop: every command is submitted once the render-ahead clock
     * is within 2048 frames of its time. */
    while (consumed < LENGTH) {
        uint64_t clock = su_audio_system_clock(live);
        float gain = 1.0f;
        while (next_command < 18u + 2u) {
            /* command k: 0 = ambience, 1..18 = shots/sequences, 19 = listener move at shot 9 */
            uint64_t at = next_command == 0u ? 0u : next_command <= 18u ? 100u + (next_command - 1u) * 2500u
                : 100u + 9u * 2500u + 20u;
            SuAudioTrigger trigger;
            if (at >= clock + 2048u) {
                break;
            }
            if (at < clock) {
                at = clock;
            }
            memset(&trigger, 0, sizeof trigger);
            trigger.struct_size = sizeof trigger;
            trigger.session_generation = 1u;
            trigger.sample_time = at;
            if (next_command == 0u) {
                trigger.event_id = 3u;
                trigger.scope_id = 1u;
            } else if (next_command <= 18u) {
                uint32_t shot_index = next_command - 1u;
                trigger.event_id = shot_index % 4u == 3u ? 2u : 1u;
                trigger.emitter = emitters[shot_index % 3u];
                trigger.scope_id = 2u;
            } else {
                SuAudioCameraListener listener;
                memset(&listener, 0, sizeof listener);
                listener.pos = meters(5.0, -8.0);
                listener.rot.w = 1.0f;
                CHECK(su_audio_system_listener_set(live, &listener) == SU_OK);
                next_command++;
                continue;
            }
            trigger.commit_seq = posted;
            CHECK(su_audio_system_post(live, &trigger, &handles[posted], NULL) == SU_OK);
            posted++;
            next_command++;
        }
        CHECK(su_audio_system_update(live) == SU_OK);
        CHECK(su_audio_bus_gain_at(su_audio_system_buses(live), 30u, &gain) == SU_OK);
        if (su_audio_system_clock(live) < 24000u && gain < result.min_ambience_gain_early) {
            result.min_ambience_gain_early = gain;
        }
        {
            SuAudioClusterStats running;
            su_audio_cluster_stats(su_audio_system_cluster(live), &running);
            CHECK(running.outputs <= 2u || running.infeasible != 0u);
            if (running.outputs > result.cluster_outputs) {
                result.cluster_outputs = running.outputs;
            }
        }
        {
            uint32_t take = (uint32_t)(LENGTH - consumed < chunk ? LENGTH - consumed : chunk);
            CHECK(su_audio_system_ring_read(live, live_pcm + consumed * 2u, take) == take);
            consumed += take;
            chunk = chunk == 441u ? 480u : chunk == 480u ? 97u : 441u;
        }
    }
    {
        const SuAudioSystemCommand *journal = NULL;
        uint32_t count = 0u;
        SuAudioSystemStats stats;
        SuAudioVoiceStats voice_stats;
        SuAudioClusterStats cluster;
        SuAudioBusMeter meter;
        uint32_t drained = 0u;
        su_audio_system_stats(live, &stats);
        CHECK(stats.underrun_frames == 0u);
        CHECK(su_audio_system_journal(live, &journal, &count) == SU_OK && count == posted + 6u);
        desc.output = SU_AUDIO_OUTPUT_OFFLINE;
        CHECK(su_audio_system_create(&desc, &offline, message) == SU_OK);
        CHECK(su_audio_system_replay(offline, journal, count, LENGTH, offline_pcm) == SU_OK);
        CHECK(memcmp(live_pcm, offline_pcm, sizeof live_pcm) == 0);
        su_audio_voice_pool_stats(su_audio_system_voices(live), &voice_stats);
        result.scope_limited = voice_stats.reasons[SU_AUDIO_REASON_SCOPE_LIMIT];
        su_audio_cluster_stats(su_audio_system_cluster(live), &cluster);
        result.cluster_joins = cluster.joins;
        CHECK(su_audio_bus_meter_read(su_audio_system_buses(offline), 30u, &meter, 0) == SU_OK);
        result.final_ambience_fader_db = meter.fader_db;
        CHECK(su_audio_trace_drain(su_audio_system_telemetry(live), entries, 64u, &drained) == SU_OK);
        CHECK(drained == posted + 2u);
        CHECK(su_audio_trace_csv(entries, drained, text, sizeof text, &written) == SU_OK && clean_text(text));
        write_file(dir, "integrated_trace.csv", text, written);
    }
    CHECK(su_audio_system_control(live, "audio.inspect", NULL, 0u, report, sizeof report, &written) == SU_OK);
    CHECK(clean_text(report) && strstr(report, "su_audio_system_inspect_v1") != NULL);
    write_file(dir, "integrated_inspect.json", report, written);
    {
        static const char params[] = "{\"frames\":48000}";
        SuSha256 sha;
        uint8_t digest[32];
        char hex[65];
        su_sha256_init(&sha);
        su_sha256_update(&sha, live_pcm, sizeof live_pcm);
        su_sha256_final(&sha, digest);
        su_sha256_hex(digest, hex);
        CHECK(su_audio_system_control(live, "audio.render.offline", params, sizeof params - 1u, report, sizeof report,
            &written) == SU_OK);
        CHECK(strstr(report, hex) != NULL);
        write_file(dir, "integrated_offline.json", report, written);
    }
    CHECK(su_audio_wav_encode_f32(live_pcm, LENGTH, RATE, wav, sizeof wav, &written) == SU_OK);
    write_file(dir, "integrated.wav", wav, written);
    su_audio_system_destroy(offline);
    su_audio_system_destroy(live);
    su_audio_graph_release(graph);
    CHECK(su_audio_assets_destroy(assets) == SU_OK);
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
    CHECK(a.max_cluster_outputs == b.max_cluster_outputs && a.cluster_joins == b.cluster_joins);
    /* Three emitters are folded into at most two cluster outputs. */
    CHECK(a.max_cluster_outputs >= 1u && a.max_cluster_outputs <= 2u && a.cluster_joins > 0u);
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
        "\"min_ambience_gain_early\":%.4f,\"max_cluster_outputs\":%u}\n", LENGTH, lufs, 20.0 * log10(peak), voice_count, a.virtual_peak,
        (unsigned long long)a.stolen, a.min_ambience_gain_early, a.max_cluster_outputs);
    write_file(dir, "composed.json", text, strlen(text));
    printf("%s", text);
    {
        Integrated integrated = integrated_stage(dir);
        double ilufs = 0.0;
        double ipeak = 0.0;
        /* The integrated runtime reproduces the composed slice's behaviour: per-bus voice limits virtualise
         * shots, shots duck the ambience bus before the parameter moves its fader to -12 dB, and the three
         * emitters fold into at most two cluster outputs. */
        CHECK(integrated.scope_limited > 0u);
        CHECK(integrated.min_ambience_gain_early < 0.95);
        CHECK(fabs(integrated.final_ambience_fader_db + 12.0f) < 0.01);
        CHECK(integrated.cluster_outputs >= 1u && integrated.cluster_outputs <= 2u && integrated.cluster_joins > 0u);
        CHECK(su_audio_measure_stereo(live_pcm, LENGTH, RATE, &ilufs, &ipeak) == SU_OK);
        CHECK(ilufs > -60.0 && ilufs < 0.0 && ipeak > 0.0 && ipeak <= 1.5);
        (void)snprintf(text, sizeof text, "{\"schema\":\"su_audio_integrated_v1\",\"frames\":%u,"
            "\"integrated_lufs\":%.3f,\"true_peak_dbtp\":%.3f,\"scope_limited\":%llu,\"min_ambience_gain_early\":%.4f,"
            "\"cluster_outputs\":%u,\"realtime_equals_offline\":%s}\n", LENGTH, ilufs, 20.0 * log10(ipeak),
            (unsigned long long)integrated.scope_limited, integrated.min_ambience_gain_early,
            integrated.cluster_outputs, memcmp(live_pcm, offline_pcm, sizeof live_pcm) == 0 ? "true" : "false");
        write_file(dir, "integrated.json", text, strlen(text));
        printf("%s", text);
    }
    printf("audio_acceptance: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
