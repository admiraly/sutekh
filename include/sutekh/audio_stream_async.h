#ifndef SUTEKH_AUDIO_STREAM_ASYNC_H
#define SUTEKH_AUDIO_STREAM_ASYNC_H

#include "sutekh/audio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SU_STREAM_OFFLINE_SYNC 0u
#define SU_STREAM_ASYNC 1u

typedef struct SuAudioStreamStatsEx {
    uint64_t underrun_frames;
    uint64_t underrun_reads;
    uint64_t decode_errors;
    uint64_t resident_bytes;
    uint32_t mode;
    uint32_t window_frames;
    uint32_t window_count;
} SuAudioStreamStatsEx;

uint32_t su_audio_stream_mode(SuAudioStream *stream, uint32_t mode);
uint32_t su_audio_stream_prime(SuAudioStream *stream, uint64_t frame, uint32_t timeout_ms);
void su_audio_stream_stats_ex(const SuAudioStream *stream, SuAudioStreamStatsEx *out);

#ifdef __cplusplus
}
#endif

#endif
