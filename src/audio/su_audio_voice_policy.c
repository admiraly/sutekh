#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/audio_voice_policy.h"

#define Q SU_AUDIO_VOICE_QUANTUM

typedef struct PoolVoice {
    SuAudioLogicalDesc desc;
    uint32_t generation;
    uint32_t state;
    uint32_t reason;
    uint32_t played;          /* rendered at least one physical frame */
    uint64_t cursor;
    uint64_t virtual_frames;
    float audibility;
    uint32_t fade_total;      /* wake fade-in */
    uint32_t fade_remain;
    uint32_t release_total;   /* owner stop release */
    uint32_t release_remain;
    uint32_t next_free;
} PoolVoice;

typedef struct PoolTail {
    const SuAudioClip *clip;
    uint64_t cursor;
    float gain;
    float pan;
    uint32_t loop;
    uint32_t total;
    uint32_t remain;
} PoolTail;

typedef struct RankKey {
    int32_t priority;
    float audibility;
    uint64_t seq;
    uint32_t node;
    uint32_t index;
} RankKey;

struct SuAudioVoicePool {
    SuAudioVoicePoolDesc desc;
    PoolVoice *voices;
    PoolTail *tails;
    RankKey *keys;
    uint32_t *accepted;
    uint8_t *desired;
    uint32_t free_head;
    uint32_t logical;
    uint64_t clock;
    float cache[Q * 2u];
    uint32_t cache_pos;
    SuAudioVoiceStats stats;
};

#define NO_FREE UINT32_MAX

static int finite_f(float v) { return v == v && v - v == 0.0f; }

static uint32_t handle_index(const SuAudioVoicePool *pool, SuAudioLogicalVoice handle, PoolVoice **out) {
    uint32_t index;
    if (!pool || !handle) return SU_E_STALE_HANDLE;
    index = (uint32_t)(handle & 0xffffffffu);
    if (index == 0u || index > pool->desc.max_logical) return SU_E_STALE_HANDLE;
    *out = &pool->voices[index - 1u];
    if ((*out)->generation != (uint32_t)(handle >> 32) || (*out)->state == SU_AUDIO_VOICE_DEAD) return SU_E_STALE_HANDLE;
    return SU_OK;
}

static void set_reason(SuAudioVoicePool *pool, PoolVoice *voice, uint32_t reason) {
    if (voice->reason != reason) {
        voice->reason = reason;
        pool->stats.reasons[reason]++;
    }
}

static void free_voice(SuAudioVoicePool *pool, PoolVoice *voice, uint32_t reason) {
    set_reason(pool, voice, reason);
    voice->state = SU_AUDIO_VOICE_DEAD;
    voice->generation++;
    if (voice->generation == 0u) voice->generation = 1u;
    voice->next_free = pool->free_head;
    pool->free_head = (uint32_t)(voice - pool->voices);
    pool->logical--;
}

uint32_t su_audio_voice_pool_create(const SuAudioVoicePoolDesc *desc, SuAudioVoicePool **out) {
    SuAudioVoicePool *pool;
    uint32_t i;
    if (!out) return SU_E_INVALID_ARGUMENT;
    *out = NULL;
    if (!desc || desc->struct_size != sizeof *desc) return SU_E_INVALID_ARGUMENT;
    if (desc->max_logical == 0u || desc->max_logical > 65536u || desc->max_physical == 0u ||
        desc->max_physical > desc->max_logical || desc->hero_reserve >= desc->max_physical ||
        desc->transition_reserve > desc->max_logical || !finite_f(desc->audibility_floor) ||
        desc->audibility_floor < 0.0f) return SU_E_INVALID_ARGUMENT;
    pool = (SuAudioVoicePool *)calloc(1u, sizeof *pool);
    if (!pool) return SU_E_RESOURCE_LIMIT;
    pool->desc = *desc;
    pool->voices = (PoolVoice *)calloc(desc->max_logical, sizeof *pool->voices);
    pool->tails = (PoolTail *)calloc(desc->transition_reserve ? desc->transition_reserve : 1u, sizeof *pool->tails);
    pool->keys = (RankKey *)calloc(desc->max_logical, sizeof *pool->keys);
    pool->accepted = (uint32_t *)calloc(desc->max_physical, sizeof *pool->accepted);
    pool->desired = (uint8_t *)calloc(desc->max_logical, 1u);
    if (!pool->voices || !pool->tails || !pool->keys || !pool->accepted || !pool->desired) {
        su_audio_voice_pool_destroy(pool);
        return SU_E_RESOURCE_LIMIT;
    }
    for (i = 0u; i < desc->max_logical; ++i) {
        pool->voices[i].generation = 1u;
        pool->voices[i].next_free = i + 1u < desc->max_logical ? i + 1u : NO_FREE;
    }
    pool->free_head = 0u;
    pool->cache_pos = Q;
    pool->stats.runtime_bytes = sizeof *pool + (uint64_t)desc->max_logical *
        (sizeof *pool->voices + sizeof *pool->keys + 1u) +
        (uint64_t)(desc->transition_reserve ? desc->transition_reserve : 1u) * sizeof *pool->tails +
        (uint64_t)desc->max_physical * sizeof *pool->accepted;
    *out = pool;
    return SU_OK;
}

void su_audio_voice_pool_destroy(SuAudioVoicePool *pool) {
    if (!pool) return;
    free(pool->voices);
    free(pool->tails);
    free(pool->keys);
    free(pool->accepted);
    free(pool->desired);
    free(pool);
}

uint32_t su_audio_voice_add(SuAudioVoicePool *pool, const SuAudioLogicalDesc *desc, SuAudioLogicalVoice *out) {
    PoolVoice *voice;
    uint32_t index;
    const SuAudioVoicePolicy *policy;
    if (out) *out = 0u;
    if (!pool || !desc || !out || desc->struct_size != sizeof *desc || !desc->clip) return SU_E_INVALID_ARGUMENT;
    policy = &desc->policy;
    if (!desc->clip->samples || desc->clip->frames == 0u || (desc->clip->channels != 1u && desc->clip->channels != 2u) ||
        (pool->desc.sample_rate && desc->clip->sample_rate != pool->desc.sample_rate) ||
        !finite_f(desc->gain) || desc->gain < 0.0f || !finite_f(desc->pan) || desc->pan < -1.0f || desc->pan > 1.0f ||
        !finite_f(policy->distance_offset) || policy->limit_scope > SU_AUDIO_LIMIT_NODE ||
        policy->virtual_mode > SU_AUDIO_VIRTUAL_DISCARD ||
        (policy->flags & ~(SU_AUDIO_VOICE_LOOP | SU_AUDIO_VOICE_HERO)) != 0u) return SU_E_INVALID_ARGUMENT;
    if (pool->free_head == NO_FREE) {
        pool->stats.refused++;
        return SU_E_RESOURCE_LIMIT;
    }
    index = pool->free_head;
    voice = &pool->voices[index];
    pool->free_head = voice->next_free;
    {
        uint32_t generation = voice->generation;
        memset(voice, 0, sizeof *voice);
        voice->generation = generation;
    }
    voice->desc = *desc;
    voice->state = SU_AUDIO_VOICE_VIRTUAL; /* ranked at the next quantum boundary */
    voice->reason = SU_AUDIO_REASON_NONE;
    voice->audibility = desc->gain / (policy->distance_offset > 1.0f ? policy->distance_offset : 1.0f);
    pool->logical++;
    pool->stats.admitted++;
    if (pool->logical > pool->stats.logical_peak) pool->stats.logical_peak = pool->logical;
    *out = ((uint64_t)voice->generation << 32) | (uint64_t)(index + 1u);
    return SU_OK;
}

uint32_t su_audio_voice_set_audibility(SuAudioVoicePool *pool, SuAudioLogicalVoice handle,
                                       float gain_linear, float distance_m) {
    PoolVoice *voice;
    float d;
    uint32_t status = handle_index(pool, handle, &voice);
    if (status != SU_OK) return status;
    if (!finite_f(gain_linear) || gain_linear < 0.0f || !finite_f(distance_m) || distance_m < 0.0f)
        return SU_E_INVALID_ARGUMENT;
    d = distance_m + voice->desc.policy.distance_offset;
    voice->audibility = voice->desc.gain * gain_linear / (d > 1.0f ? d : 1.0f);
    return SU_OK;
}

uint32_t su_audio_voice_stop(SuAudioVoicePool *pool, SuAudioLogicalVoice handle, uint32_t release_frames) {
    PoolVoice *voice;
    uint32_t status = handle_index(pool, handle, &voice);
    if (status != SU_OK) return status;
    if (release_frames == 0u) release_frames = voice->desc.policy.release_frames;
    if (voice->state == SU_AUDIO_VOICE_VIRTUAL || release_frames == 0u) {
        free_voice(pool, voice, SU_AUDIO_REASON_STOPPED);
        return SU_OK;
    }
    if (voice->state == SU_AUDIO_VOICE_STOPPING && voice->release_remain <= release_frames) return SU_OK;
    if (voice->state == SU_AUDIO_VOICE_STOPPING) {
        /* shorter release continues from the current envelope */
        float g = (float)voice->release_remain / (float)voice->release_total;
        voice->release_total = (uint32_t)ceilf((float)release_frames / (g > 0.0f ? g : 1.0f));
        if (voice->release_total < release_frames) voice->release_total = release_frames;
    } else {
        voice->release_total = release_frames;
    }
    voice->release_remain = release_frames;
    voice->state = SU_AUDIO_VOICE_STOPPING;
    set_reason(pool, voice, SU_AUDIO_REASON_STOPPED);
    return SU_OK;
}

uint32_t su_audio_voice_info(const SuAudioVoicePool *pool, SuAudioLogicalVoice handle, SuAudioVoiceInfo *out) {
    PoolVoice *voice;
    uint32_t status;
    if (!out) return SU_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    status = handle_index(pool, handle, &voice);
    if (status != SU_OK) return status;
    out->state = voice->state;
    out->reason = voice->reason;
    out->cursor = voice->cursor;
    out->virtual_frames = voice->virtual_frames;
    out->audibility = voice->audibility;
    return SU_OK;
}

static int rank_compare(const void *a, const void *b) {
    const RankKey *x = (const RankKey *)a;
    const RankKey *y = (const RankKey *)b;
    if (x->priority != y->priority) return x->priority > y->priority ? -1 : 1;
    if (x->audibility != y->audibility) return x->audibility > y->audibility ? -1 : 1;
    if (x->seq != y->seq) return x->seq < y->seq ? -1 : 1;
    if (x->node != y->node) return x->node < y->node ? -1 : 1;
    return x->index < y->index ? -1 : (x->index > y->index ? 1 : 0);
}

static uint64_t scope_key(const SuAudioLogicalDesc *desc, uint32_t scope) {
    switch (scope) {
    case SU_AUDIO_LIMIT_EVENT: return desc->event_id;
    case SU_AUDIO_LIMIT_EMITTER: return desc->emitter;
    case SU_AUDIO_LIMIT_BUS: return desc->bus_id;
    case SU_AUDIO_LIMIT_NODE: return desc->node_id;
    default: return 0u;
    }
}

static int start_tail(SuAudioVoicePool *pool, const PoolVoice *voice) {
    uint32_t i;
    uint32_t release = voice->desc.policy.release_frames;
    float env;
    if (release == 0u || !voice->played) return 1; /* hard switch requested, or nothing audible yet */
    for (i = 0u; i < pool->desc.transition_reserve; ++i) {
        PoolTail *tail = &pool->tails[i];
        if (tail->remain != 0u) continue;
        env = voice->fade_total ? (float)(voice->fade_total - voice->fade_remain) / (float)voice->fade_total : 1.0f;
        tail->clip = voice->desc.clip;
        tail->cursor = voice->cursor;
        tail->gain = voice->desc.gain * env;
        tail->pan = voice->desc.pan;
        tail->loop = (voice->desc.policy.flags & SU_AUDIO_VOICE_LOOP) != 0u;
        tail->total = release;
        tail->remain = release;
        return 1;
    }
    return 0;
}

static void expire_virtual(SuAudioVoicePool *pool) {
    uint32_t i;
    for (i = 0u; i < pool->desc.max_logical; ++i) {
        PoolVoice *voice = &pool->voices[i];
        const SuAudioVoicePolicy *policy = &voice->desc.policy;
        if (voice->state != SU_AUDIO_VOICE_VIRTUAL) continue;
        if (policy->wake_deadline_frames && voice->virtual_frames >= policy->wake_deadline_frames) {
            free_voice(pool, voice, SU_AUDIO_REASON_EXPIRED_DEADLINE);
            pool->stats.expired++;
        } else if (!(policy->flags & SU_AUDIO_VOICE_LOOP) && voice->cursor >= voice->desc.clip->frames) {
            free_voice(pool, voice, SU_AUDIO_REASON_EXPIRED_END);
            pool->stats.expired++;
        }
    }
}

static void rank(SuAudioVoicePool *pool) {
    uint32_t i, n = 0u, accepted = 0u, nonhero = 0u, stopping = 0u, physical = 0u, slots, nonhero_cap;
    expire_virtual(pool);
    for (i = 0u; i < pool->desc.max_logical; ++i) {
        PoolVoice *voice = &pool->voices[i];
        pool->desired[i] = 0u;
        if (voice->state == SU_AUDIO_VOICE_STOPPING) stopping++;
        if (voice->state != SU_AUDIO_VOICE_PHYSICAL && voice->state != SU_AUDIO_VOICE_VIRTUAL) continue;
        pool->keys[n].priority = voice->desc.policy.priority;
        pool->keys[n].audibility = voice->audibility;
        pool->keys[n].seq = voice->desc.trigger_seq;
        pool->keys[n].node = voice->desc.node_id;
        pool->keys[n].index = i;
        n++;
    }
    qsort(pool->keys, n, sizeof *pool->keys, rank_compare);
    slots = pool->desc.max_physical > stopping ? pool->desc.max_physical - stopping : 0u;
    nonhero_cap = pool->desc.max_physical - pool->desc.hero_reserve;
    nonhero += stopping;
    for (i = 0u; i < n; ++i) {
        PoolVoice *voice = &pool->voices[pool->keys[i].index];
        const SuAudioVoicePolicy *policy = &voice->desc.policy;
        uint32_t reason = SU_AUDIO_REASON_PHYSICAL;
        int hero = (policy->flags & SU_AUDIO_VOICE_HERO) != 0u;
        if (voice->audibility <= pool->desc.audibility_floor) reason = SU_AUDIO_REASON_INAUDIBLE;
        else if (accepted >= slots) reason = SU_AUDIO_REASON_RANK;
        else if (!hero && nonhero >= nonhero_cap) reason = SU_AUDIO_REASON_HERO_RESERVED;
        else if (policy->physical_limit) {
            uint32_t j, same = 0u;
            uint64_t key = scope_key(&voice->desc, policy->limit_scope);
            for (j = 0u; j < accepted; ++j)
                if (scope_key(&pool->voices[pool->accepted[j]].desc, policy->limit_scope) == key) same++;
            if (same >= policy->physical_limit) reason = SU_AUDIO_REASON_SCOPE_LIMIT;
        }
        if (reason == SU_AUDIO_REASON_PHYSICAL) {
            pool->desired[pool->keys[i].index] = 1u;
            pool->accepted[accepted++] = pool->keys[i].index;
            if (!hero) nonhero++;
        } else if (voice->state == SU_AUDIO_VOICE_VIRTUAL) {
            set_reason(pool, voice, reason);
        } else {
            voice->reason = reason; /* applied on demotion below */
            voice->reason |= 0x80000000u;
        }
    }
    /* demotions: they free slots, but only once a fade tail is available */
    for (i = n; i-- > 0u;) { /* lowest rank is stolen first */
        PoolVoice *voice = &pool->voices[pool->keys[i].index];
        uint32_t reason;
        if (voice->state != SU_AUDIO_VOICE_PHYSICAL) continue;
        if (pool->desired[pool->keys[i].index]) { physical++; continue; }
        reason = voice->reason & 0x7fffffffu;
        voice->reason = SU_AUDIO_REASON_PHYSICAL;
        if (!start_tail(pool, voice)) {
            physical++;
            continue;
        }
        pool->stats.stolen++;
        if (voice->desc.policy.virtual_mode == SU_AUDIO_VIRTUAL_DISCARD) {
            free_voice(pool, voice, SU_AUDIO_REASON_DISCARDED);
            continue;
        }
        voice->state = SU_AUDIO_VOICE_VIRTUAL;
        voice->virtual_frames = 0u;
        voice->fade_total = voice->fade_remain = 0u;
        set_reason(pool, voice, reason);
    }
    physical += stopping;
    /* promotions in rank order */
    for (i = 0u; i < accepted; ++i) {
        PoolVoice *voice = &pool->voices[pool->accepted[i]];
        if (voice->state != SU_AUDIO_VOICE_VIRTUAL) continue;
        if (physical >= pool->desc.max_physical) {
            if (voice->reason != SU_AUDIO_REASON_TRANSITION_FULL) pool->stats.deferred++;
            set_reason(pool, voice, SU_AUDIO_REASON_TRANSITION_FULL);
            continue;
        }
        physical++;
        voice->state = SU_AUDIO_VOICE_PHYSICAL;
        if (voice->played || voice->cursor != 0u) {
            pool->stats.woken++;
            if (voice->desc.policy.virtual_mode == SU_AUDIO_VIRTUAL_RESTART) voice->cursor = 0u;
            voice->fade_total = voice->fade_remain = voice->desc.policy.release_frames;
        }
        set_reason(pool, voice, SU_AUDIO_REASON_PHYSICAL);
    }
}

static float clip_sample(const SuAudioClip *clip, uint64_t cursor, uint32_t ch) {
    return clip->samples[cursor * clip->channels + (clip->channels == 2u ? ch : 0u)];
}

static void mix_frame(float *out, const SuAudioClip *clip, uint64_t cursor, float gain, float pan) {
    float lg = pan > 0.0f ? 1.0f - pan : 1.0f;
    float rg = pan < 0.0f ? 1.0f + pan : 1.0f;
    out[0] += clip_sample(clip, cursor, 0u) * gain * lg;
    out[1] += clip_sample(clip, cursor, 1u) * gain * rg;
}

static void render_quantum(SuAudioVoicePool *pool) {
    uint32_t i, f, transitions = 0u, physical = 0u, virt = 0u;
    float *out = pool->cache;
    memset(pool->cache, 0, sizeof pool->cache);
    rank(pool);
    for (i = 0u; i < pool->desc.transition_reserve; ++i) {
        PoolTail *tail = &pool->tails[i];
        if (!tail->remain) continue;
        transitions++;
        for (f = 0u; f < Q && tail->remain; ++f) {
            if (tail->cursor >= tail->clip->frames) {
                if (!tail->loop) { tail->remain = 0u; break; }
                tail->cursor = 0u;
            }
            tail->remain--;
            mix_frame(out + f * 2u, tail->clip, tail->cursor, tail->gain * (float)tail->remain / (float)tail->total,
                      tail->pan);
            tail->cursor++;
        }
    }
    for (i = 0u; i < pool->desc.max_logical; ++i) {
        PoolVoice *voice = &pool->voices[i];
        const SuAudioClip *clip = voice->desc.clip;
        int loop = (voice->desc.policy.flags & SU_AUDIO_VOICE_LOOP) != 0u;
        if (voice->state == SU_AUDIO_VOICE_VIRTUAL) {
            virt++;
            voice->virtual_frames += Q;
            if (voice->desc.policy.virtual_mode == SU_AUDIO_VIRTUAL_ADVANCE) {
                voice->cursor += Q;
                if (loop) voice->cursor %= clip->frames;
            }
            continue;
        }
        if (voice->state != SU_AUDIO_VOICE_PHYSICAL && voice->state != SU_AUDIO_VOICE_STOPPING) continue;
        physical++;
        voice->played = 1u;
        for (f = 0u; f < Q; ++f) {
            float env = 1.0f;
            if (voice->cursor >= clip->frames) {
                if (!loop) break;
                voice->cursor = 0u;
            }
            if (voice->fade_remain) {
                env = (float)(voice->fade_total - voice->fade_remain) / (float)voice->fade_total;
                voice->fade_remain--;
            }
            if (voice->state == SU_AUDIO_VOICE_STOPPING) {
                if (!voice->release_remain) break;
                voice->release_remain--;
                env *= (float)voice->release_remain / (float)voice->release_total;
            }
            mix_frame(out + f * 2u, clip, voice->cursor, voice->desc.gain * env, voice->desc.pan);
            voice->cursor++;
        }
        if (voice->state == SU_AUDIO_VOICE_STOPPING && !voice->release_remain) {
            free_voice(pool, voice, SU_AUDIO_REASON_STOPPED);
        } else if (!loop && voice->cursor >= clip->frames) {
            free_voice(pool, voice, SU_AUDIO_REASON_FINISHED);
        }
    }
    pool->stats.physical = physical;
    pool->stats.virtual_voices = virt;
    pool->stats.transitions = transitions;
    if (physical > pool->stats.physical_peak) pool->stats.physical_peak = physical;
    if (virt > pool->stats.virtual_peak) pool->stats.virtual_peak = virt;
    if (transitions > pool->stats.transition_peak) pool->stats.transition_peak = transitions;
    pool->clock += Q;
    pool->cache_pos = 0u;
}

uint32_t su_audio_voice_pool_render(SuAudioVoicePool *pool, float *out, uint32_t frames) {
    uint32_t done = 0u;
    if (!pool || (!out && frames)) return SU_E_INVALID_ARGUMENT;
    while (done < frames) {
        uint32_t take;
        if (pool->cache_pos == Q) {
            if (pool->clock > UINT64_MAX - Q) return SU_E_RESOURCE_LIMIT;
            render_quantum(pool);
        }
        take = Q - pool->cache_pos;
        if (take > frames - done) take = frames - done;
        memcpy(out + (size_t)done * 2u, pool->cache + pool->cache_pos * 2u, (size_t)take * 2u * sizeof(float));
        pool->cache_pos += take;
        done += take;
    }
    return SU_OK;
}

void su_audio_voice_pool_stats(const SuAudioVoicePool *pool, SuAudioVoiceStats *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!pool) return;
    *out = pool->stats;
    out->clock = pool->clock;
    out->logical = pool->logical;
}

const char *su_audio_voice_reason_name(uint32_t reason) {
    static const char *const names[SU_AUDIO_REASON_COUNT] = {
        "none", "physical", "rank", "scope_limit", "hero_reserved", "transition_full",
        "expired_end", "expired_deadline", "discarded", "stopped", "finished", "inaudible"
    };
    return reason < SU_AUDIO_REASON_COUNT ? names[reason] : "unknown";
}
