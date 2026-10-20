#ifndef SUTEKH_AUDIO_GRAPH_H
#define SUTEKH_AUDIO_GRAPH_H

#include <stddef.h>
#include <stdint.h>
#include "sutekh/audio_asset.h"
#include "sutekh/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SuAudioGraph SuAudioGraph;
typedef struct SuAudioGraphLimits {
    uint32_t max_nodes;
    uint32_t max_edges;
    uint32_t max_depth;
    uint32_t max_expansion;
    size_t max_json_bytes;
} SuAudioGraphLimits;
typedef struct SuAudioGraphDiagnostic {
    uint32_t code;
    uint32_t node_id;
    size_t offset;
    char bound[32];
    char message[160];
} SuAudioGraphDiagnostic;
typedef struct SuAudioGraphLeaf {
    SuAudioAssetRef asset;
    uint64_t delay_frames;
    float gain;
    float pitch;
    uint32_t node_id;
    uint32_t bus_id;
    uint32_t flags;
} SuAudioGraphLeaf;
typedef struct SuAudioGraphSwitch { uint32_t switch_id; uint32_t value; } SuAudioGraphSwitch;
typedef struct SuAudioGraphParameter { uint32_t parameter_id; float value; } SuAudioGraphParameter;
typedef struct SuAudioGraphHistory {
    uint32_t node_id;
    uint32_t last_child_id;
    uint32_t next_child_id;
} SuAudioGraphHistory;
typedef struct SuAudioGraphExpandState {
    const SuAudioGraphSwitch *switches;
    size_t switch_count;
    const SuAudioGraphParameter *parameters;
    size_t parameter_count;
    SuAudioGraphHistory *history;
    size_t history_count;
} SuAudioGraphExpandState;

uint32_t su_audio_graph_prepare(const void *json, size_t len, SuAudioAssets *assets,
    const SuAudioGraphLimits *limits, SuAudioGraph **out, SuAudioGraphDiagnostic *diagnostic);
uint32_t su_audio_graph_retain(SuAudioGraph *graph);
void su_audio_graph_release(SuAudioGraph *graph);
uint32_t su_audio_graph_sample_rate(const SuAudioGraph *graph);
uint32_t su_audio_graph_expand(const SuAudioGraph *graph, uint32_t event_id,
    uint64_t seed, uint64_t emitter, uint64_t commit_seq, SuAudioGraphLeaf *leaves,
    size_t capacity, size_t *count, SuAudioGraphDiagnostic *diagnostic);
uint32_t su_audio_graph_expand_ex(const SuAudioGraph *graph, uint32_t event_id,
    uint64_t seed, uint64_t emitter, uint64_t commit_seq, const SuAudioGraphExpandState *state,
    SuAudioGraphLeaf *leaves, size_t capacity, size_t *count, SuAudioGraphDiagnostic *diagnostic);

#ifdef __cplusplus
}
#endif
#endif
