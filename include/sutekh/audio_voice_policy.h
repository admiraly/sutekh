#ifndef SUTEKH_AUDIO_VOICE_POLICY_H
#define SUTEKH_AUDIO_VOICE_POLICY_H

#include <stdint.h>
#include "sutekh/audio.h"
#include "sutekh/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Audibility-ranked physical voices over a bounded logical pool (A05).
 * Single-owner presentation object: every call, including rendering, is
 * serialized by the owner. Ranking runs at fixed 128-frame quantum boundaries
 * of the pool clock (output sample frames). The legacy SuAudioVoiceDesc.priority
 * stealing in the legacy mixer is unchanged and unrelated. */

#define SU_AUDIO_VOICE_QUANTUM 128u

typedef uint64_t SuAudioLogicalVoice; /* 0 = none; generational */

enum {
    SU_AUDIO_LIMIT_GLOBAL = 0,   /* physical_limit counts every voice with this scope */
    SU_AUDIO_LIMIT_EVENT = 1,    /* ... sharing event_id */
    SU_AUDIO_LIMIT_EMITTER = 2,  /* ... sharing emitter */
    SU_AUDIO_LIMIT_BUS = 3,      /* ... sharing bus_id */
    SU_AUDIO_LIMIT_NODE = 4      /* ... sharing graph node_id */
};

enum {
    SU_AUDIO_VIRTUAL_ADVANCE = 0, /* cursor keeps advancing while virtual; resumes in place */
    SU_AUDIO_VIRTUAL_PAUSE = 1,   /* cursor frozen while virtual; resumes where it stopped */
    SU_AUDIO_VIRTUAL_RESTART = 2, /* resumes from frame 0 */
    SU_AUDIO_VIRTUAL_DISCARD = 3  /* demotion ends the voice */
};

#define SU_AUDIO_VOICE_LOOP UINT32_C(1) /* loops the whole clip; otherwise a one-shot */
#define SU_AUDIO_VOICE_HERO UINT32_C(2) /* may use the hero reservation */

/* Last state-change reason of a logical voice. Stable values. */
enum {
    SU_AUDIO_REASON_NONE = 0,
    SU_AUDIO_REASON_PHYSICAL = 1,         /* ranked into a physical slot */
    SU_AUDIO_REASON_RANK = 2,             /* virtual: outranked for the shared pool */
    SU_AUDIO_REASON_SCOPE_LIMIT = 3,      /* virtual: its physical_limit scope is full */
    SU_AUDIO_REASON_HERO_RESERVED = 4,    /* virtual: only hero slots remained */
    SU_AUDIO_REASON_TRANSITION_FULL = 5,  /* promotion deferred: no fade slot for the voice it would steal */
    SU_AUDIO_REASON_EXPIRED_END = 6,      /* one-shot passed its end while virtual */
    SU_AUDIO_REASON_EXPIRED_DEADLINE = 7, /* virtual longer than wake_deadline_frames */
    SU_AUDIO_REASON_DISCARDED = 8,        /* SU_AUDIO_VIRTUAL_DISCARD demotion */
    SU_AUDIO_REASON_STOPPED = 9,          /* owner stop */
    SU_AUDIO_REASON_FINISHED = 10,        /* one-shot played to its end */
    SU_AUDIO_REASON_INAUDIBLE = 11,       /* virtual: audibility at or below the pool floor */
    SU_AUDIO_REASON_COUNT = 12
};

enum {
    SU_AUDIO_VOICE_DEAD = 0,
    SU_AUDIO_VOICE_PHYSICAL = 1,
    SU_AUDIO_VOICE_VIRTUAL = 2,
    SU_AUDIO_VOICE_STOPPING = 3 /* physical release tail after an owner stop */
};

typedef struct SuAudioVoicePolicy {
    int32_t priority;              /* higher ranks first, before audibility */
    float distance_offset;         /* metres added to the reported distance before attenuation */
    uint32_t physical_limit;       /* max physical voices in limit_scope; 0 = unlimited */
    uint32_t limit_scope;          /* SU_AUDIO_LIMIT_* */
    uint32_t virtual_mode;         /* SU_AUDIO_VIRTUAL_* */
    uint64_t wake_deadline_frames; /* expire after this long virtual; 0 = never */
    uint32_t release_frames;       /* steal fade-out and wake fade-in length; 0 = hard switch */
    uint32_t flags;                /* SU_AUDIO_VOICE_* */
} SuAudioVoicePolicy;

typedef struct SuAudioVoicePoolDesc {
    uint32_t struct_size;
    uint32_t sample_rate;          /* informational; frames are the unit */
    uint32_t max_logical;          /* 1..65536 */
    uint32_t max_physical;         /* 1..max_logical */
    uint32_t hero_reserve;         /* physical slots only hero voices may use; < max_physical */
    uint32_t transition_reserve;   /* concurrent steal fade-out tails */
    float audibility_floor;        /* audibility <= floor never becomes physical; 0 = none */
} SuAudioVoicePoolDesc;

typedef struct SuAudioLogicalDesc {
    uint32_t struct_size;
    SuAudioVoicePolicy policy;
    const SuAudioClip *clip;       /* mono or stereo; must outlive the voice; rate must match */
    float gain;                    /* linear */
    float pan;                     /* -1..1, constant-sum for mono, balance for stereo */
    uint32_t event_id;
    uint32_t node_id;              /* graph leaf node ID */
    uint32_t bus_id;
    uint64_t emitter;
    uint64_t instance;
    uint64_t trigger_seq;          /* admission order; ties rank lower-first */
} SuAudioLogicalDesc;

typedef struct SuAudioVoiceInfo {
    uint32_t state;                /* SU_AUDIO_VOICE_* */
    uint32_t reason;               /* SU_AUDIO_REASON_* */
    uint64_t cursor;               /* next clip frame for this logical voice */
    uint64_t virtual_frames;       /* frames spent virtual since the last demotion */
    float audibility;              /* last ranking score input */
} SuAudioVoiceInfo;

typedef struct SuAudioVoiceStats {
    uint64_t clock;                /* first unrendered pool frame */
    uint32_t logical;
    uint32_t physical;
    uint32_t virtual_voices;
    uint32_t transitions;          /* fade-out tails in flight */
    uint32_t logical_peak;
    uint32_t physical_peak;
    uint32_t virtual_peak;
    uint32_t transition_peak;
    uint64_t admitted;
    uint64_t refused;              /* logical pool full */
    uint64_t stolen;               /* physical -> virtual demotions */
    uint64_t woken;                /* virtual -> physical promotions */
    uint64_t expired;
    uint64_t deferred;             /* promotions delayed by a full transition reserve */
    uint64_t reasons[SU_AUDIO_REASON_COUNT]; /* transitions into each reason */
    uint64_t runtime_bytes;        /* allocated payload, excluding allocator overhead */
} SuAudioVoiceStats;

typedef struct SuAudioVoicePool SuAudioVoicePool;

uint32_t su_audio_voice_pool_create(const SuAudioVoicePoolDesc *desc, SuAudioVoicePool **out);
void su_audio_voice_pool_destroy(SuAudioVoicePool *pool);
/* Admits a logical voice starting at the next unrendered quantum boundary.
 * Full pool: SU_E_RESOURCE_LIMIT and stats.refused++ (no implicit stealing of logical voices). */
uint32_t su_audio_voice_add(SuAudioVoicePool *pool, const SuAudioLogicalDesc *desc, SuAudioLogicalVoice *out);
/* Ranking input: audibility = desc.gain * gain_linear / max(1, distance_m + distance_offset).
 * Until called, gain_linear = 1 and distance_m = 0. Non-finite or negative input is rejected. */
uint32_t su_audio_voice_set_audibility(SuAudioVoicePool *pool, SuAudioLogicalVoice voice,
                                       float gain_linear, float distance_m);
/* Physical voices fade over release_frames (0 = policy release); virtual voices end immediately. */
uint32_t su_audio_voice_stop(SuAudioVoicePool *pool, SuAudioLogicalVoice voice, uint32_t release_frames);
uint32_t su_audio_voice_info(const SuAudioVoicePool *pool, SuAudioLogicalVoice voice, SuAudioVoiceInfo *out);
/* Interleaved stereo; any frame count; internal quanta are cached so chunking does not change output. */
uint32_t su_audio_voice_pool_render(SuAudioVoicePool *pool, float *out, uint32_t frames);
void su_audio_voice_pool_stats(const SuAudioVoicePool *pool, SuAudioVoiceStats *out);
const char *su_audio_voice_reason_name(uint32_t reason);

#ifdef __cplusplus
}
#endif
#endif
