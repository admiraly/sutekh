/* Output-device test on SDL3's dummy audio driver (sutekh/audio.h): open the default output with the driver forced
 * to "dummy" (no hardware, real-time paced by SDL), play a looping tone through the mixer and a ramp through the
 * PULL path the integrated runtime uses, and check that su_audio_device_frames advances and that the stats add up:
 * one callback per period, histogram bins summing to the callback count, a maximum no larger than the total, no
 * backend errors, and the info and stats agreeing. Then close both devices cleanly.
 *
 * Built twice: plain, and with the backend compiled against counting allocators (SU_AUDIO_SDL3_ALLOC_TEST), which
 * also checks that the callback allocates nothing in steady state (the open path is the positive control).
 * Exit 77 when the SDL3 library or its dummy driver is not available here. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L /* setenv */
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/su_platform.h"
#include "sutekh/audio.h"
#include "audio/su_audio_int.h"

#ifdef SU_AUDIO_SDL3_ALLOC_TEST
#include <stdatomic.h>
/* The counting allocators the backend object is compiled against (see the audio_device_sdl3_alloc target). */
static _Atomic unsigned long long alloc_calls;

void *sdl_test_malloc(size_t bytes) {
    atomic_fetch_add(&alloc_calls, 1u);
    return malloc(bytes);
}

void *sdl_test_calloc(size_t count, size_t bytes) {
    atomic_fetch_add(&alloc_calls, 1u);
    return calloc(count, bytes);
}

void *sdl_test_realloc(void *memory, size_t bytes) {
    atomic_fetch_add(&alloc_calls, 1u);
    return realloc(memory, bytes);
}

void sdl_test_free(void *memory) {
    free(memory);
}
#endif

static int fails;
static uint32_t pull_frames;

static void check(int condition, const char *what) {
    if (!condition) {
        fails++;
        printf("FAIL: %s\n", what);
    }
}

static uint64_t now_us(void) {
    uint64_t ns = 0u;
    (void)su_platform_monotonic_ns(&ns);
    return ns / 1000u;
}

/* Waits until the device thread has rendered at least frames, or seconds pass; returns the frames seen. */
static uint64_t wait_frames(const SuAudioDevice *device, uint64_t frames, double seconds) {
    uint64_t begin = now_us();
    uint64_t rendered = su_audio_device_frames(device);
    while (rendered < frames && (double)(now_us() - begin) < seconds * 1e6) {
        su_platform_sleep_ms(1u);
        rendered = su_audio_device_frames(device);
    }
    return rendered;
}

static void check_stats(const SuAudioDevice *device, const SuAudioDeviceInfo *info) {
    SuAudioDeviceStats stats;
    uint64_t frames;
    uint64_t callbacks_in_bins = 0u;
    uint64_t intervals_in_bins = 0u;
    uint32_t bin;
    memset(&stats, 0, sizeof stats);
    su_audio_device_stats(device, &stats);
    frames = su_audio_device_frames(device); /* after the counters: the frame count only grows */
    for (bin = 0u; bin < SU_AUDIO_DEVICE_HISTOGRAM_BINS; bin++) {
        callbacks_in_bins += stats.callback_us[bin];
        intervals_in_bins += stats.interval_us[bin];
    }
    printf("stats: %llu callbacks, %llu frames, total %llu us, max %llu us, over budget %llu, interval max %llu us, "
           "empty %llu, timeouts %llu, errors %llu\n",
           (unsigned long long)stats.callbacks, (unsigned long long)frames,
           (unsigned long long)stats.callback_total_us, (unsigned long long)stats.callback_max_us,
           (unsigned long long)stats.callback_over_budget, (unsigned long long)stats.interval_max_us,
           (unsigned long long)stats.empty_buffers, (unsigned long long)stats.wait_timeouts,
           (unsigned long long)stats.backend_errors);
    /* The device keeps rendering while this runs, and a stats snapshot is not a transaction (the counters are read
     * one by one), so a counter read after another may be one callback ahead. */
    check(stats.callbacks > 0u, "callbacks were counted");
    check(frames > 0u, "the device thread rendered frames");
    check(frames >= stats.callbacks * info->buffer_frames &&
          frames <= (stats.callbacks + 2u) * info->buffer_frames, "one to three periods per callback");
    check(callbacks_in_bins >= stats.callbacks && callbacks_in_bins <= stats.callbacks + 1u,
          "the callback histogram has one sample per callback");
    check(intervals_in_bins <= stats.callbacks + 1u, "interval samples are at most one per callback");
    check(stats.callback_total_us <= stats.callback_max_us * (stats.callbacks + 1u),
          "no callback exceeds the maximum");
    check(stats.callback_over_budget <= stats.callbacks + 1u, "over-budget callbacks are a subset");
    check(stats.empty_buffers <= stats.callbacks + 1u, "starvations are a subset of the callbacks");
    check(stats.backend_errors == 0u, "no backend errors");
    check(stats.wait_timeouts == 0u, "the SDL3 backend has no event wait to time out");
    check(stats.sample_rate == info->sample_rate, "stats and info agree on the rate");
    check(stats.buffer_frames == info->buffer_frames, "stats and info agree on the buffer");
    check(stats.stream_latency_ms == info->latency_ms, "stats and info agree on the latency");
    check(memcmp(stats.name, info->name, sizeof stats.name) == 0, "stats and info agree on the name");
}

static void fill_pull(void *user, float *out, uint32_t frames) {
    uint32_t frame;
    (void)user;
    for (frame = 0u; frame < frames; frame++) {
        out[(size_t)frame * 2u] = 0.01f;
        out[(size_t)frame * 2u + 1u] = -0.01f;
    }
    pull_frames += frames;
}

int main(void) {
    SuAudioMixerDesc desc;
    SuAudioMixer *mixer = NULL;
    SuAudioDevice *device = NULL;
    SuAudioDeviceInfo info;
    SuAudioClip *tone = NULL;
    SuAudioVoiceDesc voice_desc;
    SuAudioVoice voice = 0;
    SuAudioStats mixer_stats;
    char message[256];
    uint32_t rate;
    uint64_t first;
    uint64_t second;
    memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    if (setenv("SDL_AUDIO_DRIVER", "dummy", 1) != 0) {
        printf("FAIL: setenv\n");
        return 1;
    }
    if (su_audio_mixer_create(&desc, &mixer) != SU_OK || mixer == NULL) {
        printf("FAIL: mixer\n");
        return 1;
    }
    rate = su_audio_mixer_rate(mixer);
    message[0] = '\0';
    if (su_audio_device_open(mixer, &device, &info, message) != SU_OK) {
        printf("audio device unavailable: %s\n", message);
        su_audio_mixer_destroy(mixer);
        return 77;
    }
    printf("device: \"%s\", %u Hz, buffer %u frames, stream latency %.2f ms\n", info.name, (unsigned)info.sample_rate,
           (unsigned)info.buffer_frames, info.latency_ms);
    check(info.name[0] != '\0', "the device has a name");
    check(strstr(info.name, "dummy") != NULL, "the name reports SDL's dummy driver");
    check(info.sample_rate == rate, "the stream runs at the mixer's rate");
    check(info.buffer_frames > 0u, "the device reports a buffer");
    check(info.latency_ms > 0.0, "the device reports a latency");
#ifdef SU_AUDIO_SDL3_ALLOC_TEST
    /* Positive control: opening allocated (the device record and the render buffer). */
    check(atomic_load(&alloc_calls) > 0u, "opening allocates, so the counting allocators are wired in");
#endif

    /* A looping tone through the mixer: the device thread is the only renderer. */
    if (su_audio_clip_tone(SU_AUDIO_WAVE_SINE, 440.0f, 0.5f, rate, 1, &tone) != SU_OK || tone == NULL) {
        printf("FAIL: tone\n");
        su_audio_device_close(device);
        su_audio_mixer_destroy(mixer);
        return 1;
    }
    memset(&voice_desc, 0, sizeof voice_desc);
    voice_desc.clip = tone;
    voice_desc.gain = 0.05f;
    voice_desc.flags = SU_AUDIO_LOOP;
    check(su_audio_play(mixer, &voice_desc, &voice) == SU_OK, "the tone plays");
    first = wait_frames(device, rate / 10u, 2.0);
    check(first >= rate / 10u, "the device thread rendered 100 ms of audio");
#ifdef SU_AUDIO_SDL3_ALLOC_TEST
    /* Steady state: from here the callbacks must not allocate at all. */
    atomic_store(&alloc_calls, 0u);
    (void)wait_frames(device, first + rate / 10u, 2.0);
    check(atomic_load(&alloc_calls) == 0u, "the render callback allocates nothing");
#endif
    second = wait_frames(device, first + 1u, 1.0);
    check(second > first, "su_audio_device_frames advances");
    check_stats(device, &info);
    su_audio_mixer_stats(mixer, &mixer_stats);
    check(mixer_stats.frames_rendered >= second, "the mixer rendered at least what the device took");
    check(mixer_stats.peak > 0.0f, "the tone reached the device");
    su_audio_device_close(device);
    device = NULL;
    su_audio_mixer_destroy(mixer);
    mixer = NULL;
    su_audio_clip_free(tone);

    /* PULL mode, as the integrated runtime opens it: the device thread pulls from the caller instead. */
    message[0] = '\0';
    if (su_audio_device_open_pull(rate, fill_pull, NULL, &device, &info, message) != SU_OK) {
        printf("FAIL: pull device unavailable: %s\n", message);
        return 1;
    }
    first = wait_frames(device, rate / 10u, 2.0);
    check(first >= rate / 10u, "the PULL device thread rendered 100 ms of audio");
    check(pull_frames >= first, "every rendered frame came from the pull callback");
    check_stats(device, &info);
    su_audio_device_close(device);

    printf("test_audio_device_sdl3: %s\n", fails == 0 ? "ok" : "FAILED");
    return fails == 0 ? 0 : 1;
}
