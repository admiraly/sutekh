/* Combined audio system: offline and real-time (ring) output paths, journal replay, checkpoint verification,
 * control-plane methods and steady-state allocations. Every sample is synthesized here. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/audio_system.h"
#include "ir/su_sha256.h"

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
    SuAudioVoicePolicyRule rules[1];
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
    setup_free(&s);
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
#endif
    printf("audio_system: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
