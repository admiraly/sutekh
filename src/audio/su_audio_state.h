#ifndef SU_AUDIO_STATE_H
#define SU_AUDIO_STATE_H

/* Symmetric checkpoint serializer shared by the audio components. One function per component walks its
 * mutable state through su_audio_state_io: with data == NULL it only counts bytes, with save != 0 it writes,
 * otherwise it reads. The layout is the build's in-memory representation of plain numeric fields (profile
 * audio_float_local_v1: same build only); pointers are never written. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct SuAudioStateIo {
    uint8_t *data;
    size_t capacity;
    size_t pos;
    int save;
    int failed;
} SuAudioStateIo;

static inline void su_audio_state_io(SuAudioStateIo *io, void *value, size_t bytes) {
    if (io->failed) {
        return;
    }
    if (io->data != NULL) {
        if (bytes > io->capacity - io->pos) {
            io->failed = 1;
            return;
        }
        if (io->save) {
            memcpy(io->data + io->pos, value, bytes);
        } else {
            memcpy(value, io->data + io->pos, bytes);
        }
    }
    io->pos += bytes;
}

#define SU_STATE(io, field) su_audio_state_io((io), &(field), sizeof(field))

#endif
