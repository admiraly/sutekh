#ifndef SU_AUDIO_CLUSTER_INT_H
#define SU_AUDIO_CLUSTER_INT_H

/* Private interface between clustering, the scene and the integrated event runtime. */

#include "sutekh/audio_cluster.h"
#include "sutekh/audio_spatial.h"

uint32_t su_audio_cluster_capacity(const SuAudioCluster *cluster);
struct SuAudioStateIo;
uint32_t su_audio_cluster_state(SuAudioCluster *cluster, struct SuAudioStateIo *io);
/* Emitters (positions, curves, generations) and the listener; curve tables are configuration. */
uint32_t su_audio_scene_state(SuAudioScene *scene, struct SuAudioStateIo *io);
void su_audio_scene_capacity(const SuAudioScene *scene, uint32_t *emitters, uint32_t *curves, uint32_t *curve_count);

#endif
