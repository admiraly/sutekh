#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/audio_event.h"
#include "su_audio_event_int.h"
#include "su_audio_event_view.h"

typedef struct EventLeaf {
    SuAudioGraphLeaf data;
    SuAudioVoiceDesc voice_desc;
    SuAudioVoice voice;
    uint64_t at;
    uint32_t state;
    float lowpass_hz;
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
    status = su_audio_graph_expand(events->graph, trigger->event_id, events->desc.project_seed,
        trigger->emitter, trigger->commit_seq, events->scratch, events->desc.max_leaves_per_instance, &count, diagnostic);
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
            uint32_t status = render_quantum(events);
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
        leaves[index].gain = leaf->voice_desc.gain;
        leaves[index].pitch = leaf->voice_desc.pitch;
        leaves[index].lowpass_hz = leaf->lowpass_hz;
    }
    return SU_OK;
}
