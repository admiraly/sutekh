#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/audio_asset.h"
#include "sutekh/audio_stream_async.h"
#include "../../src/audio/su_audio_int.h"
#include "../../src/platform/su_platform.h"
#include "../../src/platform/su_thread.h"

static unsigned int checks;
static unsigned int failures;

#ifdef SU_AUDIO_INSTRUMENTED
static _Thread_local int rendering;
static unsigned int callback_side_effects;
static _Atomic int pause_decoder;
static _Atomic int decoder_paused;

static void callback_effect(void) {
    if (rendering) {
        callback_side_effects++;
    }
}

void *su_test_malloc(size_t bytes) {
    callback_effect();
    return malloc(bytes);
}

void *su_test_calloc(size_t count, size_t bytes) {
    callback_effect();
    return calloc(count, bytes);
}

void *su_test_realloc(void *memory, size_t bytes) {
    callback_effect();
    return realloc(memory, bytes);
}

void su_test_free(void *memory) {
    callback_effect();
    free(memory);
}

size_t su_test_fread(void *memory, size_t bytes, size_t count, FILE *file) {
    callback_effect();
    if (!rendering && atomic_load_explicit(&pause_decoder, memory_order_acquire)) {
        atomic_store_explicit(&decoder_paused, 1, memory_order_release);
        while (atomic_load_explicit(&pause_decoder, memory_order_acquire)) {
            su_platform_sleep_ms(1u);
        }
        atomic_store_explicit(&decoder_paused, 0, memory_order_release);
    }
    return fread(memory, bytes, count, file);
}

FILE *su_test_fopen(const char *path, const char *mode) {
    callback_effect();
    return fopen(path, mode);
}

int su_test_fclose(FILE *file) {
    callback_effect();
    return fclose(file);
}

int su_test_fseek(FILE *file, long offset, int origin) {
    callback_effect();
    return fseek(file, offset, origin);
}

#ifdef _WIN32
int su_test_fseeki64(FILE *file, long long offset, int origin) {
    callback_effect();
    return _fseeki64(file, offset, origin);
}
#endif

void su_test_mutex_lock(SuMutex *mutex) {
    callback_effect();
    su_mutex_lock(mutex);
}

void su_test_cond_wait(SuCond *condition, SuMutex *mutex) {
    callback_effect();
    su_cond_wait(condition, mutex);
}

void su_test_sleep_ms(uint32_t milliseconds) {
    callback_effect();
    su_platform_sleep_ms(milliseconds);
}
#endif

static void guarded_render(SuAudioMixer *mixer, float *output, uint32_t frames) {
#ifdef SU_AUDIO_INSTRUMENTED
    rendering = 1;
#endif
    su_audio_mixer_render(mixer, output, frames);
#ifdef SU_AUDIO_INSTRUMENTED
    rendering = 0;
#endif
}

static void guarded_frame(SuAudioStream *stream, uint64_t frame, float *left, float *right) {
#ifdef SU_AUDIO_INSTRUMENTED
    rendering = 1;
#endif
    su_stream_frame(stream, frame, left, right);
#ifdef SU_AUDIO_INSTRUMENTED
    rendering = 0;
#endif
}

#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        failures++; \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while (0)

static void lifetime(void) {
    float samples[] = {0.25f, -0.5f, 0.75f, -1.0f};
    SuAudioClip source = {samples, 2u, 2u, 48000u};
    SuAudioAssets *assets = NULL;
    SuAudioAssetRef first = {0u, 0u};
    SuAudioAssetRef second = {0u, 0u};
    SuAudioAssetRef failed = {9u, 9u};
    const SuAudioClip *clip = NULL;
    const SuAudioClip *also = NULL;
    SuAudioAssetStats stats;
    CHECK(su_audio_assets_create(1u, sizeof samples, &assets) == SU_OK);
    if (assets == NULL) {
        return;
    }
    CHECK(su_audio_asset_prepare(assets, 42u, &source, &first) == SU_OK);
    CHECK(first.id == 42u && first.generation != 0u);
    CHECK(su_audio_asset_acquire(assets, first, &clip) == SU_OK);
    CHECK(su_audio_asset_acquire(assets, first, &also) == SU_OK);
    CHECK(clip == also && clip != NULL);
    if (clip != NULL) {
        CHECK(clip->samples != samples);
        CHECK(memcmp(clip->samples, samples, sizeof samples) == 0);
        CHECK(clip->frames == 2u && clip->channels == 2u && clip->sample_rate == 48000u);
        samples[0] = 0.5f;
        CHECK(clip->samples[0] == 0.25f);
    }
    CHECK(su_audio_asset_prepare(assets, 42u, &source, &failed) == SU_E_RESOURCE_LIMIT);
    CHECK(failed.id == 0u && failed.generation == 0u);
    CHECK(su_audio_asset_unload(assets, first) == SU_E_RESOURCE_LIMIT);
    CHECK(su_audio_assets_destroy(assets) == SU_E_RESOURCE_LIMIT);
    CHECK(su_audio_asset_release(assets, first) == SU_OK);
    CHECK(su_audio_asset_unload(assets, first) == SU_E_RESOURCE_LIMIT);
    CHECK(su_audio_asset_release(assets, first) == SU_OK);
    CHECK(su_audio_asset_release(assets, first) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_asset_prepare(assets, 42u, &source, &second) == SU_OK);
    CHECK(second.id == first.id && second.generation != first.generation);
    CHECK(su_audio_asset_acquire(assets, first, &clip) == SU_E_STALE_HANDLE && clip == NULL);
    CHECK(su_audio_asset_release(assets, first) == SU_E_STALE_HANDLE);
    CHECK(su_audio_asset_unload(assets, first) == SU_E_STALE_HANDLE);
    CHECK(su_audio_asset_acquire(assets, second, &clip) == SU_OK);
    CHECK(clip != NULL && clip->samples[0] == 0.5f);
    CHECK(su_audio_asset_release(assets, second) == SU_OK);
    CHECK(su_audio_asset_unload(assets, second) == SU_OK);
    CHECK(su_audio_asset_acquire(assets, second, &clip) == SU_E_STALE_HANDLE && clip == NULL);
    CHECK(su_audio_asset_prepare(assets, 7u, &source, &second) == SU_OK);
    CHECK(second.id == 7u && second.generation != first.generation);
    CHECK(su_audio_asset_unload(assets, second) == SU_OK);
    CHECK(su_audio_asset_prepare(assets, 42u, &source, &second) == SU_OK);
    CHECK(second.generation > first.generation + 1u);
    su_audio_assets_stats(assets, &stats);
    CHECK(stats.resident_bytes == sizeof samples && stats.refused_admissions == 3u);
    printf("asset PCM: %llu bytes, capacity 1, refused admissions %llu\n",
           (unsigned long long)stats.resident_bytes, (unsigned long long)stats.refused_admissions);
    CHECK(su_audio_assets_destroy(assets) == SU_OK);
}

static void limits(void) {
    float samples[8] = {0};
    SuAudioClip source = {samples, 4u, 1u, 48000u};
    SuAudioAssets *assets = NULL;
    SuAudioAssetRef first;
    SuAudioAssetRef second;
    SuAudioAssetRef failed;
    SuAudioAssetStats stats;
    const SuAudioClip *clip = NULL;
    CHECK(su_audio_assets_create(2u, 16u, &assets) == SU_OK);
    if (assets == NULL) {
        return;
    }
    CHECK(su_audio_asset_prepare(assets, 1u, &source, &first) == SU_OK);
    CHECK(su_audio_asset_prepare(assets, 2u, &source, &failed) == SU_E_RESOURCE_LIMIT);
    source.frames = 8u;
    CHECK(su_audio_asset_prepare(assets, 1u, &source, &failed) == SU_E_RESOURCE_LIMIT);
    CHECK(su_audio_asset_acquire(assets, first, &clip) == SU_OK);
    CHECK(clip != NULL && clip->frames == 4u);
    CHECK(su_audio_asset_release(assets, first) == SU_OK);
    source.frames = 2u;
    CHECK(su_audio_asset_prepare(assets, 1u, &source, &first) == SU_OK);
    CHECK(su_audio_asset_prepare(assets, 2u, &source, &second) == SU_OK);
    source.frames = 1u;
    CHECK(su_audio_asset_prepare(assets, 3u, &source, &failed) == SU_E_RESOURCE_LIMIT);
    CHECK(su_audio_asset_unload(assets, first) == SU_OK);
    CHECK(su_audio_asset_prepare(assets, 3u, &source, &first) == SU_OK);
    su_audio_assets_stats(assets, &stats);
    CHECK(stats.resident_bytes == 12u && stats.refused_admissions == 3u);
    CHECK(su_audio_assets_destroy(assets) == SU_OK);
}

static void invalid(void) {
    float sample = 0.25f;
    SuAudioClip source = {&sample, 1u, 1u, 48000u};
    SuAudioAssets *assets = NULL;
    SuAudioAssetRef ref = {0u, 0u};
    const SuAudioClip *clip = NULL;
    SuAudioAssetStats stats = {9u, 9u};
    CHECK(su_audio_assets_create(0u, 4u, &assets) == SU_E_INVALID_ARGUMENT && assets == NULL);
    CHECK(su_audio_assets_create(1u, 0u, &assets) == SU_E_INVALID_ARGUMENT && assets == NULL);
    CHECK(su_audio_assets_create(1u, 4u, NULL) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_assets_create(1u, 4u, &assets) == SU_OK);
    if (assets == NULL) {
        return;
    }
    CHECK(su_audio_asset_prepare(NULL, 1u, &source, &ref) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_asset_prepare(assets, 0u, &source, &ref) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_asset_prepare(assets, 1u, NULL, &ref) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_asset_prepare(assets, 1u, &source, NULL) == SU_E_INVALID_ARGUMENT);
    source.samples = NULL;
    CHECK(su_audio_asset_prepare(assets, 1u, &source, &ref) == SU_E_INVALID_ARGUMENT);
    source.samples = &sample;
    source.frames = 0u;
    CHECK(su_audio_asset_prepare(assets, 1u, &source, &ref) == SU_E_INVALID_ARGUMENT);
    source.frames = 1u;
    source.channels = 3u;
    CHECK(su_audio_asset_prepare(assets, 1u, &source, &ref) == SU_E_INVALID_ARGUMENT);
    source.channels = 1u;
    source.sample_rate = 999u;
    CHECK(su_audio_asset_prepare(assets, 1u, &source, &ref) == SU_E_INVALID_ARGUMENT);
    source.sample_rate = 384001u;
    CHECK(su_audio_asset_prepare(assets, 1u, &source, &ref) == SU_E_INVALID_ARGUMENT);
    source.sample_rate = 48000u;
    CHECK(su_audio_asset_acquire(NULL, ref, &clip) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_asset_acquire(assets, ref, NULL) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_asset_acquire(assets, ref, &clip) == SU_E_STALE_HANDLE && clip == NULL);
    CHECK(su_audio_asset_release(NULL, ref) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_asset_unload(NULL, ref) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_asset_prepare(assets, 1u, &source, &ref) == SU_OK);
    CHECK(su_audio_asset_release(assets, ref) == SU_E_INVALID_ARGUMENT);
    su_audio_assets_stats(NULL, &stats);
    CHECK(stats.resident_bytes == 0u && stats.refused_admissions == 0u);
    su_audio_assets_stats(assets, NULL);
    CHECK(su_audio_assets_destroy(assets) == SU_OK);
    CHECK(su_audio_assets_destroy(NULL) == SU_OK);
}

static void resident_render(void) {
    float samples[] = {0.25f, -0.25f, 0.5f, -0.5f};
    float output[4] = {0};
    SuAudioClip source = {samples, 2u, 2u, 48000u};
    SuAudioAssets *assets = NULL;
    SuAudioAssetRef ref;
    const SuAudioClip *clip = NULL;
    SuAudioMixer *mixer = NULL;
    SuAudioMixerDesc desc;
    SuAudioVoiceDesc voice_desc;
    SuAudioVoice voice;
    memset(&desc, 0, sizeof desc);
    memset(&voice_desc, 0, sizeof voice_desc);
    desc.struct_size = sizeof desc;
    desc.sample_rate = 48000u;
    desc.max_voices = 1u;
    desc.command_capacity = 8u;
    CHECK(su_audio_assets_create(1u, sizeof samples, &assets) == SU_OK);
    CHECK(su_audio_mixer_create(&desc, &mixer) == SU_OK);
    if (assets == NULL || mixer == NULL) {
        su_audio_mixer_destroy(mixer);
        CHECK(su_audio_assets_destroy(assets) == SU_OK);
        return;
    }
    CHECK(su_audio_asset_prepare(assets, 101u, &source, &ref) == SU_OK);
    CHECK(su_audio_asset_acquire(assets, ref, &clip) == SU_OK);
    voice_desc.clip = clip;
    voice_desc.gain = 1.0f;
    CHECK(voice_desc.stream == NULL);
    CHECK(su_audio_play(mixer, &voice_desc, &voice) == SU_OK);
    memset(samples, 0, sizeof samples);
    CHECK(su_audio_asset_unload(assets, ref) == SU_E_RESOURCE_LIMIT);
    guarded_render(mixer, output, 2u);
    CHECK(output[0] == 0.25f && output[1] == -0.25f);
    CHECK(output[2] == 0.5f && output[3] == -0.5f);
    guarded_render(mixer, output, 1u);
    su_audio_update(mixer);
    CHECK(!su_audio_voice_active(mixer, voice));
    CHECK(su_audio_asset_release(assets, ref) == SU_OK);
    CHECK(su_audio_asset_unload(assets, ref) == SU_OK);
    su_audio_mixer_destroy(mixer);
    CHECK(su_audio_assets_destroy(assets) == SU_OK);
}

static void finite_pcm(void) {
    float samples[] = {0.25f, -0.25f};
    const float bad[] = {NAN, INFINITY, -INFINITY};
    SuAudioClip source = {samples, 2u, 1u, 48000u};
    SuAudioAssets *assets = NULL;
    SuAudioAssetRef original;
    SuAudioAssetRef refused;
    const SuAudioClip *clip = NULL;
    size_t index;
    CHECK(su_audio_assets_create(2u, 32u, &assets) == SU_OK);
    if (assets == NULL) {
        return;
    }
    CHECK(su_audio_asset_prepare(assets, 1u, &source, &original) == SU_OK);
    for (index = 0; index < sizeof bad / sizeof bad[0]; index++) {
        samples[1] = bad[index];
        CHECK(su_audio_asset_prepare(assets, 2u, &source, &refused) == SU_E_INVALID_ARGUMENT);
        CHECK(refused.id == 0u && refused.generation == 0u);
        CHECK(su_audio_asset_prepare(assets, 1u, &source, &refused) == SU_E_INVALID_ARGUMENT);
        CHECK(refused.id == 0u && refused.generation == 0u);
        CHECK(su_audio_asset_acquire(assets, original, &clip) == SU_OK);
        CHECK(clip != NULL && clip->samples[0] == 0.25f && clip->samples[1] == -0.25f);
        CHECK(su_audio_asset_release(assets, original) == SU_OK);
    }
    CHECK(su_audio_assets_destroy(assets) == SU_OK);
}

static void put32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static void fractional_demand(void) {
    uint8_t wav[44u + 8192u * 2u] = {0};
    SuAudioStream *stream = NULL;
    SuAudioStreamStatsEx stats;
    SuAudioStreamInfo info;
    float left;
    float right;
    uint32_t index;
    uint32_t phase;
    uint32_t recovered = 0u;
    char message[256];
    memcpy(wav, "RIFF", 4u);
    put32(wav + 4u, sizeof wav - 8u);
    memcpy(wav + 8u, "WAVEfmt ", 8u);
    put32(wav + 16u, 16u);
    wav[20] = 1u;
    wav[22] = 1u;
    put32(wav + 24u, 48000u);
    put32(wav + 28u, 96000u);
    wav[32] = 2u;
    wav[34] = 16u;
    memcpy(wav + 36u, "data", 4u);
    put32(wav + 40u, sizeof wav - 44u);
    for (index = 0u; index < 8192u; index++) {
        wav[45u + index * 2u] = 32u;
    }
    CHECK(su_audio_stream_open_memory(wav, sizeof wav, 2048u, &stream, message) == SU_OK);
    if (stream == NULL) {
        return;
    }
    CHECK(su_audio_stream_mode(stream, SU_STREAM_ASYNC) == SU_OK);
    CHECK(su_audio_stream_prime(stream, 0u, 2000u) == SU_OK);
    for (index = 0u; index < 2000u; index++) {
        su_audio_stream_info(stream, &info);
        if (info.frames_decoded >= 2048u) {
            break;
        }
        su_platform_sleep_ms(1u);
    }
    CHECK(info.frames_decoded >= 2048u);
    for (phase = 0u; phase < 2u; phase++) {
        uint64_t primary = phase == 0u ? 1023u : 3071u;
        uint64_t partner = phase == 0u ? 1024u : 7u;
        if (phase != 0u) {
            su_stream_prepare_loop(stream, partner);
            CHECK(su_audio_stream_prime(stream, primary, 2000u) == SU_OK);
        }
        for (index = 0u; index < 8u; index++) {
            su_stream_sample_begin(stream);
            guarded_frame(stream, primary, &left, &right);
            CHECK(left == 0.25f && right == 0.25f);
            su_platform_sleep_ms(10u);
            guarded_frame(stream, partner, &left, &right);
            CHECK(left == 0.25f && right == 0.25f);
            CHECK(su_stream_sample_missed(stream) == 0);
            su_platform_sleep_ms(10u);
        }
    }
    su_audio_stream_stats_ex(stream, &stats);
    CHECK(stats.underrun_frames == 0u && stats.underrun_reads == 0u);
    CHECK(su_audio_stream_prime(stream, 4096u, 2000u) == SU_OK);
    CHECK(su_audio_stream_prime(stream, 1024u, 2000u) == SU_OK);
    su_stream_sample_begin(stream);
    guarded_frame(stream, 1023u, &left, &right);
    CHECK(left == 0.0f && right == 0.0f);
    guarded_frame(stream, 1024u, &left, &right);
    CHECK(su_stream_sample_missed(stream) != 0);
    for (index = 0u; index < 2000u && recovered < 8u; index++) {
        int ready;
        su_platform_sleep_ms(10u);
        su_stream_sample_begin(stream);
        guarded_frame(stream, 1023u, &left, &right);
        ready = left == 0.25f && right == 0.25f;
        guarded_frame(stream, 1024u, &left, &right);
        ready = ready && left == 0.25f && right == 0.25f;
        if (su_stream_sample_missed(stream) == 0 && ready) {
            recovered++;
        } else {
            recovered = 0u;
        }
    }
    CHECK(recovered == 8u);
    guarded_frame(stream, 7000u, &left, &right);
    CHECK(left == 0.0f && right == 0.0f);
    for (index = 0u; index < 2000u; index++) {
        su_platform_sleep_ms(1u);
        guarded_frame(stream, 7000u, &left, &right);
        if (left == 0.25f && right == 0.25f) {
            break;
        }
    }
    CHECK(index < 2000u);
    CHECK(su_stream_sample_missed(stream) != 0);
    CHECK(su_audio_stream_close(stream) == SU_OK);
    printf("fractional demand: adjacent-window and loop-head partners retain primary across worker progress\n");
}

static void async_stream(void) {
    uint8_t wav[44u + 32768u * 2u] = {0};
    SuAudioStream *stream = NULL;
    SuAudioStream *file_stream = NULL;
    SuAudioClip *reference = NULL;
    SuAudioStreamInfo info;
    SuAudioStreamStatsEx stats;
    SuAudioStreamStatsEx initial;
    SuAudioMixerDesc desc;
    SuAudioVoiceDesc voice_desc;
    SuAudioMixer *mixer = NULL;
    SuAudioVoice voice;
    double position = 0.0;
    float output[128];
    float left;
    float right;
    uint32_t index;
    char message[256];
    FILE *file;
    memcpy(wav, "RIFF", 4u);
    put32(wav + 4u, sizeof wav - 8u);
    memcpy(wav + 8u, "WAVEfmt ", 8u);
    put32(wav + 16u, 16u);
    wav[20] = 1u;
    wav[22] = 1u;
    put32(wav + 24u, 48000u);
    put32(wav + 28u, 96000u);
    wav[32] = 2u;
    wav[34] = 16u;
    memcpy(wav + 36u, "data", 4u);
    put32(wav + 40u, sizeof wav - 44u);
    for (index = 0; index < 32768u; index++) {
        uint32_t sample = 1000u + index % 10000u;
        wav[44u + index * 2u] = (uint8_t)sample;
        wav[45u + index * 2u] = (uint8_t)(sample >> 8);
    }
    CHECK(su_audio_clip_from_wav(wav, sizeof wav, &reference, message) == SU_OK);
    CHECK(su_audio_stream_open_memory(wav, sizeof wav, 2048u, &stream, message) == SU_OK);
    if (reference == NULL || stream == NULL) {
        su_audio_clip_free(reference);
        CHECK(su_audio_stream_close(stream) == SU_OK);
        return;
    }
    CHECK(su_audio_stream_mode(NULL, SU_STREAM_ASYNC) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_stream_mode(stream, 99u) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_stream_prime(stream, 0u, 0u) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_stream_mode(stream, SU_STREAM_ASYNC) == SU_OK);
    CHECK(su_audio_stream_mode(stream, SU_STREAM_ASYNC) == SU_OK);
    CHECK(su_audio_stream_prime(stream, 32768u, 1u) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_stream_prime(stream, 0u, 2000u) == SU_OK);
    su_audio_stream_stats_ex(stream, &initial);
    su_audio_stream_info(stream, &info);
    CHECK(initial.mode == SU_STREAM_ASYNC && initial.window_count == 2u && initial.window_frames == 1024u);
    CHECK(info.ring_bytes == 8192u && initial.resident_bytes == info.resident_bytes);
    for (index = 0; index < 32768u; index += 1024u) {
        uint32_t offset;
        CHECK(su_audio_stream_prime(stream, index, 2000u) == SU_OK);
        for (offset = 0; offset < 1024u; offset++) {
            guarded_frame(stream, index + offset, &left, &right);
            CHECK(left == reference->samples[index + offset] && left == right);
        }
    }
    su_audio_stream_stats_ex(stream, &stats);
    CHECK(stats.resident_bytes == initial.resident_bytes && stats.decode_errors == 0u);
    CHECK(stats.underrun_frames == 0u && stats.underrun_reads == 0u);
    CHECK(su_audio_stream_prime(stream, 0u, 2000u) == SU_OK);
    memset(&desc, 0, sizeof desc);
    memset(&voice_desc, 0, sizeof voice_desc);
    desc.struct_size = sizeof desc;
    desc.sample_rate = 48000u;
    desc.max_voices = 1u;
    desc.command_capacity = 8u;
    CHECK(su_audio_mixer_create(&desc, &mixer) == SU_OK);
    if (mixer != NULL) {
        voice_desc.stream = stream;
        voice_desc.gain = 1.0f;
        voice_desc.start_frame = 20000u;
        CHECK(su_audio_play(mixer, &voice_desc, &voice) == SU_OK);
        CHECK(su_audio_stream_mode(stream, SU_STREAM_OFFLINE_SYNC) == SU_E_INVALID_ARGUMENT);
        CHECK(su_audio_stream_prime(stream, 20000u, 1u) == SU_E_INVALID_ARGUMENT);
        CHECK(su_audio_stream_close(stream) == SU_E_INVALID_ARGUMENT);
        guarded_render(mixer, output, 1u);
        CHECK(output[0] == 0.0f && output[1] == 0.0f);
        CHECK(su_audio_voice_position(mixer, voice, &position) == SU_OK && position == 20001.0);
        su_audio_stream_stats_ex(stream, &stats);
        CHECK(stats.underrun_frames >= 1u);
        su_audio_mixer_destroy(mixer);
        mixer = NULL;
    }
    CHECK(su_audio_stream_prime(stream, 20001u, 2000u) == SU_OK);
    CHECK(su_audio_mixer_create(&desc, &mixer) == SU_OK);
    if (mixer != NULL) {
        voice_desc.start_frame = 20001u;
        CHECK(su_audio_play(mixer, &voice_desc, &voice) == SU_OK);
        guarded_render(mixer, output, 64u);
        for (index = 0; index < 64u; index++) {
            CHECK(output[index * 2u] == reference->samples[20001u + index]);
            CHECK(output[index * 2u] == output[index * 2u + 1u]);
        }
        CHECK(su_audio_seek(mixer, voice, 15000u) == SU_OK);
        guarded_render(mixer, output, 1u);
        CHECK(output[0] == 0.0f && output[1] == 0.0f);
        CHECK(su_audio_voice_position(mixer, voice, &position) == SU_OK && position == 15001.0);
        su_audio_mixer_destroy(mixer);
        mixer = NULL;
    }
    CHECK(su_audio_stream_prime(stream, 15001u, 2000u) == SU_OK);
    guarded_frame(stream, 15001u, &left, &right);
    CHECK(left == reference->samples[15001u] && right == left);
    su_stream_prepare_loop(stream, 7u);
    CHECK(su_audio_stream_prime(stream, 15001u, 2000u) == SU_OK);
    guarded_frame(stream, 7u, &left, &right);
    CHECK(left == reference->samples[7u] && right == left);
    CHECK(su_audio_stream_prime(stream, 20000u, 2000u) == SU_OK);
    CHECK(su_audio_mixer_create(&desc, &mixer) == SU_OK);
    if (mixer != NULL) {
        voice_desc.start_frame = 20009u;
        voice_desc.loop_start = 7u;
        voice_desc.loop_end = 20010u;
        voice_desc.flags = SU_AUDIO_LOOP;
        voice_desc.pitch = 0.5f;
        CHECK(su_audio_play(mixer, &voice_desc, &voice) == SU_OK);
        guarded_render(mixer, output, 2u);
        CHECK(output[0] == reference->samples[20009u]);
        CHECK(output[2] == 0.5f * (reference->samples[20009u] + reference->samples[7u]));
        su_audio_mixer_destroy(mixer);
        mixer = NULL;
    }
    su_audio_stream_stats_ex(stream, &stats);
    CHECK(stats.resident_bytes == initial.resident_bytes && stats.decode_errors == 0u);
    printf("async WAV: ring %llu bytes, %u windows x %u frames, resident %llu bytes, underrun frames %llu, reads %llu\n",
           (unsigned long long)info.ring_bytes, stats.window_count, stats.window_frames,
           (unsigned long long)stats.resident_bytes, (unsigned long long)stats.underrun_frames,
           (unsigned long long)stats.underrun_reads);
    CHECK(su_audio_stream_mode(stream, SU_STREAM_OFFLINE_SYNC) == SU_OK);
    su_stream_frame(stream, 30000u, &left, &right);
    CHECK(left == reference->samples[30000u] && right == left);
    CHECK(su_audio_stream_mode(stream, SU_STREAM_ASYNC) == SU_OK);
    CHECK(su_audio_stream_prime(stream, 12345u, 2000u) == SU_OK);
    guarded_frame(stream, 12345u, &left, &right);
    CHECK(left == reference->samples[12345u] && right == left);
    CHECK(su_audio_stream_close(stream) == SU_OK);
    file = fopen("test_audio_asset_synthetic.wav", "wb");
    CHECK(file != NULL);
    if (file != NULL) {
        CHECK(fwrite(wav, 1u, sizeof wav, file) == sizeof wav);
        CHECK(fclose(file) == 0);
        CHECK(su_audio_stream_open_file("test_audio_asset_synthetic.wav", 2048u, &file_stream, message) == SU_OK);
        if (file_stream != NULL) {
#ifdef SU_AUDIO_INSTRUMENTED
            CHECK(callback_side_effects == 0u);
            guarded_frame(file_stream, 30000u, &left, &right);
            CHECK(callback_side_effects > 0u);
            printf("instrumentation positive control: legacy synchronous file reads/seeks %u\n", callback_side_effects);
            callback_side_effects = 0u;
#endif
            CHECK(su_audio_stream_mode(file_stream, SU_STREAM_ASYNC) == SU_OK);
            CHECK(su_audio_stream_prime(file_stream, 30000u, 2000u) == SU_OK);
            guarded_frame(file_stream, 30000u, &left, &right);
            CHECK(left == reference->samples[30000u] && right == left);
#ifdef SU_AUDIO_INSTRUMENTED
            CHECK(su_audio_stream_prime(file_stream, 20000u, 2000u) == SU_OK);
            CHECK(su_audio_mixer_create(&desc, &mixer) == SU_OK);
            if (mixer != NULL) {
                atomic_store_explicit(&pause_decoder, 1, memory_order_release);
                voice_desc.stream = file_stream;
                voice_desc.start_frame = 20009u;
                voice_desc.loop_start = 7u;
                voice_desc.loop_end = 20010u;
                voice_desc.flags = SU_AUDIO_LOOP;
                voice_desc.pitch = 0.5f;
                CHECK(su_audio_play(mixer, &voice_desc, &voice) == SU_OK);
                guarded_render(mixer, output, 0u);
                for (index = 0u; index < 2000u; index++) {
                    if (atomic_load_explicit(&decoder_paused, memory_order_acquire)) {
                        break;
                    }
                    su_platform_sleep_ms(1u);
                }
                CHECK(atomic_load_explicit(&decoder_paused, memory_order_acquire) != 0);
                guarded_render(mixer, output, 2u);
                CHECK(output[0] == reference->samples[20009u] && output[1] == output[0]);
                CHECK(output[2] == 0.0f && output[3] == 0.0f);
                CHECK(su_audio_voice_position(mixer, voice, &position) == SU_OK && position == 7.0);
                su_audio_stream_stats_ex(file_stream, &stats);
                CHECK(stats.underrun_frames == 1u && stats.underrun_reads == 1u);
                printf("paused decoder: missing interpolation partner silenced both channels; 1 output underrun frame\n");
                atomic_store_explicit(&pause_decoder, 0, memory_order_release);
                su_audio_mixer_destroy(mixer);
                mixer = NULL;
            }
#endif
            CHECK(su_audio_stream_close(file_stream) == SU_OK);
        }
        CHECK(remove("test_audio_asset_synthetic.wav") == 0);
    }
    su_audio_clip_free(reference);
}

static void async_ogg(const char *path) {
    FILE *file = fopen(path, "rb");
    uint8_t *bytes = NULL;
    long length = 0;
    SuAudioClip *clip = NULL;
    SuAudioStream *stream = NULL;
    SuAudioStreamStatsEx stats;
    char message[256];
    uint32_t frame;
    CHECK(file != NULL);
    if (file == NULL) {
        return;
    }
    CHECK(fseek(file, 0, SEEK_END) == 0);
    length = ftell(file);
    CHECK(length > 0 && fseek(file, 0, SEEK_SET) == 0);
    if (length > 0) {
        bytes = malloc((size_t)length);
    }
    CHECK(bytes != NULL);
    if (bytes != NULL) {
        CHECK(fread(bytes, 1u, (size_t)length, file) == (size_t)length);
        CHECK(su_audio_clip_from_ogg(bytes, (size_t)length, &clip, message) == SU_OK);
        CHECK(su_audio_stream_open_memory(bytes, (size_t)length, 2048u, &stream, message) == SU_OK);
        if (clip != NULL && stream != NULL) {
            CHECK(su_audio_stream_mode(stream, SU_STREAM_ASYNC) == SU_OK);
            for (frame = 0u; frame < clip->frames; frame += 1024u) {
                uint32_t offset;
                CHECK(su_audio_stream_prime(stream, frame, 2000u) == SU_OK);
                for (offset = 0u; offset < 1024u && frame + offset < clip->frames; offset++) {
                    float left;
                    float right;
                    size_t sample = (size_t)(frame + offset) * clip->channels;
                    guarded_frame(stream, frame + offset, &left, &right);
                    CHECK(left == clip->samples[sample]);
                    CHECK(right == clip->samples[sample + (clip->channels == 2u ? 1u : 0u)]);
                }
            }
            su_audio_stream_stats_ex(stream, &stats);
            CHECK(stats.decode_errors == 0u && stats.underrun_frames == 0u);
            printf("async Ogg: %u frames sample-identical, resident %llu bytes\n", clip->frames,
                   (unsigned long long)stats.resident_bytes);
        }
        CHECK(su_audio_stream_close(stream) == SU_OK);
        su_audio_clip_free(clip);
        free(bytes);
    }
    CHECK(fclose(file) == 0);
}

int main(int argc, char **argv) {
    lifetime();
    limits();
    invalid();
    resident_render();
    finite_pcm();
    async_stream();
    fractional_demand();
    if (argc > 1 && su_audio_ogg_available()) {
        async_ogg(argv[1]);
    }
#ifdef SU_AUDIO_INSTRUMENTED
    CHECK(callback_side_effects == 0u);
    printf("instrumented callback allocations/frees, file calls, mutex/condition waits, sleeps: %u\n",
           callback_side_effects);
#endif
    printf("asset ABI bytes: id %zu, generation %zu, ref %zu, stats %zu\n",
           sizeof(SuAudioAssetId), sizeof(SuAudioAssetGeneration), sizeof(SuAudioAssetRef), sizeof(SuAudioAssetStats));
    printf("test_audio_asset: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
