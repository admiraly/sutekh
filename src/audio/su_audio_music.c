#include <stdlib.h>
#include <string.h>

#include "sutekh/audio_parameter.h"

typedef struct MusicSegment {
    uint32_t segment_id;
    uint32_t stem_count;
    SuAudioAssetRef stems[SU_AUDIO_MUSIC_MAX_STEMS];
    const SuAudioClip *clips[SU_AUDIO_MUSIC_MAX_STEMS];
    SuAudioTempoPoint tempo[SU_AUDIO_MUSIC_MAX_TEMPO];
    uint64_t tempo_frame[SU_AUDIO_MUSIC_MAX_TEMPO];
    uint32_t tempo_count;
    uint32_t beats_per_bar;
    uint64_t cues[SU_AUDIO_MUSIC_MAX_CUES];
    uint32_t cue_count;
    uint64_t length;
    uint32_t loop;
} MusicSegment;

/* fade: 0 none, 1 in, 2 out */
typedef struct MusicPlayer {
    uint32_t active;
    uint32_t segment;
    uint64_t start;
    uint32_t fade;
    uint64_t fade_start;
    uint32_t fade_frames;
} MusicPlayer;

typedef struct MusicStinger {
    uint32_t state; /* 0 free, 1 scheduled/playing, 2 finished (pin held until retire) */
    SuAudioAssetRef ref;
    const SuAudioClip *clip;
    uint64_t start;
    float gain;
} MusicStinger;

typedef struct MusicPending {
    uint32_t active;
    uint32_t segment;
    uint64_t at;
    uint32_t fade_in;
    uint32_t fade_out;
    int stinger;
} MusicPending;

struct SuAudioMusic {
    SuAudioMusicDesc desc;
    SuAudioAssets *assets;
    MusicSegment *segments;
    uint32_t segment_count;
    MusicPlayer current;
    MusicPlayer outgoing;
    MusicPending pending;
    MusicStinger stingers[SU_AUDIO_MUSIC_STINGERS];
    uint64_t clock;
};

/* floor(a * b / d) or ceil, with a 128-bit intermediate. Returns 0 on overflow. */
static int muldiv(uint64_t a, uint64_t b, uint64_t d, int round_up, uint64_t *out) {
    uint64_t a_lo = a & 0xFFFFFFFFu;
    uint64_t a_hi = a >> 32;
    uint64_t b_lo = b & 0xFFFFFFFFu;
    uint64_t b_hi = b >> 32;
    uint64_t p0 = a_lo * b_lo;
    uint64_t p1 = a_lo * b_hi;
    uint64_t p2 = a_hi * b_lo;
    uint64_t p3 = a_hi * b_hi;
    uint64_t mid = (p0 >> 32) + (p1 & 0xFFFFFFFFu) + (p2 & 0xFFFFFFFFu);
    uint64_t lo = (p0 & 0xFFFFFFFFu) | (mid << 32);
    uint64_t hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
    uint64_t quotient = 0u;
    uint64_t rem = 0u;
    int bit;
    if (d == 0u || hi >= d) {
        return 0;
    }
    rem = hi;
    for (bit = 63; bit >= 0; bit--) {
        uint64_t top = rem >> 63;
        rem = (rem << 1) | ((lo >> bit) & 1u);
        if (top != 0u || rem >= d) {
            rem -= d;
            quotient |= (uint64_t)1u << bit;
        }
    }
    if (round_up && rem != 0u) {
        if (quotient == UINT64_MAX) {
            return 0;
        }
        quotient++;
    }
    *out = quotient;
    return 1;
}

static uint64_t frames_per_minute(const SuAudioMusic *music) {
    return (uint64_t)60000u * music->desc.sample_rate;
}

static int beat_frame(const SuAudioMusic *music, const MusicSegment *seg, uint64_t beat, uint64_t *out) {
    uint32_t k = seg->tempo_count - 1u;
    uint64_t delta;
    while (k > 0u && seg->tempo[k].beat > beat) {
        k--;
    }
    if (!muldiv(beat - seg->tempo[k].beat, frames_per_minute(music), seg->tempo[k].bpm_milli, 0, &delta)
        || delta > UINT64_MAX - seg->tempo_frame[k]) {
        return 0;
    }
    *out = seg->tempo_frame[k] + delta;
    return 1;
}

/* Smallest multiple-of-step beat whose frame is at or after p. */
static int next_beat(const SuAudioMusic *music, const MusicSegment *seg, uint64_t p, uint64_t step,
                     uint64_t *frame) {
    uint32_t k = seg->tempo_count - 1u;
    uint64_t n;
    uint64_t beat;
    while (k > 0u && seg->tempo_frame[k] > p) {
        k--;
    }
    if (!muldiv(p - seg->tempo_frame[k], seg->tempo[k].bpm_milli, frames_per_minute(music), 1, &n)
        || n > UINT64_MAX - seg->tempo[k].beat) {
        return 0;
    }
    beat = seg->tempo[k].beat + n;
    if (k + 1u < seg->tempo_count && seg->tempo[k + 1u].beat < beat) {
        beat = seg->tempo[k + 1u].beat;
    }
    if (beat % step != 0u) {
        if (beat > UINT64_MAX - step) {
            return 0;
        }
        beat += step - beat % step;
    }
    return beat_frame(music, seg, beat, frame);
}

static MusicSegment *find_segment(const SuAudioMusic *music, uint32_t id, uint32_t *index) {
    uint32_t i;
    for (i = 0u; i < music->segment_count; i++) {
        if (music->segments[i].segment_id == id) {
            if (index != NULL) {
                *index = i;
            }
            return &music->segments[i];
        }
    }
    return NULL;
}

uint32_t su_audio_music_create(const SuAudioMusicDesc *desc, SuAudioAssets *assets, SuAudioMusic **out) {
    SuAudioMusic *music;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (desc == NULL || desc->struct_size != sizeof *desc || assets == NULL || desc->sample_rate < 1000u
        || desc->sample_rate > 384000u || desc->max_segments == 0u || desc->max_segments > 4096u) {
        return SU_E_INVALID_ARGUMENT;
    }
    music = (SuAudioMusic *)calloc(1u, sizeof *music);
    if (music == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    music->segments = (MusicSegment *)calloc(desc->max_segments, sizeof *music->segments);
    if (music->segments == NULL) {
        free(music);
        return SU_E_RESOURCE_LIMIT;
    }
    music->desc = *desc;
    music->assets = assets;
    music->clock = desc->start_sample;
    *out = music;
    return SU_OK;
}

static void release_segment(SuAudioMusic *music, MusicSegment *seg, uint32_t count) {
    uint32_t i;
    for (i = 0u; i < count; i++) {
        (void)su_audio_asset_release(music->assets, seg->stems[i]);
    }
}

static void retire_stingers(SuAudioMusic *music, int all) {
    uint32_t i;
    for (i = 0u; i < SU_AUDIO_MUSIC_STINGERS; i++) {
        if (music->stingers[i].state == 2u || (all && music->stingers[i].state != 0u)) {
            (void)su_audio_asset_release(music->assets, music->stingers[i].ref);
            memset(&music->stingers[i], 0, sizeof music->stingers[i]);
        }
    }
}

void su_audio_music_destroy(SuAudioMusic *music) {
    uint32_t i;
    if (music == NULL) {
        return;
    }
    retire_stingers(music, 1);
    for (i = 0u; i < music->segment_count; i++) {
        release_segment(music, &music->segments[i], music->segments[i].stem_count);
    }
    free(music->segments);
    free(music);
}

uint32_t su_audio_music_segment_add(SuAudioMusic *music, const SuAudioMusicSegmentDesc *desc) {
    MusicSegment *seg;
    uint32_t i;
    if (music == NULL || desc == NULL || desc->struct_size != sizeof *desc || desc->segment_id == 0u
        || desc->stem_count == 0u || desc->stem_count > SU_AUDIO_MUSIC_MAX_STEMS || desc->stems == NULL
        || desc->tempo_count == 0u || desc->tempo_count > SU_AUDIO_MUSIC_MAX_TEMPO || desc->tempo == NULL
        || desc->beats_per_bar == 0u || desc->cue_count > SU_AUDIO_MUSIC_MAX_CUES
        || (desc->cue_count != 0u && desc->cue_frames == NULL) || desc->length_frames == 0u
        || desc->tempo[0].beat != 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (find_segment(music, desc->segment_id, NULL) != NULL) {
        return SU_E_DUPLICATE_KEY;
    }
    if (music->segment_count == music->desc.max_segments) {
        return SU_E_RESOURCE_LIMIT;
    }
    seg = &music->segments[music->segment_count];
    memset(seg, 0, sizeof *seg);
    seg->segment_id = desc->segment_id;
    seg->beats_per_bar = desc->beats_per_bar;
    seg->length = desc->length_frames;
    seg->loop = desc->loop != 0u;
    seg->tempo_count = desc->tempo_count;
    for (i = 0u; i < desc->tempo_count; i++) {
        if (desc->tempo[i].bpm_milli < 1000u || desc->tempo[i].bpm_milli > 1000000u
            || (i > 0u && desc->tempo[i].beat <= desc->tempo[i - 1u].beat)) {
            return SU_E_INVALID_ARGUMENT;
        }
        seg->tempo[i] = desc->tempo[i];
        if (i > 0u) {
            /* Uses only points 0..i-1, which are already in place. */
            seg->tempo_count = i;
            if (!beat_frame(music, seg, desc->tempo[i].beat, &seg->tempo_frame[i])) {
                return SU_E_INVALID_ARGUMENT;
            }
            seg->tempo_count = desc->tempo_count;
        }
    }
    for (i = 0u; i < desc->cue_count; i++) {
        if (desc->cue_frames[i] >= desc->length_frames) {
            return SU_E_INVALID_ARGUMENT;
        }
        seg->cues[i] = desc->cue_frames[i];
    }
    seg->cue_count = desc->cue_count;
    for (i = 0u; i < desc->stem_count; i++) {
        const SuAudioClip *clip = NULL;
        uint32_t status = su_audio_asset_acquire(music->assets, desc->stems[i], &clip);
        if (status == SU_OK && (clip->channels < 1u || clip->channels > 2u
                                || clip->sample_rate != music->desc.sample_rate)) {
            (void)su_audio_asset_release(music->assets, desc->stems[i]);
            status = SU_E_SCHEMA_MISMATCH;
        }
        if (status != SU_OK) {
            release_segment(music, seg, i);
            return status;
        }
        seg->stems[i] = desc->stems[i];
        seg->clips[i] = clip;
    }
    seg->stem_count = desc->stem_count;
    music->segment_count++;
    return SU_OK;
}

uint32_t su_audio_music_beat_frame(const SuAudioMusic *music, uint32_t segment_id, uint64_t beat, uint64_t *out) {
    const MusicSegment *seg;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = 0u;
    if (music == NULL || (seg = find_segment(music, segment_id, NULL)) == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    return beat_frame(music, seg, beat, out) ? SU_OK : SU_E_RESOURCE_LIMIT;
}

static uint64_t fade_end(const MusicPlayer *player) {
    return player->active && player->fade != 0u ? player->fade_start + player->fade_frames : 0u;
}

/* Computes the absolute boundary for a transition requested at t (t >= clock). */
static uint32_t boundary_for(const SuAudioMusic *music, const SuMusicTransition *tr, uint64_t t, uint64_t *at) {
    const MusicSegment *seg;
    uint64_t offset;
    uint64_t base;
    uint64_t p;
    uint64_t local = 0u;
    if (!music->current.active) {
        *at = t;
        return SU_OK;
    }
    seg = &music->segments[music->current.segment];
    if (t < music->current.start) {
        /* The current segment has not begun yet: its boundary is its start. */
        *at = music->current.start;
        return SU_OK;
    }
    offset = t - music->current.start;
    if (seg->loop) {
        p = offset % seg->length;
    } else {
        p = offset;
    }
    base = t - p;
    if (!seg->loop && p >= seg->length) {
        *at = t;
        return SU_OK;
    }
    switch (tr->sync) {
    case SU_AUDIO_MUSIC_IMMEDIATE:
        local = p;
        break;
    case SU_AUDIO_MUSIC_BEAT:
    case SU_AUDIO_MUSIC_BAR:
        if (!next_beat(music, seg, p, tr->sync == SU_AUDIO_MUSIC_BAR ? seg->beats_per_bar : 1u, &local)) {
            return SU_E_RESOURCE_LIMIT;
        }
        if (local > seg->length) {
            local = seg->length;
        }
        break;
    case SU_AUDIO_MUSIC_CUE:
        if (tr->cue >= seg->cue_count) {
            return SU_E_INVALID_ARGUMENT;
        }
        local = seg->cues[tr->cue];
        if (local < p) {
            if (tr->late_policy == SU_AUDIO_MUSIC_LATE_IMMEDIATE) {
                local = p;
            } else if (tr->late_policy == SU_AUDIO_MUSIC_LATE_NEXT && seg->loop) {
                local += seg->length;
            } else {
                return SU_E_STALE_REVISION;
            }
        }
        break;
    case SU_AUDIO_MUSIC_SEGMENT_END:
        local = seg->length;
        break;
    default:
        return SU_E_INVALID_ARGUMENT;
    }
    if (local > UINT64_MAX - base) {
        return SU_E_RESOURCE_LIMIT;
    }
    *at = base + local;
    return SU_OK;
}

uint32_t su_audio_music_transition(SuAudioMusic *music, const SuMusicTransition *transition,
                                   uint64_t sample_time, uint64_t *boundary) {
    uint32_t index;
    uint32_t status;
    uint32_t i;
    uint64_t at = 0u;
    uint64_t earliest;
    int stinger = -1;
    if (boundary != NULL) {
        *boundary = 0u;
    }
    if (music == NULL || transition == NULL || transition->struct_size != sizeof *transition
        || transition->late_policy > SU_AUDIO_MUSIC_LATE_IMMEDIATE
        || find_segment(music, transition->segment_id, &index) == NULL
        || (transition->stinger.id != 0u && !(transition->stinger_gain >= 0.0f && transition->stinger_gain <= 16.0f))) {
        return SU_E_INVALID_ARGUMENT;
    }
    retire_stingers(music, 0);
    if (sample_time < music->clock) {
        if (transition->late_policy == SU_AUDIO_MUSIC_LATE_REJECT) {
            return SU_E_STALE_REVISION;
        }
        sample_time = music->clock;
    }
    earliest = fade_end(&music->current);
    if (fade_end(&music->outgoing) > earliest) {
        earliest = fade_end(&music->outgoing);
    }
    if (sample_time < earliest) {
        sample_time = earliest;
    }
    status = boundary_for(music, transition, sample_time, &at);
    if (status != SU_OK) {
        return status;
    }
    if (transition->stinger.id != 0u) {
        const SuAudioClip *clip = NULL;
        for (i = 0u; i < SU_AUDIO_MUSIC_STINGERS; i++) {
            if (music->stingers[i].state == 0u) {
                stinger = (int)i;
                break;
            }
        }
        if (stinger < 0 && !(music->pending.active && music->pending.stinger >= 0)) {
            return SU_E_RESOURCE_LIMIT;
        }
        if (stinger < 0) {
            stinger = music->pending.stinger;
            (void)su_audio_asset_release(music->assets, music->stingers[stinger].ref);
            memset(&music->stingers[stinger], 0, sizeof music->stingers[stinger]);
            music->pending.stinger = -1;
        }
        status = su_audio_asset_acquire(music->assets, transition->stinger, &clip);
        if (status == SU_OK && (clip->channels < 1u || clip->channels > 2u
                                || clip->sample_rate != music->desc.sample_rate)) {
            (void)su_audio_asset_release(music->assets, transition->stinger);
            status = SU_E_SCHEMA_MISMATCH;
        }
        if (status != SU_OK) {
            return status;
        }
        music->stingers[stinger].ref = transition->stinger;
        music->stingers[stinger].clip = clip;
        music->stingers[stinger].start = at;
        music->stingers[stinger].gain = transition->stinger_gain;
        music->stingers[stinger].state = 1u;
    }
    if (music->pending.active && music->pending.stinger >= 0) {
        MusicStinger *old = &music->stingers[music->pending.stinger];
        (void)su_audio_asset_release(music->assets, old->ref);
        memset(old, 0, sizeof *old);
    }
    music->pending.active = 1u;
    music->pending.segment = index;
    music->pending.at = at;
    music->pending.fade_in = transition->fade_in_frames;
    music->pending.fade_out = transition->fade_out_frames;
    music->pending.stinger = stinger;
    if (boundary != NULL) {
        *boundary = at;
    }
    return SU_OK;
}

static void apply_pending(SuAudioMusic *music) {
    MusicPending *pending = &music->pending;
    if (music->current.active) {
        music->outgoing = music->current;
        if (pending->fade_out == 0u) {
            music->outgoing.active = 0u;
        } else {
            music->outgoing.fade = 2u;
            music->outgoing.fade_start = pending->at;
            music->outgoing.fade_frames = pending->fade_out;
        }
    }
    memset(&music->current, 0, sizeof music->current);
    music->current.active = 1u;
    music->current.segment = pending->segment;
    music->current.start = pending->at;
    if (pending->fade_in != 0u) {
        music->current.fade = 1u;
        music->current.fade_start = pending->at;
        music->current.fade_frames = pending->fade_in;
    }
    pending->active = 0u;
    pending->stinger = -1;
}

static void mix_player(const SuAudioMusic *music, MusicPlayer *player, uint64_t t, float *frame) {
    const MusicSegment *seg;
    uint64_t local;
    double gain = 1.0;
    uint32_t s;
    if (!player->active || t < player->start) {
        return;
    }
    seg = &music->segments[player->segment];
    local = t - player->start;
    if (seg->loop) {
        local %= seg->length;
    } else if (local >= seg->length) {
        player->active = 0u;
        return;
    }
    if (player->fade != 0u) {
        uint64_t elapsed = t - player->fade_start;
        double f = elapsed >= player->fade_frames ? 1.0 : (double)elapsed / (double)player->fade_frames;
        if (player->fade == 1u) {
            gain = f;
            if (elapsed >= player->fade_frames) {
                player->fade = 0u;
            }
        } else {
            if (elapsed >= player->fade_frames) {
                player->active = 0u;
                return;
            }
            gain = 1.0 - f;
        }
    }
    for (s = 0u; s < seg->stem_count; s++) {
        const SuAudioClip *clip = seg->clips[s];
        if (local < clip->frames) {
            const float *x = clip->samples + (size_t)local * clip->channels;
            frame[0] += (float)(x[0] * gain);
            frame[1] += (float)(x[clip->channels - 1u] * gain);
        }
    }
}

uint32_t su_audio_music_render(SuAudioMusic *music, float *out_stereo, uint32_t frames) {
    uint32_t i;
    uint32_t s;
    if (music == NULL || out_stereo == NULL || (uint64_t)frames > UINT64_MAX - music->clock) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (i = 0u; i < frames; i++) {
        uint64_t t = music->clock + i;
        float *frame = out_stereo + (size_t)i * 2u;
        frame[0] = 0.0f;
        frame[1] = 0.0f;
        if (music->pending.active && music->pending.at == t) {
            apply_pending(music);
        }
        mix_player(music, &music->outgoing, t, frame);
        mix_player(music, &music->current, t, frame);
        for (s = 0u; s < SU_AUDIO_MUSIC_STINGERS; s++) {
            MusicStinger *st = &music->stingers[s];
            if (st->state == 1u && t >= st->start && !(music->pending.active && music->pending.stinger == (int)s)) {
                uint64_t local = t - st->start;
                if (local < st->clip->frames) {
                    const float *x = st->clip->samples + (size_t)local * st->clip->channels;
                    frame[0] += x[0] * st->gain;
                    frame[1] += x[st->clip->channels - 1u] * st->gain;
                }
                if (local + 1u >= st->clip->frames) {
                    st->state = 2u;
                }
            }
        }
    }
    music->clock += frames;
    return SU_OK;
}

void su_audio_music_state(const SuAudioMusic *music, SuAudioMusicState *out) {
    uint32_t i;
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (music == NULL) {
        return;
    }
    out->clock = music->clock;
    if (music->current.active) {
        const MusicSegment *seg = &music->segments[music->current.segment];
        out->segment_id = seg->segment_id;
        out->segment_start = music->current.start;
        if (music->clock >= music->current.start) {
            out->segment_frame = music->clock - music->current.start;
            if (seg->loop) {
                out->segment_frame %= seg->length;
            }
        }
    }
    if (music->pending.active) {
        out->pending_segment_id = music->segments[music->pending.segment].segment_id;
        out->pending_at = music->pending.at;
    }
    if (music->outgoing.active) {
        out->fading_segment_id = music->segments[music->outgoing.segment].segment_id;
    }
    for (i = 0u; i < SU_AUDIO_MUSIC_STINGERS; i++) {
        out->stingers += music->stingers[i].state == 1u;
    }
}
