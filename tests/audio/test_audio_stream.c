/* Streamed audio, start offsets, loop points and seeking (sutekh/audio.h), offline.
 *
 * Checks: a streamed WAV (from a file and from memory) and a streamed Ogg render byte-identical to the same clip
 * decoded whole (plain, pitched, spatial, odd block sizes, tiny window); start offsets and loop regions in frames
 * and seconds against the exact expected samples; seeking (accurate to the sample, both sources, seconds, bounds);
 * argument refusals; stream ownership; and memory: a five-minute clip streamed through the mixer with its
 * accounted resident bytes, against the whole decode, plus the process's peak working set on Windows.
 *
 * Usage: test_audio_stream WORKDIR [file.ogg]   (WORKDIR receives temporary WAV files, removed at the end) */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

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

static SuAudioMixer *mixer(uint32_t rate) {
    SuAudioMixerDesc d;
    SuAudioMixer *m = NULL;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.sample_rate = rate;
    d.max_voices = 8;
    CHECK(su_audio_mixer_create(&d, &m) == SU_OK);
    return m;
}

static void render(SuAudioMixer *m, float *out, uint32_t frames, uint32_t block) {
    uint32_t done = 0;
    while (done < frames) {
        uint32_t n = frames - done < block ? frames - done : block;
        su_audio_mixer_render(m, out + (size_t)done * 2u, n);
        done += n;
    }
}

/* ---- synthetic WAV (16-bit PCM): two tones plus a little noise, deterministic ---- */

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wav_header(uint8_t h[44], uint32_t frames, uint32_t channels, uint32_t rate) {
    uint32_t data = frames * channels * 2u;
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36u + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16u);
    h[20] = 1;
    h[21] = 0;
    h[22] = (uint8_t)channels;
    h[23] = 0;
    put32(h + 24, rate);
    put32(h + 28, rate * channels * 2u);
    h[32] = (uint8_t)(channels * 2u);
    h[33] = 0;
    h[34] = 16;
    h[35] = 0;
    memcpy(h + 36, "data", 4);
    put32(h + 40, data);
}

static int16_t synth(uint64_t i, uint32_t ch, uint32_t rate, uint64_t *rng) {
    double t = (double)i / (double)rate;
    double v = 0.45 * sin(6.283185307179586 * (220.0 + 110.0 * (double)ch) * t) +
               0.25 * sin(6.283185307179586 * 1234.5 * t);
    *rng ^= *rng << 13;
    *rng ^= *rng >> 7;
    *rng ^= *rng << 17;
    v += 0.05 * ((double)(*rng >> 40) / 8388608.0 - 1.0);
    return (int16_t)lrint(v * 32767.0);
}

static int write_wav(const char *path, uint32_t frames, uint32_t channels, uint32_t rate) {
    FILE *f = fopen(path, "wb");
    uint8_t h[44];
    uint64_t rng = 0x9E3779B97F4A7C15u;
    uint32_t i;
    uint32_t c;
    if (f == NULL) {
        return 0;
    }
    wav_header(h, frames, channels, rate);
    (void)fwrite(h, 1, 44, f);
    for (i = 0; i < frames; i++) {
        for (c = 0; c < channels; c++) {
            int16_t s = synth(i, c, rate, &rng);
            uint8_t b[2];
            b[0] = (uint8_t)(uint16_t)s;
            b[1] = (uint8_t)((uint16_t)s >> 8);
            (void)fwrite(b, 1, 2, f);
        }
    }
    return fclose(f) == 0;
}

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long n;
    if (f == NULL) {
        return NULL;
    }
    (void)fseek(f, 0, SEEK_END);
    n = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n);
    if (b != NULL && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    (void)fclose(f);
    *len = (size_t)n;
    return b;
}

static uint64_t fnv(uint64_t h, const float *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    size_t i;
    for (i = 0; i < n * sizeof(float); i++) {
        h = (h ^ b[i]) * 0x100000001B3ull;
    }
    return h;
}

/* Renders the same voice description once from the clip and once from the stream; returns 1 when the bytes match. */
static int same_render(uint32_t mrate, const SuAudioClip *clip, SuAudioStream *stream, SuAudioVoiceDesc d,
                       uint32_t frames, uint32_t block, float *a, float *b) {
    SuAudioMixer *m = mixer(mrate);
    SuAudioVoice v;
    int ok;
    d.clip = clip;
    d.stream = NULL;
    CHECK(su_audio_play(m, &d, &v) == SU_OK);
    render(m, a, frames, block);
    su_audio_mixer_destroy(m);
    m = mixer(mrate);
    d.clip = NULL;
    d.stream = stream;
    CHECK(su_audio_play(m, &d, &v) == SU_OK);
    render(m, b, frames, block);
    su_audio_mixer_destroy(m);
    ok = memcmp(a, b, (size_t)frames * 2u * sizeof(float)) == 0;
    return ok;
}

static SuAudioVoiceDesc vd(float gain, uint32_t flags) {
    SuAudioVoiceDesc d;
    memset(&d, 0, sizeof d);
    d.gain = gain;
    d.flags = flags;
    return d;
}

static float g_a[48000 * 2 * 8];
static float g_b[48000 * 2 * 8];

/* The same comparisons for one source (a decoded clip and its two stream flavours). */
static void identity_suite(const char *label, const SuAudioClip *clip, SuAudioStream *sf, SuAudioStream *sm,
                           uint32_t seek_to) {
    SuAudioVoiceDesc d;
    uint32_t frames_out = 48000u * 2u;
    SuAudioStream *both[2];
    int k;
    both[0] = sf;
    both[1] = sm;
    for (k = 0; k < 2; k++) {
        SuAudioStream *s = both[k];
        /* plain, 480-frame blocks (resampled 44.1 -> 48 kHz: fractional positions) */
        d = vd(0.8f, 0);
        CHECK(same_render(48000u, clip, s, d, frames_out, 480u, g_a, g_b));
        /* odd block size */
        CHECK(same_render(48000u, clip, s, d, frames_out, 333u, g_a, g_b));
        /* pitched */
        d = vd(1.0f, 0);
        d.pitch = 1.37f;
        CHECK(same_render(48000u, clip, s, d, frames_out, 480u, g_a, g_b));
        /* spatial with Doppler motion */
        d = vd(1.0f, SU_AUDIO_SPATIAL);
        d.pos = su_wpos(3.0, 0.5, -2.0);
        d.vel.x = -40.0f;
        d.doppler = 1.0f;
        d.min_distance = 1.0f;
        CHECK(same_render(48000u, clip, s, d, frames_out, 480u, g_a, g_b));
        /* start offset, loop region (wraps several times, fractional pitch) */
        d = vd(1.0f, SU_AUDIO_LOOP);
        d.pitch = 0.9f;
        d.loop_start = seek_to / 8u + 7u;
        d.loop_end = seek_to / 8u + 5000u;
        d.start_frame = d.loop_start + 100u;
        CHECK(same_render(44100u, clip, s, d, frames_out, 480u, g_a, g_b));
        /* looping the whole clip */
        d = vd(1.0f, SU_AUDIO_LOOP);
        CHECK(same_render(44100u, clip, s, d, (uint32_t)(clip->frames * 2u + 100u) < 48000u * 8u
                                                    ? (uint32_t)(clip->frames * 2u + 100u) : 48000u * 8u,
                          480u, g_a, g_b));
    }
    printf("identity[%s]: streamed (file and memory) == decoded whole, byte for byte, in 6 configurations each\n", label);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    char p20[512];
    char p300[512];
    SuAudioClip *clip = NULL;
    SuAudioStream *sf = NULL;
    SuAudioStream *sm = NULL;
    SuAudioStream *tiny = NULL;
    uint8_t *bytes;
    size_t blen = 0;
    char msg[256];
    uint32_t i;

    (void)snprintf(p20, sizeof p20, "%s/stream_20s.wav", dir);
    (void)snprintf(p300, sizeof p300, "%s/stream_300s.wav", dir);
    CHECK(write_wav(p20, 44100u * 20u, 2, 44100u));
    bytes = slurp(p20, &blen);
    CHECK(bytes != NULL);
    if (bytes == NULL) {
        return 1;
    }
    CHECK(su_audio_clip_from_wav(bytes, blen, &clip, msg) == SU_OK);
    CHECK(su_audio_stream_open_file(p20, 0, &sf, msg) == SU_OK);
    CHECK(su_audio_stream_open_memory(bytes, blen, 0, &sm, msg) == SU_OK);
    if (clip == NULL || sf == NULL || sm == NULL) {
        return 1;
    }
    {
        SuAudioStreamInfo inf;
        su_audio_stream_info(sf, &inf);
        CHECK(inf.frames == clip->frames && inf.channels == 2u && inf.sample_rate == 44100u);
        CHECK(inf.ring_frames == 16384u);
    }

    /* ---- identity with the whole decode ---- */
    identity_suite("wav 20 s stereo", clip, sf, sm, clip->frames / 2u);
    /* the same with the smallest window: loop wraps and the seek-free interpolation partner */
    CHECK(su_audio_stream_open_file(p20, 2048u, &tiny, msg) == SU_OK);
    if (tiny != NULL) {
        SuAudioVoiceDesc d = vd(1.0f, SU_AUDIO_LOOP);
        SuAudioStreamInfo inf;
        d.loop_start = 100000u;
        d.loop_end = 106000u;
        d.pitch = 1.9f;
        CHECK(same_render(44100u, clip, tiny, d, 48000u * 4u, 480u, g_a, g_b));
        su_audio_stream_info(tiny, &inf);
        CHECK(inf.ring_frames == 2048u && inf.seeks > 0u);
        printf("tiny window: ring %u frames, %u decoder seeks over 4 s of a 6000-frame loop\n", (unsigned)inf.ring_frames,
               (unsigned)inf.seeks);
        CHECK(su_audio_stream_close(tiny) == SU_OK);
    }
    {   /* mono 8 kHz source through a stereo mixer */
        SuAudioClip *mc = NULL;
        SuAudioStream *ms = NULL;
        uint8_t *mb;
        size_t ml = 0;
        char pm[512];
        SuAudioVoiceDesc d = vd(1.0f, 0);
        (void)snprintf(pm, sizeof pm, "%s/stream_mono.wav", dir);
        CHECK(write_wav(pm, 8000u * 3u, 1, 8000u));
        mb = slurp(pm, &ml);
        CHECK(mb != NULL && su_audio_clip_from_wav(mb, ml, &mc, msg) == SU_OK);
        CHECK(su_audio_stream_open_file(pm, 0, &ms, msg) == SU_OK);
        if (mc != NULL && ms != NULL) {
            CHECK(same_render(48000u, mc, ms, d, 48000u * 2u, 480u, g_a, g_b));
            d.flags = SU_AUDIO_SPATIAL;
            d.pos = su_wpos(1.0, 0.0, -1.0);
            CHECK(same_render(48000u, mc, ms, d, 48000u * 2u, 480u, g_a, g_b));
        }
        CHECK(su_audio_stream_close(ms) == SU_OK);
        su_audio_clip_free(mc);
        free(mb);
        (void)remove(pm);
    }

    /* ---- start offset and loop points: exact expected samples (mixer rate == clip rate, so no interpolation) ---- */
    for (i = 0; i < 3; i++) {
        SuAudioStream *s = i == 0 ? NULL : (i == 1 ? sf : sm);
        SuAudioMixer *m = mixer(44100u);
        SuAudioVoice v;
        SuAudioVoiceDesc d = vd(1.0f, 0);
        uint32_t bad = 0;
        uint32_t k;
        d.clip = s == NULL ? clip : NULL;
        d.stream = s;
        d.start_frame = 10000u;
        CHECK(su_audio_play(m, &d, &v) == SU_OK);
        render(m, g_a, 4000u, 480u);
        for (k = 0; k < 4000u; k++) {
            bad += g_a[k * 2u] != clip->samples[(10000u + k) * 2u] || g_a[k * 2u + 1u] != clip->samples[(10000u + k) * 2u + 1u];
        }
        CHECK(bad == 0u);
        su_audio_mixer_destroy(m);

        /* start in seconds (0.25 s = 11025 frames) */
        m = mixer(44100u);
        memset(&d, 0, sizeof d);
        d.gain = 1.0f;
        d.clip = s == NULL ? clip : NULL;
        d.stream = s;
        d.start_seconds = 0.25;
        CHECK(su_audio_play(m, &d, &v) == SU_OK);
        render(m, g_a, 1000u, 480u);
        bad = 0;
        for (k = 0; k < 1000u; k++) {
            bad += g_a[k * 2u] != clip->samples[(11025u + k) * 2u];
        }
        CHECK(bad == 0u);
        su_audio_mixer_destroy(m);

        /* loop [1000, 3000) from start 500: output i is source frame 500+i, wrapped into the region */
        m = mixer(44100u);
        memset(&d, 0, sizeof d);
        d.gain = 1.0f;
        d.flags = SU_AUDIO_LOOP;
        d.clip = s == NULL ? clip : NULL;
        d.stream = s;
        d.start_frame = 500u;
        d.loop_start = 1000u;
        d.loop_end = 3000u;
        CHECK(su_audio_play(m, &d, &v) == SU_OK);
        render(m, g_a, 9000u, 480u);
        bad = 0;
        for (k = 0; k < 9000u; k++) {
            uint32_t p = 500u + k;
            if (p >= 3000u) {
                p = 1000u + (p - 1000u) % 2000u;
            }
            bad += g_a[k * 2u] != clip->samples[p * 2u] || g_a[k * 2u + 1u] != clip->samples[p * 2u + 1u];
        }
        CHECK(bad == 0u);
        {
            double pos = -1.0;
            CHECK(su_audio_voice_position(m, v, &pos) == SU_OK);
            CHECK(pos == (double)(1000u + (500u + 9000u - 1000u) % 2000u));
        }
        su_audio_mixer_destroy(m);

        /* loop in seconds: [0.1 s, 0.2 s) = [4410, 8820) */
        m = mixer(44100u);
        memset(&d, 0, sizeof d);
        d.gain = 1.0f;
        d.flags = SU_AUDIO_LOOP;
        d.clip = s == NULL ? clip : NULL;
        d.stream = s;
        d.loop_start_seconds = 0.1;
        d.loop_end_seconds = 0.2;
        CHECK(su_audio_play(m, &d, &v) == SU_OK);
        render(m, g_a, 20000u, 480u);
        bad = 0;
        for (k = 0; k < 20000u; k++) {
            uint32_t p = k;
            if (p >= 8820u) {
                p = 4410u + (p - 4410u) % 4410u;
            }
            bad += g_a[k * 2u] != clip->samples[p * 2u];
        }
        CHECK(bad == 0u);
        su_audio_mixer_destroy(m);

        /* a non-looping voice ignores the loop points and ends at the clip's end; a start near the end is short */
        m = mixer(44100u);
        memset(&d, 0, sizeof d);
        d.gain = 1.0f;
        d.clip = s == NULL ? clip : NULL;
        d.stream = s;
        d.start_frame = clip->frames - 600u;
        d.loop_start = 10u;
        d.loop_end = 20u;
        CHECK(su_audio_play(m, &d, &v) == SU_OK);
        render(m, g_a, 1000u, 480u);
        {
            SuAudioStats st;
            su_audio_update(m);
            su_audio_mixer_stats(m, &st);
            CHECK(st.voices_active == 0u);
            CHECK(!su_audio_voice_active(m, v));
            bad = 0;
            for (k = 0; k < 600u; k++) {
                bad += g_a[k * 2u] != clip->samples[(clip->frames - 600u + k) * 2u];
            }
            for (k = 600u; k < 1000u; k++) {
                bad += g_a[k * 2u] != 0.0f;
            }
            CHECK(bad == 0u);
        }
        su_audio_mixer_destroy(m);
    }

    /* ---- seeking ---- */
    for (i = 0; i < 3; i++) {
        SuAudioStream *s = i == 0 ? NULL : (i == 1 ? sf : sm);
        SuAudioMixer *m = mixer(44100u);
        SuAudioVoice v;
        SuAudioVoiceDesc d = vd(1.0f, 0);
        double pos = 0.0;
        uint32_t bad = 0;
        uint32_t k;
        static const uint32_t targets[] = {777777u, 5u, 400000u, 400001u, 123456u};
        d.clip = s == NULL ? clip : NULL;
        d.stream = s;
        CHECK(su_audio_play(m, &d, &v) == SU_OK);
        render(m, g_a, 100u, 100u);
        CHECK(su_audio_voice_position(m, v, &pos) == SU_OK && pos == 100.0);
        for (k = 0; k < 5u; k++) {
            uint32_t t = targets[k];
            uint32_t j;
            CHECK(su_audio_seek(m, v, t) == SU_OK);
            render(m, g_a, 333u, 333u);
            for (j = 0; j < 333u; j++) {
                bad += g_a[j * 2u] != clip->samples[(size_t)(t + j) * 2u] ||
                       g_a[j * 2u + 1u] != clip->samples[(size_t)(t + j) * 2u + 1u];
            }
            CHECK(su_audio_voice_position(m, v, &pos) == SU_OK && pos == (double)(t + 333u));
        }
        CHECK(bad == 0u);
        CHECK(su_audio_seek_seconds(m, v, 2.5) == SU_OK); /* 110250 */
        render(m, g_a, 64u, 64u);
        CHECK(g_a[0] == clip->samples[110250u * 2u] && g_a[1] == clip->samples[110250u * 2u + 1u]);
        CHECK(su_audio_seek(m, v, clip->frames) == SU_E_INVALID_ARGUMENT);
        CHECK(su_audio_seek(m, v, clip->frames - 1u) == SU_OK);
        CHECK(su_audio_seek_seconds(m, v, -1.0) == SU_E_INVALID_ARGUMENT);
        CHECK(su_audio_seek(m, 0x7777000000000001ull, 0) == SU_E_STALE_HANDLE);
        render(m, g_a, 10u, 10u); /* plays the last frame, then ends */
        su_audio_update(m);
        CHECK(su_audio_seek(m, v, 0) == SU_E_STALE_HANDLE);
        su_audio_mixer_destroy(m);

        /* a looping voice cannot be sought beyond its loop end */
        m = mixer(44100u);
        memset(&d, 0, sizeof d);
        d.gain = 1.0f;
        d.flags = SU_AUDIO_LOOP;
        d.clip = s == NULL ? clip : NULL;
        d.stream = s;
        d.loop_start = 100u;
        d.loop_end = 200u;
        CHECK(su_audio_play(m, &d, &v) == SU_OK);
        CHECK(su_audio_seek(m, v, 200u) == SU_E_INVALID_ARGUMENT);
        CHECK(su_audio_seek(m, v, 199u) == SU_OK);
        su_audio_mixer_destroy(m);
    }

    /* ---- argument refusals and ownership ---- */
    {
        SuAudioMixer *m = mixer(44100u);
        SuAudioVoice v;
        SuAudioVoice v2;
        SuAudioVoiceDesc d = vd(1.0f, 0);
        d.clip = clip;
        d.stream = sf;
        CHECK(su_audio_play(m, &d, &v) == SU_E_INVALID_ARGUMENT);   /* both */
        d.clip = NULL;
        d.stream = NULL;
        CHECK(su_audio_play(m, &d, &v) == SU_E_INVALID_ARGUMENT);   /* neither */
        d.clip = clip;
        d.start_frame = clip->frames;
        CHECK(su_audio_play(m, &d, &v) == SU_E_INVALID_ARGUMENT);
        d.start_frame = 0;
        d.start_seconds = 1.0;
        d.start_frame = 5;
        CHECK(su_audio_play(m, &d, &v) == SU_E_INVALID_ARGUMENT);   /* frames and seconds together */
        d.start_frame = 0;
        d.start_seconds = -1.0;
        CHECK(su_audio_play(m, &d, &v) == SU_E_INVALID_ARGUMENT);
        d.start_seconds = 0.0;
        d.loop_end = clip->frames + 1u;
        CHECK(su_audio_play(m, &d, &v) == SU_E_INVALID_ARGUMENT);
        d.loop_end = 50u;
        d.loop_start = 50u;
        CHECK(su_audio_play(m, &d, &v) == SU_E_INVALID_ARGUMENT);   /* empty region */
        d.loop_start = 10u;
        d.flags = SU_AUDIO_LOOP;
        d.start_frame = 60u;
        CHECK(su_audio_play(m, &d, &v) == SU_E_INVALID_ARGUMENT);   /* start beyond the loop end */
        d.start_frame = 0;
        d.flags = 0;
        d.loop_end = 0;
        d.loop_start = 0;
        CHECK(su_audio_play(m, &d, &v) == SU_OK);

        d.clip = NULL;
        d.stream = sf;
        CHECK(su_audio_play(m, &d, &v) == SU_OK);
        CHECK(su_audio_stream_in_use(sf));
        CHECK(su_audio_play(m, &d, &v2) == SU_E_INVALID_ARGUMENT);  /* already playing */
        CHECK(su_audio_stream_close(sf) == SU_E_INVALID_ARGUMENT);
        CHECK(su_audio_stop(m, v, 0.0f) == SU_OK);
        render(m, g_a, 480u, 480u);
        CHECK(!su_audio_stream_in_use(sf));
        CHECK(su_audio_play(m, &d, &v) == SU_OK);                   /* free again */
        /* a stolen stream voice frees its stream */
        su_audio_mixer_destroy(m);
        CHECK(!su_audio_stream_in_use(sf));                         /* destroy releases */
        {
            SuAudioMixerDesc md;
            SuAudioMixer *m1 = NULL;
            SuAudioVoice a1;
            SuAudioVoice a2;
            SuAudioVoiceDesc d1 = vd(1.0f, 0);
            memset(&md, 0, sizeof md);
            md.struct_size = sizeof md;
            md.sample_rate = 44100u;
            md.max_voices = 1;
            CHECK(su_audio_mixer_create(&md, &m1) == SU_OK);
            d1.stream = sf;
            CHECK(su_audio_play(m1, &d1, &a1) == SU_OK);
            render(m1, g_a, 100u, 100u);
            d1.stream = NULL;
            d1.clip = clip;
            CHECK(su_audio_play(m1, &d1, &a2) == SU_OK);            /* steals the only voice */
            render(m1, g_a, 100u, 100u);
            CHECK(!su_audio_stream_in_use(sf));
            su_audio_mixer_destroy(m1);
        }
    }
    {   /* unreadable and wrong inputs */
        SuAudioStream *bad = NULL;
        CHECK(su_audio_stream_open_file("no/such/file.wav", 0, &bad, msg) == SU_E_IO && bad == NULL);
        CHECK(su_audio_stream_open_memory("RIFF....WAVE", 12, 0, &bad, msg) == SU_E_PARSE && bad == NULL);
        CHECK(su_audio_stream_open_memory("junkjunkjunk", 12, 0, &bad, msg) == SU_E_PARSE && bad == NULL);
        CHECK(su_audio_stream_open_memory(bytes, blen, 100u, &bad, msg) == SU_E_INVALID_ARGUMENT && bad == NULL);
        CHECK(su_audio_stream_open_memory(bytes, blen / 2u, 0, &bad, msg) == SU_E_PARSE && bad == NULL); /* chunk past end */
    }

    /* ---- Ogg Vorbis ---- */
    if (argc > 2 && su_audio_ogg_available()) {
        SuAudioClip *oc = NULL;
        SuAudioStream *of = NULL;
        SuAudioStream *om = NULL;
        size_t ol = 0;
        uint8_t *ob = slurp(argv[2], &ol);
        CHECK(ob != NULL);
        if (ob != NULL) {
            CHECK(su_audio_clip_from_ogg(ob, ol, &oc, msg) == SU_OK);
            CHECK(su_audio_stream_open_file(argv[2], 0, &of, msg) == SU_OK);
            CHECK(su_audio_stream_open_memory(ob, ol, 0, &om, msg) == SU_OK);
            if (oc != NULL && of != NULL && om != NULL) {
                SuAudioStreamInfo inf;
                SuAudioMixer *m;
                SuAudioVoice v;
                SuAudioVoiceDesc d = vd(1.0f, 0);
                uint32_t bad = 0;
                uint32_t k;
                static const uint32_t targets[] = {20000u, 3u, 11025u, 11026u, 1000u};
                su_audio_stream_info(of, &inf);
                CHECK(inf.frames == oc->frames && inf.channels == oc->channels && inf.sample_rate == oc->sample_rate);
                printf("ogg stream: %llu frames, decoder state %llu bytes, window %llu bytes, resident %llu bytes "
                       "(whole decode %llu bytes)\n", (unsigned long long)inf.frames, (unsigned long long)inf.decoder_bytes,
                       (unsigned long long)inf.ring_bytes, (unsigned long long)inf.resident_bytes,
                       (unsigned long long)oc->frames * oc->channels * 4u);
                identity_suite("ogg", oc, of, om, oc->frames / 2u);
                /* accurate Ogg seeks */
                m = mixer(oc->sample_rate);
                d.stream = of;
                CHECK(su_audio_play(m, &d, &v) == SU_OK);
                render(m, g_a, 64u, 64u);
                for (k = 0; k < 5u; k++) {
                    uint32_t j;
                    CHECK(su_audio_seek(m, v, targets[k]) == SU_OK);
                    render(m, g_a, 257u, 257u);
                    for (j = 0; j < 257u; j++) {
                        bad += g_a[j * 2u] != oc->samples[(size_t)(targets[k] + j) * 2u] ||
                               g_a[j * 2u + 1u] != oc->samples[(size_t)(targets[k] + j) * 2u + 1u];
                    }
                }
                CHECK(bad == 0u);
                su_audio_mixer_destroy(m);
                m = mixer(oc->sample_rate);
                {
                    SuAudioVoiceDesc d2 = vd(1.0f, SU_AUDIO_LOOP);
                    d2.stream = om;
                    d2.start_frame = 5000u;
                    d2.loop_start = 2000u;
                    d2.loop_end = 9000u;
                    CHECK(su_audio_play(m, &d2, &v) == SU_OK);
                    render(m, g_a, 30000u, 480u);
                    bad = 0;
                    for (k = 0; k < 30000u; k++) {
                        uint32_t p = 5000u + k;
                        if (p >= 9000u) {
                            p = 2000u + (p - 2000u) % 7000u;
                        }
                        bad += g_a[k * 2u] != oc->samples[(size_t)p * 2u] || g_a[k * 2u + 1u] != oc->samples[(size_t)p * 2u + 1u];
                    }
                    CHECK(bad == 0u);
                }
                su_audio_mixer_destroy(m);
            }
            (void)su_audio_stream_close(of);
            (void)su_audio_stream_close(om);
            su_audio_clip_free(oc);
            free(ob);
        }
    } else {
        printf("ogg: skipped (no file given or built without stb_vorbis)\n");
    }

    CHECK(su_audio_stream_close(sf) == SU_OK);
    CHECK(su_audio_stream_close(sm) == SU_OK);
    su_audio_clip_free(clip);
    free(bytes);
    (void)remove(p20);

    /* ---- memory: five minutes of stereo 44.1 kHz through the mixer at 48 kHz ---- */
    {
        const uint32_t secs = 300u;
        const uint32_t frames_in = 44100u * secs;
        const uint32_t frames_out = 48000u * secs;
        SuAudioMixer *m;
        SuAudioVoice v;
        SuAudioVoiceDesc d = vd(1.0f, 0);
        SuAudioStreamInfo inf;
        uint64_t h_stream = 0xCBF29CE484222325ull;
        uint64_t h_full = 0xCBF29CE484222325ull;
        uint32_t done = 0;
        double ws0 = 0.0;
        double ws1 = 0.0;
        double ws2 = 0.0;
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS pmc;
#endif
        CHECK(write_wav(p300, frames_in, 2, 44100u));
#ifdef _WIN32
        (void)K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
        ws0 = (double)pmc.PeakWorkingSetSize;
#endif
        {
            SuAudioStream *s = NULL;
            CHECK(su_audio_stream_open_file(p300, 0, &s, msg) == SU_OK);
            m = mixer(48000u);
            d.stream = s;
            CHECK(su_audio_play(m, &d, &v) == SU_OK);
            while (done < frames_out) {
                uint32_t n = frames_out - done < 4800u ? frames_out - done : 4800u;
                su_audio_mixer_render(m, g_a, n);
                h_stream = fnv(h_stream, g_a, (size_t)n * 2u);
                done += n;
            }
            su_audio_stream_info(s, &inf);
            su_audio_mixer_destroy(m);
            CHECK(su_audio_stream_close(s) == SU_OK);
            printf("memory: %u s stereo 44.1 kHz clip streamed through a 48 kHz mixer: %llu frames, resident %llu bytes "
                   "(window %llu, decoder %llu), %llu frames decoded, %u seeks\n", (unsigned)secs,
                   (unsigned long long)inf.frames, (unsigned long long)inf.resident_bytes,
                   (unsigned long long)inf.ring_bytes, (unsigned long long)inf.decoder_bytes,
                   (unsigned long long)inf.frames_decoded, (unsigned)inf.seeks);
            CHECK(inf.frames == frames_in && inf.seeks == 0u);
            CHECK(inf.frames_decoded >= inf.frames && inf.frames_decoded <= inf.frames + 1024u);
        }
#ifdef _WIN32
        (void)K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
        ws1 = (double)pmc.PeakWorkingSetSize;
#endif
        {
            size_t fl = 0;
            uint8_t *fb = slurp(p300, &fl);
            SuAudioClip *big = NULL;
            CHECK(fb != NULL && su_audio_clip_from_wav(fb, fl, &big, msg) == SU_OK);
            free(fb);
#ifdef _WIN32
            (void)K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
            ws2 = (double)pmc.PeakWorkingSetSize;
#endif
            if (big != NULL) {
                uint64_t whole = (uint64_t)big->frames * big->channels * sizeof(float);
                m = mixer(48000u);
                d.stream = NULL;
                d.clip = big;
                done = 0;
                CHECK(su_audio_play(m, &d, &v) == SU_OK);
                while (done < frames_out) {
                    uint32_t n = frames_out - done < 4800u ? frames_out - done : 4800u;
                    su_audio_mixer_render(m, g_a, n);
                    h_full = fnv(h_full, g_a, (size_t)n * 2u);
                    done += n;
                }
                su_audio_mixer_destroy(m);
                CHECK(h_full == h_stream);
                CHECK(inf.resident_bytes * 50u < whole);
                printf("memory: whole decode %llu bytes; stream resident %llu bytes (%.2f%%); FNV-1a of the %u s "
                       "output: stream %016llx, whole %016llx\n", (unsigned long long)whole,
                       (unsigned long long)inf.resident_bytes, 100.0 * (double)inf.resident_bytes / (double)whole,
                       (unsigned)secs, (unsigned long long)h_stream, (unsigned long long)h_full);
                su_audio_clip_free(big);
            }
        }
#ifdef _WIN32
        printf("memory: process peak working set (observation): start %.1f MiB, after streaming 5 min %.1f MiB, after "
               "decoding it whole %.1f MiB\n", ws0 / 1048576.0, ws1 / 1048576.0, ws2 / 1048576.0);
        CHECK(ws1 - ws0 < 8.0 * 1048576.0);
#else
        (void)ws0;
        (void)ws1;
        (void)ws2;
#endif
        (void)remove(p300);
    }

    printf("test_audio_stream: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
