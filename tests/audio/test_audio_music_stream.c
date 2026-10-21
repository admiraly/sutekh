#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/audio_parameter.h"
#include "audio/su_audio_parameter_int.h"
#include "audio/su_audio_state.h"

static unsigned checks;
static unsigned failures;

#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        failures++; \
        fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
    } \
} while (0)

static void compare_source(const SuAudioMusicStreamSource *first, const SuAudioMusicStreamSource *second,
                           uint32_t rate, uint64_t length) {
    SuAudioAssets *assets = NULL;
    SuAudioMusic *players[2] = {NULL, NULL};
    SuAudioMusicDesc desc = {sizeof desc, rate, 1u, 0u};
    SuAudioTempoPoint tempo = {0u, 120000u};
    SuAudioMusicSegmentDesc segment;
    SuMusicTransition transition;
    SuAudioStateIo io;
    uint8_t checkpoint[4096];
    float expected[514];
    float actual[514];
    uint64_t boundary = 0u;
    uint64_t frame;
    uint64_t total = length * 3u + 509u;
    uint32_t player;
    double energy = 0.0;
    CHECK(su_audio_assets_create(1u, 1u, &assets) == SU_OK);
    memset(&segment, 0, sizeof segment);
    segment.struct_size = sizeof segment;
    segment.segment_id = 1u;
    segment.stem_count = 1u;
    segment.tempo = &tempo;
    segment.tempo_count = 1u;
    segment.beats_per_bar = 4u;
    segment.length_frames = length;
    segment.loop = 1u;
    memset(&transition, 0, sizeof transition);
    transition.struct_size = sizeof transition;
    transition.segment_id = 1u;
    for (player = 0u; player < 2u; player++) {
        CHECK(su_audio_music_create(&desc, assets, &players[player]) == SU_OK);
        segment.stream_stems = player == 0u ? first : second;
        CHECK(su_audio_music_segment_add(players[player], &segment) == SU_OK);
        CHECK(su_audio_music_transition(players[player], &transition, 0u, &boundary) == SU_OK);
    }
    CHECK(su_audio_music_render(players[0], expected, 1u) == SU_OK);
    CHECK(su_audio_music_render(players[1], actual, 1u) == SU_OK);
    CHECK(memcmp(expected, actual, 2u * sizeof(float)) == 0);
    transition.fade_in_frames = 509u;
    transition.fade_out_frames = 509u;
    for (player = 0u; player < 2u; player++) {
        CHECK(su_audio_music_transition(players[player], &transition, length - 7u, &boundary) == SU_OK);
        CHECK(boundary == length - 7u);
    }
    for (frame = 1u; frame < total;) {
        uint32_t count = total - frame < 257u ? (uint32_t)(total - frame) : 257u;
        uint32_t index;
        CHECK(su_audio_music_render(players[0], expected, count) == SU_OK);
        for (index = 0u; index < count; index++) {
            CHECK(su_audio_music_render(players[1], actual + index * 2u, 1u) == SU_OK);
        }
        CHECK(memcmp(expected, actual, (size_t)count * 2u * sizeof(float)) == 0);
        for (index = 0u; index < count * 2u; index++) {
            energy += (double)expected[index] * expected[index];
        }
        if (frame <= length && frame + count > length) {
            memset(&io, 0, sizeof io);
            io.save = 1;
            io.data = checkpoint;
            io.capacity = sizeof checkpoint;
            CHECK(su_audio_music_state_io(players[0], &io) == SU_OK);
            CHECK(su_audio_music_render(players[1], actual, 257u) == SU_OK);
            io.capacity = io.pos;
            io.pos = 0u;
            io.save = 0;
            CHECK(su_audio_music_state_io(players[1], &io) == SU_OK);
        }
        frame += count;
    }
    CHECK(energy > 0.01);
    for (player = 0u; player < 2u; player++) {
        su_audio_music_destroy(players[player]);
    }
    CHECK(su_audio_assets_destroy(assets) == SU_OK);
}

static void mono_source(void) {
    static const uint8_t wav[] = {
        'R', 'I', 'F', 'F', 44, 0, 0, 0, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
        0x80, 0xbb, 0, 0, 0, 0x77, 1, 0, 2, 0, 16, 0,
        'd', 'a', 't', 'a', 8, 0, 0, 0, 0, 0, 0, 32, 0, 64, 0, 96
    };
    SuAudioMusicStreamSource source = {NULL, wav, sizeof wav, 2048u};
    compare_source(&source, &source, 48000u, 16u);
}

static void ogg_source(const char *path) {
    SuAudioMusicStreamSource sources[2];
    SuAudioStream *probe = NULL;
    SuAudioStreamInfo info;
    uint8_t *bytes;
    FILE *file;
    long size;
    uint32_t status = su_audio_stream_open_file(path, 2048u, &probe, NULL);
    if (status == SU_E_UNSUPPORTED_METHOD) {
        printf("audio_music_stream: optional Ogg decoder unavailable\n");
        return;
    }
    CHECK(status == SU_OK);
    if (status != SU_OK) {
        return;
    }
    su_audio_stream_info(probe, &info);
    CHECK(su_audio_stream_close(probe) == SU_OK);
    file = fopen(path, "rb");
    CHECK(file != NULL);
    if (file == NULL) {
        return;
    }
    CHECK(fseek(file, 0, SEEK_END) == 0);
    size = ftell(file);
    CHECK(size > 0);
    if (size <= 0) {
        (void)fclose(file);
        return;
    }
    bytes = (uint8_t *)malloc((size_t)size);
    CHECK(bytes != NULL);
    if (bytes == NULL) {
        (void)fclose(file);
        return;
    }
    rewind(file);
    CHECK(fread(bytes, 1u, (size_t)size, file) == (size_t)size);
    CHECK(fclose(file) == 0);
    memset(sources, 0, sizeof sources);
    sources[0].path = path;
    sources[0].ring_frames = 2048u;
    sources[1].bytes = bytes;
    sources[1].byte_count = (size_t)size;
    sources[1].ring_frames = 3073u;
    compare_source(&sources[0], &sources[1], info.sample_rate, info.frames);
    free(bytes);
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    mono_source();
    if (argc == 2) {
        ogg_source(argv[1]);
    }
    printf("audio_music_stream: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
