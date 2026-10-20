#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/audio_bus.h"

#define NONE UINT32_MAX

typedef struct Bus {
    SuAudioBusDesc desc;
    uint32_t parent;
    float *in;
    float fader;
    float fader_target;
    float fader_step;
    uint32_t ramp_remain;
    float hdr_gain;           /* applied at the end of the last block */
    float hdr_db;
    float loudness;
    float last_gain;
    float last_duck;
    float peak;
    double sumsq;
    float true_peak;
    float hist[2][3];
    uint64_t frames;
    uint32_t target_first, target_count; /* rules attenuating this bus (CSR into rule_index) */
    uint32_t source_first, source_count; /* rules detecting on this bus */
} Bus;

typedef struct Rule {
    SuAudioDuckRule desc;
    uint32_t source, target;
    float attack_step, release_step; /* dB per sample */
    uint32_t hold_samples;
    uint32_t hold;
    float env;
    float slope;                     /* 1 - 1 / ratio */
    float *det;                      /* linear stereo-linked peak per sample of this block */
    float *ring;                     /* delayed rules: previous max_block_frames samples */
} Rule;

typedef struct IdIndex { SuAudioBusId id; uint32_t index; } IdIndex;

struct SuAudioBusTree {
    SuAudioBusTreeDesc desc;
    Bus *buses;
    Rule *rules;
    IdIndex *lookup;
    uint32_t *order;
    uint32_t *target_rules;
    uint32_t *source_rules;
    uint32_t root;
    uint32_t pending_frames;  /* frame count of inputs mixed for the next block; 0 = none yet */
    uint32_t ring_pos;
    float hdr_top;
    uint64_t bytes;
};

static int finite_f(float v) { return v == v && v - v == 0.0f; }

static uint32_t fail(SuAudioBusDiagnostic *diagnostic, uint32_t code, SuAudioBusId bus, uint32_t rule) {
    if (diagnostic) {
        diagnostic->code = code;
        diagnostic->bus_id = bus;
        diagnostic->rule_index = rule;
    }
    return code == SU_AUDIO_BUS_DIAG_CYCLE ? SU_E_SCHEMA_MISMATCH : SU_E_INVALID_ARGUMENT;
}

static int id_compare(const void *a, const void *b) {
    const IdIndex *x = (const IdIndex *)a;
    const IdIndex *y = (const IdIndex *)b;
    return x->id < y->id ? -1 : (x->id > y->id ? 1 : 0);
}

static uint32_t find(const SuAudioBusTree *tree, SuAudioBusId id) {
    uint32_t lo = 0u, hi = tree->desc.bus_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (tree->lookup[mid].id == id) return tree->lookup[mid].index;
        if (tree->lookup[mid].id < id) lo = mid + 1u; else hi = mid;
    }
    return NONE;
}

static float db_to_gain(float db) { return powf(10.0f, db / 20.0f); }

static float ms_step(float max_db, float ms, uint32_t rate) {
    double frames = (double)ms * (double)rate / 1000.0;
    return frames < 1.0 ? INFINITY : (float)((double)max_db / frames);
}

static void *take(unsigned char **cursor, size_t bytes) {
    void *p = *cursor;
    *cursor += (bytes + 15u) & ~(size_t)15u;
    return p;
}

static size_t round16(size_t bytes) { return (bytes + 15u) & ~(size_t)15u; }

void su_audio_bus_tree_destroy(SuAudioBusTree *tree) {
    free(tree);
}

static uint32_t validate(const SuAudioBusTreeDesc *desc, SuAudioBusDiagnostic *diagnostic) {
    uint32_t i;
    if (desc->sample_rate == 0u || desc->max_block_frames == 0u || desc->max_block_frames > SU_AUDIO_BUS_MAX_BLOCK ||
        desc->bus_count == 0u || desc->bus_count > SU_AUDIO_BUS_MAX || !desc->buses ||
        desc->rule_count > SU_AUDIO_BUS_MAX_RULES || (desc->rule_count && !desc->rules))
        return fail(diagnostic, SU_AUDIO_BUS_DIAG_DESC, 0u, NONE);
    for (i = 0u; i < desc->bus_count; ++i) {
        const SuAudioBusDesc *bus = &desc->buses[i];
        if (bus->id == SU_AUDIO_BUS_ROOT || !finite_f(bus->gain_db) || bus->gain_db > 120.0f ||
            !finite_f(bus->max_duck_db) || bus->max_duck_db < 0.0f)
            return fail(diagnostic, SU_AUDIO_BUS_DIAG_DESC, bus->id, NONE);
    }
    for (i = 0u; i < desc->rule_count; ++i) {
        const SuAudioDuckRule *rule = &desc->rules[i];
        if (!finite_f(rule->threshold_db) || !finite_f(rule->ratio) || rule->ratio < 1.0f ||
            !finite_f(rule->attack_ms) || rule->attack_ms < 0.0f || !finite_f(rule->hold_ms) || rule->hold_ms < 0.0f ||
            !finite_f(rule->release_ms) || rule->release_ms < 0.0f || !finite_f(rule->max_attenuation_db) ||
            rule->max_attenuation_db <= 0.0f || (rule->flags & ~SU_AUDIO_DUCK_DELAY_QUANTUM) != 0u)
            return fail(diagnostic, SU_AUDIO_BUS_DIAG_DESC, rule->target, i);
    }
    if (desc->hdr.enabled && (!finite_f(desc->hdr.window_db) || desc->hdr.window_db <= 0.0f ||
        !finite_f(desc->hdr.ceiling_db) || !finite_f(desc->hdr.release_ms) || desc->hdr.release_ms < 0.0f))
        return fail(diagnostic, SU_AUDIO_BUS_DIAG_DESC, 0u, NONE);
    return SU_OK;
}

static uint32_t build_order(SuAudioBusTree *tree, uint32_t *indegree, SuAudioBusDiagnostic *diagnostic) {
    uint32_t n = tree->desc.bus_count, done, i, k;
    uint8_t *placed = (uint8_t *)(indegree + n);
    memset(indegree, 0, sizeof *indegree * n);
    memset(placed, 0, n);
    for (i = 0u; i < n; ++i) if (tree->buses[i].parent != NONE) indegree[tree->buses[i].parent]++;
    for (i = 0u; i < tree->desc.rule_count; ++i)
        if (!(tree->rules[i].desc.flags & SU_AUDIO_DUCK_DELAY_QUANTUM)) indegree[tree->rules[i].target]++;
    /* Kahn's algorithm; the ready bus with the smallest ID goes first (lookup is ID-sorted). */
    for (done = 0u; done < n; ++done) {
        uint32_t pick = NONE;
        for (k = 0u; k < n; ++k) {
            uint32_t b = tree->lookup[k].index;
            if (!placed[b] && indegree[b] == 0u) { pick = b; break; }
        }
        if (pick == NONE) {
            for (k = 0u; k < n; ++k)
                if (!placed[tree->lookup[k].index])
                    return fail(diagnostic, SU_AUDIO_BUS_DIAG_CYCLE, tree->lookup[k].id, NONE);
        }
        placed[pick] = 1u;
        tree->order[done] = pick;
        if (tree->buses[pick].parent != NONE) indegree[tree->buses[pick].parent]--;
        for (k = 0u; k < tree->buses[pick].source_count; ++k) {
            const Rule *rule = &tree->rules[tree->source_rules[tree->buses[pick].source_first + k]];
            if (!(rule->desc.flags & SU_AUDIO_DUCK_DELAY_QUANTUM)) indegree[rule->target]--;
        }
    }
    return SU_OK;
}

uint32_t su_audio_bus_tree_create(const SuAudioBusTreeDesc *desc, SuAudioBusTree **out,
                                  SuAudioBusDiagnostic *diagnostic) {
    SuAudioBusTree *tree;
    unsigned char *cursor;
    size_t bytes, block_bytes;
    uint32_t i, n, r, roots = 0u, delayed = 0u, status, *indegree;
    if (diagnostic) {
        diagnostic->code = SU_AUDIO_BUS_DIAG_NONE;
        diagnostic->bus_id = 0u;
        diagnostic->rule_index = NONE;
    }
    if (!out) return SU_E_INVALID_ARGUMENT;
    *out = NULL;
    if (!desc || desc->struct_size != sizeof *desc) return fail(diagnostic, SU_AUDIO_BUS_DIAG_DESC, 0u, NONE);
    status = validate(desc, diagnostic);
    if (status != SU_OK) return status;
    n = desc->bus_count;
    r = desc->rule_count;
    for (i = 0u; i < r; ++i) delayed += (desc->rules[i].flags & SU_AUDIO_DUCK_DELAY_QUANTUM) != 0u;
    block_bytes = (size_t)desc->max_block_frames * sizeof(float);
    bytes = round16(sizeof *tree) + round16(sizeof(Bus) * n) + round16(sizeof(Rule) * (r ? r : 1u)) +
            round16(sizeof(IdIndex) * n) + round16(sizeof(uint32_t) * n) + 2u * round16(sizeof(uint32_t) * (r ? r : 1u)) +
            round16(sizeof(uint32_t) * n + n) + (size_t)n * round16(block_bytes * 2u) +
            (size_t)r * round16(block_bytes) + (size_t)delayed * round16(block_bytes);
    tree = (SuAudioBusTree *)calloc(1u, bytes);
    if (!tree) return SU_E_RESOURCE_LIMIT;
    cursor = (unsigned char *)tree;
    take(&cursor, sizeof *tree);
    tree->desc = *desc;
    tree->desc.buses = NULL;
    tree->desc.rules = NULL;
    tree->bytes = bytes;
    tree->buses = (Bus *)take(&cursor, sizeof(Bus) * n);
    tree->rules = (Rule *)take(&cursor, sizeof(Rule) * (r ? r : 1u));
    tree->lookup = (IdIndex *)take(&cursor, sizeof(IdIndex) * n);
    tree->order = (uint32_t *)take(&cursor, sizeof(uint32_t) * n);
    tree->target_rules = (uint32_t *)take(&cursor, sizeof(uint32_t) * (r ? r : 1u));
    tree->source_rules = (uint32_t *)take(&cursor, sizeof(uint32_t) * (r ? r : 1u));
    indegree = (uint32_t *)take(&cursor, sizeof(uint32_t) * n + n);
    for (i = 0u; i < n; ++i) {
        tree->lookup[i].id = desc->buses[i].id;
        tree->lookup[i].index = i;
    }
    qsort(tree->lookup, n, sizeof *tree->lookup, id_compare);
    for (i = 1u; i < n; ++i)
        if (tree->lookup[i].id == tree->lookup[i - 1u].id) {
            SuAudioBusId id = tree->lookup[i].id;
            free(tree);
            return fail(diagnostic, SU_AUDIO_BUS_DIAG_DUPLICATE, id, NONE);
        }
    tree->root = NONE;
    for (i = 0u; i < n; ++i) {
        Bus *bus = &tree->buses[i];
        bus->desc = desc->buses[i];
        bus->in = (float *)take(&cursor, block_bytes * 2u);
        bus->fader = bus->fader_target = db_to_gain(bus->desc.gain_db);
        bus->hdr_gain = 1.0f;
        bus->last_gain = bus->fader;
        bus->loudness = SU_AUDIO_LOUDNESS_SILENT;
        if (bus->desc.parent == SU_AUDIO_BUS_ROOT) {
            bus->parent = NONE;
            tree->root = i;
            roots++;
        } else {
            bus->parent = find(tree, bus->desc.parent);
            if (bus->parent == NONE) {
                SuAudioBusId id = bus->desc.id;
                free(tree);
                return fail(diagnostic, SU_AUDIO_BUS_DIAG_UNKNOWN_BUS, id, NONE);
            }
        }
    }
    if (roots != 1u) {
        free(tree);
        return fail(diagnostic, SU_AUDIO_BUS_DIAG_ROOT, 0u, NONE);
    }
    for (i = 0u; i < r; ++i) {
        Rule *rule = &tree->rules[i];
        rule->desc = desc->rules[i];
        rule->source = find(tree, rule->desc.source);
        rule->target = find(tree, rule->desc.target);
        if (rule->source == NONE || rule->target == NONE) {
            SuAudioBusId id = rule->source == NONE ? rule->desc.source : rule->desc.target;
            free(tree);
            return fail(diagnostic, SU_AUDIO_BUS_DIAG_UNKNOWN_BUS, id, i);
        }
        rule->attack_step = ms_step(rule->desc.max_attenuation_db, rule->desc.attack_ms, desc->sample_rate);
        rule->release_step = ms_step(rule->desc.max_attenuation_db, rule->desc.release_ms, desc->sample_rate);
        rule->hold_samples = (uint32_t)((double)rule->desc.hold_ms * (double)desc->sample_rate / 1000.0 + 0.5);
        rule->slope = 1.0f - 1.0f / rule->desc.ratio;
        rule->det = (float *)take(&cursor, block_bytes);
        if (rule->desc.flags & SU_AUDIO_DUCK_DELAY_QUANTUM) rule->ring = (float *)take(&cursor, block_bytes);
        tree->buses[rule->target].target_count++;
        tree->buses[rule->source].source_count++;
    }
    {
        uint32_t t = 0u, s = 0u;
        for (i = 0u; i < n; ++i) {
            tree->buses[i].target_first = t;
            tree->buses[i].source_first = s;
            t += tree->buses[i].target_count;
            s += tree->buses[i].source_count;
            tree->buses[i].target_count = tree->buses[i].source_count = 0u;
        }
        for (i = 0u; i < r; ++i) { /* rule declaration order within each list */
            Bus *target = &tree->buses[tree->rules[i].target];
            Bus *source = &tree->buses[tree->rules[i].source];
            tree->target_rules[target->target_first + target->target_count++] = i;
            tree->source_rules[source->source_first + source->source_count++] = i;
        }
    }
    status = build_order(tree, indegree, diagnostic);
    if (status != SU_OK) {
        free(tree);
        return status;
    }
    tree->hdr_top = SU_AUDIO_LOUDNESS_SILENT;
    *out = tree;
    return SU_OK;
}

uint32_t su_audio_bus_order(const SuAudioBusTree *tree, uint32_t index, SuAudioBusId *out) {
    if (!tree || !out || index >= tree->desc.bus_count) return SU_E_INVALID_ARGUMENT;
    *out = tree->buses[tree->order[index]].desc.id;
    return SU_OK;
}

uint32_t su_audio_bus_mix(SuAudioBusTree *tree, SuAudioBusId id, const float *stereo, uint32_t frames) {
    uint32_t b, i;
    float *in;
    if (!tree || !stereo || frames == 0u || frames > tree->desc.max_block_frames) return SU_E_INVALID_ARGUMENT;
    if (tree->pending_frames && tree->pending_frames != frames) return SU_E_INVALID_ARGUMENT;
    b = find(tree, id);
    if (b == NONE) return SU_E_INVALID_ARGUMENT;
    tree->pending_frames = frames;
    in = tree->buses[b].in;
    for (i = 0u; i < frames * 2u; ++i) in[i] += stereo[i];
    return SU_OK;
}

uint32_t su_audio_bus_set_loudness(SuAudioBusTree *tree, SuAudioBusId id, float loudness_db) {
    uint32_t b;
    if (!tree || !finite_f(loudness_db)) return SU_E_INVALID_ARGUMENT;
    b = find(tree, id);
    if (b == NONE) return SU_E_INVALID_ARGUMENT;
    if (loudness_db > tree->buses[b].loudness) tree->buses[b].loudness = loudness_db;
    return SU_OK;
}

uint32_t su_audio_bus_set_gain(SuAudioBusTree *tree, SuAudioBusId id, float gain_db, uint32_t ramp_frames) {
    Bus *bus;
    uint32_t b;
    if (!tree || !finite_f(gain_db) || gain_db > 120.0f) return SU_E_INVALID_ARGUMENT;
    b = find(tree, id);
    if (b == NONE) return SU_E_INVALID_ARGUMENT;
    bus = &tree->buses[b];
    bus->fader_target = gain_db <= -200.0f ? 0.0f : db_to_gain(gain_db);
    bus->ramp_remain = ramp_frames;
    if (ramp_frames == 0u) bus->fader = bus->fader_target;
    else bus->fader_step = (bus->fader_target - bus->fader) / (float)ramp_frames;
    return SU_OK;
}

static void update_hdr(SuAudioBusTree *tree, uint32_t frames) {
    const SuAudioHdrPolicy *hdr = &tree->desc.hdr;
    float top = SU_AUDIO_LOUDNESS_SILENT;
    uint32_t i;
    for (i = 0u; i < tree->desc.bus_count; ++i)
        if (tree->buses[i].desc.hdr && tree->buses[i].loudness > top) top = tree->buses[i].loudness;
    if (hdr->release_ms > 0.0f && tree->hdr_top > SU_AUDIO_LOUDNESS_SILENT) {
        double fall = (double)hdr->window_db * (double)frames * 1000.0 / ((double)hdr->release_ms * tree->desc.sample_rate);
        float held = (float)((double)tree->hdr_top - fall);
        if (held > top) top = held;
    }
    tree->hdr_top = top;
}

static float bus_hdr_target(const SuAudioBusTree *tree, Bus *bus) {
    const SuAudioHdrPolicy *hdr = &tree->desc.hdr;
    float offset;
    if (!hdr->enabled || !bus->desc.hdr) { bus->hdr_db = 0.0f; return 1.0f; }
    if (bus->loudness > SU_AUDIO_LOUDNESS_SILENT && bus->loudness < tree->hdr_top - hdr->window_db) {
        bus->hdr_db = 200.0f;
        return 0.0f;
    }
    offset = tree->hdr_top - hdr->ceiling_db;
    if (offset < 0.0f) offset = 0.0f;
    bus->hdr_db = offset;
    return db_to_gain(-offset);
}

static float duck_target(const Rule *rule, float level) {
    float red;
    if (level <= 0.0f) return 0.0f;
    red = (20.0f * log10f(level) - rule->desc.threshold_db) * rule->slope;
    if (red <= 0.0f) return 0.0f;
    return red < rule->desc.max_attenuation_db ? red : rule->desc.max_attenuation_db;
}

static float duck_step(Rule *rule, float level) {
    float target = duck_target(rule, level);
    if (target >= rule->env) {
        rule->env = target - rule->env > rule->attack_step ? rule->env + rule->attack_step : target;
        rule->hold = rule->hold_samples;
    } else if (rule->hold) {
        rule->hold--;
    } else {
        rule->env = rule->env - target > rule->release_step ? rule->env - rule->release_step : target;
    }
    return rule->env;
}

static float catmull(float p0, float p1, float p2, float p3, float t) {
    return 0.5f * (2.0f * p1 + (p2 - p0) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t +
                   (3.0f * p1 - p0 - 3.0f * p2 + p3) * t * t * t);
}

static void meter(Bus *bus, const float *buf, uint32_t frames) {
    uint32_t f, c;
    for (f = 0u; f < frames; ++f) {
        for (c = 0u; c < 2u; ++c) {
            float s = buf[f * 2u + c], a = fabsf(s), *h = bus->hist[c];
            float t1 = fabsf(catmull(h[0], h[1], h[2], s, 0.25f));
            float t2 = fabsf(catmull(h[0], h[1], h[2], s, 0.5f));
            float t3 = fabsf(catmull(h[0], h[1], h[2], s, 0.75f));
            if (a > bus->peak) bus->peak = a;
            if (a > bus->true_peak) bus->true_peak = a;
            if (t1 > bus->true_peak) bus->true_peak = t1;
            if (t2 > bus->true_peak) bus->true_peak = t2;
            if (t3 > bus->true_peak) bus->true_peak = t3;
            bus->sumsq += (double)s * (double)s;
            h[0] = h[1];
            h[1] = h[2];
            h[2] = s;
        }
    }
    bus->frames += frames;
}

uint32_t su_audio_bus_process(SuAudioBusTree *tree, float *out, uint32_t frames) {
    uint32_t i, k, f, block;
    if (!tree || !out || frames == 0u || frames > tree->desc.max_block_frames) return SU_E_INVALID_ARGUMENT;
    if (tree->pending_frames && tree->pending_frames != frames) return SU_E_INVALID_ARGUMENT;
    block = tree->desc.max_block_frames;
    for (i = 0u; i < tree->desc.rule_count; ++i) {
        Rule *rule = &tree->rules[i];
        if (!rule->ring) continue;
        for (f = 0u; f < frames; ++f) rule->det[f] = rule->ring[(tree->ring_pos + f) % block];
    }
    if (tree->desc.hdr.enabled) update_hdr(tree, frames);
    for (k = 0u; k < tree->desc.bus_count; ++k) {
        Bus *bus = &tree->buses[tree->order[k]];
        float *buf = bus->in;
        float hdr_from = bus->hdr_gain, hdr_to = bus_hdr_target(tree, bus);
        float duck = 0.0f, gain = bus->last_gain;
        for (f = 0u; f < frames; ++f) {
            float hdr_gain = hdr_from + (hdr_to - hdr_from) * (float)(f + 1u) / (float)frames;
            uint32_t r;
            if (bus->ramp_remain) {
                bus->ramp_remain--;
                bus->fader = bus->ramp_remain ? bus->fader + bus->fader_step : bus->fader_target;
            }
            duck = 0.0f;
            for (r = 0u; r < bus->target_count; ++r) {
                Rule *rule = &tree->rules[tree->target_rules[bus->target_first + r]];
                duck += duck_step(rule, rule->det[f]);
            }
            if (bus->desc.max_duck_db > 0.0f && duck > bus->desc.max_duck_db) duck = bus->desc.max_duck_db;
            gain = bus->fader * hdr_gain;
            if (duck > 0.0f) gain *= db_to_gain(-duck);
            buf[f * 2u] *= gain;
            buf[f * 2u + 1u] *= gain;
        }
        bus->hdr_gain = hdr_to;
        bus->last_gain = gain;
        bus->last_duck = duck;
        meter(bus, buf, frames);
        for (i = 0u; i < bus->source_count; ++i) {
            Rule *rule = &tree->rules[tree->source_rules[bus->source_first + i]];
            for (f = 0u; f < frames; ++f) {
                float l = fabsf(buf[f * 2u]), r = fabsf(buf[f * 2u + 1u]);
                float level = l > r ? l : r;
                if (rule->ring) rule->ring[(tree->ring_pos + f) % block] = level;
                else rule->det[f] = level;
            }
        }
        if (bus->parent == NONE) {
            memcpy(out, buf, (size_t)frames * 2u * sizeof(float));
        } else {
            float *dst = tree->buses[bus->parent].in;
            for (f = 0u; f < frames * 2u; ++f) dst[f] += buf[f];
        }
        memset(buf, 0, (size_t)frames * 2u * sizeof(float));
        bus->loudness = SU_AUDIO_LOUDNESS_SILENT;
    }
    tree->ring_pos = (tree->ring_pos + frames) % block;
    tree->pending_frames = 0u;
    return SU_OK;
}

uint32_t su_audio_bus_gain_at(const SuAudioBusTree *tree, SuAudioBusId id, float *out) {
    uint32_t b;
    if (!tree || !out) return SU_E_INVALID_ARGUMENT;
    b = find(tree, id);
    if (b == NONE) return SU_E_INVALID_ARGUMENT;
    *out = tree->buses[b].last_gain;
    return SU_OK;
}

uint32_t su_audio_bus_meter_read(SuAudioBusTree *tree, SuAudioBusId id, SuAudioBusMeter *out, int reset) {
    Bus *bus;
    uint32_t b;
    if (!tree || !out) return SU_E_INVALID_ARGUMENT;
    b = find(tree, id);
    if (b == NONE) return SU_E_INVALID_ARGUMENT;
    bus = &tree->buses[b];
    out->peak = bus->peak;
    out->rms = bus->frames ? (float)sqrt(bus->sumsq / (double)(bus->frames * 2u)) : 0.0f;
    out->true_peak = bus->true_peak;
    out->fader_db = bus->fader > 0.0f ? 20.0f * log10f(bus->fader) : -200.0f;
    out->duck_db = bus->last_duck;
    out->hdr_db = bus->hdr_db;
    out->frames = bus->frames;
    if (reset) {
        bus->peak = bus->true_peak = 0.0f;
        bus->sumsq = 0.0;
        bus->frames = 0u;
    }
    return SU_OK;
}

uint64_t su_audio_bus_tree_bytes(const SuAudioBusTree *tree) {
    return tree ? tree->bytes : 0u;
}
