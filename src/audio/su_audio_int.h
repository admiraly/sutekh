/* Internal interfaces between the audio translation units (not installed). */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef SUTEKH_AUDIO_INT_H
#define SUTEKH_AUDIO_INT_H

#include <stdint.h>
#include <stdio.h>

#include "sutekh/audio.h"

/* A sequential decoder with seek. Frames are interleaved float; mono stays one channel. */
typedef struct SuReader {
    void *ctx;
    uint32_t channels;
    uint32_t rate;
    uint64_t frames;
    uint64_t decoder_bytes;                       /* resident memory of the decoder itself */
    int (*seek)(void *ctx, uint64_t frame);       /* 1 on success */
    uint32_t (*read)(void *ctx, float *out, uint32_t frames); /* frames produced; fewer than asked only at the end */
    void (*close)(void *ctx);
} SuReader;

/* Exactly one of f (a FILE the reader takes ownership of on success) and mem (borrowed, must outlive the reader). */
uint32_t su_reader_wav(FILE *f, const uint8_t *mem, size_t len, SuReader *r, char msg[256]);
uint32_t su_reader_ogg(FILE *f, const uint8_t *mem, size_t len, SuReader *r, char msg[256]);

/* One PCM sample of a WAVE data chunk (format 1 integer PCM or 3 float); shared with the whole-clip decoder so
 * both paths produce identical floats. */
float su_wav_sample(const uint8_t *p, uint32_t format, uint32_t bits);

/* Render-thread side of a stream. */
int su_stream_claim(SuAudioStream *s);       /* game thread: 1 if the stream was free */
void su_stream_release(SuAudioStream *s);
uint64_t su_stream_frames(const SuAudioStream *s);
uint32_t su_stream_channels(const SuAudioStream *s);
uint32_t su_stream_rate(const SuAudioStream *s);
/* Caches the first frames of a loop so the wrap needs no seek for the interpolation partner. */
void su_stream_prepare_loop(SuAudioStream *s, uint64_t loop_start);
/* Frame idx (l = r for mono). Frames at or past the end read as silence. */
void su_stream_frame(SuAudioStream *s, uint64_t idx, float *l, float *r);

#endif
