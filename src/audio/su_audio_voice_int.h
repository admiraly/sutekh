#ifndef SU_AUDIO_VOICE_INT_H
#define SU_AUDIO_VOICE_INT_H

/* Private interface between the voice pool and the integrated event runtime. */

#include "sutekh/audio_voice_policy.h"

/* Renders one 128-frame quantum, adding each voice into routes[route] (interleaved stereo, caller-cleared).
 * Requires an empty render cache (never mixed with su_audio_voice_pool_render). */
uint32_t su_audio_voice_pool_quantum(SuAudioVoicePool *pool, float *const *routes, uint32_t route_count,
                                    float *loudness);
uint32_t su_audio_voice_set_loudness(SuAudioVoicePool *pool, SuAudioLogicalVoice voice, float loudness_db);
/* Moves the clock of an idle pool (no voices, empty cache). */
uint32_t su_audio_voice_pool_set_clock(SuAudioVoicePool *pool, uint64_t clock);
uint64_t su_audio_voice_pool_clock(const SuAudioVoicePool *pool);
void su_audio_voice_pool_desc(const SuAudioVoicePool *pool, SuAudioVoicePoolDesc *out);
uint32_t su_audio_voice_set_route(SuAudioVoicePool *pool, SuAudioLogicalVoice voice, uint32_t route);
/* Nonzero while a steal fade tail still reads clip. */
int su_audio_voice_pool_clip_busy(const SuAudioVoicePool *pool, const SuAudioClip *clip);

/* Per-voice capture for spatial clustering: for every voice rendered in the last quantum, the logical handle,
 * route and its mono pre-pan signal (after envelope and low-pass, before the channel gains). */
typedef struct SuAudioVoiceCapture {
    SuAudioLogicalVoice voice;
    uint32_t route;
    uint32_t tail;                 /* nonzero: a steal fade tail (voice is the stolen voice's handle) */
    float gain_left;
    float gain_right;
    float *mono;                   /* SU_AUDIO_VOICE_QUANTUM samples */
} SuAudioVoiceCapture;

/* Enables capture into caller storage (capacity entries, each with its own mono buffer); NULL disables. */
uint32_t su_audio_voice_pool_capture(SuAudioVoicePool *pool, SuAudioVoiceCapture *entries, uint32_t capacity);
uint32_t su_audio_voice_pool_captured(const SuAudioVoicePool *pool);

/* Checkpoint state. Clip pointers are written as the voice/tail tag set below and resolved on load. */
typedef const SuAudioClip *(*SuAudioClipResolver)(void *user, uint64_t tag);
uint32_t su_audio_voice_set_tag(SuAudioVoicePool *pool, SuAudioLogicalVoice voice, uint64_t tag);
struct SuAudioStateIo;
uint32_t su_audio_voice_pool_state(SuAudioVoicePool *pool, struct SuAudioStateIo *io, SuAudioClipResolver resolve,
                                   void *user);

#endif
