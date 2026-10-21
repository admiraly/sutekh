/* Internal device-output helpers: timing histogram bins and underrun de-click fades (not installed). */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef SUTEKH_AUDIO_OUTPUT_H
#define SUTEKH_AUDIO_OUTPUT_H

#include <stddef.h>
#include <stdint.h>
#include "sutekh/audio.h"

#define SU_AUDIO_GAP_FADE 64u

typedef struct SuAudioGap {
    float last[2];
    float tail[2];
    uint32_t missing;
    uint32_t recovering;
    int gap;
} SuAudioGap;

static inline uint32_t su_audio_time_bin(uint64_t us) {
    uint32_t bin = 0u;
    while (us > 1u && bin + 1u < SU_AUDIO_DEVICE_HISTOGRAM_BINS) {
        us >>= 1u;
        bin++;
    }
    return bin;
}

static inline void su_audio_gap_apply(SuAudioGap *state, float *pcm, uint32_t valid, uint32_t frames) {
    uint32_t frame;
    uint32_t channel;
    if (frames == 0u) {
        return;
    }
    if (valid == frames && !state->gap && state->recovering == 0u) {
        state->last[0] = pcm[(size_t)(frames - 1u) * 2u];
        state->last[1] = pcm[(size_t)(frames - 1u) * 2u + 1u];
        return;
    }
    if (valid != 0u && state->gap) {
        state->recovering = SU_AUDIO_GAP_FADE;
        state->tail[0] = state->last[0];
        state->tail[1] = state->last[1];
        state->gap = 0;
    }
    for (frame = 0u; frame < valid; frame++) {
        float gain = state->recovering != 0u
            ? (float)(SU_AUDIO_GAP_FADE - state->recovering + 1u) / (float)SU_AUDIO_GAP_FADE : 1.0f;
        if (state->recovering != 0u) {
            state->recovering--;
        }
        for (channel = 0u; channel < 2u; channel++) {
            float sample = pcm[(size_t)frame * 2u + channel] * gain + state->tail[channel] * (1.0f - gain);
            if (valid >= SU_AUDIO_GAP_FADE && valid < frames && valid - frame <= SU_AUDIO_GAP_FADE) {
                sample *= (float)(valid - frame - 1u) / (float)SU_AUDIO_GAP_FADE;
            }
            pcm[(size_t)frame * 2u + channel] = sample;
            state->last[channel] = pcm[(size_t)frame * 2u + channel];
        }
    }
    if (valid < frames) {
        if (!state->gap) {
            state->missing = 0u;
            state->tail[0] = state->last[0];
            state->tail[1] = state->last[1];
            state->gap = 1;
        }
        for (frame = valid; frame < frames; frame++) {
            float gain;
            if (state->missing < SU_AUDIO_GAP_FADE) {
                state->missing++;
            }
            gain = (float)(SU_AUDIO_GAP_FADE - state->missing) / (float)SU_AUDIO_GAP_FADE;
            for (channel = 0u; channel < 2u; channel++) {
                pcm[(size_t)frame * 2u + channel] = state->tail[channel] * gain;
                state->last[channel] = pcm[(size_t)frame * 2u + channel];
            }
        }
    }
}

#endif
