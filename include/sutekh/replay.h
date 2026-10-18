/* Sutekh command-stream replay (generation 1), part of the host library (su_host / sutekh_host).
 *
 * A replay is the initial state plus every tick's input record: re-simulating the inputs on the same runtime
 * reproduces every state, and the recorded digests prove it tick by tick. Sparse keyframes (full snapshots)
 * make seeking cheap: restore the nearest keyframe at or before the target tick, then re-simulate.
 *
 * Format ("SUREPLAY", version 1; all integers little-endian):
 *   header  "SUREPLAY" 0x00 | u32 format version | u32 host API version | u32 len + engine version text |
 *           world identity (32) | schema digest (32) | u32 system count | per system: u32 len + system id,
 *           semantic hash (32), u32 len + numeric profile | u64 seed | u32 tick rate (Hz) | u64 start tick |
 *           initial snapshot digest (32) | u64 keyframe interval | u64 digest interval
 *   records in tick order; for each tick T, at most one of each, in this order:
 *           'K' u64 T | u64 len | snapshot bytes (state at T, before T's inputs; always at the start tick)
 *           'D' u64 T | digest (32) (state at T; not written where a keyframe is)
 *           'I' u32 len | input record (su_host_input_encode format) for T, only when T had inputs
 *   end     'E' u64 end tick | digest of the end state (32)
 * The bytes are canonical: one recording gives one byte string, and parsing is strict (order, ranges,
 * lengths, the end record last).
 *
 * Verification failures: a world whose identity differs from the replay's is SU_E_SCHEMA_MISMATCH. A state
 * whose digest differs from the recorded one (a desync) is reported as SU_E_INTERNAL with
 * err->index == SU_REPLAY_DESYNC and err->tick the tick whose state differs: the status codes are a frozen
 * contract, and a desync means an invariant (re-simulation reproduces the state) did not hold. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef SUTEKH_REPLAY_H
#define SUTEKH_REPLAY_H

#include <stddef.h>
#include <stdint.h>

#include "sutekh/host.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SU_REPLAY_FORMAT_VERSION 1u
#define SU_REPLAY_DESYNC UINT32_C(0xDE5C)   /* SuHostError.index of a digest mismatch */

typedef struct SuReplayOptions {
    uint32_t struct_size;          /* sizeof(SuReplayOptions) */
    uint32_t reserved0;            /* 0 */
    uint64_t keyframe_interval;    /* ticks between keyframes, >= 1 (also the seek granularity) */
    uint64_t digest_interval;      /* ticks between digest records; 0 = keyframes only */
} SuReplayOptions;

/* ---- recording ---- */

typedef struct SuReplayWriter SuReplayWriter;

/* Starts a recording of w at its current tick: header and the initial keyframe. The world's identity and
 * settings are fixed for the recording. */
SU_HOST_API uint32_t su_replay_writer_begin(SuHostWorld *w, const SuReplayOptions *opt, SuReplayWriter **out,
                                            SuHostError *err);
/* Steps w by up to ticks ticks (as su_host_step) and records each tick: its input record as applied, and the
 * keyframes and digests the options ask for. The world must be the one the writer began with, at the tick the
 * writer has reached; anything else is SU_E_INVALID_ARGUMENT. A failing tick is not recorded. */
SU_HOST_API uint32_t su_replay_writer_step(SuReplayWriter *rw, SuHostWorld *w, uint64_t ticks, uint64_t *done,
                                           SuHostError *err);
/* Writes the end record and hands over the bytes (*bytes, *len), to be released with su_replay_free. The
 * writer accepts no more ticks afterwards. */
SU_HOST_API uint32_t su_replay_writer_finish(SuReplayWriter *rw, const SuHostWorld *w, uint8_t **bytes, size_t *len);
SU_HOST_API void su_replay_writer_destroy(SuReplayWriter *rw);
SU_HOST_API void su_replay_free(uint8_t *bytes);

/* ---- playback ---- */

typedef struct SuReplay SuReplay;

typedef struct SuReplayInfo {
    uint32_t format_version;
    uint32_t host_api_version;
    const char *engine_version;    /* owned by the replay */
    uint8_t identity[32];
    uint8_t schema_digest[32];
    uint32_t system_count;
    uint64_t seed;
    uint32_t tick_rate_hz;
    uint64_t start_tick;
    uint64_t end_tick;
    uint8_t initial_digest[32];
    uint8_t final_digest[32];
    uint64_t keyframe_interval;
    uint64_t digest_interval;
    uint32_t keyframe_count;
    uint32_t digest_count;
    uint32_t input_count;          /* ticks that had inputs */
} SuReplayInfo;

/* Parses and indexes a replay (the bytes are copied). Malformed bytes are SU_E_PARSE. */
SU_HOST_API uint32_t su_replay_open(const uint8_t *bytes, size_t len, SuReplay **out, SuHostError *err);
SU_HOST_API void su_replay_close(SuReplay *r);
SU_HOST_API uint32_t su_replay_info(const SuReplay *r, SuReplayInfo *out);
/* System index i in schedule order: id, semantic hash (hex) and numeric profile. */
SU_HOST_API uint32_t su_replay_system(const SuReplay *r, uint32_t index, const char **system_id,
                                      const char **semantic_hash_hex, const char **numeric_profile);

/* Puts w at tick (start_tick..end_tick): restores the nearest keyframe at or before it, then re-simulates the
 * recorded inputs up to tick, verifying every recorded digest on the way. Queued inputs of w are dropped.
 * w must have the replay's identity (create it from the same capsules and settings). */
SU_HOST_API uint32_t su_replay_seek(const SuReplay *r, SuHostWorld *w, uint64_t tick, SuHostError *err);
/* Re-simulates up to ticks ticks from w's current tick (which must lie in [start_tick, end_tick]) with the
 * recorded inputs, verifying each recorded keyframe and digest, and the end digest when the end is reached.
 * *done counts ticks run. Queued inputs of w for those ticks are dropped first. */
SU_HOST_API uint32_t su_replay_play(const SuReplay *r, SuHostWorld *w, uint64_t ticks, uint64_t *done,
                                    SuHostError *err);

#ifdef __cplusplus
}
#endif

#endif
