/* Platforms without an output backend yet (everything but Windows): the mixer works offline; opening a device
 * reports that no backend exists. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdio.h>
#include <string.h>

#include "sutekh/audio.h"
#include "su_audio_int.h"

uint32_t su_audio_device_open(SuAudioMixer *m, SuAudioDevice **out, SuAudioDeviceInfo *info, char message[256]) {
    (void)m;
    (void)info;
    if (out != NULL) {
        *out = NULL;
    }
    if (message != NULL) {
        (void)snprintf(message, 256, "no audio output backend on this platform yet (WASAPI is Windows only)");
    }
    return SU_E_UNSUPPORTED_METHOD;
}

void su_audio_device_close(SuAudioDevice *d) {
    (void)d;
}

uint32_t su_audio_device_open_pull(uint32_t sample_rate, SuAudioPullFn pull, void *user, SuAudioDevice **out,
                                   SuAudioDeviceInfo *info, char message[256]) {
    (void)sample_rate;
    (void)pull;
    (void)user;
    return su_audio_device_open(NULL, out, info, message);
}

uint64_t su_audio_device_frames(const SuAudioDevice *d) {
    (void)d;
    return 0;
}

void su_audio_device_stats(const SuAudioDevice *device, SuAudioDeviceStats *out) {
    (void)device;
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
}
