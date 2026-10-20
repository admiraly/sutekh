#ifndef SUTEKH_AUDIO_ASSET_H
#define SUTEKH_AUDIO_ASSET_H

#include "sutekh/audio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t SuAudioAssetId;
typedef uint32_t SuAudioAssetGeneration;
typedef struct SuAudioAssets SuAudioAssets;

typedef struct SuAudioAssetRef {
    SuAudioAssetId id;
    SuAudioAssetGeneration generation;
} SuAudioAssetRef;

typedef struct SuAudioAssetStats {
    uint64_t resident_bytes;
    uint64_t refused_admissions;
} SuAudioAssetStats;

uint32_t su_audio_assets_create(uint32_t capacity, uint64_t max_resident_bytes, SuAudioAssets **out);
uint32_t su_audio_assets_destroy(SuAudioAssets *assets);
uint32_t su_audio_asset_prepare(SuAudioAssets *assets, SuAudioAssetId id, const SuAudioClip *clip,
                               SuAudioAssetRef *out);
uint32_t su_audio_asset_acquire(SuAudioAssets *assets, SuAudioAssetRef ref, const SuAudioClip **out);
uint32_t su_audio_asset_release(SuAudioAssets *assets, SuAudioAssetRef ref);
uint32_t su_audio_asset_unload(SuAudioAssets *assets, SuAudioAssetRef ref);
void su_audio_assets_stats(const SuAudioAssets *assets, SuAudioAssetStats *out);

#ifdef __cplusplus
}
#endif

#endif
