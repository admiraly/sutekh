#ifndef SU_AUDIO_BUS_INT_H
#define SU_AUDIO_BUS_INT_H

/* Private interface between the bus tree and the integrated event runtime. */

#include "sutekh/audio_bus.h"

uint32_t su_audio_bus_count(const SuAudioBusTree *tree);
uint32_t su_audio_bus_max_block(const SuAudioBusTree *tree);
/* Parent ID of a bus (SU_AUDIO_BUS_ROOT for the root). */
uint32_t su_audio_bus_parent(const SuAudioBusTree *tree, SuAudioBusId bus, SuAudioBusId *parent);
/* Copies the bus's processed output (after fader, duck and window, before it is summed into its parent) into
 * dst on every process call; dst holds max_block_frames stereo frames. NULL detaches. */
uint32_t su_audio_bus_capture(SuAudioBusTree *tree, SuAudioBusId bus, float *dst);
struct SuAudioStateIo;
uint32_t su_audio_bus_tree_state(SuAudioBusTree *tree, struct SuAudioStateIo *io);

#endif
