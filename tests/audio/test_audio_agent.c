#include "platform/su_posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent/su_agent.h"
#include "agent/su_proto.h"
#include "sutekh/audio_system.h"
#include "sutekh/status.h"

static unsigned checks;
static unsigned failures;

#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static char response[1u << 20];
static char expected[1u << 20];

static void request(const SuAgentConfig *config, const char *method, const char *params, uint32_t status) {
    FILE *input = tmpfile();
    FILE *output = tmpfile();
    size_t bytes;
    SuJsonDoc doc;
    SuJsonError error;
    SuJsonLimits limits = {sizeof response, 32u};
    const SuJsonValue *value;
    uint64_t actual = 0u;
    CHECK(input != NULL && output != NULL);
    if (input == NULL || output == NULL) {
        if (input != NULL) fclose(input);
        if (output != NULL) fclose(output);
        return;
    }
    CHECK(fprintf(input, "{\"protocol\":\"0.1\",\"id\":\"audio-119\",\"method\":\"%s\",\"params\":%s}\n",
                  method, params) > 0);
    rewind(input);
    CHECK(su_agent_serve(fileno(input), fileno(output), config) == 0);
    rewind(output);
    bytes = fread(response, 1u, sizeof response - 1u, output);
    response[bytes] = '\0';
    CHECK(bytes > 0u && response[bytes - 1u] == '\n');
    CHECK(strchr(response, '\n') == response + bytes - 1u);
    if (su_json_parse((const uint8_t *)response, bytes, &limits, &doc, &error) != SU_OK) {
        CHECK(0);
        fclose(input);
        fclose(output);
        return;
    }
    value = su_json_member(doc.root, "id");
    if (status == SU_E_DUPLICATE_KEY) {
        CHECK(value != NULL && value->type == SU_JSON_NULL);
    } else {
        CHECK(su_json_is_string(value) && strcmp(value->u.string.data, "audio-119") == 0);
    }
    value = su_json_member(doc.root, "protocol");
    CHECK(su_json_is_string(value) && strcmp(value->u.string.data, "0.1") == 0);
    value = su_json_member(doc.root, "ok");
    CHECK(value != NULL && value->type == SU_JSON_BOOL && value->u.boolean == (status == SU_OK));
    if (status != SU_OK) {
        value = su_json_member(su_json_member(doc.root, "error"), "status");
        CHECK(su_json_get_uint(value, &actual) && actual == status);
    }
    su_json_doc_free(&doc);
    CHECK(fclose(input) == 0);
    CHECK(fclose(output) == 0);
}

static void compare_direct(const SuAgentConfig *config, const char *method, const char *params) {
    size_t written = 0u;
    size_t prefix;
    CHECK(su_audio_system_control(config->audio_system, method, params, strlen(params),
                                  expected, sizeof expected, &written) == SU_OK);
    request(config, method, params, SU_OK);
    prefix = strlen("{\"protocol\":\"0.1\",\"id\":\"audio-119\",\"ok\":true,\"result\":");
    CHECK(strlen(response) == prefix + written + 2u);
    CHECK(memcmp(response + prefix, expected, written) == 0);
    CHECK(strcmp(response + prefix + written, "}\n") == 0);
}

static void session(const SuAgentConfig *config) {
    static const char *const methods[] = {
        "engine.info", "audio.inspect", "audio.trace.read", "audio.trace.stats", "audio.render.offline"
    };
    FILE *input = tmpfile();
    FILE *output = tmpfile();
    size_t index;
    CHECK(input != NULL && output != NULL);
    if (input == NULL || output == NULL) {
        if (input != NULL) fclose(input);
        if (output != NULL) fclose(output);
        return;
    }
    for (index = 0u; index < sizeof methods / sizeof methods[0]; index++) {
        CHECK(fprintf(input, "{\"protocol\":\"0.1\",\"id\":\"%zu\",\"method\":\"%s\",\"params\":%s}\n",
                      index, methods[index], index == 4u ? "{\"frames\":1024}" : "{}") > 0);
    }
    rewind(input);
    CHECK(su_agent_serve(fileno(input), fileno(output), config) == 0);
    rewind(output);
    for (index = 0u; index < sizeof methods / sizeof methods[0]; index++) {
        SuJsonDoc doc;
        SuJsonError error;
        SuJsonLimits limits = {sizeof response, 32u};
        const SuJsonValue *value;
        char id[32];
        CHECK(snprintf(id, sizeof id, "%zu", index) > 0);
        CHECK(fgets(response, sizeof response, output) != NULL);
        if (su_json_parse((const uint8_t *)response, strlen(response), &limits, &doc, &error) != SU_OK) {
            CHECK(0);
            continue;
        }
        value = su_json_member(doc.root, "id");
        CHECK(su_json_is_string(value) && strcmp(value->u.string.data, id) == 0);
        value = su_json_member(doc.root, "ok");
        CHECK(value != NULL && value->type == SU_JSON_BOOL && value->u.boolean);
        CHECK(su_json_member(doc.root, "result") != NULL);
        su_json_doc_free(&doc);
    }
    CHECK(fgetc(output) == EOF);
    CHECK(fclose(input) == 0);
    CHECK(fclose(output) == 0);
}

int main(int argc, char **argv) {
    static const char graph_json[] =
        "{\"schema\":\"su_audio_events_v1\",\"numeric_profile\":\"audio_float_local_v1\","
        "\"seed_policy\":\"event_emitter_sequence_v1\",\"sample_rate\":48000,\"nodes\":["
        "{\"id\":1,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":1},\"bus_id\":1}],"
        "\"events\":[{\"id\":1,\"root\":1}]}";
    SuAgentConfig config;
    SuAudioAssets *assets = NULL;
    SuAudioGraph *graph = NULL;
    SuAudioSystem *system = NULL;
    char message[256];
    SuAudioSystemDesc desc;
    SuAudioGraphLimits limits = {4u, 4u, 2u, 4u, 8192u};
    SuAudioGraphDiagnostic diagnostic;
    SuAudioClip clip;
    SuAudioAssetRef asset;
    SuAudioTrigger trigger;
    SuAudioInstance instance;
    SuAudioSystemStats before;
    SuAudioSystemStats after;
    float samples[512];
    float pcm[256];
    uint32_t index;
    const char *method;
    if (argc != 2) return 2;
    memset(&config, 0, sizeof config);
    config.project_root = argv[1];
    request(&config, "engine.info", "{}", SU_OK);
    CHECK(strstr(response, "\"world.inspect\"") != NULL);
    CHECK(strstr(response, "\"capsule.submit\"") != NULL);
    CHECK(strstr(response, "audio.") == NULL);
    for (index = 0u; (method = su_audio_system_method(index)) != NULL; index++) {
        request(&config, method, "{}", SU_E_UNSUPPORTED_METHOD);
    }
    for (index = 0u; index < 512u; index++) samples[index] = 0.25f;
    CHECK(su_audio_assets_create(2u, 8192u, &assets) == SU_OK);
    clip.channels = 1u;
    clip.sample_rate = 48000u;
    clip.samples = samples;
    clip.frames = 512u;
    CHECK(su_audio_asset_prepare(assets, 1u, &clip, &asset) == SU_OK);
    CHECK(su_audio_graph_prepare(graph_json, sizeof graph_json - 1u, assets, &limits, &graph, &diagnostic) == SU_OK);
    memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.assets = assets;
    desc.graph = graph;
    desc.events.struct_size = sizeof desc.events;
    desc.events.sample_rate = 48000u;
    desc.events.max_instances = 4u;
    desc.events.max_leaves_per_instance = 1u;
    desc.events.max_logical_voices = 4u;
    desc.events.command_capacity = 4u;
    desc.events.project_seed = 1u;
    desc.events.session_generation = 1u;
    desc.scene.struct_size = sizeof desc.scene;
    desc.scene.max_emitters = 2u;
    desc.scene.max_curves = 1u;
    desc.voices.struct_size = sizeof desc.voices;
    desc.voices.sample_rate = 48000u;
    desc.voices.max_logical = 4u;
    desc.voices.max_physical = 2u;
    desc.voices.transition_reserve = 1u;
    desc.journal_capacity = 16u;
    desc.telemetry_capacity = 16u;
    CHECK(su_audio_system_create(&desc, &system, message) == SU_OK);
    if (system == NULL) return 1;
    memset(&trigger, 0, sizeof trigger);
    trigger.struct_size = sizeof trigger;
    trigger.event_id = 1u;
    trigger.session_generation = 1u;
    CHECK(su_audio_system_post(system, &trigger, &instance, NULL) == SU_OK);
    CHECK(su_audio_system_render(system, pcm, 128u) == SU_OK);
    CHECK(pcm[64] != 0.0f);
    config.audio_system = system;
    request(&config, "engine.info", "{}", SU_OK);
    for (index = 0u; (method = su_audio_system_method(index)) != NULL; index++) {
        CHECK(strstr(response, method) != NULL);
    }
    su_audio_system_stats(system, &before);
    compare_direct(&config, "audio.inspect", "{}");
    request(&config, "audio.trace.read", "{}", SU_OK);
    CHECK(strstr(response, "\"entries\":[{") != NULL);
    CHECK(strstr(response, "\"id\":1,") != NULL);
    compare_direct(&config, "audio.trace.read", "{}");
    compare_direct(&config, "audio.trace.stats", "{}");
    compare_direct(&config, "audio.render.offline", "{\"frames\":1024}");
    CHECK(strstr(response, "\"pcm_sha256\":") != NULL);
    CHECK(strstr(response, "\"sample_peak\":0.000000") == NULL);
    session(&config);
    request(&config, "audio.render.offline", "{\"fr\\u0061mes\":1024}", SU_OK);
    request(&config, "audio.unknown", "{}", SU_E_UNSUPPORTED_METHOD);
    request(&config, "audio.inspect", "{\"unexpected\":1}", SU_E_TYPE);
    request(&config, "audio.trace.read", "{\"cursor\":0}", SU_E_TYPE);
    request(&config, "audio.trace.stats", "[]", SU_E_TYPE);
    request(&config, "audio.inspect", "{\"token\":\"untrusted\"}", SU_E_TYPE);
    request(&config, "audio.render.offline", "{}", SU_E_INVALID_ARGUMENT);
    request(&config, "audio.render.offline", "{\"frames\":0}", SU_E_INVALID_ARGUMENT);
    request(&config, "audio.render.offline", "{\"frames\":-1}", SU_E_INVALID_ARGUMENT);
    request(&config, "audio.render.offline", "{\"frames\":1.5}", SU_E_INVALID_ARGUMENT);
    request(&config, "audio.render.offline", "{\"frames\":null}", SU_E_INVALID_ARGUMENT);
    request(&config, "audio.render.offline", "{\"frames\":true}", SU_E_INVALID_ARGUMENT);
    request(&config, "audio.render.offline", "{\"frames\":\"1024\"}", SU_E_INVALID_ARGUMENT);
    request(&config, "audio.render.offline", "{\"frames\":28800001}", SU_E_INVALID_ARGUMENT);
    request(&config, "audio.render.offline", "{\"frames\":18446744073709551616}", SU_E_INVALID_ARGUMENT);
    request(&config, "audio.render.offline", "{\"frames\":1,\"frames\":2}", SU_E_DUPLICATE_KEY);
    request(&config, "audio.render.offline", "{\"frames\":1024,\"path\":\"ignored\"}", SU_E_TYPE);
    request(&config, "world.inspect", "{}", SU_E_TYPE);
    compare_direct(&config, "audio.inspect", "{}");
    su_audio_system_stats(system, &after);
    CHECK(before.clock == after.clock);
    CHECK(before.rendered_frames == after.rendered_frames);
    CHECK(before.journal_count == after.journal_count);
    CHECK(su_audio_system_render(system, pcm, 128u) == SU_OK);
    su_audio_system_destroy(system);
    system = NULL;
    desc.journal_capacity = 0u;
    desc.telemetry_capacity = 0u;
    CHECK(su_audio_system_create(&desc, &system, message) == SU_OK);
    config.audio_system = system;
    request(&config, "audio.inspect", "{}", SU_OK);
    request(&config, "audio.trace.read", "{}", SU_E_UNSUPPORTED_METHOD);
    request(&config, "audio.trace.stats", "{}", SU_E_UNSUPPORTED_METHOD);
    request(&config, "audio.render.offline", "{\"frames\":1024}", SU_E_RESOURCE_LIMIT);
    su_audio_system_destroy(system);
    su_audio_graph_release(graph);
    CHECK(su_audio_assets_destroy(assets) == SU_OK);
    printf("audio_agent: %u checks, %u failures\n", checks, failures);
    return failures != 0u;
}
