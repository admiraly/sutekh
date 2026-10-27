/* Output-device backend selection (sutekh/audio.h): a missing SDL3 library, an audio subsystem that cannot start,
 * and a build with no audio backend at all each give the defined SU_E_UNSUPPORTED_METHOD with a message, a null
 * device and zeroed stats, and nothing crashes. Where a default device does exist the same call opens it (and the
 * run says so); the open/render/close checks live in audio_device_sdl3.
 *
 * --no-driver: run with SDL_AUDIO_DRIVER naming a driver that does not exist, which must be refused. SDL reads the
 * environment once, on its first call, so this case needs its own process (the audio_device_backend_no_driver
 * registration sets the variable); everything else here would then see the same driver.
 *
 * Windows is not covered here: the WASAPI backend is the existing audio_device test. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L /* setenv, unsetenv */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/audio.h"

static int fails;

static void check(int condition, const char *what) {
    if (!condition) {
        fails++;
        printf("FAIL: %s\n", what);
    }
}

/* Opens once and reports; a refusal must be SU_E_UNSUPPORTED_METHOD with a message and no device. */
static void expect_open(const char *what) {
    SuAudioMixerDesc desc;
    SuAudioMixer *mixer = NULL;
    SuAudioDevice *device = NULL;
    SuAudioDeviceInfo info;
    SuAudioDeviceStats stats;
    char message[256];
    uint32_t status;
    memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    memset(&info, 0, sizeof info);
    memset(&stats, 0xff, sizeof stats);
    message[0] = '\0';
    check(su_audio_mixer_create(&desc, &mixer) == SU_OK && mixer != NULL, "mixer");
    status = su_audio_device_open(mixer, &device, &info, message);
    printf("%s: status %u, message \"%s\"\n", what, (unsigned)status, message);
    if (status == SU_OK) {
        check(device != NULL, "an open device was returned");
        check(info.sample_rate == su_audio_mixer_rate(mixer), "the device runs at the mixer's rate");
        su_audio_device_close(device);
    } else {
        check(status == SU_E_UNSUPPORTED_METHOD, "the refusal is SU_E_UNSUPPORTED_METHOD");
        check(device == NULL, "the refusal leaves no device");
        check(message[0] != '\0', "the refusal has a message");
    }
    /* A null device: zeroed stats, no frames, close is a no-op. */
    su_audio_device_stats(NULL, &stats);
    check(stats.callbacks == 0u && stats.sample_rate == 0u && stats.name[0] == '\0', "stats of no device are zeroed");
    check(su_audio_device_frames(NULL) == 0u, "no frames without a device");
    su_audio_device_close(NULL);
    su_audio_mixer_destroy(mixer);
}

int main(int argc, char **argv) {
    int no_driver = argc > 1 && strcmp(argv[1], "--no-driver") == 0;
#ifdef SU_TEST_SDL3_AUDIO
    SuAudioMixerDesc desc;
    SuAudioMixer *mixer = NULL;
    SuAudioDevice *device = NULL;
    SuAudioDeviceInfo info;
    char message[256];
    memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    check(su_audio_mixer_create(&desc, &mixer) == SU_OK && mixer != NULL, "mixer");
    /* Argument validation before any SDL3 call (the nodevice stub refuses every call alike). */
    message[0] = '\0';
    check(su_audio_device_open(NULL, &device, &info, message) == SU_E_INVALID_ARGUMENT, "null mixer is refused");
    check(su_audio_device_open(mixer, NULL, &info, message) == SU_E_INVALID_ARGUMENT, "null out is refused");
    su_audio_mixer_destroy(mixer);
#endif
    if (no_driver) {
#ifdef SU_TEST_SDL3_AUDIO
        expect_open("no audio driver");
#else
        printf("--no-driver needs the SDL3 audio backend\n");
        return 1;
#endif
        printf("test_audio_device_backend: %s\n", fails == 0 ? "ok" : "FAILED");
        return fails == 0 ? 0 : 1;
    }
    /* The SDL3 library is not loadable (or this build has no audio backend at all). */
    if (setenv("SUTEKH_SDL3_LIBRARY", "/nonexistent/libsutekh-test-sdl3.so", 1) != 0) {
        printf("FAIL: setenv\n");
        return 1;
    }
    expect_open("no SDL3 library");
    (void)unsetenv("SUTEKH_SDL3_LIBRARY");
#ifdef SU_TEST_SDL3_AUDIO
    /* The default device, or the refusal when this machine has none (headless CI). */
    expect_open("default device");
#else
    printf("built without the SDL3 audio backend (no SDL3 headers at configure time): nodevice backend only\n");
#endif
    printf("test_audio_device_backend: %s\n", fails == 0 ? "ok" : "FAILED");
    return fails == 0 ? 0 : 1;
}
