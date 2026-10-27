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
 * Library: su_audio (always built; WASAPI output on Windows, SDL3 output on other platforms where the SDL3 headers
 * were found, otherwise a defined refusal; Ogg Vorbis decoding when the pinned stb_vorbis is present, see
 * tools/bootstrap_asset_deps.py). */
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

/* PlayStation 4-bit ADPCM (PS-ADPCM, SPU ADPCM, VAG). 16-byte frames of 28 samples: byte 0 = (filter << 4) | shift,
 * byte 1 = flags, then 14 bytes of signed nibbles, low nibble first. Each sample is
 * ((nibble << 12 as int16) >> shift) + ((hist1 * c1 + hist2 * c2) >> 6) (arithmetic shifts, no rounding term),
 * clamped to int16, with (c1, c2) = (0,0) (60,0) (115,-52) (98,-55) (122,-60); filter > 4 reads as 0 and
 * shift > 12 as 9. History starts at 0 for each channel. Flags: bit 0 end, bit 1 repeat (inside the loop), bit 2
 * loop start; end and repeat together mark the last frame of the loop. Floats are sample / 32768. */
typedef struct SuAudioPsAdpcmDesc {
    uint32_t sample_rate;          /* 1000..384000 */
    uint32_t channels;             /* 1 or 2 */
    uint32_t interleave;           /* bytes per channel block, a multiple of 16; 0 = contiguous (mono only). The data
                                    * must hold whole groups of channels * interleave bytes. */
    int stop_at_end_flag;          /* non-zero: decoding stops after the first frame (of channel 0) with flag bit 0 */
} SuAudioPsAdpcmDesc;

/* Decodes raw frames into a clip. loop_start_frame and loop_end_frame (each optional) receive the loop region in
 * sample frames from channel 0's flags (start: the first loop-start frame; end: just past the first end+repeat
 * frame at or after it, else the end of the clip), or 0 and 0 without a loop-start flag. SU_E_PARSE with a reason on
 * malformed input, SU_E_INVALID_ARGUMENT for a null pointer or a bad descriptor. */
uint32_t su_audio_clip_from_ps_adpcm(const void *bytes, size_t len, const SuAudioPsAdpcmDesc *d, SuAudioClip **out,
                                     uint32_t *loop_start_frame, uint32_t *loop_end_frame, char message[256]);
/* A "VAGp" file: 48-byte big-endian header (magic at 0, version at 4, data size at 0x0C, sample rate at 0x10, name at
 * 0x20), then mono frames from 0x30 up to the data size. Decoding stops at the end flag. Loop outputs as above. */
uint32_t su_audio_clip_from_vag(const void *bytes, size_t len, SuAudioClip **out, uint32_t *loop_start_frame,
                                uint32_t *loop_end_frame, char message[256]);

#define SU_AUDIO_WAVE_SINE 0u
#define SU_AUDIO_WAVE_SQUARE 1u
#define SU_AUDIO_WAVE_SAW 2u
#define SU_AUDIO_WAVE_NOISE 3u     /* deterministic white noise (xorshift) */
/* A generated mono tone of seconds at sample_rate, amplitude 1, with 5 ms fades unless the clip is meant to loop
 * (loopable: whole periods and no fades). */
uint32_t su_audio_clip_tone(uint32_t wave, float frequency, float seconds, uint32_t sample_rate, int loopable,
                            SuAudioClip **out);
void su_audio_clip_free(SuAudioClip *clip);

/* ---- streamed clips ----
 * For long music: nothing is decoded up front. A stream holds a decoder (WAV or Ogg Vorbis, from a file or from
 * memory the caller keeps alive) and a bounded window of decoded frames (ring_frames, default 16384); resident
 * memory is the window plus the decoder state, independent of the clip's length. Decoding happens in the mixer's
 * render call (chunks of 1024 frames, on whatever thread renders), so offline output is deterministic and a stream
 * is sample-identical to the same clip decoded whole (same decoder, same float conversion). Because decoding and
 * file reads run on the render thread, short sounds should stay SuAudioClip; seeks (a seek command, a loop wrap
 * outside the window) cost one decoder seek.
 * One stream serves one voice at a time (su_audio_play refuses a stream that is in use); a finished or stolen
 * voice frees it. Streams are not owned by the mixer. */
typedef struct SuAudioStream SuAudioStream;

typedef struct SuAudioStreamInfo {
    uint64_t frames;
    uint32_t channels;
    uint32_t sample_rate;
    uint32_t ring_frames;
    uint64_t ring_bytes;           /* the window */
    uint64_t decoder_bytes;        /* decoder state (Ogg: the codebooks and per-frame buffers stb_vorbis reports) */
    uint64_t resident_bytes;       /* everything the stream holds, accounted */
    uint64_t frames_decoded;       /* since open, including re-decodes after seeks */
    uint32_t seeks;                /* decoder seeks since open */
} SuAudioStreamInfo;

/* Format is detected from the first bytes (RIFF WAVE or Ogg). ring_frames 0 = 16384; at least 2048.
 * SU_E_IO (cannot open), SU_E_PARSE (with a reason), SU_E_UNSUPPORTED_METHOD (Ogg without stb_vorbis). */
uint32_t su_audio_stream_open_file(const char *path, uint32_t ring_frames, SuAudioStream **out, char message[256]);
uint32_t su_audio_stream_open_memory(const void *bytes, size_t len, uint32_t ring_frames, SuAudioStream **out,
                                     char message[256]);
/* SU_E_INVALID_ARGUMENT while a voice is using it (stop the voice and render once first). */
uint32_t su_audio_stream_close(SuAudioStream *s);
void su_audio_stream_info(const SuAudioStream *s, SuAudioStreamInfo *out);
int su_audio_stream_in_use(const SuAudioStream *s);

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
    /* Exactly one of clip and stream is set. */
    SuAudioStream *stream;
    /* Start offset and loop points, in frames of the source (its own sample rate), or in seconds. A seconds field
     * is used only while its frames field is 0 (both non-zero: SU_E_INVALID_ARGUMENT); seconds convert with
     * llround(seconds * clip rate). Defaults: start 0, loop start 0, loop end 0 = the end of the clip. Playback
     * begins at the start offset (also when looping, where it may lie inside or before the loop region); on
     * reaching loop_end a looping voice continues at loop_start. Without SU_AUDIO_LOOP the clip plays to its real
     * end and the loop points are ignored. Requires start < frames, loop_start < loop_end <= frames, and, when
     * looping, start < loop_end; otherwise SU_E_INVALID_ARGUMENT. */
    uint64_t start_frame;
    uint64_t loop_start;
    uint64_t loop_end;
    double start_seconds;
    double loop_start_seconds;
    double loop_end_seconds;
} SuAudioVoiceDesc;

/* Starts a voice (applied at the next render). SU_E_RESOURCE_LIMIT when no voice could be had or the command
 * queue is full. */
uint32_t su_audio_play(SuAudioMixer *m, const SuAudioVoiceDesc *d, SuAudioVoice *out);
/* Moves a spatial voice (from an interpolated transform) and sets its gain. Stale handles: SU_E_STALE_HANDLE. */
uint32_t su_audio_voice_update(SuAudioMixer *m, SuAudioVoice v, SuWorldPos pos, SuVec3f vel, float gain);
/* Changes a playing voice's pitch (playback rate multiplier, 0 < pitch <= 64; applied at the next render, with the rate
 * gliding linearly from the old pitch to the new over that block, so the read position stays continuous: no click, no
 * phase jump). Stacks with Doppler. Stale handles: SU_E_STALE_HANDLE. */
uint32_t su_audio_voice_set_pitch(SuAudioMixer *m, SuAudioVoice v, float pitch);
/* Sets a voice's linear gain alone (applied at the next render, ramped across the block). Stale: SU_E_STALE_HANDLE. */
uint32_t su_audio_voice_set_gain(SuAudioMixer *m, SuAudioVoice v, float gain);
uint32_t su_audio_voice_set_lowpass(SuAudioMixer *m, SuAudioVoice v, float cutoff_hz);
/* Stops a voice with a linear fade of fade_s seconds (0: at the next render, still click-free over one block). */
uint32_t su_audio_stop(SuAudioMixer *m, SuAudioVoice v, float fade_s);
/* Jumps a voice to an exact source frame (applied at the next render; the next output sample is that frame, no
 * interpolation offset). frame must be below the voice's loop end when it loops, else below the clip length:
 * SU_E_INVALID_ARGUMENT. Streams seek the decoder, accurate to the sample. */
uint32_t su_audio_seek(SuAudioMixer *m, SuAudioVoice v, uint64_t frame);
uint32_t su_audio_seek_seconds(SuAudioMixer *m, SuAudioVoice v, double seconds);
/* Position of a voice in source frames (the next frame to play, fractional with pitch) as of the last render. */
uint32_t su_audio_voice_position(const SuAudioMixer *m, SuAudioVoice v, double *frames);
/* 1 while a voice is queued or playing (as known to the game thread after su_audio_update). */
int su_audio_voice_active(const SuAudioMixer *m, SuAudioVoice v);
/* Listener (the camera): position, orientation (+x right, +y up, looking down -z), velocity. */
uint32_t su_audio_listener(SuAudioMixer *m, SuWorldPos pos, SuQuatf rot, SuVec3f vel);
uint32_t su_audio_master_gain(SuAudioMixer *m, float gain);
/* Soft limiter on the final mix (applied at the next render). knee in [0.1, 1): samples up to knee pass unchanged, larger
 * ones bend smoothly towards 1 (never beyond it) (knee + (1 - knee) tanh((|x| - knee) / (1 - knee))), so the output does not hard-clamp. 0 (the
 * default) turns it off: the plain clamp. SuAudioStats.peak is still the level before the limiter and .clipped counts the
 * samples that would have clamped. Stateless, so renders stay deterministic. */
uint32_t su_audio_master_limiter(SuAudioMixer *m, float knee);
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
    float peak;                    /* largest absolute output sample of the last render, before the clamp / limiter */
    uint32_t clipped;              /* samples clamped to [-1, 1] in the last render (with the limiter: samples that would have been) */
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
    char name[128];                /* endpoint; the SDL3 backend adds SDL's audio driver in parentheses */
    uint32_t sample_rate;          /* the stream's rate (the mixer's; the system converts) */
    uint32_t buffer_frames;        /* device buffer, in the stream's (the mixer's) frames */
    double latency_ms;             /* stream latency: WASAPI's GetStreamLatency; the SDL3 backend's device buffer
                                    * plus the one device period it keeps queued in SDL's stream */
} SuAudioDeviceInfo;

/* Opens the default output and starts rendering the mixer. Windows: WASAPI shared mode, event driven. Other
 * platforms: SDL3's default playback device, with the SDL3 library opened at run time (SUTEKH_SDL3_LIBRARY
 * overrides its name); SDL's own audio thread calls back once per device period instead of a thread of ours. Both
 * take float32 stereo at the mixer's rate and let the system convert. SU_E_UNSUPPORTED_METHOD where no backend
 * exists (a build without SDL3 headers), the SDL3 library cannot be loaded, or no device is present; the message
 * says which. */
uint32_t su_audio_device_open(SuAudioMixer *m, SuAudioDevice **out, SuAudioDeviceInfo *info, char message[256]);
/* Stops the thread and closes the device. */
void su_audio_device_close(SuAudioDevice *d);
/* Frames the device thread has rendered so far. */
uint64_t su_audio_device_frames(const SuAudioDevice *d);

#define SU_AUDIO_DEVICE_HISTOGRAM_BINS 24u

typedef struct SuAudioDeviceStats {
    uint64_t callbacks;
    uint64_t callback_total_us;
    uint64_t callback_max_us;
    uint64_t callback_over_budget;
    uint64_t callback_us[SU_AUDIO_DEVICE_HISTOGRAM_BINS];
    uint64_t interval_us[SU_AUDIO_DEVICE_HISTOGRAM_BINS];
    uint64_t interval_max_us;
    uint64_t empty_buffers;
    uint64_t wait_timeouts;
    uint64_t backend_errors;
    double stream_latency_ms;
    uint32_t sample_rate;
    uint32_t buffer_frames;
    char name[128];
} SuAudioDeviceStats;

/* Cumulative device-thread timing, read from any thread (approximate snapshot: counters are race-free, the set is
 * not a transaction). callback_* measure one submission (SDL3: one stream callback), interval_us the time between
 * submissions. empty_buffers counts submissions that found the device buffer (WASAPI) or SDL's stream empty,
 * startup included: a starvation indicator, not a hardware glitch count. wait_timeouts is always 0 on the SDL3
 * backend, which has no event wait. See docs/AUDIO_DEVICE_LATENCY.md. */
void su_audio_device_stats(const SuAudioDevice *device, SuAudioDeviceStats *out);

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
