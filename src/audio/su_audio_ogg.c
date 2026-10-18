/* Ogg Vorbis clips through the pinned stb_vorbis 1.22 (tools/bootstrap_asset_deps.py). Built only when it is
 * present; su_audio_noogg.c stands in otherwise. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "su_audio_int.h"

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

/* ---- streaming reader (pull API of the same pinned stb_vorbis) ---- */

typedef struct stb_vorbis stb_vorbis;
typedef struct SuVorbisInfo {
    unsigned int sample_rate;
    int channels;
    unsigned int setup_memory_required;
    unsigned int setup_temp_memory_required;
    unsigned int temp_memory_required;
    int max_frame_size;
} SuVorbisInfo;
stb_vorbis *stb_vorbis_open_memory(const unsigned char *data, int len, int *error, const void *alloc);
stb_vorbis *stb_vorbis_open_file(FILE *f, int close_handle_on_close, int *error, const void *alloc);
SuVorbisInfo stb_vorbis_get_info(stb_vorbis *f);
unsigned int stb_vorbis_stream_length_in_samples(stb_vorbis *f);
int stb_vorbis_seek(stb_vorbis *f, unsigned int sample_number);
int stb_vorbis_get_samples_short_interleaved(stb_vorbis *f, int channels, short *buffer, int num_shorts);
void stb_vorbis_close(stb_vorbis *f);

typedef struct OggR {
    stb_vorbis *v;
    uint32_t channels;
} OggR;

static int ogg_seek(void *ctx, uint64_t frame) {
    OggR *o = ctx;
    return frame <= 0xFFFFFFFFu && stb_vorbis_seek(o->v, (unsigned int)frame) != 0;
}

/* Same short-to-float conversion as su_audio_clip_from_ogg, so a streamed clip equals the decoded one bit for bit. */
static uint32_t ogg_read(void *ctx, float *out, uint32_t frames) {
    OggR *o = ctx;
    short tmp[1024 * 2];
    uint32_t done = 0;
    while (done < frames) {
        uint32_t want = frames - done < 1024u ? frames - done : 1024u;
        int n = stb_vorbis_get_samples_short_interleaved(o->v, (int)o->channels, tmp, (int)(want * o->channels));
        int i;
        if (n <= 0) {
            break;
        }
        for (i = 0; i < n * (int)o->channels; i++) {
            out[(size_t)done * o->channels + (size_t)i] = (float)tmp[i] / 32768.0f;
        }
        done += (uint32_t)n;
    }
    return done;
}

static void ogg_close(void *ctx) {
    OggR *o = ctx;
    stb_vorbis_close(o->v); /* closes the FILE it was given */
    free(o);
}

uint32_t su_reader_ogg(FILE *f, const uint8_t *mem, size_t len, SuReader *r, char msg[256]) {
    OggR *o = calloc(1, sizeof *o);
    int err = 0;
    SuVorbisInfo info;
    unsigned int total;
    if (o == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    if (f != NULL) {
        rewind(f); /* the format probe consumed the first bytes */
    }
    o->v = mem != NULL ? stb_vorbis_open_memory(mem, (int)len, &err, NULL) : stb_vorbis_open_file(f, 1, &err, NULL);
    if (o->v == NULL) {
        if (msg != NULL) {
            (void)snprintf(msg, 256, "not a decodable Ogg Vorbis stream (error %d)", err);
        }
        free(o);
        return SU_E_PARSE;
    }
    info = stb_vorbis_get_info(o->v);
    total = stb_vorbis_stream_length_in_samples(o->v);
    if (info.channels < 1 || info.channels > 2 || info.sample_rate < 1000u || info.sample_rate > 384000u || total == 0u) {
        stb_vorbis_close(o->v);
        free(o);
        if (msg != NULL) {
            (void)snprintf(msg, 256, "unsupported Ogg stream (%d channels, %u Hz, %u frames)", info.channels,
                           info.sample_rate, total);
        }
        return SU_E_PARSE;
    }
    o->channels = (uint32_t)info.channels;
    r->ctx = o;
    r->channels = o->channels;
    r->rate = info.sample_rate;
    r->frames = total;
    r->decoder_bytes = (uint64_t)info.setup_memory_required + info.temp_memory_required + 4096u + sizeof *o;
    r->seek = ogg_seek;
    r->read = ogg_read;
    r->close = ogg_close;
    return SU_OK;
}
