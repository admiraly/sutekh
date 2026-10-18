/* Builds without the pinned stb_vorbis: the Ogg entry point exists and says so. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdio.h>

#include "sutekh/audio.h"

int su_audio_ogg_available(void) {
    return 0;
}

uint32_t su_audio_clip_from_ogg(const void *bytes, size_t len, SuAudioClip **out, char message[256]) {
    (void)bytes;
    (void)len;
    if (out != NULL) {
        *out = NULL;
    }
    if (message != NULL) {
        (void)snprintf(message, 256, "built without stb_vorbis (run tools/bootstrap_asset_deps.py)");
    }
    return SU_E_UNSUPPORTED_METHOD;
}
