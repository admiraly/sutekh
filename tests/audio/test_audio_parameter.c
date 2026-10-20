#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sutekh/audio_parameter.h"

static unsigned checks;
static unsigned failures;

#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        failures++; \
        fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
    } \
} while (0)

#define RATE 48000u

static const SuAudioParamDef param_defs[] = {
    {1u, 0.0f, 1.0f, 0.0f, 100u, SU_AUDIO_PARAM_CLAMP},
    {2u, -1.0f, 1.0f, 0.5f, 0u, SU_AUDIO_PARAM_REJECT},
    {9u, 0.0f, 1.0f, 0.25f, 0u, SU_AUDIO_PARAM_CLAMP}
};

static const SuAudioSwitchDef switch_defs[] = {
    {5u, 0u, SU_AUDIO_SWITCH_CONTINUE, 0u},
    {6u, 1u, SU_AUDIO_SWITCH_FIRST_ONLY, 0u},
    {7u, 1u, SU_AUDIO_SWITCH_FADE, 100u}
};

static SuAudioParamScope scope(uint32_t kind, uint64_t key) {
    SuAudioParamScope result;
    result.kind = kind;
    result.key = key;
    return result;
}

static SuAudioParamTarget target(SuAudioEmitter emitter, SuAudioInstance instance) {
    SuAudioParamTarget result;
    result.emitter = emitter;
    result.instance = instance;
    return result;
}

static SuAudioParams *make_params(const SuAudioParamBinding *bindings, uint32_t binding_count, uint32_t scoped) {
    SuAudioParamsDesc desc;
    SuAudioParams *params = NULL;
    memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.sample_rate = RATE;
    desc.params = param_defs;
    desc.param_count = 3u;
    desc.switches = switch_defs;
    desc.switch_count = 3u;
    desc.bindings = bindings;
    desc.binding_count = binding_count;
    desc.max_scoped_values = scoped;
    CHECK(su_audio_params_create(&desc, &params) == SU_OK);
    return params;
}

static int near(float a, float b, float tolerance) {
    return fabsf(a - b) <= tolerance;
}

static void defaults_and_policies(void) {
    SuAudioParams *params = make_params(NULL, 0u, 4u);
    SuAudioParamTarget none = target(0u, 0u);
    SuAudioParamStats stats;
    SuAudioParamsDesc bad;
    SuAudioParamDef dup[2];
    SuAudioParams *other = NULL;
    float value = -9.0f;
    uint32_t sw = 99u;
    CHECK(su_audio_param_get(params, none, 1u, 0u, &value) == SU_OK && value == 0.0f);
    CHECK(su_audio_param_get(params, none, 2u, 1000u, &value) == SU_OK && value == 0.5f);
    CHECK(su_audio_param_get(params, none, 3u, 0u, &value) == SU_E_INVALID_ARGUMENT && value == 0.0f);
    CHECK(su_audio_switch_get(params, none, 6u, 0u, &sw) == SU_OK && sw == 1u);
    CHECK(su_audio_switch_get(params, none, 8u, 0u, &sw) == SU_E_INVALID_ARGUMENT);
    /* Clamp policy narrows, reject policy refuses, both leave state consistent. */
    CHECK(su_audio_param_set_at(params, scope(SU_AUDIO_SCOPE_GLOBAL, 0u), 9u, 4.0f, 10u) == SU_OK);
    CHECK(su_audio_param_get(params, none, 9u, 10u, &value) == SU_OK && value == 1.0f);
    CHECK(su_audio_param_set_at(params, scope(SU_AUDIO_SCOPE_GLOBAL, 0u), 2u, 1.5f, 10u) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_param_set_at(params, scope(SU_AUDIO_SCOPE_GLOBAL, 0u), 2u, (float)NAN, 10u) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_param_get(params, none, 2u, 10u, &value) == SU_OK && value == 0.5f);
    CHECK(su_audio_param_set_at(params, scope(SU_AUDIO_SCOPE_GLOBAL, 7u), 2u, 0.0f, 10u) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_param_set_at(params, scope(SU_AUDIO_SCOPE_EMITTER, 0u), 2u, 0.0f, 10u) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_param_set_at(params, scope(9u, 1u), 2u, 0.0f, 10u) == SU_E_INVALID_ARGUMENT);
    su_audio_params_stats(params, &stats);
    CHECK(stats.clamped == 1u && stats.rejected == 2u && stats.sets == 1u && stats.scoped_values == 0u);
    CHECK(stats.runtime_bytes > 0u && stats.scoped_capacity == 4u);
    su_audio_params_destroy(params);

    memset(&bad, 0, sizeof bad);
    bad.struct_size = sizeof bad;
    bad.sample_rate = RATE;
    dup[0] = param_defs[0];
    dup[1] = param_defs[0];
    bad.params = dup;
    bad.param_count = 2u;
    CHECK(su_audio_params_create(&bad, &other) == SU_E_DUPLICATE_KEY && other == NULL);
    dup[1].id = 4u;
    dup[1].default_value = 3.0f;
    CHECK(su_audio_params_create(&bad, &other) == SU_E_INVALID_ARGUMENT && other == NULL);
    bad.struct_size = 1u;
    CHECK(su_audio_params_create(&bad, &other) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_params_create(NULL, &other) == SU_E_INVALID_ARGUMENT);
}

static void smoothing_endpoints(void) {
    SuAudioParams *params = make_params(NULL, 0u, 4u);
    SuAudioParamTarget none = target(0u, 0u);
    float value = 0.0f;
    CHECK(su_audio_param_set_at(params, scope(SU_AUDIO_SCOPE_GLOBAL, 0u), 1u, 1.0f, 1000u) == SU_OK);
    CHECK(su_audio_param_get(params, none, 1u, 999u, &value) == SU_OK && value == 0.0f);
    CHECK(su_audio_param_get(params, none, 1u, 1000u, &value) == SU_OK && value == 0.0f);
    CHECK(su_audio_param_get(params, none, 1u, 1050u, &value) == SU_OK && value == 0.5f);
    CHECK(su_audio_param_get(params, none, 1u, 1100u, &value) == SU_OK && value == 1.0f);
    CHECK(su_audio_param_get(params, none, 1u, 900000u, &value) == SU_OK && value == 1.0f);
    /* A retarget mid-ramp starts from the current smoothed value: no step. */
   CHECK(su_audio_param_set_at(params, scope(SU_AUDIO_SCOPE_GLOBAL, 0u), 1u, 0.0f, 1075u) == SU_OK);
    CHECK(su_audio_param_get(params, none, 1u, 1075u, &value) == SU_OK && value == 0.75f);
    CHECK(su_audio_param_get(params, none, 1u, 1125u, &value) == SU_OK && near(value, 0.375f, 1e-6f));
    CHECK(su_audio_param_get(params, none, 1u, 1175u, &value) == SU_OK && value == 0.0f);
    /* Same-timestamp commands replace the last one. */
    CHECK(su_audio_param_set_at(params, scope(SU_AUDIO_SCOPE_GLOBAL, 0u), 1u, 1.0f, 1075u) == SU_OK);
    CHECK(su_audio_param_get(params, none, 1u, 1175u, &value) == SU_OK && value == 1.0f);
    /* Late commands are refused once the clock has advanced; reads of history stay exact. */
    CHECK(su_audio_params_advance(params, 2000u) == SU_OK);
    CHECK(su_audio_params_advance(params, 1999u) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_param_set_at(params, scope(SU_AUDIO_SCOPE_GLOBAL, 0u), 1u, 0.0f, 1999u) == SU_E_STALE_REVISION);
    CHECK(su_audio_param_get(params, none, 1u, 2000u, &value) == SU_OK && value == 1.0f);
    {
        uint32_t i;
        uint32_t status = SU_OK;
        for (i = 0u; i < SU_AUDIO_PARAM_PENDING + 1u && status == SU_OK; i++) {
            status = su_audio_param_set_at(params, scope(SU_AUDIO_SCOPE_GLOBAL, 0u), 9u, 0.1f * (float)i, 3000u + i);
        }
        CHECK(status == SU_E_RESOURCE_LIMIT && i == SU_AUDIO_PARAM_PENDING);
        CHECK(su_audio_params_advance(params, 3100u) == SU_OK);
        CHECK(su_audio_param_set_at(params, scope(SU_AUDIO_SCOPE_GLOBAL, 0u), 9u, 0.5f, 3100u) == SU_OK);
    }
    su_audio_params_destroy(params);
}

static void scoping(void) {
    SuAudioParams *params = make_params(NULL, 0u, 3u);
    const SuAudioEmitter a = 0x100000001u;
    const SuAudioEmitter b = 0x100000002u;
    const SuAudioInstance x = 0x200000001u;
    SuAudioParamStats stats;
    float value = 0.0f;
    uint32_t sw = 0u;
    CHECK(su_audio_param_set_at(params, scope(SU_AUDIO_SCOPE_GLOBAL, 0u), 9u, 0.1f, 0u) == SU_OK);
    CHECK(su_audio_param_set_at(params, scope(SU_AUDIO_SCOPE_EMITTER, a), 9u, 0.6f, 100u) == SU_OK);
    CHECK(su_audio_param_set_at(params, scope(SU_AUDIO_SCOPE_INSTANCE, x), 9u, 0.9f, 200u) == SU_OK);
    /* Before an override begins, lookup falls through to the next scope. */
    CHECK(su_audio_param_get(params, target(a, x), 9u, 50u, &value) == SU_OK && value == 0.1f);
    CHECK(su_audio_param_get(params, target(a, x), 9u, 150u, &value) == SU_OK && value == 0.6f);
    CHECK(su_audio_param_get(params, target(a, x), 9u, 250u, &value) == SU_OK && value == 0.9f);
    CHECK(su_audio_param_get(params, target(a, 0u), 9u, 250u, &value) == SU_OK && value == 0.6f);
    CHECK(su_audio_param_get(params, target(b, 0u), 9u, 250u, &value) == SU_OK && value == 0.1f);
    CHECK(su_audio_param_get(params, target(b, x), 9u, 250u, &value) == SU_OK && value == 0.9f);
    /* Emitter switch isolation. */
    CHECK(su_audio_switch_set_at(params, scope(SU_AUDIO_SCOPE_EMITTER, b), 5u, 3u, 10u) == SU_OK);
    CHECK(su_audio_switch_get(params, target(b, 0u), 5u, 10u, &sw) == SU_OK && sw == 3u);
    CHECK(su_audio_switch_get(params, target(a, 0u), 5u, 10u, &sw) == SU_OK && sw == 0u);
    /* Bounded scoped storage refuses without corrupting existing values. */
    CHECK(su_audio_param_set_at(params, scope(SU_AUDIO_SCOPE_EMITTER, b), 1u, 0.5f, 10u) == SU_E_RESOURCE_LIMIT);
    su_audio_params_stats(params, &stats);
    CHECK(stats.scoped_values == 3u);
    CHECK(su_audio_param_scope_clear(params, scope(SU_AUDIO_SCOPE_INSTANCE, x)) == SU_OK);
    CHECK(su_audio_param_get(params, target(a, x), 9u, 250u, &value) == SU_OK && value == 0.6f);
    CHECK(su_audio_param_scope_clear(params, scope(SU_AUDIO_SCOPE_GLOBAL, 0u)) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_param_set_at(params, scope(SU_AUDIO_SCOPE_EMITTER, b), 1u, 0.5f, 10u) == SU_OK);
    /* First scoped ramp starts from the inherited global value. */
    CHECK(su_audio_param_get(params, target(b, 0u), 1u, 60u, &value) == SU_OK && value == 0.25f);
    /* A refused write to a new scoped slot does not leak it. */
    CHECK(su_audio_params_advance(params, 500u) == SU_OK);
    CHECK(su_audio_param_scope_clear(params, scope(SU_AUDIO_SCOPE_EMITTER, b)) == SU_OK);
    CHECK(su_audio_param_set_at(params, scope(SU_AUDIO_SCOPE_EMITTER, b), 1u, 0.5f, 10u) == SU_E_STALE_REVISION);
    su_audio_params_stats(params, &stats);
    CHECK(stats.scoped_values == 1u);
    su_audio_params_destroy(params);
}

static void switch_policies(void) {
    SuAudioParams *params = make_params(NULL, 0u, 4u);
    const SuAudioEmitter e = 0x100000001u;
    SuAudioSwitchMix mix;
    CHECK(su_audio_switch_set_at(params, scope(SU_AUDIO_SCOPE_EMITTER, e), 6u, 2u, 500u) == SU_OK);
    CHECK(su_audio_switch_mix(params, target(e, 0u), 6u, 100u, 600u, &mix) == SU_OK);
    CHECK(mix.new_value == 1u && mix.new_gain == 1.0f && mix.old_gain == 0.0f);
    CHECK(su_audio_switch_mix(params, target(e, 0u), 6u, 500u, 600u, &mix) == SU_OK && mix.new_value == 2u);
    CHECK(su_audio_switch_set_at(params, scope(SU_AUDIO_SCOPE_EMITTER, e), 5u, 4u, 500u) == SU_OK);
    CHECK(su_audio_switch_mix(params, target(e, 0u), 5u, 100u, 499u, &mix) == SU_OK && mix.new_value == 0u);
    CHECK(su_audio_switch_mix(params, target(e, 0u), 5u, 100u, 500u, &mix) == SU_OK && mix.new_value == 4u
          && mix.new_gain == 1.0f);
    /* Fade: complementary gains from the change timestamp. */
    CHECK(su_audio_switch_set_at(params, scope(SU_AUDIO_SCOPE_EMITTER, e), 7u, 2u, 1000u) == SU_OK);
    CHECK(su_audio_switch_mix(params, target(e, 0u), 7u, 100u, 999u, &mix) == SU_OK
          && mix.new_value == 1u && mix.new_gain == 1.0f);
    CHECK(su_audio_switch_mix(params, target(e, 0u), 7u, 100u, 1000u, &mix) == SU_OK
          && mix.old_value == 1u && mix.new_value == 2u && mix.old_gain == 1.0f && mix.new_gain == 0.0f);
    CHECK(su_audio_switch_mix(params, target(e, 0u), 7u, 100u, 1050u, &mix) == SU_OK
          && mix.old_gain == 0.5f && mix.new_gain == 0.5f && mix.old_gain + mix.new_gain == 1.0f);
    CHECK(su_audio_switch_mix(params, target(e, 0u), 7u, 100u, 1100u, &mix) == SU_OK
          && mix.new_value == 2u && mix.new_gain == 1.0f && mix.old_gain == 0.0f);
    /* An instance started after the change has no fade tail. */
    CHECK(su_audio_switch_mix(params, target(e, 0u), 7u, 1000u, 1010u, &mix) == SU_OK
          && mix.new_value == 2u && mix.new_gain == 1.0f);
    /* Repeated changes: the newest change fades from the value immediately before it. */
    CHECK(su_audio_switch_set_at(params, scope(SU_AUDIO_SCOPE_EMITTER, e), 7u, 3u, 1040u) == SU_OK);
    CHECK(su_audio_switch_mix(params, target(e, 0u), 7u, 100u, 1065u, &mix) == SU_OK
          && mix.old_value == 2u && mix.new_value == 3u && mix.new_gain == 0.25f);
    CHECK(su_audio_switch_set_at(params, scope(SU_AUDIO_SCOPE_EMITTER, e), 7u, 3u, 1070u) == SU_OK);
    CHECK(su_audio_switch_mix(params, target(e, 0u), 7u, 100u, 1075u, &mix) == SU_OK
          && mix.new_value == 3u && mix.new_gain == 1.0f);
    /* Advance keeps the history needed by an unfinished fade. */
    CHECK(su_audio_params_advance(params, 1100u) == SU_OK);
    CHECK(su_audio_switch_mix(params, target(e, 0u), 7u, 100u, 1100u, &mix) == SU_OK
          && mix.new_value == 3u && mix.new_gain == 1.0f);
    CHECK(su_audio_switch_mix(params, target(e, 0u), 7u, 2000u, 1000u, &mix) == SU_E_INVALID_ARGUMENT);
    su_audio_params_destroy(params);
}

static const char graph_json[] =
    "{\"schema\":\"su_audio_events_v1\",\"numeric_profile\":\"audio_float_local_v1\","
    "\"seed_policy\":\"event_emitter_sequence_v1\",\"sample_rate\":48000,\"nodes\":["
    "{\"id\":1,\"type\":\"sound\",\"asset\":{\"id\":1,\"generation\":1}},"
    "{\"id\":2,\"type\":\"sound\",\"asset\":{\"id\":2,\"generation\":2}},"
    "{\"id\":3,\"type\":\"switch\",\"switch_id\":5,\"default\":1,\"cases\":[{\"value\":2,\"node\":2}]},"
    "{\"id\":4,\"type\":\"blend\",\"parameter_id\":9,\"points\":[{\"value\":0,\"node\":1},{\"value\":1,\"node\":2}]}],"
    "\"events\":[{\"id\":1,\"root\":3},{\"id\":2,\"root\":4}]}";

static void graph_selection(void) {
    SuAudioAssets *assets = NULL;
    SuAudioGraph *graph = NULL;
    SuAudioGraphLimits limits = {16u, 32u, 8u, 16u, 8192u};
    SuAudioGraphDiagnostic diagnostic;
    SuAudioAssetRef ref;
    float pcm[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    SuAudioClip clip = {pcm, 4u, 1u, RATE};
    SuAudioParams *params = make_params(NULL, 0u, 8u);
    SuAudioGraphParameter values[3];
    SuAudioGraphSwitch switches[3];
    SuAudioGraphExpandState state;
    SuAudioGraphLeaf leaves[4];
    size_t count = 0u;
    const SuAudioEmitter a = 0x100000001u;
    const SuAudioEmitter b = 0x100000002u;
    CHECK(su_audio_assets_create(4u, 4096u, &assets) == SU_OK);
    CHECK(su_audio_asset_prepare(assets, 1u, &clip, &ref) == SU_OK);
    CHECK(su_audio_asset_prepare(assets, 2u, &clip, &ref) == SU_OK);
    CHECK(su_audio_graph_prepare(graph_json, sizeof graph_json - 1u, assets, &limits, &graph, &diagnostic) == SU_OK);
    CHECK(su_audio_switch_set_at(params, scope(SU_AUDIO_SCOPE_EMITTER, a), 5u, 2u, 100u) == SU_OK);
    CHECK(su_audio_param_set_at(params, scope(SU_AUDIO_SCOPE_EMITTER, a), 9u, 0.75f, 100u) == SU_OK);
    CHECK(su_audio_param_expand_state(params, target(a, 0u), 100u, 100u, values, 3u, switches, 3u, &state) == SU_OK);
    CHECK(su_audio_graph_expand_ex(graph, 1u, 1u, a, 1u, &state, leaves, 4u, &count, &diagnostic) == SU_OK);
    CHECK(count == 1u && leaves[0].node_id == 2u);
    CHECK(su_audio_graph_expand_ex(graph, 2u, 1u, a, 2u, &state, leaves, 4u, &count, &diagnostic) == SU_OK);
    CHECK(count == 2u && near(leaves[0].gain, 0.25f, 1e-6f) && near(leaves[1].gain, 0.75f, 1e-6f));
    CHECK(su_audio_param_expand_state(params, target(b, 0u), 100u, 100u, values, 3u, switches, 3u, &state) == SU_OK);
    CHECK(su_audio_graph_expand_ex(graph, 1u, 1u, b, 3u, &state, leaves, 4u, &count, &diagnostic) == SU_OK);
    CHECK(count == 1u && leaves[0].node_id == 1u);
    CHECK(su_audio_graph_expand_ex(graph, 2u, 1u, b, 4u, &state, leaves, 4u, &count, &diagnostic) == SU_OK);
    CHECK(count == 2u && near(leaves[0].gain, 0.75f, 1e-6f));
    /* Before the scoped change the default case is selected. */
    CHECK(su_audio_param_expand_state(params, target(a, 0u), 99u, 99u, values, 3u, switches, 3u, &state) == SU_OK);
    CHECK(su_audio_graph_expand_ex(graph, 1u, 1u, a, 5u, &state, leaves, 4u, &count, &diagnostic) == SU_OK);
    CHECK(count == 1u && leaves[0].node_id == 1u);
    CHECK(su_audio_param_expand_state(params, target(a, 0u), 99u, 99u, values, 2u, switches, 3u, &state)
          == SU_E_RESOURCE_LIMIT);
    su_audio_params_destroy(params);
    su_audio_graph_release(graph);
    CHECK(su_audio_assets_destroy(assets) == SU_OK);
}

static void controls_and_pitch_continuity(void) {
    static const SuAudioBindingPoint pitch_points[] = {{0.0f, 1.0f}, {1.0f, 2.0f}};
    static const SuAudioBindingPoint gain_points[] = {{-1.0f, -12.0f}, {1.0f, 0.0f}};
    static const SuAudioBindingPoint lowpass_points[] = {{0.0f, 20000.0f}, {1.0f, 500.0f}};
    SuAudioParamBinding bindings[3];
    SuAudioParams *params;
    SuAudioParamControls controls;
    SuAudioParamVoice voice;
    float *ramp = (float *)malloc(8192u * sizeof *ramp);
    float *out = (float *)malloc(2048u * 2u * sizeof *out);
    SuAudioClip clip;
    uint32_t i;
    int continuous = 1;
    double min_step = 1.0;
    double max_step = 0.0;
    memset(bindings, 0, sizeof bindings);
    bindings[0].param_id = 1u;
    bindings[0].control = SU_AUDIO_CONTROL_PITCH;
    bindings[0].points = pitch_points;
    bindings[0].point_count = 2u;
    bindings[1].param_id = 2u;
    bindings[1].control = SU_AUDIO_CONTROL_GAIN_DB;
    bindings[1].points = gain_points;
    bindings[1].point_count = 2u;
    bindings[2].param_id = 9u;
    bindings[2].control = SU_AUDIO_CONTROL_BUS_GAIN_DB;
    bindings[2].bus_id = 3u;
    bindings[2].points = gain_points;
    bindings[2].point_count = 2u;
    params = make_params(bindings, 3u, 4u);
    CHECK(ramp != NULL && out != NULL);
    if (ramp == NULL || out == NULL || params == NULL) {
        free(ramp);
        free(out);
        su_audio_params_destroy(params);
        return;
    }
    CHECK(su_audio_param_controls(params, target(0u, 0u), 0u, &controls) == SU_OK);
    CHECK(controls.pitch == 1.0f && near(controls.gain_db, -3.0f, 1e-5f) && controls.bus_id == 3u);
    CHECK(near(controls.bus_gain_db, -4.5f, 1e-5f) && controls.lowpass_hz == 24000.0f);
    /* Lowpass bindings take the minimum cutoff; invalid curves are refused. */
    {
        SuAudioParamBinding lp = bindings[0];
        SuAudioParams *other;
        SuAudioParamsDesc desc;
        lp.control = SU_AUDIO_CONTROL_LOWPASS_HZ;
        lp.points = lowpass_points;
        other = make_params(&lp, 1u, 1u);
        CHECK(su_audio_param_set_at(other, scope(SU_AUDIO_SCOPE_GLOBAL, 0u), 1u, 1.0f, 0u) == SU_OK);
        CHECK(su_audio_param_controls(other, target(0u, 0u), 100u, &controls) == SU_OK && controls.lowpass_hz == 500.0f);
        su_audio_params_destroy(other);
        memset(&desc, 0, sizeof desc);
        desc.struct_size = sizeof desc;
        desc.sample_rate = RATE;
        desc.params = param_defs;
        desc.param_count = 3u;
        lp.points = gain_points;
        lp.control = SU_AUDIO_CONTROL_PITCH;
        desc.bindings = &lp;
        desc.binding_count = 1u;
        other = NULL;
        CHECK(su_audio_params_create(&desc, &other) == SU_E_INVALID_ARGUMENT && other == NULL);
        lp.param_id = 77u;
        lp.points = pitch_points;
        CHECK(su_audio_params_create(&desc, &other) == SU_E_INVALID_ARGUMENT && other == NULL);
    }
    /* Parameter-driven pitch ramps keep the cursor continuous: the output of a
     * linear ramp clip advances by exactly the per-frame pitch, never jumps. */
    for (i = 0u; i < 8192u; i++) {
        ramp[i] = (float)i * (1.0f / 1024.0f);
    }
    clip.samples = ramp;
    clip.frames = 8192u;
    clip.channels = 1u;
    clip.sample_rate = RATE;
    CHECK(su_audio_param_set_at(params, scope(SU_AUDIO_SCOPE_GLOBAL, 0u), 2u, 1.0f, 0u) == SU_OK);
    CHECK(su_audio_param_set_at(params, scope(SU_AUDIO_SCOPE_GLOBAL, 0u), 9u, 1.0f, 0u) == SU_OK);
    CHECK(su_audio_param_set_at(params, scope(SU_AUDIO_SCOPE_GLOBAL, 0u), 1u, 1.0f, 500u) == SU_OK);
    CHECK(su_audio_param_voice_init(&voice, &clip, target(0u, 0u), 0u, 0u) == SU_OK);
    for (i = 0u; i < 2048u; i += 256u) {
        CHECK(su_audio_param_voice_render(&voice, params, out + (size_t)i * 2u, 256u) == SU_OK);
    }
    for (i = 1u; i < 2048u; i++) {
        double step = ((double)out[i * 2u] - (double)out[(i - 1u) * 2u]) * 1024.0;
        if (step < min_step) {
            min_step = step;
        }
        if (step > max_step) {
            max_step = step;
        }
        if (i <= 500u && fabs(step - 1.0) > 1e-3) {
            continuous = 0;
        }
        if (i > 601u && fabs(step - 2.0) > 1e-3) {
            continuous = 0;
        }
        if (out[i * 2u] != out[i * 2u + 1u]) {
            continuous = 0;
        }
    }
    CHECK(continuous);
    CHECK(min_step > 0.999 && max_step < 2.001);
    /* Cursor equals the integral of the smoothed pitch: 500 + 100*1.5 + 1448*2. */
    CHECK(fabs(voice.cursor - (500.0 + 149.5 + 1448.0 * 2.0)) < 0.05);
    CHECK(su_audio_param_voice_init(&voice, NULL, target(0u, 0u), 0u, 0u) == SU_E_INVALID_ARGUMENT);
    {
        clock_t begin = clock();
        clock_t end;
        CHECK(su_audio_param_voice_init(&voice, &clip, target(0u, 0u), 0u, 1u) == SU_OK);
        for (i = 0u; i < 480000u; i += 1024u) {
            uint32_t n = 480000u - i < 1024u ? 480000u - i : 1024u;
            CHECK(su_audio_param_voice_render(&voice, params, out, n) == SU_OK);
        }
        end = clock();
        printf("param voice: 3 bindings, %.3f ms CPU per 48000 frames (10 s measured)\n",
               (double)(end - begin) * 100.0 / (double)CLOCKS_PER_SEC);
    }
    su_audio_params_destroy(params);
    free(ramp);
    free(out);
}

typedef struct MusicFixture {
    SuAudioAssets *assets;
    SuAudioMusic *music;
    float *a1;
    float *a2;
    float *b;
    float stinger[4];
} MusicFixture;

#define SEG_FRAMES 192000u

static void music_fixture(MusicFixture *f, uint64_t start) {
    SuAudioMusicDesc desc;
    SuAudioAssetRef ref;
    SuAudioClip clip;
    uint32_t i;
    memset(f, 0, sizeof *f);
    f->a1 = (float *)malloc(SEG_FRAMES * sizeof(float));
    f->a2 = (float *)malloc(SEG_FRAMES * sizeof(float));
    f->b = (float *)malloc(SEG_FRAMES * 2u * sizeof(float));
    CHECK(f->a1 != NULL && f->a2 != NULL && f->b != NULL);
    if (f->a1 == NULL || f->a2 == NULL || f->b == NULL) {
        return;
    }
    for (i = 0u; i < SEG_FRAMES; i++) {
        f->a1[i] = (float)(i % 1000u) * 0.0005f;
        f->a2[i] = 0.5f;
        f->b[i * 2u] = 2.0f;
        f->b[i * 2u + 1u] = 3.0f;
    }
    f->stinger[0] = 0.125f;
    f->stinger[1] = 0.25f;
    f->stinger[2] = 0.0f;
    f->stinger[3] = 0.0f;
    CHECK(su_audio_assets_create(8u, 8u * 1024u * 1024u, &f->assets) == SU_OK);
    clip.channels = 1u;
    clip.sample_rate = RATE;
    clip.frames = SEG_FRAMES;
    clip.samples = f->a1;
    CHECK(su_audio_asset_prepare(f->assets, 1u, &clip, &ref) == SU_OK);
    clip.samples = f->a2;
    CHECK(su_audio_asset_prepare(f->assets, 2u, &clip, &ref) == SU_OK);
    clip.samples = f->b;
    clip.channels = 2u;
    CHECK(su_audio_asset_prepare(f->assets, 3u, &clip, &ref) == SU_OK);
    clip.samples = f->stinger;
    clip.channels = 1u;
    clip.frames = 4u;
    CHECK(su_audio_asset_prepare(f->assets, 4u, &clip, &ref) == SU_OK);
    clip.sample_rate = 44100u;
    CHECK(su_audio_asset_prepare(f->assets, 5u, &clip, &ref) == SU_OK);
    memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.sample_rate = RATE;
    desc.max_segments = 4u;
    desc.start_sample = start;
    CHECK(su_audio_music_create(&desc, f->assets, &f->music) == SU_OK);
}

static void music_fixture_free(MusicFixture *f) {
    su_audio_music_destroy(f->music);
    if (f->assets != NULL) {
        CHECK(su_audio_assets_destroy(f->assets) == SU_OK);
    }
    free(f->a1);
    free(f->a2);
    free(f->b);
}

static void add_segments(MusicFixture *f) {
    static const SuAudioAssetRef stems_a[] = {{1u, 1u}, {2u, 2u}};
    static const SuAudioAssetRef stems_b[] = {{3u, 3u}};
    static const SuAudioTempoPoint tempo_a[] = {{0u, 120000u}, {8u, 90000u}};
    static const SuAudioTempoPoint tempo_b[] = {{0u, 133000u}};
    static const uint64_t cues[] = {1000u, 150000u};
    SuAudioMusicSegmentDesc seg;
    memset(&seg, 0, sizeof seg);
    seg.struct_size = sizeof seg;
    seg.segment_id = 10u;
    seg.stems = stems_a;
    seg.stem_count = 2u;
    seg.tempo = tempo_a;
    seg.tempo_count = 2u;
    seg.beats_per_bar = 4u;
    seg.cue_frames = cues;
    seg.cue_count = 2u;
    seg.length_frames = SEG_FRAMES;
    seg.loop = 1u;
    CHECK(su_audio_music_segment_add(f->music, &seg) == SU_OK);
    CHECK(su_audio_music_segment_add(f->music, &seg) == SU_E_DUPLICATE_KEY);
    seg.segment_id = 20u;
    seg.stems = stems_b;
    seg.stem_count = 1u;
    seg.tempo = tempo_b;
    seg.tempo_count = 1u;
    seg.cue_count = 0u;
    seg.loop = 0u;
    CHECK(su_audio_music_segment_add(f->music, &seg) == SU_OK);
}

static SuMusicTransition transition(uint32_t segment, uint32_t sync, uint32_t fade_in, uint32_t fade_out) {
    SuMusicTransition t;
    memset(&t, 0, sizeof t);
    t.struct_size = sizeof t;
    t.segment_id = segment;
    t.sync = sync;
    t.fade_in_frames = fade_in;
    t.fade_out_frames = fade_out;
    return t;
}

static void tempo_math(void) {
    MusicFixture f;
    uint64_t frame = 0u;
    uint64_t beat;
    int exact = 1;
    music_fixture(&f, 0u);
    add_segments(&f);
    CHECK(su_audio_music_beat_frame(f.music, 10u, 1u, &frame) == SU_OK && frame == 24000u);
    CHECK(su_audio_music_beat_frame(f.music, 10u, 8u, &frame) == SU_OK && frame == 192000u);
    CHECK(su_audio_music_beat_frame(f.music, 10u, 9u, &frame) == SU_OK && frame == 224000u);
    /* Long-duration sample clocks: no accumulated drift at non-integer frames per beat. */
    for (beat = 0u; beat <= 6000000u; beat += 999983u) {
        CHECK(su_audio_music_beat_frame(f.music, 20u, beat, &frame) == SU_OK);
        if (frame != beat * 2880000000u / 133000u) {
            exact = 0;
        }
    }
    CHECK(exact);
    CHECK(su_audio_music_beat_frame(f.music, 10u, 1000000000u, &frame) == SU_OK
          && frame == 192000u + ((uint64_t)1000000000u - 8u) * 32000u);
    CHECK(su_audio_music_beat_frame(f.music, 99u, 1u, &frame) == SU_E_INVALID_ARGUMENT);
    {
        static const SuAudioTempoPoint bad_tempo[] = {{1u, 120000u}};
        static const SuAudioTempoPoint slow[] = {{0u, 10u}};
        static const SuAudioAssetRef bad_stem[] = {{5u, 5u}};
        static const SuAudioAssetRef stem[] = {{2u, 2u}};
        SuAudioMusicSegmentDesc seg;
        memset(&seg, 0, sizeof seg);
        seg.struct_size = sizeof seg;
        seg.segment_id = 30u;
        seg.stems = stem;
        seg.stem_count = 1u;
        seg.tempo = bad_tempo;
        seg.tempo_count = 1u;
        seg.beats_per_bar = 4u;
        seg.length_frames = 100u;
        CHECK(su_audio_music_segment_add(f.music, &seg) == SU_E_INVALID_ARGUMENT);
        seg.tempo = slow;
        CHECK(su_audio_music_segment_add(f.music, &seg) == SU_E_INVALID_ARGUMENT);
       seg.stems = bad_stem;
        {
            static const SuAudioTempoPoint ok[] = {{0u, 60000u}};
            seg.tempo = ok;
            CHECK(su_audio_music_segment_add(f.music, &seg) == SU_E_SCHEMA_MISMATCH);
        }
    }
    music_fixture_free(&f);
}

static void music_transitions(void) {
    MusicFixture f;
    SuMusicTransition t;
    SuAudioMusicState state;
    uint64_t boundary = 0u;
    float *out = (float *)malloc(SEG_FRAMES * 2u * sizeof *out);
    int ok = 1;
    uint32_t i;
    music_fixture(&f, 0u);
    add_segments(&f);
    CHECK(out != NULL);
    if (out == NULL) {
        music_fixture_free(&f);
        return;
    }
    t = transition(10u, SU_AUDIO_MUSIC_BAR, 0u, 0u);
    CHECK(su_audio_music_transition(f.music, &t, 0u, &boundary) == SU_OK && boundary == 0u);
    CHECK(su_audio_music_render(f.music, out, 30000u) == SU_OK);
    /* Stems stay sample-locked: the mix is exactly stem1[f] + stem2[f]. */
    for (i = 0u; i < 30000u; i++) {
        float expected = f.a1[i] + f.a2[i];
        if (out[i * 2u] != expected || out[i * 2u + 1u] != expected) {
            ok = 0;
        }
    }
    CHECK(ok);
    /* Beat and bar boundaries from the tempo map; a newer request replaces the pending one. */
    t = transition(20u, SU_AUDIO_MUSIC_BEAT, 1000u, 1000u);
    CHECK(su_audio_music_transition(f.music, &t, 30000u, &boundary) == SU_OK && boundary == 48000u);
    CHECK(su_audio_music_transition(f.music, &t, 48001u, &boundary) == SU_OK && boundary == 72000u);
    t.sync = SU_AUDIO_MUSIC_BAR;
    CHECK(su_audio_music_transition(f.music, &t, 30000u, &boundary) == SU_OK && boundary == 96000u);
    t.sync = SU_AUDIO_MUSIC_CUE;
    t.cue = 0u;
    CHECK(su_audio_music_transition(f.music, &t, 30000u, &boundary) == SU_E_STALE_REVISION);
    t.late_policy = SU_AUDIO_MUSIC_LATE_NEXT;
    CHECK(su_audio_music_transition(f.music, &t, 30000u, &boundary) == SU_OK && boundary == SEG_FRAMES + 1000u);
    t.late_policy = SU_AUDIO_MUSIC_LATE_IMMEDIATE;
    CHECK(su_audio_music_transition(f.music, &t, 30000u, &boundary) == SU_OK && boundary == 30000u);
    t.cue = 2u;
    CHECK(su_audio_music_transition(f.music, &t, 30000u, &boundary) == SU_E_INVALID_ARGUMENT);
    t.cue = 1u;
    CHECK(su_audio_music_transition(f.music, &t, 30000u, &boundary) == SU_OK && boundary == 150000u);
    t.sync = SU_AUDIO_MUSIC_SEGMENT_END;
    CHECK(su_audio_music_transition(f.music, &t, 30000u, &boundary) == SU_OK && boundary == SEG_FRAMES);
    /* Late request (before the clock) under the reject policy. */
    t.sync = SU_AUDIO_MUSIC_BAR;
    t.late_policy = SU_AUDIO_MUSIC_LATE_REJECT;
    CHECK(su_audio_music_transition(f.music, &t, 29999u, &boundary) == SU_E_STALE_REVISION);
    /* Final choice: next bar with a 1000-frame crossfade and a stinger at the boundary. */
    t.stinger.id = 4u;
    t.stinger.generation = 4u;
    t.stinger_gain = 2.0f;
    CHECK(su_audio_music_transition(f.music, &t, 30000u, &boundary) == SU_OK && boundary == 96000u);
    su_audio_music_state(f.music, &state);
    CHECK(state.pending_segment_id == 20u && state.pending_at == 96000u && state.segment_id == 10u);
    CHECK(su_audio_music_render(f.music, out, 70000u) == SU_OK);
    ok = 1;
    for (i = 0u; i < 70000u; i++) {
        uint64_t at = 30000u + i;
        double g = at < 96000u ? 0.0 : (at - 96000u >= 1000u ? 1.0 : (double)(at - 96000u) / 1000.0);
        double old = at < 97000u ? (double)(f.a1[at] + f.a2[at]) * (1.0 - g) : 0.0;
        double sting = at >= 96000u && at < 96004u ? (double)f.stinger[at - 96000u] * 2.0 : 0.0;
        double left = old + 2.0 * g + sting;
        double right = old + 3.0 * g + sting;
        if (fabs(out[i * 2u] - left) > 1e-5 || fabs(out[i * 2u + 1u] - right) > 1e-5) {
            if (ok) {
                fprintf(stderr, "music mismatch at %llu: %f %f vs %f %f\n", (unsigned long long)at,
                        (double)out[i * 2u], (double)out[i * 2u + 1u], left, right);
            }
            ok = 0;
        }
    }
    CHECK(ok);
    /* Exact sample at the boundary: old at full gain, new at zero, stinger onset. */
    CHECK(out[(96000u - 30000u) * 2u] == f.a1[96000u] + f.a2[96000u] + 0.25f);
    su_audio_music_state(f.music, &state);
    CHECK(state.segment_id == 20u && state.segment_start == 96000u && state.fading_segment_id == 0u);
    CHECK(state.stingers == 0u && state.pending_segment_id == 0u && state.clock == 100000u);
    /* Non-looping segment end: after it, transitions are immediate. */
    t = transition(10u, SU_AUDIO_MUSIC_BAR, 0u, 0u);
    CHECK(su_audio_music_transition(f.music, &t, 100000u, &boundary) == SU_OK);
    CHECK(boundary == 96000u + (uint64_t)4u * 2880000000u / 133000u);
    t.segment_id = 77u;
    CHECK(su_audio_music_transition(f.music, &t, 100000u, &boundary) == SU_E_INVALID_ARGUMENT);
    free(out);
    music_fixture_free(&f);
}

static void music_large_clock(void) {
    MusicFixture f;
    SuMusicTransition t;
    uint64_t boundary = 0u;
    float out[256];
    const uint64_t start = (uint64_t)1u << 40;
    music_fixture(&f, start);
    add_segments(&f);
    t = transition(10u, SU_AUDIO_MUSIC_IMMEDIATE, 0u, 0u);
    CHECK(su_audio_music_transition(f.music, &t, start + 5u, &boundary) == SU_OK && boundary == start + 5u);
    CHECK(su_audio_music_render(f.music, out, 128u) == SU_OK);
    CHECK(out[0] == 0.0f && out[9] == 0.0f && out[10] == f.a1[0] + f.a2[0]);
    t = transition(20u, SU_AUDIO_MUSIC_BEAT, 64u, 64u);
    CHECK(su_audio_music_transition(f.music, &t, start + 128u, &boundary) == SU_OK && boundary == start + 5u + 24000u);
    /* While a crossfade is active, later boundaries wait for its end. */
    {
        uint32_t i;
        for (i = 0u; i < 200u; i++) {
            CHECK(su_audio_music_render(f.music, out, 128u) == SU_OK);
        }
    }
    t = transition(10u, SU_AUDIO_MUSIC_IMMEDIATE, 0u, 0u);
    CHECK(su_audio_music_transition(f.music, &t, start + 5u + 24000u + 10u, &boundary) == SU_E_STALE_REVISION);
    t.late_policy = SU_AUDIO_MUSIC_LATE_IMMEDIATE;
    CHECK(su_audio_music_transition(f.music, &t, start + 5u + 24000u + 10u, &boundary) == SU_OK
          && boundary == start + 128u * 201u);
    music_fixture_free(&f);
}

static void music_cost(void) {
    MusicFixture f;
    SuMusicTransition t;
    uint64_t boundary;
    float *out = (float *)malloc(1024u * 2u * sizeof *out);
    clock_t begin;
    clock_t end;
    uint32_t i;
    music_fixture(&f, 0u);
    add_segments(&f);
    if (out == NULL) {
        music_fixture_free(&f);
        return;
    }
    t = transition(10u, SU_AUDIO_MUSIC_IMMEDIATE, 0u, 0u);
    CHECK(su_audio_music_transition(f.music, &t, 0u, &boundary) == SU_OK);
    CHECK(su_audio_music_render(f.music, out, 1000u) == SU_OK);
    t = transition(20u, SU_AUDIO_MUSIC_BEAT, 24000u, 24000u);
    CHECK(su_audio_music_transition(f.music, &t, 1000u, &boundary) == SU_OK);
    begin = clock();
    for (i = 0u; i < 480000u; i += 1000u) {
        CHECK(su_audio_music_render(f.music, out, 1000u) == SU_OK);
    }
    end = clock();
    printf("music: 3 stems, crossfade, loop wrap: %.3f ms CPU per 48000 frames (10 s measured)\n",
           (double)(end - begin) * 100.0 / (double)CLOCKS_PER_SEC);
    free(out);
    music_fixture_free(&f);
}

int main(void) {
    defaults_and_policies();
    smoothing_endpoints();
    scoping();
    switch_policies();
    graph_selection();
    controls_and_pitch_continuity();
    tempo_math();
    music_transitions();
    music_large_clock();
    music_cost();
    printf("audio_parameter: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
