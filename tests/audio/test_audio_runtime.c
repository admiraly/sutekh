/* Integrated event runtime: committed events drive the A05 voice pool with pitch and per-voice spatial
 * filtering. Every sample is synthesized here. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/audio_event.h"
#include "sutekh/audio_parameter.h"
#include "sutekh/audio_cluster.h"

static unsigned checks;
static unsigned failures;
static FILE *evidence;

static void record_bytes(const void *data, size_t size) {
    if (evidence != NULL && fwrite(data, 1u, size, evidence) != size) {
        failures++;
    }
}

static uint32_t recorded_render(SuAudioEvents *events, float *out, uint32_t frames) {
    uint32_t status = su_audio_events_render(events, out, frames);
    if (status == SU_OK) {
        record_bytes(out, (size_t)frames * 2u * sizeof *out);
    }
    return status;
}

static uint32_t recorded_save(SuAudioEvents *events, void *out, size_t capacity, size_t *written) {
    uint32_t status = su_audio_events_checkpoint_save(events, out, capacity, written);
    if (status == SU_OK && out != NULL) {
        record_bytes(out, *written);
    }
    return status;
}

#define su_audio_events_render recorded_render
#define su_audio_events_checkpoint_save recorded_save

#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        if (failures++ < 40u) \
            fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
    } \
} while (0)

#define RATE 48000u
#define FRAMES 24000u
#define TONE 4800u
#define LOOP 2400u

static const char graph_json[] =
    "{\"schema\":\"su_audio_events_v1\",\"numeric_profile\":\"audio_float_local_v1\","
    "\"seed_policy\":\"event_emitter_sequence_v1\",\"sample_rate\":48000,\"nodes\":["
    "{\"id\":1,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":1},\"gain\":0.5},"
    "{\"id\":2,\"type\":\"sound\",\"asset\":{\"id\":2,\"generation\":2},\"flags\":1},"
    "{\"id\":3,\"type\":\"sound\",\"asset\":{\"id\":3,\"generation\":3},\"pitch\":0.5},"
    "{\"id\":4,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":1},\"pitch\":1.5},"
    "{\"id\":5,\"type\":\"sound\",\"asset\":{\"id\":4,\"generation\":4}},"
    "{\"id\":6,\"type\":\"sequence\",\"children\":[{\"node\":1,\"offset_frames\":0},{\"node\":4,\"offset_frames\":333}]}],"
    "\"events\":[{\"id\":1,\"root\":1},{\"id\":2,\"root\":2},{\"id\":3,\"root\":3},{\"id\":4,\"root\":6},"
    "{\"id\":5,\"root\":5}]}";

static float tone[TONE];
static float loop_tone[LOOP];
static float ramp[TONE];
static float noise[TONE * 4u];
static float out_a[FRAMES * 2u];
static float out_b[FRAMES * 2u];

static void synthesize(void) {
    uint32_t index;
    uint32_t state = 0x1234567u;
    for (index = 0; index < TONE; index++) {
        tone[index] = (float)(0.8 * sin(2.0 * 3.14159265358979 * 440.0 * index / RATE));
        ramp[index] = (float)index / (float)TONE;
    }
    for (index = 0; index < LOOP; index++) {
        loop_tone[index] = (float)(0.5 * sin(2.0 * 3.14159265358979 * 300.0 * index / RATE));
    }
    for (index = 0; index < TONE * 4u; index++) {
        state = state * 1664525u + 1013904223u;
        noise[index] = (float)((double)(state >> 8) / 8388608.0 - 1.0) * 0.25f;
    }
}

static SuWorldPos meters(double x, double z) {
    SuWorldPos pos;
    pos.x = (int64_t)(x * 4294967296.0);
    pos.y = 0;
    pos.z = (int64_t)(z * 4294967296.0);
    return pos;
}

typedef struct Rig {
    SuAudioAssets *assets;
    SuAudioGraph *graph;
    SuAudioScene *scene;
    SuAudioEvents *events;
    SuAudioVoicePool *pool;
    SuAudioEmitter near_emitter;
    SuAudioEmitter far_emitter;
    uint64_t seq;
} Rig;

typedef struct RigOptions {
    int integrated;
    uint32_t physical;
    uint32_t release;
    uint32_t virtual_mode;
    float lowpass_far;
    uint64_t start;
    uint32_t emitter_capacity;
    float doppler;
} RigOptions;

static RigOptions default_options(int integrated) {
    RigOptions options;
    memset(&options, 0, sizeof options);
    options.integrated = integrated;
    options.physical = 16u;
    options.release = 64u;
    options.virtual_mode = SU_AUDIO_VIRTUAL_ADVANCE;
    return options;
}

static void rig_create(Rig *rig, const RigOptions *options) {
    SuAudioGraphLimits limits = {16u, 32u, 8u, 16u, 8192u};
    SuAudioGraphDiagnostic diagnostic;
    SuAudioSceneDesc scene_desc = {sizeof scene_desc, 8u, 4u};
    SuAudioEventsDesc desc;
    SuAudioClip clip;
    SuAudioAssetRef ref;
    SuAudioCurvePoint points[2];
    SuAudioCurveDesc curve;
    SuAudioCurveId curve_id = 0u;
    SuAudioEmitterSnapshot emitter;
    SuAudioCameraListener listener;
    if (options->emitter_capacity != 0u) {
        scene_desc.max_emitters = options->emitter_capacity;
    }
    memset(rig, 0, sizeof *rig);
    CHECK(su_audio_assets_create(8u, 1u << 20, &rig->assets) == SU_OK);
    clip.channels = 1u;
    clip.sample_rate = RATE;
    clip.samples = tone;
    clip.frames = TONE;
    CHECK(su_audio_asset_prepare(rig->assets, 1u, &clip, &ref) == SU_OK);
    clip.samples = loop_tone;
    clip.frames = LOOP;
    CHECK(su_audio_asset_prepare(rig->assets, 2u, &clip, &ref) == SU_OK);
    clip.samples = ramp;
    clip.frames = TONE;
    CHECK(su_audio_asset_prepare(rig->assets, 3u, &clip, &ref) == SU_OK);
    clip.samples = noise;
    clip.frames = TONE * 4u;
    CHECK(su_audio_asset_prepare(rig->assets, 4u, &clip, &ref) == SU_OK);
    CHECK(su_audio_graph_prepare(graph_json, sizeof graph_json - 1u, rig->assets, &limits, &rig->graph,
        &diagnostic) == SU_OK);
    if (rig->graph == NULL) {
        fprintf(stderr, "graph: %s (%s)\n", diagnostic.message, diagnostic.bound);
    }
    CHECK(su_audio_scene_create(&scene_desc, &rig->scene) == SU_OK);
    memset(points, 0, sizeof points);
    points[0].distance_m = 1.0f;
    points[0].scale = 1.0f;
    points[1].distance_m = 50.0f;
    points[1].gain_db = -12.0f;
    points[1].scale = 1.0f;
    if (options->lowpass_far > 0.0f) {
        points[0].lowpass_hz = 24000.0f;
        points[1].lowpass_hz = options->lowpass_far;
        points[1].gain_db = 0.0f;
    }
    curve.struct_size = sizeof curve;
    curve.points = points;
    curve.point_count = 2u;
    CHECK(su_audio_scene_curve_add(rig->scene, &curve, &curve_id) == SU_OK);
    memset(&emitter, 0, sizeof emitter);
    emitter.curve = curve_id;
    emitter.scale = 1.0f;
    emitter.pos = meters(2.0, -2.0);
    CHECK(su_audio_emitter_create(rig->scene, &emitter, &rig->near_emitter) == SU_OK);
    emitter.pos = meters(-30.0, -40.0);
    CHECK(su_audio_emitter_create(rig->scene, &emitter, &rig->far_emitter) == SU_OK);
    memset(&listener, 0, sizeof listener);
    listener.rot.w = 1.0f;
    CHECK(su_audio_scene_listener_set(rig->scene, &listener) == SU_OK);
    memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.sample_rate = RATE;
    desc.max_instances = 16u;
    desc.max_leaves_per_instance = 4u;
    desc.max_logical_voices = 64u;
    desc.command_capacity = 16u;
    desc.project_seed = 77u;
    desc.session_generation = 1u;
    desc.start_sample = options->start;
    CHECK(su_audio_events_create(&desc, rig->assets, rig->scene, rig->graph, &rig->events) == SU_OK);
    if (options->integrated) {
        SuAudioVoicePoolDesc pool_desc;
        SuAudioEventsHook hook;
        memset(&pool_desc, 0, sizeof pool_desc);
        pool_desc.struct_size = sizeof pool_desc;
        pool_desc.sample_rate = RATE;
        pool_desc.max_logical = 64u;
        pool_desc.max_physical = options->physical;
        pool_desc.transition_reserve = 4u;
        CHECK(su_audio_voice_pool_create(&pool_desc, &rig->pool) == SU_OK);
        memset(&hook, 0, sizeof hook);
        hook.struct_size = sizeof hook;
        hook.version = SU_AUDIO_EVENTS_HOOK_VERSION;
        hook.voices = rig->pool;
        hook.doppler = options->doppler;
        hook.default_policy.virtual_mode = options->virtual_mode;
        hook.default_policy.release_frames = options->release;
        CHECK(su_audio_events_attach(rig->events, &hook) == SU_OK);
    }
}

static void rig_destroy(Rig *rig) {
    su_audio_events_destroy(rig->events);
    su_audio_voice_pool_destroy(rig->pool);
    su_audio_graph_release(rig->graph);
    su_audio_scene_destroy(rig->scene);
    CHECK(su_audio_assets_destroy(rig->assets) == SU_OK);
}

static SuAudioInstance post(Rig *rig, uint32_t event_id, uint64_t at, SuAudioEmitter emitter) {
    SuAudioTrigger trigger;
    SuAudioInstance instance = 0u;
    memset(&trigger, 0, sizeof trigger);
    trigger.struct_size = sizeof trigger;
    trigger.event_id = event_id;
    trigger.emitter = emitter;
    trigger.sample_time = at;
    trigger.commit_seq = rig->seq++;
    trigger.session_generation = 1u;
    CHECK(su_audio_event_post(rig->events, &trigger, &instance, NULL) == SU_OK);
    return instance;
}

static void render_chunks(Rig *rig, float *out, uint32_t frames, uint32_t chunk) {
    uint32_t done = 0u;
    while (done < frames) {
        uint32_t take = frames - done < chunk ? frames - done : chunk;
        CHECK(su_audio_events_render(rig->events, out + (size_t)done * 2u, take) == SU_OK);
        done += take;
    }
}

static float max_step(const float *pcm, uint32_t frames) {
    float worst = 0.0f;
    uint32_t index;
    for (index = 1u; index < frames; index++) {
        float left = fabsf(pcm[index * 2u] - pcm[index * 2u - 2u]);
        float right = fabsf(pcm[index * 2u + 1u] - pcm[index * 2u - 1u]);
        worst = left > worst ? left : worst;
        worst = right > worst ? right : worst;
    }
    return worst;
}

/* The integrated runtime renders exactly what the legacy mixer path renders for unpitched, pitched, looping,
 * delayed and statically spatialised voices: same law, same interpolation, same block-independent timing. */
static void test_legacy_equivalence(void) {
    Rig legacy;
    Rig integrated;
    RigOptions options = default_options(0);
    rig_create(&legacy, &options);
    options.integrated = 1;
    rig_create(&integrated, &options);
    {
        Rig *rigs[2];
        uint32_t r;
        rigs[0] = &legacy;
        rigs[1] = &integrated;
        for (r = 0; r < 2u; r++) {
            (void)post(rigs[r], 1u, 300u, 0u);
            (void)post(rigs[r], 3u, 1001u, 0u);
            (void)post(rigs[r], 4u, 2050u, 0u);
            (void)post(rigs[r], 2u, 4000u, rigs[r]->near_emitter);
            (void)post(rigs[r], 1u, 7777u, rigs[r]->far_emitter);
        }
    }
    render_chunks(&legacy, out_a, FRAMES, 1000u);
    render_chunks(&integrated, out_b, FRAMES, 37u);
    CHECK(memcmp(out_a, out_b, sizeof out_a) == 0);
    /* sample-exact delayed start */
    CHECK(out_b[299u * 2u] == 0.0f && out_b[300u * 2u] == 0.0f && out_b[301u * 2u] == tone[1] * 0.5f);
    rig_destroy(&legacy);
    rig_destroy(&integrated);
}

/* Pitch 0.5 on a ramp clip reads half-frame positions through linear interpolation. */
static void test_pitch(void) {
    Rig rig;
    RigOptions options = default_options(1);
    uint32_t index;
    int exact = 1;
    rig_create(&rig, &options);
    (void)post(&rig, 3u, 128u, 0u);
    render_chunks(&rig, out_a, 2048u, 128u);
    for (index = 0; index < 1000u; index++) {
        float expected = (ramp[index / 2u] + (index % 2u ? (ramp[index / 2u + 1u] - ramp[index / 2u]) * 0.5f : 0.0f));
        if (out_a[(128u + index) * 2u] != expected) {
            exact = 0;
        }
    }
    CHECK(exact);
    rig_destroy(&rig);
}

/* The per-voice curve low-pass removes high-frequency energy of a far voice. */
static void test_lowpass(void) {
    Rig rig;
    RigOptions options = default_options(1);
    double near_hf = 0.0;
    double far_hf = 0.0;
    double near_e = 0.0;
    double far_e = 0.0;
    uint32_t index;
    options.lowpass_far = 800.0f;
    rig_create(&rig, &options);
    (void)post(&rig, 5u, 0u, rig.near_emitter);
    (void)post(&rig, 5u, 19300u, rig.far_emitter);
    render_chunks(&rig, out_a, FRAMES, 512u);
    for (index = 2000u; index < 10000u; index++) {
        double d = (double)out_a[index * 2u] - out_a[index * 2u - 2u];
        near_hf += d * d;
        near_e += (double)out_a[index * 2u] * out_a[index * 2u];
    }
    for (index = 19500u; index < 23900u; index++) {
        double d = (double)out_a[index * 2u] - out_a[index * 2u - 2u];
        far_hf += d * d;
        far_e += (double)out_a[index * 2u] * out_a[index * 2u];
    }
    CHECK(near_e > 0.0 && far_e > 0.0);
    /* white noise through a one-pole 800 Hz low-pass keeps about an eighth of its first-difference energy */
    CHECK(far_hf / far_e < 0.2 * (near_hf / near_e));
    rig_destroy(&rig);
}

/* One physical voice: a near loop steals the slot of a far loop, then stops, and the far loop resumes. The
 * transitions fade, so no sample step exceeds the tones' own slope plus one envelope increment. */
static void test_steal_resume(void) {
    Rig rig;
    RigOptions options = default_options(1);
    SuAudioInstance near_instance;
    SuAudioVoiceStats stats;
    float step;
    options.physical = 1u;
    options.release = 96u;
    rig_create(&rig, &options);
    (void)post(&rig, 2u, 0u, rig.far_emitter);
    near_instance = post(&rig, 2u, 6000u, rig.near_emitter);
    CHECK(su_audio_instance_stop(rig.events, near_instance, 12000u, 300u) == SU_OK);
    render_chunks(&rig, out_a, FRAMES, 333u);
    su_audio_voice_pool_stats(rig.pool, &stats);
    CHECK(stats.stolen >= 1u && stats.woken >= 1u && stats.physical_peak == 1u);
    step = max_step(out_a, FRAMES);
    /* 300 Hz at amplitude 0.5 moves at most 0.0197 per sample; a hard cut would step by up to 0.5 */
    CHECK(step < 0.045f);
    /* the far loop is audible again after the near one released */
    {
        double energy = 0.0;
        uint32_t index;
        for (index = 14000u; index < 16000u; index++) {
            energy += (double)out_a[index * 2u] * out_a[index * 2u];
        }
        CHECK(energy > 0.01);
    }
    rig_destroy(&rig);
}

/* Exact stop timing, cancellation of a stop before the start, and retirement. */
static void test_stop_timing(void) {
    Rig rig;
    RigOptions options = default_options(1);
    SuAudioInstance first;
    SuAudioInstance second;
    SuAudioEventStats stats;
    uint32_t index;
    int silent = 1;
    int unchanged = 1;
    rig_create(&rig, &options);
    first = post(&rig, 2u, 1000u, 0u);
    second = post(&rig, 2u, 5000u, 0u);
    CHECK(su_audio_instance_stop(rig.events, first, 2500u, 100u) == SU_OK);
    CHECK(su_audio_instance_stop(rig.events, second, 4000u, 100u) == SU_OK);
    render_chunks(&rig, out_a, 8192u, 128u);
    for (index = 0; index < 1000u; index++) {
        silent &= out_a[index * 2u] == 0.0f;
    }
    for (index = 1000u; index < 2500u; index++) {
        unchanged &= out_a[index * 2u] == loop_tone[(index - 1000u) % LOOP];
    }
    CHECK(silent && unchanged);
    CHECK(out_a[2500u * 2u] != 0.0f && fabsf(out_a[2599u * 2u]) < 0.01f);
    silent = 1;
    for (index = 2600u; index < 8192u; index++) {
        silent &= out_a[index * 2u] == 0.0f;
    }
    CHECK(silent);
    su_audio_events_update(rig.events);
    su_audio_events_stats(rig.events, &stats);
    CHECK(stats.instances == 0u && stats.logical_voices == 0u && !su_audio_instance_active(rig.events, first));
    rig_destroy(&rig);
}

static void test_attach_validation(void) {
    Rig rig;
    RigOptions options = default_options(0);
    SuAudioEventsHook hook;
    SuAudioVoicePool *pool = NULL;
    SuAudioVoicePoolDesc pool_desc;
    rig_create(&rig, &options);
    memset(&pool_desc, 0, sizeof pool_desc);
    pool_desc.struct_size = sizeof pool_desc;
    pool_desc.max_logical = 8u;
    pool_desc.max_physical = 4u;
    CHECK(su_audio_voice_pool_create(&pool_desc, &pool) == SU_OK);
    memset(&hook, 0, sizeof hook);
    hook.struct_size = sizeof hook;
    hook.version = SU_AUDIO_EVENTS_HOOK_VERSION;
    hook.voices = pool;
    /* the pool must hold every admitted leaf */
    CHECK(su_audio_events_attach(rig.events, &hook) == SU_E_INVALID_ARGUMENT);
    hook.version = 99u;
    CHECK(su_audio_events_attach(rig.events, &hook) == SU_E_INVALID_ARGUMENT);
    su_audio_voice_pool_destroy(pool);
    pool_desc.max_logical = 64u;
    CHECK(su_audio_voice_pool_create(&pool_desc, &pool) == SU_OK);
    hook.voices = pool;
    hook.version = SU_AUDIO_EVENTS_HOOK_VERSION;
    hook.loudness_rule_count = 1u;
    CHECK(su_audio_events_attach(rig.events, &hook) == SU_E_INVALID_ARGUMENT);
    {
        SuAudioLoudnessRule loudness = {0u, 0u, NAN};
        hook.loudness_rules = &loudness;
        CHECK(su_audio_events_attach(rig.events, &hook) == SU_E_INVALID_ARGUMENT);
        loudness.loudness_db = 0.0f;
        hook.loudness_rule_count = SU_AUDIO_EVENTS_MAX_LOUDNESS_RULES + 1u;
        CHECK(su_audio_events_attach(rig.events, &hook) == SU_E_INVALID_ARGUMENT);
        hook.loudness_rules = NULL;
        hook.loudness_rule_count = 0u;
    }
    {
        SuAudioSpeakerLayout layout;
        memset(&layout, 0, sizeof layout);
        hook.speaker_layout = &layout;
        CHECK(su_audio_events_attach(rig.events, &hook) == SU_E_INVALID_ARGUMENT);
        hook.speaker_layout = NULL;
    }
    (void)post(&rig, 1u, 0u, 0u);
    CHECK(su_audio_events_attach(rig.events, &hook) == SU_E_RESOURCE_LIMIT);
    rig_destroy(&rig);
    su_audio_voice_pool_destroy(pool);
}

/* ---- parameters, switches, buses, music and the master limiter ---- */

static const char mix_json[] =
    "{\"schema\":\"su_audio_events_v1\",\"numeric_profile\":\"audio_float_local_v1\","
    "\"seed_policy\":\"event_emitter_sequence_v1\",\"sample_rate\":48000,\"nodes\":["
    "{\"id\":1,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":1},\"bus_id\":10},"
    "{\"id\":2,\"type\":\"sound\",\"asset\":{\"id\":2,\"generation\":2},\"bus_id\":30,\"flags\":1},"
    "{\"id\":3,\"type\":\"sound\",\"asset\":{\"id\":3,\"generation\":3},\"bus_id\":10},"
    "{\"id\":4,\"type\":\"switch\",\"switch_id\":5,\"default\":1,\"cases\":[{\"value\":2,\"node\":3}]},"
    "{\"id\":5,\"type\":\"blend\",\"parameter_id\":9,\"points\":[{\"value\":0,\"node\":1},{\"value\":1,\"node\":3}]}],"
    "\"events\":[{\"id\":1,\"root\":1},{\"id\":2,\"root\":2},{\"id\":3,\"root\":4},{\"id\":4,\"root\":5}]}";

enum { MIX_GAIN = 1, MIX_BUS_GAIN = 2, MIX_MUSIC = 4, MIX_PITCH = 8, MIX_HDR = 16, MIX_LOUDNESS = 32,
       MIX_STEAL = 64 };

typedef struct MixRig {
    SuAudioAssets *assets;
    SuAudioGraph *graph;
    SuAudioEvents *events;
    SuAudioVoicePool *pool;
    SuAudioParams *params;
    SuAudioBusTree *tree;
    SuAudioMusic *music;
    uint64_t seq;
} MixRig;

static void mix_create(MixRig *rig, uint32_t flags, float knee) {
    static const SuAudioBindingPoint identity[2] = {{-24.0f, -24.0f}, {0.0f, 0.0f}};
    static const SuAudioBindingPoint fader[2] = {{0.0f, 0.0f}, {1.0f, -12.0f}};
    static const SuAudioBindingPoint octave[2] = {{0.0f, 1.0f}, {1.0f, 2.0f}};
    SuAudioGraphLimits limits = {16u, 32u, 8u, 16u, 8192u};
    SuAudioGraphDiagnostic diagnostic;
    SuAudioEventsDesc desc;
    SuAudioClip clip;
    SuAudioAssetRef ref;
    SuAudioParamDef defs[3];
    SuAudioSwitchDef sw;
    SuAudioParamBinding bindings[3];
    SuAudioParamsDesc params_desc;
    SuAudioBusDesc buses[4];
    SuAudioDuckRule duck;
    SuAudioBusTreeDesc tree_desc;
    SuAudioBusDiagnostic bus_diag;
    SuAudioVoicePoolDesc pool_desc;
    SuAudioEventsHook hook;
    uint32_t binding_count = 0u;
    uint32_t index;
    memset(rig, 0, sizeof *rig);
    CHECK(su_audio_assets_create(8u, 1u << 20, &rig->assets) == SU_OK);
    clip.channels = 1u;
    clip.sample_rate = RATE;
    clip.samples = tone;
    clip.frames = TONE;
    CHECK(su_audio_asset_prepare(rig->assets, 1u, &clip, &ref) == SU_OK);
    clip.samples = loop_tone;
    clip.frames = LOOP;
    CHECK(su_audio_asset_prepare(rig->assets, 2u, &clip, &ref) == SU_OK);
    clip.samples = ramp;
    clip.frames = TONE;
    CHECK(su_audio_asset_prepare(rig->assets, 3u, &clip, &ref) == SU_OK);
    CHECK(su_audio_graph_prepare(mix_json, sizeof mix_json - 1u, rig->assets, &limits, &rig->graph,
        &diagnostic) == SU_OK);
    memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.sample_rate = RATE;
    desc.max_instances = 16u;
    desc.max_leaves_per_instance = 4u;
    desc.max_logical_voices = 64u;
    desc.command_capacity = 16u;
    desc.project_seed = 5u;
    desc.session_generation = 1u;
    CHECK(su_audio_events_create(&desc, rig->assets, NULL, rig->graph, &rig->events) == SU_OK);
    memset(defs, 0, sizeof defs);
    defs[0].id = 9u;
    defs[0].max_value = 1.0f;
    defs[1].id = 1u;
    defs[1].min_value = -24.0f;
    defs[2].id = 2u;
    defs[2].max_value = 1.0f;
    defs[2].smoothing_frames = 4800u;
    memset(&sw, 0, sizeof sw);
    sw.id = 5u;
    sw.default_value = 1u;
    sw.policy = SU_AUDIO_SWITCH_CONTINUE;
    memset(bindings, 0, sizeof bindings);
    if (flags & MIX_GAIN) {
        bindings[binding_count].param_id = 1u;
        bindings[binding_count].control = SU_AUDIO_CONTROL_GAIN_DB;
        bindings[binding_count].points = identity;
        bindings[binding_count++].point_count = 2u;
    }
    if (flags & MIX_BUS_GAIN) {
        bindings[binding_count].param_id = 2u;
        bindings[binding_count].control = SU_AUDIO_CONTROL_BUS_GAIN_DB;
        bindings[binding_count].bus_id = 30u;
        bindings[binding_count].points = fader;
        bindings[binding_count++].point_count = 2u;
    }
    if (flags & MIX_PITCH) {
        bindings[binding_count].param_id = 9u;
        bindings[binding_count].control = SU_AUDIO_CONTROL_PITCH;
        bindings[binding_count].points = octave;
        bindings[binding_count++].point_count = 2u;
    }
    memset(&params_desc, 0, sizeof params_desc);
    params_desc.struct_size = sizeof params_desc;
    params_desc.sample_rate = RATE;
    params_desc.params = defs;
    params_desc.param_count = 3u;
    params_desc.switches = &sw;
    params_desc.switch_count = 1u;
    params_desc.bindings = bindings;
    params_desc.binding_count = binding_count;
    params_desc.max_scoped_values = 16u;
    CHECK(su_audio_params_create(&params_desc, &rig->params) == SU_OK);
    memset(buses, 0, sizeof buses);
    buses[0].id = 1u;
    buses[0].parent = SU_AUDIO_BUS_ROOT;
    buses[1].id = 10u;
    buses[2].id = 30u;
    buses[3].id = 40u;
    for (index = 1u; index < 4u; index++) {
        buses[index].parent = 1u;
        buses[index].hdr = 1u;
    }
    memset(&duck, 0, sizeof duck);
    duck.source = 10u;
    duck.target = 30u;
    duck.threshold_db = -30.0f;
    duck.ratio = 4.0f;
    duck.attack_ms = 5.0f;
    duck.hold_ms = 20.0f;
    duck.release_ms = 100.0f;
    duck.max_attenuation_db = 12.0f;
    memset(&tree_desc, 0, sizeof tree_desc);
    tree_desc.struct_size = sizeof tree_desc;
    tree_desc.sample_rate = RATE;
    tree_desc.max_block_frames = 128u;
    tree_desc.buses = buses;
    tree_desc.bus_count = 4u;
    tree_desc.rules = &duck;
    tree_desc.rule_count = 1u;
    tree_desc.hdr.enabled = (flags & MIX_HDR) != 0u;
    tree_desc.hdr.window_db = 30.0f;
    CHECK(su_audio_bus_tree_create(&tree_desc, &rig->tree, &bus_diag) == SU_OK);
    memset(&pool_desc, 0, sizeof pool_desc);
    pool_desc.struct_size = sizeof pool_desc;
    pool_desc.sample_rate = RATE;
    pool_desc.max_logical = 64u;
    pool_desc.max_physical = (flags & MIX_STEAL) != 0u ? 1u : 16u;
    pool_desc.transition_reserve = 4u;
    CHECK(su_audio_voice_pool_create(&pool_desc, &rig->pool) == SU_OK);
    memset(&hook, 0, sizeof hook);
    hook.struct_size = sizeof hook;
    hook.version = SU_AUDIO_EVENTS_HOOK_VERSION;
    hook.voices = rig->pool;
    hook.default_policy.release_frames = 64u;
    hook.params = rig->params;
    hook.buses = rig->tree;
    hook.limiter_knee = knee;
    if (flags & MIX_STEAL) {
        SuAudioVoicePolicyRule priority;
        memset(&priority, 0, sizeof priority);
        priority.node_id = 2u;
        priority.policy.priority = 100;
        hook.policies = &priority;
        hook.policy_count = 1u;
        hook.default_policy.release_frames = 256u;
        if (flags & MIX_LOUDNESS) {
            static const SuAudioLoudnessRule loudness[] = {{1u, 10u, 20.0f}, {2u, 30u, -40.0f}};
            hook.loudness_rules = loudness;
            hook.loudness_rule_count = 2u;
        }
        CHECK(su_audio_events_attach(rig->events, &hook) == SU_OK);
        return;
    }
    if (flags & MIX_LOUDNESS) {
        static const SuAudioLoudnessRule loudness[] = {{1u, 10u, 20.0f}, {2u, 30u, -40.0f}, {0u, 0u, 5.0f}};
        hook.loudness_rules = loudness;
        hook.loudness_rule_count = 3u;
    }
    if (flags & MIX_MUSIC) {
        static const SuAudioAssetRef stems[1] = {{2u, 2u}};
        static const SuAudioTempoPoint tempo[1] = {{0u, 120000u}};
        SuAudioMusicDesc music_desc;
        SuAudioMusicSegmentDesc seg;
        SuMusicTransition move;
        uint64_t boundary = 0u;
        memset(&music_desc, 0, sizeof music_desc);
        music_desc.struct_size = sizeof music_desc;
        music_desc.sample_rate = RATE;
        music_desc.max_segments = 2u;
        CHECK(su_audio_music_create(&music_desc, rig->assets, &rig->music) == SU_OK);
        memset(&seg, 0, sizeof seg);
        seg.struct_size = sizeof seg;
        seg.segment_id = 1u;
        seg.stems = stems;
        seg.stem_count = 1u;
        seg.tempo = tempo;
        seg.tempo_count = 1u;
        seg.beats_per_bar = 4u;
        seg.length_frames = LOOP;
        seg.loop = 1u;
        CHECK(su_audio_music_segment_add(rig->music, &seg) == SU_OK);
        memset(&move, 0, sizeof move);
        move.struct_size = sizeof move;
        move.segment_id = 1u;
        move.sync = SU_AUDIO_MUSIC_IMMEDIATE;
        CHECK(su_audio_music_transition(rig->music, &move, 0u, &boundary) == SU_OK);
        hook.music = rig->music;
        hook.music_bus_id = 40u;
    }
    CHECK(su_audio_events_attach(rig->events, &hook) == SU_OK);
}

static void mix_destroy(MixRig *rig) {
    su_audio_events_destroy(rig->events);
    su_audio_music_destroy(rig->music);
    su_audio_voice_pool_destroy(rig->pool);
    su_audio_params_destroy(rig->params);
    su_audio_bus_tree_destroy(rig->tree);
    su_audio_graph_release(rig->graph);
    CHECK(su_audio_assets_destroy(rig->assets) == SU_OK);
}

static void mix_post(MixRig *rig, uint32_t event_id, uint64_t at) {
    SuAudioTrigger trigger;
    SuAudioInstance instance = 0u;
    memset(&trigger, 0, sizeof trigger);
    trigger.struct_size = sizeof trigger;
    trigger.event_id = event_id;
    trigger.sample_time = at;
    trigger.commit_seq = rig->seq++;
    trigger.session_generation = 1u;
    CHECK(su_audio_event_post(rig->events, &trigger, &instance, NULL) == SU_OK);
}

/* Switch and blend nodes expand from the parameter state at the trigger time. */
static void test_switch_blend(void) {
    MixRig rig;
    SuAudioParamScope global = {SU_AUDIO_SCOPE_GLOBAL, 0u};
    uint32_t index;
    int exact = 1;
    float worst = 0.0f;
    mix_create(&rig, 0u, 0.0f);
    CHECK(su_audio_switch_set_at(rig.params, global, 5u, 2u, 2000u) == SU_OK);
    CHECK(su_audio_param_set_at(rig.params, global, 9u, 0.25f, 0u) == SU_OK);
    mix_post(&rig, 3u, 100u);   /* before the switch change: default case, tone */
    mix_post(&rig, 3u, 6000u);  /* after: ramp */
    mix_post(&rig, 4u, 12000u); /* blend 0.75 tone + 0.25 ramp */
    CHECK(su_audio_events_render(rig.events, out_a, FRAMES) == SU_OK);
    for (index = 0; index < 1000u; index++) {
        exact &= out_a[(100u + index) * 2u] == tone[index];
        exact &= out_a[(6000u + index) * 2u] == ramp[index];
    }
    CHECK(exact);
    for (index = 0; index < 4000u; index++) {
        float expected = tone[index] * 0.75f + ramp[index] * 0.25f;
        float error = fabsf(out_a[(12000u + index) * 2u] - expected);
        worst = error > worst ? error : worst;
    }
    CHECK(worst < 1.0e-6f);
    mix_destroy(&rig);
}

/* GAIN_DB and PITCH bindings modulate every voice of the runtime. */
static void test_voice_bindings(void) {
    MixRig rig;
    SuAudioParamScope global = {SU_AUDIO_SCOPE_GLOBAL, 0u};
    uint32_t index;
    float worst = 0.0f;
    float scale = powf(10.0f, -6.0f / 20.0f);
    mix_create(&rig, MIX_GAIN | MIX_PITCH, 0.0f);
    CHECK(su_audio_param_set_at(rig.params, global, 1u, -6.0f, 0u) == SU_OK);
    CHECK(su_audio_param_set_at(rig.params, global, 9u, 1.0f, 0u) == SU_OK); /* pitch x2 */
    mix_post(&rig, 1u, 256u);
    CHECK(su_audio_events_render(rig.events, out_a, 4096u) == SU_OK);
    for (index = 0; index < 2000u; index++) {
        float error = fabsf(out_a[(256u + index) * 2u] - tone[index * 2u] * scale);
        worst = error > worst ? error : worst;
    }
    CHECK(worst < 1.0e-6f);
    mix_destroy(&rig);
}

/* Shots on bus 10 duck the ambience bus through its real signal; a BUS_GAIN_DB binding owns the ambience
 * fader; music renders through its own bus; the soft limiter bends the master. */
static void test_buses_music_limiter(void) {
    MixRig rig;
    SuAudioParamScope global = {SU_AUDIO_SCOPE_GLOBAL, 0u};
    SuAudioBusMeter meter;
    float before = 1.0f;
    float during = 1.0f;
    float peak = 0.0f;
    uint32_t done;
    mix_create(&rig, MIX_BUS_GAIN | MIX_MUSIC, 0.0f);
    mix_post(&rig, 2u, 0u);
    mix_post(&rig, 1u, 12000u);
    CHECK(su_audio_param_set_at(rig.params, global, 2u, 1.0f, 18000u) == SU_OK);
    for (done = 0u; done < FRAMES; done += 128u) {
        float gain = 0.0f;
        uint32_t take = FRAMES - done < 128u ? FRAMES - done : 128u;
        CHECK(su_audio_events_render(rig.events, out_a + (size_t)done * 2u, take) == SU_OK);
        CHECK(su_audio_bus_gain_at(rig.tree, 30u, &gain) == SU_OK);
        if (done >= 4000u && done < 11900u && gain < before) {
            before = gain;
        }
        if (done >= 12200u && done < 16000u && gain < during) {
            during = gain;
        }
    }
    CHECK(before > 0.999f);
    CHECK(during < 0.7f);
    CHECK(su_audio_bus_meter_read(rig.tree, 30u, &meter, 0) == SU_OK);
    CHECK(fabsf(meter.fader_db + 12.0f) < 0.01f);
    CHECK(su_audio_bus_meter_read(rig.tree, 40u, &meter, 0) == SU_OK);
    CHECK(meter.peak > 0.4f);
    mix_destroy(&rig);
    /* the limiter: bus 10 boosted by 18 dB drives the master far over full scale */
    mix_create(&rig, 0u, 0.5f);
    CHECK(su_audio_bus_set_gain(rig.tree, 10u, 18.0f, 0u) == SU_OK);
    mix_post(&rig, 1u, 0u);
    CHECK(su_audio_events_render(rig.events, out_a, 4800u) == SU_OK);
    for (done = 0u; done < 4800u * 2u; done++) {
        float magnitude = fabsf(out_a[done]);
        peak = magnitude > peak ? magnitude : peak;
    }
    /* tanh saturates to exactly full scale in float for inputs this far over the knee, never beyond */
    CHECK(peak <= 1.0f && peak > 0.95f);
    {
        uint32_t bent = 0u;
        for (done = 0u; done < 4800u * 2u; done++) {
            float magnitude = fabsf(out_a[done]);
            bent += magnitude > 0.55f && magnitude < 0.99f;
        }
        CHECK(bent > 100u);
    }
    mix_destroy(&rig);
}

/* ---- clustering and direct checkpoints over the full runtime ---- */

#define FULL_FRAMES 48000u

typedef struct FullRig {
    MixRig mix;
    SuAudioScene *scene;
    SuAudioCluster *cluster;
    SuAudioEmitter emitters[2];
    SuAudioInstance handles[64];
    uint32_t posted;
    uint64_t object_hash;
    uint64_t object_frames;
} FullRig;

static void object_output(void *user, const float *pcm, uint32_t frames, uint32_t channels, uint64_t time) {
    FullRig *rig = user;
    uint32_t sample;
    CHECK(channels == 4u && frames == SU_AUDIO_EVENT_QUANTUM);
    rig->object_hash ^= time;
    for (sample = 0u; sample < frames * channels; sample++) {
        uint32_t bits;
        CHECK(isfinite(pcm[sample]));
        memcpy(&bits, &pcm[sample], sizeof bits);
        rig->object_hash = (rig->object_hash ^ bits) * UINT64_C(1099511628211);
    }
    rig->object_frames += frames;
}

static void full_create(FullRig *rig, int with_cluster, uint32_t physical) {
    static const SuAudioBindingPoint identity[2] = {{-24.0f, -24.0f}, {0.0f, 0.0f}};
    static const SuAudioBindingPoint fader[2] = {{0.0f, 0.0f}, {1.0f, -12.0f}};
    static const SuAudioBindingPoint octave[2] = {{0.0f, 1.0f}, {1.0f, 2.0f}};
    static const SuAudioAssetRef stems[1] = {{2u, 2u}};
    static const SuAudioTempoPoint tempo[1] = {{0u, 120000u}};
    SuAudioGraphLimits limits = {16u, 32u, 8u, 16u, 8192u};
    SuAudioGraphDiagnostic diagnostic;
    SuAudioEventsDesc desc;
    SuAudioClip clip;
    SuAudioAssetRef ref;
    SuAudioParamDef defs[3];
    SuAudioSwitchDef sw;
    SuAudioParamBinding bindings[3];
    SuAudioParamsDesc params_desc;
    SuAudioBusDesc buses[4];
    SuAudioDuckRule duck;
    SuAudioBusTreeDesc tree_desc;
    SuAudioBusDiagnostic bus_diag;
    SuAudioVoicePoolDesc pool_desc;
    SuAudioEventsHook hook;
    SuAudioSceneDesc scene_desc = {sizeof scene_desc, 4u, 2u};
    SuAudioCurvePoint points[2];
    SuAudioCurveDesc curve;
    SuAudioCurveId curve_id = 0u;
    SuAudioEmitterSnapshot emitter;
    SuAudioCameraListener listener;
    SuAudioMusicDesc music_desc;
    SuAudioMusicSegmentDesc seg;
    SuMusicTransition move;
    uint64_t boundary = 0u;
    uint32_t index;
    MixRig *m = &rig->mix;
    memset(rig, 0, sizeof *rig);
    CHECK(su_audio_assets_create(8u, 1u << 20, &m->assets) == SU_OK);
    clip.channels = 1u;
    clip.sample_rate = RATE;
    clip.samples = tone;
    clip.frames = TONE;
    CHECK(su_audio_asset_prepare(m->assets, 1u, &clip, &ref) == SU_OK);
    clip.samples = loop_tone;
    clip.frames = LOOP;
    CHECK(su_audio_asset_prepare(m->assets, 2u, &clip, &ref) == SU_OK);
    clip.samples = ramp;
    clip.frames = TONE;
    CHECK(su_audio_asset_prepare(m->assets, 3u, &clip, &ref) == SU_OK);
    CHECK(su_audio_graph_prepare(mix_json, sizeof mix_json - 1u, m->assets, &limits, &m->graph, &diagnostic) == SU_OK);
    CHECK(su_audio_scene_create(&scene_desc, &rig->scene) == SU_OK);
    memset(points, 0, sizeof points);
    points[0].distance_m = 1.0f;
    points[0].lowpass_hz = 24000.0f;
    points[0].scale = 1.0f;
    points[1].distance_m = 60.0f;
    points[1].gain_db = -18.0f;
    points[1].lowpass_hz = 2000.0f;
    points[1].scale = 1.0f;
    curve.struct_size = sizeof curve;
    curve.points = points;
    curve.point_count = 2u;
    CHECK(su_audio_scene_curve_add(rig->scene, &curve, &curve_id) == SU_OK);
    memset(&emitter, 0, sizeof emitter);
    emitter.curve = curve_id;
    emitter.scale = 1.0f;
    emitter.pos = meters(-6.0, -4.0);
    CHECK(su_audio_emitter_create(rig->scene, &emitter, &rig->emitters[0]) == SU_OK);
    emitter.pos = meters(20.0, -25.0);
    CHECK(su_audio_emitter_create(rig->scene, &emitter, &rig->emitters[1]) == SU_OK);
    memset(&listener, 0, sizeof listener);
    listener.rot.w = 1.0f;
    CHECK(su_audio_scene_listener_set(rig->scene, &listener) == SU_OK);
    memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.sample_rate = RATE;
    desc.max_instances = 24u;
    desc.max_leaves_per_instance = 4u;
    desc.max_logical_voices = 64u;
    desc.command_capacity = 16u;
    desc.project_seed = 9u;
    desc.session_generation = 1u;
    CHECK(su_audio_events_create(&desc, m->assets, rig->scene, m->graph, &m->events) == SU_OK);
    memset(defs, 0, sizeof defs);
    defs[0].id = 9u;
    defs[0].max_value = 1.0f;
    defs[0].smoothing_frames = 2400u;
    defs[1].id = 1u;
    defs[1].min_value = -24.0f;
    defs[1].smoothing_frames = 1200u;
    defs[2].id = 2u;
    defs[2].max_value = 1.0f;
    defs[2].smoothing_frames = 4800u;
    memset(&sw, 0, sizeof sw);
    sw.id = 5u;
    sw.default_value = 1u;
    sw.policy = SU_AUDIO_SWITCH_CONTINUE;
    memset(bindings, 0, sizeof bindings);
    bindings[0].param_id = 1u;
    bindings[0].control = SU_AUDIO_CONTROL_GAIN_DB;
    bindings[0].points = identity;
    bindings[0].point_count = 2u;
    bindings[1].param_id = 2u;
    bindings[1].control = SU_AUDIO_CONTROL_BUS_GAIN_DB;
    bindings[1].bus_id = 30u;
    bindings[1].points = fader;
    bindings[1].point_count = 2u;
    bindings[2].param_id = 9u;
    bindings[2].control = SU_AUDIO_CONTROL_PITCH;
    bindings[2].points = octave;
    bindings[2].point_count = 2u;
    memset(&params_desc, 0, sizeof params_desc);
    params_desc.struct_size = sizeof params_desc;
    params_desc.sample_rate = RATE;
    params_desc.params = defs;
    params_desc.param_count = 3u;
    params_desc.switches = &sw;
    params_desc.switch_count = 1u;
    params_desc.bindings = bindings;
    params_desc.binding_count = 3u;
    params_desc.max_scoped_values = 32u;
    CHECK(su_audio_params_create(&params_desc, &m->params) == SU_OK);
    memset(buses, 0, sizeof buses);
    buses[0].id = 1u;
    buses[0].parent = SU_AUDIO_BUS_ROOT;
    buses[1].id = 10u;
    buses[2].id = 30u;
    buses[3].id = 40u;
    for (index = 1u; index < 4u; index++) {
        buses[index].parent = 1u;
    }
    memset(&duck, 0, sizeof duck);
    duck.source = 10u;
    duck.target = 30u;
    duck.threshold_db = -30.0f;
    duck.ratio = 4.0f;
    duck.attack_ms = 5.0f;
    duck.hold_ms = 20.0f;
    duck.release_ms = 100.0f;
    duck.max_attenuation_db = 12.0f;
    duck.flags = SU_AUDIO_DUCK_DELAY_QUANTUM;
    memset(&tree_desc, 0, sizeof tree_desc);
    tree_desc.struct_size = sizeof tree_desc;
    tree_desc.sample_rate = RATE;
    tree_desc.max_block_frames = 128u;
    tree_desc.buses = buses;
    tree_desc.bus_count = 4u;
    tree_desc.rules = &duck;
    tree_desc.rule_count = 1u;
    CHECK(su_audio_bus_tree_create(&tree_desc, &m->tree, &bus_diag) == SU_OK);
    memset(&pool_desc, 0, sizeof pool_desc);
    pool_desc.struct_size = sizeof pool_desc;
    pool_desc.sample_rate = RATE;
    pool_desc.max_logical = 64u;
    pool_desc.max_physical = physical;
    pool_desc.transition_reserve = 2u;
    CHECK(su_audio_voice_pool_create(&pool_desc, &m->pool) == SU_OK);
    memset(&music_desc, 0, sizeof music_desc);
    music_desc.struct_size = sizeof music_desc;
    music_desc.sample_rate = RATE;
    music_desc.max_segments = 2u;
    CHECK(su_audio_music_create(&music_desc, m->assets, &m->music) == SU_OK);
    memset(&seg, 0, sizeof seg);
    seg.struct_size = sizeof seg;
    seg.segment_id = 1u;
    seg.stems = stems;
    seg.stem_count = 1u;
    seg.tempo = tempo;
    seg.tempo_count = 1u;
    seg.beats_per_bar = 4u;
    seg.length_frames = LOOP;
    seg.loop = 1u;
    CHECK(su_audio_music_segment_add(m->music, &seg) == SU_OK);
    memset(&move, 0, sizeof move);
    move.struct_size = sizeof move;
    move.segment_id = 1u;
    move.sync = SU_AUDIO_MUSIC_IMMEDIATE;
    CHECK(su_audio_music_transition(m->music, &move, 0u, &boundary) == SU_OK);
    memset(&hook, 0, sizeof hook);
    hook.struct_size = sizeof hook;
    hook.version = SU_AUDIO_EVENTS_HOOK_VERSION;
    hook.voices = m->pool;
    hook.default_policy.release_frames = 64u;
    hook.default_policy.virtual_mode = SU_AUDIO_VIRTUAL_ADVANCE;
    hook.params = m->params;
    hook.buses = m->tree;
    hook.music = m->music;
    hook.music_bus_id = 40u;
    hook.limiter_knee = 0.8f;
    if (with_cluster) {
        SuAudioClusterDesc cluster_desc;
        memset(&cluster_desc, 0, sizeof cluster_desc);
        cluster_desc.struct_size = sizeof cluster_desc;
        cluster_desc.width_rad = 1.0f;
        cluster_desc.leave_ratio = 1.25f;
        cluster_desc.hold_frames = 512u;
        cluster_desc.crossfade_frames = 256u;
        cluster_desc.output_limit = 2u;
        cluster_desc.max_objects = 16u;
        CHECK(su_audio_cluster_create(&cluster_desc, &rig->cluster) == SU_OK);
        hook.cluster = rig->cluster;
        hook.cluster_inputs = SU_AUDIO_EVENTS_CLUSTER_VOICES | SU_AUDIO_EVENTS_CLUSTER_BUSES;
    }
    if (with_cluster == 2) {
        SuAudioSpeakerLayout layout;
        memset(&layout, 0, sizeof layout);
        layout.count = 4u;
        layout.directions[0].x = -1.0f;
        layout.directions[1].x = 1.0f;
        layout.directions[2].y = 1.0f;
        layout.directions[3].z = -1.0f;
        layout.bed_right = 1u;
        hook.speaker_layout = &layout;
        hook.object_output = object_output;
        hook.object_user = rig;
        CHECK(su_audio_events_attach(m->events, &hook) == SU_OK);
        return;
    }
    CHECK(su_audio_events_attach(m->events, &hook) == SU_OK);
}

static void full_destroy(FullRig *rig) {
    mix_destroy(&rig->mix);
    su_audio_cluster_destroy(rig->cluster);
    su_audio_scene_destroy(rig->scene);
}

/* A deterministic presentation script: commands issued at render positions (as a game would issue them
 * between render calls), each targeting a time at or after the position. */
typedef struct ScriptItem {
    uint32_t issue;
    uint32_t kind;     /* 0 post, 1 param, 2 switch, 3 listener, 4 emitter, 5 stop */
    uint32_t a;
    uint32_t b;
    float value;
} ScriptItem;

static ScriptItem script[64];
static uint32_t script_count;

static void script_add(uint32_t issue, uint32_t kind, uint32_t a, uint32_t b, float value) {
    script[script_count].issue = issue;
    script[script_count].kind = kind;
    script[script_count].a = a;
    script[script_count].b = b;
    script[script_count].value = value;
    script_count++;
}

static void build_script(void) {
    uint32_t index;
    script_count = 0u;
    script_add(0u, 0u, 2u, 0u, 0.0f);        /* ambience loop, bus 30, handle 0 */
    script_add(0u, 1u, 2u, 9000u, 0.6f);     /* ambience fader binding */
    script_add(0u, 2u, 5u, 20000u, 2.0f);    /* switch to the ramp case */
    for (index = 0; index < 14u; index++) {
        uint32_t issue = 700u + index * 3100u;
        script_add(issue, 0u, index % 3u == 0u ? 1u : index % 3u == 1u ? 3u : 4u, 1u + index % 2u, 0.0f);
    }
    script_add(15000u, 3u, 0u, 0u, 0.0f);    /* listener turns */
    script_add(25000u, 4u, 1u, 0u, 0.0f);    /* emitter 2 approaches */
    script_add(10000u, 6u, 1u, 11000u, -17.0f);
    script_add(30000u, 5u, 0u, 40000u, 500.0f);
    script_add(31000u, 1u, 9u, 33000u, 0.5f); /* pitch glide on every voice */
    script_add(36000u, 1u, 1u, 36500u, -9.0f);
    for (index = 1u; index < script_count; index++) { /* stable insertion sort by issue position */
        ScriptItem item = script[index];
        uint32_t at = index;
        while (at > 0u && script[at - 1u].issue > item.issue) {
            script[at] = script[at - 1u];
            at--;
        }
        script[at] = item;
    }
}

static void script_apply(FullRig *rig, const ScriptItem *item) {
    MixRig *m = &rig->mix;
    SuAudioParamScope global = {SU_AUDIO_SCOPE_GLOBAL, 0u};
    switch (item->kind) {
    case 0u: {
        SuAudioTrigger trigger;
        SuAudioInstance instance = 0u;
        memset(&trigger, 0, sizeof trigger);
        trigger.struct_size = sizeof trigger;
        trigger.event_id = item->a;
        trigger.emitter = item->b ? rig->emitters[item->b - 1u] : 0u;
        trigger.sample_time = item->issue + 200u;
        trigger.commit_seq = m->seq++;
        trigger.session_generation = 1u;
        trigger.scope_id = 3u;
        CHECK(su_audio_event_post(m->events, &trigger, &instance, NULL) == SU_OK);
        rig->handles[rig->posted++] = instance;
        break;
    }
    case 1u:
        CHECK(su_audio_param_set_at(m->params, global, item->a, item->value, item->b) == SU_OK);
        break;
    case 2u:
        CHECK(su_audio_switch_set_at(m->params, global, item->a, (uint32_t)item->value, item->b) == SU_OK);
        break;
    case 3u: {
        SuAudioCameraListener listener;
        memset(&listener, 0, sizeof listener);
        listener.rot.y = 0.38268343f;
        listener.rot.w = 0.92387953f;
        listener.pos = meters(1.0, 0.0);
        CHECK(su_audio_scene_listener_set(rig->scene, &listener) == SU_OK);
        break;
    }
    case 4u: {
        SuVec3f velocity = {0.0f, 0.0f, 0.0f};
        CHECK(su_audio_emitter_update(rig->scene, rig->emitters[item->a], meters(4.0, -3.0), velocity) == SU_OK);
        break;
    }
    case 5u:
        CHECK(su_audio_instance_stop(m->events, rig->handles[item->a], item->b, (uint32_t)item->value) == SU_OK);
        break;
    default: {
        SuAudioParamScope instance_scope = {SU_AUDIO_SCOPE_INSTANCE, rig->handles[0]};
        CHECK(su_audio_param_set_at(m->params, instance_scope, item->a, item->value, item->b) == SU_OK);
        break;
    }
    }
}

/* Renders [from, to) applying script items issued in that range; chunk sizes vary. */
static void script_run(FullRig *rig, uint32_t from, uint32_t to, float *out) {
    uint32_t position = from;
    uint32_t item = 0u;
    uint32_t chunk = 61u;
    while (item < script_count && script[item].issue < from) {
        item++;
    }
    while (position < to) {
        uint32_t next = to;
        uint32_t take;
        while (item < script_count && script[item].issue == position) {
            script_apply(rig, &script[item++]);
        }
        if (item < script_count && script[item].issue < next) {
            next = script[item].issue;
        }
        take = next - position < chunk ? next - position : chunk;
        chunk = chunk == 61u ? 500u : chunk == 500u ? 128u : 61u;
        CHECK(su_audio_events_render(rig->mix.events, out + (size_t)position * 2u, take) == SU_OK);
        position += take;
    }
}

static float full_a[FULL_FRAMES * 2u];
static float full_b[FULL_FRAMES * 2u];
static uint8_t blob[1u << 20];

static void test_authored_loudness(void) {
    MixRig plain;
    MixRig authored;
    MixRig restored;
    SuAudioBusMeter meter;
    size_t written = 0u;
    mix_create(&plain, 0u, 0.0f);
    mix_create(&authored, MIX_LOUDNESS, 0.0f);
    mix_post(&plain, 1u, 0u);
    mix_post(&authored, 1u, 0u);
    CHECK(su_audio_events_render(plain.events, out_a, 1024u) == SU_OK);
    CHECK(su_audio_events_render(authored.events, out_b, 1024u) == SU_OK);
    CHECK(memcmp(out_a, out_b, 2048u * sizeof(float)) == 0);
    mix_destroy(&plain);
    mix_destroy(&authored);
    mix_create(&authored, MIX_HDR | MIX_LOUDNESS, 0.0f);
    mix_create(&restored, MIX_HDR | MIX_LOUDNESS, 0.0f);
    mix_post(&authored, 1u, 0u);
    mix_post(&authored, 2u, 0u);
    CHECK(su_audio_events_render(authored.events, out_a, 1u) == SU_OK);
    CHECK(su_audio_bus_meter_read(authored.tree, 10u, &meter, 0) == SU_OK);
    CHECK(meter.hdr_db == 20.0f);
    CHECK(su_audio_bus_meter_read(authored.tree, 30u, &meter, 0) == SU_OK);
    CHECK(meter.hdr_db == 200.0f);
    CHECK(su_audio_events_checkpoint_save(authored.events, blob, sizeof blob, &written) == SU_OK);
    CHECK(su_audio_events_checkpoint_load(restored.events, blob, written) == SU_OK);
    CHECK(su_audio_events_render(authored.events, out_a, 8192u) == SU_OK);
    CHECK(su_audio_events_render(restored.events, out_b, 8192u) == SU_OK);
    CHECK(memcmp(out_a, out_b, 16384u * sizeof(float)) == 0);
    CHECK(su_audio_bus_meter_read(authored.tree, 30u, &meter, 0) == SU_OK);
    CHECK(meter.hdr_db == 0.0f);
    mix_destroy(&authored);
    mix_destroy(&restored);
    mix_create(&plain, MIX_HDR, 0.0f);
    CHECK(su_audio_events_checkpoint_load(plain.events, blob, written) == SU_E_SCHEMA_MISMATCH);
    mix_post(&plain, 1u, 0u);
    CHECK(su_audio_events_render(plain.events, out_a, 128u) == SU_OK);
    CHECK(su_audio_bus_meter_read(plain.tree, 10u, &meter, 0) == SU_OK);
    CHECK(meter.hdr_db == 0.0f);
    mix_destroy(&plain);
    mix_create(&authored, MIX_HDR | MIX_LOUDNESS | MIX_STEAL, 0.0f);
    mix_post(&authored, 1u, 127u);
    mix_post(&authored, 2u, 128u);
    CHECK(su_audio_events_render(authored.events, out_a, 128u) == SU_OK);
    CHECK(su_audio_bus_meter_read(authored.tree, 10u, &meter, 0) == SU_OK);
    CHECK(meter.hdr_db == 20.0f);
    CHECK(su_audio_events_render(authored.events, out_a, 128u) == SU_OK);
    CHECK(su_audio_bus_meter_read(authored.tree, 10u, &meter, 0) == SU_OK);
    CHECK(meter.hdr_db == 20.0f);
    CHECK(su_audio_events_render(authored.events, out_a, 128u) == SU_OK);
    CHECK(su_audio_bus_meter_read(authored.tree, 10u, &meter, 0) == SU_OK);
    CHECK(meter.hdr_db == 20.0f);
    CHECK(su_audio_events_render(authored.events, out_a, 128u) == SU_OK);
    CHECK(su_audio_bus_meter_read(authored.tree, 10u, &meter, 0) == SU_OK);
    CHECK(meter.hdr_db == 0.0f);
    {
        SuAudioVoiceStats stats;
        su_audio_voice_pool_stats(authored.pool, &stats);
        CHECK(stats.stolen == 1u && stats.virtual_voices == 1u);
    }
    mix_destroy(&authored);
}

static void test_object_output(void) {
    FullRig plain;
    FullRig rendered;
    FullRig restored;
    size_t written = 0u;
    build_script();
    full_create(&plain, 1, 3u);
    full_create(&rendered, 2, 3u);
    full_create(&restored, 2, 3u);
    script_run(&plain, 0u, 9001u, full_a);
    script_run(&rendered, 0u, 9001u, full_b);
    CHECK(memcmp(full_a, full_b, 18002u * sizeof(float)) == 0);
    CHECK(rendered.object_frames == su_audio_events_clock(rendered.mix.events));
    CHECK(rendered.object_hash != 0u);
    CHECK(su_audio_events_checkpoint_save(rendered.mix.events, blob, sizeof blob, &written) == SU_OK);
    CHECK(su_audio_events_checkpoint_load(restored.mix.events, blob, written) == SU_OK);
    memcpy(restored.handles, rendered.handles, sizeof restored.handles);
    restored.posted = rendered.posted;
    restored.mix.seq = rendered.mix.seq;
    rendered.object_hash = 0u;
    rendered.object_frames = 0u;
    script_run(&rendered, 9001u, FULL_FRAMES, full_a);
    script_run(&restored, 9001u, FULL_FRAMES, full_b);
    CHECK(memcmp(full_a + 18002u, full_b + 18002u, (FULL_FRAMES - 9001u) * 2u * sizeof(float)) == 0);
    CHECK(rendered.object_hash == restored.object_hash);
    CHECK(rendered.object_frames == restored.object_frames);
    CHECK(su_audio_events_checkpoint_load(plain.mix.events, blob, written) == SU_E_SCHEMA_MISMATCH);
    full_destroy(&plain);
    full_destroy(&rendered);
    full_destroy(&restored);
}

/* Clustering consumes voice identity and pre-master buses without changing the stereo output. */
static void test_cluster_feed(void) {
    FullRig plain;
    FullRig clustered;
    SuAudioClusterStats stats;
    build_script();
    full_create(&plain, 0, 3u);
    full_create(&clustered, 1, 3u);
    script_run(&plain, 0u, FULL_FRAMES, full_a);
    script_run(&clustered, 0u, FULL_FRAMES, full_b);
    CHECK(memcmp(full_a, full_b, sizeof full_a) == 0);
    su_audio_cluster_stats(clustered.cluster, &stats);
    CHECK(stats.clock == FULL_FRAMES);
    CHECK(stats.beds >= 3u);           /* the three pre-master buses */
    CHECK(stats.created > 0u && stats.joins > 0u);
    CHECK(stats.outputs <= 2u || stats.infeasible != 0u);
    full_destroy(&plain);
    full_destroy(&clustered);
}

/* A direct checkpoint taken at any position, loaded into a fresh runtime, continues exactly like the
 * uninterrupted render; replaying the script from the start to the same position (the verifier path) agrees
 * too. Checkpoint size does not grow with position. */
static void test_checkpoints(void) {
    static const uint32_t positions[] = {1u, 127u, 128u, 4000u, 9001u, 15000u, 20000u, 25555u, 30001u,
                                         33333u, 40000u, 47000u};
    FullRig reference;
    size_t smallest = (size_t)-1;
    size_t largest = 0u;
    uint32_t p;
    build_script();
    full_create(&reference, 1, 3u);
    script_run(&reference, 0u, FULL_FRAMES, full_a);
    {
        SuAudioVoiceStats stats;
        su_audio_voice_pool_stats(reference.mix.pool, &stats);
        CHECK(stats.stolen > 0u && stats.virtual_peak > 0u);
    }
    for (p = 0; p < sizeof positions / sizeof positions[0]; p++) {
        FullRig replay;
        FullRig restored;
        size_t size = 0u;
        size_t written = 0u;
        uint32_t position = positions[p];
        full_create(&replay, 1, 3u);
        script_run(&replay, 0u, position, full_b);
        CHECK(memcmp(full_a, full_b, (size_t)position * 2u * sizeof(float)) == 0);
        CHECK(su_audio_events_checkpoint_save(replay.mix.events, NULL, 0u, &size) == SU_OK);
        CHECK(size <= sizeof blob);
        CHECK(su_audio_events_checkpoint_save(replay.mix.events, blob, sizeof blob, &written) == SU_OK);
        CHECK(written == size);
        smallest = written < smallest ? written : smallest;
        largest = written > largest ? written : largest;
        full_create(&restored, 1, 3u);
        CHECK(su_audio_events_checkpoint_load(restored.mix.events, blob, written) == SU_OK);
        /* the game restores its own presentation state: handles and the commit sequence */
        memcpy(restored.handles, replay.handles, sizeof restored.handles);
        restored.posted = replay.posted;
        restored.mix.seq = replay.mix.seq;
        CHECK(su_audio_events_clock(restored.mix.events) == su_audio_events_clock(replay.mix.events));
        script_run(&restored, position, FULL_FRAMES, full_b);
        CHECK(memcmp(full_a + (size_t)position * 2u, full_b + (size_t)position * 2u,
            (size_t)(FULL_FRAMES - position) * 2u * sizeof(float)) == 0);
        /* the replayed runtime continues identically as well */
        script_run(&replay, position, FULL_FRAMES, full_b);
        CHECK(memcmp(full_a, full_b, sizeof full_a) == 0);
        full_destroy(&replay);
        full_destroy(&restored);
    }
    /* sizes vary only with live voices and instances, never with elapsed time */
    CHECK(largest < 2u * smallest);
    {
        FullRig other;
        size_t written = 0u;
        CHECK(su_audio_events_checkpoint_save(reference.mix.events, blob, sizeof blob, &written) == SU_OK);
        full_create(&other, 1, 4u); /* different physical voice count: fingerprint differs */
        CHECK(su_audio_events_checkpoint_load(other.mix.events, blob, written) == SU_E_SCHEMA_MISMATCH);
        blob[written / 2u] ^= 1u;   /* corruption: checksum */
        CHECK(su_audio_events_checkpoint_load(reference.mix.events, blob, written) == SU_E_SCHEMA_MISMATCH);
        full_destroy(&other);
    }
    {
        Rig legacy;
        RigOptions options = default_options(0);
        size_t written = 0u;
        rig_create(&legacy, &options);
        CHECK(su_audio_events_checkpoint_save(legacy.events, NULL, 0u, &written) == SU_E_UNSUPPORTED_METHOD);
        rig_destroy(&legacy);
    }
    full_destroy(&reference);
}

static void test_shared_spatial_controls(void) {
    uint32_t mode;
    for (mode = 0u; mode < 8u; mode++) {
        Rig rig;
        RigOptions options = default_options(1);
        SuAudioEmitterSnapshot emitter;
        SuAudioEmitter collision = 0u;
        uint32_t index;
        options.physical = mode < 4u ? 2u : 16u;
        options.virtual_mode = mode % 4u;
        options.lowpass_far = 1500.0f;
        options.emitter_capacity = 264u;
        options.doppler = 1.0f;
        rig_create(&rig, &options);
        CHECK(su_audio_emitter_get(rig.scene, rig.near_emitter, &emitter) == SU_OK);
        for (index = 0u; index < 255u; index++) {
            CHECK(su_audio_emitter_create(rig.scene, &emitter, &collision) == SU_OK);
        }
        CHECK((uint32_t)collision % 256u == (uint32_t)rig.near_emitter % 256u);
        for (index = 0u; index < 12u; index++) {
            (void)post(&rig, index < 6u ? 2u : index % 2u ? 1u : 3u, 0u,
                index < 4u ? rig.near_emitter : index < 8u ? collision : 0u);
        }
        for (index = 0u; index < 24u; index++) {
            static uint8_t checkpoint[131072];
            size_t written = 0u;
            SuVec3f velocity = {4.0f, 0.0f, -3.0f};
            SuAudioCameraListener listener;
            CHECK(su_audio_emitter_update(rig.scene, rig.near_emitter,
                meters((double)index * 2.0 - 20.0, -4.0), velocity) == SU_OK);
            CHECK(su_audio_scene_listener_get(rig.scene, &listener) == SU_OK);
            listener.pos = meters((double)index * 0.5, 0.0);
            CHECK(su_audio_scene_listener_set(rig.scene, &listener) == SU_OK);
            render_chunks(&rig, out_a, 128u, 61u);
            CHECK(su_audio_events_checkpoint_save(rig.events, checkpoint, sizeof checkpoint, &written) == SU_OK);
        }
        rig_destroy(&rig);
    }
}

int main(int argc, char **argv) {
    if (argc > 1) {
        evidence = fopen(argv[1], "wb");
        if (evidence == NULL) {
            return 1;
        }
    }
    synthesize();
    test_authored_loudness();
    test_object_output();
    test_shared_spatial_controls();
    test_cluster_feed();
    test_checkpoints();
    test_switch_blend();
    test_voice_bindings();
    test_buses_music_limiter();
    test_attach_validation();
    test_legacy_equivalence();
    test_pitch();
    test_lowpass();
    test_steal_resume();
    test_stop_timing();
    if (evidence != NULL && fclose(evidence) != 0) {
        failures++;
    }
    printf("audio_runtime: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
