/* Ogg Vorbis clips through the pinned stb_vorbis 1.22 (tools/bootstrap_asset_deps.py). Built only when it is
 * present; su_audio_noogg.c stands in otherwise. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "sutekh/audio.h"

/* From stb_vorbis.c (compiled in su_audio_thirdparty.c). */
int stb_vorbis_decode_memory(const unsigned char *mem, int len, int *channels, int *sample_rate, short **output);

int su_audio_ogg_available(void) {
    return 1;
}

uint32_t su_audio_clip_from_ogg(const void *bytes, size_t len, SuAudioClip **out, char message[256]) {
    int channels = 0;
    int rate = 0;
    short *pcm = NULL;
    int frames;
    SuAudioClip *c;
    int i;
    if (out == NULL || (bytes == NULL && len > 0u) || len > 0x7FFFFFFFu) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    frames = len >= 4u ? stb_vorbis_decode_memory(bytes, (int)len, &channels, &rate, &pcm) : -1;
    if (frames <= 0 || pcm == NULL || channels < 1 || channels > 2 || rate < 1000 || rate > 384000) {
        free(pcm);
        if (message != NULL) {
            (void)snprintf(message, 256, "not a decodable mono or stereo Ogg Vorbis stream");
        }
        return SU_E_PARSE;
    }
    c = calloc(1, sizeof *c);
    if (c != NULL) {
        c->samples = malloc((size_t)frames * (size_t)channels * sizeof(float));
    }
    if (c == NULL || c->samples == NULL) {
        free(c);
        free(pcm);
        return SU_E_RESOURCE_LIMIT;
    }
    for (i = 0; i < frames * channels; i++) {
        c->samples[i] = (float)pcm[i] / 32768.0f;
    }
    c->frames = (uint32_t)frames;
    c->channels = (uint32_t)channels;
    c->sample_rate = (uint32_t)rate;
    free(pcm);
    *out = c;
    return SU_OK;
}
