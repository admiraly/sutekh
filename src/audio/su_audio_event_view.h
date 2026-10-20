#ifndef SU_AUDIO_EVENT_VIEW_H
#define SU_AUDIO_EVENT_VIEW_H

/* Private read-only view of the event runtime for inspection (A10). */

#include "sutekh/audio_event.h"

typedef struct SuAudioEventLeafView {
    SuAudioGraphLeaf data;
    uint64_t start_sample;
    uint32_t state; /* 0 pending, 1 playing, 2 finished */
    float gain;
    float pitch;
    float lowpass_hz;
} SuAudioEventLeafView;

typedef struct SuAudioEventInstanceView {
    SuAudioInstance handle;
    SuAudioEmitter emitter;
    uint64_t order;
    uint32_t state; /* 0 free, 1 active, 2 completed awaiting update */
    uint32_t scope;
    uint32_t count;
} SuAudioEventInstanceView;

/* Copies slot `slot` (0-based) and up to `capacity` leaves. Free slots return
 * SU_OK with state 0. Returns SU_E_INVALID_ARGUMENT past max_instances. */
uint32_t su_audio_events_view(const SuAudioEvents *events, uint32_t slot, SuAudioEventsDesc *desc,
    SuAudioEventInstanceView *instance, SuAudioEventLeafView *leaves, uint32_t capacity);

#endif
