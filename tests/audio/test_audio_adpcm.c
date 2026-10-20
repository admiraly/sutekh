/* PS-ADPCM (SPU ADPCM / VAG) decoding (sutekh/audio.h): hand-computed bit-exact frames (prediction filters,
 * flooring shifts, clamping, out-of-range filter and shift), round trips of synthesized signals through a reference
 * encoder in this file (brute-force filter and shift search per 28-sample frame), stereo interleave, loop and end
 * flags, the VAGp header, and refusals of malformed input. No external data. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/audio.h"

static long g_checks = 0;
static long g_fail = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        g_checks++;                                                                  \
        if (!(cond)) {                                                               \
            g_fail++;                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

#define FS 28u

static int32_t pcm_at(const SuAudioClip *c, uint32_t i) {
    return (int32_t)lround((double)c->samples[i] * 32768.0);
}

/* ---------- reference encoder ---------- */

static const int32_t k_coef[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

static int32_t floor_shift(int32_t x, int s) {
    return (int32_t)floor((double)x / (double)(1 << s));
}

static int32_t clamp16(int32_t s) {
    return s > 32767 ? 32767 : (s < -32768 ? -32768 : s);
}

/* Encodes 28 samples into one frame, choosing the filter and shift with the least squared error of the decoded
 * result (simulated exactly as the decoder runs it), and advances the history. */
static void encode_frame(const int16_t *x, uint8_t flags, int32_t *h1, int32_t *h2, uint8_t f[16]) {
    double best = -1.0;
    int bf = 0;
    int bs = 0;
    uint8_t bn[FS];
    int32_t bh1 = 0;
    int32_t bh2 = 0;
    int filt;
    int sh;
    memset(bn, 0, sizeof bn);
    for (filt = 0; filt < 5; filt++) {
        for (sh = 0; sh <= 12; sh++) {
            int32_t a = *h1;
            int32_t b = *h2;
            double err = 0.0;
            uint8_t nib[FS];
            uint32_t i;
            for (i = 0; i < FS; i++) {
                int32_t pred = floor_shift(a * k_coef[filt][0] + b * k_coef[filt][1], 6);
                int32_t step = 1 << (12 - sh);
                double q = floor((double)(x[i] - pred) / (double)step + 0.5);
                int32_t n = q > 7.0 ? 7 : (q < -8.0 ? -8 : (int32_t)q);
                int32_t s = clamp16(floor_shift(n * 4096, sh) + pred);
                err += (double)(s - x[i]) * (double)(s - x[i]);
                nib[i] = (uint8_t)(n & 15);
                b = a;
                a = s;
            }
            if (best < 0.0 || err < best) {
                best = err;
                bf = filt;
                bs = sh;
                memcpy(bn, nib, sizeof bn);
                bh1 = a;
                bh2 = b;
            }
        }
    }
    {
        uint32_t i;
        f[0] = (uint8_t)((bf << 4) | bs);
        f[1] = flags;
        for (i = 0; i < 14u; i++) {
            f[2u + i] = (uint8_t)(bn[2u * i] | (bn[2u * i + 1u] << 4));
        }
    }
    *h1 = bh1;
    *h2 = bh2;
}

/* Encodes n frames of x (n * 28 samples) into dst, one 16-byte frame after another. */
static void encode(const int16_t *x, uint32_t n, uint8_t *dst) {
    int32_t h1 = 0;
    int32_t h2 = 0;
    uint32_t k;
    for (k = 0; k < n; k++) {
        encode_frame(x + k * FS, 0, &h1, &h2, dst + k * 16u);
    }
}

static void make_signal(int16_t *x, uint32_t n, double f1, double f2, double rate) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        double t = (double)i / rate;
        double v = 0.55 * sin(2.0 * 3.141592653589793 * f1 * t) + 0.3 * sin(2.0 * 3.141592653589793 * f2 * t);
        x[i] = (int16_t)lround(v * 32767.0);
    }
}

/* RMS error of a decoded channel against the source, as a fraction of full scale. */
static double rms_err(const SuAudioClip *c, uint32_t ch, const int16_t *x, uint32_t n) {
    double acc = 0.0;
    uint32_t i;
    for (i = 0; i < n; i++) {
        double e = (double)c->samples[i * c->channels + ch] - (double)x[i] / 32768.0;
        acc += e * e;
    }
    return sqrt(acc / (double)n);
}

/* ---------- tests ---------- */

static SuAudioPsAdpcmDesc mono(uint32_t rate) {
    SuAudioPsAdpcmDesc d;
    memset(&d, 0, sizeof d);
    d.sample_rate = rate;
    d.channels = 1u;
    return d;
}

static void test_bit_exact(void) {
    uint8_t f[48];
    SuAudioPsAdpcmDesc d = mono(22050u);
    SuAudioClip *c = NULL;
    char msg[256];
    uint32_t ls = 99u;
    uint32_t le = 99u;
    uint32_t i;
    /* frame 0: filter 0, shift 0; nibbles 0..3 = 1, 7, -1, -8; nibbles 26, 27 = 7, 3 */
    memset(f, 0, sizeof f);
    f[2] = 0x71;
    f[3] = 0x8F;
    f[15] = 0x37;
    /* frame 1: filter 2 (115, -52), shift 2; nibble 0 = -1, the rest 0 */
    f[16] = 0x22;
    f[18] = 0x0F;
    /* frame 2: filter 5 (reads as 0), shift 13 (reads as 9); nibble 0 = 1 */
    f[32] = 0x5D;
    f[34] = 0x01;
    CHECK(su_audio_clip_from_ps_adpcm(f, sizeof f, &d, &c, &ls, &le, msg) == SU_OK);
    if (c == NULL) {
        return;
    }
    CHECK(c->frames == 84u && c->channels == 1u && c->sample_rate == 22050u);
    CHECK(ls == 0u && le == 0u);
    CHECK(pcm_at(c, 0) == 4096);
    CHECK(pcm_at(c, 1) == 28672);
    CHECK(pcm_at(c, 2) == -4096);
    CHECK(pcm_at(c, 3) == -32768);
    for (i = 4; i < 26u; i++) {
        CHECK(pcm_at(c, i) == 0);
    }
    CHECK(pcm_at(c, 26) == 28672);
    CHECK(pcm_at(c, 27) == 12288);
    /* -4096 >> 2 + (12288 * 115 + 28672 * -52) >> 6 = -1024 + -1216 */
    CHECK(pcm_at(c, 28) == -2240);
    /* (-2240 * 115 + 12288 * -52) >> 6 = -896576 / 64 */
    CHECK(pcm_at(c, 29) == -14009);
    /* (-14009 * 115 + -2240 * -52) >> 6 = floor(-1494555 / 64) = -23353 (flooring, not truncation) */
    CHECK(pcm_at(c, 30) == -23353);
    /* frame 2 predicts nothing and shifts by 9: 4096 >> 9 = 8 */
    CHECK(pcm_at(c, 56) == 8);
    CHECK(pcm_at(c, 57) == 0);
    su_audio_clip_free(c);

    /* clamping: history 28672, 28672; filter 1 (60, 0), shift 0, nibble 7: 28672 + 26880 clamps to 32767 */
    memset(f, 0, sizeof f);
    f[15] = 0x77;
    f[16] = 0x10;
    f[18] = 0x07;
    f[19] = 0x08;
    c = NULL;
    CHECK(su_audio_clip_from_ps_adpcm(f, 32u, &d, &c, NULL, NULL, msg) == SU_OK);
    if (c != NULL) {
        CHECK(c->frames == 56u);
        CHECK(pcm_at(c, 28) == 32767);
        CHECK(pcm_at(c, 29) == 30719);         /* (32767 * 60) >> 6 */
        CHECK(pcm_at(c, 30) == -32768 + 28799); /* nibble -8 plus (30719 * 60) >> 6 */
        su_audio_clip_free(c);
    }
    /* negative clamp: history -32768 (nibble -8, shift 0), then filter 4 with nibble -8 */
    memset(f, 0, sizeof f);
    f[15] = 0x88;
    f[16] = 0x40;
    f[18] = 0x08;
    c = NULL;
    CHECK(su_audio_clip_from_ps_adpcm(f, 32u, &d, &c, NULL, NULL, msg) == SU_OK);
    if (c != NULL) {
        CHECK(pcm_at(c, 27) == -32768);
        CHECK(pcm_at(c, 28) == -32768); /* -32768 + (-32768 * 122 + -32768 * -60) >> 6 = -32768 - 31744 */
        su_audio_clip_free(c);
    }
}

static void test_round_trip_mono(void) {
    enum { FRAMES = 200 };
    static int16_t x[FRAMES * FS];
    static uint8_t enc[FRAMES * 16];
    SuAudioPsAdpcmDesc d = mono(44100u);
    SuAudioClip *c = NULL;
    char msg[256];
    double e;
    make_signal(x, FRAMES * FS, 440.0, 1250.0, 44100.0);
    encode(x, FRAMES, enc);
    CHECK(su_audio_clip_from_ps_adpcm(enc, sizeof enc, &d, &c, NULL, NULL, msg) == SU_OK);
    if (c == NULL) {
        return;
    }
    CHECK(c->frames == FRAMES * FS && c->channels == 1u);
    e = rms_err(c, 0, x, FRAMES * FS);
    printf("ps-adpcm mono round trip: rms error %.6f of full scale\n", e);
    CHECK(e < 0.005);
    su_audio_clip_free(c);
}

static void test_round_trip_stereo(void) {
    enum { FRAMES = 64, IL = 64 }; /* 4 frames per channel block */
    static int16_t l[FRAMES * FS];
    static int16_t r[FRAMES * FS];
    static uint8_t el[FRAMES * 16];
    static uint8_t er[FRAMES * 16];
    static uint8_t data[FRAMES * 16 * 2];
    SuAudioPsAdpcmDesc d;
    SuAudioClip *c = NULL;
    char msg[256];
    uint32_t g;
    make_signal(l, FRAMES * FS, 300.0, 2000.0, 32000.0);
    make_signal(r, FRAMES * FS, 700.0, 90.0, 32000.0);
    encode(l, FRAMES, el);
    encode(r, FRAMES, er);
    for (g = 0; g < FRAMES * 16 / IL; g++) {
        memcpy(data + g * IL * 2, el + g * IL, IL);
        memcpy(data + g * IL * 2 + IL, er + g * IL, IL);
    }
    memset(&d, 0, sizeof d);
    d.sample_rate = 32000u;
    d.channels = 2u;
    d.interleave = IL;
    CHECK(su_audio_clip_from_ps_adpcm(data, sizeof data, &d, &c, NULL, NULL, msg) == SU_OK);
    if (c == NULL) {
        return;
    }
    CHECK(c->frames == FRAMES * FS && c->channels == 2u);
    CHECK(rms_err(c, 0, l, FRAMES * FS) < 0.005);
    CHECK(rms_err(c, 1, r, FRAMES * FS) < 0.005);
    su_audio_clip_free(c);
}

static void test_flags(void) {
    enum { FRAMES = 10 };
    static int16_t x[FRAMES * FS];
    static uint8_t enc[FRAMES * 16];
    SuAudioPsAdpcmDesc d = mono(22050u);
    SuAudioClip *c = NULL;
    SuAudioClip *full = NULL;
    char msg[256];
    uint32_t ls = 0;
    uint32_t le = 0;
    make_signal(x, FRAMES * FS, 500.0, 50.0, 22050.0);
    encode(x, FRAMES, enc);
    /* loop from frame 2 through frame 5 (end + repeat), then trailing frames after the end */
    enc[2 * 16 + 1] = 6;
    enc[3 * 16 + 1] = 2;
    enc[4 * 16 + 1] = 2;
    enc[5 * 16 + 1] = 3;
    CHECK(su_audio_clip_from_ps_adpcm(enc, sizeof enc, &d, &full, &ls, &le, msg) == SU_OK);
    CHECK(full != NULL && full->frames == FRAMES * FS);
    CHECK(ls == 2u * FS && le == 6u * FS);
    d.stop_at_end_flag = 1;
    CHECK(su_audio_clip_from_ps_adpcm(enc, sizeof enc, &d, &c, &ls, &le, msg) == SU_OK);
    CHECK(c != NULL && c->frames == 6u * FS);
    CHECK(ls == 2u * FS && le == 6u * FS);
    if (c != NULL && full != NULL) {
        CHECK(memcmp(c->samples, full->samples, (size_t)c->frames * sizeof(float)) == 0);
    }
    su_audio_clip_free(c);
    c = NULL;
    /* loop start without an end: the loop runs to the end of the clip */
    enc[5 * 16 + 1] = 2;
    d.stop_at_end_flag = 0;
    CHECK(su_audio_clip_from_ps_adpcm(enc, sizeof enc, &d, &c, &ls, &le, msg) == SU_OK);
    CHECK(ls == 2u * FS && le == FRAMES * FS);
    su_audio_clip_free(c);
    c = NULL;
    /* a plain end flag (no loop) */
    memset(enc + 1, 0, 1);
    enc[2 * 16 + 1] = 0;
    enc[3 * 16 + 1] = 0;
    enc[4 * 16 + 1] = 1;
    enc[5 * 16 + 1] = 0;
    d.stop_at_end_flag = 1;
    CHECK(su_audio_clip_from_ps_adpcm(enc, sizeof enc, &d, &c, &ls, &le, msg) == SU_OK);
    CHECK(c != NULL && c->frames == 5u * FS);
    CHECK(ls == 0u && le == 0u);
    su_audio_clip_free(c);
    su_audio_clip_free(full);
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void test_vag(void) {
    enum { FRAMES = 20 };
    static int16_t x[FRAMES * FS];
    static uint8_t file[0x30 + (FRAMES + 2) * 16];
    SuAudioClip *c = NULL;
    char msg[256];
    uint32_t ls = 0;
    uint32_t le = 0;
    uint32_t i;
    memset(file, 0, sizeof file);
    memcpy(file, "VAGp", 4);
    put_be32(file + 4, 0x20u);
    put_be32(file + 0x0C, (FRAMES + 1) * 16u);
    put_be32(file + 0x10, 16000u);
    memcpy(file + 0x20, "synthetic", 9);
    make_signal(x, FRAMES * FS, 1000.0, 333.0, 16000.0);
    encode(x, FRAMES, file + 0x30);
    file[0x30 + 3 * 16 + 1] = 4;
    file[0x30 + (FRAMES - 1) * 16 + 1] = 3;
    /* a trailing frame inside the data size after the end flag, and one beyond the data size */
    file[0x30 + FRAMES * 16] = 0x0C;
    file[0x30 + FRAMES * 16 + 2] = 0x77;
    CHECK(su_audio_clip_from_vag(file, sizeof file, &c, &ls, &le, msg) == SU_OK);
    if (c != NULL) {
        CHECK(c->frames == FRAMES * FS && c->channels == 1u && c->sample_rate == 16000u);
        CHECK(ls == 3u * FS && le == FRAMES * FS);
        CHECK(rms_err(c, 0, x, FRAMES * FS) < 0.005);
        su_audio_clip_free(c);
        c = NULL;
    }
    /* refusals */
    CHECK(su_audio_clip_from_vag(file, 0x2F, &c, NULL, NULL, msg) == SU_E_PARSE && c == NULL);
    file[0] = 'X';
    CHECK(su_audio_clip_from_vag(file, sizeof file, &c, NULL, NULL, msg) == SU_E_PARSE && c == NULL);
    file[0] = 'V';
    put_be32(file + 0x0C, (FRAMES + 3) * 16u);
    CHECK(su_audio_clip_from_vag(file, sizeof file, &c, NULL, NULL, msg) == SU_E_PARSE && c == NULL);
    put_be32(file + 0x0C, 0xFFFFFFF0u);
    CHECK(su_audio_clip_from_vag(file, sizeof file, &c, NULL, NULL, msg) == SU_E_PARSE && c == NULL);
    put_be32(file + 0x0C, 8u);
    CHECK(su_audio_clip_from_vag(file, sizeof file, &c, NULL, NULL, msg) == SU_E_PARSE && c == NULL);
    put_be32(file + 0x0C, 16u);
    put_be32(file + 0x10, 0u);
    CHECK(su_audio_clip_from_vag(file, sizeof file, &c, NULL, NULL, msg) == SU_E_PARSE && c == NULL);
    CHECK(su_audio_clip_from_vag(NULL, 0, &c, NULL, NULL, msg) == SU_E_PARSE);
    CHECK(su_audio_clip_from_vag(file, sizeof file, NULL, NULL, NULL, msg) == SU_E_INVALID_ARGUMENT);
    for (i = 0; i < 0x30u; i++) {
        CHECK(su_audio_clip_from_vag(file, i, &c, NULL, NULL, msg) == SU_E_PARSE && c == NULL);
    }
}

static void test_refusals(void) {
    uint8_t f[64];
    SuAudioPsAdpcmDesc d = mono(22050u);
    SuAudioClip *c = NULL;
    char msg[256];
    memset(f, 0, sizeof f);
    CHECK(su_audio_clip_from_ps_adpcm(f, 0, &d, &c, NULL, NULL, msg) == SU_E_PARSE && c == NULL);
    CHECK(su_audio_clip_from_ps_adpcm(f, 17, &d, &c, NULL, NULL, msg) == SU_E_PARSE && c == NULL);
    CHECK(su_audio_clip_from_ps_adpcm(f, 16, NULL, &c, NULL, NULL, msg) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_clip_from_ps_adpcm(f, 16, &d, NULL, NULL, NULL, msg) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_clip_from_ps_adpcm(NULL, 16, &d, &c, NULL, NULL, msg) == SU_E_INVALID_ARGUMENT);
    d.sample_rate = 0u;
    CHECK(su_audio_clip_from_ps_adpcm(f, 16, &d, &c, NULL, NULL, msg) == SU_E_INVALID_ARGUMENT);
    d.sample_rate = 22050u;
    d.channels = 3u;
    d.interleave = 16u;
    CHECK(su_audio_clip_from_ps_adpcm(f, 48, &d, &c, NULL, NULL, msg) == SU_E_INVALID_ARGUMENT);
    d.channels = 2u;
    d.interleave = 0u;
    CHECK(su_audio_clip_from_ps_adpcm(f, 32, &d, &c, NULL, NULL, msg) == SU_E_INVALID_ARGUMENT);
    d.interleave = 24u;
    CHECK(su_audio_clip_from_ps_adpcm(f, 48, &d, &c, NULL, NULL, msg) == SU_E_INVALID_ARGUMENT);
    d.interleave = 32u;
    CHECK(su_audio_clip_from_ps_adpcm(f, 48, &d, &c, NULL, NULL, msg) == SU_E_PARSE && c == NULL);
    CHECK(su_audio_clip_from_ps_adpcm(f, 64, &d, &c, NULL, NULL, msg) == SU_OK);
    if (c != NULL) {
        CHECK(c->frames == 56u && c->channels == 2u);
        su_audio_clip_free(c);
    }
}

int main(void) {
    test_bit_exact();
    test_round_trip_mono();
    test_round_trip_stereo();
    test_flags();
    test_vag();
    test_refusals();
    printf("audio_adpcm: %ld checks, %ld failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
