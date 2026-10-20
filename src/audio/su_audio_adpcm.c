/* PlayStation 4-bit ADPCM (PS-ADPCM / SPU ADPCM / VAG) decoding into clips (sutekh/audio.h). Portable; no
 * third-party code. Input is untrusted: every size is checked before it is used. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/audio.h"

#define FRAME_BYTES 16u
#define FRAME_SAMPLES 28u

static const int32_t k_coef[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

static void msgf(char *msg, const char *fmt, ...) {
    if (msg != NULL) {
        va_list ap;
        va_start(ap, fmt);
        (void)vsnprintf(msg, 256, fmt, ap);
        va_end(ap);
    }
}

/* Arithmetic (flooring) right shift, independent of how the compiler shifts negative values. */
static int32_t asr(int32_t x, uint32_t s) {
    if (x >= 0) {
        return x >> s;
    }
    return -(int32_t)((uint32_t)(-(x + 1)) >> s) - 1;
}

/* Decodes one 16-byte frame into 28 int16 samples, updating the two-sample history. */
static void decode_frame(const uint8_t *f, int32_t *hist1, int32_t *hist2, int16_t out[FRAME_SAMPLES]) {
    uint32_t filter = (uint32_t)f[0] >> 4;
    uint32_t shift = (uint32_t)f[0] & 15u;
    int32_t c1;
    int32_t c2;
    uint32_t i;
    if (filter > 4u) {
        filter = 0u;
    }
    if (shift > 12u) {
        shift = 9u;
    }
    c1 = k_coef[filter][0];
    c2 = k_coef[filter][1];
    for (i = 0; i < FRAME_SAMPLES; i++) {
        uint32_t byte = f[2u + i / 2u];
        int32_t nib = (int32_t)((i & 1u) ? (byte >> 4) : (byte & 15u));
        int32_t s;
        nib = (nib ^ 8) - 8; /* sign-extend the nibble */
        s = asr(nib * 4096, shift) + asr(*hist1 * c1 + *hist2 * c2, 6u);
        if (s > 32767) {
            s = 32767;
        } else if (s < -32768) {
            s = -32768;
        }
        *hist2 = *hist1;
        *hist1 = s;
        out[i] = (int16_t)s;
    }
}

uint32_t su_audio_clip_from_ps_adpcm(const void *bytes, size_t len, const SuAudioPsAdpcmDesc *d, SuAudioClip **out,
                                     uint32_t *loop_start_frame, uint32_t *loop_end_frame, char message[256]) {
    const uint8_t *b = bytes;
    size_t block;           /* bytes per channel block */
    size_t group;           /* bytes per group of all channels' blocks */
    size_t groups;
    size_t frames_per_block;
    size_t total_frames;    /* ADPCM frames per channel */
    size_t used_frames;
    size_t k;
    uint32_t ch;
    uint32_t channels;
    int found_start = 0;
    int found_end = 0;
    size_t loop_start = 0;
    size_t loop_end = 0;
    SuAudioClip *c;
    if (loop_start_frame != NULL) {
        *loop_start_frame = 0u;
    }
    if (loop_end_frame != NULL) {
        *loop_end_frame = 0u;
    }
    if (out == NULL || d == NULL || (bytes == NULL && len > 0u)) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    channels = d->channels;
    if (channels < 1u || channels > 2u) {
        msgf(message, "unsupported channel count %u", (unsigned)channels);
        return SU_E_INVALID_ARGUMENT;
    }
    if (d->sample_rate < 1000u || d->sample_rate > 384000u) {
        msgf(message, "unsupported sample rate %u", (unsigned)d->sample_rate);
        return SU_E_INVALID_ARGUMENT;
    }
    if (d->interleave % FRAME_BYTES != 0u || (d->interleave == 0u && channels > 1u)) {
        msgf(message, "interleave %u is not a non-zero multiple of 16", (unsigned)d->interleave);
        return SU_E_INVALID_ARGUMENT;
    }
    if (len == 0u) {
        msgf(message, "no data");
        return SU_E_PARSE;
    }
    if (d->interleave == 0u) {
        if (len % FRAME_BYTES != 0u) {
            msgf(message, "data length %llu is not a multiple of 16", (unsigned long long)len);
            return SU_E_PARSE;
        }
        block = len;
    } else {
        block = d->interleave;
    }
    group = block * channels;
    if (len % group != 0u) {
        msgf(message, "data length %llu is not a whole number of %llu-byte channel groups", (unsigned long long)len,
             (unsigned long long)group);
        return SU_E_PARSE;
    }
    groups = len / group;
    frames_per_block = block / FRAME_BYTES;
    total_frames = groups * frames_per_block;

/* Channel ch's ADPCM frame k. */
#define FRAME_AT(ch_, k_) (b + ((k_) / frames_per_block) * group + (size_t)(ch_) * block + \
                           ((k_) % frames_per_block) * FRAME_BYTES)

    used_frames = total_frames;
    for (k = 0; k < total_frames; k++) {
        uint32_t flags = FRAME_AT(0u, k)[1];
        if (!found_start && (flags & 4u) != 0u) {
            found_start = 1;
            loop_start = k;
        }
        if (found_start && !found_end && (flags & 3u) == 3u) {
            found_end = 1;
            loop_end = k + 1u;
        }
        if (d->stop_at_end_flag && (flags & 1u) != 0u) {
            used_frames = k + 1u;
            break;
        }
    }
    if (found_start && !found_end) {
        loop_end = used_frames;
    }
    if (used_frames > (size_t)(UINT32_MAX / FRAME_SAMPLES / 2u)) {
        msgf(message, "too many samples");
        return SU_E_PARSE;
    }
    c = calloc(1, sizeof *c);
    if (c == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    c->samples = calloc(used_frames * FRAME_SAMPLES * channels + 1u, sizeof(float));
    if (c->samples == NULL) {
        free(c);
        return SU_E_RESOURCE_LIMIT;
    }
    c->frames = (uint32_t)(used_frames * FRAME_SAMPLES);
    c->channels = channels;
    c->sample_rate = d->sample_rate;
    for (ch = 0; ch < channels; ch++) {
        int32_t h1 = 0;
        int32_t h2 = 0;
        for (k = 0; k < used_frames; k++) {
            int16_t pcm[FRAME_SAMPLES];
            uint32_t i;
            decode_frame(FRAME_AT(ch, k), &h1, &h2, pcm);
            for (i = 0; i < FRAME_SAMPLES; i++) {
                c->samples[(k * FRAME_SAMPLES + i) * channels + ch] = (float)pcm[i] / 32768.0f;
            }
        }
    }
#undef FRAME_AT
    if (found_start && loop_start < used_frames) {
        if (loop_end > used_frames) {
            loop_end = used_frames;
        }
        if (loop_start_frame != NULL) {
            *loop_start_frame = (uint32_t)(loop_start * FRAME_SAMPLES);
        }
        if (loop_end_frame != NULL) {
            *loop_end_frame = (uint32_t)(loop_end * FRAME_SAMPLES);
        }
    }
    *out = c;
    return SU_OK;
}

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

uint32_t su_audio_clip_from_vag(const void *bytes, size_t len, SuAudioClip **out, uint32_t *loop_start_frame,
                                uint32_t *loop_end_frame, char message[256]) {
    const uint8_t *b = bytes;
    uint32_t size;
    uint32_t rate;
    SuAudioPsAdpcmDesc d;
    if (loop_start_frame != NULL) {
        *loop_start_frame = 0u;
    }
    if (loop_end_frame != NULL) {
        *loop_end_frame = 0u;
    }
    if (out == NULL || (bytes == NULL && len > 0u)) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (len < 0x30u || memcmp(b, "VAGp", 4) != 0) {
        msgf(message, "not a VAGp file");
        return SU_E_PARSE;
    }
    size = rd32be(b + 0x0C);
    rate = rd32be(b + 0x10);
    if ((size_t)size > len - 0x30u) {
        msgf(message, "data size %u runs past the end (%llu bytes after the header)", (unsigned)size,
             (unsigned long long)(len - 0x30u));
        return SU_E_PARSE;
    }
    size -= size % FRAME_BYTES; /* a trailing partial frame is ignored */
    if (size == 0u) {
        msgf(message, "no sample data");
        return SU_E_PARSE;
    }
    if (rate < 1000u || rate > 384000u) {
        msgf(message, "unsupported sample rate %u", (unsigned)rate);
        return SU_E_PARSE;
    }
    memset(&d, 0, sizeof d);
    d.sample_rate = rate;
    d.channels = 1u;
    d.interleave = 0u;
    d.stop_at_end_flag = 1;
    return su_audio_clip_from_ps_adpcm(b + 0x30, size, &d, out, loop_start_frame, loop_end_frame, message);
}
