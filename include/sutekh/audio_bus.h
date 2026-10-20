#ifndef SUTEKH_AUDIO_BUS_H
#define SUTEKH_AUDIO_BUS_H

#include <stdint.h>
#include "sutekh/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed bus tree with sample-ramped gains, sidechain ducking and an optional
 * dynamic-range window (A06). Single-owner presentation object: every call,
 * including processing, is serialized by the owner. All storage, including
 * scratch and execution order, is prepared at creation; processing never
 * allocates. Bus IDs are the authored graph leaf bus_id values. */

typedef uint32_t SuAudioBusId;

#define SU_AUDIO_BUS_ROOT UINT32_MAX          /* parent value of the single root (master) bus */
#define SU_AUDIO_BUS_MAX 4096u
#define SU_AUDIO_BUS_MAX_RULES 4096u
#define SU_AUDIO_BUS_MAX_BLOCK 8192u
#define SU_AUDIO_LOUDNESS_SILENT (-200.0f)    /* loudness at or below this means no source */

#define SU_AUDIO_DUCK_DELAY_QUANTUM UINT32_C(1) /* detector reads the source one block (max_block_frames) late;
                                                   the only way a sidechain may close a feedback cycle */

typedef struct SuAudioBusDesc {
    SuAudioBusId id;
    SuAudioBusId parent;           /* SU_AUDIO_BUS_ROOT for the master */
    float gain_db;                 /* initial fader */
    float max_duck_db;             /* clamp of the summed duck attenuation on this bus (dB, >= 0); 0 = no clamp */
    uint32_t hdr;                  /* nonzero: participates in the dynamic-range window */
} SuAudioBusDesc;

typedef struct SuAudioDuckRule {
    SuAudioBusId source;           /* sidechain detector: this bus's post-gain output */
    SuAudioBusId target;           /* attenuated bus */
    float threshold_db;            /* dBFS of the stereo-linked sample peak */
    float ratio;                   /* >= 1; reduction = (level - threshold) * (1 - 1 / ratio) */
    float attack_ms;               /* time for the reduction to rise by max_attenuation_db; 0 = instant */
    float hold_ms;                 /* reduction held after the detector falls */
    float release_ms;              /* time for the reduction to fall by max_attenuation_db; 0 = instant */
    float max_attenuation_db;      /* > 0 */
    uint32_t flags;                /* SU_AUDIO_DUCK_* */
} SuAudioDuckRule;

/* Window over authored source loudness (dB, the caller's units; not measured from PCM). The loudest current
 * participating bus defines the window top T, which falls no faster than window_db per release_ms. Every
 * participating bus is attenuated by max(0, T - ceiling_db); one quieter than T - window_db is culled to
 * silence. Gains ramp linearly across each processed block. Disabled (enabled = 0) it has no effect at all;
 * storing values does not enable it. */
typedef struct SuAudioHdrPolicy {
    uint32_t enabled;
    float window_db;               /* > 0 when enabled */
    float ceiling_db;
    float release_ms;              /* >= 0; 0 = the top follows the loudest bus instantly */
} SuAudioHdrPolicy;

typedef struct SuAudioBusTreeDesc {
    uint32_t struct_size;
    uint32_t sample_rate;          /* converts the *_ms fields */
    uint32_t max_block_frames;     /* 1..SU_AUDIO_BUS_MAX_BLOCK; also the delayed-sidechain latency */
    const SuAudioBusDesc *buses;
    uint32_t bus_count;
    const SuAudioDuckRule *rules;
    uint32_t rule_count;
    SuAudioHdrPolicy hdr;
} SuAudioBusTreeDesc;

enum {
    SU_AUDIO_BUS_DIAG_NONE = 0,
    SU_AUDIO_BUS_DIAG_DESC = 1,          /* bad descriptor value; see bus_id/rule_index */
    SU_AUDIO_BUS_DIAG_DUPLICATE = 2,     /* duplicate bus ID */
    SU_AUDIO_BUS_DIAG_UNKNOWN_BUS = 3,   /* parent, source or target not declared */
    SU_AUDIO_BUS_DIAG_ROOT = 4,          /* not exactly one root */
    SU_AUDIO_BUS_DIAG_CYCLE = 5          /* parent or undelayed sidechain cycle */
};

typedef struct SuAudioBusDiagnostic {
    uint32_t code;                 /* SU_AUDIO_BUS_DIAG_* */
    SuAudioBusId bus_id;
    uint32_t rule_index;           /* UINT32_MAX when not a rule */
} SuAudioBusDiagnostic;

typedef struct SuAudioBusMeter {
    float peak;                    /* max |sample| since the last reset, linear */
    float rms;                     /* over both channels since the last reset, linear */
    float true_peak;               /* 4x cubic-interpolated inter-sample estimate (>= peak), linear */
    float fader_db;                /* at the last processed sample */
    float duck_db;                 /* clamped attenuation at the last processed sample (>= 0) */
    float hdr_db;                  /* window attenuation at the end of the last block (>= 0; culled = 200) */
    uint64_t frames;               /* since the last reset */
} SuAudioBusMeter;

typedef struct SuAudioBusTree SuAudioBusTree;

uint32_t su_audio_bus_tree_create(const SuAudioBusTreeDesc *desc, SuAudioBusTree **out,
                                  SuAudioBusDiagnostic *diagnostic);
void su_audio_bus_tree_destroy(SuAudioBusTree *tree);
/* Execution order: index 0..bus_count-1; children and undelayed sidechain sources precede their consumers. */
uint32_t su_audio_bus_order(const SuAudioBusTree *tree, uint32_t index, SuAudioBusId *out);
/* Adds interleaved stereo into a bus for the next process call; frames must not exceed max_block_frames and
 * every input of one block must use the frame count later passed to process. */
uint32_t su_audio_bus_mix(SuAudioBusTree *tree, SuAudioBusId bus, const float *stereo, uint32_t frames);
/* Authored loudness of the loudest active source on the bus for the next block; reset to silent each block. */
uint32_t su_audio_bus_set_loudness(SuAudioBusTree *tree, SuAudioBusId bus, float loudness_db);
/* Fader target reached linearly (in linear gain) over ramp_frames; 0 = from the next sample. */
uint32_t su_audio_bus_set_gain(SuAudioBusTree *tree, SuAudioBusId bus, float gain_db, uint32_t ramp_frames);
/* Mixes one block into interleaved stereo out (overwritten) and clears every bus input. */
uint32_t su_audio_bus_process(SuAudioBusTree *tree, float *out, uint32_t frames);
/* Total linear gain (fader x duck x window) applied to the last processed sample of the bus. */
uint32_t su_audio_bus_gain_at(const SuAudioBusTree *tree, SuAudioBusId bus, float *out);
uint32_t su_audio_bus_meter_read(SuAudioBusTree *tree, SuAudioBusId bus, SuAudioBusMeter *out, int reset);
uint64_t su_audio_bus_tree_bytes(const SuAudioBusTree *tree);

#ifdef __cplusplus
}
#endif
#endif
