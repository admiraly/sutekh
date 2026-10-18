/* Builds without the pinned stb_vorbis: the Ogg entry point exists and says so. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdio.h>

#include "su_audio_int.h"

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

uint32_t su_reader_ogg(FILE *f, const uint8_t *mem, size_t len, SuReader *r, char msg[256]) {
    (void)f;
    (void)mem;
    (void)len;
    (void)r;
    if (msg != NULL) {
        (void)snprintf(msg, 256, "built without stb_vorbis (run tools/bootstrap_asset_deps.py)");
    }
    return SU_E_UNSUPPORTED_METHOD;
}
