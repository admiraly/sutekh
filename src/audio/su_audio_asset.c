#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/audio_asset.h"

typedef struct SuAudioAssetSlot {
    SuAudioAssetRef ref;
    SuAudioClip clip;
    uint64_t bytes;
    uint32_t references;
} SuAudioAssetSlot;

struct SuAudioAssets {
    SuAudioAssetSlot *slots;
    uint32_t capacity;
    SuAudioAssetGeneration generation;
    uint64_t max_resident_bytes;
    SuAudioAssetStats stats;
};

static uint32_t refuse(SuAudioAssets *assets, uint32_t status) {
    if (assets != NULL && assets->stats.refused_admissions != UINT64_MAX) {
        assets->stats.refused_admissions++;
    }
    return status;
}

static SuAudioAssetSlot *find_id(SuAudioAssets *assets, SuAudioAssetId id) {
    uint32_t index;
    for (index = 0; index < assets->capacity; index++) {
        if (assets->slots[index].ref.id == id) {
            return &assets->slots[index];
        }
    }
    return NULL;
}

static SuAudioAssetSlot *find_ref(SuAudioAssets *assets, SuAudioAssetRef ref) {
    SuAudioAssetSlot *slot;
    if (assets == NULL || ref.id == 0u || ref.generation == 0u) {
        return NULL;
    }
    slot = find_id(assets, ref.id);
    return slot != NULL && slot->ref.generation == ref.generation ? slot : NULL;
}

uint32_t su_audio_assets_create(uint32_t capacity, uint64_t max_resident_bytes, SuAudioAssets **out) {
    SuAudioAssets *assets;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (capacity == 0u || max_resident_bytes == 0u ||
        (uint64_t)capacity * sizeof(SuAudioAssetSlot) > SIZE_MAX) {
        return SU_E_INVALID_ARGUMENT;
    }
    assets = calloc(1, sizeof *assets);
    if (assets == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    assets->slots = calloc(capacity, sizeof *assets->slots);
    if (assets->slots == NULL) {
        free(assets);
        return SU_E_RESOURCE_LIMIT;
    }
    assets->capacity = capacity;
    assets->max_resident_bytes = max_resident_bytes;
    *out = assets;
    return SU_OK;
}

uint32_t su_audio_assets_destroy(SuAudioAssets *assets) {
    uint32_t index;
    if (assets == NULL) {
        return SU_OK;
    }
    for (index = 0; index < assets->capacity; index++) {
        if (assets->slots[index].references != 0u) {
            return SU_E_RESOURCE_LIMIT;
        }
    }
    for (index = 0; index < assets->capacity; index++) {
        free(assets->slots[index].clip.samples);
    }
    free(assets->slots);
    free(assets);
    return SU_OK;
}

uint32_t su_audio_asset_prepare(SuAudioAssets *assets, SuAudioAssetId id, const SuAudioClip *clip,
                               SuAudioAssetRef *out) {
    SuAudioAssetSlot *slot;
    SuAudioClip copy;
    uint64_t bytes;
    uint64_t retained;
    size_t sample_index;
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (assets == NULL || id == 0u || clip == NULL || out == NULL || clip->samples == NULL ||
        clip->frames == 0u || clip->channels < 1u || clip->channels > 2u ||
        clip->sample_rate < 1000u || clip->sample_rate > 384000u) {
        return refuse(assets, SU_E_INVALID_ARGUMENT);
    }
    bytes = (uint64_t)clip->frames * clip->channels * sizeof(float);
    slot = find_id(assets, id);
    if ((slot != NULL && slot->references != 0u) || assets->generation == UINT32_MAX) {
        return refuse(assets, SU_E_RESOURCE_LIMIT);
    }
    retained = assets->stats.resident_bytes - (slot != NULL ? slot->bytes : 0u);
    if (bytes > SIZE_MAX || bytes > assets->max_resident_bytes - retained) {
        return refuse(assets, SU_E_RESOURCE_LIMIT);
    }
    if (slot == NULL) {
        slot = find_id(assets, 0u);
        if (slot == NULL) {
            return refuse(assets, SU_E_RESOURCE_LIMIT);
        }
    }
    for (sample_index = 0; sample_index < (size_t)bytes / sizeof(float); sample_index++) {
        if (!isfinite(clip->samples[sample_index])) {
            return refuse(assets, SU_E_INVALID_ARGUMENT);
        }
    }
    copy = *clip;
    copy.samples = malloc((size_t)bytes);
    if (copy.samples == NULL) {
        return refuse(assets, SU_E_RESOURCE_LIMIT);
    }
    memcpy(copy.samples, clip->samples, (size_t)bytes);
    free(slot->clip.samples);
    slot->clip = copy;
    slot->bytes = bytes;
    slot->ref.id = id;
    slot->ref.generation = ++assets->generation;
    assets->stats.resident_bytes = retained + bytes;
    *out = slot->ref;
    return SU_OK;
}

uint32_t su_audio_asset_acquire(SuAudioAssets *assets, SuAudioAssetRef ref, const SuAudioClip **out) {
    SuAudioAssetSlot *slot;
    if (out != NULL) {
        *out = NULL;
    }
    if (assets == NULL || out == NULL) {
        return refuse(assets, SU_E_INVALID_ARGUMENT);
    }
    slot = find_ref(assets, ref);
    if (slot == NULL) {
        return refuse(assets, SU_E_STALE_HANDLE);
    }
    if (slot->references == UINT32_MAX) {
        return refuse(assets, SU_E_RESOURCE_LIMIT);
    }
    slot->references++;
    *out = &slot->clip;
    return SU_OK;
}

uint32_t su_audio_asset_release(SuAudioAssets *assets, SuAudioAssetRef ref) {
    SuAudioAssetSlot *slot;
    if (assets == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    slot = find_ref(assets, ref);
    if (slot == NULL) {
        return SU_E_STALE_HANDLE;
    }
    if (slot->references == 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    slot->references--;
    return SU_OK;
}

uint32_t su_audio_asset_unload(SuAudioAssets *assets, SuAudioAssetRef ref) {
    SuAudioAssetSlot *slot;
    if (assets == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    slot = find_ref(assets, ref);
    if (slot == NULL) {
        return SU_E_STALE_HANDLE;
    }
    if (slot->references != 0u) {
        return SU_E_RESOURCE_LIMIT;
    }
    assets->stats.resident_bytes -= slot->bytes;
    free(slot->clip.samples);
    memset(slot, 0, sizeof *slot);
    return SU_OK;
}

void su_audio_assets_stats(const SuAudioAssets *assets, SuAudioAssetStats *out) {
    if (out != NULL) {
        if (assets != NULL) {
            *out = assets->stats;
        } else {
            memset(out, 0, sizeof *out);
        }
    }
}
