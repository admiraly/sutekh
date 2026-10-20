#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/audio_parameter.h"

/* One timeline segment. Parameters ramp linearly from `from` at `start` to
 * `to` at `end`; switches step to `value` at `start`. */
typedef struct ParamSegment {
    uint64_t start;
    uint64_t end;
    float from;
    float to;
    uint32_t value;
} ParamSegment;

typedef struct ParamSlot {
    uint32_t used;
    uint32_t kind;
    uint64_t key;
    uint32_t is_switch;
    uint32_t def_index;
    uint32_t count;
    ParamSegment seg[SU_AUDIO_PARAM_PENDING];
} ParamSlot;

typedef struct ParamBinding {
    SuAudioParamBinding desc;
    uint32_t def_index;
    SuAudioBindingPoint points[SU_AUDIO_PARAM_MAX_BINDING_POINTS];
} ParamBinding;

struct SuAudioParams {
    SuAudioParamsDesc desc;
    SuAudioParamDef *params;
    SuAudioSwitchDef *switches;
    ParamBinding *bindings;
    ParamSlot *global;   /* param_count + switch_count */
    ParamSlot *scoped;   /* max_scoped_values */
    uint64_t clock;
    SuAudioParamStats stats;
};

static int find_param(const SuAudioParams *params, SuAudioParamId id, uint32_t *index) {
    uint32_t i;
    for (i = 0u; i < params->desc.param_count; i++) {
        if (params->params[i].id == id) {
            *index = i;
            return 1;
        }
    }
    return 0;
}

static int find_switch(const SuAudioParams *params, SuAudioSwitchId id, uint32_t *index) {
    uint32_t i;
    for (i = 0u; i < params->desc.switch_count; i++) {
        if (params->switches[i].id == id) {
            *index = i;
            return 1;
        }
    }
    return 0;
}

uint32_t su_audio_params_create(const SuAudioParamsDesc *desc, SuAudioParams **out) {
    SuAudioParams *params;
    uint32_t i;
    uint32_t j;
    size_t bytes;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (desc == NULL || desc->struct_size != sizeof *desc || desc->sample_rate == 0u
        || desc->param_count > SU_AUDIO_PARAM_MAX_DEFS || desc->switch_count > SU_AUDIO_PARAM_MAX_DEFS
        || desc->binding_count > SU_AUDIO_PARAM_MAX_DEFS || desc->max_scoped_values > 1048576u
        || (desc->param_count != 0u && desc->params == NULL)
        || (desc->switch_count != 0u && desc->switches == NULL)
        || (desc->binding_count != 0u && desc->bindings == NULL)) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (i = 0u; i < desc->param_count; i++) {
        const SuAudioParamDef *def = &desc->params[i];
        if (!isfinite(def->min_value) || !isfinite(def->max_value) || !isfinite(def->default_value)
            || def->min_value > def->max_value || def->default_value < def->min_value
            || def->default_value > def->max_value || def->range_policy > SU_AUDIO_PARAM_REJECT) {
            return SU_E_INVALID_ARGUMENT;
        }
        for (j = 0u; j < i; j++) {
            if (desc->params[j].id == def->id) {
                return SU_E_DUPLICATE_KEY;
            }
        }
    }
    for (i = 0u; i < desc->switch_count; i++) {
        if (desc->switches[i].policy > SU_AUDIO_SWITCH_FADE) {
            return SU_E_INVALID_ARGUMENT;
        }
        for (j = 0u; j < i; j++) {
            if (desc->switches[j].id == desc->switches[i].id) {
                return SU_E_DUPLICATE_KEY;
            }
        }
    }
    params = (SuAudioParams *)calloc(1u, sizeof *params);
    if (params == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    params->desc = *desc;
    params->clock = desc->start_sample;
    params->params = (SuAudioParamDef *)calloc(desc->param_count + 1u, sizeof *params->params);
    params->switches = (SuAudioSwitchDef *)calloc(desc->switch_count + 1u, sizeof *params->switches);
    params->bindings = (ParamBinding *)calloc(desc->binding_count + 1u, sizeof *params->bindings);
    params->global = (ParamSlot *)calloc(desc->param_count + desc->switch_count + 1u, sizeof *params->global);
    params->scoped = (ParamSlot *)calloc(desc->max_scoped_values + 1u, sizeof *params->scoped);
    if (params->params == NULL || params->switches == NULL || params->bindings == NULL
        || params->global == NULL || params->scoped == NULL) {
        su_audio_params_destroy(params);
        return SU_E_RESOURCE_LIMIT;
    }
    if (desc->param_count != 0u) {
        memcpy(params->params, desc->params, desc->param_count * sizeof *params->params);
    }
    if (desc->switch_count != 0u) {
        memcpy(params->switches, desc->switches, desc->switch_count * sizeof *params->switches);
    }
    params->desc.params = params->params;
    params->desc.switches = params->switches;
    for (i = 0u; i < desc->binding_count; i++) {
        const SuAudioParamBinding *in = &desc->bindings[i];
        ParamBinding *binding = &params->bindings[i];
        if (!find_param(params, in->param_id, &binding->def_index) || in->control > SU_AUDIO_CONTROL_BUS_GAIN_DB
            || in->points == NULL || in->point_count == 0u || in->point_count > SU_AUDIO_PARAM_MAX_BINDING_POINTS) {
            su_audio_params_destroy(params);
            return SU_E_INVALID_ARGUMENT;
        }
        for (j = 0u; j < in->point_count; j++) {
            float output = in->points[j].output;
            if (!isfinite(in->points[j].value) || !isfinite(output)
                || (j > 0u && !(in->points[j].value > in->points[j - 1u].value))
                || ((in->control == SU_AUDIO_CONTROL_PITCH) && !(output > 0.0f && output <= 64.0f))
                || ((in->control == SU_AUDIO_CONTROL_LOWPASS_HZ) && !(output > 0.0f))) {
                su_audio_params_destroy(params);
                return SU_E_INVALID_ARGUMENT;
            }
            binding->points[j] = in->points[j];
        }
        binding->desc = *in;
        binding->desc.points = NULL;
    }
    params->desc.bindings = NULL;
    for (i = 0u; i < desc->param_count; i++) {
        ParamSlot *slot = &params->global[i];
        slot->used = 1u;
        slot->def_index = i;
        slot->count = 1u;
        slot->seg[0].from = slot->seg[0].to = params->params[i].default_value;
    }
    for (i = 0u; i < desc->switch_count; i++) {
        ParamSlot *slot = &params->global[desc->param_count + i];
        slot->used = 1u;
        slot->is_switch = 1u;
        slot->def_index = i;
        slot->count = 1u;
        slot->seg[0].value = params->switches[i].default_value;
    }
    bytes = sizeof *params + (desc->param_count + 1u) * sizeof *params->params
        + (desc->switch_count + 1u) * sizeof *params->switches
        + (desc->binding_count + 1u) * sizeof *params->bindings
        + (desc->param_count + desc->switch_count + 1u) * sizeof *params->global
        + (desc->max_scoped_values + 1u) * sizeof *params->scoped;
    params->stats.runtime_bytes = (uint64_t)bytes;
    params->stats.scoped_capacity = desc->max_scoped_values;
    *out = params;
    return SU_OK;
}

void su_audio_params_destroy(SuAudioParams *params) {
    if (params == NULL) {
        return;
    }
    free(params->params);
    free(params->switches);
    free(params->bindings);
    free(params->global);
    free(params->scoped);
    free(params);
}

static float segment_value(const ParamSegment *seg, uint64_t t) {
    double span;
    double offset;
    if (t >= seg->end || seg->end == seg->start) {
        return seg->to;
    }
    if (t <= seg->start) {
        return seg->from;
    }
    span = (double)(seg->end - seg->start);
    offset = (double)(t - seg->start);
    return (float)((double)seg->from + ((double)seg->to - (double)seg->from) * (offset / span));
}

/* Index of the last segment starting at or before t, or -1. */
static int slot_segment(const ParamSlot *slot, uint64_t t) {
    int index = (int)slot->count - 1;
    while (index >= 0 && slot->seg[index].start > t) {
        index--;
    }
    return index;
}

static ParamSlot *scoped_slot(const SuAudioParams *params, uint32_t kind, uint64_t key,
                              uint32_t is_switch, uint32_t def_index) {
    uint32_t i;
    for (i = 0u; i < params->desc.max_scoped_values; i++) {
        ParamSlot *slot = &params->scoped[i];
        if (slot->used && slot->kind == kind && slot->key == key && slot->is_switch == is_switch
            && slot->def_index == def_index) {
            return slot;
        }
    }
    return NULL;
}

static ParamSlot *global_slot(const SuAudioParams *params, uint32_t is_switch, uint32_t def_index) {
    return &params->global[is_switch ? params->desc.param_count + def_index : def_index];
}

/* Resolves instance -> emitter -> global at time t; returns slot and segment. */
static const ParamSlot *resolve(const SuAudioParams *params, SuAudioParamTarget target, uint32_t is_switch,
                                uint32_t def_index, uint64_t t, int *segment) {
    const ParamSlot *slot;
    int index;
    if (target.instance != 0u) {
        slot = scoped_slot(params, SU_AUDIO_SCOPE_INSTANCE, target.instance, is_switch, def_index);
        if (slot != NULL && (index = slot_segment(slot, t)) >= 0) {
            *segment = index;
            return slot;
        }
    }
    if (target.emitter != 0u) {
        slot = scoped_slot(params, SU_AUDIO_SCOPE_EMITTER, target.emitter, is_switch, def_index);
        if (slot != NULL && (index = slot_segment(slot, t)) >= 0) {
            *segment = index;
            return slot;
        }
    }
    slot = global_slot(params, is_switch, def_index);
    index = slot_segment(slot, t);
    *segment = index < 0 ? 0 : index;
    return slot;
}

static float param_value(const SuAudioParams *params, SuAudioParamTarget target, uint32_t def_index, uint64_t t) {
    int index = 0;
    const ParamSlot *slot = resolve(params, target, 0u, def_index, t, &index);
    if (slot->seg[index].start > t) {
        return slot->seg[index].from;
    }
    return segment_value(&slot->seg[index], t);
}

static uint32_t switch_value(const SuAudioParams *params, SuAudioParamTarget target, uint32_t def_index, uint64_t t) {
    int index = 0;
    const ParamSlot *slot = resolve(params, target, 1u, def_index, t, &index);
    return slot->seg[index].value;
}

static uint32_t scope_slot_for_write(SuAudioParams *params, SuAudioParamScope scope, uint32_t is_switch,
                                     uint32_t def_index, ParamSlot **out) {
    uint32_t i;
    ParamSlot *slot;
    if (scope.kind == SU_AUDIO_SCOPE_GLOBAL) {
        if (scope.key != 0u) {
            return SU_E_INVALID_ARGUMENT;
        }
        *out = global_slot(params, is_switch, def_index);
        return SU_OK;
    }
    if ((scope.kind != SU_AUDIO_SCOPE_EMITTER && scope.kind != SU_AUDIO_SCOPE_INSTANCE) || scope.key == 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    slot = scoped_slot(params, scope.kind, scope.key, is_switch, def_index);
    if (slot != NULL) {
        *out = slot;
        return SU_OK;
    }
    for (i = 0u; i < params->desc.max_scoped_values; i++) {
        slot = &params->scoped[i];
        if (!slot->used) {
            memset(slot, 0, sizeof *slot);
            slot->used = 1u;
            slot->kind = scope.kind;
            slot->key = scope.key;
            slot->is_switch = is_switch;
            slot->def_index = def_index;
            params->stats.scoped_values++;
            *out = slot;
            return SU_OK;
        }
    }
    params->stats.rejected++;
    return SU_E_RESOURCE_LIMIT;
}

/* Common ordering checks; returns the segment index to write. */
static uint32_t prepare_write(SuAudioParams *params, ParamSlot *slot, uint64_t t, uint32_t *index) {
    if (t < params->clock) {
        params->stats.late++;
        return SU_E_STALE_REVISION;
    }
    if (slot->count != 0u && t < slot->seg[slot->count - 1u].start) {
        params->stats.late++;
        return SU_E_STALE_REVISION;
    }
    if (slot->count != 0u && t == slot->seg[slot->count - 1u].start) {
        *index = slot->count - 1u;
        return SU_OK;
    }
    if (slot->count == SU_AUDIO_PARAM_PENDING) {
        params->stats.rejected++;
        return SU_E_RESOURCE_LIMIT;
    }
    *index = slot->count;
    return SU_OK;
}

uint32_t su_audio_param_set_at(SuAudioParams *params, SuAudioParamScope scope, SuAudioParamId id,
                               float value, uint64_t sample_time) {
    uint32_t def_index;
    uint32_t index = 0u;
    uint32_t status;
    const SuAudioParamDef *def;
    ParamSlot *slot = NULL;
    ParamSegment *seg;
    float from;
    if (params == NULL || !find_param(params, id, &def_index)) {
        return SU_E_INVALID_ARGUMENT;
    }
    def = &params->params[def_index];
    if (!isfinite(value) || sample_time > UINT64_MAX - def->smoothing_frames) {
        params->stats.rejected++;
        return SU_E_INVALID_ARGUMENT;
    }
    if (value < def->min_value || value > def->max_value) {
        if (def->range_policy == SU_AUDIO_PARAM_REJECT) {
            params->stats.rejected++;
            return SU_E_INVALID_ARGUMENT;
        }
        value = value < def->min_value ? def->min_value : def->max_value;
        params->stats.clamped++;
    }
    status = scope_slot_for_write(params, scope, 0u, def_index, &slot);
    if (status != SU_OK) {
        return status;
    }
    status = prepare_write(params, slot, sample_time, &index);
    if (status != SU_OK) {
        if (slot->count == 0u) {
            slot->used = 0u;
            params->stats.scoped_values--;
        }
        return status;
    }
    if (index > 0u) {
        from = segment_value(&slot->seg[index - 1u], sample_time);
    } else if (slot->count != 0u) {
        /* Replacing the only segment at the same timestamp keeps its origin. */
        from = slot->seg[0].from;
    } else {
        SuAudioParamTarget global_target = {0u, 0u};
        from = param_value(params, global_target, def_index, sample_time);
    }
    seg =&slot->seg[index];
    seg->start = sample_time;
    seg->end = sample_time + def->smoothing_frames;
    seg->from = from;
    seg->to = value;
    seg->value = 0u;
    if (index == slot->count) {
        slot->count++;
    }
    params->stats.sets++;
    return SU_OK;
}

uint32_t su_audio_switch_set_at(SuAudioParams *params, SuAudioParamScope scope, SuAudioSwitchId id,
                                uint32_t value, uint64_t sample_time) {
    uint32_t def_index;
    uint32_t index = 0u;
    uint32_t status;
    ParamSlot *slot = NULL;
    if (params == NULL || !find_switch(params, id, &def_index)) {
        return SU_E_INVALID_ARGUMENT;
    }
    status = scope_slot_for_write(params, scope, 1u, def_index, &slot);
    if (status != SU_OK) {
        return status;
    }
    status = prepare_write(params, slot, sample_time, &index);
    if (status != SU_OK) {
        if (slot->count == 0u) {
            slot->used = 0u;
            params->stats.scoped_values--;
        }
        return status;
    }
    memset(&slot->seg[index], 0, sizeof slot->seg[index]);
    slot->seg[index].start = sample_time;
    slot->seg[index].end = sample_time;
    slot->seg[index].value = value;
    if (index == slot->count) {
        slot->count++;
    }
    params->stats.sets++;
    return SU_OK;
}

uint32_t su_audio_param_get(const SuAudioParams *params, SuAudioParamTarget target, SuAudioParamId id,
                            uint64_t sample_time, float *out) {
    uint32_t def_index;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = 0.0f;
    if (params == NULL || !find_param(params, id, &def_index)) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = param_value(params, target, def_index, sample_time);
    return SU_OK;
}

uint32_t su_audio_switch_get(const SuAudioParams *params, SuAudioParamTarget target, SuAudioSwitchId id,
                             uint64_t sample_time, uint32_t *out) {
    uint32_t def_index;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = 0u;
    if (params == NULL || !find_switch(params, id, &def_index)) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = switch_value(params, target, def_index, sample_time);
    return SU_OK;
}

uint32_t su_audio_switch_mix(const SuAudioParams *params, SuAudioParamTarget target, SuAudioSwitchId id,
                             uint64_t instance_start, uint64_t sample_time, SuAudioSwitchMix *out) {
    uint32_t def_index;
    const SuAudioSwitchDef *def;
    const ParamSlot *slot;
    int index = 0;
    uint64_t change;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    if (params == NULL || !find_switch(params, id, &def_index) || sample_time < instance_start) {
        return SU_E_INVALID_ARGUMENT;
    }
    def = &params->switches[def_index];
    out->new_gain = 1.0f;
    if (def->policy == SU_AUDIO_SWITCH_FIRST_ONLY) {
        out->old_value = out->new_value = switch_value(params, target, def_index, instance_start);
        return SU_OK;
    }
    out->old_value = out->new_value = switch_value(params, target, def_index, sample_time);
    if (def->policy == SU_AUDIO_SWITCH_CONTINUE || def->fade_frames == 0u) {
        return SU_OK;
    }
    slot = resolve(params, target, 1u, def_index, sample_time, &index);
    change = slot->seg[index].start;
    if (change <= instance_start || change > sample_time || sample_time - change >= def->fade_frames) {
        return SU_OK;
    }
    out->old_value = switch_value(params, target, def_index, change - 1u);
    if (out->old_value == out->new_value) {
        return SU_OK;
    }
    out->new_gain = (float)((double)(sample_time - change) / (double)def->fade_frames);
    out->old_gain = 1.0f - out->new_gain;
    return SU_OK;
}

static float binding_eval(const ParamBinding *binding, float value) {
    uint32_t i;
    const SuAudioBindingPoint *p = binding->points;
    uint32_t n = binding->desc.point_count;
    if (value <= p[0].value) {
        return p[0].output;
    }
    if (value >= p[n - 1u].value) {
        return p[n - 1u].output;
    }
    for (i = 1u; i < n; i++) {
        if (value <= p[i].value) {
            double f = ((double)value - p[i - 1u].value) / ((double)p[i].value - p[i - 1u].value);
            return (float)(p[i - 1u].output + (p[i].output - p[i - 1u].output) * f);
        }
    }
    return p[n - 1u].output;
}

uint32_t su_audio_param_controls(const SuAudioParams *params, SuAudioParamTarget target,
                                 uint64_t sample_time, SuAudioParamControls *out) {
    uint32_t i;
    int bus_set = 0;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    out->pitch = 1.0f;
    if (params == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    out->lowpass_hz = (float)params->desc.sample_rate * 0.5f;
    for (i = 0u; i < params->desc.binding_count; i++) {
        const ParamBinding *binding = &params->bindings[i];
        float value = param_value(params, target, binding->def_index, sample_time);
        float mapped = binding_eval(binding, value);
        switch (binding->desc.control) {
        case SU_AUDIO_CONTROL_GAIN_DB:
            out->gain_db += mapped;
            break;
        case SU_AUDIO_CONTROL_PITCH:
            out->pitch *= mapped;
            break;
        case SU_AUDIO_CONTROL_LOWPASS_HZ:
            if (mapped < out->lowpass_hz) {
                out->lowpass_hz = mapped;
            }
            break;
        default:
            if (!bus_set) {
                out->bus_id = binding->desc.bus_id;
                bus_set = 1;
            }
            if (binding->desc.bus_id == out->bus_id) {
                out->bus_gain_db += mapped;
            }
            break;
        }
    }
    if (out->pitch > 64.0f) {
        out->pitch = 64.0f;
    }
    return SU_OK;
}

uint32_t su_audio_params_advance(SuAudioParams *params, uint64_t sample_time) {
    uint32_t i;
    uint32_t total;
    if (params == NULL || sample_time < params->clock) {
        return SU_E_INVALID_ARGUMENT;
    }
    params->clock = sample_time;
    total = params->desc.param_count + params->desc.switch_count;
    for (i = 0u; i < total + params->desc.max_scoped_values; i++) {
        ParamSlot *slot = i < total ? &params->global[i] : &params->scoped[i - total];
        uint32_t drop = 0u;
        uint64_t keep = 0u;
        if (!slot->used) {
            continue;
        }
        if (slot->is_switch) {
            keep = params->switches[slot->def_index].fade_frames;
        }
        while (drop + 1u < slot->count && slot->seg[drop + 1u].start <= sample_time
               && sample_time - slot->seg[drop + 1u].start >= keep) {
            drop++;
        }
        if (drop != 0u) {
            memmove(slot->seg, slot->seg + drop, (slot->count - drop) * sizeof slot->seg[0]);
            slot->count -= drop;
        }
    }
    return SU_OK;
}

uint32_t su_audio_param_scope_clear(SuAudioParams *params, SuAudioParamScope scope) {
    uint32_t i;
    if (params == NULL || (scope.kind != SU_AUDIO_SCOPE_EMITTER && scope.kind != SU_AUDIO_SCOPE_INSTANCE)
        || scope.key == 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (i = 0u; i < params->desc.max_scoped_values; i++) {
        ParamSlot *slot = &params->scoped[i];
        if (slot->used && slot->kind == scope.kind && slot->key == scope.key) {
            slot->used = 0u;
            params->stats.scoped_values--;
        }
    }
    return SU_OK;
}

uint32_t su_audio_param_expand_state(const SuAudioParams *params, SuAudioParamTarget target,
    uint64_t instance_start, uint64_t sample_time, SuAudioGraphParameter *parameters,
    size_t parameter_capacity, SuAudioGraphSwitch *switches, size_t switch_capacity,
    SuAudioGraphExpandState *out) {
    uint32_t i;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    if (params == NULL || sample_time < instance_start
        || parameter_capacity < params->desc.param_count || switch_capacity < params->desc.switch_count
        || (params->desc.param_count != 0u && parameters == NULL)
        || (params->desc.switch_count != 0u && switches == NULL)) {
        return params == NULL || sample_time < instance_start ? SU_E_INVALID_ARGUMENT : SU_E_RESOURCE_LIMIT;
    }
    for (i = 0u; i < params->desc.param_count; i++) {
        parameters[i].parameter_id = params->params[i].id;
        parameters[i].value = param_value(params, target, i, sample_time);
    }
    for (i = 0u; i < params->desc.switch_count; i++) {
        uint64_t at = params->switches[i].policy == SU_AUDIO_SWITCH_FIRST_ONLY ? instance_start : sample_time;
        switches[i].switch_id = params->switches[i].id;
        switches[i].value = switch_value(params, target, i, at);
    }
    out->parameters = parameters;
    out->parameter_count = params->desc.param_count;
    out->switches = switches;
    out->switch_count = params->desc.switch_count;
    return SU_OK;
}

void su_audio_params_stats(const SuAudioParams *params, SuAudioParamStats *out) {
    if (out == NULL) {
        return;
    }
    if (params == NULL) {
        memset(out, 0, sizeof *out);
        return;
    }
    *out = params->stats;
}

uint32_t su_audio_param_voice_init(SuAudioParamVoice *voice, const SuAudioClip *clip,
                                   SuAudioParamTarget target, uint64_t start_sample, uint32_t loop) {
    if (voice == NULL || clip == NULL || clip->samples == NULL || clip->frames == 0u
        || (clip->channels != 1u && clip->channels != 2u) || clip->sample_rate == 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(voice, 0, sizeof *voice);
    voice->clip = clip;
    voice->target = target;
    voice->clock = start_sample;
    voice->loop = loop != 0u;
    return SU_OK;
}

static float clip_sample(const SuAudioClip *clip, uint32_t frame, uint32_t channel, uint32_t loop) {
    if (frame >= clip->frames) {
        if (!loop) {
            return 0.0f;
        }
        frame %= clip->frames;
    }
    return clip->samples[(size_t)frame * clip->channels + (clip->channels == 2u ? channel : 0u)];
}

uint32_t su_audio_param_voice_render(SuAudioParamVoice *voice, const SuAudioParams *params,
                                     float *out_stereo, uint32_t frames) {
    uint32_t i;
    uint32_t ch;
    double rate;
    if (voice == NULL || params == NULL || out_stereo == NULL || voice->clip == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    rate = (double)voice->clip->sample_rate / (double)params->desc.sample_rate;
    for (i = 0u; i < frames; i++) {
        SuAudioParamControls controls;
        double gain;
        double alpha;
        double base;
        double frac;
        uint32_t index;
        (void)su_audio_param_controls(params, voice->target, voice->clock + i, &controls);
        gain = pow(10.0, ((double)controls.gain_db + (double)controls.bus_gain_db) / 20.0);
        alpha = (double)controls.lowpass_hz >= (double)params->desc.sample_rate * 0.5 ? 1.0
            : 1.0 - exp(-2.0 * 3.14159265358979323846 * (double)controls.lowpass_hz
                        / (double)params->desc.sample_rate);
        if (voice->loop && voice->cursor >= (double)voice->clip->frames) {
            voice->cursor = fmod(voice->cursor, (double)voice->clip->frames);
        }
        base = floor(voice->cursor);
        frac = voice->cursor - base;
        index = base >= 4294967295.0 ? UINT32_MAX : (uint32_t)base;
        for (ch = 0u; ch < 2u; ch++) {
            double a = clip_sample(voice->clip, index, ch, voice->loop);
            double b = index == UINT32_MAX ? 0.0 : clip_sample(voice->clip, index + 1u, ch, voice->loop);
            double x = (a + (b - a) * frac) * gain;
            voice->lowpass_state[ch] = (float)((double)voice->lowpass_state[ch]
                + alpha * (x - (double)voice->lowpass_state[ch]));
            out_stereo[(size_t)i * 2u + ch] = voice->lowpass_state[ch];
        }
        voice->cursor += (double)controls.pitch * rate;
    }
    voice->clock += frames;
    return SU_OK;
}
