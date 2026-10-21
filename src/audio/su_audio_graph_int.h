#ifndef SU_AUDIO_GRAPH_INT_H
#define SU_AUDIO_GRAPH_INT_H

#include "sutekh/audio_parameter.h"

uint32_t su_audio_graph_expand_live(const SuAudioGraph *graph, uint32_t event_id,
    uint64_t seed, uint64_t emitter, uint64_t sequence, const SuAudioGraphExpandState *initial,
    const SuAudioParams *params, SuAudioParamTarget target, uint64_t start, uint64_t now,
    SuAudioGraphLeaf *leaves, size_t capacity, size_t *count, SuAudioGraphDiagnostic *diagnostic);

#endif
