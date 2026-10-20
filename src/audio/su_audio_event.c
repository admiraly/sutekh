#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/audio_event.h"
#include "su_audio_event_int.h"
#include "su_audio_event_view.h"
#include "su_audio_voice_int.h"
#include "su_audio_bus_int.h"
#include "su_audio_parameter_int.h"

typedef struct EventLeaf {
    SuAudioGraphLeaf data;
    SuAudioVoiceDesc voice_desc;
    SuAudioVoice voice;
    uint64_t at;
    uint32_t state;
    float lowpass_hz;
    SuAudioLogicalVoice logical;   /* integrated mode */
    const SuAudioClip *clip;
    float ctl_gain;
    float ctl_pitch;
} EventLeaf;

typedef struct EventInstance {
    uint32_t generation;
    uint32_t state;
    uint32_t count;
    uint32_t scope;
    uint64_t order;
    SuAudioEmitter emitter;
    EventLeaf *leaves;
} EventInstance;

typedef struct EventStop {
    uint64_t target;
    uint64_t at;
    uint32_t type;
    uint32_t release_frames;
} EventStop;

struct SuAudioEvents {
    SuAudioEventsDesc desc;
    SuAudioAssets *assets;
    SuAudioScene *scene;
    SuAudioGraph *graph;
    SuAudioMixer *mixer;
    EventInstance *instances;
    EventLeaf *leaves;
    SuAudioGraphLeaf *scratch;
    EventStop *stops;
    uint32_t stop_count;
    uint64_t order;
    SuAudioCommitCursor cursor;
    SuAudioEventStats stats;
    float quantum[SU_AUDIO_EVENT_QUANTUM * 2u];
    uint32_t cached;
    uint32_t consumed;
    /* integrated mode */
    SuAudioVoicePool *pool;
    SuAudioVoicePolicyRule *policies;
    uint32_t policy_count;
    SuAudioVoicePolicy default_policy;
    uint64_t trigger_seq;
    float *route_mem;
    float **routes;
    uint32_t route_count;
    SuAudioParams *params;
    SuAudioGraphParameter *param_values;
    SuAudioGraphSwitch *switch_values;
    uint32_t param_count;
    uint32_t switch_count;
    SuAudioBusGain *bus_gains;
    uint32_t bus_gain_capacity;
    SuAudioBusTree *buses;
    SuAudioBusId *route_ids;
    uint32_t root_route;
    SuAudioMusic *music;
    uint32_t music_bus_id;
    float limiter_knee;
    float music_buffer[SU_AUDIO_EVENT_QUANTUM * 2u];
};

static SuAudioInstance instance_handle(uint32_t slot, uint32_t generation) {
    return ((uint64_t)generation << 32) | ((uint64_t)slot + 1u);
}

static EventInstance *find_instance(const SuAudioEvents *events, SuAudioInstance handle) {
    uint32_t slot = (uint32_t)handle;
    if (slot == 0u || slot > events->desc.max_instances) {
        return NULL;
    }
    if (events->instances[slot - 1u].generation != (uint32_t)(handle >> 32)
        || events->instances[slot - 1u].state != 1u) {
        return NULL;
    }
    return &events->instances[slot - 1u];
}

uint32_t su_audio_events_create(const SuAudioEventsDesc *desc, SuAudioAssets *assets,
                                SuAudioScene *scene, SuAudioGraph *graph, SuAudioEvents **out) {
    SuAudioEvents *events;
    SuAudioMixerDesc mixer_desc;
    size_t leaf_count;
    uint32_t status;
    uint32_t index;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (desc == NULL || desc->struct_size != sizeof *desc || assets == NULL || graph == NULL
        || desc->max_instances == 0u || desc->max_instances > 4096u
        || desc->max_leaves_per_instance == 0u || desc->max_leaves_per_instance > 4096u
        || desc->max_logical_voices == 0u || desc->max_logical_voices > 4096u
        || desc->command_capacity == 0u || desc->command_capacity > 65536u
        || desc->sample_rate < 1000u || desc->sample_rate > 384000u
        || desc->sample_rate != su_audio_graph_sample_rate(graph)
        || desc->session_generation == 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    leaf_count = (size_t)desc->max_instances * desc->max_leaves_per_instance;
    if (leaf_count > SIZE_MAX / sizeof(EventLeaf)) {
        return SU_E_RESOURCE_LIMIT;
    }
    events = calloc(1, sizeof *events);
    if (events == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    events->desc = *desc;
    events->instances = calloc(desc->max_instances, sizeof *events->instances);
    events->leaves = calloc(leaf_count, sizeof *events->leaves);
    events->scratch = calloc(desc->max_leaves_per_instance, sizeof *events->scratch);
    events->stops = calloc(desc->command_capacity, sizeof *events->stops);
    if (events->instances == NULL || events->leaves == NULL || events->scratch == NULL || events->stops == NULL) {
        su_audio_events_destroy(events);
        return SU_E_RESOURCE_LIMIT;
    }
    memset(&mixer_desc, 0, sizeof mixer_desc);
    mixer_desc.struct_size = sizeof mixer_desc;
    mixer_desc.sample_rate = desc->sample_rate;
    mixer_desc.max_voices = desc->max_logical_voices;
    mixer_desc.command_capacity = desc->max_logical_voices * 4u + 16u;
    status = su_audio_mixer_create(&mixer_desc, &events->mixer);
    if (status != SU_OK) {
        su_audio_events_destroy(events);
        return status;
    }
    events->assets = assets;
    events->scene = scene;
    status = su_audio_graph_retain(graph);
    if (status != SU_OK) {
        su_audio_events_destroy(events);
        return status;
    }
    events->graph = graph;
    events->cursor.session_generation = desc->session_generation;
    events->stats.render_clock = desc->start_sample;
    events->stats.runtime_bytes = sizeof *events + (uint64_t)desc->max_instances * sizeof *events->instances
        + (uint64_t)leaf_count * sizeof *events->leaves
        + (uint64_t)desc->max_leaves_per_instance * sizeof *events->scratch
        + (uint64_t)desc->command_capacity * sizeof *events->stops;
    for (index = 0; index < desc->max_instances; index++) {
        events->instances[index].leaves = events->leaves + (size_t)index * desc->max_leaves_per_instance;
    }
    *out = events;
    return SU_OK;
}

void su_audio_events_destroy(SuAudioEvents *events) {
    uint32_t slot;
    if (events == NULL) {
        return;
    }
    su_audio_mixer_destroy(events->mixer);
    if (events->assets != NULL && events->instances != NULL) {
        for (slot = 0; slot < events->desc.max_instances; slot++) {
            uint32_t index;
            for (index = 0; index < events->instances[slot].count; index++) {
                (void)su_audio_asset_release(events->assets, events->instances[slot].leaves[index].data.asset);
            }
        }
    }
    if (events->graph != NULL) {
        su_audio_graph_release(events->graph);
    }
    free(events->param_values);
    free(events->switch_values);
    free(events->bus_gains);
    free(events->route_ids);
    free(events->routes);
    free(events->route_mem);
    free(events->policies);
    free(events->stops);
    free(events->scratch);
    free(events->leaves);
    free(events->instances);
    free(events);
}

void su_audio_events_update(SuAudioEvents *events) {
    uint32_t slot;
    if (events == NULL) {
        return;
    }
    for (slot = 0; slot < events->desc.max_instances; slot++) {
        EventInstance *instance = &events->instances[slot];
        uint32_t index;
        if (instance->state != 2u) {
            continue;
        }
        for (index = 0; index < instance->count; index++) {
            (void)su_audio_asset_release(events->assets, instance->leaves[index].data.asset);
        }
        if (events->params != NULL) {
            SuAudioParamScope scope;
            scope.kind = SU_AUDIO_SCOPE_INSTANCE;
            scope.key = instance_handle(slot, instance->generation);
            (void)su_audio_param_scope_clear(events->params, scope);
        }
        events->stats.logical_voices -= instance->count;
        events->stats.instances--;
        instance->count = 0u;
        instance->state = 0u;
    }
}

uint32_t su_audio_event_post(SuAudioEvents *events, const SuAudioTrigger *trigger,
                             SuAudioInstance *out, SuAudioGraphDiagnostic *diagnostic) {
    size_t count = 0u;
    size_t index;
    uint32_t slot;
    uint32_t status;
    EventInstance *instance = NULL;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = 0u;
    if (events == NULL || trigger == NULL || trigger->struct_size != sizeof *trigger
        || trigger->session_generation != events->cursor.session_generation || trigger->commit_seq == UINT64_MAX) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (trigger->commit_seq < events->cursor.next_seq) {
        events->stats.duplicates++;
        return SU_E_STALE_REVISION;
    }
    if (trigger->sample_time < events->stats.render_clock) {
        events->stats.late_commands++;
        return SU_E_INVALID_ARGUMENT;
    }
    su_audio_events_update(events);
    for (slot = 0; slot < events->desc.max_instances; slot++) {
        if (events->instances[slot].state == 0u && events->instances[slot].generation != UINT32_MAX) {
            instance = &events->instances[slot];
            break;
        }
    }
    if (instance == NULL || events->order == UINT64_MAX) {
        events->stats.refused++;
        return SU_E_RESOURCE_LIMIT;
    }
    if (events->params != NULL) {
        SuAudioGraphExpandState state;
        SuAudioParamTarget target;
        target.emitter = trigger->emitter;
        target.instance = instance_handle(slot,
            instance->generation + 1u == 0u ? 1u : instance->generation + 1u);
        status = su_audio_param_expand_state(events->params, target, trigger->sample_time, trigger->sample_time,
            events->param_values, events->param_count, events->switch_values, events->switch_count, &state);
        if (status == SU_OK) {
            status = su_audio_graph_expand_ex(events->graph, trigger->event_id, events->desc.project_seed,
                trigger->emitter, trigger->commit_seq, &state, events->scratch, events->desc.max_leaves_per_instance,
                &count, diagnostic);
        }
    } else {
        status = su_audio_graph_expand(events->graph, trigger->event_id, events->desc.project_seed,
            trigger->emitter, trigger->commit_seq, events->scratch, events->desc.max_leaves_per_instance, &count,
            diagnostic);
    }
    if (status != SU_OK) {
        events->stats.refused++;
        return status;
    }
    if (count > events->desc.max_logical_voices - events->stats.logical_voices) {
        events->stats.refused++;
        return SU_E_RESOURCE_LIMIT;
    }
    for (index = 0; index < count; index++) {
        EventLeaf *leaf = &instance->leaves[index];
        const SuAudioClip *clip = NULL;
        memset(leaf, 0, sizeof *leaf);
        leaf->data = events->scratch[index];
        if (leaf->data.delay_frames > UINT64_MAX - trigger->sample_time) {
            status = SU_E_INVALID_ARGUMENT;
            break;
        }
        status = su_audio_asset_acquire(events->assets, leaf->data.asset, &clip);
        if (status != SU_OK) {
            break;
        }
        instance->count++;
        leaf->clip = clip;
        leaf->voice_desc.clip = clip;
        leaf->voice_desc.gain = leaf->data.gain;
        leaf->voice_desc.pitch = leaf->data.pitch;
        leaf->voice_desc.flags = leaf->data.flags;
        leaf->at = trigger->sample_time + leaf->data.delay_frames;
        if (trigger->emitter != 0u) {
            status = su_audio_scene_resolve(events->scene, trigger->emitter, &leaf->voice_desc, &leaf->lowpass_hz);
            if (status != SU_OK) {
                break;
            }
        }
    }
    if (status != SU_OK || count == 0u) {
        for (index = 0; index < instance->count; index++) {
            (void)su_audio_asset_release(events->assets, instance->leaves[index].data.asset);
        }
        instance->count = 0u;
        events->stats.refused++;
        return status != SU_OK ? status : SU_E_SCHEMA_MISMATCH;
    }
    instance->generation++;
    if (instance->generation == 0u) {
        instance->generation = 1u;
    }
    instance->state = 1u;
    instance->scope = trigger->scope_id;
    instance->emitter = trigger->emitter;
    instance->order = ++events->order;
    events->stats.instances++;
    events->stats.logical_voices += (uint32_t)count;
    events->stats.admitted++;
    events->cursor.next_seq = trigger->commit_seq + 1u;
    *out = instance_handle(slot, instance->generation);
    return SU_OK;
}

static uint32_t queue_stop(SuAudioEvents *events, uint32_t type, uint64_t target,
                            uint64_t sample_time, uint32_t release_frames) {
    EventStop *stop;
    if (events == NULL || sample_time > UINT64_MAX - release_frames) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (sample_time < events->stats.render_clock) {
        events->stats.late_commands++;
        return SU_E_INVALID_ARGUMENT;
    }
    if (events->stop_count == events->desc.command_capacity) {
        events->stats.refused++;
        return SU_E_RESOURCE_LIMIT;
    }
    stop = &events->stops[events->stop_count++];
    stop->type = type;
    stop->target = target;
    stop->at = sample_time;
    stop->release_frames = release_frames;
    return SU_OK;
}

uint32_t su_audio_instance_stop(SuAudioEvents *events, SuAudioInstance instance,
                                uint64_t sample_time, uint32_t release_frames) {
    if (events == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (find_instance(events, instance) == NULL) {
        return SU_E_STALE_HANDLE;
    }
    return queue_stop(events, 1u, instance, sample_time, release_frames);
}

uint32_t su_audio_emitter_stop(SuAudioEvents *events, SuAudioEmitter emitter,
                               uint64_t sample_time, uint32_t release_frames) {
    return emitter == 0u ? SU_E_INVALID_ARGUMENT : queue_stop(events, 2u, emitter, sample_time, release_frames);
}

uint32_t su_audio_scope_stop(SuAudioEvents *events, uint32_t scope_id,
                             uint64_t sample_time, uint32_t release_frames) {
    return queue_stop(events, 3u, scope_id, sample_time, release_frames);
}

static void collect_finished(SuAudioEvents *events) {
    uint32_t slot;
    su_audio_update(events->mixer);
    for (slot = 0; slot < events->desc.max_instances; slot++) {
        EventInstance *instance = &events->instances[slot];
        uint32_t index;
        uint32_t remaining = 0u;
        if (instance->state != 1u) {
            continue;
        }
        for (index = 0; index < instance->count; index++) {
            EventLeaf *leaf = &instance->leaves[index];
            if (leaf->state == 1u && !su_audio_voice_active(events->mixer, leaf->voice)) {
                leaf->state = 2u;
            }
            if (leaf->state != 2u) {
                remaining++;
            }
        }
        if (remaining == 0u) {
            instance->state = 2u;
        }
    }
}

static void apply_stops(SuAudioEvents *events, uint64_t now) {
    uint32_t command = 0u;
    while (command < events->stop_count) {
        EventStop stop = events->stops[command];
        uint32_t slot;
        if (stop.at != now) {
            command++;
            continue;
        }
        for (slot = 0; slot < events->desc.max_instances; slot++) {
            EventInstance *instance = &events->instances[slot];
            uint32_t index;
            if (instance->state != 1u || !((stop.type == 1u && stop.target == instance_handle(slot, instance->generation))
                || (stop.type == 2u && stop.target == instance->emitter)
                || (stop.type == 3u && stop.target == instance->scope))) {
                continue;
            }
            for (index = 0; index < instance->count; index++) {
                EventLeaf *leaf = &instance->leaves[index];
                if (leaf->state == 0u) {
                    leaf->state = 2u;
                } else if (leaf->state == 1u) {
                    uint32_t status = su_audio_event_voice_release(events->mixer, leaf->voice, stop.release_frames);
                    if (status != SU_OK) {
                        events->stats.render_failures++;
                    } else if (stop.release_frames == 0u) {
                        leaf->state = 2u;
                    }
                }
            }
        }
        events->stop_count--;
        memmove(&events->stops[command], &events->stops[command + 1u],
            (size_t)(events->stop_count - command) * sizeof *events->stops);
    }
}

static uint64_t start_due(SuAudioEvents *events, uint64_t now, uint64_t next) {
    uint64_t previous = 0u;
    uint32_t visited;
    for (visited = 0; visited < events->desc.max_instances; visited++) {
        EventInstance *instance = NULL;
        uint32_t slot;
        uint32_t index;
        for (slot = 0; slot < events->desc.max_instances; slot++) {
            EventInstance *candidate = &events->instances[slot];
            if (candidate->state == 1u && candidate->order > previous
                && (instance == NULL || candidate->order < instance->order)) {
                instance = candidate;
            }
        }
        if (instance == NULL) {
            break;
        }
        previous = instance->order;
        for (index = 0; index < instance->count; index++) {
            EventLeaf *leaf = &instance->leaves[index];
            if (leaf->state == 0u && leaf->at == now) {
                uint32_t status = su_audio_play(events->mixer, &leaf->voice_desc, &leaf->voice);
                leaf->state = status == SU_OK ? 1u : 2u;
                if (status != SU_OK) {
                    events->stats.render_failures++;
                } else if (leaf->lowpass_hz > 0.0f) {
                    status = su_audio_voice_set_lowpass(events->mixer, leaf->voice, leaf->lowpass_hz);
                    if (status != SU_OK) {
                        events->stats.render_failures++;
                    }
                }
            } else if (leaf->state == 0u && leaf->at < next) {
                next = leaf->at;
            }
        }
    }
    return next;
}

static void refresh_scene(SuAudioEvents *events) {
    uint32_t slot;
    if (events->scene == NULL) {
        return;
    }
    if (su_audio_scene_listener_apply(events->scene, events->mixer) != SU_OK) {
        events->stats.render_failures++;
    }
    for (slot = 0; slot < events->desc.max_instances; slot++) {
        EventInstance *instance = &events->instances[slot];
        uint32_t index;
        if (instance->state != 1u || instance->emitter == 0u) {
            continue;
        }
        for (index = 0; index < instance->count; index++) {
            EventLeaf *leaf = &instance->leaves[index];
            uint32_t status;
            if (leaf->state == 2u) {
                continue;
            }
            leaf->voice_desc.gain = leaf->data.gain;
            leaf->voice_desc.min_distance = 0.0f;
            leaf->voice_desc.max_distance = 0.0f;
            leaf->voice_desc.rolloff = 0.0f;
            status = su_audio_scene_resolve(events->scene, instance->emitter, &leaf->voice_desc, &leaf->lowpass_hz);
            if (status != SU_OK) {
                if (leaf->state == 1u) {
                    (void)su_audio_event_voice_release(events->mixer, leaf->voice, 0u);
                } else {
                    leaf->state = 2u;
                }
            } else if (leaf->state == 1u) {
                status = su_audio_event_voice_spatial(events->mixer, leaf->voice, &leaf->voice_desc);
                if (status == SU_OK) {
                    status = su_audio_voice_set_lowpass(events->mixer, leaf->voice, leaf->lowpass_hz);
                }
                if (status != SU_OK) {
                    events->stats.render_failures++;
                }
            }
        }
    }
}

/* ---- integrated mode: leaves drive the voice pool ---- */

static const SuAudioVoicePolicy *leaf_policy(const SuAudioEvents *events, const SuAudioGraphLeaf *data) {
    uint32_t index;
    for (index = 0; index < events->policy_count; index++) {
        const SuAudioVoicePolicyRule *rule = &events->policies[index];
        if ((rule->node_id == 0u || rule->node_id == data->node_id)
            && (rule->bus_id == 0u || rule->bus_id == data->bus_id)) {
            return &rule->policy;
        }
    }
    return &events->default_policy;
}

static SuVec3f rotate_inverse(SuQuatf q, SuVec3f v) {
    SuQuatf c;
    c.x = -q.x;
    c.y = -q.y;
    c.z = -q.z;
    c.w = q.w;
    return su_quatf_rotate(c, v);
}

/* Spatial and authored controls of one leaf at the current quantum (the legacy mixer's spatial law). */
static uint32_t leaf_controls(SuAudioEvents *events, const EventInstance *instance, EventLeaf *leaf,
                              const SuAudioCameraListener *listener, SuAudioVoiceControls *out, float *audibility) {
    float gain = leaf->data.gain;
    float pitch = (leaf->data.pitch > 0.0f ? leaf->data.pitch : 1.0f)
        * ((float)leaf->clip->sample_rate / (float)events->desc.sample_rate);
    memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    leaf->lowpass_hz = 0.0f;
    if (instance->emitter != 0u && events->scene != NULL) {
        SuAudioVoiceDesc desc = leaf->voice_desc;
        SuVec3f rel;
        SuVec3f local;
        float distance;
        float attenuation;
        float left;
        float right;
        uint32_t status;
        desc.gain = leaf->data.gain;
        desc.min_distance = 0.0f;
        desc.max_distance = 0.0f;
        desc.rolloff = 0.0f;
        status = su_audio_scene_resolve(events->scene, instance->emitter, &desc, &leaf->lowpass_hz);
        if (status != SU_OK) {
            return status;
        }
        rel = su_wpos_delta(desc.pos, listener->pos);
        local = rotate_inverse(listener->rot, rel);
        distance = sqrtf(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z);
        attenuation = su_audio_attenuation(distance, desc.min_distance, desc.max_distance, desc.rolloff);
        su_audio_pan(local, &left, &right);
        gain = desc.gain * attenuation;
        out->gain_left = left * gain;
        out->gain_right = right * gain;
        out->mono_mix = 1u;
    } else {
        out->gain_left = gain;
        out->gain_right = gain;
    }
    if (events->params != NULL) {
        SuAudioParamTarget target;
        float gain_db = 0.0f;
        float ratio = 1.0f;
        float cutoff = 0.0f;
        target.emitter = instance->emitter;
        target.instance = instance_handle((uint32_t)(instance - events->instances), instance->generation);
        (void)su_audio_param_voice_controls(events->params, target,
            events->stats.render_clock + SU_AUDIO_EVENT_QUANTUM, &gain_db, &ratio, &cutoff);
        if (gain_db != 0.0f) {
            float scale = powf(10.0f, gain_db / 20.0f);
            gain *= scale;
            out->gain_left *= scale;
            out->gain_right *= scale;
        }
        pitch *= ratio;
        if (cutoff < (float)events->desc.sample_rate * 0.5f
            && (leaf->lowpass_hz <= 0.0f || cutoff < leaf->lowpass_hz)) {
            leaf->lowpass_hz = cutoff > 0.0f ? cutoff : 1.0f;
        }
    }
    if (pitch > 64.0f) {
        pitch = 64.0f;
    }
    out->pitch = pitch;
    out->lowpass_hz = leaf->lowpass_hz;
    leaf->ctl_gain = gain;
    leaf->ctl_pitch = pitch;
    *audibility = gain;
    return SU_OK;
}

static void pooled_start(SuAudioEvents *events, uint64_t now, uint64_t end) {
    uint64_t previous = 0u;
    uint32_t visited;
    for (visited = 0; visited < events->desc.max_instances; visited++) {
        EventInstance *instance = NULL;
        uint32_t slot;
        uint32_t index;
        for (slot = 0; slot < events->desc.max_instances; slot++) {
            EventInstance *candidate = &events->instances[slot];
            if (candidate->state == 1u && candidate->order > previous
                && (instance == NULL || candidate->order < instance->order)) {
                instance = candidate;
            }
        }
        if (instance == NULL) {
            break;
        }
        previous = instance->order;
        for (index = 0; index < instance->count; index++) {
            EventLeaf *leaf = &instance->leaves[index];
            SuAudioLogicalDesc desc;
            uint32_t status;
            if (leaf->state != 0u || leaf->at >= end) {
                continue;
            }
            memset(&desc, 0, sizeof desc);
            desc.struct_size = sizeof desc;
            desc.policy = *leaf_policy(events, &leaf->data);
            if ((leaf->data.flags & SU_AUDIO_LOOP) != 0u) {
                desc.policy.flags |= SU_AUDIO_VOICE_LOOP;
            }
            desc.clip = leaf->clip;
            desc.gain = 1.0f;
            desc.node_id = leaf->data.node_id;
            desc.bus_id = leaf->data.bus_id;
            desc.emitter = instance->emitter;
            desc.instance = instance_handle((uint32_t)(instance - events->instances), instance->generation);
            desc.trigger_seq = ++events->trigger_seq;
            status = su_audio_voice_add_at(events->pool, &desc, leaf->at < now ? now : leaf->at, &leaf->logical);
            if (status == SU_OK) {
                status = su_audio_voice_set_tag(events->pool, leaf->logical,
                    ((uint64_t)leaf->data.asset.generation << 32) | leaf->data.asset.id);
            }
            if (status == SU_OK && events->buses != NULL) {
                uint32_t route = events->root_route;
                uint32_t r;
                for (r = 0; r < events->route_count; r++) {
                    if (events->route_ids[r] == leaf->data.bus_id) {
                        route = r;
                        break;
                    }
                }
                status = su_audio_voice_set_route(events->pool, leaf->logical, route);
            }
            leaf->state = status == SU_OK ? 1u : 2u;
            if (status != SU_OK) {
                events->stats.render_failures++;
            }
        }
    }
}

static void pooled_stops(SuAudioEvents *events, uint64_t end) {
    uint32_t command = 0u;
    while (command < events->stop_count) {
        EventStop stop = events->stops[command];
        uint32_t slot;
        if (stop.at >= end) {
            command++;
            continue;
        }
        for (slot = 0; slot < events->desc.max_instances; slot++) {
            EventInstance *instance = &events->instances[slot];
            uint32_t index;
            if (instance->state != 1u || !((stop.type == 1u && stop.target == instance_handle(slot, instance->generation))
                || (stop.type == 2u && stop.target == instance->emitter)
                || (stop.type == 3u && stop.target == instance->scope))) {
                continue;
            }
            for (index = 0; index < instance->count; index++) {
                EventLeaf *leaf = &instance->leaves[index];
                if (leaf->state == 0u) {
                    leaf->state = 2u;
                } else if (leaf->state == 1u
                    && su_audio_voice_stop_at(events->pool, leaf->logical, stop.at, stop.release_frames) != SU_OK) {
                    leaf->state = 3u;
                }
            }
        }
        events->stop_count--;
        memmove(&events->stops[command], &events->stops[command + 1u],
            (size_t)(events->stop_count - command) * sizeof *events->stops);
    }
}

static void pooled_controls(SuAudioEvents *events) {
    SuAudioCameraListener listener;
    uint32_t slot;
    memset(&listener, 0, sizeof listener);
    listener.rot.w = 1.0f;
    if (events->scene != NULL && su_audio_scene_listener_get(events->scene, &listener) != SU_OK) {
        events->stats.render_failures++;
    }
    for (slot = 0; slot < events->desc.max_instances; slot++) {
        EventInstance *instance = &events->instances[slot];
        uint32_t index;
        if (instance->state != 1u) {
            continue;
        }
        for (index = 0; index < instance->count; index++) {
            EventLeaf *leaf = &instance->leaves[index];
            SuAudioVoiceControls controls;
            float audibility = 0.0f;
            if (leaf->state != 1u) {
                continue;
            }
            if (leaf_controls(events, instance, leaf, &listener, &controls, &audibility) != SU_OK) {
                /* the emitter is gone: end the voice like the legacy runtime */
                (void)su_audio_voice_stop_at(events->pool, leaf->logical, events->stats.render_clock, 0u);
                continue;
            }
            if (su_audio_voice_set_controls(events->pool, leaf->logical, &controls) != SU_OK
                || su_audio_voice_set_audibility(events->pool, leaf->logical, audibility, 0.0f) != SU_OK) {
                events->stats.render_failures++;
            }
        }
    }
}

static void pooled_collect(SuAudioEvents *events) {
    uint32_t slot;
    for (slot = 0; slot < events->desc.max_instances; slot++) {
        EventInstance *instance = &events->instances[slot];
        uint32_t index;
        uint32_t remaining = 0u;
        if (instance->state != 1u) {
            continue;
        }
        for (index = 0; index < instance->count; index++) {
            EventLeaf *leaf = &instance->leaves[index];
            SuAudioVoiceInfo info;
            if (leaf->state == 1u && su_audio_voice_info(events->pool, leaf->logical, &info) != SU_OK) {
                leaf->state = 3u; /* voice ended; a steal tail may still read the clip */
            }
            if (leaf->state == 3u && !su_audio_voice_pool_clip_busy(events->pool, leaf->clip)) {
                leaf->state = 2u;
            }
            if (leaf->state != 2u) {
                remaining++;
            }
        }
        if (remaining == 0u) {
            instance->state = 2u;
        }
    }
}

static uint32_t render_quantum_pooled(SuAudioEvents *events) {
    uint64_t now = events->stats.render_clock;
    uint64_t end;
    uint32_t route;
    uint32_t index;
    uint32_t status;
    if (now > UINT64_MAX - SU_AUDIO_EVENT_QUANTUM) {
        return SU_E_RESOURCE_LIMIT;
    }
    end = now + SU_AUDIO_EVENT_QUANTUM;
    if (events->params != NULL && su_audio_params_advance(events->params, now) != SU_OK) {
        events->stats.render_failures++;
    }
    pooled_start(events, now, end);
    pooled_stops(events, end);
    pooled_controls(events);
    memset(events->route_mem, 0, (size_t)events->route_count * SU_AUDIO_EVENT_QUANTUM * 2u * sizeof(float));
    status = su_audio_voice_pool_quantum(events->pool, events->routes, events->route_count);
    if (status != SU_OK) {
        return status;
    }
    if (events->music != NULL) {
        status = su_audio_music_render(events->music, events->music_buffer, SU_AUDIO_EVENT_QUANTUM);
        if (status != SU_OK) {
            return status;
        }
    }
    if (events->buses != NULL) {
        for (route = 0u; route < events->route_count && status == SU_OK; route++) {
            status = su_audio_bus_mix(events->buses, events->route_ids[route], events->routes[route],
                SU_AUDIO_EVENT_QUANTUM);
        }
        if (events->music != NULL && status == SU_OK) {
            status = su_audio_bus_mix(events->buses, events->music_bus_id, events->music_buffer,
                SU_AUDIO_EVENT_QUANTUM);
        }
        if (events->params != NULL && events->bus_gain_capacity != 0u && status == SU_OK) {
            uint32_t count = 0u;
            status = su_audio_param_bus_gains(events->params, end, events->bus_gains, events->bus_gain_capacity,
                &count);
            for (index = 0; index < count && status == SU_OK; index++) {
                float db = events->bus_gains[index].gain_db;
                status = su_audio_bus_set_gain(events->buses, events->bus_gains[index].bus_id,
                    db > 120.0f ? 120.0f : db, SU_AUDIO_EVENT_QUANTUM);
            }
        }
        if (status == SU_OK) {
            status = su_audio_bus_process(events->buses, events->quantum, SU_AUDIO_EVENT_QUANTUM);
        }
        if (status != SU_OK) {
            return status;
        }
    } else {
        memcpy(events->quantum, events->routes[0], sizeof events->quantum);
        if (events->music != NULL) {
            for (index = 0; index < SU_AUDIO_EVENT_QUANTUM * 2u; index++) {
                events->quantum[index] += events->music_buffer[index];
            }
        }
    }
    if (events->limiter_knee > 0.0f) {
        float knee = events->limiter_knee;
        for (index = 0; index < SU_AUDIO_EVENT_QUANTUM * 2u; index++) {
            float sample = events->quantum[index];
            float magnitude = fabsf(sample);
            if (magnitude > knee) {
                float bent = knee + (1.0f - knee) * tanhf((magnitude - knee) / (1.0f - knee));
                events->quantum[index] = sample < 0.0f ? -bent : bent;
            }
        }
    } else {
        for (index = 0; index < SU_AUDIO_EVENT_QUANTUM * 2u; index++) {
            float sample = events->quantum[index];
            events->quantum[index] = sample > 1.0f ? 1.0f : sample < -1.0f ? -1.0f : sample;
        }
    }
    pooled_collect(events);
    events->stats.render_clock = end;
    events->cached = SU_AUDIO_EVENT_QUANTUM;
    events->consumed = 0u;
    return SU_OK;
}

uint32_t su_audio_events_attach(SuAudioEvents *events, const SuAudioEventsHook *hook) {
    SuAudioVoiceStats pool_stats;
    SuAudioVoicePoolDesc pool_desc;
    SuAudioVoicePolicyRule *policies = NULL;
    SuAudioParams *params = NULL;
    SuAudioBusTree *buses = NULL;
    SuAudioMusic *music = NULL;
    SuAudioGraphParameter *param_values;
    SuAudioGraphSwitch *switch_values;
    SuAudioBusGain *bus_gains;
    SuAudioBusId *route_ids;
    uint32_t music_bus_id = 0u;
    uint32_t route_count = 1u;
    uint32_t param_count = 0u;
    uint32_t switch_count = 0u;
    uint32_t bus_bindings = 0u;
    uint32_t index;
    uint32_t status;
    float knee = 0.0f;
    float *route_mem;
    float **routes;
    if (events == NULL || hook == NULL || hook->struct_size < offsetof(SuAudioEventsHook, params)
        || hook->struct_size > sizeof *hook || hook->version != SU_AUDIO_EVENTS_HOOK_VERSION
        || hook->voices == NULL || hook->policy_count > 4096u || (hook->policy_count != 0u && hook->policies == NULL)
        || events->pool != NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    su_audio_events_update(events);
    su_audio_voice_pool_stats(hook->voices, &pool_stats);
    su_audio_voice_pool_desc(hook->voices, &pool_desc);
    if (pool_desc.max_logical < events->desc.max_logical_voices
        || (pool_desc.sample_rate != 0u && pool_desc.sample_rate != events->desc.sample_rate)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (events->stats.instances != 0u || events->stop_count != 0u || events->consumed != events->cached
        || pool_stats.logical != 0u) {
        return SU_E_RESOURCE_LIMIT;
    }
    if (SU_AUDIO_EVENTS_HOOK_HAS(hook, params)) {
        params = hook->params;
    }
    if (SU_AUDIO_EVENTS_HOOK_HAS(hook, buses)) {
        buses = hook->buses;
    }
    if (SU_AUDIO_EVENTS_HOOK_HAS(hook, music)) {
        music = hook->music;
    }
    if (SU_AUDIO_EVENTS_HOOK_HAS(hook, music_bus_id)) {
        music_bus_id = hook->music_bus_id;
    }
    if (SU_AUDIO_EVENTS_HOOK_HAS(hook, limiter_knee)) {
        knee = hook->limiter_knee;
    }
    if (!(knee == 0.0f || (knee >= 0.1f && knee < 1.0f))) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (buses != NULL) {
        route_count = su_audio_bus_count(buses);
        if (su_audio_bus_max_block(buses) < SU_AUDIO_EVENT_QUANTUM) {
            return SU_E_INVALID_ARGUMENT;
        }
    }
    if (music != NULL) {
        SuAudioMusicState music_state;
        su_audio_music_state(music, &music_state);
        if (music_state.clock != events->stats.render_clock) {
            return SU_E_INVALID_ARGUMENT;
        }
    }
    if (params != NULL) {
        su_audio_params_counts(params, &param_count, &switch_count, &bus_bindings);
    }
    route_mem = calloc((size_t)route_count * SU_AUDIO_EVENT_QUANTUM * 2u, sizeof *route_mem);
    routes = calloc(route_count, sizeof *routes);
    route_ids = calloc(route_count, sizeof *route_ids);
    param_values = calloc(param_count + 1u, sizeof *param_values);
    switch_values = calloc(switch_count + 1u, sizeof *switch_values);
    bus_gains = calloc(bus_bindings + 1u, sizeof *bus_gains);
    if (hook->policy_count != 0u) {
        policies = calloc(hook->policy_count, sizeof *policies);
    }
    status = route_mem == NULL || routes == NULL || route_ids == NULL || param_values == NULL
        || switch_values == NULL || bus_gains == NULL || (hook->policy_count != 0u && policies == NULL)
        ? SU_E_RESOURCE_LIMIT : SU_OK;
    for (index = 0; status == SU_OK && buses != NULL && index < route_count; index++) {
        status = su_audio_bus_order(buses, index, &route_ids[index]);
    }
    if (status == SU_OK && music != NULL && buses != NULL) {
        if (music_bus_id == 0u) {
            music_bus_id = route_ids[route_count - 1u];
        }
        for (index = 0; index < route_count && route_ids[index] != music_bus_id; index++) {
        }
        status = index == route_count ? SU_E_INVALID_ARGUMENT : SU_OK;
    }
    if (status == SU_OK && su_audio_voice_pool_set_clock(hook->voices, events->stats.render_clock) != SU_OK) {
        status = SU_E_INVALID_ARGUMENT;
    }
    if (status != SU_OK) {
        free(route_mem);
        free(routes);
        free(route_ids);
        free(param_values);
        free(switch_values);
        free(bus_gains);
        free(policies);
        return status;
    }
    if (policies != NULL) {
        memcpy(policies, hook->policies, (size_t)hook->policy_count * sizeof *policies);
    }
    for (index = 0; index < route_count; index++) {
        routes[index] = route_mem + (size_t)index * SU_AUDIO_EVENT_QUANTUM * 2u;
    }
    events->params = params;
    events->param_values = param_values;
    events->switch_values = switch_values;
    events->param_count = param_count;
    events->switch_count = switch_count;
    events->bus_gains = bus_gains;
    events->bus_gain_capacity = bus_bindings;
    events->buses = buses;
    events->route_ids = route_ids;
    events->root_route = route_count - 1u; /* execution order ends with the root */
    events->music = music;
    events->music_bus_id = music_bus_id;
    events->limiter_knee = knee;
    events->policies = policies;
    events->policy_count = hook->policy_count;
    events->default_policy = hook->default_policy;
    events->route_mem = route_mem;
    events->routes = routes;
    events->route_count = route_count;
    events->pool = hook->voices;
    events->stats.runtime_bytes += (uint64_t)route_count * (SU_AUDIO_EVENT_QUANTUM * 2u * sizeof *route_mem
        + sizeof *routes + sizeof *route_ids) + (uint64_t)(param_count + 1u) * sizeof *param_values
        + (uint64_t)(switch_count + 1u) * sizeof *switch_values + (uint64_t)(bus_bindings + 1u) * sizeof *bus_gains
        + (uint64_t)hook->policy_count * sizeof *policies;
    return SU_OK;
}

static uint32_t render_quantum(SuAudioEvents *events) {
    uint64_t now = events->stats.render_clock;
    uint64_t end;
    uint32_t offset = 0u;
    if (now > UINT64_MAX - SU_AUDIO_EVENT_QUANTUM) {
        return SU_E_RESOURCE_LIMIT;
    }
    end = now + SU_AUDIO_EVENT_QUANTUM;
    refresh_scene(events);
    while (now < end) {
        uint32_t command;
        uint32_t frames;
        apply_stops(events, now);
        collect_finished(events);
        uint64_t next = start_due(events, now, end);
        for (command = 0; command < events->stop_count; command++) {
            if (events->stops[command].at < next) {
                next = events->stops[command].at;
            }
        }
        frames = (uint32_t)(next - now);
        su_audio_mixer_render(events->mixer, events->quantum + (size_t)offset * 2u, frames);
        collect_finished(events);
        now = next;
        offset += frames;
    }
    events->stats.render_clock = end;
    events->cached = SU_AUDIO_EVENT_QUANTUM;
    events->consumed = 0u;
    return SU_OK;
}

uint32_t su_audio_events_render(SuAudioEvents *events, float *out, uint32_t frames) {
    uint32_t done = 0u;
    if (events == NULL || (out == NULL && frames != 0u)
        || events->stats.delivered_frames > UINT64_MAX - frames) {
        return SU_E_INVALID_ARGUMENT;
    }
    while (done < frames) {
        uint32_t take;
        if (events->consumed == events->cached) {
            uint32_t status = events->pool != NULL ? render_quantum_pooled(events) : render_quantum(events);
            if (status != SU_OK) {
                return status;
            }
        }
        take = events->cached - events->consumed;
        if (take > frames - done) {
            take = frames - done;
        }
        memcpy(out + (size_t)done * 2u, events->quantum + (size_t)events->consumed * 2u,
            (size_t)take * 2u * sizeof *out);
        done += take;
        events->consumed += take;
        events->stats.delivered_frames += take;
    }
    return SU_OK;
}

int su_audio_instance_active(const SuAudioEvents *events, SuAudioInstance instance) {
    return events != NULL && find_instance(events, instance) != NULL;
}

uint64_t su_audio_events_clock(const SuAudioEvents *events) {
    return events != NULL ? events->stats.render_clock : 0u;
}

void su_audio_events_stats(const SuAudioEvents *events, SuAudioEventStats *out) {
    if (events != NULL && out != NULL) {
        *out = events->stats;
        out->queued_commands = events->stop_count;
    }
}

uint32_t su_audio_commit_cursor_snapshot(const SuAudioEvents *events, SuAudioCommitCursor *out) {
    if (events == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = events->cursor;
    return SU_OK;
}

uint32_t su_audio_commit_cursor_restore(SuAudioEvents *events, const SuAudioCommitCursor *cursor) {
    if (events == NULL || cursor == NULL || cursor->session_generation != events->cursor.session_generation) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (cursor->next_seq < events->cursor.next_seq) {
        return SU_E_STALE_REVISION;
    }
    events->cursor = *cursor;
    return SU_OK;
}

uint32_t su_audio_events_session(SuAudioEvents *events, uint64_t session_generation) {
    if (events == NULL || session_generation <= events->cursor.session_generation) {
        return SU_E_INVALID_ARGUMENT;
    }
    su_audio_events_update(events);
    if (events->stats.instances != 0u || events->stop_count != 0u || events->consumed != events->cached) {
        return SU_E_RESOURCE_LIMIT;
    }
    events->cursor.session_generation = session_generation;
    events->cursor.next_seq = 0u;
    return SU_OK;
}

uint32_t su_audio_events_view(const SuAudioEvents *events, uint32_t slot, SuAudioEventsDesc *desc,
    SuAudioEventInstanceView *instance, SuAudioEventLeafView *leaves, uint32_t capacity) {
    const EventInstance *source;
    uint32_t index;
    if (events == NULL || desc == NULL || instance == NULL || (leaves == NULL && capacity != 0u)
        || slot >= events->desc.max_instances) {
        return SU_E_INVALID_ARGUMENT;
    }
    *desc = events->desc;
    source = &events->instances[slot];
    memset(instance, 0, sizeof *instance);
    instance->state = source->state;
    if (source->state == 0u) {
        return SU_OK;
    }
    instance->handle = instance_handle(slot, source->generation);
    instance->emitter = source->emitter;
    instance->order = source->order;
    instance->scope = source->scope;
    instance->count = source->count;
    for (index = 0; index < source->count && index < capacity; index++) {
        const EventLeaf *leaf = &source->leaves[index];
        leaves[index].data = leaf->data;
        leaves[index].start_sample = leaf->at;
        leaves[index].state = leaf->state;
        leaves[index].state = leaf->state == 3u ? 2u : leaf->state;
        leaves[index].gain = events->pool != NULL ? leaf->ctl_gain : leaf->voice_desc.gain;
        leaves[index].pitch = events->pool != NULL ? leaf->ctl_pitch : leaf->voice_desc.pitch;
        leaves[index].lowpass_hz = leaf->lowpass_hz;
    }
    return SU_OK;
}
