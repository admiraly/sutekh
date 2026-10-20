#ifndef SU_AUDIO_PARAMETER_INT_H
#define SU_AUDIO_PARAMETER_INT_H

/* Private interface between parameters/music and the integrated event runtime. */

#include "sutekh/audio_parameter.h"

typedef struct SuAudioBusGain {
    uint32_t bus_id;
    float gain_db;
} SuAudioBusGain;

void su_audio_params_counts(const SuAudioParams *params, uint32_t *param_count, uint32_t *switch_count,
                            uint32_t *bus_binding_count);
/* Voice controls only (gain, pitch, low-pass); bus-gain bindings are skipped. */
uint32_t su_audio_param_voice_controls(const SuAudioParams *params, SuAudioParamTarget target, uint64_t sample_time,
                                       float *gain_db, float *pitch, float *lowpass_hz);
/* Summed BUS_GAIN_DB bindings per distinct bus (global target), in first-binding order. */
uint32_t su_audio_param_bus_gains(const SuAudioParams *params, uint64_t sample_time, SuAudioBusGain *out,
                                  uint32_t capacity, uint32_t *count);

struct SuAudioStateIo;
uint32_t su_audio_params_state(SuAudioParams *params, struct SuAudioStateIo *io);
uint32_t su_audio_music_state_io(SuAudioMusic *music, struct SuAudioStateIo *io);

#endif
