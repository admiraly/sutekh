/* Combined audio system: offline and real-time (ring) output paths, journal replay, checkpoint verification,
 * control-plane methods and steady-state allocations. Every sample is synthesized here. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/audio_system.h"
#include "ir/su_sha256.h"
#include "platform/su_platform.h"
#include "platform/su_thread.h"
#include <stdatomic.h>

#ifdef SU_AUDIO_SYSTEM_ALLOC_TEST
size_t sys_allocations;
void *sys_malloc(size_t size);
void *sys_calloc(size_t count, size_t size);
void *sys_realloc(void *memory, size_t size);
void sys_free(void *memory);
void *sys_malloc(size_t size) {
    sys_allocations++;
    return malloc(size);
}
void *sys_calloc(size_t count, size_t size) {
    sys_allocations++;
    return calloc(count, size);
}
void *sys_realloc(void *memory, size_t size) {
    sys_allocations++;
    return realloc(memory, size);
}
void sys_free(void *memory) {
    free(memory);
}
#endif

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
#define FRAMES 96000u
#define TONE 4800u
#define LOOP 2400u

static const char graph_json[] =
    "{\"schema\":\"su_audio_events_v1\",\"numeric_profile\":\"audio_float_local_v1\","
    "\"seed_policy\":\"event_emitter_sequence_v1\",\"sample_rate\":48000,\"nodes\":["
    "{\"id\":1,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":1},\"bus_id\":10},"
    "{\"id\":2,\"type\":\"sound\",\"asset\":{\"id\":2,\"generation\":2},\"bus_id\":30,\"flags\":1},"
    "{\"id\":3,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":1},\"bus_id\":20,\"pitch\":0.75},"
    "{\"id\":4,\"type\":\"random\",\"children\":[{\"node\":1,\"weight\":2},{\"node\":3,\"weight\":1}]}],"
    "\"events\":[{\"id\":1,\"root\":4},{\"id\":2,\"root\":2}]}";

static float tone[TONE];
static float loop_tone[LOOP];
#ifndef SU_AUDIO_SYSTEM_ALLOC_TEST
static float offline_pcm[FRAMES * 2u];
static float ring_pcm[FRAMES * 2u];
static char text[1u << 18];
#endif
static uint8_t blob[1u << 20];

static void synthesize(void) {
    uint32_t index;
    for (index = 0; index < TONE; index++) {
        double env = exp(-(double)index / 1200.0);
        tone[index] = (float)(0.7 * env * sin(2.0 * 3.14159265358979 * 523.25 * index / RATE));
    }
    for (index = 0; index < LOOP; index++) {
        loop_tone[index] = (float)(0.3 * sin(2.0 * 3.14159265358979 * 200.0 * index / RATE));
    }
}

static SuWorldPos meters(double x, double z) {
    SuWorldPos pos;
    pos.x = (int64_t)(x * 4294967296.0);
    pos.y = 0;
    pos.z = (int64_t)(z * 4294967296.0);
    return pos;
}

typedef struct Setup {
    SuAudioAssets *assets;
    SuAudioGraph *graph;
    SuAudioCurvePoint points[2];
    SuAudioCurveDesc curve;
    SuAudioBusDesc buses[4];
    SuAudioDuckRule duck;
    SuAudioBusTreeDesc tree;
    SuAudioParamDef param;
    SuAudioBindingPoint binding_points[2];
    SuAudioParamBinding binding;
    SuAudioParamsDesc params;
    SuAudioClusterDesc cluster;
    SuAudioVoicePolicyRule rules[2];
    SuAudioSystemDesc desc;
} Setup;

static void setup_init(Setup *s, uint32_t output) {
    SuAudioGraphLimits limits = {16u, 32u, 8u, 16u, 8192u};
    SuAudioGraphDiagnostic diagnostic;
    SuAudioClip clip;
    SuAudioAssetRef ref;
    uint32_t index;
    memset(s, 0, sizeof *s);
    CHECK(su_audio_assets_create(4u, 1u << 20, &s->assets) == SU_OK);
    clip.channels = 1u;
    clip.sample_rate = RATE;
    clip.samples = tone;
    clip.frames = TONE;
    CHECK(su_audio_asset_prepare(s->assets, 1u, &clip, &ref) == SU_OK);
    clip.samples = loop_tone;
    clip.frames = LOOP;
    CHECK(su_audio_asset_prepare(s->assets, 2u, &clip, &ref) == SU_OK);
    CHECK(su_audio_graph_prepare(graph_json, sizeof graph_json - 1u, s->assets, &limits, &s->graph, &diagnostic)
        == SU_OK);
    s->points[0].distance_m = 1.0f;
    s->points[0].lowpass_hz = 24000.0f;
    s->points[0].scale = 1.0f;
    s->points[1].distance_m = 50.0f;
    s->points[1].gain_db = -20.0f;
    s->points[1].lowpass_hz = 3000.0f;
    s->points[1].scale = 1.0f;
    s->curve.struct_size = sizeof s->curve;
    s->curve.points = s->points;
    s->curve.point_count = 2u;
    s->buses[0].id = 1u;
    s->buses[0].parent = SU_AUDIO_BUS_ROOT;
    s->buses[1].id = 10u;
    s->buses[2].id = 20u;
    s->buses[3].id = 30u;
    for (index = 1u; index < 4u; index++) {
        s->buses[index].parent = 1u;
    }
    s->duck.source = 10u;
    s->duck.target = 30u;
    s->duck.threshold_db = -30.0f;
    s->duck.ratio = 4.0f;
    s->duck.attack_ms = 5.0f;
    s->duck.hold_ms = 30.0f;
    s->duck.release_ms = 150.0f;
    s->duck.max_attenuation_db = 9.0f;
    s->tree.struct_size = sizeof s->tree;
    s->tree.sample_rate = RATE;
    s->tree.max_block_frames = 128u;
    s->tree.buses = s->buses;
    s->tree.bus_count = 4u;
    s->tree.rules = &s->duck;
    s->tree.rule_count = 1u;
    s->param.id = 1u;
    s->param.max_value = 1.0f;
    s->param.smoothing_frames = 960u;
    s->binding_points[1].value = 1.0f;
    s->binding_points[1].output = -12.0f;
    s->binding.param_id = 1u;
    s->binding.control = SU_AUDIO_CONTROL_BUS_GAIN_DB;
    s->binding.bus_id = 30u;
    s->binding.points = s->binding_points;
    s->binding.point_count = 2u;
    s->params.struct_size = sizeof s->params;
    s->params.sample_rate = RATE;
    s->params.params = &s->param;
    s->params.param_count = 1u;
    s->params.bindings = &s->binding;
    s->params.binding_count = 1u;
    s->params.max_scoped_values = 8u;
    s->cluster.struct_size = sizeof s->cluster;
    s->cluster.width_rad = 1.0f;
    s->cluster.leave_ratio = 1.25f;
    s->cluster.hold_frames = 1024u;
    s->cluster.crossfade_frames = 256u;
    s->cluster.output_limit = 2u;
    s->cluster.max_objects = 32u;
    s->rules[0].bus_id = 10u;
    s->rules[0].policy.physical_limit = 2u;
    s->rules[0].policy.limit_scope = SU_AUDIO_LIMIT_BUS;
    s->rules[0].policy.release_frames = 64u;
    s->desc.struct_size = sizeof s->desc;
    s->desc.output = output;
    s->desc.ring_frames = 1024u;
    s->desc.assets = s->assets;
    s->desc.graph = s->graph;
    s->desc.events.struct_size = sizeof s->desc.events;
    s->desc.events.sample_rate = RATE;
    s->desc.events.max_instances = 32u;
    s->desc.events.max_leaves_per_instance = 2u;
    s->desc.events.max_logical_voices = 64u;
    s->desc.events.command_capacity = 16u;
    s->desc.events.project_seed = 31u;
    s->desc.events.session_generation = 1u;
    s->desc.scene.struct_size = sizeof s->desc.scene;
    s->desc.scene.max_emitters = 8u;
    s->desc.scene.max_curves = 2u;
    s->desc.curves = &s->curve;
    s->desc.curve_count = 1u;
    s->desc.voices.struct_size = sizeof s->desc.voices;
    s->desc.voices.sample_rate = RATE;
    s->desc.voices.max_logical = 64u;
    s->desc.voices.max_physical = 6u;
    s->desc.voices.transition_reserve = 4u;
    s->desc.policies = s->rules;
    s->desc.policy_count = 1u;
    s->desc.default_policy.release_frames = 64u;
    s->desc.buses = &s->tree;
    s->desc.params = &s->params;
    s->desc.cluster = &s->cluster;
    s->desc.cluster_inputs = SU_AUDIO_EVENTS_CLUSTER_VOICES | SU_AUDIO_EVENTS_CLUSTER_BUSES;
    s->desc.limiter_knee = 0.9f;
    s->desc.journal_capacity = 256u;
    s->desc.telemetry_capacity = 256u;
}

static void setup_free(Setup *s) {
    su_audio_graph_release(s->graph);
    CHECK(su_audio_assets_destroy(s->assets) == SU_OK);
}

typedef struct Session {
    SuAudioEmitter emitters[3];
    SuAudioInstance ambience;
    uint64_t seq;
} Session;

/* Game-side presentation commands, issued when the system has produced `position` frames. */
static void issue(SuAudioSystem *system, Session *session, uint64_t position) {
    SuAudioTrigger trigger;
    SuAudioInstance instance = 0u;
    uint64_t now = su_audio_system_clock(system);
    memset(&trigger, 0, sizeof trigger);
    trigger.struct_size = sizeof trigger;
    trigger.session_generation = 1u;
    if (position == 0u) {
        SuAudioEmitterSnapshot emitter;
        SuAudioCameraListener listener;
        SuAudioParamScope global = {SU_AUDIO_SCOPE_GLOBAL, 0u};
        uint32_t index;
        memset(&emitter, 0, sizeof emitter);
        emitter.curve = 1u;
        emitter.scale = 1.0f;
        for (index = 0; index < 3u; index++) {
            emitter.pos = meters(-15.0 + 15.0 * index, -4.0 - 6.0 * index);
            CHECK(su_audio_system_emitter_create(system, &emitter, &session->emitters[index]) == SU_OK);
        }
        memset(&listener, 0, sizeof listener);
        listener.rot.w = 1.0f;
        CHECK(su_audio_system_listener_set(system, &listener) == SU_OK);
        trigger.event_id = 2u;
        trigger.sample_time = now;
        trigger.commit_seq = session->seq++;
        CHECK(su_audio_system_post(system, &trigger, &session->ambience, NULL) == SU_OK);
        CHECK(su_audio_system_param_set(system, global, 1u, 1.0f, 60000u) == SU_OK);
        return;
    }
    if (position % 2048u == 0u && position < 80000u) {
        trigger.event_id = 1u;
        trigger.emitter = session->emitters[(position / 2048u) % 3u];
        trigger.sample_time = now + 37u;
        trigger.commit_seq = session->seq++;
        CHECK(su_audio_system_post(system, &trigger, &instance, NULL) == SU_OK);
    }
    if (position == 40960u) {
        SuVec3f velocity = {0.0f, 0.0f, 0.0f};
        CHECK(su_audio_system_emitter_update(system, session->emitters[2], meters(3.0, -2.0), velocity) == SU_OK);
    }
    if (position == 81920u) {
        CHECK(su_audio_system_stop(system, SU_AUDIO_SYSTEM_CMD_STOP_INSTANCE, session->ambience, now + 500u, 2000u)
            == SU_OK);
    }
}

#ifndef SU_AUDIO_SYSTEM_ALLOC_TEST
/* Offline path: commands at quantum positions, rendered in odd chunks. */
static void run_offline(SuAudioSystem *system, Session *session, float *out, uint64_t frames) {
    uint64_t position = 0u;
    while (position < frames) {
        uint32_t take = 128u;
        issue(system, session, position);
        CHECK(su_audio_system_render(system, out + position * 2u, take) == SU_OK);
        position += take;
    }
}

#endif

/* One game frame of presentation commands, issued at the current render clock. */
static void game_frame(SuAudioSystem *system, Session *session, uint32_t frame) {
    SuAudioTrigger trigger;
    SuAudioInstance instance = 0u;
    uint64_t now = su_audio_system_clock(system);
    if (frame == 0u) {
        issue(system, session, 0u);
        return;
    }
    memset(&trigger, 0, sizeof trigger);
    trigger.struct_size = sizeof trigger;
    trigger.session_generation = 1u;
    if (frame % 5u == 0u) {
        trigger.event_id = 1u;
        trigger.emitter = session->emitters[(frame / 5u) % 3u];
        trigger.sample_time = now + 37u;
        trigger.commit_seq = session->seq++;
        CHECK(su_audio_system_post(system, &trigger, &instance, NULL) == SU_OK);
    }
    if (frame == 100u) {
        SuVec3f velocity = {0.0f, 0.0f, 0.0f};
        CHECK(su_audio_system_emitter_update(system, session->emitters[2], meters(3.0, -2.0), velocity) == SU_OK);
    }
    if (frame == 180u) {
        CHECK(su_audio_system_stop(system, SU_AUDIO_SYSTEM_CMD_STOP_INSTANCE, session->ambience, now + 500u, 2000u)
            == SU_OK);
    }
}

#ifndef SU_AUDIO_SYSTEM_ALLOC_TEST
/* Real-time path: per game frame the owner issues commands and updates the render-ahead ring; a consumer reads
 * the ring in device-sized chunks (what the device thread does). */
static void run_ring(SuAudioSystem *system, Session *session, float *out, uint64_t frames) {
    uint64_t consumed = 0u;
    uint32_t frame = 0u;
    uint32_t chunk = 441u;
    while (consumed < frames) {
        uint32_t take = (uint32_t)(frames - consumed < chunk ? frames - consumed : chunk);
        uint32_t got;
        game_frame(system, session, frame++);
        CHECK(su_audio_system_update(system) == SU_OK);
        got = su_audio_system_ring_read(system, out + consumed * 2u, take);
        CHECK(got == take);
        consumed += got;
        chunk = chunk == 441u ? 480u : chunk == 480u ? 97u : 441u;
    }
}

/* The real-time ring stream and the offline journal replay are the same PCM. */
static void test_paths_identical(void) {
    Setup s;
    SuAudioSystem *ring = NULL;
    SuAudioSystem *replayed = NULL;
    Session b;
    SuAudioSystemStats stats;
    const SuAudioSystemCommand *journal = NULL;
    uint32_t count = 0u;
    char message[256];
    setup_init(&s, SU_AUDIO_OUTPUT_RING);
    CHECK(su_audio_system_create(&s.desc, &ring, message) == SU_OK);
    memset(&b, 0, sizeof b);
    run_ring(ring, &b, ring_pcm, FRAMES);
    su_audio_system_stats(ring, &stats);
    CHECK(stats.underrun_frames == 0u && stats.consumed_frames == FRAMES);
    CHECK(su_audio_system_journal(ring, &journal, &count) == SU_OK && count > 40u);
    s.desc.output = SU_AUDIO_OUTPUT_OFFLINE;
    CHECK(su_audio_system_create(&s.desc, &replayed, message) == SU_OK);
    CHECK(su_audio_system_replay(replayed, journal, count, FRAMES, offline_pcm) == SU_OK);
    CHECK(memcmp(offline_pcm, ring_pcm, sizeof offline_pcm) == 0);
    {
        /* the slice exercises every wired component */
        SuAudioVoiceStats voices;
        SuAudioClusterStats cluster;
        SuAudioBusMeter meter;
        su_audio_voice_pool_stats(su_audio_system_voices(replayed), &voices);
        su_audio_cluster_stats(su_audio_system_cluster(replayed), &cluster);
        CHECK(voices.reasons[SU_AUDIO_REASON_SCOPE_LIMIT] > 0u);
        CHECK(cluster.objects > 0u && cluster.clock == FRAMES);
        CHECK(su_audio_bus_meter_read(su_audio_system_buses(replayed), 30u, &meter, 0) == SU_OK);
        CHECK(fabsf(meter.fader_db + 12.0f) < 0.01f);
    }
    /* reading past the rendered audio delivers silence and counts it */
    {
        float spill[256];
        SuAudioSystemStats after;
        uint32_t got = su_audio_system_ring_read(ring, spill, 128u);
        su_audio_system_stats(ring, &after);
        CHECK(after.underrun_frames == 128u - got);
    }
    su_audio_system_destroy(replayed);
    su_audio_system_destroy(ring);
    setup_free(&s);
}

static void test_checkpoint_verify(void) {
    Setup s;
    SuAudioSystem *system = NULL;
    Session a;
    Session saved;
    const SuAudioSystemCommand *journal = NULL;
    uint32_t count = 0u;
    size_t written = 0u;
    uint64_t position;
    setup_init(&s, SU_AUDIO_OUTPUT_OFFLINE);
    CHECK(su_audio_system_create(&s.desc, &system, NULL) == SU_OK);
    memset(&a, 0, sizeof a);
    run_offline(system, &a, offline_pcm, 30080u);
    CHECK(su_audio_system_checkpoint_save(system, blob, sizeof blob, &written) == SU_OK);
    saved = a; /* the game keeps its own presentation state with the checkpoint */
    for (position = 30080u; position < FRAMES; position += 128u) {
        issue(system, &a, position);
        CHECK(su_audio_system_render(system, offline_pcm + position * 2u, 128u) == SU_OK);
    }
    CHECK(su_audio_system_journal(system, &journal, &count) == SU_OK);
    CHECK(su_audio_system_checkpoint_verify(&s.desc, journal, count, blob, written, 48000u) == SU_OK);
    {
        /* a direct restore continues exactly like the uninterrupted render */
        SuAudioSystem *restored = NULL;
        Session b = saved;
        CHECK(su_audio_system_create(&s.desc, &restored, NULL) == SU_OK);
        CHECK(su_audio_system_checkpoint_load(restored, blob, written) == SU_OK);
        CHECK(su_audio_system_clock(restored) == 30080u);
        for (position = 30080u; position < FRAMES; position += 128u) {
            issue(restored, &b, position);
            CHECK(su_audio_system_render(restored, ring_pcm + position * 2u, 128u) == SU_OK);
        }
        CHECK(memcmp(offline_pcm + 30080u * 2u, ring_pcm + 30080u * 2u, (FRAMES - 30080u) * 2u * sizeof(float)) == 0);
        su_audio_system_destroy(restored);
    }
    blob[written - 9u] ^= 0x40u;
    CHECK(su_audio_system_checkpoint_verify(&s.desc, journal, count, blob, written, 4800u) == SU_E_SCHEMA_MISMATCH);
    su_audio_system_destroy(system);
    setup_free(&s);
}

static void test_control_methods(void) {
    Setup s;
    SuAudioSystem *system = NULL;
    Session a;
    size_t written = 0u;
    uint32_t index;
    setup_init(&s, SU_AUDIO_OUTPUT_OFFLINE);
    CHECK(su_audio_system_create(&s.desc, &system, NULL) == SU_OK);
    memset(&a, 0, sizeof a);
    run_offline(system, &a, offline_pcm, 24064u);
    for (index = 0; su_audio_system_method(index) != NULL; index++) {
    }
    CHECK(index == 4u && strcmp(su_audio_system_method(3u), "audio.render.offline") == 0);
    CHECK(su_audio_system_control(system, "audio.inspect", NULL, 0u, text, sizeof text, &written) == SU_OK);
    CHECK(written == strlen(text) && strstr(text, "\"schema\":\"su_audio_system_inspect_v1\"") != NULL);
    CHECK(strstr(text, "\"buses\":[") != NULL || strstr(text, "\"buses\":{") != NULL);
    CHECK(strstr(text, "su_audio_cluster_v1") != NULL);
    CHECK(su_audio_system_control(system, "audio.trace.stats", NULL, 0u, text, sizeof text, &written) == SU_OK);
    CHECK(strstr(text, "\"recorded\":") != NULL && strstr(text, "\"ok\":") != NULL);
    CHECK(su_audio_system_control(system, "audio.trace.read", NULL, 0u, text, sizeof text, &written) == SU_OK);
    CHECK(strstr(text, "\"kind\"") != NULL || strstr(text, "kind") != NULL);
    {
        static const char params[] = "{\"frames\": 24064}";
        SuSha256 sha;
        uint8_t digest[32];
        char hex[65];
        su_sha256_init(&sha);
        su_sha256_update(&sha, offline_pcm, 24064u * 2u * sizeof(float));
        su_sha256_final(&sha, digest);
        su_sha256_hex(digest, hex);
        CHECK(su_audio_system_control(system, "audio.render.offline", params, sizeof params - 1u, text, sizeof text,
            &written) == SU_OK);
        CHECK(strstr(text, hex) != NULL && strstr(text, "\"frames\":24064") != NULL);
        CHECK(strstr(text, "\"integrated_lufs\":-") != NULL);
    }
    CHECK(su_audio_system_control(system, "audio.unknown", NULL, 0u, text, sizeof text, &written)
        == SU_E_UNSUPPORTED_METHOD);
    CHECK(su_audio_system_control(system, "audio.inspect", NULL, 0u, text, 16u, &written) == SU_E_RESOURCE_LIMIT);
    su_audio_system_destroy(system);
    setup_free(&s);
}

/* Sum groups: bindings with one sum_group add their outputs (linear gain, pitch terms, hertz) before joining the
 * rest; a grouped BUS_GAIN_DB binding is refused. */
static void test_sum_groups(void) {
    SuAudioParamDef defs[2];
    SuAudioBindingPoint pa[2], pb[2], la[2], lb[2], qa[2], qb[2];
    SuAudioParamBinding b[6];
    SuAudioParamsDesc d;
    SuAudioParams *params = NULL;
    SuAudioParamControls c;
    SuAudioParamTarget t = {0u, 0u};
    SuAudioParamScope g = {SU_AUDIO_SCOPE_GLOBAL, 0u};
    uint32_t i;
    memset(defs, 0, sizeof defs);
    defs[0].id = 1u;
    defs[0].max_value = 1.0f;
    defs[1].id = 2u;
    defs[1].max_value = 1.0f;
    /* gain = 0.1 + 0.4 p1 + 0.5 p2; pitch = 0.8 + 0.4 p1 + 0 p2; low-pass = 1000 + 9000 p1 + 2000 p2 */
    pa[0].value = 0.0f; pa[0].output = 0.1f; pa[1].value = 1.0f; pa[1].output = 0.5f;
    pb[0].value = 0.0f; pb[0].output = 0.0f; pb[1].value = 1.0f; pb[1].output = 0.5f;
    qa[0].value = 0.0f; qa[0].output = 0.8f; qa[1].value = 1.0f; qa[1].output = 1.2f;
    qb[0].value = 0.0f; qb[0].output = 0.0f; qb[1].value = 1.0f; qb[1].output = 0.0f;
    la[0].value = 0.0f; la[0].output = 1000.0f; la[1].value = 1.0f; la[1].output = 10000.0f;
    lb[0].value = 0.0f; lb[0].output = 0.0f; lb[1].value = 1.0f; lb[1].output = 2000.0f;
    memset(b, 0, sizeof b);
    b[0].param_id = 1u; b[0].control = SU_AUDIO_CONTROL_GAIN_DB; b[0].points = pa; b[0].point_count = 2u; b[0].sum_group = 7u;
    b[1].param_id = 2u; b[1].control = SU_AUDIO_CONTROL_GAIN_DB; b[1].points = pb; b[1].point_count = 2u; b[1].sum_group = 7u;
    b[2].param_id = 1u; b[2].control = SU_AUDIO_CONTROL_PITCH; b[2].points = qa; b[2].point_count = 2u; b[2].sum_group = 7u;
    b[3].param_id = 2u; b[3].control = SU_AUDIO_CONTROL_PITCH; b[3].points = qb; b[3].point_count = 2u; b[3].sum_group = 7u;
    b[4].param_id = 1u; b[4].control = SU_AUDIO_CONTROL_LOWPASS_HZ; b[4].points = la; b[4].point_count = 2u; b[4].sum_group = 3u;
    b[5].param_id = 2u; b[5].control = SU_AUDIO_CONTROL_LOWPASS_HZ; b[5].points = lb; b[5].point_count = 2u; b[5].sum_group = 3u;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.sample_rate = RATE;
    d.params = defs;
    d.param_count = 2u;
    d.bindings = b;
    d.binding_count = 6u;
    d.max_scoped_values = 4u;
    CHECK(su_audio_params_create(&d, &params) == SU_OK);
    CHECK(su_audio_param_set_at(params, g, 1u, 0.5f, 0u) == SU_OK);
    CHECK(su_audio_param_set_at(params, g, 2u, 1.0f, 0u) == SU_OK);
    CHECK(su_audio_param_controls(params, t, 10u, &c) == SU_OK);
    CHECK(fabsf(c.gain_db - 20.0f * log10f(0.8f)) < 1e-3f); /* 0.1 + 0.2 + 0.5 */
    CHECK(fabsf(c.pitch - 1.0f) < 1e-5f);                   /* 0.8 + 0.2 + 0 */
    CHECK(fabsf(c.lowpass_hz - 7500.0f) < 0.5f);            /* 1000 + 4500 + 2000 */
    su_audio_params_destroy(params);
    b[1].control = SU_AUDIO_CONTROL_BUS_GAIN_DB;
    b[1].bus_id = 30u;
    CHECK(su_audio_params_create(&d, &params) != SU_OK && params == NULL);
    for (i = 0; i < 6u; i++) {
        b[i].sum_group = 0u;
    }
    printf("audio_system: sum groups add gain, pitch and low-pass terms\n");
}

/* PULL render: a reader thread paced like a device (480 frames every 10 ms) renders what it reads; the owner issues
 * its game frames under the system lock and stalls for 150 ms once. The reader never underruns, and the stream is
 * the journal replay bit for bit. */
typedef struct PullReader {
    SuAudioSystem *system;
    float *out;
    uint32_t frames;
    _Atomic uint32_t done;
    uint32_t short_reads;
} PullReader;

static void *pull_reader_main(void *arg) {
    PullReader *reader = (PullReader *)arg;
    uint64_t start = 0u;
    uint32_t position = 0u;
    (void)su_platform_monotonic_ns(&start);
    while (position < reader->frames) {
        uint32_t take = reader->frames - position < 480u ? reader->frames - position : 480u;
        uint64_t now = 0u;
        uint64_t due = start + (uint64_t)(position / 480u) * 10000000u;
        do {
            (void)su_platform_monotonic_ns(&now);
        } while (now < due);
        reader->short_reads += su_audio_system_ring_read(reader->system, reader->out + (size_t)position * 2u, take)
            != take;
        position += take;
    }
    atomic_store(&reader->done, 1u);
    return NULL;
}

static void test_pull_render(void) {
    Setup s;
    SuAudioSystem *system = NULL;
    SuAudioSystem *replayed = NULL;
    SuAudioSystemStats stats;
    const SuAudioSystemCommand *journal = NULL;
    uint32_t count = 0u;
    PullReader reader;
    SuThread thread;
    Session a;
    uint32_t frame = 0u;
    const uint32_t frames = 48000u;
    setup_init(&s, SU_AUDIO_OUTPUT_RING);
    s.desc.render = SU_AUDIO_RENDER_PULL;
    s.desc.ring_frames = 1024u;
    s.desc.journal_capacity = 4096u;
    CHECK(su_audio_system_create(&s.desc, &system, NULL) == SU_OK);
    memset(&a, 0, sizeof a);
    memset(&reader, 0, sizeof reader);
    reader.system = system;
    reader.out = ring_pcm;
    reader.frames = frames;
    su_audio_system_lock(system);
    game_frame(system, &a, frame++);
    su_audio_system_unlock(system);
    CHECK(su_audio_system_update(system) == SU_OK); /* a no-op in PULL mode */
    CHECK(su_thread_create(&thread, pull_reader_main, &reader) == 0);
    while (!atomic_load(&reader.done)) {
        su_audio_system_lock(system);
        if (su_audio_system_clock(system) + 4800u < frames) { /* stop posting near the end of the read */
            game_frame(system, &a, frame);
        }
        su_audio_system_unlock(system);
        frame++;
        su_platform_sleep_ms(frame == 10u ? 150u : 16u); /* one long owner stall */
    }
    CHECK(su_thread_join(thread) == 0);
    su_audio_system_stats(system, &stats);
    CHECK(reader.short_reads == 0u && stats.underrun_frames == 0u);
    /* the owner frame count depends on scheduling (a loaded host sleeps longer); any posted frame proves the path */
    CHECK(su_audio_system_journal(system, &journal, &count) == SU_OK && count > 0u);
    s.desc.output = SU_AUDIO_OUTPUT_OFFLINE;
    CHECK(su_audio_system_create(&s.desc, &replayed, NULL) == SU_OK);
    CHECK(su_audio_system_replay(replayed, journal, count, frames, offline_pcm) == SU_OK);
    CHECK(memcmp(offline_pcm, ring_pcm, (size_t)frames * 2u * sizeof(float)) == 0);
    printf("audio_system: PULL render, %u owner frames with a 150 ms stall, 0 underruns, replay identical\n", frame);
    su_audio_system_destroy(replayed);
    su_audio_system_destroy(system);
    setup_free(&s);
}

static void test_device_output(void) {
    Setup s;
    SuAudioSystem *system = NULL;
    char message[256];
    uint32_t status;
    setup_init(&s, SU_AUDIO_OUTPUT_DEVICE);
    message[0] = '\0';
    status = su_audio_system_create(&s.desc, &system, message);
    /* a host without an output device reports it; with one, the device thread reads the ring */
    CHECK(status == SU_OK || status == SU_E_UNSUPPORTED_METHOD);
    if (status == SU_OK) {
        CHECK(su_audio_system_update(system) == SU_OK);
        su_audio_system_destroy(system);
    } else {
        CHECK(message[0] != '\0' && system == NULL);
    }
    setup_free(&s);
}

typedef struct HostInput {
    uint32_t calls;
    uint64_t next_time;
    uint32_t out_of_order;
} HostInput;

static void host_input_render(void *user, float *stereo, uint32_t frames, uint64_t sample_time) {
    HostInput *input = (HostInput *)user;
    uint32_t index;
    input->out_of_order += sample_time != input->next_time || stereo[0] != 0.0f;
    input->next_time = sample_time + frames;
    input->calls++;
    for (index = 0; index < frames * 2u; index++) {
        stereo[index] = 0.25f;
    }
}

/* Positive-going zero crossings of the left channel: the played frequency of a looping tone. */
static uint32_t crossings(const float *pcm, uint32_t frames) {
    uint32_t index;
    uint32_t count = 0u;
    for (index = 1u; index < frames; index++) {
        count += pcm[(index - 1u) * 2u] < 0.0f && pcm[index * 2u] >= 0.0f;
    }
    return count;
}

/* Renders the looping 200 Hz tone (event 2, bus 30) for one second, optionally on an emitter approaching the
 * listener at 34.3 m/s; returns the left-channel RMS and the zero-crossing count. */
static float scoped_db = 0.0f; /* nonzero: param 2 set to it in the voice's instance scope (and -6 dB globally) */

static void render_loop(Setup *s, int spatial, double *rms, uint32_t *count) {
    SuAudioSystem *system = NULL;
    SuAudioTrigger trigger;
    SuAudioInstance instance;
    uint64_t position;
    double sum = 0.0;
    uint32_t index;
    CHECK(su_audio_system_create(&s->desc, &system, NULL) == SU_OK);
    memset(&trigger, 0, sizeof trigger);
    trigger.struct_size = sizeof trigger;
    trigger.event_id = 2u;
    trigger.commit_seq = 1u;
    trigger.session_generation = 1u;
    if (spatial) {
        SuAudioEmitterSnapshot emitter;
        memset(&emitter, 0, sizeof emitter);
        emitter.pos = meters(0.0, -10.0);
        emitter.vel.z = 34.3f; /* toward the listener at the origin */
        emitter.scale = 1.0f;
        CHECK(su_audio_system_emitter_create(system, &emitter, &trigger.emitter) == SU_OK);
    }
    CHECK(su_audio_system_post(system, &trigger, &instance, NULL) == SU_OK);
    if (scoped_db != 0.0f) {
        SuAudioParamScope global = {SU_AUDIO_SCOPE_GLOBAL, 0u};
        SuAudioParamScope mine = {SU_AUDIO_SCOPE_INSTANCE, 0u};
        mine.key = instance;
        CHECK(su_audio_system_param_set(system, global, 2u, -6.0f, 0u) == SU_OK);
        CHECK(su_audio_system_param_set(system, mine, 2u, scoped_db, 0u) == SU_OK);
    }
    for (position = 0u; position < 48000u; position += 4800u) {
        CHECK(su_audio_system_render(system, offline_pcm + position * 2u, 4800u) == SU_OK);
    }
    for (index = 4800u; index < 48000u; index++) {
        sum += (double)offline_pcm[index * 2u] * offline_pcm[index * 2u];
    }
    *rms = sqrt(sum / 43200.0);
    *count = crossings(offline_pcm + 4800u * 2u, 43200u);
    su_audio_system_destroy(system);
}

/* Host bus inputs, Doppler on spatial voices and bus-filtered voice bindings. */
static void test_host_extensions(void) {
    Setup s;
    SuAudioSystem *system = NULL;
    SuAudioBusInput input;
    HostInput host;
    SuAudioParamBinding bindings[2];
    SuAudioBindingPoint quiet[2];
    SuAudioParamDef params[2];
    double plain_rms = 0.0;
    double rms = 0.0;
    uint32_t plain_count = 0u;
    uint32_t count = 0u;
    uint32_t index;
    size_t written = 0u;
    char message[256];

    /* bus input: constant 0.25 into bus 20; called once per quantum in order, mixed through the tree */
    setup_init(&s, SU_AUDIO_OUTPUT_OFFLINE);
    memset(&host, 0, sizeof host);
    input.bus_id = 20u;
    input.render = host_input_render;
    input.user = &host;
    s.desc.inputs = &input;
    s.desc.input_count = 1u;
    CHECK(su_audio_system_create(&s.desc, &system, message) == SU_OK);
    CHECK(su_audio_system_render(system, offline_pcm, 1000u) == SU_OK);
    CHECK(host.calls == 8u && host.out_of_order == 0u);
    for (index = 0; index < 2000u; index++) {
        CHECK(fabsf(offline_pcm[index] - 0.25f) < 1e-6f);
    }
    CHECK(su_audio_system_control(system, "audio.render.offline", "{\"frames\":1000}", 15u, text, sizeof text,
        &written) == SU_OK);
    CHECK(host.calls == 8u); /* the replay ran without the host input */
    su_audio_system_destroy(system);
    input.bus_id = 99u;
    CHECK(su_audio_system_create(&s.desc, &system, message) != SU_OK && system == NULL);
    input.bus_id = 0u;
    input.render = NULL;
    CHECK(su_audio_system_create(&s.desc, &system, message) != SU_OK && system == NULL);
    s.desc.input_count = 0u;
    s.desc.doppler = 1.5f;
    CHECK(su_audio_system_create(&s.desc, &system, message) != SU_OK && system == NULL);
    s.desc.doppler = 0.0f;

    /* SU_AUDIO_LIMIT_EVENT counts per posted event: one physical voice each for events 1 and 2 */
    {
        SuAudioTrigger trigger;
        SuAudioInstance instance;
        SuAudioVoiceStats voices;
        uint32_t posts;
        s.desc.policy_count = 0u;
        s.desc.default_policy.physical_limit = 1u;
        s.desc.default_policy.limit_scope = SU_AUDIO_LIMIT_EVENT;
        CHECK(su_audio_system_create(&s.desc, &system, message) == SU_OK);
        memset(&trigger, 0, sizeof trigger);
        trigger.struct_size = sizeof trigger;
        trigger.session_generation = 1u;
        for (posts = 0; posts < 3u; posts++) {
            trigger.event_id = posts < 2u ? 1u : 2u;
            trigger.commit_seq = posts + 1u;
            CHECK(su_audio_system_post(system, &trigger, &instance, NULL) == SU_OK);
        }
        CHECK(su_audio_system_render(system, offline_pcm, 256u) == SU_OK);
        su_audio_voice_pool_stats(su_audio_system_voices(system), &voices);
        CHECK(voices.logical == 3u && voices.physical == 2u);
        su_audio_system_destroy(system);
        s.desc.policy_count = 1u;
        s.desc.default_policy.physical_limit = 0u;
        s.desc.default_policy.limit_scope = 0u;
    }

    /* an emitter exactly to the right: the left gain rounds to a tiny negative (cosf(pi/2)); it must be clamped, not
     * leave the voice uncontrolled (unit gain, centred) */
    {
        SuAudioTrigger trigger;
        SuAudioInstance instance;
        SuAudioEmitterSnapshot emitter;
        double left = 0.0, right = 0.0;
        CHECK(su_audio_system_create(&s.desc, &system, message) == SU_OK);
        memset(&emitter, 0, sizeof emitter);
        emitter.pos = meters(10.0, 0.0);
        emitter.scale = 1.0f;
        memset(&trigger, 0, sizeof trigger);
        trigger.struct_size = sizeof trigger;
        trigger.event_id = 2u;
        trigger.commit_seq = 1u;
        trigger.session_generation = 1u;
        CHECK(su_audio_system_emitter_create(system, &emitter, &trigger.emitter) == SU_OK);
        CHECK(su_audio_system_post(system, &trigger, &instance, NULL) == SU_OK);
        CHECK(su_audio_system_render(system, offline_pcm, 4800u) == SU_OK);
        for (index = 1200u; index < 4800u; index++) {
            left += fabs(offline_pcm[index * 2u]);
            right += fabs(offline_pcm[index * 2u + 1u]);
        }
        CHECK(left < 1e-3 && right > 1.0 && right / 3600.0 < 0.3 * 0.2); /* quiet: 1/10 m distance law, panned */
        su_audio_system_destroy(system);
    }

    /* Doppler: 200 Hz approaching at a tenth of the speed of sound plays at 343 / (343 - 34.3) = 1.111x */
    render_loop(&s, 1, &plain_rms, &plain_count);
    s.desc.doppler = 1.0f;
    render_loop(&s, 1, &rms, &count);
    CHECK(plain_count >= 179u && plain_count <= 181u);
    CHECK(count >= 198u && count <= 202u);
    CHECK(fabs(rms - plain_rms) < 0.05 * plain_rms);
    s.desc.doppler = 0.0f;

    /* bus filter: a -20 dB binding on bus 30 lowers the bus-30 voice; the same binding on bus 20 does not */
    memset(params, 0, sizeof params);
    params[0] = s.param;
    params[1].id = 2u;
    params[1].max_value = 1.0f;
    quiet[0].value = 0.0f;
    quiet[0].output = -20.0f;
    quiet[1].value = 1.0f;
    quiet[1].output = -20.0f;
    bindings[0] = s.binding;
    memset(&bindings[1], 0, sizeof bindings[1]);
    bindings[1].param_id = 2u;
    bindings[1].control = SU_AUDIO_CONTROL_GAIN_DB;
    bindings[1].bus_id = 20u;
    bindings[1].points = quiet;
    bindings[1].point_count = 2u;
    s.params.params = params;
    s.params.param_count = 2u;
    s.params.bindings = bindings;
    s.params.binding_count = 2u;
    render_loop(&s, 0, &plain_rms, &plain_count);
    bindings[1].bus_id = 30u;
    render_loop(&s, 0, &rms, &count);
    CHECK(plain_rms > 0.05 && fabs(rms / plain_rms - 0.1) < 0.005);
    bindings[1].bus_id = 0u; /* every voice */
    render_loop(&s, 0, &rms, &count);
    CHECK(fabs(rms / plain_rms - 0.1) < 0.005);
    /* an identity binding: the instance-scoped value wins over the global one */
    params[1].min_value = -60.0f;
    params[1].max_value = 0.0f;
    quiet[0].value = -60.0f;
    quiet[0].output = -60.0f;
    quiet[1].value = 0.0f;
    quiet[1].output = 0.0f;
    scoped_db = -20.0f;
    render_loop(&s, 0, &rms, &count);
    CHECK(fabs(rms / plain_rms - 0.1) < 0.005);
    scoped_db = 0.0f;
    setup_free(&s);
}

#endif

#ifndef SU_AUDIO_SYSTEM_ALLOC_TEST
static uint32_t admission_post(SuAudioSystem *system, uint32_t event_id, uint64_t at, uint64_t seq,
                                SuAudioInstance *instance) {
    SuAudioTrigger trigger;
    memset(&trigger, 0, sizeof trigger);
    trigger.struct_size = sizeof trigger;
    trigger.event_id = event_id;
    trigger.sample_time = at;
    trigger.commit_seq = seq;
    trigger.session_generation = 1u;
    return su_audio_system_post(system, &trigger, instance, NULL);
}

static void test_admission_cooldown(void) {
    Setup setup;
    SuAudioSystem *system = NULL;
    SuAudioSystem *restored = NULL;
    SuAudioInstance first = 0u;
    SuAudioInstance instance = 0u;
    SuAudioEventStats stats;
    const SuAudioSystemCommand *journal = NULL;
    uint32_t count = 0u;
    size_t written = 0u;
    size_t repeat_bytes = 0u;
    static uint8_t repeat[sizeof blob];
    setup_init(&setup, SU_AUDIO_OUTPUT_OFFLINE);
    setup.rules[0].bus_id = 0u;
    setup.rules[0].event_id = 1u;
    setup.rules[0].min_retrigger_frames = 256u;
    CHECK(su_audio_system_create(&setup.desc, &system, NULL) == SU_OK);
    CHECK(admission_post(system, 1u, 0u, 0u, &first) == SU_OK);
    CHECK(admission_post(system, 1u, 0u, 1u, &instance) == SU_E_AUDIO_COOLDOWN && instance == 0u);
    CHECK(admission_post(system, 1u, 255u, 1u, &instance) == SU_E_AUDIO_COOLDOWN);
    CHECK(admission_post(system, 2u, 0u, 1u, &instance) == SU_OK);
    CHECK(su_audio_system_checkpoint_save(system, blob, sizeof blob, &written) == SU_OK);
    CHECK(su_audio_system_checkpoint_save(system, repeat, sizeof repeat, &repeat_bytes) == SU_OK);
    CHECK(written == repeat_bytes && memcmp(blob, repeat, written) == 0);
    CHECK(su_audio_system_create(&setup.desc, &restored, NULL) == SU_OK);
    CHECK(su_audio_system_checkpoint_load(restored, blob, written) == SU_OK);
    CHECK(admission_post(restored, 1u, 255u, 2u, &instance) == SU_E_AUDIO_COOLDOWN);
    CHECK(admission_post(system, 1u, 256u, 2u, &instance) == SU_OK);
    CHECK(admission_post(restored, 1u, 256u, 2u, &instance) == SU_OK);
    CHECK(admission_post(system, 1u, 128u, 3u, &instance) == SU_E_AUDIO_COOLDOWN);
    CHECK(su_audio_system_control(system, "audio.trace.read", "{}", 2u, text, sizeof text, &repeat_bytes) == SU_OK);
    CHECK(strstr(text, "cooldown") != NULL && strstr(text, "SU_E_AUDIO_COOLDOWN") != NULL);
    {
        SuAudioTelemetryStats telemetry;
        su_audio_trace_stats(su_audio_system_telemetry(system), &telemetry);
        CHECK(telemetry.reasons[SU_AUDIO_TRACE_REASON_COOLDOWN] == 3u);
    }
    CHECK(su_audio_system_journal(system, &journal, &count) == SU_OK);
    CHECK(su_audio_system_checkpoint_verify(&setup.desc, journal, count, blob, written, 1024u) == SU_OK);
    su_audio_events_stats(su_audio_system_events(system), &stats);
    CHECK(stats.admitted == 3u && stats.refused == 3u && stats.instances == 3u);
    CHECK(su_audio_system_render(system, offline_pcm, 512u) == SU_OK);
    CHECK(su_audio_system_render(restored, ring_pcm, 1u) == SU_OK);
    CHECK(su_audio_system_render(restored, ring_pcm + 2u, 511u) == SU_OK);
    CHECK(memcmp(offline_pcm, ring_pcm, 512u * 2u * sizeof(float)) == 0);
    su_audio_system_destroy(restored);
    restored = NULL;
    setup.rules[0].min_retrigger_frames++;
    CHECK(su_audio_system_create(&setup.desc, &restored, NULL) == SU_OK);
    CHECK(su_audio_system_checkpoint_load(restored, blob, written) == SU_E_SCHEMA_MISMATCH);
    su_audio_system_destroy(restored);
    su_audio_system_destroy(system);
    system = NULL;
    setup.rules[0].min_retrigger_frames = 10000u;
    CHECK(su_audio_system_create(&setup.desc, &system, NULL) == SU_OK);
    CHECK(admission_post(system, 999u, 0u, 0u, &instance) != SU_OK);
    CHECK(admission_post(system, 1u, 0u, 0u, &instance) == SU_OK);
    CHECK(su_audio_system_render(system, offline_pcm, 8192u) == SU_OK);
    CHECK(su_audio_system_checkpoint_save(system, blob, sizeof blob, &written) == SU_OK);
    CHECK(admission_post(system, 1u, 9999u, 1u, &instance) == SU_E_AUDIO_COOLDOWN);
    restored = NULL;
    CHECK(su_audio_system_create(&setup.desc, &restored, NULL) == SU_OK);
    CHECK(su_audio_system_checkpoint_load(restored, blob, written) == SU_OK);
    CHECK(admission_post(restored, 1u, 9999u, 1u, &instance) == SU_E_AUDIO_COOLDOWN);
    CHECK(admission_post(restored, 1u, 10000u, 1u, &instance) == SU_OK);
    su_audio_system_destroy(restored);
    su_audio_system_destroy(system);
    setup_free(&setup);
}

static void test_admission_atomic(void) {
    static const char parallel_graph[] =
        "{\"schema\":\"su_audio_events_v1\",\"numeric_profile\":\"audio_float_local_v1\","
        "\"seed_policy\":\"event_emitter_sequence_v1\",\"sample_rate\":48000,\"nodes\":["
        "{\"id\":1,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":1}},"
        "{\"id\":3,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":1}},"
        "{\"id\":4,\"type\":\"parallel\",\"children\":[1,3]}],"
        "\"events\":[{\"id\":1,\"root\":4},{\"id\":2,\"root\":1},{\"id\":3,\"root\":3}]}";
    SuAudioGraphLimits limits = {16u, 32u, 8u, 16u, 8192u};
    Setup setup;
    SuAudioSystem *system = NULL;
    SuAudioInstance first = 0u;
    SuAudioInstance instance = 0u;
    SuAudioEventStats stats;
    setup_init(&setup, SU_AUDIO_OUTPUT_OFFLINE);
    su_audio_graph_release(setup.graph);
    setup.graph = NULL;
    CHECK(su_audio_graph_prepare(parallel_graph, sizeof parallel_graph - 1u, setup.assets, &limits,
        &setup.graph, NULL) == SU_OK);
    setup.desc.graph = setup.graph;
    setup.rules[0].bus_id = 0u;
    setup.rules[0].node_id = 1u;
    setup.rules[0].min_retrigger_frames = 512u;
    setup.rules[1].node_id = 3u;
    setup.rules[1].group_id = 7u;
    setup.desc.policy_count = 2u;
    CHECK(su_audio_system_create(&setup.desc, &system, NULL) == SU_OK);
    CHECK(admission_post(system, 3u, 0u, 0u, &first) == SU_OK);
    CHECK(admission_post(system, 1u, 0u, 1u, &instance) == SU_E_AUDIO_GROUP_BUSY && instance == 0u);
    CHECK(admission_post(system, 2u, 0u, 1u, &instance) == SU_OK);
    su_audio_events_stats(su_audio_system_events(system), &stats);
    CHECK(stats.instances == 2u && stats.logical_voices == 2u && stats.admitted == 2u && stats.refused == 1u);
    su_audio_system_destroy(system);
    system = NULL;
    setup.rules[0].node_id = 0u;
    setup.rules[0].group_id = 7u;
    setup.desc.policy_count = 1u;
    CHECK(su_audio_system_create(&setup.desc, &system, NULL) == SU_OK);
    CHECK(admission_post(system, 1u, 0u, 0u, &first) == SU_OK);
    su_audio_events_stats(su_audio_system_events(system), &stats);
    CHECK(stats.logical_voices == 2u);
    CHECK(admission_post(system, 1u, 512u, 1u, &instance) == SU_E_AUDIO_GROUP_BUSY);
    su_audio_system_destroy(system);
    system = NULL;
    setup.rules[0].group_id = 0u;
    setup.desc.events.max_instances = 1u;
    CHECK(su_audio_system_create(&setup.desc, &system, NULL) == SU_OK);
    CHECK(admission_post(system, 2u, 0u, 0u, &first) == SU_OK);
    CHECK(admission_post(system, 2u, 20000u, 1u, &instance) == SU_E_RESOURCE_LIMIT);
    CHECK(su_audio_system_render(system, offline_pcm, 8192u) == SU_OK);
    CHECK(admission_post(system, 2u, 8192u, 1u, &instance) == SU_OK);
    su_audio_system_destroy(system);
    system = NULL;
    setup.desc.events.max_instances = 4u;
    CHECK(su_audio_system_create(&setup.desc, &system, NULL) == SU_OK);
    CHECK(admission_post(system, 2u, UINT64_MAX - 512u, 0u, &first) == SU_OK);
    CHECK(admission_post(system, 2u, UINT64_MAX - 1u, 1u, &instance) == SU_E_AUDIO_COOLDOWN);
    CHECK(admission_post(system, 2u, UINT64_MAX, 1u, &instance) == SU_OK);
    su_audio_system_destroy(system);
    setup_free(&setup);
}

static void test_admission_groups(void) {
    Setup setup;
    SuAudioSystem *system = NULL;
    SuAudioSystem *restored = NULL;
    SuAudioInstance first = 0u;
    SuAudioInstance instance = 0u;
    size_t written = 0u;
    size_t text_bytes = 0u;
    setup_init(&setup, SU_AUDIO_OUTPUT_OFFLINE);
    setup.rules[0].bus_id = 0u;
    setup.rules[0].event_id = 1u;
    setup.rules[0].group_id = 7u;
    setup.rules[1].event_id = 2u;
    setup.rules[1].group_id = 7u;
    setup.rules[1].policy.priority = 100u;
    setup.desc.policy_count = 2u;
    CHECK(su_audio_system_create(&setup.desc, &system, NULL) == SU_OK);
    CHECK(admission_post(system, 1u, 128u, 0u, &first) == SU_OK);
    CHECK(admission_post(system, 2u, 128u, 1u, &instance) == SU_E_AUDIO_GROUP_BUSY && instance == 0u);
    CHECK(su_audio_system_checkpoint_save(system, blob, sizeof blob, &written) == SU_OK);
    CHECK(su_audio_system_create(&setup.desc, &restored, NULL) == SU_OK);
    CHECK(su_audio_system_checkpoint_load(restored, blob, written) == SU_OK);
    CHECK(admission_post(restored, 2u, 128u, 1u, &instance) == SU_E_AUDIO_GROUP_BUSY);
    CHECK(su_audio_system_render(system, offline_pcm, 256u) == SU_OK);
    CHECK(admission_post(system, 2u, 256u, 1u, &instance) == SU_E_AUDIO_GROUP_BUSY);
    CHECK(su_audio_system_stop(system, SU_AUDIO_SYSTEM_CMD_STOP_INSTANCE, first, 256u, 256u) == SU_OK);
    CHECK(admission_post(system, 2u, 256u, 1u, &instance) == SU_E_AUDIO_GROUP_BUSY);
    CHECK(su_audio_system_render(system, offline_pcm, 128u) == SU_OK);
    CHECK(admission_post(system, 2u, 384u, 1u, &instance) == SU_E_AUDIO_GROUP_BUSY);
    CHECK(su_audio_system_render(system, offline_pcm, 512u) == SU_OK);
    CHECK(admission_post(system, 2u, 896u, 1u, &instance) == SU_OK);
    CHECK(su_audio_system_control(system, "audio.trace.read", "{}", 2u, text, sizeof text, &text_bytes) == SU_OK);
    CHECK(strstr(text, "group_busy") != NULL && strstr(text, "SU_E_AUDIO_GROUP_BUSY") != NULL);
    {
        SuAudioTelemetryStats telemetry;
        su_audio_trace_stats(su_audio_system_telemetry(system), &telemetry);
        CHECK(telemetry.reasons[SU_AUDIO_TRACE_REASON_GROUP_BUSY] == 4u);
    }
    su_audio_system_destroy(restored);
    su_audio_system_destroy(system);
    system = NULL;
    setup.rules[1].group_id = 8u;
    CHECK(su_audio_system_create(&setup.desc, &system, NULL) == SU_OK);
    CHECK(admission_post(system, 1u, 0u, 0u, &first) == SU_OK);
    CHECK(admission_post(system, 2u, 0u, 1u, &instance) == SU_OK);
    CHECK(su_audio_system_render(system, offline_pcm, 8192u) == SU_OK);
    CHECK(admission_post(system, 1u, 8192u, 2u, &instance) == SU_OK);
    su_audio_system_destroy(system);
    system = NULL;
    setup.rules[0].event_id = 0u;
    setup.rules[0].node_id = 2u;
    setup.rules[0].min_retrigger_frames = 256u;
    setup.rules[0].group_id = 0u;
    setup.desc.policy_count = 1u;
    CHECK(su_audio_system_create(&setup.desc, &system, NULL) == SU_OK);
    CHECK(admission_post(system, 2u, 0u, 0u, &first) == SU_OK);
    CHECK(admission_post(system, 2u, 255u, 1u, &instance) == SU_E_AUDIO_COOLDOWN);
    CHECK(admission_post(system, 1u, 0u, 1u, &instance) == SU_OK);
    CHECK(admission_post(system, 2u, 256u, 2u, &instance) == SU_OK);
    su_audio_system_destroy(system);
    system = NULL;
    setup.rules[0].min_retrigger_frames = 0u;
    setup.rules[0].group_id = 9u;
    setup.desc.voices.audibility_floor = 1000.0f;
    CHECK(su_audio_system_create(&setup.desc, &system, NULL) == SU_OK);
    CHECK(admission_post(system, 2u, 0u, 0u, &first) == SU_OK);
    CHECK(su_audio_system_render(system, offline_pcm, 128u) == SU_OK);
    {
        SuAudioVoiceStats voices;
        su_audio_voice_pool_stats(su_audio_system_voices(system), &voices);
        CHECK(voices.virtual_voices == 1u && voices.physical == 0u);
    }
    CHECK(admission_post(system, 2u, 128u, 1u, &instance) == SU_E_AUDIO_GROUP_BUSY);
    CHECK(su_audio_system_stop(system, SU_AUDIO_SYSTEM_CMD_STOP_INSTANCE, first, 128u, 0u) == SU_OK);
    CHECK(su_audio_system_render(system, offline_pcm, 128u) == SU_OK);
    CHECK(admission_post(system, 2u, 256u, 1u, &instance) == SU_OK);
    su_audio_system_destroy(system);
    setup_free(&setup);
}
#endif

#ifdef SU_AUDIO_SYSTEM_ALLOC_TEST
/* After creation, ten seconds of posts, spatial updates, parameter changes, stops, clustering, telemetry,
 * journaling and checkpoint saves allocate nothing. */
static void test_steady_state(void) {
    Setup s;
    SuAudioSystem *system = NULL;
    Session a;
    SuAudioSystemStats stats;
    uint64_t position;
    size_t before;
    size_t written = 0u;
    setup_init(&s, SU_AUDIO_OUTPUT_RING);
    s.desc.journal_capacity = 4096u;
    s.desc.telemetry_capacity = 4096u;
    CHECK(su_audio_system_create(&s.desc, &system, NULL) == SU_OK);
    memset(&a, 0, sizeof a);
    issue(system, &a, 0u);
    before = sys_allocations;
    for (position = 128u; position < 480000u; position += 128u) {
        float out[128u * 2u];
        game_frame(system, &a, (uint32_t)(position / 128u) % 150u + 1u);
        CHECK(su_audio_system_update(system) == SU_OK);
        (void)su_audio_system_ring_read(system, out, 128u);
        if (position % 96000u == 0u) {
            CHECK(su_audio_system_checkpoint_save(system, blob, sizeof blob, &written) == SU_OK);
        }
    }
    CHECK(sys_allocations == before);
    su_audio_system_stats(system, &stats);
    CHECK(stats.runtime_bytes > 0u);
    printf("audio_system_alloc: %llu bytes, 0 allocations over %u frames\n", (unsigned long long)stats.runtime_bytes,
        480000u);
    su_audio_system_destroy(system);
    setup_free(&s);
}
#endif

int main(void) {
    synthesize();
#ifdef SU_AUDIO_SYSTEM_ALLOC_TEST
    test_steady_state();
#else
    test_paths_identical();
    test_checkpoint_verify();
    test_control_methods();
    test_device_output();
    test_host_extensions();
    test_pull_render();
    test_sum_groups();
    test_admission_cooldown();
    test_admission_groups();
    test_admission_atomic();
#endif
    printf("audio_system: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
