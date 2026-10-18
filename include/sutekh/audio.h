/* Sutekh audio (M5): clips, a voice mixer with 3D panning, distance attenuation and Doppler, an output device, and
 * consumption of committed host events.
 *
 * Boundary (docs/RENDERING_AND_ASSETS.md section 6): audio consumes committed effect events and interpolated
 * presentation transforms. It never reads or writes simulation state, never runs inside a simulation step, and a
 * re-simulated tick (rollback) does not replay a sound: SuAudioEventCursor passes each event sequence number once.
 *
 * Threads: one game thread calls everything except su_audio_mixer_render, which the device thread (or an offline
 * caller) runs. Commands travel through a bounded single-producer single-consumer queue; voice slots are
 * preallocated; the render path never allocates, locks or blocks. Without an output device the mixer still runs
 * offline (render to a buffer), which is how the tests check it.
 *
 * Library: su_audio (always built; WASAPI output on Windows; Ogg Vorbis decoding when the pinned stb_vorbis is
 * present, see tools/bootstrap_asset_deps.py). */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef SUTEKH_AUDIO_H
#define SUTEKH_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#include "sutekh/host.h"
#include "sutekh/render.h"
#include "sutekh/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- clips: immutable float PCM ---- */

typedef struct SuAudioClip {
    float *samples;            /* interleaved */
    uint32_t frames;
    uint32_t channels;         /* 1 or 2 */
    uint32_t sample_rate;
} SuAudioClip;

/* RIFF WAVE: PCM 8/16/24/32-bit or IEEE float 32, mono or stereo. SU_E_PARSE with a reason. */
uint32_t su_audio_clip_from_wav(const void *bytes, size_t len, SuAudioClip **out, char message[256]);
/* Ogg Vorbis (stb_vorbis). SU_E_UNSUPPORTED_METHOD when built without it. */
uint32_t su_audio_clip_from_ogg(const void *bytes, size_t len, SuAudioClip **out, char message[256]);
int su_audio_ogg_available(void);

#define SU_AUDIO_WAVE_SINE 0u
#define SU_AUDIO_WAVE_SQUARE 1u
#define SU_AUDIO_WAVE_SAW 2u
#define SU_AUDIO_WAVE_NOISE 3u     /* deterministic white noise (xorshift) */
/* A generated mono tone of seconds at sample_rate, amplitude 1, with 5 ms fades unless the clip is meant to loop
 * (loopable: whole periods and no fades). */
uint32_t su_audio_clip_tone(uint32_t wave, float frequency, float seconds, uint32_t sample_rate, int loopable,
                            SuAudioClip **out);
void su_audio_clip_free(SuAudioClip *clip);

/* ---- mixer ---- */

typedef struct SuAudioMixer SuAudioMixer;
typedef uint64_t SuAudioVoice;     /* 0 = none; generational */

typedef struct SuAudioMixerDesc {
    uint32_t struct_size;          /* sizeof(SuAudioMixerDesc) */
    uint32_t sample_rate;          /* output rate; 0 = 48000 */
    uint32_t max_voices;           /* 0 = 64 */
    uint32_t command_capacity;     /* queued commands between renders; 0 = 1024 */
    float speed_of_sound;          /* units per second for Doppler; 0 = 343 */
} SuAudioMixerDesc;

uint32_t su_audio_mixer_create(const SuAudioMixerDesc *d, SuAudioMixer **out);
/* The caller stops any device first. Clips are not owned by the mixer. */
void su_audio_mixer_destroy(SuAudioMixer *m);
uint32_t su_audio_mixer_rate(const SuAudioMixer *m);

#define SU_AUDIO_LOOP 0x1u
#define SU_AUDIO_SPATIAL 0x2u      /* position, velocity and distances apply */

typedef struct SuAudioVoiceDesc {
    const SuAudioClip *clip;       /* must outlive the voice */
    float gain;                    /* linear */
    float pitch;                   /* playback rate multiplier; 0 = 1 */
    uint32_t flags;                /* SU_AUDIO_* */
    SuWorldPos pos;                /* SU_AUDIO_SPATIAL */
    SuVec3f vel;                   /* units per second */
    float min_distance;            /* full gain inside; 0 = 1 */
    float max_distance;            /* silent beyond; 0 = no limit */
    float rolloff;                 /* gain = (min / d)^rolloff between min and max; 0 = 1 */
    float doppler;                 /* 0 = none, 1 = physical */
    uint32_t priority;             /* when every voice is busy, the oldest voice of a lower or equal priority is
                                    * stolen; otherwise the play is refused */
} SuAudioVoiceDesc;

/* Starts a voice (applied at the next render). SU_E_RESOURCE_LIMIT when no voice could be had or the command
 * queue is full. */
uint32_t su_audio_play(SuAudioMixer *m, const SuAudioVoiceDesc *d, SuAudioVoice *out);
/* Moves a spatial voice (from an interpolated transform) and sets its gain. Stale handles: SU_E_STALE_HANDLE. */
uint32_t su_audio_voice_update(SuAudioMixer *m, SuAudioVoice v, SuWorldPos pos, SuVec3f vel, float gain);
/* Stops a voice with a linear fade of fade_s seconds (0: at the next render, still click-free over one block). */
uint32_t su_audio_stop(SuAudioMixer *m, SuAudioVoice v, float fade_s);
/* 1 while a voice is queued or playing (as known to the game thread after su_audio_update). */
int su_audio_voice_active(const SuAudioMixer *m, SuAudioVoice v);
/* Listener (the camera): position, orientation (+x right, +y up, looking down -z), velocity. */
uint32_t su_audio_listener(SuAudioMixer *m, SuWorldPos pos, SuQuatf rot, SuVec3f vel);
uint32_t su_audio_master_gain(SuAudioMixer *m, float gain);
/* Game thread, once per frame: collects voices that finished so their slots can be reused. */
void su_audio_update(SuAudioMixer *m);

/* Renders frames of stereo float32 (interleaved L R) into out, applying queued commands first. Deterministic for
 * the same command sequence and block sizes. Real-time safe: no allocation, no locks. */
void su_audio_mixer_render(SuAudioMixer *m, float *out, uint32_t frames);

typedef struct SuAudioStats {
    uint32_t voices_active;        /* as of the last render */
    uint32_t voices_stolen;
    uint32_t plays_refused;
    uint32_t commands_dropped;     /* queue full */
    uint64_t frames_rendered;
    float peak;                    /* largest absolute output sample of the last render, before the clamp */
    uint32_t clipped;              /* samples clamped to [-1, 1] in the last render */
} SuAudioStats;
void su_audio_mixer_stats(const SuAudioMixer *m, SuAudioStats *out);

/* Pure helpers used by the mixer (exposed for tests and tools). */
/* Gain of a source at distance d. */
float su_audio_attenuation(float d, float min_distance, float max_distance, float rolloff);
/* Equal-power stereo gains for a listener-space direction (x right): pan = x / |dir|. */
void su_audio_pan(SuVec3f dir_listener, float *left, float *right);
/* Doppler pitch factor for a source and listener (positions and velocities in the same frame), clamped to
 * [0.25, 4]. */
float su_audio_doppler(SuVec3f listener_pos, SuVec3f listener_vel, SuVec3f source_pos, SuVec3f source_vel,
                       float speed_of_sound, float amount);

/* ---- output device ---- */

typedef struct SuAudioDevice SuAudioDevice;

typedef struct SuAudioDeviceInfo {
    char name[128];
    uint32_t sample_rate;          /* the stream's rate (the mixer's; the system converts) */
    uint32_t buffer_frames;        /* device buffer */
    double latency_ms;             /* stream latency reported by the device */
} SuAudioDeviceInfo;

/* Opens the default output (WASAPI shared mode, event driven, float32 stereo at the mixer's rate) and starts a
 * thread that renders the mixer. SU_E_UNSUPPORTED_METHOD where no backend exists (non-Windows builds) or no device
 * is present; message says which. */
uint32_t su_audio_device_open(SuAudioMixer *m, SuAudioDevice **out, SuAudioDeviceInfo *info, char message[256]);
/* Stops the thread and closes the device. */
void su_audio_device_close(SuAudioDevice *d);
/* Frames the device thread has rendered so far. */
uint64_t su_audio_device_frames(const SuAudioDevice *d);

/* ---- host events ---- */

/* Passes each committed host event once, in sequence order, even when rollback re-emits it with the same sequence
 * number. Feed it su_host_events_read results (or su_host_events_for_tick of a tick as it is committed). */
typedef struct SuAudioEventCursor {
    uint64_t next_seq;
} SuAudioEventCursor;
/* 1 when ev has not been passed before (and advances the cursor), 0 for an event already seen. */
int su_audio_event_new(SuAudioEventCursor *c, const SuHostEvent *ev);

#ifdef __cplusplus
}
#endif

#endif
