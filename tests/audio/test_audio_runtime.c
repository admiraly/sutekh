/* Integrated event runtime: committed events drive the A05 voice pool with pitch and per-voice spatial
 * filtering. Every sample is synthesized here. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/audio_event.h"

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
    (void)post(&rig, 1u, 0u, 0u);
    CHECK(su_audio_events_attach(rig.events, &hook) == SU_E_RESOURCE_LIMIT);
    rig_destroy(&rig);
    su_audio_voice_pool_destroy(pool);
}

int main(void) {
    synthesize();
    test_attach_validation();
    test_legacy_equivalence();
    test_pitch();
    test_lowpass();
    test_steal_resume();
    test_stop_timing();
    printf("audio_runtime: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
