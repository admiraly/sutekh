/* Non-Windows output for sutekh/audio.h: SDL3's default playback device, float32 stereo at the mixer's rate (SDL
 * converts to the device's own format and rate), driven by SDL's audio thread, which calls back once per device
 * period. The SDL3 library is opened at run time with dlopen (no link-time dependency) as the Linux window backend
 * does (src/platform/window_sdl3.c); the two keep separate loaders because su_audio must work without su_window
 * (SUTEKH_WINDOW=OFF, or a host with sound but no window). Every entry point used exists in SDL 3.2.0 and its
 * signature is checked at compile time against the pinned headers. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#if !defined(_WIN32)

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <dlfcn.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "platform/su_platform.h"
#include "sutekh/audio.h"
#include "su_audio_int.h"
#include "su_audio_output.h"

/* The stream's input format: interleaved float32 stereo, the mixer's output. */
#define SU_SDL3_FRAME_BYTES 8u
/* Largest device period this backend accepts, in the stream's input frames (bounds the scratch buffer). */
#define SU_SDL3_MAX_PERIOD (1u << 20)

/* ---------- run-time loading ---------- */

#define SDL_FNS(X)                                                                                            \
    X(int, SDL_GetVersion, (void))                                                                            \
    X(bool, SDL_InitSubSystem, (SDL_InitFlags flags))                                                         \
    X(void, SDL_QuitSubSystem, (SDL_InitFlags flags))                                                         \
    X(const char *, SDL_GetError, (void))                                                                     \
    X(bool, SDL_ClearError, (void))                                                                           \
    X(const char *, SDL_GetCurrentAudioDriver, (void))                                                        \
    X(SDL_AudioStream *, SDL_OpenAudioDeviceStream,                                                           \
      (SDL_AudioDeviceID devid, const SDL_AudioSpec *spec, SDL_AudioStreamCallback callback, void *userdata))  \
    X(void, SDL_DestroyAudioStream, (SDL_AudioStream * stream))                                               \
    X(bool, SDL_ResumeAudioStreamDevice, (SDL_AudioStream * stream))                                          \
    X(bool, SDL_PauseAudioStreamDevice, (SDL_AudioStream * stream))                                           \
    X(bool, SDL_PutAudioStreamData, (SDL_AudioStream * stream, const void *buf, int len))                     \
    X(int, SDL_GetAudioStreamQueued, (SDL_AudioStream * stream))                                              \
    X(SDL_AudioDeviceID, SDL_GetAudioStreamDevice, (SDL_AudioStream * stream))                                \
    X(const char *, SDL_GetAudioDeviceName, (SDL_AudioDeviceID devid))                                        \
    X(bool, SDL_GetAudioDeviceFormat, (SDL_AudioDeviceID devid, SDL_AudioSpec * spec, int *sample_frames))

#define X(ret, name, args) typedef ret(SDLCALL *PFN_##name) args;
SDL_FNS(X)
#undef X
/* Each pointer type must be the type of the header's declaration (unevaluated: no reference to the symbol). */
#define X(ret, name, args) \
    _Static_assert(_Generic((&name), PFN_##name: 1, default: 0), #name " differs from the SDL3 headers");
SDL_FNS(X)
#undef X

typedef struct SdlAudioFns {
    void *lib;
#define X(ret, name, args) PFN_##name name;
    SDL_FNS(X)
#undef X
} SdlAudioFns;

/* The library stays loaded once opened, as in the window backend: SDL may leave helper threads behind. */
static SdlAudioFns g_sdl;

static int load_sdl(char *msg) {
    const char *name = getenv("SUTEKH_SDL3_LIBRARY");
    int v;
    if (g_sdl.lib != NULL) {
        return 1;
    }
    if (name == NULL || name[0] == '\0') {
        name = "libSDL3.so.0";
    }
    g_sdl.lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (g_sdl.lib == NULL) {
        const char *why = dlerror();
        snprintf(msg, 256, "the SDL3 library could not be loaded (install SDL 3.2 or later, or set "
                 "SUTEKH_SDL3_LIBRARY): %s", why != NULL ? why : name);
        return 0;
    }
#define X(ret, fname, args)                                       \
    *(void **)&g_sdl.fname = dlsym(g_sdl.lib, #fname);            \
    if (g_sdl.fname == NULL) {                                    \
        const char *why = dlerror();                              \
        snprintf(msg, 256, "%s has no entry point %s: %s", name, #fname, why != NULL ? why : ""); \
        dlclose(g_sdl.lib);                                       \
        memset(&g_sdl, 0, sizeof g_sdl);                          \
        return 0;                                                 \
    }
    SDL_FNS(X)
#undef X
    v = g_sdl.SDL_GetVersion();
    if (v < SDL_VERSIONNUM(3, 2, 0)) {
        snprintf(msg, 256, "%s is SDL %d.%d.%d; 3.2.0 or later is required", name, SDL_VERSIONNUM_MAJOR(v),
                 SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
        dlclose(g_sdl.lib);
        memset(&g_sdl, 0, sizeof g_sdl);
        return 0;
    }
    return 1;
}

/* ---------- the device ---------- */

struct SuAudioDevice {
    SuAudioMixer *mixer;
    SuAudioPullFn pull;     /* pull-mode source (integrated runtime ring) instead of the mixer */
    void *pull_user;
    SDL_AudioStream *stream;
    float *scratch;         /* render buffer: sized once at open, never resized from the callback */
    uint32_t scratch_frames;
    uint32_t period_frames; /* one device period in the stream's input frames */
    uint64_t last_us;       /* entry time of the previous callback (callback thread only) */
    _Atomic uint64_t frames;
    _Atomic uint64_t callbacks, callback_total_us, callback_max_us, callback_over_budget;
    _Atomic uint64_t callback_us[SU_AUDIO_DEVICE_HISTOGRAM_BINS];
    _Atomic uint64_t interval_us[SU_AUDIO_DEVICE_HISTOGRAM_BINS];
    _Atomic uint64_t interval_max_us, empty_buffers, backend_errors;
    SuAudioDeviceInfo info;
    SuAudioGap recovery;
};

static uint64_t now_us(void) {
    uint64_t ns = 0u;
    (void)su_platform_monotonic_ns(&ns);
    return ns / 1000u;
}

/* Runs on SDL's audio thread, once per device period, with the stream lock held (SDL mutexes are reentrant, so
 * SDL_GetAudioStreamQueued and SDL_PutAudioStreamData may be called here). Real-time safe: renders into the buffer
 * allocated at open and allocates, locks (beyond the stream's own reentrant lock) and blocks on nothing.
 *
 * Supply policy: the SDL stream is the render-ahead buffer this backend owns, the counterpart of the WASAPI device
 * buffer's padding. Each callback tops the stream up to two device periods of input, so one period stays queued while
 * the device consumes one; a callback that finds the stream empty is a starvation, counted and de-clicked as in the
 * WASAPI path. additional_amount (what the current request still needs) is not used: the queue is read directly, so
 * the top-up does not depend on how SDL rounded the request. */
static void SDLCALL device_callback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount) {
    SuAudioDevice *d = userdata;
    uint64_t begin;
    uint64_t elapsed;
    uint32_t period;
    uint32_t queued;
    uint32_t want;
    int queued_bytes;
    (void)additional_amount;
    if (d == NULL) {
        return;
    }
    begin = now_us();
    if (d->last_us != 0u) {
        uint64_t interval = begin - d->last_us;
        atomic_fetch_add(&d->interval_us[su_audio_time_bin(interval)], 1u);
        if (interval > atomic_load(&d->interval_max_us)) {
            atomic_store(&d->interval_max_us, interval);
        }
    }
    d->last_us = begin;
    period = total_amount > 0 ? (uint32_t)total_amount / SU_SDL3_FRAME_BYTES : 0u;
    if (period == 0u) {
        period = d->period_frames; /* the device reported no request size: use the one measured at open */
    }
    queued_bytes = g_sdl.SDL_GetAudioStreamQueued(stream);
    queued = queued_bytes > 0 ? (uint32_t)queued_bytes / SU_SDL3_FRAME_BYTES : 0u;
    want = period * 2u > queued ? period * 2u - queued : 0u;
    if (want > d->scratch_frames) {
        /* The device buffer grew after open: supply what fits and say so rather than allocate here. */
        want = d->scratch_frames;
        atomic_fetch_add(&d->backend_errors, 1u);
    }
    if (want == 0u) {
        return;
    }
    if (queued == 0u) {
        atomic_fetch_add(&d->empty_buffers, 1u);
        if (atomic_load(&d->frames) != 0u) {
            memset(&d->recovery, 0, sizeof d->recovery);
            d->recovery.gap = 1;
        }
    }
    if (d->pull != NULL) {
        d->pull(d->pull_user, d->scratch, want);
    } else {
        su_audio_mixer_render(d->mixer, d->scratch, want);
    }
    if (d->recovery.gap || d->recovery.recovering != 0u) {
        su_audio_gap_apply(&d->recovery, d->scratch, want, want);
    }
    if (!g_sdl.SDL_PutAudioStreamData(stream, d->scratch, (int)(want * SU_SDL3_FRAME_BYTES))) {
        atomic_fetch_add(&d->backend_errors, 1u);
        return;
    }
    elapsed = now_us() - begin;
    atomic_fetch_add(&d->callback_us[su_audio_time_bin(elapsed)], 1u);
    atomic_fetch_add(&d->callback_total_us, elapsed);
    atomic_fetch_add(&d->callbacks, 1u);
    if (elapsed > atomic_load(&d->callback_max_us)) {
        atomic_store(&d->callback_max_us, elapsed);
    }
    if (elapsed * d->info.sample_rate > (uint64_t)want * 1000000u) {
        atomic_fetch_add(&d->callback_over_budget, 1u);
    }
    atomic_fetch_add(&d->frames, want);
}

static uint32_t fail(char *msg, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(msg, 256, fmt, ap);
    va_end(ap);
    return SU_E_UNSUPPORTED_METHOD;
}

static uint32_t device_start(SuAudioMixer *m, SuAudioPullFn pull, void *user, uint32_t rate, SuAudioDevice **out,
                             SuAudioDeviceInfo *info, char message[256]) {
    char local[256];
    char *msg = message != NULL ? message : local;
    SuAudioDevice *d;
    SDL_AudioSpec spec;
    SDL_AudioSpec dev_spec;
    SDL_AudioDeviceID dev;
    const char *dev_name;
    const char *driver;
    uint64_t period;
    uint32_t status = SU_E_UNSUPPORTED_METHOD;
    int dev_frames = 0;
    msg[0] = '\0';
    *out = NULL;
    if (!load_sdl(msg)) {
        return SU_E_UNSUPPORTED_METHOD;
    }
    (void)g_sdl.SDL_ClearError();
    if (!g_sdl.SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        const char *err = g_sdl.SDL_GetError();
        /* SDL leaves the error unset when every driver failed to initialise without a reason of its own. */
        return fail(msg, "SDL could not start its audio subsystem: %s",
                    err != NULL && err[0] != '\0' ? err : "no audio driver is available");
    }
    d = calloc(1, sizeof *d);
    if (d == NULL) {
        g_sdl.SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return SU_E_RESOURCE_LIMIT;
    }
    d->mixer = m;
    d->pull = pull;
    d->pull_user = user;
    atomic_init(&d->frames, 0u);
    atomic_init(&d->callbacks, 0u);
    atomic_init(&d->callback_total_us, 0u);
    atomic_init(&d->callback_max_us, 0u);
    atomic_init(&d->callback_over_budget, 0u);
    atomic_init(&d->interval_max_us, 0u);
    atomic_init(&d->empty_buffers, 0u);
    atomic_init(&d->backend_errors, 0u);
    {
        uint32_t bin;
        for (bin = 0u; bin < SU_AUDIO_DEVICE_HISTOGRAM_BINS; bin++) {
            atomic_init(&d->callback_us[bin], 0u);
            atomic_init(&d->interval_us[bin], 0u);
        }
    }
    d->info.sample_rate = rate;
    /* The stream's input side is the mixer's format; SDL converts to whatever the device opened with. */
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = (int)rate;
    (void)g_sdl.SDL_ClearError();
    d->stream = g_sdl.SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, device_callback, d);
    if (d->stream == NULL) {
        const char *err = g_sdl.SDL_GetError();
        (void)fail(msg, "no audio output device could be opened: %s",
                   err != NULL && err[0] != '\0' ? err : "SDL found no default output device");
        goto fail;
    }
    dev = g_sdl.SDL_GetAudioStreamDevice(d->stream);
    if (dev == 0 || !g_sdl.SDL_GetAudioDeviceFormat(dev, &dev_spec, &dev_frames) || dev_frames <= 0 ||
        dev_spec.freq <= 0) {
        (void)fail(msg, "the audio device reported no usable format");
        goto fail;
    }
    /* One device period in the stream's input frames: the queue depth kept ahead of the device (see the callback). */
    period = ((uint64_t)dev_frames * rate + (uint64_t)dev_spec.freq / 2u) / (uint64_t)dev_spec.freq;
    if (period == 0u) {
        period = 1u;
    }
    if (period > SU_SDL3_MAX_PERIOD) {
        (void)fail(msg, "the audio device buffer is too large (%d frames)", dev_frames);
        goto fail;
    }
    d->period_frames = (uint32_t)period;
    d->scratch_frames = d->period_frames * 4u + SU_AUDIO_GAP_FADE;
    d->scratch = malloc((size_t)d->scratch_frames * 2u * sizeof(float));
    if (d->scratch == NULL) {
        status = SU_E_RESOURCE_LIMIT;
        (void)snprintf(msg, 256, "allocation");
        goto fail;
    }
    driver = g_sdl.SDL_GetCurrentAudioDriver();
    dev_name = g_sdl.SDL_GetAudioDeviceName(dev);
    if (dev_name != NULL && dev_name[0] != '\0') {
        if (driver != NULL && driver[0] != '\0') {
            (void)snprintf(d->info.name, sizeof d->info.name, "%s (%.64s)", dev_name, driver);
        } else {
            (void)snprintf(d->info.name, sizeof d->info.name, "%s", dev_name);
        }
    } else if (driver != NULL) {
        (void)snprintf(d->info.name, sizeof d->info.name, "default output (%.96s)", driver);
    }
    /* buffer_frames and latency_ms are in the stream's (the mixer's) frames, so buffer_frames / sample_rate is a
     * duration. The stream keeps one device period queued ahead of the device, which is part of the latency. */
    d->info.buffer_frames = d->period_frames;
    d->info.latency_ms = (double)(d->period_frames * 2u) * 1000.0 / (double)rate;
    if (!g_sdl.SDL_ResumeAudioStreamDevice(d->stream)) {
        const char *err = g_sdl.SDL_GetError();
        (void)fail(msg, "the audio device could not be started: %s", err != NULL ? err : "");
        goto fail;
    }
    if (info != NULL) {
        *info = d->info;
    }
    *out = d;
    return SU_OK;
fail:
    if (d->stream != NULL) {
        g_sdl.SDL_DestroyAudioStream(d->stream);
    }
    g_sdl.SDL_QuitSubSystem(SDL_INIT_AUDIO);
    free(d->scratch);
    free(d);
    return status;
}

void su_audio_device_close(SuAudioDevice *d) {
    if (d == NULL) {
        return;
    }
    if (d->stream != NULL) {
        (void)g_sdl.SDL_PauseAudioStreamDevice(d->stream);
        /* Waits for SDL's audio thread, so the callback has returned before the buffer and the device go. */
        g_sdl.SDL_DestroyAudioStream(d->stream);
    }
    if (g_sdl.lib != NULL) {
        g_sdl.SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    free(d->scratch);
    free(d);
}

void su_audio_device_stats(const SuAudioDevice *device, SuAudioDeviceStats *out) {
    uint32_t bin;
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (device == NULL) {
        return;
    }
    out->callbacks = atomic_load(&device->callbacks);
    out->callback_total_us = atomic_load(&device->callback_total_us);
    out->callback_max_us = atomic_load(&device->callback_max_us);
    out->callback_over_budget = atomic_load(&device->callback_over_budget);
    out->interval_max_us = atomic_load(&device->interval_max_us);
    out->empty_buffers = atomic_load(&device->empty_buffers);
    out->backend_errors = atomic_load(&device->backend_errors);
    out->stream_latency_ms = device->info.latency_ms;
    out->sample_rate = device->info.sample_rate;
    out->buffer_frames = device->info.buffer_frames;
    memcpy(out->name, device->info.name, sizeof out->name);
    for (bin = 0u; bin < SU_AUDIO_DEVICE_HISTOGRAM_BINS; bin++) {
        out->callback_us[bin] = atomic_load(&device->callback_us[bin]);
        out->interval_us[bin] = atomic_load(&device->interval_us[bin]);
    }
}

uint32_t su_audio_device_open(SuAudioMixer *m, SuAudioDevice **out, SuAudioDeviceInfo *info, char message[256]) {
    if (m == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    return device_start(m, NULL, NULL, su_audio_mixer_rate(m), out, info, message);
}

uint32_t su_audio_device_open_pull(uint32_t sample_rate, SuAudioPullFn pull, void *user, SuAudioDevice **out,
                                   SuAudioDeviceInfo *info, char message[256]) {
    if (pull == NULL || out == NULL || sample_rate < 1000u || sample_rate > 384000u) {
        return SU_E_INVALID_ARGUMENT;
    }
    return device_start(NULL, pull, user, sample_rate, out, info, message);
}

uint64_t su_audio_device_frames(const SuAudioDevice *d) {
    return d != NULL ? atomic_load(&((SuAudioDevice *)d)->frames) : 0u;
}

#endif /* !_WIN32 */
