/* CPU cost of the integrated audio runtime per second of rendered audio (offline output, one thread).
 * Default scene: 512 logical / 64 physical looping spatial voices on 256 moving emitters, 16 buses with 8
 * sidechain ducks, parameters on every voice and one bus fader, three music stems and clustering. Synthetic
 * PCM only. Usage: bench_audio_system [seconds] [json-out] [physical-voices] */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sutekh/audio_system.h"

#define RATE 48000u
#define CLIPS 8u
#define CLIP_FRAMES 24000u
#define BUSES 16u
#define DUCKS 8u
#define EMITTERS 256u
#define LOGICAL 512u
static uint32_t physical_voices = 64u;

static float clip_pcm[CLIPS][CLIP_FRAMES];
static float stem_pcm[3][RATE * 2u];
static float block[1024u * 2u];

static double now_seconds(void) {
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) != TIME_UTC) {
        return 0.0;
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static SuWorldPos meters(double x, double z) {
    SuWorldPos pos;
    pos.x = (int64_t)(x * 4294967296.0);
    pos.y = 0;
    pos.z = (int64_t)(z * 4294967296.0);
    return pos;
}

typedef struct Variant {
    const char *name;
    int buses;
    int params;
    int music;
    int cluster;
} Variant;

typedef struct Result {
    double cpu_ms_per_second;
    double wall_ms_per_second;
    uint64_t runtime_bytes;
    uint32_t physical_peak;
    uint32_t virtual_peak;
    uint64_t stolen;
    uint32_t cluster_outputs;
} Result;

static int run(const Variant *variant, double seconds, Result *result) {
    static char graph_json[16384];
    static const SuAudioBindingPoint gain_points[2] = {{0.0f, -6.0f}, {1.0f, 0.0f}};
    static const SuAudioBindingPoint pitch_points[2] = {{0.0f, 0.9f}, {1.0f, 1.1f}};
    static const SuAudioBindingPoint fader_points[2] = {{0.0f, 0.0f}, {1.0f, -6.0f}};
    static const SuAudioTempoPoint tempo[1] = {{0u, 120000u}};
    SuAudioAssets *assets = NULL;
    SuAudioGraph *graph = NULL;
    SuAudioGraphLimits limits = {64u, 64u, 8u, 16u, 16384u};
    SuAudioGraphDiagnostic diagnostic;
    SuAudioAssetRef refs[CLIPS + 3u];
    SuAudioCurvePoint points[2];
    SuAudioCurveDesc curve;
    SuAudioBusDesc buses[BUSES];
    SuAudioDuckRule ducks[DUCKS];
    SuAudioBusTreeDesc tree;
    SuAudioParamDef defs[3];
    SuAudioParamBinding bindings[3];
    SuAudioParamsDesc params;
    SuAudioMusicDesc music;
    SuAudioMusicSegmentDesc segment;
    SuAudioClusterDesc cluster;
    SuAudioSystemDesc desc;
    SuAudioSystem *system = NULL;
    SuAudioEmitter emitters[EMITTERS];
    SuAudioSystemStats stats;
    SuAudioVoiceStats voices;
    char message[256];
    size_t length = 0u;
    uint64_t frames = (uint64_t)(seconds * RATE);
    uint64_t done = 0u;
    uint32_t index;
    clock_t cpu_start;
    double wall_start;
    memset(result, 0, sizeof *result);
    if (su_audio_assets_create(CLIPS + 4u, 64u << 20, &assets) != SU_OK) {
        return 0;
    }
    for (index = 0; index < CLIPS + 3u; index++) {
        SuAudioClip clip;
        clip.channels = index < CLIPS ? 1u : 2u;
        clip.sample_rate = RATE;
        clip.samples = index < CLIPS ? clip_pcm[index] : stem_pcm[index - CLIPS];
        clip.frames = index < CLIPS ? CLIP_FRAMES : RATE;
        if (su_audio_asset_prepare(assets, index + 1u, &clip, &refs[index]) != SU_OK) {
            return 0;
        }
    }
    length += (size_t)snprintf(graph_json + length, sizeof graph_json - length,
        "{\"schema\":\"su_audio_events_v1\",\"numeric_profile\":\"audio_float_local_v1\","
        "\"seed_policy\":\"event_emitter_sequence_v1\",\"sample_rate\":48000,\"nodes\":[");
    for (index = 0; index < 32u; index++) {
        length += (size_t)snprintf(graph_json + length, sizeof graph_json - length,
            "%s{\"id\":%u,\"type\":\"sound\",\"asset\":{\"id\":%u,\"generation\":%u},\"bus_id\":%u,\"flags\":1,"
            "\"gain\":0.25,\"pitch\":%.2f}", index ? "," : "", index + 1u, refs[index % CLIPS].id,
            refs[index % CLIPS].generation, variant->buses ? 101u + index % (BUSES - 1u) : 0u,
            0.8 + 0.4 * (double)(index % 5u) / 4.0);
    }
    length += (size_t)snprintf(graph_json + length, sizeof graph_json - length, "],\"events\":[");
    for (index = 0; index < 32u; index++) {
        length += (size_t)snprintf(graph_json + length, sizeof graph_json - length, "%s{\"id\":%u,\"root\":%u}",
            index ? "," : "", index + 1u, index + 1u);
    }
    length += (size_t)snprintf(graph_json + length, sizeof graph_json - length, "]}");
    if (su_audio_graph_prepare(graph_json, length, assets, &limits, &graph, &diagnostic) != SU_OK) {
        fprintf(stderr, "graph: %s\n", diagnostic.message);
        return 0;
    }
    memset(points, 0, sizeof points);
    points[0].distance_m = 1.0f;
    points[0].lowpass_hz = 20000.0f;
    points[0].scale = 1.0f;
    points[1].distance_m = 100.0f;
    points[1].gain_db = -24.0f;
    points[1].lowpass_hz = 2000.0f;
    points[1].scale = 1.0f;
    curve.struct_size = sizeof curve;
    curve.points = points;
    curve.point_count = 2u;
    memset(buses, 0, sizeof buses);
    buses[0].id = 100u;
    buses[0].parent = SU_AUDIO_BUS_ROOT;
    for (index = 1u; index < BUSES; index++) {
        buses[index].id = 100u + index;
        buses[index].parent = 100u;
    }
    memset(ducks, 0, sizeof ducks);
    for (index = 0; index < DUCKS; index++) {
        ducks[index].source = 101u + index;
        ducks[index].target = 101u + DUCKS + index % (BUSES - 1u - DUCKS);
        ducks[index].threshold_db = -36.0f;
        ducks[index].ratio = 3.0f;
        ducks[index].attack_ms = 10.0f;
        ducks[index].hold_ms = 50.0f;
        ducks[index].release_ms = 250.0f;
        ducks[index].max_attenuation_db = 9.0f;
        ducks[index].flags = index % 2u ? SU_AUDIO_DUCK_DELAY_QUANTUM : 0u;
    }
    memset(&tree, 0, sizeof tree);
    tree.struct_size = sizeof tree;
    tree.sample_rate = RATE;
    tree.max_block_frames = 128u;
    tree.buses = buses;
    tree.bus_count = BUSES;
    tree.rules = ducks;
    tree.rule_count = DUCKS;
    memset(defs, 0, sizeof defs);
    for (index = 0; index < 3u; index++) {
        defs[index].id = index + 1u;
        defs[index].max_value = 1.0f;
        defs[index].default_value = 0.5f;
        defs[index].smoothing_frames = 4800u;
    }
    memset(bindings, 0, sizeof bindings);
    bindings[0].param_id = 1u;
    bindings[0].control = SU_AUDIO_CONTROL_GAIN_DB;
    bindings[0].points = gain_points;
    bindings[0].point_count = 2u;
    bindings[1].param_id = 2u;
    bindings[1].control = SU_AUDIO_CONTROL_PITCH;
    bindings[1].points = pitch_points;
    bindings[1].point_count = 2u;
    bindings[2].param_id = 3u;
    bindings[2].control = SU_AUDIO_CONTROL_BUS_GAIN_DB;
    bindings[2].bus_id = 115u;
    bindings[2].points = fader_points;
    bindings[2].point_count = 2u;
    memset(&params, 0, sizeof params);
    params.struct_size = sizeof params;
    params.sample_rate = RATE;
    params.params = defs;
    params.param_count = 3u;
    params.bindings = bindings;
    params.binding_count = variant->buses ? 3u : 2u;
    params.max_scoped_values = 64u;
    memset(&music, 0, sizeof music);
    music.struct_size = sizeof music;
    music.sample_rate = RATE;
    music.max_segments = 1u;
    memset(&segment, 0, sizeof segment);
    segment.struct_size = sizeof segment;
    segment.segment_id = 1u;
    segment.stems = &refs[CLIPS];
    segment.stem_count = 3u;
    segment.tempo = tempo;
    segment.tempo_count = 1u;
    segment.beats_per_bar = 4u;
    segment.length_frames = RATE;
    segment.loop = 1u;
    memset(&cluster, 0, sizeof cluster);
    cluster.struct_size = sizeof cluster;
    cluster.width_rad = 0.6f;
    cluster.leave_ratio = 1.25f;
    cluster.hold_frames = 2048u;
    cluster.crossfade_frames = 512u;
    cluster.output_limit = 16u;
    cluster.max_objects = physical_voices + 16u + BUSES;
    memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.output = SU_AUDIO_OUTPUT_OFFLINE;
    desc.assets = assets;
    desc.graph = graph;
    desc.events.struct_size = sizeof desc.events;
    desc.events.sample_rate = RATE;
    desc.events.max_instances = LOGICAL;
    desc.events.max_leaves_per_instance = 1u;
    desc.events.max_logical_voices = LOGICAL;
    desc.events.command_capacity = 64u;
    desc.events.project_seed = 1u;
    desc.events.session_generation = 1u;
    desc.scene.struct_size = sizeof desc.scene;
    desc.scene.max_emitters = EMITTERS;
    desc.scene.max_curves = 1u;
    desc.curves = &curve;
    desc.curve_count = 1u;
    desc.voices.struct_size = sizeof desc.voices;
    desc.voices.sample_rate = RATE;
    desc.voices.max_logical = LOGICAL;
    desc.voices.max_physical = physical_voices;
    desc.voices.hero_reserve = 4u;
    desc.voices.transition_reserve = 16u;
    desc.default_policy.virtual_mode = SU_AUDIO_VIRTUAL_ADVANCE;
    desc.default_policy.release_frames = 256u;
    desc.buses = variant->buses ? &tree : NULL;
    desc.params = variant->params ? &params : NULL;
    desc.music = variant->music ? &music : NULL;
    desc.segments = variant->music ? &segment : NULL;
    desc.segment_count = variant->music ? 1u : 0u;
    desc.music_bus_id = variant->buses ? 114u : 0u;
    desc.cluster = variant->cluster ? &cluster : NULL;
    desc.cluster_inputs = SU_AUDIO_EVENTS_CLUSTER_VOICES | (variant->buses ? SU_AUDIO_EVENTS_CLUSTER_BUSES : 0u);
    desc.limiter_knee = 0.8f;
    if (su_audio_system_create(&desc, &system, message) != SU_OK) {
        fprintf(stderr, "create: %s\n", message);
        return 0;
    }
    for (index = 0; index < EMITTERS; index++) {
        SuAudioEmitterSnapshot emitter;
        double angle = 6.283185307179586 * index / EMITTERS;
        double radius = 3.0 + 90.0 * (double)((index * 37u) % EMITTERS) / EMITTERS;
        memset(&emitter, 0, sizeof emitter);
        emitter.curve = 1u;
        emitter.scale = 1.0f;
        emitter.pos = meters(radius * cos(angle), radius * sin(angle));
        (void)su_audio_system_emitter_create(system, &emitter, &emitters[index]);
    }
    for (index = 0; index < LOGICAL; index++) {
        SuAudioTrigger trigger;
        SuAudioInstance instance = 0u;
        memset(&trigger, 0, sizeof trigger);
        trigger.struct_size = sizeof trigger;
        trigger.event_id = 1u + index % 32u;
        trigger.emitter = emitters[index % EMITTERS];
        trigger.sample_time = index * 7u;
        trigger.commit_seq = index;
        trigger.session_generation = 1u;
        if (su_audio_system_post(system, &trigger, &instance, NULL) != SU_OK) {
            fprintf(stderr, "post %u refused\n", index);
            return 0;
        }
    }
    if (variant->music) {
        SuMusicTransition move;
        memset(&move, 0, sizeof move);
        move.struct_size = sizeof move;
        move.segment_id = 1u;
        move.sync = SU_AUDIO_MUSIC_IMMEDIATE;
        (void)su_audio_system_music_transition(system, &move, 0u, NULL);
    }
    cpu_start = clock();
    wall_start = now_seconds();
    while (done < frames) {
        uint32_t take = frames - done < 1024u ? (uint32_t)(frames - done) : 1024u;
        if (done % 2048u == 0u) {
            /* a game frame: emitters orbit, parameters move */
            uint32_t moved;
            double t = (double)done / RATE;
            for (moved = 0; moved < EMITTERS; moved++) {
                double angle = 6.283185307179586 * moved / EMITTERS + 0.2 * t;
                double radius = 3.0 + 90.0 * (double)((moved * 37u) % EMITTERS) / EMITTERS;
                SuVec3f velocity = {0.0f, 0.0f, 0.0f};
                (void)su_audio_system_emitter_update(system, emitters[moved], meters(radius * cos(angle),
                    radius * sin(angle)), velocity);
            }
            if (variant->params && done % 24000u == 0u) {
                SuAudioParamScope global = {SU_AUDIO_SCOPE_GLOBAL, 0u};
                float value = (float)(0.5 + 0.5 * sin(t));
                (void)su_audio_system_param_set(system, global, 1u, value, su_audio_system_clock(system));
                (void)su_audio_system_param_set(system, global, 2u, 1.0f - value, su_audio_system_clock(system));
                (void)su_audio_system_param_set(system, global, 3u, value, su_audio_system_clock(system));
            }
        }
        if (su_audio_system_render(system, block, take) != SU_OK) {
            return 0;
        }
        done += take;
    }
    result->cpu_ms_per_second = 1000.0 * (double)(clock() - cpu_start) / CLOCKS_PER_SEC / seconds;
    result->wall_ms_per_second = 1000.0 * (now_seconds() - wall_start) / seconds;
    su_audio_system_stats(system, &stats);
    su_audio_voice_pool_stats(su_audio_system_voices(system), &voices);
    result->runtime_bytes = stats.runtime_bytes;
    result->physical_peak = voices.physical_peak;
    result->virtual_peak = voices.virtual_peak;
    result->stolen = voices.stolen;
    if (variant->cluster) {
        SuAudioClusterStats cluster_stats;
        su_audio_cluster_stats(su_audio_system_cluster(system), &cluster_stats);
        result->cluster_outputs = cluster_stats.outputs;
    }
    su_audio_system_destroy(system);
    su_audio_graph_release(graph);
    (void)su_audio_assets_destroy(assets);
    return 1;
}

int main(int argc, char **argv) {
    static const Variant variants[] = {
        {"full", 1, 1, 1, 1},
        {"no_clustering", 1, 1, 1, 0},
        {"voices_only", 0, 0, 0, 0},
    };
    double seconds = argc > 1 ? atof(argv[1]) : 10.0;
    FILE *json = argc > 2 ? fopen(argv[2], "wb") : NULL;
    uint32_t index;
    uint32_t state = 0x9e3779b9u;
    if (!(seconds > 0.0) || seconds > 600.0) {
        seconds = 10.0;
    }
    if (argc > 3) {
        physical_voices = (uint32_t)atoi(argv[3]);
        if (physical_voices < 8u || physical_voices > LOGICAL) {
            physical_voices = 64u;
        }
    }
    for (index = 0; index < CLIPS * CLIP_FRAMES; index++) {
        uint32_t clip = index / CLIP_FRAMES;
        uint32_t frame = index % CLIP_FRAMES;
        state = state * 1664525u + 1013904223u;
        clip_pcm[clip][frame] = (float)(0.4 * sin(6.283185307179586 * (110.0 + 55.0 * clip) * frame / RATE)
            + 0.1 * ((double)(state >> 8) / 8388608.0 - 1.0));
    }
    for (index = 0; index < 3u * RATE * 2u; index++) {
        uint32_t stem = index / (RATE * 2u);
        uint32_t frame = (index % (RATE * 2u)) / 2u;
        stem_pcm[stem][index % (RATE * 2u)] = (float)(0.2 * sin(6.283185307179586 * (220.0 * (stem + 1u)) * frame / RATE));
    }
    printf("audio system cost: %u logical / %u physical voices, %u emitters, %u buses, %u ducks, 3 stems, %.1f s\n",
        LOGICAL, physical_voices, EMITTERS, BUSES, DUCKS, seconds);
    if (json != NULL) {
        fprintf(json, "{\"schema\":\"su_audio_system_bench_v1\",\"seconds\":%.3f,\"logical\":%u,\"physical\":%u,"
            "\"emitters\":%u,\"buses\":%u,\"ducks\":%u,\"stems\":3,\"variants\":[", seconds, LOGICAL, physical_voices,
            EMITTERS, BUSES, DUCKS);
    }
    for (index = 0; index < sizeof variants / sizeof variants[0]; index++) {
        Result result;
        if (!run(&variants[index], seconds, &result)) {
            fprintf(stderr, "variant %s failed\n", variants[index].name);
            return 1;
        }
        printf("%-14s cpu %7.2f ms/s  wall %7.2f ms/s  physical peak %u  virtual peak %u  stolen %llu  "
            "clusters %u  bytes %llu\n", variants[index].name, result.cpu_ms_per_second, result.wall_ms_per_second,
            result.physical_peak, result.virtual_peak, (unsigned long long)result.stolen, result.cluster_outputs,
            (unsigned long long)result.runtime_bytes);
        if (json != NULL) {
            fprintf(json, "%s{\"name\":\"%s\",\"cpu_ms_per_second\":%.3f,\"wall_ms_per_second\":%.3f,"
                "\"physical_peak\":%u,\"virtual_peak\":%u,\"stolen\":%llu,\"runtime_bytes\":%llu}", index ? "," : "",
                variants[index].name, result.cpu_ms_per_second, result.wall_ms_per_second, result.physical_peak,
                result.virtual_peak, (unsigned long long)result.stolen, (unsigned long long)result.runtime_bytes);
        }
    }
    if (json != NULL) {
        fprintf(json, "]}\n");
        fclose(json);
    }
    return 0;
}
