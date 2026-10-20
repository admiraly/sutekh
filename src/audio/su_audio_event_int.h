#ifndef SU_AUDIO_EVENT_INT_H
#define SU_AUDIO_EVENT_INT_H

#include "sutekh/audio.h"

uint32_t su_audio_event_voice_release(SuAudioMixer *mixer, SuAudioVoice voice, uint32_t frames);
uint32_t su_audio_event_voice_spatial(SuAudioMixer *mixer, SuAudioVoice voice, const SuAudioVoiceDesc *desc);

#endif
