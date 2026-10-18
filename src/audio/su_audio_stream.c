/* Streamed clips (sutekh/audio.h): a decoder (WAV or Ogg Vorbis, from a file or borrowed memory) plus a bounded
 * window of decoded frames. The mixer's render thread pulls frames through su_stream_frame; decoding happens in
 * chunks on that thread, so memory is the window plus the decoder state no matter how long the clip is, and the
 * output is a pure function of the command sequence (no worker timing). */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "su_audio_int.h"

#ifdef _WIN32
#define SU_FSEEK(f, o) _fseeki64((f), (long long)(o), SEEK_SET)
#else
#include <sys/types.h>
#define SU_FSEEK(f, o) fseeko((f), (off_t)(o), SEEK_SET)
#endif

#define HEAD_FRAMES 4u
#define DECODE_CHUNK 1024u
#define DEFAULT_RING 16384u
#define MIN_RING 2048u

static void msgf(char *msg, const char *fmt, ...) {
    if (msg != NULL) {
        va_list ap;
        va_start(ap, fmt);
        (void)vsnprintf(msg, 256, fmt, ap);
        va_end(ap);
    }
}

float su_wav_sample(const uint8_t *p, uint32_t format, uint32_t bits) {
    if (format == 3u) {
        uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        float s;
        memcpy(&s, &u, 4);
        return (s - s == 0.0f) ? s : 0.0f; /* finite only */
    }
    if (bits == 8u) {
        return ((float)p[0] - 128.0f) / 128.0f;
    }
    if (bits == 16u) {
        return (float)(int16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)) / 32768.0f;
    }
    if (bits == 24u) {
        int32_t v = (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24)) >> 8;
        return (float)v / 8388608.0f;
    }
    return (float)((double)(int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                                     ((uint32_t)p[3] << 24)) / 2147483648.0);
}

/* ---------- WAV reader ---------- */

typedef struct WavR {
    FILE *f;
    const uint8_t *mem;
    uint64_t size;
    uint64_t data_off;
    uint64_t frames;
    uint64_t pos;
    uint32_t format, channels, bits, block;
} WavR;

static int wav_at(WavR *w, uint64_t off, void *dst, size_t n) {
    if (off > w->size || n > w->size - off) {
        return 0;
    }
    if (w->mem != NULL) {
        memcpy(dst, w->mem + off, n);
        return 1;
    }
    return SU_FSEEK(w->f, off) == 0 && fread(dst, 1, n, w->f) == n;
}

static int wav_seek(void *ctx, uint64_t frame) {
    WavR *w = ctx;
    if (frame > w->frames) {
        return 0;
    }
    w->pos = frame;
    return 1;
}

static uint32_t wav_read(void *ctx, float *out, uint32_t frames) {
    WavR *w = ctx;
    uint8_t buf[4096];
    uint32_t per = (uint32_t)(sizeof buf / w->block);
    uint32_t done = 0;
    uint32_t bytes_per = w->bits / 8u;
    while (done < frames && w->pos < w->frames) {
        uint32_t n = frames - done < per ? frames - done : per;
        uint32_t i;
        if (w->frames - w->pos < n) {
            n = (uint32_t)(w->frames - w->pos);
        }
        if (!wav_at(w, w->data_off + w->pos * w->block, buf, (size_t)n * w->block)) {
            break;
        }
        for (i = 0; i < n * w->channels; i++) {
            out[(size_t)done * w->channels + i] = su_wav_sample(buf + (size_t)i * bytes_per, w->format, w->bits);
        }
        w->pos += n;
        done += n;
    }
    return done;
}

static void wav_close(void *ctx) {
    WavR *w = ctx;
    if (w->f != NULL) {
        (void)fclose(w->f);
    }
    free(w);
}

static uint32_t le16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t su_reader_wav(FILE *f, const uint8_t *mem, size_t len, SuReader *r, char msg[256]) {
    WavR *w = calloc(1, sizeof *w);
    uint8_t h[12];
    uint8_t fmt[40];
    uint64_t at = 12;
    uint32_t fmt_len = 0;
    uint64_t data_len = 0;
    int have_data = 0;
    if (w == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    w->f = f;
    w->mem = mem;
    if (mem != NULL) {
        w->size = len;
    } else if (SU_FSEEK(f, 0) == 0 && fseek(f, 0, SEEK_END) == 0) {
#ifdef _WIN32
        w->size = (uint64_t)_ftelli64(f);
#else
        w->size = (uint64_t)ftello(f);
#endif
    }
    if (!wav_at(w, 0, h, 12) || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) {
        msgf(msg, "not a RIFF WAVE file");
        free(w);
        return SU_E_PARSE;
    }
    while (at + 8u <= w->size) {
        uint8_t ch[8];
        uint32_t n;
        (void)wav_at(w, at, ch, 8);
        n = le32(ch + 4);
        if ((uint64_t)n > w->size - at - 8u) {
            msgf(msg, "chunk at %llu runs past the end", (unsigned long long)at);
            free(w);
            return SU_E_PARSE;
        }
        if (memcmp(ch, "fmt ", 4) == 0 && n >= 16u) {
            fmt_len = n < sizeof fmt ? n : (uint32_t)sizeof fmt;
            (void)wav_at(w, at + 8u, fmt, fmt_len);
        } else if (memcmp(ch, "data", 4) == 0) {
            w->data_off = at + 8u;
            data_len = n;
            have_data = 1;
        }
        at += 8u + n + (n & 1u);
    }
    if (fmt_len == 0u || !have_data) {
        msgf(msg, "missing fmt or data chunk");
        free(w);
        return SU_E_PARSE;
    }
    w->format = le16(fmt);
    w->channels = le16(fmt + 2);
    w->bits = le16(fmt + 14);
    if (w->format == 0xFFFEu && fmt_len >= 26u) {
        w->format = le16(fmt + 24);
    }
    {
        uint32_t rate = le32(fmt + 4);
        if ((w->format != 1u && w->format != 3u) || w->channels < 1u || w->channels > 2u || rate < 1000u ||
            rate > 384000u || (w->format == 1u && w->bits != 8u && w->bits != 16u && w->bits != 24u && w->bits != 32u) ||
            (w->format == 3u && w->bits != 32u)) {
            msgf(msg, "unsupported WAVE format %u, %u channels, %u Hz, %u bits", (unsigned)w->format,
                 (unsigned)w->channels, (unsigned)rate, (unsigned)w->bits);
            free(w);
            return SU_E_PARSE;
        }
        r->rate = rate;
    }
    w->block = w->channels * (w->bits / 8u);
    w->frames = data_len / w->block;
    if (w->frames == 0u) {
        msgf(msg, "no samples");
        free(w);
        return SU_E_PARSE;
    }
    r->ctx = w;
    r->channels = w->channels;
    r->frames = w->frames;
    r->decoder_bytes = sizeof *w + (f != NULL ? 4096u : 0u); /* + the C runtime's FILE buffer (about 4 KiB) */
    r->seek = wav_seek;
    r->read = wav_read;
    r->close = wav_close;
    return SU_OK;
}

/* ---------- stream: decoder + window ---------- */

struct SuAudioStream {
    SuReader r;
    FILE *file;               /* owned by the reader */
    uint32_t channels;
    uint32_t rate;
    uint64_t frames;
    float *ring;              /* ring_frames * channels, indexed by absolute frame % ring_frames */
    uint32_t ring_frames;
    uint64_t wbase;           /* window [wbase, wend) is resident; the reader is positioned at wend */
    uint64_t wend;
    float head[HEAD_FRAMES * 2u];
    uint64_t head_start;
    int head_valid;
    float *chunk;             /* DECODE_CHUNK * channels */
    _Atomic int in_use;
    _Atomic uint64_t decoded;
    _Atomic uint32_t seeks;
};

static uint32_t stream_make(SuAudioStream **out, uint32_t ring_frames, SuReader *r) {
    SuAudioStream *s;
    if (ring_frames == 0u) {
        ring_frames = DEFAULT_RING;
    }
    if (ring_frames < MIN_RING || ring_frames > (1u << 24)) {
        r->close(r->ctx);
        return SU_E_INVALID_ARGUMENT;
    }
    s = calloc(1, sizeof *s);
    if (s != NULL) {
        s->ring = calloc((size_t)ring_frames * r->channels, sizeof(float));
        s->chunk = calloc((size_t)DECODE_CHUNK * r->channels, sizeof(float));
    }
    if (s == NULL || s->ring == NULL || s->chunk == NULL) {
        if (s != NULL) {
            free(s->ring);
            free(s->chunk);
            free(s);
        }
        r->close(r->ctx);
        return SU_E_RESOURCE_LIMIT;
    }
    s->r = *r;
    s->channels = r->channels;
    s->rate = r->rate;
    s->frames = r->frames;
    s->ring_frames = ring_frames;
    atomic_init(&s->in_use, 0);
    atomic_init(&s->decoded, 0u);
    atomic_init(&s->seeks, 0u);
    *out = s;
    return SU_OK;
}

static uint32_t open_any(FILE *f, const uint8_t *mem, size_t len, uint32_t ring_frames, SuAudioStream **out,
                         char msg[256]) {
    uint8_t magic[4] = {0, 0, 0, 0};
    SuReader r;
    uint32_t st;
    memset(&r, 0, sizeof r);
    if (mem != NULL) {
        if (len >= 4u) {
            memcpy(magic, mem, 4);
        }
    } else if (SU_FSEEK(f, 0) != 0 || fread(magic, 1, 4, f) != 4u) {
        msgf(msg, "cannot read the file");
        (void)fclose(f);
        return SU_E_PARSE;
    }
    if (memcmp(magic, "RIFF", 4) == 0) {
        st = su_reader_wav(f, mem, len, &r, msg);
    } else if (memcmp(magic, "OggS", 4) == 0) {
        st = su_reader_ogg(f, mem, len, &r, msg);
    } else {
        msgf(msg, "unrecognised audio format (expected RIFF WAVE or Ogg Vorbis)");
        st = SU_E_PARSE;
    }
    if (st != SU_OK) {
        if (f != NULL && (r.ctx == NULL)) {
            (void)fclose(f); /* readers close it themselves only once they own it */
        }
        return st;
    }
    return stream_make(out, ring_frames, &r);
}

uint32_t su_audio_stream_open_file(const char *path, uint32_t ring_frames, SuAudioStream **out, char message[256]) {
    FILE *f;
    if (path == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    f = fopen(path, "rb");
    if (f == NULL) {
        msgf(message, "cannot open %s", path);
        return SU_E_IO;
    }
    return open_any(f, NULL, 0, ring_frames, out, message);
}

uint32_t su_audio_stream_open_memory(const void *bytes, size_t len, uint32_t ring_frames, SuAudioStream **out,
                                     char message[256]) {
    if (bytes == NULL || out == NULL || len > 0x7FFFFFFFu) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    return open_any(NULL, bytes, len, ring_frames, out, message);
}

int su_audio_stream_in_use(const SuAudioStream *s) {
    return s != NULL && atomic_load_explicit(&((SuAudioStream *)s)->in_use, memory_order_acquire) != 0;
}

uint32_t su_audio_stream_close(SuAudioStream *s) {
    if (s == NULL) {
        return SU_OK;
    }
    if (su_audio_stream_in_use(s)) {
        return SU_E_INVALID_ARGUMENT;
    }
    s->r.close(s->r.ctx);
    free(s->ring);
    free(s->chunk);
    free(s);
    return SU_OK;
}

void su_audio_stream_info(const SuAudioStream *s, SuAudioStreamInfo *out) {
    if (s == NULL || out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    out->frames = s->frames;
    out->channels = s->channels;
    out->sample_rate = s->rate;
    out->ring_frames = s->ring_frames;
    out->ring_bytes = (uint64_t)s->ring_frames * s->channels * sizeof(float);
    out->decoder_bytes = s->r.decoder_bytes;
    out->resident_bytes = sizeof *s + out->ring_bytes + (uint64_t)DECODE_CHUNK * s->channels * sizeof(float) +
                          s->r.decoder_bytes;
    out->frames_decoded = atomic_load_explicit(&((SuAudioStream *)s)->decoded, memory_order_relaxed);
    out->seeks = atomic_load_explicit(&((SuAudioStream *)s)->seeks, memory_order_relaxed);
}

int su_stream_claim(SuAudioStream *s) {
    return atomic_exchange_explicit(&s->in_use, 1, memory_order_acq_rel) == 0;
}

void su_stream_release(SuAudioStream *s) {
    if (s != NULL) {
        atomic_store_explicit(&s->in_use, 0, memory_order_release);
    }
}

uint64_t su_stream_frames(const SuAudioStream *s) {
    return s->frames;
}

uint32_t su_stream_channels(const SuAudioStream *s) {
    return s->channels;
}

uint32_t su_stream_rate(const SuAudioStream *s) {
    return s->rate;
}

/* Moves the reader (and the window) to frame idx. */
static void reposition(SuAudioStream *s, uint64_t idx) {
    (void)s->r.seek(s->r.ctx, idx);
    s->wbase = s->wend = idx;
    atomic_fetch_add_explicit(&s->seeks, 1u, memory_order_relaxed);
}

/* Decodes one chunk at wend into the window (zeros if the decoder runs dry). */
static void decode_more(SuAudioStream *s) {
    uint32_t n = DECODE_CHUNK;
    uint32_t got;
    uint32_t i;
    uint32_t ch = s->channels;
    if (s->frames > s->wend && s->frames - s->wend < n) {
        n = (uint32_t)(s->frames - s->wend);
    }
    got = s->wend < s->frames ? s->r.read(s->r.ctx, s->chunk, n) : 0u;
    if (got < n) {
        memset(s->chunk + (size_t)got * ch, 0, (size_t)(n - got) * ch * sizeof(float));
        (void)s->r.seek(s->r.ctx, s->wend + n); /* keep the reader at wend after the zero padding */
    }
    for (i = 0; i < n; i++) {
        uint32_t slot = (uint32_t)((s->wend + i) % s->ring_frames);
        memcpy(s->ring + (size_t)slot * ch, s->chunk + (size_t)i * ch, ch * sizeof(float));
    }
    s->wend += n;
    if (s->wend - s->wbase > s->ring_frames) {
        s->wbase = s->wend - s->ring_frames;
    }
    atomic_fetch_add_explicit(&s->decoded, n, memory_order_relaxed);
}

void su_stream_prepare_loop(SuAudioStream *s, uint64_t loop_start) {
    uint64_t resume = s->wend;
    uint32_t i;
    if (s->head_valid && s->head_start == loop_start) {
        return;
    }
    (void)s->r.seek(s->r.ctx, loop_start);
    atomic_fetch_add_explicit(&s->seeks, 1u, memory_order_relaxed);
    {
        float tmp[HEAD_FRAMES * 2u];
        uint32_t got = s->r.read(s->r.ctx, tmp, HEAD_FRAMES);
        for (i = 0; i < HEAD_FRAMES * s->channels; i++) {
            s->head[i] = i < got * s->channels ? tmp[i] : 0.0f;
        }
    }
    s->head_start = loop_start;
    s->head_valid = 1;
    (void)s->r.seek(s->r.ctx, resume); /* the window is untouched; the reader goes back to wend */
    atomic_fetch_add_explicit(&s->seeks, 1u, memory_order_relaxed);
}

void su_stream_frame(SuAudioStream *s, uint64_t idx, float *l, float *r) {
    const float *p;
    uint32_t ch = s->channels;
    if (idx >= s->frames) {
        *l = *r = 0.0f;
        return;
    }
    if (!(idx >= s->wbase && idx < s->wend)) {
        if (s->head_valid && idx >= s->head_start && idx < s->head_start + HEAD_FRAMES) {
            p = s->head + (size_t)(idx - s->head_start) * ch;
            *l = p[0];
            *r = ch == 2u ? p[1] : p[0];
            return;
        }
        if (idx < s->wbase || idx - s->wend >= s->ring_frames) {
            reposition(s, idx);
        }
        while (idx >= s->wend) {
            decode_more(s);
        }
    }
    p = s->ring + (size_t)(idx % s->ring_frames) * ch;
    *l = p[0];
    *r = ch == 2u ? p[1] : p[0];
}
