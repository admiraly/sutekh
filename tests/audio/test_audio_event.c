#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sutekh/audio_event.h"

static unsigned checks;
static unsigned failures;

#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        failures++; \
        fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
    } \
} while (0)

static const char graph_json[] =
    "{\"schema\":\"su_audio_events_v1\",\"numeric_profile\":\"audio_float_local_v1\","
    "\"seed_policy\":\"event_emitter_sequence_v1\",\"sample_rate\":48000,\"nodes\":["
    "{\"id\":1,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":1}},"
    "{\"id\":2,\"type\":\"sequence\",\"children\":[{\"node\":1,\"offset_frames\":7}]},"
    "{\"id\":3,\"type\":\"sequence\",\"children\":[{\"node\":2,\"offset_frames\":5}]},"
    "{\"id\":4,\"type\":\"sound\",\"asset\":{\"id\":2,\"generation\":2},\"flags\":1},"
    "{\"id\":5,\"type\":\"parallel\",\"children\":[1,3]}],"
    "\"events\":[{\"id\":1,\"root\":1},{\"id\":2,\"root\":3},{\"id\":3,\"root\":4},"
    "{\"id\":4,\"root\":5}]}";

typedef struct Fixture {
    SuAudioAssets *assets;
    SuAudioGraph *graph;
    SuAudioEvents *events;
    SuAudioScene *scene;
    SuAudioEventsDesc desc;
} Fixture;

static Fixture fixture(uint32_t capacity, uint64_t start) {
    Fixture result;
    SuAudioGraphLimits limits = {16u, 32u, 8u, 16u, 8192u};
    SuAudioGraphDiagnostic diagnostic;
    SuAudioAssetRef ref;
    float impulse[] = {0.25f, 0.0f, 0.0f, 0.0f};
    float constant[] = {0.125f, 0.125f, 0.125f, 0.125f};
    SuAudioClip clip = {impulse, 4u, 1u, 48000u};
    SuAudioSceneDesc scene_desc = {sizeof scene_desc, 4u, 4u};
    memset(&result, 0, sizeof result);
    CHECK(su_audio_assets_create(4u, 4096u, &result.assets) == SU_OK);
    CHECK(su_audio_asset_prepare(result.assets, 1u, &clip, &ref) == SU_OK);
    CHECK(ref.generation == 1u);
    clip.samples = constant;
    CHECK(su_audio_asset_prepare(result.assets, 2u, &clip, &ref) == SU_OK);
    CHECK(su_audio_graph_prepare(graph_json, sizeof graph_json - 1u, result.assets, &limits,
        &result.graph, &diagnostic) == SU_OK);
    CHECK(su_audio_scene_create(&scene_desc, &result.scene) == SU_OK);
    result.desc.struct_size = sizeof result.desc;
    result.desc.sample_rate = 48000u;
    result.desc.max_instances = capacity;
    result.desc.max_leaves_per_instance = 4u;
    result.desc.max_logical_voices = capacity * 4u;
    result.desc.command_capacity = 2u;
    result.desc.project_seed = 123u;
    result.desc.session_generation = 1u;
    result.desc.start_sample = start;
    CHECK(su_audio_events_create(&result.desc, result.assets, result.scene, result.graph, &result.events) == SU_OK);
    return result;
}

static void cleanup(Fixture *fixture_data) {
    su_audio_events_destroy(fixture_data->events);
    su_audio_graph_release(fixture_data->graph);
    su_audio_scene_destroy(fixture_data->scene);
    CHECK(su_audio_assets_destroy(fixture_data->assets) == SU_OK);
}

static uint32_t post(Fixture *fixture_data, uint32_t event_id, uint64_t at, uint64_t sequence,
                       SuAudioEmitter emitter, uint32_t scope, SuAudioInstance *out) {
    SuAudioTrigger trigger;
    SuAudioGraphDiagnostic diagnostic;
    memset(&trigger, 0, sizeof trigger);
    trigger.struct_size = sizeof trigger;
    trigger.event_id = event_id;
    trigger.sample_time = at;
    trigger.commit_seq = sequence;
    trigger.session_generation = 1u;
    trigger.emitter = emitter;
    trigger.scope_id = scope;
    return su_audio_event_post(fixture_data->events, &trigger, out, &diagnostic);
}

static void exact_timestamps(void) {
    static const uint64_t times[] = {0u, 1u, 127u, 128u, 129u};
    Fixture data = fixture(8u, 0u);
    SuAudioInstance instance;
    float pcm[512u * 2u];
    uint32_t index;
    for (index = 0u; index < sizeof times / sizeof times[0]; index++) {
        CHECK(post(&data, 1u, times[index], index, 0u, 0u, &instance) == SU_OK);
    }
    CHECK(post(&data, 2u, 256u, 8u, 0u, 0u, &instance) == SU_OK);
    CHECK(su_audio_events_render(data.events, pcm, 512u) == SU_OK);
    for (index = 0; index < 512u; index++) {
        uint32_t point;
        float expected = index == 268u ? 0.25f : 0.0f;
        for (point = 0; point < sizeof times / sizeof times[0]; point++) {
            if (index == times[point]) {
                expected += 0.25f;
            }
        }
        CHECK(pcm[index * 2u] == expected);
        CHECK(pcm[index * 2u + 1u] == expected);
    }
    CHECK(!su_audio_instance_active(data.events, instance));
    su_audio_events_update(data.events);
    CHECK(su_audio_instance_stop(data.events, instance, 512u, 0u) == SU_E_STALE_HANDLE);
    cleanup(&data);
}

static void chunks(void) {
    Fixture whole = fixture(8u, 0u);
    Fixture pieces = fixture(8u, 0u);
    float expected[1024u * 2u];
    float actual[1024u * 2u];
    SuAudioInstance instance;
    uint32_t offset = 0u;
    uint32_t index;
    SuAudioEventStats stats;
    CHECK(post(&whole, 3u, 31u, 0u, 0u, 0u, &instance) == SU_OK);
    CHECK(su_audio_instance_stop(whole.events, instance, 333u, 37u) == SU_OK);
    CHECK(post(&pieces, 3u, 31u, 0u, 0u, 0u, &instance) == SU_OK);
    CHECK(su_audio_instance_stop(pieces.events, instance, 333u, 37u) == SU_OK);
    CHECK(post(&whole, 4u, 500u, 1u, 0u, 0u, &instance) == SU_OK);
    CHECK(post(&pieces, 4u, 500u, 1u, 0u, 0u, &instance) == SU_OK);
    CHECK(su_audio_events_render(whole.events, expected, 1024u) == SU_OK);
    for (index = 0u; offset < 1024u; index++) {
        uint32_t count = (index * 47u) % 191u + 1u;
        if (count > 1024u - offset) {
            count = 1024u - offset;
        }
        CHECK(su_audio_events_render(pieces.events, actual + offset * 2u, count) == SU_OK);
        offset += count;
    }
    CHECK(memcmp(actual, expected, sizeof actual) == 0);
    su_audio_events_stats(pieces.events, &stats);
    CHECK(stats.render_failures == 0u);
    CHECK(stats.render_clock == 1024u && stats.delivered_frames == 1024u);
    cleanup(&whole);
    cleanup(&pieces);
}

static void cancellation_and_admission(void) {
    Fixture data = fixture(2u, 0u);
    SuAudioInstance first;
    SuAudioInstance second;
    SuAudioInstance refused;
    SuAudioCommitCursor cursor;
    SuAudioEventStats stats;
    float pcm[256u * 2u];
    uint32_t index;
    CHECK(post(&data, 2u, 20u, 3u, 0u, 11u, &first) == SU_OK);
    CHECK(post(&data, 1u, 77u, 4u, 0u, 12u, &second) == SU_OK);
    CHECK(post(&data, 1u, 99u, 5u, 0u, 12u, &refused) == SU_E_RESOURCE_LIMIT);
    CHECK(su_audio_commit_cursor_snapshot(data.events, &cursor) == SU_OK);
    CHECK(cursor.next_seq == 5u);
    CHECK(post(&data, 1u, 99u, 4u, 0u, 12u, &refused) == SU_E_STALE_REVISION);
    CHECK(post(&data, 1u, 99u, 2u, 0u, 12u, &refused) == SU_E_STALE_REVISION);
    CHECK(su_audio_scope_stop(data.events, 11u, 32u, 0u) == SU_OK);
    CHECK(su_audio_scope_stop(data.events, 99u, 33u, 0u) == SU_OK);
    CHECK(su_audio_scope_stop(data.events, 99u, 34u, 0u) == SU_E_RESOURCE_LIMIT);
    CHECK(su_audio_events_render(data.events, pcm, 256u) == SU_OK);
    for (index = 0; index < 256u; index++) {
        CHECK(pcm[index * 2u] == (index == 77u ? 0.25f : 0.0f));
    }
    CHECK(!su_audio_instance_active(data.events, first));
    CHECK(!su_audio_instance_active(data.events, second));
    su_audio_events_update(data.events);
    CHECK(post(&data, 1u, 256u, 5u, 0u, 0u, &refused) == SU_OK);
    CHECK(refused != first);
    CHECK(su_audio_commit_cursor_restore(data.events, &cursor) == SU_E_STALE_REVISION);
    CHECK(su_audio_events_render(data.events, pcm, 256u) == SU_OK);
    su_audio_events_update(data.events);
    su_audio_events_stats(data.events, &stats);
    CHECK(stats.instances == 0u && stats.logical_voices == 0u);
    CHECK(stats.duplicates == 2u && stats.refused == 2u);
    CHECK(su_audio_events_session(data.events, 2u) == SU_OK);
    CHECK(su_audio_commit_cursor_snapshot(data.events, &cursor) == SU_OK);
    CHECK(cursor.session_generation == 2u && cursor.next_seq == 0u);
    cleanup(&data);
}

static void recovery_and_overflow(void) {
    Fixture data = fixture(2u, UINT64_MAX - 127u);
    SuAudioInstance instance;
    SuAudioCommitCursor cursor = {1u, 300u};
    float pcm[2];
    CHECK(post(&data, 2u, UINT64_MAX - 5u, 0u, 0u, 0u, &instance) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_commit_cursor_restore(data.events, &cursor) == SU_OK);
    CHECK(post(&data, 1u, UINT64_MAX - 5u, 299u, 0u, 0u, &instance) == SU_E_STALE_REVISION);
    CHECK(post(&data, 1u, UINT64_MAX - 5u, UINT64_MAX, 0u, 0u, &instance) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_events_render(data.events, pcm, 1u) == SU_E_RESOURCE_LIMIT);
    cleanup(&data);
    data = fixture(2u, 0u);
    CHECK(su_audio_events_render(data.events, pcm, 1u) == SU_OK);
    CHECK(su_audio_events_clock(data.events) == 128u);
    CHECK(post(&data, 1u, 1u, 0u, 0u, 0u, &instance) == SU_E_INVALID_ARGUMENT);
    cleanup(&data);
}

static void emitter_scope(void) {
    Fixture data = fixture(4u, 0u);
    SuAudioEmitterSnapshot emitter_desc;
    SuAudioEmitter first;
    SuAudioEmitter second;
    SuAudioInstance first_instance;
    SuAudioInstance second_instance;
    float pcm[256u * 2u];
    memset(&emitter_desc, 0, sizeof emitter_desc);
    emitter_desc.pos = su_wpos(0.0, 0.0, -1.0);
    emitter_desc.scale = 1.0f;
    CHECK(su_audio_emitter_create(data.scene, &emitter_desc, &first) == SU_OK);
    CHECK(su_audio_emitter_create(data.scene, &emitter_desc, &second) == SU_OK);
    CHECK(post(&data, 3u, 0u, 0u, first, 0u, &first_instance) == SU_OK);
    CHECK(post(&data, 3u, 0u, 1u, second, 0u, &second_instance) == SU_OK);
    CHECK(su_audio_emitter_stop(data.events, first, 10u, 0u) == SU_OK);
    CHECK(su_audio_events_render(data.events, pcm, 256u) == SU_OK);
    CHECK(!su_audio_instance_active(data.events, first_instance));
    CHECK(su_audio_instance_active(data.events, second_instance));
    CHECK(su_audio_emitter_destroy(data.scene, second) == SU_OK);
    CHECK(su_audio_events_render(data.events, pcm, 256u) == SU_OK);
    CHECK(!su_audio_instance_active(data.events, second_instance));
    CHECK(pcm[0] == 0.0f && pcm[1] == 0.0f);
    CHECK(post(&data, 3u, 512u, 2u, second, 0u, &first_instance) == SU_E_STALE_HANDLE);
    cleanup(&data);
}

static void measured_reference(void) {
    Fixture data = fixture(8u, 0u);
    SuAudioInstance instance;
    SuAudioEventStats stats;
    float pcm[128u * 2u];
    clock_t begin;
    clock_t elapsed;
    uint32_t index;
    CHECK(post(&data, 3u, 0u, 0u, 0u, 0u, &instance) == SU_OK);
    begin = clock();
    for (index = 0u; index < 375u; index++) {
        CHECK(su_audio_events_render(data.events, pcm, 128u) == SU_OK);
    }
    elapsed = clock() - begin;
    su_audio_events_stats(data.events, &stats);
    printf("reference: frames=48000 runtime_bytes=%llu cpu_seconds=%.9f render_failures=%llu\n",
        (unsigned long long)stats.runtime_bytes, (double)elapsed / CLOCKS_PER_SEC,
        (unsigned long long)stats.render_failures);
    CHECK(stats.render_failures == 0u);
    cleanup(&data);
}

static void stop_overrides_and_saturation(void) {
    Fixture data = fixture(2u, 0u);
    SuAudioInstance first;
    SuAudioInstance second;
    SuAudioEventStats stats;
    float pcm[256u * 2u];
    uint32_t index;
    su_audio_events_destroy(data.events);
    data.desc.command_capacity = 64u;
    data.desc.max_logical_voices = 2u;
    CHECK(su_audio_events_create(&data.desc, data.assets, data.scene, data.graph, &data.events) == SU_OK);
    CHECK(post(&data, 3u, 0u, 0u, 0u, 0u, &first) == SU_OK);
    CHECK(post(&data, 3u, 0u, 1u, 0u, 0u, &second) == SU_OK);
    for (index = 0u; index < 32u; index++) {
        CHECK(su_audio_instance_stop(data.events, first, 10u, 1000u) == SU_OK);
    }
    CHECK(su_audio_instance_stop(data.events, second, 10u, 0u) == SU_OK);
    CHECK(su_audio_instance_stop(data.events, first, 20u, 0u) == SU_OK);
    CHECK(su_audio_events_render(data.events, pcm, 256u) == SU_OK);
    CHECK(pcm[0] == 0.25f && pcm[10u * 2u] == 0.125f);
    for (index = 20u; index < 256u; index++) {
        CHECK(pcm[index * 2u] == 0.0f && pcm[index * 2u + 1u] == 0.0f);
    }
    CHECK(!su_audio_instance_active(data.events, first));
    CHECK(!su_audio_instance_active(data.events, second));
    su_audio_events_stats(data.events, &stats);
    CHECK(stats.render_failures == 0u);
    cleanup(&data);
    for (index = 0u; index < 3u; index++) {
        static const uint32_t at[] = {10u, 127u, 128u};
        uint32_t sample;
        data = fixture(1u, 0u);
        CHECK(post(&data, 3u, 0u, 0u, 0u, 0u, &first) == SU_OK);
        CHECK(su_audio_instance_stop(data.events, first, at[index], 1u) == SU_OK);
        CHECK(su_audio_events_render(data.events, pcm, 256u) == SU_OK);
        for (sample = 0u; sample < 256u; sample++) {
            CHECK(pcm[sample * 2u] == (sample <= at[index] ? 0.125f : 0.0f));
        }
        cleanup(&data);
    }
}

static void live_attenuation_and_tail_lifetime(void) {
    Fixture data = fixture(2u, 0u);
    SuAudioEmitterSnapshot emitter_desc;
    SuAudioEmitter emitter;
    SuAudioInstance instance;
    SuAudioCurvePoint point = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    SuAudioCurveDesc curve_desc = {sizeof curve_desc, &point, 1u};
    SuAudioCurveId curve;
    float pcm[128u * 2u];
    float expected = 0.125f * sqrtf(0.5f);
    memset(&emitter_desc, 0, sizeof emitter_desc);
    emitter_desc.pos = su_wpos(0.0, 0.0, -10.0);
    emitter_desc.scale = 1.0f;
    CHECK(su_audio_emitter_create(data.scene, &emitter_desc, &emitter) == SU_OK);
    CHECK(post(&data, 3u, 0u, 0u, emitter, 0u, &instance) == SU_OK);
    CHECK(su_audio_events_render(data.events, pcm, 128u) == SU_OK);
    CHECK(fabsf(pcm[254] - expected * 0.1f) < 1.0e-6f);
    CHECK(su_audio_emitter_scale(data.scene, emitter, 2.0f) == SU_OK);
    CHECK(su_audio_events_render(data.events, pcm, 128u) == SU_OK);
    CHECK(fabsf(pcm[254] - expected * 0.2f) < 1.0e-6f);
    CHECK(su_audio_scene_curve_add(data.scene, &curve_desc, &curve) == SU_OK);
    CHECK(su_audio_emitter_curve(data.scene, emitter, curve) == SU_OK);
    CHECK(su_audio_events_render(data.events, pcm, 128u) == SU_OK);
    CHECK(fabsf(pcm[254] - expected) < 1.0e-6f);
    CHECK(su_audio_emitter_curve(data.scene, emitter, 0u) == SU_OK);
    CHECK(su_audio_events_render(data.events, pcm, 128u) == SU_OK);
    CHECK(fabsf(pcm[254] - expected * 0.2f) < 1.0e-6f);
    CHECK(su_audio_instance_stop(data.events, instance, 512u, 1000u) == SU_OK);
    CHECK(su_audio_events_render(data.events, pcm, 128u) == SU_OK);
    CHECK(su_audio_instance_active(data.events, instance));
    CHECK(su_audio_emitter_destroy(data.scene, emitter) == SU_OK);
    CHECK(su_audio_events_render(data.events, pcm, 128u) == SU_OK);
    CHECK(!su_audio_instance_active(data.events, instance));
    CHECK(pcm[0] == 0.0f && pcm[1] == 0.0f);
    su_audio_events_update(data.events);
    cleanup(&data);
}

static void logical_budget(void) {
    Fixture data = fixture(2u, 0u);
    SuAudioInstance instance;
    SuAudioCommitCursor cursor;
    su_audio_events_destroy(data.events);
    data.desc.max_logical_voices = 1u;
    CHECK(su_audio_events_create(&data.desc, data.assets, data.scene, data.graph, &data.events) == SU_OK);
    CHECK(post(&data, 4u, 0u, 0u, 0u, 0u, &instance) == SU_E_RESOURCE_LIMIT);
    CHECK(su_audio_commit_cursor_snapshot(data.events, &cursor) == SU_OK && cursor.next_seq == 0u);
    CHECK(post(&data, 1u, 0u, 0u, 0u, 0u, &instance) == SU_OK);
    CHECK(post(&data, 1u, 0u, 1u, 0u, 0u, &instance) == SU_E_RESOURCE_LIMIT);
    cleanup(&data);
}

int main(void) {
    exact_timestamps();
    chunks();
    cancellation_and_admission();
    recovery_and_overflow();
    emitter_scope();
    stop_overrides_and_saturation();
    live_attenuation_and_tail_lifetime();
    logical_budget();
    measured_reference();
    printf("audio_event: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
