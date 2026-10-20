/* Audio clips, the voice mixer and the host-event cursor (sutekh/audio.h). Portable; no third-party code.
 *
 * The game thread owns voice-slot bookkeeping (generations, priorities, start order) and sends commands through a
 * single-producer single-consumer ring; the render side (device thread or offline caller) applies them at the start
 * of each block and reports finished voices through a second ring. Neither side locks; the render side never
 * allocates. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "su_audio_int.h"

#define PI_F 3.14159265358979f

static void msgf(char *msg, const char *fmt, ...) {
    if (msg != NULL) {
        va_list ap;
        va_start(ap, fmt);
        (void)vsnprintf(msg, 256, fmt, ap);
        va_end(ap);
    }
}

/* ---------- clips ---------- */

static SuAudioClip *clip_alloc(uint32_t frames, uint32_t channels, uint32_t rate) {
    SuAudioClip *c = calloc(1, sizeof *c);
    if (c == NULL) {
        return NULL;
    }
    c->samples = calloc((size_t)frames * channels + 1u, sizeof(float));
    if (c->samples == NULL) {
        free(c);
        return NULL;
    }
    c->frames = frames;
    c->channels = channels;
    c->sample_rate = rate;
    return c;
}

void su_audio_clip_free(SuAudioClip *clip) {
    if (clip != NULL) {
        free(clip->samples);
        free(clip);
    }
}

uint32_t su_audio_clip_tone(uint32_t wave, float frequency, float seconds, uint32_t sample_rate, int loopable,
                            SuAudioClip **out) {
    uint32_t frames;
    uint32_t i;
    uint32_t fade;
    uint64_t rng = 0x2545F4914F6CDD1Du;
    SuAudioClip *c;
    if (out == NULL || wave > SU_AUDIO_WAVE_NOISE || !(frequency > 0.0f) || !(seconds > 0.0f) || sample_rate < 8000u ||
        sample_rate > 384000u || seconds > 600.0f) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (loopable && wave != SU_AUDIO_WAVE_NOISE) {
        /* whole periods: round the length to a multiple of the period */
        double periods = floor((double)seconds * (double)frequency + 0.5);
        if (periods < 1.0) {
            periods = 1.0;
        }
        frames = (uint32_t)llround(periods * (double)sample_rate / (double)frequency);
    } else {
        frames = (uint32_t)llround((double)seconds * (double)sample_rate);
    }
    if (frames == 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    c = clip_alloc(frames, 1, sample_rate);
    if (c == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    fade = loopable ? 0u : sample_rate / 200u; /* 5 ms */
    if (fade * 2u > frames) {
        fade = frames / 2u;
    }
    for (i = 0; i < frames; i++) {
        double ph = fmod((double)i * (double)frequency / (double)sample_rate, 1.0);
        float s;
        switch (wave) {
        case SU_AUDIO_WAVE_SINE:
            s = (float)sin(2.0 * 3.141592653589793 * ph);
            break;
        case SU_AUDIO_WAVE_SQUARE:
            s = ph < 0.5 ? 1.0f : -1.0f;
            break;
        case SU_AUDIO_WAVE_SAW:
            s = (float)(2.0 * ph - 1.0);
            break;
        default:
            rng ^= rng << 13;
            rng ^= rng >> 7;
            rng ^= rng << 17;
            s = (float)((double)(rng >> 11) / 4503599627370496.0 - 1.0);
            break;
        }
        if (fade > 0u && i < fade) {
            s *= (float)i / (float)fade;
        } else if (fade > 0u && frames - 1u - i < fade) {
            s *= (float)(frames - 1u - i) / (float)fade;
        }
        c->samples[i] = s;
    }
    *out = c;
    return SU_OK;
}

static uint32_t rd16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t su_audio_clip_from_wav(const void *bytes, size_t len, SuAudioClip **out, char message[256]) {
    const uint8_t *b = bytes;
    size_t at = 12;
    const uint8_t *fmt = NULL;
    const uint8_t *data = NULL;
    uint32_t data_len = 0;
    uint32_t format;
    uint32_t channels;
    uint32_t rate;
    uint32_t bits;
    uint32_t block;
    uint32_t frames;
    uint32_t i;
    SuAudioClip *c;
    if (out == NULL || (bytes == NULL && len > 0u)) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (len < 12u || memcmp(b, "RIFF", 4) != 0 || memcmp(b + 8, "WAVE", 4) != 0) {
        msgf(message, "not a RIFF WAVE file");
        return SU_E_PARSE;
    }
    while (at + 8u <= len) {
        uint32_t n = rd32(b + at + 4);
        if (n > len - at - 8u) {
            msgf(message, "chunk at %u runs past the end", (unsigned)at);
            return SU_E_PARSE;
        }
        if (memcmp(b + at, "fmt ", 4) == 0 && n >= 16u) {
            fmt = b + at + 8;
        } else if (memcmp(b + at, "data", 4) == 0) {
            data = b + at + 8;
            data_len = n;
        }
        at += 8u + n + (n & 1u);
    }
    if (fmt == NULL || data == NULL) {
        msgf(message, "missing fmt or data chunk");
        return SU_E_PARSE;
    }
    format = rd16(fmt);
    channels = rd16(fmt + 2);
    rate = rd32(fmt + 4);
    bits = rd16(fmt + 14);
    if (format == 0xFFFEu && rd32(fmt - 4) >= 40u) {
        format = rd16(fmt + 24); /* WAVE_FORMAT_EXTENSIBLE: the sub-format's first two bytes */
    }
    if ((format != 1u && format != 3u) || channels < 1u || channels > 2u || rate < 1000u || rate > 384000u ||
        (format == 1u && bits != 8u && bits != 16u && bits != 24u && bits != 32u) || (format == 3u && bits != 32u)) {
        msgf(message, "unsupported WAVE format %u, %u channels, %u Hz, %u bits", (unsigned)format, (unsigned)channels,
             (unsigned)rate, (unsigned)bits);
        return SU_E_PARSE;
    }
    block = channels * (bits / 8u);
    frames = data_len / block;
    if (frames == 0u) {
        msgf(message, "no samples");
        return SU_E_PARSE;
    }
    c = clip_alloc(frames, channels, rate);
    if (c == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    for (i = 0; i < frames * channels; i++) {
        c->samples[i] = su_wav_sample(data + (size_t)i * (bits / 8u), format, bits);
    }
    *out = c;
    return SU_OK;
}

/* ---------- pure helpers ---------- */

float su_audio_attenuation(float d, float min_distance, float max_distance, float rolloff) {
    float mn = min_distance > 0.0f ? min_distance : 1.0f;
    float ro = rolloff > 0.0f ? rolloff : 1.0f;
    if (max_distance > 0.0f && d >= max_distance) {
        return 0.0f;
    }
    if (d <= mn) {
        return 1.0f;
    }
    return ro == 1.0f ? mn / d : powf(mn / d, ro);
}

void su_audio_pan(SuVec3f dir, float *left, float *right) {
    float l = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    float pan = l > 1e-9f ? dir.x / l : 0.0f;
    float a = (pan + 1.0f) * (PI_F * 0.25f);
    *left = cosf(a);
    *right = sinf(a);
}

float su_audio_doppler(SuVec3f lp, SuVec3f lv, SuVec3f sp, SuVec3f sv, float c, float amount) {
    SuVec3f u;
    float d;
    float vl;
    float vs;
    float f;
    if (!(amount > 0.0f) || !(c > 0.0f)) {
        return 1.0f;
    }
    u.x = sp.x - lp.x;
    u.y = sp.y - lp.y;
    u.z = sp.z - lp.z;
    d = sqrtf(u.x * u.x + u.y * u.y + u.z * u.z);
    if (d < 1e-6f) {
        return 1.0f;
    }
    u.x /= d;
    u.y /= d;
    u.z /= d;
    vl = (lv.x * u.x + lv.y * u.y + lv.z * u.z) * amount;    /* listener toward the source */
    vs = -(sv.x * u.x + sv.y * u.y + sv.z * u.z) * amount;   /* source toward the listener */
    if (vs > 0.9f * c) {
        vs = 0.9f * c;
    }
    f = (c + vl) / (c - vs);
    return f < 0.25f ? 0.25f : f > 4.0f ? 4.0f : f;
}

/* ---------- mixer ---------- */

#define CMD_PLAY 1u
#define CMD_UPDATE 2u
#define CMD_STOP 3u
#define CMD_LISTENER 4u
#define CMD_MASTER 5u
#define CMD_SEEK 6u
#define CMD_PITCH 7u
#define CMD_GAIN 8u
#define CMD_LIMIT 9u

typedef struct Cmd {
    uint32_t type;
    uint32_t slot;
    uint32_t gen;
    float a;
    uint64_t frame;       /* PLAY: start frame; SEEK: target */
    uint64_t ls, le;      /* PLAY: resolved loop region */
    SuAudioVoiceDesc d;   /* PLAY; UPDATE uses pos, vel, gain; LISTENER uses pos, vel and rot */
    SuQuatf rot;
} Cmd;

typedef struct Voice {
    uint32_t gen;
    int active;
    SuAudioVoiceDesc d;
    double pos;           /* frames into the clip */
    uint64_t frames;      /* source length */
    uint64_t ls, le;      /* loop region [ls, le) */
    uint32_t channels;
    uint32_t src_rate;
    SuAudioStream *stream; /* claimed by the game thread at play, released when the voice ends */
    float cur_l, cur_r;   /* gains at the end of the last block */
    int fresh;            /* no ramp on the first block */
    float fade;           /* 1 = full */
    float fade_step;      /* per frame; < 0 while stopping */
    float pitch_from;     /* > 0 for the block after a pitch change: the rate ramps from this pitch to d.pitch */
} Voice;

typedef struct GtSlot {
    uint32_t gen;
    int busy;
    uint32_t priority;
    uint64_t order;
    uint64_t limit;       /* exclusive seek bound */
    uint32_t src_rate;
} GtSlot;

struct SuAudioMixer {
    uint32_t rate;
    uint32_t max_voices;
    float c;
    /* commands: game thread -> render */
    Cmd *cmds;
    uint32_t cmd_cap;
    _Atomic uint32_t cmd_head; /* consumer */
    _Atomic uint32_t cmd_tail; /* producer */
    /* ended voices: render -> game thread (slot | gen << 16 is not enough; two words) */
    uint32_t *ended;
    uint32_t ended_cap;
    _Atomic uint32_t end_head;
    _Atomic uint32_t end_tail;
    /* render side */
    Voice *v;
    _Atomic uint64_t *vpos;   /* per slot: bits of the voice's position after the last render */
    SuWorldPos lpos;
    SuQuatf lrot;
    SuVec3f lvel;
    float master;
    float knee;               /* soft limiter threshold; 0 = off (hard clamp) */
    /* game side */
    GtSlot *gt;
    uint64_t order;
    /* stats */
    _Atomic uint32_t stat_active;
    uint32_t stolen;
    uint32_t refused;
    uint32_t dropped;
    _Atomic uint64_t frames;
    _Atomic uint32_t peak_bits;  /* float bits */
    _Atomic uint32_t clipped;
};

uint32_t su_audio_mixer_create(const SuAudioMixerDesc *d, SuAudioMixer **out) {
    SuAudioMixer *m;
    if (d == NULL || out == NULL || d->struct_size != sizeof *d || d->max_voices > 4096u) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    m = calloc(1, sizeof *m);
    if (m == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    m->rate = d->sample_rate != 0u ? d->sample_rate : 48000u;
    m->max_voices = d->max_voices != 0u ? d->max_voices : 64u;
    m->cmd_cap = d->command_capacity != 0u ? d->command_capacity : 1024u;
    m->c = d->speed_of_sound > 0.0f ? d->speed_of_sound : 343.0f;
    m->ended_cap = m->max_voices * 4u + 4u;
    m->cmds = calloc(m->cmd_cap, sizeof *m->cmds);
    m->ended = calloc(m->ended_cap * 2u, sizeof *m->ended);
    m->v = calloc(m->max_voices, sizeof *m->v);
    m->gt = calloc(m->max_voices, sizeof *m->gt);
    m->vpos = calloc(m->max_voices, sizeof *m->vpos);
    if (m->cmds == NULL || m->ended == NULL || m->v == NULL || m->gt == NULL || m->vpos == NULL) {
        su_audio_mixer_destroy(m);
        return SU_E_RESOURCE_LIMIT;
    }
    m->lrot.w = 1.0f;
    m->master = 1.0f;
    atomic_init(&m->cmd_head, 0u);
    atomic_init(&m->cmd_tail, 0u);
    atomic_init(&m->end_head, 0u);
    atomic_init(&m->end_tail, 0u);
    atomic_init(&m->stat_active, 0u);
    atomic_init(&m->frames, 0u);
    atomic_init(&m->peak_bits, 0u);
    atomic_init(&m->clipped, 0u);
    *out = m;
    return SU_OK;
}

void su_audio_mixer_destroy(SuAudioMixer *m) {
    if (m != NULL) {
        if (m->v != NULL) {
            uint32_t i;
            for (i = 0; i < m->max_voices; i++) {
                if (m->v[i].stream != NULL) {
                    su_stream_release(m->v[i].stream);
                }
            }
        }
        if (m->cmds != NULL) {   /* plays queued but never rendered still hold their streams */
            uint32_t h = atomic_load_explicit(&m->cmd_head, memory_order_relaxed);
            uint32_t t = atomic_load_explicit(&m->cmd_tail, memory_order_relaxed);
            for (; h != t; h++) {
                if (m->cmds[h % m->cmd_cap].type == CMD_PLAY) {
                    su_stream_release(m->cmds[h % m->cmd_cap].d.stream);
                }
            }
        }
        free((void *)m->vpos); /* drop _Atomic: MSVC C4090 */
        free(m->cmds);
        free(m->ended);
        free(m->v);
        free(m->gt);
        free(m);
    }
}

static int push_cmd(SuAudioMixer *m, const Cmd *c) {
    uint32_t tail = atomic_load_explicit(&m->cmd_tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&m->cmd_head, memory_order_acquire);
    if (tail - head >= m->cmd_cap) {
        m->dropped++;
        return 0;
    }
    m->cmds[tail % m->cmd_cap] = *c;
    atomic_store_explicit(&m->cmd_tail, tail + 1u, memory_order_release);
    return 1;
}

static SuAudioVoice voice_handle(uint32_t slot, uint32_t gen) {
    return ((uint64_t)gen << 32) | (uint64_t)(slot + 1u);
}

static int voice_slot(const SuAudioMixer *m, SuAudioVoice h, uint32_t *slot) {
    uint32_t s = (uint32_t)(h & 0xFFFFFFFFu);
    if (m == NULL || s == 0u || s > m->max_voices || m->gt[s - 1u].gen != (uint32_t)(h >> 32) || !m->gt[s - 1u].busy) {
        return 0;
    }
    *slot = s - 1u;
    return 1;
}

/* Frames from a frames field or, while that is 0, its seconds field. 0 on success. */
static int resolve_field(uint64_t fr, double sec, uint32_t rate, uint64_t *out) {
    if (!(sec >= 0.0) || sec > 1.0e9) {
        return 1;
    }
    if (fr != 0u && sec != 0.0) {
        return 1;
    }
    *out = fr != 0u ? fr : (uint64_t)llround(sec * (double)rate);
    return 0;
}

uint32_t su_audio_play(SuAudioMixer *m, const SuAudioVoiceDesc *d, SuAudioVoice *out) {
    uint32_t i;
    uint32_t slot = UINT32_MAX;
    uint64_t frames;
    uint64_t start = 0;
    uint64_t ls = 0;
    uint64_t le = 0;
    uint32_t rate;
    int claimed = 0;
    Cmd c;
    if (m == NULL || d == NULL || out == NULL || !(d->gain >= 0.0f) || !(d->pitch >= 0.0f) ||
        (d->flags & ~(SU_AUDIO_LOOP | SU_AUDIO_SPATIAL)) != 0u || (d->clip == NULL) == (d->stream == NULL)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (d->clip != NULL) {
        if (d->clip->frames == 0u || d->clip->samples == NULL || (d->clip->channels != 1u && d->clip->channels != 2u)) {
            return SU_E_INVALID_ARGUMENT;
        }
        frames = d->clip->frames;
        rate = d->clip->sample_rate;
    } else {
        frames = su_stream_frames(d->stream);
        rate = su_stream_rate(d->stream);
    }
    if (resolve_field(d->start_frame, d->start_seconds, rate, &start) ||
        resolve_field(d->loop_start, d->loop_start_seconds, rate, &ls) ||
        resolve_field(d->loop_end, d->loop_end_seconds, rate, &le)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (le == 0u) {
        le = frames;
    }
    if (start >= frames || ls >= le || le > frames || ((d->flags & SU_AUDIO_LOOP) != 0u && start >= le)) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = 0;
    if (atomic_load_explicit(&m->cmd_tail, memory_order_relaxed) -
            atomic_load_explicit(&m->cmd_head, memory_order_acquire) >= m->cmd_cap) {
        m->dropped++;
        return SU_E_RESOURCE_LIMIT;
    }
    for (i = 0; i < m->max_voices && slot == UINT32_MAX; i++) {
        if (!m->gt[i].busy) {
            slot = i;
        }
    }
    if (d->stream != NULL) {
        if (!su_stream_claim(d->stream)) {
            return SU_E_INVALID_ARGUMENT; /* already playing */
        }
        claimed = 1;
    }
    if (slot == UINT32_MAX) {
        /* steal the oldest voice whose priority is not above the new one */
        uint64_t oldest = UINT64_MAX;
        for (i = 0; i < m->max_voices; i++) {
            if (m->gt[i].priority <= d->priority && m->gt[i].order < oldest) {
                oldest = m->gt[i].order;
                slot = i;
            }
        }
        if (slot == UINT32_MAX) {
            if (claimed) {
                su_stream_release(d->stream);
            }
            m->refused++;
            return SU_E_RESOURCE_LIMIT;
        }
        m->stolen++;
    }
    m->gt[slot].gen++;
    m->gt[slot].busy = 1;
    m->gt[slot].priority = d->priority;
    m->gt[slot].order = m->order++;
    m->gt[slot].limit = (d->flags & SU_AUDIO_LOOP) != 0u ? le : frames;
    m->gt[slot].src_rate = rate;
    memset(&c, 0, sizeof c);
    c.type = CMD_PLAY;
    c.frame = start;
    c.ls = ls;
    c.le = le;
    c.slot = slot;
    c.gen = m->gt[slot].gen;
    c.d = *d;
    (void)push_cmd(m, &c);
    *out = voice_handle(slot, m->gt[slot].gen);
    return SU_OK;
}

static uint32_t seek_cmd(SuAudioMixer *m, SuAudioVoice v, uint64_t frame) {
    uint32_t slot;
    Cmd c;
    if (!voice_slot(m, v, &slot)) {
        return SU_E_STALE_HANDLE;
    }
    if (frame >= m->gt[slot].limit) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(&c, 0, sizeof c);
    c.type = CMD_SEEK;
    c.slot = slot;
    c.gen = m->gt[slot].gen;
    c.frame = frame;
    return push_cmd(m, &c) ? SU_OK : SU_E_RESOURCE_LIMIT;
}

uint32_t su_audio_seek(SuAudioMixer *m, SuAudioVoice v, uint64_t frame) {
    return seek_cmd(m, v, frame);
}

uint32_t su_audio_seek_seconds(SuAudioMixer *m, SuAudioVoice v, double seconds) {
    uint32_t slot;
    if (!voice_slot(m, v, &slot)) {
        return SU_E_STALE_HANDLE;
    }
    if (!(seconds >= 0.0) || seconds > 1.0e9) {
        return SU_E_INVALID_ARGUMENT;
    }
    return seek_cmd(m, v, (uint64_t)llround(seconds * (double)m->gt[slot].src_rate));
}

uint32_t su_audio_voice_position(const SuAudioMixer *m, SuAudioVoice v, double *frames) {
    uint32_t slot;
    uint64_t bits;
    if (frames == NULL || !voice_slot(m, v, &slot)) {
        return SU_E_STALE_HANDLE;
    }
    bits = atomic_load_explicit(&m->vpos[slot], memory_order_relaxed);
    memcpy(frames, &bits, sizeof *frames);
    return SU_OK;
}

uint32_t su_audio_voice_set_pitch(SuAudioMixer *m, SuAudioVoice v, float pitch) {
    uint32_t slot;
    Cmd c;
    if (!voice_slot(m, v, &slot)) {
        return SU_E_STALE_HANDLE;
    }
    if (!(pitch > 0.0f) || pitch > 64.0f) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(&c, 0, sizeof c);
    c.type = CMD_PITCH;
    c.slot = slot;
    c.gen = m->gt[slot].gen;
    c.a = pitch;
    return push_cmd(m, &c) ? SU_OK : SU_E_RESOURCE_LIMIT;
}

uint32_t su_audio_voice_set_gain(SuAudioMixer *m, SuAudioVoice v, float gain) {
    uint32_t slot;
    Cmd c;
    if (!voice_slot(m, v, &slot)) {
        return SU_E_STALE_HANDLE;
    }
    if (!(gain >= 0.0f) || gain > 1.0e6f) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(&c, 0, sizeof c);
    c.type = CMD_GAIN;
    c.slot = slot;
    c.gen = m->gt[slot].gen;
    c.a = gain;
    return push_cmd(m, &c) ? SU_OK : SU_E_RESOURCE_LIMIT;
}

uint32_t su_audio_voice_update(SuAudioMixer *m, SuAudioVoice v, SuWorldPos pos, SuVec3f vel, float gain) {
    uint32_t slot;
    Cmd c;
    if (!voice_slot(m, v, &slot)) {
        return SU_E_STALE_HANDLE;
    }
    memset(&c, 0, sizeof c);
    c.type = CMD_UPDATE;
    c.slot = slot;
    c.gen = m->gt[slot].gen;
    c.d.pos = pos;
    c.d.vel = vel;
    c.d.gain = gain;
    return push_cmd(m, &c) ? SU_OK : SU_E_RESOURCE_LIMIT;
}

uint32_t su_audio_stop(SuAudioMixer *m, SuAudioVoice v, float fade_s) {
    uint32_t slot;
    Cmd c;
    if (!voice_slot(m, v, &slot)) {
        return SU_E_STALE_HANDLE;
    }
    memset(&c, 0, sizeof c);
    c.type = CMD_STOP;
    c.slot = slot;
    c.gen = m->gt[slot].gen;
    c.a = fade_s > 0.0f ? fade_s : 0.0f;
    return push_cmd(m, &c) ? SU_OK : SU_E_RESOURCE_LIMIT;
}

int su_audio_voice_active(const SuAudioMixer *m, SuAudioVoice v) {
    uint32_t slot;
    return voice_slot(m, v, &slot);
}

uint32_t su_audio_listener(SuAudioMixer *m, SuWorldPos pos, SuQuatf rot, SuVec3f vel) {
    Cmd c;
    if (m == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(&c, 0, sizeof c);
    c.type = CMD_LISTENER;
    c.d.pos = pos;
    c.d.vel = vel;
    c.rot = rot;
    return push_cmd(m, &c) ? SU_OK : SU_E_RESOURCE_LIMIT;
}

uint32_t su_audio_master_gain(SuAudioMixer *m, float gain) {
    Cmd c;
    if (m == NULL || !(gain >= 0.0f)) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(&c, 0, sizeof c);
    c.type = CMD_MASTER;
    c.a = gain;
    return push_cmd(m, &c) ? SU_OK : SU_E_RESOURCE_LIMIT;
}

uint32_t su_audio_master_limiter(SuAudioMixer *m, float knee) {
    Cmd c;
    if (m == NULL || !(knee == 0.0f || (knee >= 0.1f && knee < 1.0f))) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(&c, 0, sizeof c);
    c.type = CMD_LIMIT;
    c.a = knee;
    return push_cmd(m, &c) ? SU_OK : SU_E_RESOURCE_LIMIT;
}

void su_audio_update(SuAudioMixer *m) {
    uint32_t head;
    uint32_t tail;
    if (m == NULL) {
        return;
    }
    head = atomic_load_explicit(&m->end_head, memory_order_relaxed);
    tail = atomic_load_explicit(&m->end_tail, memory_order_acquire);
    while (head != tail) {
        uint32_t slot = m->ended[(head % m->ended_cap) * 2u];
        uint32_t gen = m->ended[(head % m->ended_cap) * 2u + 1u];
        if (slot < m->max_voices && m->gt[slot].gen == gen) {
            m->gt[slot].busy = 0;
        }
        head++;
    }
    atomic_store_explicit(&m->end_head, head, memory_order_release);
}

/* render side */

static void report_end(SuAudioMixer *m, uint32_t slot, uint32_t gen) {
    uint32_t tail = atomic_load_explicit(&m->end_tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&m->end_head, memory_order_acquire);
    if (tail - head < m->ended_cap) {
        m->ended[(tail % m->ended_cap) * 2u] = slot;
        m->ended[(tail % m->ended_cap) * 2u + 1u] = gen;
        atomic_store_explicit(&m->end_tail, tail + 1u, memory_order_release);
    }
    /* full: the game thread is not calling su_audio_update; the slot stays busy until it does (no loss of safety) */
}

static void apply_commands(SuAudioMixer *m) {
    uint32_t head = atomic_load_explicit(&m->cmd_head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&m->cmd_tail, memory_order_acquire);
    while (head != tail) {
        const Cmd *c = &m->cmds[head % m->cmd_cap];
        Voice *v = c->slot < m->max_voices ? &m->v[c->slot] : NULL;
        switch (c->type) {
        case CMD_PLAY:
            if (v->active && v->gen != c->gen) {
                report_end(m, c->slot, v->gen); /* stolen: the old generation ends */
            }
            if (v->active) {
                su_stream_release(v->stream);
            }
            memset(v, 0, sizeof *v);
            v->gen = c->gen;
            v->active = 1;
            v->d = c->d;
            v->fade = 1.0f;
            v->fresh = 1;
            v->pos = (double)c->frame;
            v->ls = c->ls;
            v->le = c->le;
            v->stream = c->d.stream;
            if (v->stream != NULL) {
                v->frames = su_stream_frames(v->stream);
                v->channels = su_stream_channels(v->stream);
                v->src_rate = su_stream_rate(v->stream);
                if ((v->d.flags & SU_AUDIO_LOOP) != 0u) {
                    su_stream_prepare_loop(v->stream, v->ls);
                }
            } else {
                v->frames = v->d.clip->frames;
                v->channels = v->d.clip->channels;
                v->src_rate = v->d.clip->sample_rate;
            }
            break;
        case CMD_SEEK:
            if (v->active && v->gen == c->gen) {
                v->pos = (double)c->frame;
            }
            break;
        case CMD_GAIN:
            if (v->active && v->gen == c->gen) {
                v->d.gain = c->a; /* the per-block gain ramp makes this click-free */
            }
            break;
        case CMD_PITCH:
            if (v->active && v->gen == c->gen) {
                float old = v->pitch_from > 0.0f ? v->d.pitch : (v->d.pitch > 0.0f ? v->d.pitch : 1.0f);
                v->pitch_from = old;
                v->d.pitch = c->a;
            }
            break;
        case CMD_UPDATE:
            if (v->active && v->gen == c->gen) {
                v->d.pos = c->d.pos;
                v->d.vel = c->d.vel;
                v->d.gain = c->d.gain;
            }
            break;
        case CMD_STOP:
            if (v->active && v->gen == c->gen && v->fade_step >= 0.0f) {
                float frames = c->a * (float)m->rate;
                v->fade_step = frames >= 1.0f ? -1.0f / frames : -1.0f; /* immediate: ends after this block's ramp */
            }
            break;
        case CMD_LISTENER:
            m->lpos = c->d.pos;
            m->lvel = c->d.vel;
            m->lrot = c->rot;
            break;
        case CMD_MASTER:
            m->master = c->a;
            break;
        case CMD_LIMIT:
            m->knee = c->a;
            break;
        default:
            break;
        }
        head++;
    }
    atomic_store_explicit(&m->cmd_head, head, memory_order_release);
}

static SuVec3f rotate_inv(SuQuatf q, SuVec3f v) {
    SuQuatf c;
    c.x = -q.x;
    c.y = -q.y;
    c.z = -q.z;
    c.w = q.w;
    return su_quatf_rotate(c, v);
}

static void src_frame(const Voice *v, uint64_t i, float *l, float *r) {
    if (v->stream != NULL) {
        su_stream_frame(v->stream, i, l, r);
    } else if (v->channels == 1u) {
        *l = *r = v->d.clip->samples[i];
    } else {
        *l = v->d.clip->samples[i * 2u];
        *r = v->d.clip->samples[i * 2u + 1u];
    }
}

/* Linearly interpolated sample at pos: mix = 1 gives the mono mix of both channels (spatial voices), else the
 * channels separately (mono sources feed both). The partner of the last frame before the loop end is the loop
 * start; without a loop, the last frame. */
static void voice_sample(const Voice *v, double pos, int loop, int mix, float *sl, float *sr) {
    uint64_t i0 = (uint64_t)pos;
    double fr = pos - (double)i0;
    float l0;
    float r0;
    float l1 = 0.0f;
    float r1 = 0.0f;
    src_frame(v, i0, &l0, &r0);
    if (fr != 0.0) {
        uint64_t i1 = i0 + 1u;
        if (i1 >= (loop ? v->le : v->frames)) {
            i1 = loop ? v->ls : v->frames - 1u;
        }
        src_frame(v, i1, &l1, &r1);
    }
    if (mix) {
        float a = v->channels == 1u ? l0 : 0.5f * (l0 + r0);
        float b = v->channels == 1u ? l1 : 0.5f * (l1 + r1);
        *sl = *sr = fr == 0.0 ? a : a + (b - a) * (float)fr;
    } else {
        *sl = fr == 0.0 ? l0 : l0 + (l1 - l0) * (float)fr;
        *sr = v->channels == 1u ? *sl : (fr == 0.0 ? r0 : r0 + (r1 - r0) * (float)fr);
    }
}

void su_audio_mixer_render(SuAudioMixer *m, float *out, uint32_t frames) {
    uint32_t vi;
    uint32_t i;
    uint32_t active = 0;
    float peak = 0.0f;
    uint32_t clipped = 0;
    if (m == NULL || out == NULL) {
        return;
    }
    apply_commands(m);
    memset(out, 0, (size_t)frames * 2u * sizeof *out);
    for (vi = 0; vi < m->max_voices; vi++) {
        Voice *v = &m->v[vi];
        float gl;
        float gr;
        float rate;
        float dl;
        float dr;
        float pe;
        float pf;
        int pitch_ramp;
        int mono_mix;
        int loop;
        int ended = 0;
        if (!v->active) {
            continue;
        }
        active++;
        loop = (v->d.flags & SU_AUDIO_LOOP) != 0u;
        rate = (float)v->src_rate / (float)m->rate * (v->d.pitch > 0.0f ? v->d.pitch : 1.0f);
        mono_mix = 0;
        if ((v->d.flags & SU_AUDIO_SPATIAL) != 0u) {
            SuVec3f rel = su_wpos_delta(v->d.pos, m->lpos);
            SuVec3f ls = rotate_inv(m->lrot, rel);
            float d = sqrtf(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z);
            float att = su_audio_attenuation(d, v->d.min_distance, v->d.max_distance, v->d.rolloff);
            SuVec3f zero = {0.0f, 0.0f, 0.0f};
            su_audio_pan(ls, &gl, &gr);
            gl *= att * v->d.gain * m->master;
            gr *= att * v->d.gain * m->master;
            rate *= su_audio_doppler(zero, m->lvel, rel, v->d.vel, m->c, v->d.doppler);
            mono_mix = 1;
        } else {
            gl = gr = v->d.gain * m->master;
        }
        if (v->fresh) {
            v->cur_l = gl;
            v->cur_r = gr;
            v->fresh = 0;
        }
        pe = v->d.pitch > 0.0f ? v->d.pitch : 1.0f;
        pf = v->pitch_from > 0.0f ? v->pitch_from : pe;
        pitch_ramp = pf != pe;
        dl = (gl - v->cur_l) / (float)(frames > 0u ? frames : 1u);
        dr = (gr - v->cur_r) / (float)(frames > 0u ? frames : 1u);
        for (i = 0; i < frames; i++) {
            float l = v->cur_l + dl * (float)(i + 1u);
            float r = v->cur_r + dr * (float)(i + 1u);
            float sl;
            float sr;
            float f = v->fade;
            if (v->fade_step == -1.0f) {
                f = 1.0f - (float)(i + 1u) / (float)frames; /* immediate stop: one-block ramp */
            }
            voice_sample(v, v->pos, loop, mono_mix, &sl, &sr);
            out[i * 2u] += sl * l * f;
            out[i * 2u + 1u] += sr * r * f;
            if (v->fade_step < 0.0f && v->fade_step != -1.0f) {
                v->fade += v->fade_step;
                if (v->fade <= 0.0f) {
                    ended = 1;
                    break;
                }
            }
            if (pitch_ramp) {
                /* the playback rate glides from the old pitch to the new one over the block: no phase jump */
                float k = (pf + (pe - pf) * (float)(i + 1u) / (float)frames) / pe;
                v->pos += (double)(rate * k);
            } else {
                v->pos += (double)rate;
            }
            if (v->pos >= (double)(loop ? v->le : v->frames)) {
                if (loop) {
                    v->pos = (double)v->ls + fmod(v->pos - (double)v->ls, (double)(v->le - v->ls));
                } else {
                    ended = 1;
                    break;
                }
            }
        }
        v->pitch_from = 0.0f;
        v->cur_l = gl;
        v->cur_r = gr;
        if (v->fade_step == -1.0f) {
            ended = 1;
        }
        {
            uint64_t pb;
            memcpy(&pb, &v->pos, sizeof pb);
            atomic_store_explicit(&m->vpos[vi], pb, memory_order_relaxed);
        }
        if (ended) {
            v->active = 0;
            su_stream_release(v->stream);
            v->stream = NULL;
            report_end(m, vi, v->gen);
        }
    }
    for (i = 0; i < frames * 2u; i++) {
        float a = fabsf(out[i]);
        peak = a > peak ? a : peak;
        if (m->knee > 0.0f) {
            /* soft limiter: linear up to the knee, then tanh towards 1 (continuous slope, never reaches 1) */
            if (a > 1.0f) {
                clipped++; /* would have clamped */
            }
            if (a > m->knee) {
                float room = 1.0f - m->knee;
                float y = m->knee + room * tanhf((a - m->knee) / room);
                out[i] = out[i] < 0.0f ? -y : y;
            }
        } else if (out[i] > 1.0f) {
            out[i] = 1.0f;
            clipped++;
        } else if (out[i] < -1.0f) {
            out[i] = -1.0f;
            clipped++;
        }
    }
    {
        uint32_t bits;
        memcpy(&bits, &peak, sizeof bits);
        atomic_store_explicit(&m->peak_bits, bits, memory_order_relaxed);
    }
    atomic_store_explicit(&m->clipped, clipped, memory_order_relaxed);
    atomic_store_explicit(&m->stat_active, active, memory_order_relaxed);
    atomic_fetch_add_explicit(&m->frames, frames, memory_order_relaxed);
}

void su_audio_mixer_stats(const SuAudioMixer *m, SuAudioStats *out) {
    if (m == NULL || out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    out->voices_active = atomic_load_explicit(&((SuAudioMixer *)m)->stat_active, memory_order_relaxed);
    out->voices_stolen = m->stolen;
    out->plays_refused = m->refused;
    out->commands_dropped = m->dropped;
    out->frames_rendered = atomic_load_explicit(&((SuAudioMixer *)m)->frames, memory_order_relaxed);
    {
        uint32_t bits = atomic_load_explicit(&((SuAudioMixer *)m)->peak_bits, memory_order_relaxed);
        memcpy(&out->peak, &bits, sizeof bits);
    }
    out->clipped = atomic_load_explicit(&((SuAudioMixer *)m)->clipped, memory_order_relaxed);
}

uint32_t su_audio_mixer_rate(const SuAudioMixer *m) {
    return m != NULL ? m->rate : 0u;
}

/* ---------- host events ---------- */

int su_audio_event_new(SuAudioEventCursor *c, const SuHostEvent *ev) {
    if (c == NULL || ev == NULL || ev->event_seq < c->next_seq) {
        return 0;
    }
    c->next_seq = ev->event_seq + 1u;
    return 1;
}
