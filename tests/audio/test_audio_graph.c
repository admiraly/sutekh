#include "sutekh/audio_graph.h"
#include "../../src/audio/su_audio_graph_int.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures;
#ifdef A03_TEST_ALLOCATORS
static unsigned expansion_allocations;
static int expanding;
void *a03_malloc(size_t size) {
    if (expanding) expansion_allocations++;
    return malloc(size);
}
void *a03_calloc(size_t count, size_t size) {
    if (expanding) expansion_allocations++;
    return calloc(count, size);
}
void a03_free(void *pointer) {
    if (expanding) expansion_allocations++;
    free(pointer);
}
#endif
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #condition); } } while (0)
#define SOUND1 "{\"id\":1,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":1}}"
#define SOUND2 "{\"id\":2,\"type\":\"sound\",\"asset\":{\"id\":2,\"generation\":2}}"
#define SOUNDS SOUND1 "," SOUND2
#define RANDOM "{\"id\":10,\"type\":\"random\",\"children\":[{\"node\":1,\"weight\":2},{\"node\":2,\"weight\":2}]}"
#define BLEND "{\"id\":30,\"type\":\"blend\",\"parameter_id\":8,\"points\":[{\"value\":0,\"node\":1},{\"value\":10,\"node\":2}]}"
#define SWITCH "{\"id\":20,\"type\":\"switch\",\"switch_id\":7,\"default\":1,\"cases\":[{\"value\":5,\"node\":2}]}"

static const SuAudioGraphLimits standard_limits = {128, 256, 32, 128, 32768};
static SuAudioAssets *assets;

static void document(char *output, size_t capacity, const char *nodes, const char *events) {
    int length = snprintf(output, capacity,
        "{\"schema\":\"su_audio_events_v1\",\"numeric_profile\":\"audio_float_local_v1\","
        "\"seed_policy\":\"event_emitter_sequence_v1\",\"sample_rate\":48000,\"nodes\":[%s],\"events\":[%s]}", nodes, events);
    CHECK(length > 0 && (size_t)length < capacity);
}
static SuAudioGraph *prepare(const char *nodes, const char *events) {
    char json[32768];
    SuAudioGraph *graph = NULL;
    SuAudioGraphDiagnostic diagnostic;
    uint32_t code;
    document(json, sizeof json, nodes, events);
    code = su_audio_graph_prepare(json, strlen(json), assets, &standard_limits, &graph, &diagnostic);
    CHECK(code == SU_OK);
    if (code != SU_OK) { fprintf(stderr, "%s: %s\n", diagnostic.bound, diagnostic.message); exit(2); }
    return graph;
}
static void reject(const char *nodes, const char *events, const SuAudioGraphLimits *limits,
                   uint32_t expected, const char *bound) {
    char json[32768];
    SuAudioGraph *graph = (SuAudioGraph *)(uintptr_t)1;
    SuAudioGraphDiagnostic diagnostic;
    document(json, sizeof json, nodes, events);
    CHECK(su_audio_graph_prepare(json, strlen(json), assets, limits, &graph, &diagnostic) == expected);
    CHECK(graph == NULL);
    CHECK(diagnostic.code == expected);
    if (bound) CHECK(strcmp(diagnostic.bound, bound) == 0);
}

static void test_parser(void) {
    const char *bad_nodes[] = {
        "{\"id\":1,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":1},\"extra\":true}",
        "{\"id\":1,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":1,\"extra\":0}}",
        "{\"id\":1,\"type\":\"sound\",\"asset\":{\"id\":1}}",
        "{\"id\":1.0,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":1}}",
        "{\"id\":4294967296,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":1}}",
        "{\"id\":0,\"type\":\"parallel\",\"children\":[1]}",
        "{\"id\":1,\"type\":\"parallel\",\"children\":[]}",
        "{\"id\":1,\"type\":\"other\"}",
        "{\"id\":1,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":1},\"gain\":1e999}",
        "{\"id\":1,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":1},\"gain\":-1}",
        "{\"id\":1,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":1},\"pitch\":0}",
        "{\"id\":1,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":1},\"pitch\":65}",
        "{\"id\":1,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":1},\"flags\":4}",
        "{\"id\":1,\"type\":\"random\",\"children\":[{\"node\":2,\"weight\":0}]}",
        "{\"id\":1,\"type\":\"random\",\"children\":[{\"node\":2,\"weight\":-1}]}",
        "{\"id\":1,\"type\":\"random\",\"children\":[{\"node\":2,\"weight\":1.5}]}",
        "{\"id\":1,\"type\":\"random\",\"children\":[{\"node\":2,\"weight\":1,\"extra\":0}]}",
        "{\"id\":1,\"type\":\"random\",\"children\":[{\"node\":2,\"weight\":1},{\"node\":2,\"weight\":2}]}",
        "{\"id\":1,\"type\":\"switch\",\"switch_id\":1,\"cases\":[]}",
        "{\"id\":1,\"type\":\"switch\",\"switch_id\":1,\"default\":2,\"cases\":[{\"node\":2,\"value\":1},{\"node\":2,\"value\":1}]}",
        "{\"id\":1,\"type\":\"blend\",\"points\":[{\"node\":2,\"value\":0}]}",
        "{\"id\":1,\"type\":\"blend\",\"parameter_id\":1,\"points\":[{\"node\":2,\"value\":1},{\"node\":2,\"value\":0}]}",
        "{\"id\":1,\"type\":\"blend\",\"parameter_id\":1,\"points\":[{\"node\":2,\"value\":0},{\"node\":2,\"value\":0}]}",
        "{\"id\":1,\"type\":\"sequence\",\"children\":[{\"node\":2,\"offset_frames\":18446744073709551616}]}",
        "{\"id\":1,\"type\":\"parallel\",\"children\":[3]}",
        SOUND1 "," SOUND1
    };
    size_t index;
    char json[32768];
    SuAudioGraph *graph = NULL;
    SuAudioGraphDiagnostic diagnostic;
    for (index = 0; index < sizeof bad_nodes / sizeof bad_nodes[0]; index++)
        reject(bad_nodes[index], "{\"id\":1,\"root\":1}", &standard_limits, SU_E_SCHEMA_MISMATCH, "schema");
    reject(SOUND1, "{\"id\":1,\"root\":1,\"extra\":0}", &standard_limits, SU_E_SCHEMA_MISMATCH, NULL);
    reject(SOUND1, "{\"id\":1,\"root\":2}", &standard_limits, SU_E_SCHEMA_MISMATCH, NULL);
    reject(SOUND1, "{\"id\":1,\"root\":1},{\"id\":1,\"root\":1}", &standard_limits, SU_E_SCHEMA_MISMATCH, NULL);
    CHECK(su_audio_graph_prepare("{", 1, assets, &standard_limits, &graph, &diagnostic) == SU_E_PARSE);
    document(json, sizeof json, "{\"id\":1,\"id\":2,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":1}}", "{\"id\":1,\"root\":1}");
    CHECK(su_audio_graph_prepare(json, strlen(json), assets, &standard_limits, &graph, &diagnostic) == SU_E_DUPLICATE_KEY);
    document(json, sizeof json, SOUND1, "{\"id\":1,\"root\":1}");
    json[strlen(json) - 1] = ',';
    CHECK(su_audio_graph_prepare(json, strlen(json), assets, &standard_limits, &graph, &diagnostic) == SU_E_PARSE);
    document(json, sizeof json, SOUND1, "{\"id\":1,\"root\":1}");
    ((unsigned char *)json)[1] = 0xffu;
    CHECK(su_audio_graph_prepare(json, strlen(json), assets, &standard_limits, &graph, &diagnostic) == SU_E_PARSE);
    CHECK(su_audio_graph_prepare("{\"extra\":0}", 11, assets, &standard_limits, &graph, &diagnostic) == SU_E_SCHEMA_MISMATCH);
}

static void test_bounds(void) {
    SuAudioGraphLimits limits = standard_limits;
    reject("{\"id\":1,\"type\":\"parallel\",\"children\":[1]}", "{\"id\":1,\"root\":1}", &limits, SU_E_SCHEMA_MISMATCH, "dag");
    reject("{\"id\":1,\"type\":\"parallel\",\"children\":[2]},{\"id\":2,\"type\":\"parallel\",\"children\":[1]}", "{\"id\":1,\"root\":1}", &limits, SU_E_SCHEMA_MISMATCH, "dag");
    limits.max_nodes = 1;
    reject(SOUNDS, "{\"id\":1,\"root\":1}", &limits, SU_E_RESOURCE_LIMIT, "max_nodes");
    limits = standard_limits; limits.max_edges = 1;
    reject(SOUNDS ",{\"id\":3,\"type\":\"parallel\",\"children\":[1,2]}", "{\"id\":1,\"root\":3}", &limits, SU_E_RESOURCE_LIMIT, "max_edges");
    limits = standard_limits; limits.max_depth = 1;
    reject(SOUNDS ",{\"id\":3,\"type\":\"parallel\",\"children\":[1]}", "{\"id\":1,\"root\":3}", &limits, SU_E_RESOURCE_LIMIT, "max_depth");
    limits = standard_limits; limits.max_expansion = 1;
    reject(SOUNDS ",{\"id\":3,\"type\":\"parallel\",\"children\":[1,2]}", "{\"id\":1,\"root\":3}", &limits, SU_E_RESOURCE_LIMIT, "max_expansion");
    reject(SOUNDS "," BLEND, "{\"id\":1,\"root\":30}", &limits, SU_E_RESOURCE_LIMIT, "max_expansion");
    limits = standard_limits; limits.max_json_bytes = 10;
    reject(SOUND1, "{\"id\":1,\"root\":1}", &limits, SU_E_RESOURCE_LIMIT, "max_json_bytes");
    limits = standard_limits;
    reject(SOUND1 ",{\"id\":3,\"type\":\"sequence\",\"children\":[{\"node\":1,\"offset_frames\":18446744073709551615}]},"
        "{\"id\":4,\"type\":\"sequence\",\"children\":[{\"node\":3,\"offset_frames\":1}]}", "{\"id\":1,\"root\":4}", &limits, SU_E_RESOURCE_LIMIT, "delay_frames");
    reject("{\"id\":1,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":9}}", "{\"id\":1,\"root\":1}", &limits, SU_E_STALE_HANDLE, "asset");
    reject(SOUND1 ",{\"id\":2,\"type\":\"sound\",\"asset\":{\"id\":99,\"generation\":1}}", "{\"id\":1,\"root\":1}", &limits, SU_E_STALE_HANDLE, "asset");
}

static void test_expansion(void) {
    SuAudioGraph *graph = prepare(SOUNDS "," RANDOM "," SWITCH "," BLEND
        ",{\"id\":40,\"type\":\"sequence\",\"children\":[{\"node\":1,\"offset_frames\":9},{\"node\":2,\"offset_frames\":3}]}"
        ",{\"id\":50,\"type\":\"parallel\",\"children\":[40,1]}",
        "{\"id\":1,\"root\":10},{\"id\":2,\"root\":20},{\"id\":3,\"root\":30},{\"id\":4,\"root\":50}");
    SuAudioGraphLeaf leaves[8], saved[8];
    SuAudioGraphDiagnostic diagnostic;
    SuAudioGraphSwitch switches[] = {{7,5}};
    SuAudioGraphParameter parameters[] = {{8,5.0f}};
    SuAudioGraphHistory history[] = {{10,0,123}};
    SuAudioGraphExpandState state = {switches,1,parameters,1,history,1};
    size_t count = 99, index;
    const uint32_t golden[] = {1,2,2,1,1,2,2,2,2,2,2,2,2,2,2,2};
    CHECK(su_audio_graph_sample_rate(graph) == 48000);
    CHECK(su_audio_graph_retain(graph) == SU_OK);
    su_audio_graph_release(graph);
#ifdef A03_TEST_ALLOCATORS
    expanding = 1;
#endif
    CHECK(su_audio_graph_expand(graph, 4, 0, 0, 0, leaves, 8, &count, &diagnostic) == SU_OK);
    CHECK(count == 3);
    CHECK(leaves[0].node_id == 1 && leaves[0].delay_frames == 9);
    CHECK(leaves[1].node_id == 2 && leaves[1].delay_frames == 3);
    CHECK(leaves[2].node_id == 1 && leaves[2].delay_frames == 0);
    CHECK(leaves[0].gain == 1.0f && leaves[0].pitch == 1.0f && leaves[0].bus_id == 0 && leaves[0].flags == 0);
    memset(leaves, 0xa5, sizeof leaves); memcpy(saved, leaves, sizeof leaves);
    CHECK(su_audio_graph_expand(graph, 4, 0, 0, 0, leaves, 2, &count, &diagnostic) == SU_E_RESOURCE_LIMIT);
    CHECK(count == 0 && !memcmp(leaves, saved, sizeof leaves));
    CHECK(!strcmp(diagnostic.bound, "capacity"));
    CHECK(su_audio_graph_expand(graph, 99, 0, 0, 0, leaves, 8, &count, NULL) == SU_E_SCHEMA_MISMATCH);
    CHECK(su_audio_graph_expand(graph, 1, 0, 0, 0, NULL, 0, &count, NULL) == SU_E_RESOURCE_LIMIT);
    for (index = 0; index < sizeof golden / sizeof golden[0]; index++) {
        CHECK(su_audio_graph_expand(graph, 1, 77, 13, index, leaves, 8, &count, NULL) == SU_OK);
        CHECK(count == 1 && leaves[0].node_id == golden[index]);
    }
    CHECK(su_audio_graph_expand(graph, 2, 0, 0, 0, leaves, 8, &count, NULL) == SU_OK && leaves[0].node_id == 1);
    CHECK(su_audio_graph_expand_ex(graph, 2, 0, 0, 0, &state, leaves, 8, &count, NULL) == SU_OK && leaves[0].node_id == 2);
    switches[0].value = 99;
    CHECK(su_audio_graph_expand_ex(graph, 2, 0, 0, 0, &state, leaves, 8, &count, NULL) == SU_OK && leaves[0].node_id == 1);
    CHECK(su_audio_graph_expand(graph, 3, 0, 0, 0, leaves, 8, &count, &diagnostic) == SU_E_SCHEMA_MISMATCH);
    CHECK(!strcmp(diagnostic.bound, "parameter"));
    CHECK(su_audio_graph_expand_ex(graph, 3, 0, 0, 0, &state, leaves, 8, &count, NULL) == SU_OK);
    CHECK(count == 2 && leaves[0].gain == 0.5f && leaves[1].gain == 0.5f);
    parameters[0].value = -5;
    CHECK(su_audio_graph_expand_ex(graph, 3, 0, 0, 0, &state, leaves, 8, &count, NULL) == SU_OK && count == 1 && leaves[0].node_id == 1);
    parameters[0].value = 15;
    CHECK(su_audio_graph_expand_ex(graph, 3, 0, 0, 0, &state, leaves, 8, &count, NULL) == SU_OK && count == 1 && leaves[0].node_id == 2);
    parameters[0].value = NAN;
    CHECK(su_audio_graph_expand_ex(graph, 3, 0, 0, 0, &state, leaves, 8, &count, NULL) == SU_E_SCHEMA_MISMATCH);
    parameters[0].value = 5;
    history[0].next_child_id = 123;
    CHECK(su_audio_graph_expand_ex(graph, 1, 77, 13, 0, &state, leaves, 0, &count, NULL) == SU_E_RESOURCE_LIMIT);
    CHECK(history[0].last_child_id == 0 && history[0].next_child_id == 123);
    for (index = 0; index < 32; index++) {
        uint32_t previous = history[0].last_child_id;
        CHECK(su_audio_graph_expand_ex(graph, 1, 77, 13, index, &state, leaves, 8, &count, NULL) == SU_OK);
        CHECK(count == 1 && leaves[0].node_id != previous && history[0].last_child_id == leaves[0].node_id);
    }
    history[0].last_child_id = 999;
    CHECK(su_audio_graph_expand_ex(graph, 1, 0, 0, 0, &state, leaves, 8, &count, NULL) == SU_E_SCHEMA_MISMATCH);
#ifdef A03_TEST_ALLOCATORS
    expanding = 0;
    CHECK(expansion_allocations == 0);
#endif
    CHECK(su_audio_asset_unload(assets, (SuAudioAssetRef){1,1}) == SU_E_RESOURCE_LIMIT);
    su_audio_graph_release(graph);
}

static void test_dag_boundaries(void) {
    char nodes[32768], json[32768];
    size_t used, index, count;
    SuAudioGraphLimits limits = {256,512,128,65536,32768};
    SuAudioGraphDiagnostic diagnostic;
    SuAudioGraph *graph = NULL;
    SuAudioGraphLeaf leaves[4], saved[4];
    used = (size_t)snprintf(nodes, sizeof nodes, "%s", SOUND1);
    for (index = 2; index <= 128; index++) {
        used += (size_t)snprintf(nodes + used, sizeof nodes - used,
            ",{\"id\":%zu,\"type\":\"parallel\",\"children\":[%zu]}", index, index - 1);
    }
    document(json, sizeof json, nodes, "{\"id\":1,\"root\":128}");
    CHECK(su_audio_graph_prepare(json, strlen(json), assets, &limits, &graph, &diagnostic) == SU_OK);
    CHECK(su_audio_graph_expand(graph,1,0,0,0,leaves,4,&count,NULL) == SU_OK && count == 1);
    su_audio_graph_release(graph);
    limits.max_depth = 127;
    reject(nodes, "{\"id\":1,\"root\":128}", &limits, SU_E_RESOURCE_LIMIT, "max_depth");
    limits.max_depth = 128;
    snprintf(nodes + used, sizeof nodes - used, ",{\"id\":129,\"type\":\"parallel\",\"children\":[128]}");
    reject(nodes, "{\"id\":1,\"root\":129}", &limits, SU_E_RESOURCE_LIMIT, "max_depth");
    used = (size_t)snprintf(nodes, sizeof nodes, "%s", SOUND1);
    for (index = 2; index <= 18; index++) {
        used += (size_t)snprintf(nodes + used, sizeof nodes - used,
            ",{\"id\":%zu,\"type\":\"parallel\",\"children\":[%zu,%zu]}", index, index - 1, index - 1);
    }
    reject(nodes, "{\"id\":1,\"root\":18}", &limits, SU_E_RESOURCE_LIMIT, "max_expansion");
    graph = prepare(SOUND1 ",{\"id\":2,\"type\":\"sequence\",\"children\":[{\"node\":1,\"offset_frames\":18446744073709551615}]}", "{\"id\":1,\"root\":2}");
    CHECK(su_audio_graph_expand(graph,1,0,0,0,leaves,4,&count,NULL) == SU_OK && leaves[0].delay_frames == UINT64_MAX);
    su_audio_graph_release(graph);
    graph = prepare(SOUNDS ",{\"id\":10,\"type\":\"random\",\"children\":[{\"node\":1,\"weight\":4294967295},{\"node\":2,\"weight\":4294967295}]}", "{\"id\":1,\"root\":10}");
    for (index = 0; index < 16; index++) CHECK(su_audio_graph_expand(graph,1,0,0,index,leaves,4,&count,NULL) == SU_OK && count == 1);
    su_audio_graph_release(graph);
    graph = prepare(SOUNDS "," RANDOM "," BLEND ",{\"id\":40,\"type\":\"parallel\",\"children\":[10,30]}", "{\"id\":1,\"root\":40}");
    {
        SuAudioGraphHistory history = {10,0,91};
        SuAudioGraphExpandState state = {NULL,0,NULL,0,&history,1};
        memset(leaves,0xa5,sizeof leaves); memcpy(saved,leaves,sizeof leaves);
        CHECK(su_audio_graph_expand_ex(graph,1,0,0,0,&state,leaves,4,&count,NULL) == SU_E_SCHEMA_MISMATCH);
        CHECK(count == 0 && !memcmp(leaves,saved,sizeof leaves) && history.last_child_id == 0 && history.next_child_id == 91);
    }
    su_audio_graph_release(graph);
    graph = prepare(SOUNDS "," RANDOM ",{\"id\":40,\"type\":\"parallel\",\"children\":[10,10]}", "{\"id\":1,\"root\":40}");
    {
        SuAudioGraphHistory first = {10,0,0}, second = {10,0,0};
        SuAudioGraphExpandState state = {NULL,0,NULL,0,&first,1};
        uint32_t initial;
        CHECK(su_audio_graph_expand_ex(graph,1,77,13,0,&state,leaves,4,&count,NULL) == SU_OK);
        CHECK(count == 2 && leaves[0].node_id == leaves[1].node_id);
        initial = leaves[0].node_id;
        state.history = &second;
        CHECK(su_audio_graph_expand_ex(graph,1,77,13,0,&state,leaves,4,&count,NULL) == SU_OK && leaves[0].node_id == initial);
        state.history = &first;
        CHECK(su_audio_graph_expand_ex(graph,1,77,13,0,&state,leaves,4,&count,NULL) == SU_OK && leaves[0].node_id != initial);
    }
    su_audio_graph_release(graph);
    document(json, sizeof json, SOUND1, "{\"id\":1,\"root\":1}");
    for (index = 0; index < strlen(json); index++) {
        char previous = json[index];
        ((unsigned char *)json)[index] = 0xffu;
        CHECK(su_audio_graph_prepare(json,strlen(json),assets,&standard_limits,&graph,&diagnostic) == SU_E_PARSE);
        CHECK(graph == NULL);
        json[index] = previous;
    }
}

static void test_exact_ids_and_pins(void) {
    SuAudioGraph *graph = prepare("{\"id\":4294967295,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":1},\"gain\":0.25,\"pitch\":1e0,\"bus_id\":4,\"flags\":2},"
        "{\"id\":2147483647,\"type\":\"sound\",\"asset\":{\"id\":2,\"generation\":2}}",
        "{\"id\":4294967295,\"root\":4294967295},{\"id\":2147483647,\"root\":2147483647}");
    SuAudioGraphLeaf leaf;
    size_t count;
    CHECK(su_audio_graph_expand(graph, UINT32_MAX, 0, 0, 0, &leaf, 1, &count, NULL) == SU_OK);
    CHECK(leaf.node_id == UINT32_MAX && leaf.asset.id == 1 && leaf.gain == 0.25f && leaf.pitch == 1 && leaf.bus_id == 4 && leaf.flags == 2);
    CHECK(su_audio_graph_expand(graph, 2147483647, 0, 0, 0, &leaf, 1, &count, NULL) == SU_OK && leaf.asset.id == 2);
    CHECK(su_audio_assets_destroy(assets) == SU_E_RESOURCE_LIMIT);
    su_audio_graph_release(graph);
    CHECK(su_audio_asset_unload(assets, (SuAudioAssetRef){1,1}) == SU_OK);
    CHECK(su_audio_asset_unload(assets, (SuAudioAssetRef){2,2}) == SU_OK);
}

static void test_live_expansion(void) {
    SuAudioGraph *graph = prepare(SOUNDS "," SWITCH "," BLEND ","
        "{\"id\":40,\"type\":\"parallel\",\"children\":[20,30]}", "{\"id\":1,\"root\":40}");
    SuAudioParamDef parameter = {0};
    SuAudioSwitchDef sw = {0};
    SuAudioParamsDesc desc = {0};
    SuAudioParams *params = NULL;
    SuAudioParamScope global = {SU_AUDIO_SCOPE_GLOBAL, 0u};
    SuAudioParamTarget target = {0u, 0u};
    SuAudioGraphSwitch initial_switch = {7u, 5u};
    SuAudioGraphExpandState initial = {0};
    SuAudioGraphLeaf leaves[4], untouched[4];
    SuAudioGraphDiagnostic diagnostic;
    size_t count = 0u;
    uint32_t now;
    parameter.id = 8u;
    parameter.max_value = 10.0f;
    sw.id = 7u;
    sw.default_value = 5u;
    sw.policy = SU_AUDIO_SWITCH_FADE;
    sw.fade_frames = 512u;
    desc.struct_size = sizeof desc;
    desc.sample_rate = 48000u;
    desc.params = &parameter;
    desc.param_count = 1u;
    desc.switches = &sw;
    desc.switch_count = 1u;
    CHECK(su_audio_params_create(&desc, &params) == SU_OK);
    CHECK(su_audio_switch_set_at(params, global, 7u, 99u, 128u) == SU_OK);
    CHECK(su_audio_param_set_at(params, global, 8u, 10.0f, 256u) == SU_OK);
    initial.switches = &initial_switch;
    initial.switch_count = 1u;
#ifdef A03_TEST_ALLOCATORS
    expansion_allocations = 0u;
    expanding = 1;
#endif
    for (now = 0u; now < 1024u; now += 128u) {
        float new_gain = now < 128u ? 0.0f : now < 640u ? (float)(now - 128u) / 512.0f : 1.0f;
        CHECK(su_audio_graph_expand_live(graph, 1u, 3u, 0u, 9u, &initial, params, target, 0u, now,
            leaves, 4u, &count, NULL) == SU_OK);
        CHECK(count == 4u);
        CHECK(leaves[0].gain == 1.0f - new_gain && leaves[1].gain == new_gain);
        CHECK(leaves[2].gain == (now < 256u ? 1.0f : 0.0f));
        CHECK(leaves[3].gain == (now < 256u ? 0.0f : 1.0f));
    }
    memcpy(untouched, leaves, sizeof leaves);
    CHECK(su_audio_graph_expand_live(graph, 1u, 3u, 0u, 9u, &initial, params, target, 0u, 1024u,
        leaves, 3u, &count, &diagnostic) == SU_E_RESOURCE_LIMIT);
    CHECK(count == 0u && strcmp(diagnostic.bound, "capacity") == 0);
    CHECK(memcmp(leaves, untouched, sizeof leaves) == 0);
#ifdef A03_TEST_ALLOCATORS
    expanding = 0;
    CHECK(expansion_allocations == 0u);
#endif
    su_audio_params_destroy(params);
    su_audio_graph_release(graph);
}

int main(void) {
    float pcm[] = {0.0f,0.25f,-0.25f,0.0f};
    SuAudioClip clip = {pcm,4,1,48000};
    SuAudioAssetRef ref;
    CHECK(su_audio_assets_create(4,4096,&assets) == SU_OK);
    CHECK(su_audio_asset_prepare(assets,1,&clip,&ref) == SU_OK && ref.generation == 1);
    CHECK(su_audio_asset_prepare(assets,2,&clip,&ref) == SU_OK && ref.generation == 2);
    test_parser(); test_bounds(); test_expansion(); test_dag_boundaries();
    test_live_expansion();
    test_exact_ids_and_pins();
    CHECK(su_audio_assets_destroy(assets) == SU_OK);
    printf("audio_graph: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
