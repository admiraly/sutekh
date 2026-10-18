/* Audio mixer correctness, offline (sutekh/audio.h): generated tones (frequency by zero crossings), exact sample
 * copies at unity rate, resampling and pitch, equal-power panning, distance attenuation, Doppler shifts measured
 * in the output, loops, fades and stops, voice limits and stealing, a full command queue, clamping, determinism,
 * WAV decoding (8/16/24/32-bit PCM, float, extensible) and refusals, Ogg refusals (and an Ogg file when one is
 * given), and the host-event cursor across a re-emitted (rolled back) sequence.
 *
 * Usage: test_audio [file.ogg]   (the Ogg file is optional: decoded and checked for length and level) */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
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

#define RATE 48000u
#define BLOCK 480u

static float g_buf[RATE * 2u * 2u];

static SuAudioMixer *mixer(uint32_t voices, uint32_t cmds) {
    SuAudioMixerDesc d;
    SuAudioMixer *m = NULL;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.sample_rate = RATE;
    d.max_voices = voices;
    d.command_capacity = cmds;
    CHECK(su_audio_mixer_create(&d, &m) == SU_OK);
    return m;
}

/* Renders frames in BLOCK-sized pieces into g_buf. */
static void render(SuAudioMixer *m, uint32_t frames) {
    uint32_t done = 0;
    while (done < frames) {
        uint32_t n = frames - done < BLOCK ? frames - done : BLOCK;
        su_audio_mixer_render(m, g_buf + (size_t)done * 2u, n);
        done += n;
    }
}

/* Frequency of channel ch over [from, to) frames from upward zero crossings (interpolated). */
static double freq_of(uint32_t ch, uint32_t from, uint32_t to) {
    double first = -1.0;
    double last = -1.0;
    uint32_t n = 0;
    uint32_t i;
    for (i = from + 1u; i < to; i++) {
        float a = g_buf[(i - 1u) * 2u + ch];
        float b = g_buf[i * 2u + ch];
        if (a < 0.0f && b >= 0.0f) {
            double t = (double)(i - 1u) + (double)(-a / (b - a));
            if (first < 0.0) {
                first = t;
            } else {
                n++;
            }
            last = t;
        }
    }
    return n > 0u ? (double)n * RATE / (last - first) : 0.0;
}

static double rms(uint32_t ch, uint32_t from, uint32_t to) {
    double s = 0.0;
    uint32_t i;
    for (i = from; i < to; i++) {
        s += (double)g_buf[i * 2u + ch] * g_buf[i * 2u + ch];
    }
    return sqrt(s / (double)(to - from));
}

static SuAudioVoiceDesc voice(const SuAudioClip *c, float gain, uint32_t flags) {
    SuAudioVoiceDesc d;
    memset(&d, 0, sizeof d);
    d.clip = c;
    d.gain = gain;
    d.flags = flags;
    return d;
}

static void test_tones_and_copy(void) {
    SuAudioClip *sine = NULL;
    SuAudioClip *low = NULL;
    SuAudioMixer *m = mixer(8, 64);
    SuAudioVoice v = 0;
    SuAudioVoiceDesc d;
    uint32_t i;
    int exact = 1;
    double f_low;
    double f_pitch;
    CHECK(su_audio_clip_tone(SU_AUDIO_WAVE_SINE, 1000.0f, 1.0f, RATE, 1, &sine) == SU_OK);
    CHECK(sine->frames == RATE && sine->channels == 1u);
    /* unity rate: the output is the clip times the gain, sample for sample, on both channels */
    d = voice(sine, 0.5f, 0);
    CHECK(su_audio_play(m, &d, &v) == SU_OK && su_audio_voice_active(m, v));
    render(m, 4800);
    for (i = 0; i < 4800u; i++) {
        exact &= g_buf[i * 2u] == 0.5f * sine->samples[i] && g_buf[i * 2u + 1u] == 0.5f * sine->samples[i];
    }
    CHECK(exact);
    CHECK(fabs(freq_of(0, 0, 4800) - 1000.0) < 0.5);
    /* resampling: a 1000 Hz tone stored at 24 kHz plays at 1000 Hz; pitch 1.5 gives 1500 Hz */
    CHECK(su_audio_stop(m, v, 0.0f) == SU_OK);
    render(m, BLOCK);
    su_audio_update(m);
    CHECK(!su_audio_voice_active(m, v));
    CHECK(su_audio_clip_tone(SU_AUDIO_WAVE_SINE, 1000.0f, 1.0f, 24000u, 1, &low) == SU_OK);
    d = voice(low, 1.0f, SU_AUDIO_LOOP);
    CHECK(su_audio_play(m, &d, &v) == SU_OK);
    render(m, 9600);
    f_low = freq_of(0, 0, 9600);
    CHECK(fabs(f_low - 1000.0) < 1.0);
    CHECK(su_audio_stop(m, v, 0.0f) == SU_OK);
    render(m, BLOCK);
    d.pitch = 1.5f;
    CHECK(su_audio_play(m, &d, &v) == SU_OK);
    render(m, 9600);
    f_pitch = freq_of(0, 0, 9600);
    CHECK(fabs(f_pitch - 1500.0) < 1.5);
    printf("tones: 1000 Hz at unity rate exact; 24 kHz clip %.2f Hz; pitch 1.5 %.2f Hz\n", f_low, f_pitch);
    su_audio_mixer_destroy(m);
    su_audio_clip_free(sine);
    su_audio_clip_free(low);
    {
        SuAudioClip *sq = NULL;
        SuAudioClip *nz = NULL;
        SuAudioClip *nz2 = NULL;
        CHECK(su_audio_clip_tone(SU_AUDIO_WAVE_SQUARE, 100.0f, 0.1f, RATE, 0, &sq) == SU_OK);
        CHECK(sq->samples[0] == 0.0f && sq->samples[600] == 1.0f && sq->samples[sq->frames - 1u] == 0.0f); /* fades */
        CHECK(su_audio_clip_tone(SU_AUDIO_WAVE_NOISE, 1.0f, 0.1f, RATE, 0, &nz) == SU_OK);
        CHECK(su_audio_clip_tone(SU_AUDIO_WAVE_NOISE, 1.0f, 0.1f, RATE, 0, &nz2) == SU_OK);
        CHECK(memcmp(nz->samples, nz2->samples, nz->frames * sizeof(float)) == 0);
        CHECK(su_audio_clip_tone(9, 1.0f, 0.1f, RATE, 0, &nz2) == SU_E_INVALID_ARGUMENT);
        su_audio_clip_free(sq);
        su_audio_clip_free(nz);
        su_audio_clip_free(nz2);
    }
}

static SuVec3f v3(float x, float y, float z) {
    SuVec3f r;
    r.x = x;
    r.y = y;
    r.z = z;
    return r;
}

static void test_spatial(void) {
    SuAudioClip *sine = NULL;
    SuAudioMixer *m = mixer(8, 64);
    SuAudioVoice v = 0;
    SuAudioVoiceDesc d;
    SuWorldPos lis = su_wpos(5.0e6, 0.0, 0.0); /* far from the origin */
    SuQuatf id = {0.0f, 0.0f, 0.0f, 1.0f};
    float l;
    float r;
    double front;
    double right;
    double near_l;
    double far_l;
    CHECK(su_audio_clip_tone(SU_AUDIO_WAVE_SINE, 500.0f, 1.0f, RATE, 1, &sine) == SU_OK);
    CHECK(su_audio_listener(m, lis, id, v3(0, 0, 0)) == SU_OK);
    /* pan law */
    su_audio_pan(v3(1, 0, 0), &l, &r);
    CHECK(fabsf(l) < 1e-6f && fabsf(r - 1.0f) < 1e-6f);
    su_audio_pan(v3(0, 0, -1), &l, &r);
    CHECK(fabsf(l - 0.70710678f) < 1e-6f && fabsf(r - 0.70710678f) < 1e-6f);
    su_audio_pan(v3(-3, 0, 4), &l, &r); /* behind-left: stereo has no front/back */
    CHECK(l > r && fabsf(l * l + r * r - 1.0f) < 1e-5f);
    /* attenuation law */
    CHECK(su_audio_attenuation(0.5f, 1.0f, 0.0f, 1.0f) == 1.0f);
    CHECK(fabsf(su_audio_attenuation(4.0f, 1.0f, 0.0f, 1.0f) - 0.25f) < 1e-7f);
    CHECK(fabsf(su_audio_attenuation(4.0f, 1.0f, 0.0f, 2.0f) - 0.0625f) < 1e-7f);
    CHECK(su_audio_attenuation(11.0f, 1.0f, 10.0f, 1.0f) == 0.0f);
    /* in the mixer: front at 1 m -> both channels at 0.7071 */
    d = voice(sine, 1.0f, SU_AUDIO_LOOP | SU_AUDIO_SPATIAL);
    d.pos = su_wpos_add(lis, v3(0, 0, -1));
    d.min_distance = 1.0f;
    CHECK(su_audio_play(m, &d, &v) == SU_OK);
    render(m, 9600);
    front = rms(0, 4800, 9600);
    CHECK(fabs(front - 0.70710678 / sqrt(2.0)) < 1e-3 && fabs(rms(1, 4800, 9600) - front) < 1e-6);
    /* move right: the right channel takes all of it */
    CHECK(su_audio_voice_update(m, v, su_wpos_add(lis, v3(1, 0, 0)), v3(0, 0, 0), 1.0f) == SU_OK);
    render(m, 9600);
    right = rms(1, 4800, 9600);
    CHECK(rms(0, 4800, 9600) < 1e-3 && fabs(right - 1.0 / sqrt(2.0)) < 1e-3);
    /* the listener turns 90 degrees left (counter-clockwise about +y): the source at +x is now behind, centred */
    CHECK(su_audio_listener(m, lis, su_quatf_axis_angle(v3(0, 1, 0), 1.5707963f), v3(0, 0, 0)) == SU_OK);
    render(m, 9600);
    CHECK(fabs(rms(0, 4800, 9600) - rms(1, 4800, 9600)) < 2e-3);
    CHECK(su_audio_listener(m, lis, id, v3(0, 0, 0)) == SU_OK);
    /* distance: 4 m with rolloff 1 is a quarter of 1 m */
    CHECK(su_audio_voice_update(m, v, su_wpos_add(lis, v3(0, 0, -1)), v3(0, 0, 0), 1.0f) == SU_OK);
    render(m, 9600);
    near_l = rms(0, 4800, 9600);
    CHECK(su_audio_voice_update(m, v, su_wpos_add(lis, v3(0, 0, -4)), v3(0, 0, 0), 1.0f) == SU_OK);
    render(m, 9600);
    far_l = rms(0, 4800, 9600);
    CHECK(fabs(far_l / near_l - 0.25) < 1e-3);
    printf("spatial: front %.4f, right %.4f, 4 m / 1 m %.4f\n", front, right, far_l / near_l);
    su_audio_mixer_destroy(m);

    /* Doppler: a source approaching at 10%% of the speed of sound sounds 1/0.9 higher, receding 1/1.1 lower */
    m = mixer(8, 64);
    CHECK(su_audio_listener(m, lis, id, v3(0, 0, 0)) == SU_OK);
    d.doppler = 1.0f;
    d.pos = su_wpos_add(lis, v3(0, 0, -100));
    d.vel = v3(0, 0, 34.3f); /* toward the listener */
    CHECK(su_audio_play(m, &d, &v) == SU_OK);
    render(m, 24000);
    {
        double up = freq_of(0, 0, 24000);
        double down;
        CHECK(fabs(up / 500.0 - 1.0 / 0.9) < 2e-3);
        CHECK(su_audio_voice_update(m, v, d.pos, v3(0, 0, -34.3f), 1.0f) == SU_OK);
        render(m, 24000);
        down = freq_of(0, 2400, 24000);
        CHECK(fabs(down / 500.0 - 1.0 / 1.1) < 2e-3);
        /* the listener moving toward a still source: (c + v) / c */
        CHECK(su_audio_voice_update(m, v, d.pos, v3(0, 0, 0), 1.0f) == SU_OK);
        CHECK(su_audio_listener(m, lis, id, v3(0, 0, -34.3f)) == SU_OK);
        render(m, 24000);
        CHECK(fabs(freq_of(0, 2400, 24000) / 500.0 - 1.1) < 2e-3);
        printf("doppler: approaching %.2f Hz (expected %.2f), receding %.2f Hz (expected %.2f)\n", up, 500.0 / 0.9, down,
               500.0 / 1.1);
    }
    CHECK(fabsf(su_audio_doppler(v3(0, 0, 0), v3(0, 0, 0), v3(0, 0, -1), v3(0, 0, 10000), 343.0f, 1.0f) - 4.0f) < 1e-6f);
    su_audio_mixer_destroy(m);
    su_audio_clip_free(sine);
}

static void test_lifetimes(void) {
    SuAudioClip *beep = NULL;
    SuAudioClip *loop = NULL;
    SuAudioMixer *m = mixer(4, 8);
    SuAudioVoice v[6];
    SuAudioVoiceDesc d;
    SuAudioStats st;
    int i;
    int k;
    CHECK(su_audio_clip_tone(SU_AUDIO_WAVE_SINE, 440.0f, 0.05f, RATE, 0, &beep) == SU_OK); /* 2400 frames */
    CHECK(su_audio_clip_tone(SU_AUDIO_WAVE_SINE, 440.0f, 0.1f, RATE, 1, &loop) == SU_OK);
    /* a one-shot ends by itself; a loop keeps going */
    d = voice(beep, 1.0f, 0);
    CHECK(su_audio_play(m, &d, &v[0]) == SU_OK);
    d = voice(loop, 1.0f, SU_AUDIO_LOOP);
    CHECK(su_audio_play(m, &d, &v[1]) == SU_OK);
    render(m, 24000);
    su_audio_update(m);
    CHECK(!su_audio_voice_active(m, v[0]) && su_audio_voice_active(m, v[1]));
    CHECK(rms(0, 20000, 24000) > 0.5); /* the loop at full level half a second in */
    /* a 10 ms fade-out: silent afterwards, and the slot comes back */
    CHECK(su_audio_stop(m, v[1], 0.01f) == SU_OK);
    render(m, 2400);
    su_audio_update(m);
    CHECK(rms(0, 480, 2400) == 0.0 && !su_audio_voice_active(m, v[1]));
    CHECK(su_audio_stop(m, v[1], 0.0f) == SU_E_STALE_HANDLE);
    /* four voices max: the fifth steals the oldest of equal priority; a lower priority is refused */
    for (i = 0; i < 4; i++) {
        d = voice(loop, 0.1f, SU_AUDIO_LOOP);
        d.priority = 1;
        CHECK(su_audio_play(m, &d, &v[i]) == SU_OK);
    }
    d.priority = 1;
    CHECK(su_audio_play(m, &d, &v[4]) == SU_OK);
    CHECK(!su_audio_voice_active(m, v[0]) && su_audio_voice_active(m, v[4]));
    d.priority = 0;
    CHECK(su_audio_play(m, &d, &v[5]) == SU_E_RESOURCE_LIMIT);
    render(m, BLOCK);
    su_audio_mixer_stats(m, &st);
    CHECK(st.voices_active == 4u && st.voices_stolen == 1u && st.plays_refused == 1u);
    /* the command queue holds 8: more updates than that between renders are refused, nothing corrupts */
    for (k = 0, i = 0; i < 12; i++) {
        k += su_audio_voice_update(m, v[1], su_wpos(0, 0, 0), v3(0, 0, 0), 0.1f) == SU_E_RESOURCE_LIMIT;
    }
    CHECK(k == 4);
    render(m, BLOCK);
    su_audio_mixer_stats(m, &st);
    CHECK(st.commands_dropped == 4u && st.voices_active == 4u);
    /* clamping: twelve loud loops sum past 1; the output stays in [-1, 1] and the clamp is counted */
    su_audio_mixer_destroy(m);
    m = mixer(16, 64);
    for (i = 0; i < 12; i++) {
        SuAudioVoice tmp;
        d = voice(loop, 1.0f, SU_AUDIO_LOOP);
        CHECK(su_audio_play(m, &d, &tmp) == SU_OK);
    }
    render(m, 4800);
    su_audio_mixer_stats(m, &st);
    CHECK(st.peak > 11.0f && st.clipped > 0u);
    {
        float mx = 0.0f;
        uint32_t q;
        for (q = 0; q < 9600u; q++) {
            mx = fabsf(g_buf[q]) > mx ? fabsf(g_buf[q]) : mx;
        }
        CHECK(mx == 1.0f);
    }
    su_audio_mixer_destroy(m);
    su_audio_clip_free(beep);
    su_audio_clip_free(loop);
}

/* The same commands and block sizes give the same bytes. */
static void test_determinism(void) {
    static float a[48000 * 2];
    SuAudioClip *saw = NULL;
    SuAudioClip *nz = NULL;
    int run;
    CHECK(su_audio_clip_tone(SU_AUDIO_WAVE_SAW, 220.0f, 0.3f, 44100u, 0, &saw) == SU_OK);
    CHECK(su_audio_clip_tone(SU_AUDIO_WAVE_NOISE, 1.0f, 0.2f, RATE, 0, &nz) == SU_OK);
    for (run = 0; run < 2; run++) {
        SuAudioMixer *m = mixer(8, 64);
        SuAudioVoiceDesc d = voice(saw, 0.7f, SU_AUDIO_SPATIAL | SU_AUDIO_LOOP);
        SuAudioVoice v;
        SuQuatf id = {0, 0, 0, 1};
        int i;
        d.pos = su_wpos(3, 1, -2);
        d.doppler = 1.0f;
        d.vel = v3(-20, 0, 5);
        CHECK(su_audio_play(m, &d, &v) == SU_OK);
        d = voice(nz, 0.3f, 0);
        CHECK(su_audio_play(m, &d, &v) == SU_OK);
        CHECK(su_audio_listener(m, su_wpos(0, 0, 0), id, v3(1, 0, 0)) == SU_OK);
        for (i = 0; i < 100; i++) {
            render(m, 480);
            if (run == 0) {
                memcpy(a + (size_t)i * 960u, g_buf, 960u * sizeof(float));
            } else {
                CHECK(memcmp(a + (size_t)i * 960u, g_buf, 960u * sizeof(float)) == 0);
            }
        }
        su_audio_mixer_destroy(m);
    }
    su_audio_clip_free(saw);
    su_audio_clip_free(nz);
}

/* ---------- WAV ---------- */

static size_t wav(uint8_t *out, uint32_t format, uint32_t channels, uint32_t rate, uint32_t bits, const void *data,
                  uint32_t data_len, int extensible) {
    uint32_t fmt_len = extensible ? 40u : 16u;
    uint32_t block = channels * bits / 8u;
    size_t o = 0;
#define PUT32(v)                                          \
    do {                                                  \
        uint32_t x_ = (v);                                \
        out[o++] = (uint8_t)x_;                           \
        out[o++] = (uint8_t)(x_ >> 8);                    \
        out[o++] = (uint8_t)(x_ >> 16);                   \
        out[o++] = (uint8_t)(x_ >> 24);                   \
    } while (0)
#define PUT16(v)                       \
    do {                               \
        out[o++] = (uint8_t)(v);       \
        out[o++] = (uint8_t)((v) >> 8); \
    } while (0)
    memcpy(out, "RIFF", 4);
    o = 4;
    PUT32(4u + 8u + fmt_len + 8u + 8u + 4u + 8u + data_len);
    memcpy(out + o, "WAVE", 4);
    o += 4;
    memcpy(out + o, "LIST", 4); /* an unrelated chunk first */
    o += 4;
    PUT32(4u);
    memcpy(out + o, "INFO", 4);
    o += 4;
    memcpy(out + o, "fmt ", 4);
    o += 4;
    PUT32(fmt_len);
    PUT16(extensible ? 0xFFFEu : format);
    PUT16(channels);
    PUT32(rate);
    PUT32(rate * block);
    PUT16(block);
    PUT16(bits);
    if (extensible) {
        PUT16(22u);
        PUT16(bits);
        PUT32(3u);
        PUT16(format);
        memcpy(out + o, "\x00\x00\x00\x00\x10\x00\x80\x00\x00\xAA\x00\x38\x9B\x71", 14);
        o += 14;
    }
    memcpy(out + o, "data", 4);
    o += 4;
    PUT32(data_len);
    memcpy(out + o, data, data_len);
    o += data_len;
    return o;
#undef PUT32
#undef PUT16
}

static void test_wav(void) {
    static uint8_t buf[4096];
    int16_t s16[6] = {0, 16384, -32768, 32767, -1, 100};
    uint8_t s24[6] = {0x00, 0x00, 0x40, 0x00, 0x00, 0x80}; /* 0.5, -1.0 */
    uint8_t s8[3] = {128, 255, 0};
    float f32[2] = {0.25f, -0.75f};
    int32_t s32[1] = {-1073741824};
    SuAudioClip *c = NULL;
    char msg[256];
    size_t n;
    n = wav(buf, 1, 2, 22050, 16, s16, sizeof s16, 0);
    CHECK(su_audio_clip_from_wav(buf, n, &c, msg) == SU_OK);
    CHECK(c != NULL && c->channels == 2u && c->frames == 3u && c->sample_rate == 22050u);
    if (c != NULL) {
        CHECK(c->samples[1] == 0.5f && c->samples[2] == -1.0f && c->samples[3] == 32767.0f / 32768.0f);
        su_audio_clip_free(c);
    }
    n = wav(buf, 1, 1, 48000, 24, s24, sizeof s24, 1);
    CHECK(su_audio_clip_from_wav(buf, n, &c, msg) == SU_OK && c->frames == 2u && c->samples[0] == 0.5f &&
          c->samples[1] == -1.0f);
    su_audio_clip_free(c);
    n = wav(buf, 1, 1, 8000, 8, s8, sizeof s8, 0);
    CHECK(su_audio_clip_from_wav(buf, n, &c, msg) == SU_OK && c->samples[0] == 0.0f && c->samples[2] == -1.0f);
    su_audio_clip_free(c);
    n = wav(buf, 3, 2, 44100, 32, f32, sizeof f32, 0);
    CHECK(su_audio_clip_from_wav(buf, n, &c, msg) == SU_OK && c->samples[0] == 0.25f && c->samples[1] == -0.75f);
    su_audio_clip_free(c);
    n = wav(buf, 1, 1, 44100, 32, s32, sizeof s32, 0);
    CHECK(su_audio_clip_from_wav(buf, n, &c, msg) == SU_OK && c->samples[0] == -0.5f);
    su_audio_clip_free(c);
    /* refusals */
    n = wav(buf, 2, 1, 44100, 16, s16, sizeof s16, 0); /* ADPCM */
    CHECK(su_audio_clip_from_wav(buf, n, &c, msg) == SU_E_PARSE && c == NULL && strstr(msg, "unsupported") != NULL);
    n = wav(buf, 1, 3, 44100, 16, s16, sizeof s16, 0); /* three channels */
    CHECK(su_audio_clip_from_wav(buf, n, &c, msg) == SU_E_PARSE);
    n = wav(buf, 1, 1, 44100, 16, s16, sizeof s16, 0);
    CHECK(su_audio_clip_from_wav(buf, n - 4u, &c, msg) == SU_E_PARSE); /* data chunk cut short */
    CHECK(su_audio_clip_from_wav("RIFX0000WAVE", 12, &c, msg) == SU_E_PARSE);
    CHECK(su_audio_clip_from_wav(buf, 8, &c, msg) == SU_E_PARSE);
}

static void test_ogg(const char *path) {
    SuAudioClip *c = NULL;
    char msg[256];
    static const uint8_t junk[64] = {'O', 'g', 'g', 'S', 0, 2};
    uint32_t st = su_audio_clip_from_ogg(junk, sizeof junk, &c, msg);
    if (!su_audio_ogg_available()) {
        CHECK(st == SU_E_UNSUPPORTED_METHOD);
        printf("ogg: not built (stb_vorbis absent)\n");
        return;
    }
    CHECK(st == SU_E_PARSE && c == NULL);
    if (path != NULL) {
        FILE *f = fopen(path, "rb");
        long n;
        uint8_t *b;
        CHECK(f != NULL);
        if (f == NULL) {
            return;
        }
        (void)fseek(f, 0, SEEK_END);
        n = ftell(f);
        (void)fseek(f, 0, SEEK_SET);
        b = malloc((size_t)n);
        CHECK(b != NULL && fread(b, 1, (size_t)n, f) == (size_t)n);
        fclose(f);
        st = su_audio_clip_from_ogg(b, (size_t)n, &c, msg);
        CHECK(st == SU_OK);
        if (st == SU_OK) {
            SuAudioMixer *m = mixer(4, 16);
            SuAudioVoiceDesc d = voice(c, 1.0f, 0);
            SuAudioVoice v;
            printf("ogg: %s: %u frames, %u channels, %u Hz\n", path, (unsigned)c->frames, (unsigned)c->channels,
                   (unsigned)c->sample_rate);
            /* the fixture: 0.5 s at 44.1 kHz, 440 Hz, left 0.5 and right 0.25 (tools/gen_audio_fixture.py) */
            CHECK(c->frames == 22050u && c->channels == 2u && c->sample_rate == 44100u);
            {
                double sl = 0.0;
                double sr = 0.0;
                uint32_t q;
                uint32_t crossings = 0;
                for (q = 2205; q < 19845u; q++) {
                    sl += (double)c->samples[q * 2u] * c->samples[q * 2u];
                    sr += (double)c->samples[q * 2u + 1u] * c->samples[q * 2u + 1u];
                    crossings += c->samples[(q - 1u) * 2u] < 0.0f && c->samples[q * 2u] >= 0.0f;
                }
                sl = sqrt(sl / 17640.0);
                sr = sqrt(sr / 17640.0);
                printf("ogg: rms left %.4f (expected %.4f), right %.4f (expected %.4f), %u cycles in 0.4 s\n", sl,
                       0.5 / sqrt(2.0), sr, 0.25 / sqrt(2.0), (unsigned)crossings);
                CHECK(fabs(sl - 0.5 / sqrt(2.0)) < 0.01 && fabs(sr - 0.25 / sqrt(2.0)) < 0.01);
                CHECK(crossings >= 175u && crossings <= 177u); /* 440 Hz * 0.4 s = 176 */
            }
            /* resampled 44.1 -> 48 kHz by the mixer: still 440 Hz */
            CHECK(su_audio_play(m, &d, &v) == SU_OK);
            render(m, 19200);
            CHECK(fabs(freq_of(0, 2400, 19200) - 440.0) < 0.5);
            su_audio_mixer_destroy(m);
            su_audio_clip_free(c);
        }
        free(b);
    } else {
        printf("ogg: refusal checked; no Ogg file given, decoding a real stream not run here\n");
    }
}

static void test_event_cursor(void) {
    SuHostEvent ev[5];
    SuAudioEventCursor cur;
    int played = 0;
    int i;
    memset(ev, 0, sizeof ev);
    memset(&cur, 0, sizeof cur);
    for (i = 0; i < 5; i++) {
        ev[i].event_seq = (uint64_t)i;
        ev[i].tick = 10u + (uint64_t)i;
        ev[i].kind = 1;
    }
    for (i = 0; i < 5; i++) {
        played += su_audio_event_new(&cur, &ev[i]);
    }
    CHECK(played == 5 && cur.next_seq == 5u);
    /* a rollback re-emits seq 3 and 4 identically: nothing plays twice */
    played = su_audio_event_new(&cur, &ev[3]) + su_audio_event_new(&cur, &ev[4]);
    CHECK(played == 0);
    ev[0].event_seq = 5;
    CHECK(su_audio_event_new(&cur, &ev[0]) == 1 && cur.next_seq == 6u);
}

int main(int argc, char **argv) {
    test_tones_and_copy();
    test_spatial();
    test_lifetimes();
    test_determinism();
    test_wav();
    test_ogg(argc > 1 ? argv[1] : NULL);
    test_event_cursor();
    printf("test_audio: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
