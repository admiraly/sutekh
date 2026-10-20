#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sutekh/audio.h"

#define RATE 48000u
#define FRAMES 9600u
#define PI 3.141592653589793

static unsigned checks;
static unsigned failures;

#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        failures++; \
        fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
    } \
} while (0)

static SuAudioMixer *mixer(uint32_t rate, uint32_t capacity) {
    SuAudioMixerDesc desc;
    SuAudioMixer *result = NULL;
    memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.sample_rate = rate;
    desc.max_voices = 1u;
    desc.command_capacity = capacity;
    CHECK(su_audio_mixer_create(&desc, &result) == SU_OK);
    return result;
}

static SuAudioVoice play(SuAudioMixer *mix, const SuAudioClip *clip, uint32_t flags) {
    SuAudioVoiceDesc desc;
    SuAudioVoice voice = 0;
    memset(&desc, 0, sizeof desc);
    desc.clip = clip;
    desc.gain = 0.25f;
    desc.flags = flags | SU_AUDIO_LOOP;
    if ((flags & SU_AUDIO_SPATIAL) != 0u) {
        desc.pos = su_wpos(0.0, 0.0, -10.0);
        desc.min_distance = 1.0f;
        desc.max_distance = 100.0f;
    }
    CHECK(su_audio_play(mix, &desc, &voice) == SU_OK);
    return voice;
}

static void tone(float *samples, uint32_t channels, uint32_t rate, float frequency) {
    uint32_t frame;
    for (frame = 0; frame < FRAMES; frame++) {
        samples[frame * channels] = (float)sin(2.0 * PI * frequency * frame / rate);
        if (channels == 2u) {
            samples[frame * channels + 1u] = (float)sin(2.0 * PI * frequency * 2.0 * frame / rate);
        }
    }
}

static double energy(const float *samples, uint32_t channel) {
    uint32_t frame;
    double sum = 0.0;
    for (frame = FRAMES / 2u; frame < FRAMES; frame++) {
        double value = samples[frame * 2u + channel];
        sum += value * value;
    }
    return sum;
}

static void response(void) {
    static const uint32_t rates[] = {8000u, 48000u, 96000u};
    static const float frequencies[] = {100.0f, 1000.0f, 3000.0f};
    float source[FRAMES * 2u];
    float dry[FRAMES * 2u];
    float wet[FRAMES * 2u];
    uint32_t rate_index, channels, frequency_index, channel;
    for (rate_index = 0; rate_index < 3u; rate_index++) {
        uint32_t rate = rates[rate_index];
        for (channels = 1u; channels <= 2u; channels++) {
            for (frequency_index = 0; frequency_index < 3u; frequency_index++) {
                SuAudioClip clip = {source, FRAMES, channels, rate};
                SuAudioMixer *plain = mixer(rate, 32u);
                SuAudioMixer *filtered = mixer(rate, 32u);
                float frequency = frequencies[frequency_index];
                SuAudioVoice voice;
                tone(source, channels, rate, frequency);
                (void)play(plain, &clip, 0u);
                voice = play(filtered, &clip, 0u);
                CHECK(su_audio_voice_set_lowpass(filtered, voice, 1000.0f) == SU_OK);
                su_audio_mixer_render(plain, dry, FRAMES);
                su_audio_mixer_render(filtered, wet, FRAMES);
                for (channel = 0; channel < 2u; channel++) {
                    double term = 1.0 - cos(2.0 * PI * 1000.0 / rate);
                    double alpha = sqrt(term * term + 2.0 * term) - term;
                    double omega = 2.0 * PI * frequency * (channels == 2u && channel == 1u ? 2.0 : 1.0) / rate;
                    double expected = alpha / sqrt(1.0 + (1.0 - alpha) * (1.0 - alpha) - 2.0 * (1.0 - alpha) * cos(omega));
                    double measured = sqrt(energy(wet, channel) / energy(dry, channel));
                    CHECK(fabs(measured - expected) < 0.015);
                    if (frequency_index == 1u && channel == 0u) {
                        CHECK(fabs(20.0 * log10(measured) + 3.0103) < 0.15);
                    }
                }
                su_audio_mixer_destroy(plain);
                su_audio_mixer_destroy(filtered);
            }
        }
    }
}

static void bypass_and_reuse(void) {
    float source[FRAMES * 2u];
    float dry[FRAMES * 2u];
    float wet[FRAMES * 2u];
    SuAudioClip clip = {source, FRAMES, 2u, RATE};
    SuAudioMixer *plain = mixer(RATE, 32u);
    SuAudioMixer *filtered = mixer(RATE, 32u);
    SuAudioVoice stale, voice;
    uint32_t frame;
    tone(source, 2u, RATE, 1000.0f);
    (void)play(plain, &clip, 0u);
    stale = play(filtered, &clip, 0u);
    CHECK(su_audio_voice_set_lowpass(filtered, stale, 0.0f) == SU_OK);
    su_audio_mixer_render(plain, dry, FRAMES);
    su_audio_mixer_render(filtered, wet, FRAMES);
    CHECK(memcmp(dry, wet, sizeof dry) == 0);
    CHECK(su_audio_voice_set_lowpass(filtered, stale, 24000.0f) == SU_OK);
    su_audio_mixer_render(plain, dry, FRAMES);
    su_audio_mixer_render(filtered, wet, FRAMES);
    CHECK(memcmp(dry, wet, sizeof dry) == 0);
    CHECK(su_audio_voice_set_lowpass(filtered, stale, 20.0f) == SU_OK);
    su_audio_mixer_render(filtered, wet, FRAMES);
    CHECK(su_audio_voice_set_lowpass(filtered, stale, 0.0f) == SU_OK);
    su_audio_mixer_render(plain, dry, FRAMES);
    su_audio_mixer_render(filtered, wet, FRAMES);
    CHECK(memcmp(dry + 960u, wet + 960u, sizeof dry - 960u * sizeof(float)) == 0);
    CHECK(su_audio_voice_set_lowpass(filtered, stale, 20.0f) == SU_OK);
    su_audio_mixer_render(filtered, wet, FRAMES);
    for (frame = 0; frame < FRAMES * 2u; frame++) {
        source[frame] = 0.0f;
    }
    CHECK(su_audio_voice_set_lowpass(filtered, stale, 50.0f) == SU_OK);
    voice = play(filtered, &clip, 0u);
    CHECK(su_audio_voice_set_lowpass(filtered, stale, 50.0f) == SU_E_STALE_HANDLE);
    CHECK(su_audio_voice_set_lowpass(filtered, voice, 1000.0f) == SU_OK);
    su_audio_mixer_render(filtered, wet, FRAMES);
    CHECK(energy(wet, 0u) == 0.0 && energy(wet, 1u) == 0.0);
    for (frame = 0; frame < FRAMES * 2u; frame++) {
        CHECK(wet[frame] == 0.0f);
    }
    CHECK(su_audio_voice_set_lowpass(filtered, voice, -1.0f) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_voice_set_lowpass(filtered, voice, NAN) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_voice_set_lowpass(filtered, voice, INFINITY) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_stop(filtered, voice, 0.0f) == SU_OK);
    su_audio_mixer_render(filtered, wet, 256u);
    su_audio_update(filtered);
    voice = play(filtered, &clip, 0u);
    su_audio_mixer_render(filtered, wet, FRAMES);
    CHECK(energy(wet, 0u) == 0.0);
    su_audio_mixer_destroy(plain);
    su_audio_mixer_destroy(filtered);
    filtered = mixer(RATE, 2u);
    voice = play(filtered, &clip, 0u);
    CHECK(su_audio_voice_set_lowpass(filtered, voice, 1000.0f) == SU_OK);
    CHECK(su_audio_voice_set_lowpass(filtered, voice, 500.0f) == SU_E_RESOURCE_LIMIT);
    su_audio_mixer_destroy(filtered);
}

static void transitions(void) {
    float source[FRAMES * 2u];
    float output[FRAMES * 2u];
    float copy[FRAMES * 2u];
    static const float cutoffs[] = {80.0f, 10000.0f, 30.0f, 0.0f, 1000.0f};
    uint32_t spatial, channels, change, frame;
    for (spatial = 0; spatial < 2u; spatial++) {
        for (channels = 1u; channels <= 2u; channels++) {
            SuAudioClip clip = {source, FRAMES, channels, RATE};
            SuAudioMixer *first = mixer(RATE, 32u);
            SuAudioMixer *second = mixer(RATE, 32u);
            SuAudioVoice voice = play(first, &clip, spatial ? SU_AUDIO_SPATIAL : 0u);
            SuAudioVoice twin = play(second, &clip, spatial ? SU_AUDIO_SPATIAL : 0u);
            float previous[2] = {0.0f, 0.0f};
            tone(source, channels, RATE, 100.0f);
            for (change = 0; change < 5u; change++) {
                CHECK(su_audio_voice_set_lowpass(first, voice, cutoffs[change]) == SU_OK);
                CHECK(su_audio_voice_set_lowpass(second, twin, cutoffs[change]) == SU_OK);
                CHECK(su_audio_voice_set_pitch(first, voice, 1.0f + (float)change * 0.1f) == SU_OK);
                CHECK(su_audio_voice_set_pitch(second, twin, 1.0f + (float)change * 0.1f) == SU_OK);
                su_audio_mixer_render(first, output, FRAMES);
                su_audio_mixer_render(second, copy, FRAMES);
                CHECK(memcmp(output, copy, sizeof output) == 0);
                for (frame = 0; frame < FRAMES; frame++) {
                    uint32_t channel;
                    for (channel = 0; channel < 2u; channel++) {
                        CHECK(isfinite(output[frame * 2u + channel]));
                        CHECK(fabsf(output[frame * 2u + channel] - previous[channel]) < 0.035f);
                        previous[channel] = output[frame * 2u + channel];
                    }
                }
            }
            CHECK(su_audio_master_limiter(first, 0.5f) == SU_OK);
            CHECK(su_audio_voice_set_gain(first, voice, 20.0f) == SU_OK);
            su_audio_mixer_render(first, output, FRAMES);
            for (frame = 0; frame < FRAMES * 2u; frame++) {
                CHECK(isfinite(output[frame]) && fabsf(output[frame]) <= 1.0f);
            }
            su_audio_mixer_destroy(first);
            su_audio_mixer_destroy(second);
        }
    }
}

static void partition(void) {
    float source[FRAMES * 2u], whole[FRAMES * 2u], split[FRAMES * 2u];
    SuAudioClip clip = {source, FRAMES, 2u, RATE};
    SuAudioMixer *first = mixer(RATE, 32u);
    SuAudioMixer *second = mixer(RATE, 32u);
    SuAudioVoice voice = play(first, &clip, 0u);
    SuAudioVoice twin = play(second, &clip, 0u);
    uint32_t offset;
    tone(source, 2u, RATE, 1000.0f);
    CHECK(su_audio_voice_set_lowpass(first, voice, 500.0f) == SU_OK);
    CHECK(su_audio_voice_set_lowpass(second, twin, 500.0f) == SU_OK);
    su_audio_mixer_render(first, whole, FRAMES);
    for (offset = 0; offset < FRAMES; offset += 64u) {
        CHECK(su_audio_voice_set_lowpass(second, twin, 500.0f) == SU_OK);
        su_audio_mixer_render(second, split + offset * 2u, 64u);
    }
    CHECK(memcmp(whole, split, sizeof whole) == 0);
    for (offset = 0; offset < FRAMES; offset += 64u) {
        CHECK(su_audio_voice_set_lowpass(first, voice, offset % 128u == 0u ? 20.0f : 10000.0f) == SU_OK);
        CHECK(su_audio_voice_set_lowpass(second, twin, offset % 128u == 0u ? 20.0f : 10000.0f) == SU_OK);
        su_audio_mixer_render(first, whole, 64u);
        su_audio_mixer_render(second, split, 64u);
        CHECK(memcmp(whole, split, 128u * sizeof(float)) == 0);
        CHECK(isfinite(whole[126]));
    }
    su_audio_mixer_destroy(first);
    su_audio_mixer_destroy(second);
}

static double benchmark(int filtered) {
    SuAudioMixerDesc desc;
    SuAudioMixer *mix = NULL;
    SuAudioClip *clip = NULL;
    SuAudioVoiceDesc voice_desc;
    float output[512];
    uint32_t voice_index, block;
    clock_t begin, end;
    memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.sample_rate = RATE;
    desc.max_voices = 64u;
    CHECK(su_audio_mixer_create(&desc, &mix) == SU_OK);
    CHECK(su_audio_clip_tone(SU_AUDIO_WAVE_NOISE, 1000.0f, 1.0f, RATE, 1, &clip) == SU_OK);
    memset(&voice_desc, 0, sizeof voice_desc);
    voice_desc.clip = clip;
    voice_desc.gain = 0.005f;
    voice_desc.flags = SU_AUDIO_LOOP;
    for (voice_index = 0; voice_index < 64u; voice_index++) {
        SuAudioVoice voice;
        CHECK(su_audio_play(mix, &voice_desc, &voice) == SU_OK);
        if (filtered) {
            CHECK(su_audio_voice_set_lowpass(mix, voice, 1000.0f) == SU_OK);
        }
    }
    for (block = 0; block < 100u; block++) {
        su_audio_mixer_render(mix, output, 256u);
    }
    begin = clock();
    for (block = 0; block < 2000u; block++) {
        su_audio_mixer_render(mix, output, 256u);
    }
    end = clock();
    su_audio_mixer_destroy(mix);
    su_audio_clip_free(clip);
    return (double)(end - begin) * 1.0e9 / CLOCKS_PER_SEC / (64.0 * 256.0 * 2000.0);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--bench") == 0) {
        unsigned trial;
        for (trial = 0; trial < 7u; trial++) {
            double dry = benchmark(0);
            double wet = benchmark(1);
            printf("trial %u: bypass %.3f, filtered %.3f, delta %.3f ns/voice/frame\n", trial, dry, wet, wet - dry);
        }
    } else {
        response();
        bypass_and_reuse();
        transitions();
        partition();
    }
    printf("audio_lowpass: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
