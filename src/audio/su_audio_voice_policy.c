#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/audio_voice_policy.h"
#include "su_audio_state.h"
#include "su_audio_voice_int.h"

#define Q SU_AUDIO_VOICE_QUANTUM
#define NO_STOP UINT64_MAX

/* Integrated playback state of a controlled voice or tail (see SuAudioVoiceControls). */
typedef struct VoiceDsp {
    uint32_t controlled;
    uint32_t mono_mix;
    uint32_t fresh;           /* next quantum starts from the target gains and pitch (no ramp) */
    uint32_t lp_remain;
    double pos;               /* fractional source frame */
    float pitch, pitch_prev;
    float gl, gr, gl_prev, gr_prev;
    float lp_alpha, lp_target, lp_step;
    float lp_l, lp_r;
    float lp_hz;              /* cut-off of lp_target (coefficient cache) */
} VoiceDsp;

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
    uint32_t route;
    uint32_t stop_release;
    uint64_t start_at;        /* absolute pool frame; silent before */
    uint64_t stop_at;         /* NO_STOP when none */
    uint64_t tag;
    VoiceDsp dsp;
} PoolVoice;

typedef struct PoolTail {
    const SuAudioClip *clip;
    uint64_t cursor;
    float gain;
    float pan;
    uint32_t loop;
    uint32_t total;
    uint32_t remain;
    uint32_t route;
    uint64_t tag;
    SuAudioLogicalVoice voice;
    VoiceDsp dsp;
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
    SuAudioVoiceCapture *capture;
    uint32_t capture_capacity;
    uint32_t captured;
};

#define NO_FREE UINT32_MAX

static int finite_f(float v) { return v == v && v - v == 0.0f; }

static SuAudioLogicalVoice make_handle(const SuAudioVoicePool *pool, const PoolVoice *voice) {
    return ((uint64_t)voice->generation << 32) | (uint64_t)((uint32_t)(voice - pool->voices) + 1u);
}

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

static uint32_t add_voice(SuAudioVoicePool *pool, const SuAudioLogicalDesc *desc, uint64_t start_frame,
                          SuAudioLogicalVoice *out) {
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
    if (start_frame < pool->clock) return SU_E_INVALID_ARGUMENT;
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
    voice->start_at = start_frame;
    voice->stop_at = NO_STOP;
    pool->logical++;
    pool->stats.admitted++;
    if (pool->logical > pool->stats.logical_peak) pool->stats.logical_peak = pool->logical;
    *out = make_handle(pool, voice);
    return SU_OK;
}

uint32_t su_audio_voice_add(SuAudioVoicePool *pool, const SuAudioLogicalDesc *desc, SuAudioLogicalVoice *out) {
    return add_voice(pool, desc, pool ? pool->clock : 0u, out);
}

uint32_t su_audio_voice_add_at(SuAudioVoicePool *pool, const SuAudioLogicalDesc *desc, uint64_t start_frame,
                               SuAudioLogicalVoice *out) {
    return add_voice(pool, desc, start_frame, out);
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

static float lowpass_alpha(const SuAudioVoicePool *pool, float cutoff_hz) {
    double rate = pool->desc.sample_rate ? (double)pool->desc.sample_rate : 48000.0;
    if (cutoff_hz > 0.0f && (double)cutoff_hz < rate * 0.5) {
        double sine = sin(3.141592653589793 * (double)cutoff_hz / rate);
        double term = 2.0 * sine * sine;
        return (float)(sqrt(term * term + 2.0 * term) - term);
    }
    return 1.0f;
}

uint32_t su_audio_voice_set_controls(SuAudioVoicePool *pool, SuAudioLogicalVoice handle,
                                     const SuAudioVoiceControls *controls) {
    PoolVoice *voice;
    VoiceDsp *dsp;
    float alpha;
    uint32_t status = handle_index(pool, handle, &voice);
    if (status != SU_OK) return status;
    if (!controls || controls->struct_size != sizeof *controls || !finite_f(controls->gain_left) ||
        !finite_f(controls->gain_right) || controls->gain_left < 0.0f || controls->gain_right < 0.0f ||
        !finite_f(controls->pitch) || !(controls->pitch > 0.0f) || controls->pitch > 64.0f ||
        !finite_f(controls->lowpass_hz) || controls->lowpass_hz < 0.0f) return SU_E_INVALID_ARGUMENT;
    dsp = &voice->dsp;
    if (!dsp->controlled) {
        dsp->controlled = 1u;
        dsp->fresh = 1u;
        dsp->pos = (double)voice->cursor;
        dsp->lp_alpha = 1.0f;
        dsp->lp_target = 1.0f;
        dsp->lp_hz = 0.0f;
    }
    dsp->gl = controls->gain_left;
    dsp->gr = controls->gain_right;
    dsp->pitch = controls->pitch;
    dsp->mono_mix = controls->mono_mix != 0u;
    alpha = controls->lowpass_hz == dsp->lp_hz ? dsp->lp_target : lowpass_alpha(pool, controls->lowpass_hz);
    dsp->lp_hz = controls->lowpass_hz;
    if (dsp->fresh && !voice->played) {
        dsp->lp_alpha = dsp->lp_target = alpha;
        dsp->lp_remain = 0u;
    } else if (alpha != dsp->lp_target) {
        uint32_t glide = (pool->desc.sample_rate ? pool->desc.sample_rate : 48000u) / 100u;
        dsp->lp_target = alpha;
        dsp->lp_remain = glide ? glide : 1u;
        dsp->lp_step = (alpha - dsp->lp_alpha) / (float)dsp->lp_remain;
    }
    return SU_OK;
}

static void begin_stop(SuAudioVoicePool *pool, PoolVoice *voice, uint32_t release_frames) {
    if (voice->state == SU_AUDIO_VOICE_STOPPING && voice->release_remain <= release_frames) return;
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
    begin_stop(pool, voice, release_frames);
    return SU_OK;
}

uint32_t su_audio_voice_stop_at(SuAudioVoicePool *pool, SuAudioLogicalVoice handle, uint64_t frame,
                                uint32_t release_frames) {
    PoolVoice *voice;
    uint32_t status = handle_index(pool, handle, &voice);
    if (status != SU_OK) return status;
    if (frame < pool->clock) return SU_E_INVALID_ARGUMENT;
    if (frame <= voice->start_at && voice->state == SU_AUDIO_VOICE_VIRTUAL && !voice->played) {
        free_voice(pool, voice, SU_AUDIO_REASON_STOPPED);
        return SU_OK;
    }
    if (voice->stop_at == NO_STOP || frame < voice->stop_at ||
        (frame == voice->stop_at && release_frames < voice->stop_release)) {
        voice->stop_at = frame;
        voice->stop_release = release_frames ? release_frames : voice->desc.policy.release_frames;
    }
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

static int pending(const SuAudioVoicePool *pool, const PoolVoice *voice) {
    return voice->start_at >= pool->clock + Q;
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
        tail->route = voice->route;
        tail->tag = voice->tag;
        tail->voice = make_handle(pool, voice);
        tail->dsp = voice->dsp;
        if (tail->dsp.controlled) {
            /* the tail holds the gains it was last heard with; envelope scales them */
            tail->dsp.gl = tail->dsp.gl_prev * env;
            tail->dsp.gr = tail->dsp.gr_prev * env;
            tail->dsp.gl_prev = tail->dsp.gl;
            tail->dsp.gr_prev = tail->dsp.gr;
            tail->dsp.pitch = tail->dsp.pitch_prev;
            tail->dsp.fresh = 0u;
        }
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
        if (voice->stop_at < pool->clock + Q && !pending(pool, voice)) {
            /* a voice that is virtual when its stop comes due ends without sound */
            free_voice(pool, voice, SU_AUDIO_REASON_STOPPED);
        } else if (policy->wake_deadline_frames && voice->virtual_frames >= policy->wake_deadline_frames) {
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
        if (pending(pool, voice)) continue;
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
            if (voice->desc.policy.virtual_mode == SU_AUDIO_VIRTUAL_RESTART) {
                voice->cursor = 0u;
                voice->dsp.pos = 0.0;
            }
            voice->fade_total = voice->fade_remain = voice->desc.policy.release_frames;
            /* the filter restarts from silence under the fade-in; gains and pitch jump to their targets */
            voice->dsp.fresh = 1u;
            voice->dsp.lp_l = voice->dsp.lp_r = 0.0f;
            voice->dsp.lp_alpha = voice->dsp.lp_target;
            voice->dsp.lp_remain = 0u;
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

/* Linearly interpolated source frame at dsp->pos; the partner of the last frame is frame 0 when looping. */
static void dsp_sample(const SuAudioClip *clip, const VoiceDsp *dsp, int loop, float *sl, float *sr) {
    uint64_t i0 = (uint64_t)dsp->pos;
    double fr = dsp->pos - (double)i0;
    float l0 = clip_sample(clip, i0, 0u), r0 = clip_sample(clip, i0, 1u);
    if (fr != 0.0) {
        uint64_t i1 = i0 + 1u;
        float l1, r1;
        if (i1 >= clip->frames) i1 = loop ? 0u : clip->frames - 1u;
        l1 = clip_sample(clip, i1, 0u);
        r1 = clip_sample(clip, i1, 1u);
        l0 += (l1 - l0) * (float)fr;
        r0 += (r1 - r0) * (float)fr;
    }
    if (dsp->mono_mix && clip->channels == 2u) l0 = r0 = 0.5f * (l0 + r0);
    *sl = l0;
    *sr = r0;
}

/* One controlled frame at quantum offset f: filters, applies env and the ramped gains, advances the cursor.
 * Returns 0 when a one-shot ran past its end. */
static int dsp_frame(VoiceDsp *dsp, const SuAudioClip *clip, int loop, uint32_t f, float env, float *out,
                     float *mono) {
    float sl, sr, t = (float)(f + 1u) / (float)Q;
    float gl = dsp->gl_prev + (dsp->gl - dsp->gl_prev) * t;
    float gr = dsp->gr_prev + (dsp->gr - dsp->gr_prev) * t;
    float step = dsp->pitch_prev + (dsp->pitch - dsp->pitch_prev) * t;
    dsp_sample(clip, dsp, loop, &sl, &sr);
    if (dsp->lp_remain) {
        dsp->lp_remain--;
        dsp->lp_alpha = dsp->lp_remain ? dsp->lp_target - dsp->lp_step * (float)dsp->lp_remain : dsp->lp_target;
    }
    if (dsp->lp_alpha != 1.0f) {
        sl = dsp->lp_l + dsp->lp_alpha * (sl - dsp->lp_l);
        sr = dsp->lp_r + dsp->lp_alpha * (sr - dsp->lp_r);
        if (fabsf(sl) < 1.0e-20f) sl = 0.0f;
        if (fabsf(sr) < 1.0e-20f) sr = 0.0f;
    }
    dsp->lp_l = sl;
    dsp->lp_r = sr;
    out[0] += sl * gl * env;
    out[1] += sr * gr * env;
    if (mono) mono[f] = 0.5f * (sl + sr) * env;
    dsp->pos += (double)step;
    if (dsp->pos >= (double)clip->frames) {
        if (!loop) return 0;
        dsp->pos = fmod(dsp->pos, (double)clip->frames);
    }
    return 1;
}

static void dsp_end_quantum(VoiceDsp *dsp) {
    dsp->gl_prev = dsp->gl;
    dsp->gr_prev = dsp->gr;
    dsp->pitch_prev = dsp->pitch;
}

static void dsp_begin_quantum(VoiceDsp *dsp) {
    if (dsp->fresh) {
        dsp->gl_prev = dsp->gl;
        dsp->gr_prev = dsp->gr;
        dsp->pitch_prev = dsp->pitch;
        dsp->fresh = 0u;
    }
}

static float *capture_slot(SuAudioVoicePool *pool, SuAudioLogicalVoice handle, uint32_t route, uint32_t tail,
                           const VoiceDsp *dsp) {
    SuAudioVoiceCapture *entry;
    if (!pool->capture || pool->captured >= pool->capture_capacity || !dsp->controlled) return NULL;
    entry = &pool->capture[pool->captured++];
    entry->voice = handle;
    entry->route = route;
    entry->tail = tail;
    entry->gain_left = dsp->gl;
    entry->gain_right = dsp->gr;
    memset(entry->mono, 0, Q * sizeof(float));
    return entry->mono;
}

static void render_tail(SuAudioVoicePool *pool, PoolTail *tail, float *out) {
    uint32_t f;
    if (tail->dsp.controlled) {
        float *mono = capture_slot(pool, tail->voice, tail->route, 1u, &tail->dsp);
        for (f = 0u; f < Q && tail->remain; ++f) {
            tail->remain--;
            if (!dsp_frame(&tail->dsp, tail->clip, (int)tail->loop, f,
                           (float)tail->remain / (float)tail->total, out + f * 2u, mono)) {
                tail->remain = 0u;
            }
        }
        tail->cursor = (uint64_t)tail->dsp.pos;
        dsp_end_quantum(&tail->dsp);
        return;
    }
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

/* Renders a physical or stopping voice for the quantum. Returns nonzero when the voice must be freed, with the
 * reason in *reason. */
static int render_voice(SuAudioVoicePool *pool, PoolVoice *voice, float *out, uint32_t *reason) {
    const SuAudioClip *clip = voice->desc.clip;
    int loop = (voice->desc.policy.flags & SU_AUDIO_VOICE_LOOP) != 0u;
    uint32_t f = voice->start_at > pool->clock ? (uint32_t)(voice->start_at - pool->clock) : 0u;
    uint32_t stop = voice->stop_at < pool->clock + Q
        ? (voice->stop_at > pool->clock ? (uint32_t)(voice->stop_at - pool->clock) : 0u) : Q;
    VoiceDsp *dsp = &voice->dsp;
    float *mono = NULL;
    if (dsp->controlled) {
        dsp_begin_quantum(dsp);
        mono = capture_slot(pool, make_handle(pool, voice), voice->route, 0u, dsp);
    }
    voice->played = 1u;
    for (; f < Q; ++f) {
        float env = 1.0f;
        if (f == stop && voice->stop_at != NO_STOP) {
            voice->stop_at = NO_STOP;
            if (voice->stop_release == 0u) {
                *reason = SU_AUDIO_REASON_STOPPED;
                set_reason(pool, voice, SU_AUDIO_REASON_STOPPED);
                if (dsp->controlled) dsp_end_quantum(dsp);
                return 1;
            }
            begin_stop(pool, voice, voice->stop_release);
        }
        if (dsp->controlled) {
            if (dsp->pos >= (double)clip->frames) {
                if (!loop) break;
                dsp->pos = fmod(dsp->pos, (double)clip->frames);
            }
        } else if (voice->cursor >= clip->frames) {
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
        if (dsp->controlled) {
            int more = dsp_frame(dsp, clip, loop, f, env, out + f * 2u, mono);
            voice->cursor = (uint64_t)dsp->pos;
            if (!more) {
                voice->cursor = clip->frames;
                break;
            }
        } else {
            mix_frame(out + f * 2u, clip, voice->cursor, voice->desc.gain * env, voice->desc.pan);
            voice->cursor++;
        }
    }
    if (dsp->controlled) dsp_end_quantum(dsp);
    if (voice->state == SU_AUDIO_VOICE_STOPPING && !voice->release_remain) {
        *reason = SU_AUDIO_REASON_STOPPED;
        return 1;
    }
    if (!loop && voice->cursor >= clip->frames) {
        *reason = SU_AUDIO_REASON_FINISHED;
        return 1;
    }
    return 0;
}

static void render_quantum(SuAudioVoicePool *pool, float *const *routes, uint32_t route_count) {
    uint32_t i, transitions = 0u, physical = 0u, virt = 0u;
    pool->captured = 0u;
    rank(pool);
    for (i = 0u; i < pool->desc.transition_reserve; ++i) {
        PoolTail *tail = &pool->tails[i];
        if (!tail->remain) continue;
        transitions++;
        render_tail(pool, tail, routes[tail->route < route_count ? tail->route : 0u]);
    }
    for (i = 0u; i < pool->desc.max_logical; ++i) {
        PoolVoice *voice = &pool->voices[i];
        const SuAudioClip *clip = voice->desc.clip;
        int loop = (voice->desc.policy.flags & SU_AUDIO_VOICE_LOOP) != 0u;
        uint32_t reason = 0u;
        if (voice->state == SU_AUDIO_VOICE_VIRTUAL) {
            uint32_t offset;
            if (pending(pool, voice)) continue;
            offset = voice->start_at > pool->clock ? (uint32_t)(voice->start_at - pool->clock) : 0u;
            virt++;
            voice->virtual_frames += Q - offset;
            if (voice->desc.policy.virtual_mode == SU_AUDIO_VIRTUAL_ADVANCE) {
                if (voice->dsp.controlled) {
                    voice->dsp.pos += (double)(Q - offset) * (double)voice->dsp.pitch;
                    if (loop) voice->dsp.pos = fmod(voice->dsp.pos, (double)clip->frames);
                    voice->cursor = (uint64_t)voice->dsp.pos;
                } else {
                    voice->cursor += Q - offset;
                    if (loop) voice->cursor %= clip->frames;
                }
            }
            if (voice->start_at > pool->clock) voice->start_at = pool->clock;
            continue;
        }
        if (voice->state != SU_AUDIO_VOICE_PHYSICAL && voice->state != SU_AUDIO_VOICE_STOPPING) continue;
        physical++;
        if (render_voice(pool, voice, routes[voice->route < route_count ? voice->route : 0u], &reason)) {
            free_voice(pool, voice, reason);
        }
    }
    pool->stats.physical = physical;
    pool->stats.virtual_voices = virt;
    pool->stats.transitions = transitions;
    if (physical > pool->stats.physical_peak) pool->stats.physical_peak = physical;
    if (virt > pool->stats.virtual_peak) pool->stats.virtual_peak = virt;
    if (transitions > pool->stats.transition_peak) pool->stats.transition_peak = transitions;
    pool->clock += Q;
}

uint32_t su_audio_voice_pool_render(SuAudioVoicePool *pool, float *out, uint32_t frames) {
    uint32_t done = 0u;
    if (!pool || (!out && frames)) return SU_E_INVALID_ARGUMENT;
    while (done < frames) {
        uint32_t take;
        if (pool->cache_pos == Q) {
            float *route = pool->cache;
            if (pool->clock > UINT64_MAX - Q) return SU_E_RESOURCE_LIMIT;
            memset(pool->cache, 0, sizeof pool->cache);
            render_quantum(pool, &route, 1u);
            pool->cache_pos = 0u;
        }
        take = Q - pool->cache_pos;
        if (take > frames - done) take = frames - done;
        memcpy(out + (size_t)done * 2u, pool->cache + pool->cache_pos * 2u, (size_t)take * 2u * sizeof(float));
        pool->cache_pos += take;
        done += take;
    }
    return SU_OK;
}

uint32_t su_audio_voice_pool_quantum(SuAudioVoicePool *pool, float *const *routes, uint32_t route_count) {
    if (!pool || !routes || route_count == 0u || pool->cache_pos != Q) return SU_E_INVALID_ARGUMENT;
    if (pool->clock > UINT64_MAX - Q) return SU_E_RESOURCE_LIMIT;
    render_quantum(pool, routes, route_count);
    return SU_OK;
}

uint32_t su_audio_voice_pool_set_clock(SuAudioVoicePool *pool, uint64_t clock) {
    if (!pool || pool->logical != 0u || pool->cache_pos != Q) return SU_E_INVALID_ARGUMENT;
    pool->clock = clock;
    return SU_OK;
}

uint64_t su_audio_voice_pool_clock(const SuAudioVoicePool *pool) {
    return pool ? pool->clock : 0u;
}

void su_audio_voice_pool_desc(const SuAudioVoicePool *pool, SuAudioVoicePoolDesc *out) {
    if (out) {
        if (pool) *out = pool->desc;
        else memset(out, 0, sizeof *out);
    }
}

uint32_t su_audio_voice_set_route(SuAudioVoicePool *pool, SuAudioLogicalVoice handle, uint32_t route) {
    PoolVoice *voice;
    uint32_t status = handle_index(pool, handle, &voice);
    if (status != SU_OK) return status;
    voice->route = route;
    return SU_OK;
}

uint32_t su_audio_voice_set_tag(SuAudioVoicePool *pool, SuAudioLogicalVoice handle, uint64_t tag) {
    PoolVoice *voice;
    uint32_t status = handle_index(pool, handle, &voice);
    if (status != SU_OK) return status;
    voice->tag = tag;
    return SU_OK;
}

int su_audio_voice_pool_clip_busy(const SuAudioVoicePool *pool, const SuAudioClip *clip) {
    uint32_t i;
    if (!pool) return 0;
    for (i = 0u; i < pool->desc.transition_reserve; ++i) {
        if (pool->tails[i].remain && pool->tails[i].clip == clip) return 1;
    }
    return 0;
}

uint32_t su_audio_voice_pool_capture(SuAudioVoicePool *pool, SuAudioVoiceCapture *entries, uint32_t capacity) {
    uint32_t i;
    if (!pool || (!entries && capacity)) return SU_E_INVALID_ARGUMENT;
    for (i = 0u; i < capacity; ++i) {
        if (!entries[i].mono) return SU_E_INVALID_ARGUMENT;
    }
    pool->capture = capacity ? entries : NULL;
    pool->capture_capacity = capacity;
    pool->captured = 0u;
    return SU_OK;
}

uint32_t su_audio_voice_pool_captured(const SuAudioVoicePool *pool) {
    return pool ? pool->captured : 0u;
}

static void dsp_state(SuAudioStateIo *io, VoiceDsp *dsp) {
    SU_STATE(io, dsp->controlled);
    SU_STATE(io, dsp->mono_mix);
    SU_STATE(io, dsp->fresh);
    SU_STATE(io, dsp->lp_remain);
    SU_STATE(io, dsp->pos);
    SU_STATE(io, dsp->pitch);
    SU_STATE(io, dsp->pitch_prev);
    SU_STATE(io, dsp->gl);
    SU_STATE(io, dsp->gr);
    SU_STATE(io, dsp->gl_prev);
    SU_STATE(io, dsp->gr_prev);
    SU_STATE(io, dsp->lp_alpha);
    SU_STATE(io, dsp->lp_target);
    SU_STATE(io, dsp->lp_step);
    SU_STATE(io, dsp->lp_l);
    SU_STATE(io, dsp->lp_r);
    SU_STATE(io, dsp->lp_hz);
}

static void desc_state(SuAudioStateIo *io, SuAudioLogicalDesc *desc) {
    SU_STATE(io, desc->policy.priority);
    SU_STATE(io, desc->policy.distance_offset);
    SU_STATE(io, desc->policy.physical_limit);
    SU_STATE(io, desc->policy.limit_scope);
    SU_STATE(io, desc->policy.virtual_mode);
    SU_STATE(io, desc->policy.wake_deadline_frames);
    SU_STATE(io, desc->policy.release_frames);
    SU_STATE(io, desc->policy.flags);
    SU_STATE(io, desc->gain);
    SU_STATE(io, desc->pan);
    SU_STATE(io, desc->event_id);
    SU_STATE(io, desc->node_id);
    SU_STATE(io, desc->bus_id);
    SU_STATE(io, desc->emitter);
    SU_STATE(io, desc->instance);
    SU_STATE(io, desc->trigger_seq);
}

uint32_t su_audio_voice_pool_state(SuAudioVoicePool *pool, SuAudioStateIo *io, SuAudioClipResolver resolve,
                                   void *user) {
    uint32_t i, tails;
    if (!pool || !io || pool->cache_pos != Q || (!io->save && io->data && !resolve)) return SU_E_INVALID_ARGUMENT;
    tails = pool->desc.transition_reserve;
    SU_STATE(io, pool->clock);
    SU_STATE(io, pool->free_head);
    SU_STATE(io, pool->logical);
    SU_STATE(io, pool->stats);
    for (i = 0u; i < pool->desc.max_logical; ++i) {
        PoolVoice *voice = &pool->voices[i];
        SU_STATE(io, voice->generation);
        SU_STATE(io, voice->state);
        SU_STATE(io, voice->next_free);
        if (voice->state == SU_AUDIO_VOICE_DEAD) continue;
        voice->desc.struct_size = sizeof voice->desc;
        desc_state(io, &voice->desc);
        SU_STATE(io, voice->reason);
        SU_STATE(io, voice->played);
        SU_STATE(io, voice->cursor);
        SU_STATE(io, voice->virtual_frames);
        SU_STATE(io, voice->audibility);
        SU_STATE(io, voice->fade_total);
        SU_STATE(io, voice->fade_remain);
        SU_STATE(io, voice->release_total);
        SU_STATE(io, voice->release_remain);
        SU_STATE(io, voice->route);
        SU_STATE(io, voice->stop_release);
        SU_STATE(io, voice->start_at);
        SU_STATE(io, voice->stop_at);
        SU_STATE(io, voice->tag);
        dsp_state(io, &voice->dsp);
        if (!io->save && io->data && !io->failed) {
            voice->desc.clip = resolve(user, voice->tag);
            if (!voice->desc.clip) return SU_E_SCHEMA_MISMATCH;
        }
    }
    for (i = 0u; i < tails; ++i) {
        PoolTail *tail = &pool->tails[i];
        SU_STATE(io, tail->remain);
        if (!tail->remain) continue;
        SU_STATE(io, tail->cursor);
        SU_STATE(io, tail->gain);
        SU_STATE(io, tail->pan);
        SU_STATE(io, tail->loop);
        SU_STATE(io, tail->total);
        SU_STATE(io, tail->route);
        SU_STATE(io, tail->tag);
        SU_STATE(io, tail->voice);
        dsp_state(io, &tail->dsp);
        if (!io->save && io->data && !io->failed) {
            tail->clip = resolve(user, tail->tag);
            if (!tail->clip) return SU_E_SCHEMA_MISMATCH;
        }
    }
    return io->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
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
