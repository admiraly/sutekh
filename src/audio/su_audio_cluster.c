#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/audio_cluster.h"
#include "su_audio_cluster_int.h"
#include "su_audio_state.h"

#define CLUSTER_PI 3.14159265358979323846
#define DOWNMIX_GAIN 0.70710678f

typedef struct ObjState {
    uint64_t id;
    uint32_t input;
    uint32_t role;
    uint32_t output_id;
    int32_t cluster;          /* cluster slot, -1 when not clustered */
    uint64_t join_clock;
    uint32_t prev_output_id;  /* nonzero while crossfading */
    int32_t prev_cluster;     /* cluster slot holding a draining reference, or -1 */
    SuVec3f prev_dir;
    uint32_t fade_pos;
    SuVec3f dir;              /* unit or zero */
    SuVec3f last_dir;         /* direction at the previous update */
    float rms;
    uint32_t metadata;
    uint32_t stayed;
    /* Assignment at the start of the current update. */
    uint32_t orig_output_id;
    uint32_t orig_prev_output_id;
    SuVec3f orig_prev_dir;
    uint32_t orig_fade_pos;
    int32_t orig_prev_cluster;
    uint64_t orig_join_clock;
} ObjState;

typedef struct ClusterSlot {
    uint32_t used;
    uint32_t id;
    uint32_t members;
    uint32_t metadata;
    uint32_t draining_refs;
    SuVec3f centroid;
    float weight;
    double acc[7];
} ClusterSlot;

struct SuAudioCluster {
    SuAudioClusterDesc desc;
    ObjState *states[2];
    uint32_t counts[2];
    uint32_t current;
    uint32_t *order;
    uint32_t *scratch;
    ClusterSlot *clusters;
    uint32_t next_id;
    uint64_t clock;
    SuAudioClusterStats stats;
};

static SuVec3f normalize(SuVec3f v) {
    double length = sqrt((double)v.x * v.x + (double)v.y * v.y + (double)v.z * v.z);
    SuVec3f out = {0.0f, 0.0f, 0.0f};
    if (length > 1e-12) {
        out.x = (float)(v.x / length);
        out.y = (float)(v.y / length);
        out.z = (float)(v.z / length);
    }
    return out;
}

static int is_zero(SuVec3f v) {
    return v.x == 0.0f && v.y == 0.0f && v.z == 0.0f;
}

static double angle(SuVec3f a, SuVec3f b) {
    double dot;
    if (is_zero(a) || is_zero(b)) {
        return is_zero(a) && is_zero(b) ? 0.0 : CLUSTER_PI;
    }
    dot = (double)a.x * b.x + (double)a.y * b.y + (double)a.z * b.z;
    if (dot > 1.0) {
        dot = 1.0;
    } else if (dot < -1.0) {
        dot = -1.0;
    }
    return acos(dot);
}

uint32_t su_audio_cluster_create(const SuAudioClusterDesc *desc, SuAudioCluster **out) {
    SuAudioCluster *cluster;
    size_t n;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (desc == NULL || desc->struct_size != sizeof *desc || !(desc->width_rad > 0.0f)
        || !(desc->width_rad <= (float)CLUSTER_PI) || !(desc->leave_ratio >= 1.0f) || !isfinite(desc->leave_ratio)
        || !(desc->proximity_m >= 0.0f) || !isfinite(desc->proximity_m) || desc->output_limit == 0u
        || desc->max_objects == 0u || desc->max_objects > SU_AUDIO_CLUSTER_MAX_OBJECTS) {
        return SU_E_INVALID_ARGUMENT;
    }
    cluster = (SuAudioCluster *)calloc(1u, sizeof *cluster);
    if (cluster == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    n = desc->max_objects;
    cluster->desc = *desc;
    cluster->states[0] = (ObjState *)calloc(n, sizeof(ObjState));
    cluster->states[1] = (ObjState *)calloc(n, sizeof(ObjState));
    cluster->order = (uint32_t *)calloc(n, sizeof(uint32_t));
    cluster->scratch = (uint32_t *)calloc(n, sizeof(uint32_t));
    cluster->clusters = (ClusterSlot *)calloc(n, sizeof(ClusterSlot));
    if (cluster->states[0] == NULL || cluster->states[1] == NULL || cluster->order == NULL
        || cluster->scratch == NULL || cluster->clusters == NULL) {
        su_audio_cluster_destroy(cluster);
        return SU_E_RESOURCE_LIMIT;
    }
    cluster->next_id = 1u;
    cluster->stats.runtime_bytes = (uint64_t)(sizeof *cluster + n * (2u * sizeof(ObjState)
        + 2u * sizeof(uint32_t) + sizeof(ClusterSlot)));
    *out = cluster;
    return SU_OK;
}

void su_audio_cluster_destroy(SuAudioCluster *cluster) {
    if (cluster == NULL) {
        return;
    }
    free(cluster->states[0]);
    free(cluster->states[1]);
    free(cluster->order);
    free(cluster->scratch);
    free(cluster->clusters);
    free(cluster);
}

/* Stable bottom-up merge sort of indices by object ID. */
static void sort_by_id(SuAudioCluster *cluster, const SuAudioClusterObject *objects, uint32_t count) {
    uint32_t width;
    uint32_t i;
    uint32_t *src = cluster->order;
    uint32_t *dst = cluster->scratch;
    for (i = 0u; i < count; i++) {
        src[i] = i;
    }
    for (width = 1u; width < count; width *= 2u) {
        uint32_t lo;
        for (lo = 0u; lo < count; lo += 2u * width) {
            uint32_t mid = lo + width < count ? lo + width : count;
            uint32_t hi = mid + width < count ? mid + width : count;
            uint32_t a = lo;
            uint32_t b = mid;
            uint32_t k = lo;
            while (a < mid && b < hi) {
                dst[k++] = objects[src[b]].id < objects[src[a]].id ? src[b++] : src[a++];
            }
            while (a < mid) {
                dst[k++] = src[a++];
            }
            while (b < hi) {
                dst[k++] = src[b++];
            }
        }
        {
            uint32_t *swap = src;
            src = dst;
            dst = swap;
        }
        if (width > count / 2u) {
            break;
        }
    }
    if (src != cluster->order) {
        memcpy(cluster->order, src, count * sizeof(uint32_t));
    }
}

static const ObjState *find_state(const ObjState *states, uint32_t count, uint64_t id) {
    uint32_t lo = 0u;
    uint32_t hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (states[mid].id == id) {
            return &states[mid];
        }
        if (states[mid].id < id) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

static void end_fade(SuAudioCluster *cluster, ObjState *s) {
    if (s->prev_cluster >= 0 && cluster->clusters[s->prev_cluster].draining_refs > 0u) {
        cluster->clusters[s->prev_cluster].draining_refs--;
    }
    s->prev_cluster = -1;
    s->prev_output_id = 0u;
    s->fade_pos = 0u;
}

/* Starts a crossfade away from the object's current output (if any). */
static void begin_fade(SuAudioCluster *cluster, ObjState *s, uint32_t old_id, SuVec3f old_dir, int32_t old_slot) {
    end_fade(cluster, s);
    if (old_id == 0u || cluster->desc.crossfade_frames == 0u) {
        return;
    }
    s->prev_output_id = old_id;
    s->prev_dir = old_dir;
    if (old_slot >= 0) {
        s->prev_cluster = old_slot;
        cluster->clusters[old_slot].draining_refs++;
    }
}

static int32_t new_cluster(SuAudioCluster *cluster, const ObjState *s) {
    uint32_t i;
    for (i = 0u; i < cluster->desc.max_objects; i++) {
        ClusterSlot *slot = &cluster->clusters[i];
        if (!slot->used) {
            memset(slot, 0, sizeof *slot);
            slot->used = 1u;
            slot->id = cluster->next_id++;
            slot->metadata = s->metadata;
            slot->centroid = s->dir;
            cluster->stats.created++;
            return (int32_t)i;
        }
    }
    return -1;
}

static void join(SuAudioCluster *cluster, ObjState *s, int32_t slot, uint32_t old_id, SuVec3f old_dir, int32_t old_slot) {
    uint32_t id = cluster->clusters[slot].id;
    if (id == s->orig_output_id && old_id != id) {
        /* Back on the output it had before this update (for example a transient
         * cluster merged back by the budget): undo this update's fades. */
        end_fade(cluster, s);
        s->prev_output_id = s->orig_prev_output_id;
        s->prev_dir = s->orig_prev_dir;
        s->fade_pos = s->orig_fade_pos;
        s->prev_cluster = s->orig_prev_cluster;
        if (s->prev_cluster >= 0) {
            cluster->clusters[s->prev_cluster].draining_refs++;
        }
        s->join_clock = s->orig_join_clock;
    } else {
        if (old_id != 0u && old_id != id) {
            begin_fade(cluster, s, old_id, old_dir, old_slot);
            cluster->stats.moves++;
        }
        s->join_clock = cluster->clock;
    }
    s->cluster = slot;
    s->output_id = id;
    cluster->clusters[slot].members++;
    cluster->stats.joins++;
}

static void recompute(SuAudioCluster *cluster, ObjState *states, uint32_t count) {
    uint32_t i;
    uint32_t c;
    for (c = 0u; c < cluster->desc.max_objects; c++) {
        memset(cluster->clusters[c].acc, 0, sizeof cluster->clusters[c].acc);
        cluster->clusters[c].weight = 0.0f;
    }
    /* Objects are accumulated in ID order, so sums do not depend on input order. */
    for (i = 0u; i < count; i++) {
        const ObjState *s = &states[i];
        double *acc;
        if (s->cluster < 0) {
            continue;
        }
        acc = cluster->clusters[s->cluster].acc;
        acc[0] += (double)s->dir.x * s->rms;
        acc[1] += (double)s->dir.y * s->rms;
        acc[2] += (double)s->dir.z * s->rms;
        acc[3] += s->dir.x;
        acc[4] += s->dir.y;
        acc[5] += s->dir.z;
        acc[6] += s->rms;
    }
    for (c = 0u; c < cluster->desc.max_objects; c++) {
        ClusterSlot *slot = &cluster->clusters[c];
        double wx = slot->acc[0];
        double wy = slot->acc[1];
        double wz = slot->acc[2];
        double px = slot->acc[3];
        double py = slot->acc[4];
        double pz = slot->acc[5];
        double weight = slot->acc[6];
        double length;
        if (!slot->used || slot->members == 0u) {
            continue;
        }
        if (!(weight > 0.0)) {
            wx = px;
            wy = py;
            wz = pz;
            weight = (double)slot->members;
        }
        length = sqrt(wx * wx + wy * wy + wz * wz);
        slot->weight = (float)weight;
        if (length <= 1e-6 * weight) {
            slot->centroid.x = slot->centroid.y = slot->centroid.z = 0.0f;
        } else {
            slot->centroid.x = (float)(wx / length);
            slot->centroid.y = (float)(wy / length);
            slot->centroid.z = (float)(wz / length);
        }
    }
}

static uint32_t live_outputs(const SuAudioCluster *cluster, uint32_t dedicated) {
    uint32_t c;
    uint32_t live = dedicated;
    for (c = 0u; c < cluster->desc.max_objects; c++) {
        live += cluster->clusters[c].used && cluster->clusters[c].members != 0u;
    }
    return live;
}

uint32_t su_audio_cluster_update(SuAudioCluster *cluster, const SuAudioClusterObject *objects, uint32_t count) {
    const ObjState *old_states;
    ObjState *states;
    uint32_t old_count;
    uint32_t i;
    uint32_t c;
    uint32_t dedicated = 0u;
    double leave;
    if (cluster == NULL || (count != 0u && objects == NULL) || count > cluster->desc.max_objects) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (i = 0u; i < count; i++) {
        const SuAudioClusterObject *o = &objects[i];
        if (!isfinite(o->direction.x) || !isfinite(o->direction.y) || !isfinite(o->direction.z)
            || !isfinite(o->distance_m) || !(o->rms >= 0.0f) || !isfinite(o->rms)
            || (o->channels != 1u && o->channels != 2u)) {
            return SU_E_INVALID_ARGUMENT;
        }
    }
    sort_by_id(cluster, objects, count);
    for (i = 1u; i < count; i++) {
        if (objects[cluster->order[i]].id == objects[cluster->order[i - 1u]].id) {
            return SU_E_DUPLICATE_KEY;
        }
    }
    old_states = cluster->states[cluster->current];
    old_count = cluster->counts[cluster->current];
    states = cluster->states[1u - cluster->current];
    /* Departed objects release their draining references. */
    for (i = 0u; i < old_count; i++) {
        uint64_t id = old_states[i].id;
        uint32_t lo = 0u;
        uint32_t hi = count;
        int present = 0;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2u;
            uint64_t mid_id = objects[cluster->order[mid]].id;
            if (mid_id == id) {
                present = 1;
                break;
            }
            if (mid_id < id) {
                lo = mid + 1u;
            } else {
                hi = mid;
            }
        }
        if (!present && old_states[i].prev_cluster >= 0
            && cluster->clusters[old_states[i].prev_cluster].draining_refs > 0u) {
            cluster->clusters[old_states[i].prev_cluster].draining_refs--;
        }
    }
    for (c = 0u; c < cluster->desc.max_objects; c++) {
        cluster->clusters[c].members = 0u;
    }
    /* Carry state and classify. */
    for (i = 0u; i < count; i++) {
        const SuAudioClusterObject *o = &objects[cluster->order[i]];
        const ObjState *prev = find_state(old_states, old_count, o->id);
        ObjState *s = &states[i];
        if (prev != NULL) {
            *s = *prev;
        } else {
            memset(s, 0, sizeof *s);
            s->id = o->id;
            s->cluster = -1;
            s->prev_cluster = -1;
        }
        s->input = cluster->order[i];
        s->last_dir = prev != NULL ? prev->dir : normalize(o->direction);
        s->stayed = 0u;
        s->orig_output_id = s->output_id;
        s->orig_prev_output_id = s->prev_output_id;
        s->orig_prev_dir = s->prev_dir;
        s->orig_fade_pos = s->fade_pos;
        s->orig_prev_cluster = s->prev_cluster;
        s->orig_join_clock = s->join_clock;
        s->dir = normalize(o->direction);
        s->rms = o->rms;
        s->metadata = o->metadata;
        if ((o->flags & SU_AUDIO_CLUSTER_BED) != 0u) {
            s->role = SU_AUDIO_CLUSTER_ROLE_BED;
        } else if ((o->flags & SU_AUDIO_CLUSTER_EXCLUDE) != 0u) {
            s->role = SU_AUDIO_CLUSTER_ROLE_EXCLUDED;
        } else if (cluster->desc.priority_threshold != 0u && o->priority >= cluster->desc.priority_threshold) {
            s->role = SU_AUDIO_CLUSTER_ROLE_PRIORITY;
        } else if (o->distance_m < cluster->desc.proximity_m) {
            s->role = SU_AUDIO_CLUSTER_ROLE_PROXIMITY;
        } else {
            s->role = SU_AUDIO_CLUSTER_ROLE_CLUSTERED;
        }
    }
    cluster->counts[1u - cluster->current] = count;
    cluster->current = 1u - cluster->current;
    leave = (double)cluster->desc.width_rad * (double)cluster->desc.leave_ratio;
    /* Beds and dedicated objects; clustered objects that may stay. */
    for (i = 0u; i < count; i++) {
        ObjState *s = &states[i];
        int32_t old_slot = s->cluster;
        if (s->role == SU_AUDIO_CLUSTER_ROLE_BED) {
            end_fade(cluster, s);
            s->cluster = -1;
            s->output_id = 0u;
            continue;
        }
        if (s->role != SU_AUDIO_CLUSTER_ROLE_CLUSTERED) {
            dedicated++;
            if (old_slot >= 0) {
                uint32_t new_id = cluster->next_id++;
                begin_fade(cluster, s, s->output_id, cluster->clusters[old_slot].centroid, old_slot);
                s->output_id = new_id;
                s->cluster = -1;
            } else if (s->output_id == 0u) {
                s->output_id = cluster->next_id++;
            }
            continue;
        }
        if (old_slot >= 0) {
            const ClusterSlot *slot = &cluster->clusters[old_slot];
            int held = cluster->clock < s->join_clock + cluster->desc.hold_frames;
            if (slot->used && slot->metadata == s->metadata && (held || angle(s->dir, slot->centroid) <= leave)) {
                cluster->clusters[old_slot].members++;
                s->stayed = 1u;
                continue;
            }
        }
    }
    /* Unassigned clustered objects join or create in ID order. */
    for (i = 0u; i < count; i++) {
        ObjState *s = &states[i];
        int32_t old_slot = s->cluster;
        int32_t best = -1;
        double best_angle = 0.0;
        SuVec3f old_dir;
        uint32_t old_id = s->output_id;
        if (s->role != SU_AUDIO_CLUSTER_ROLE_CLUSTERED || s->stayed) {
            continue;
        }
        /* A previously dedicated object fades from its own previous direction. */
        old_dir = old_slot >= 0 ? cluster->clusters[old_slot].centroid : s->last_dir;
        for (c = 0u; c < cluster->desc.max_objects; c++) {
            const ClusterSlot *slot = &cluster->clusters[c];
            double a;
            if (!slot->used || slot->metadata != s->metadata) {
                continue;
            }
            a = angle(s->dir, slot->centroid);
            if (a <= (double)cluster->desc.width_rad
                && (best < 0 || a < best_angle || (a == best_angle && slot->id < cluster->clusters[best].id))) {
                best = (int32_t)c;
                best_angle = a;
            }
        }
        if (best < 0) {
            best = new_cluster(cluster, s);
            if (best < 0) {
                return SU_E_INTERNAL;
            }
        }
        join(cluster, s, best, old_id, old_dir, old_slot);
    }
    recompute(cluster, states, count);
    /* Budget: merge the closest compatible pair until the output limit holds. */
    cluster->stats.infeasible = 0u;
    while (live_outputs(cluster, dedicated) > cluster->desc.output_limit) {
        int32_t keep = -1;
        int32_t drop = -1;
        double best_angle = 0.0;
        uint32_t a_index;
        uint32_t b_index;
        for (a_index = 0u; a_index < cluster->desc.max_objects; a_index++) {
            const ClusterSlot *a = &cluster->clusters[a_index];
            if (!a->used || a->members == 0u) {
                continue;
            }
            for (b_index = 0u; b_index < cluster->desc.max_objects; b_index++) {
                const ClusterSlot *b = &cluster->clusters[b_index];
                double d;
                if (b_index == a_index || !b->used || b->members == 0u || b->metadata != a->metadata || b->id < a->id) {
                    continue;
                }
                d = angle(a->centroid, b->centroid);
                if (keep < 0 || d < best_angle
                    || (d == best_angle && (a->id < cluster->clusters[keep].id
                        || (a->id == cluster->clusters[keep].id && b->id < cluster->clusters[drop].id)))) {
                    keep = (int32_t)a_index;
                    drop = (int32_t)b_index;
                    best_angle = d;
                }
            }
        }
        if (keep < 0) {
            cluster->stats.infeasible = 1u;
            break;
        }
        for (i = 0u; i < count; i++) {
            ObjState *s = &states[i];
            if (s->cluster == drop) {
                cluster->clusters[drop].members--;
                join(cluster, s, keep, s->output_id, cluster->clusters[drop].centroid, drop);
            }
        }
        cluster->stats.merges++;
        recompute(cluster, states, count);
    }
    for (c = 0u; c < cluster->desc.max_objects; c++) {
        ClusterSlot *slot = &cluster->clusters[c];
        if (slot->used && slot->members == 0u && slot->draining_refs == 0u) {
            slot->used = 0u;
        }
    }
    cluster->stats.objects = count;
    cluster->stats.beds = 0u;
    cluster->stats.fading = 0u;
    for (i = 0u; i < count; i++) {
        cluster->stats.beds += states[i].role == SU_AUDIO_CLUSTER_ROLE_BED;
        cluster->stats.fading += states[i].prev_output_id != 0u;
    }
    cluster->stats.dedicated = dedicated;
    cluster->stats.clusters = 0u;
    cluster->stats.draining = 0u;
    for (c = 0u; c < cluster->desc.max_objects; c++) {
        if (cluster->clusters[c].used) {
            if (cluster->clusters[c].members != 0u) {
                cluster->stats.clusters++;
            } else {
                cluster->stats.draining++;
            }
        }
    }
    cluster->stats.outputs = cluster->stats.clusters + dedicated;
    return SU_OK;
}

static SuVec3f output_dir(const SuAudioCluster *cluster, const ObjState *s) {
    if (s->cluster >= 0) {
        return cluster->clusters[s->cluster].centroid;
    }
    return s->dir;
}

uint32_t su_audio_cluster_render(SuAudioCluster *cluster, const SuAudioClusterObject *objects, uint32_t count,
                                 uint32_t frames, float *out_stereo) {
    ObjState *states;
    uint32_t i;
    uint32_t f;
    if (cluster == NULL || out_stereo == NULL || (count != 0u && objects == NULL)
        || count != cluster->counts[cluster->current]) {
        return SU_E_INVALID_ARGUMENT;
    }
    states = cluster->states[cluster->current];
    for (i = 0u; i < count; i++) {
        if (states[i].input >= count || objects[states[i].input].id != states[i].id
            || objects[states[i].input].pcm == NULL) {
            return SU_E_INVALID_ARGUMENT;
        }
    }
    memset(out_stereo, 0, (size_t)frames * 2u * sizeof *out_stereo);
    for (i = 0u; i < count; i++) {
        ObjState *s = &states[i];
        const SuAudioClusterObject *o = &objects[s->input];
        SuVec3f zero = {0.0f, 0.0f, 0.0f};
        float new_l;
        float new_r;
        float old_l = 0.0f;
        float old_r = 0.0f;
        uint32_t fade = cluster->desc.crossfade_frames;
        if (s->role == SU_AUDIO_CLUSTER_ROLE_BED) {
            su_audio_pan(zero, &new_l, &new_r);
            for (f = 0u; f < frames; f++) {
                if (o->channels == 2u) {
                    out_stereo[f * 2u] += o->pcm[f * 2u];
                    out_stereo[f * 2u + 1u] += o->pcm[f * 2u + 1u];
                } else {
                    out_stereo[f * 2u] += o->pcm[f] * new_l;
                    out_stereo[f * 2u + 1u] += o->pcm[f] * new_r;
                }
            }
            continue;
        }
        su_audio_pan(output_dir(cluster, s), &new_l, &new_r);
        if (s->prev_output_id != 0u) {
            su_audio_pan(s->prev_dir, &old_l, &old_r);
        }
        for (f = 0u; f < frames; f++) {
            float x = o->channels == 2u ? (o->pcm[f * 2u] + o->pcm[f * 2u + 1u]) * DOWNMIX_GAIN : o->pcm[f];
            float g = 1.0f;
            if (s->prev_output_id != 0u && s->fade_pos + f < fade) {
                g = (float)((double)(s->fade_pos + f) / (double)fade);
            }
            out_stereo[f * 2u] += x * (g * new_l + (1.0f - g) * old_l);
            out_stereo[f * 2u + 1u] += x * (g * new_r + (1.0f - g) * old_r);
        }
        if (s->prev_output_id != 0u) {
            if ((uint64_t)s->fade_pos + frames >= fade) {
                end_fade(cluster, s);
            } else {
                s->fade_pos += frames;
            }
        }
    }
    cluster->clock += frames;
    cluster->stats.clock = cluster->clock;
    return SU_OK;
}

uint32_t su_audio_cluster_outputs(const SuAudioCluster *cluster, SuAudioClusterOutput *out, uint32_t capacity,
                                  uint32_t *count) {
    uint32_t n = 0u;
    uint32_t i;
    uint32_t j;
    const ObjState *states;
    if (count == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *count = 0u;
    if (cluster == NULL || (capacity != 0u && out == NULL)) {
        return SU_E_INVALID_ARGUMENT;
    }
    states = cluster->states[cluster->current];
    for (i = 0u; i < cluster->desc.max_objects; i++) {
        const ClusterSlot *slot = &cluster->clusters[i];
        if (!slot->used) {
            continue;
        }
        if (n < capacity) {
            memset(&out[n], 0, sizeof out[n]);
            out[n].output_id = slot->id;
            out[n].kind = SU_AUDIO_CLUSTER_OUTPUT_CLUSTER;
            out[n].direction = slot->centroid;
            out[n].rms_weight = slot->weight;
            out[n].members = slot->members;
            out[n].metadata = slot->metadata;
            out[n].draining = slot->members == 0u;
        }
        n++;
    }
    for (i = 0u; i < cluster->counts[cluster->current]; i++) {
        const ObjState *s = &states[i];
        if (s->role == SU_AUDIO_CLUSTER_ROLE_BED || s->role == SU_AUDIO_CLUSTER_ROLE_CLUSTERED) {
            continue;
        }
        if (n < capacity) {
            memset(&out[n], 0, sizeof out[n]);
            out[n].output_id = s->output_id;
            out[n].kind = SU_AUDIO_CLUSTER_OUTPUT_DEDICATED;
            out[n].direction = s->dir;
            out[n].rms_weight = s->rms;
            out[n].members = 1u;
            out[n].metadata = s->metadata;
            out[n].object_id = s->id;
        }
        n++;
    }
    *count = n;
    if (n > capacity) {
        return SU_E_RESOURCE_LIMIT;
    }
    /* Stable diagnostic order: ascending output ID. */
    for (i = 1u; i < n; i++) {
        SuAudioClusterOutput item = out[i];
        j = i;
        while (j > 0u && out[j - 1u].output_id > item.output_id) {
            out[j] = out[j - 1u];
            j--;
        }
        out[j] = item;
    }
    return SU_OK;
}

uint32_t su_audio_cluster_assoc(const SuAudioCluster *cluster, SuAudioClusterAssoc *out, uint32_t capacity,
                                uint32_t *count) {
    uint32_t i;
    uint32_t n;
    const ObjState *states;
    if (count == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *count = 0u;
    if (cluster == NULL || (capacity != 0u && out == NULL)) {
        return SU_E_INVALID_ARGUMENT;
    }
    n = cluster->counts[cluster->current];
    states = cluster->states[cluster->current];
    *count = n;
    if (n > capacity) {
        return SU_E_RESOURCE_LIMIT;
    }
    for (i = 0u; i < n; i++) {
        const ObjState *s = &states[i];
        memset(&out[i], 0, sizeof out[i]);
        out[i].object_id = s->id;
        out[i].role = s->role;
        out[i].output_id = s->output_id;
        out[i].previous_output_id = s->prev_output_id;
        out[i].gain = 1.0f;
        out[i].previous_gain = 0.0f;
        if (s->prev_output_id != 0u && cluster->desc.crossfade_frames != 0u) {
            out[i].gain = (float)((double)s->fade_pos / (double)cluster->desc.crossfade_frames);
            out[i].previous_gain = 1.0f - out[i].gain;
        }
    }
    return SU_OK;
}

void su_audio_cluster_stats(const SuAudioCluster *cluster, SuAudioClusterStats *out) {
    if (out == NULL) {
        return;
    }
    if (cluster == NULL) {
        memset(out, 0, sizeof *out);
        return;
    }
    *out = cluster->stats;
    out->clock = cluster->clock;
}

uint32_t su_audio_cluster_capacity(const SuAudioCluster *cluster) {
    return cluster != NULL ? cluster->desc.max_objects : 0u;
}

/* Checkpoint state between updates: current object assignments, cluster slots, IDs, clock and stats. */
uint32_t su_audio_cluster_state(SuAudioCluster *cluster, SuAudioStateIo *io) {
    uint32_t i;
    uint32_t count;
    if (cluster == NULL || io == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    SU_STATE(io, cluster->current);
    count = cluster->counts[cluster->current & 1u];
    SU_STATE(io, count);
    if (io->failed || cluster->current > 1u || count > cluster->desc.max_objects) {
        return SU_E_SCHEMA_MISMATCH;
    }
    cluster->counts[cluster->current] = count;
    for (i = 0u; i < count; i++) {
        SU_STATE(io, cluster->states[cluster->current][i]);
    }
    for (i = 0u; i < cluster->desc.max_objects; i++) {
        SU_STATE(io, cluster->clusters[i].used);
        if (cluster->clusters[i].used) {
            SU_STATE(io, cluster->clusters[i]);
        }
    }
    SU_STATE(io, cluster->next_id);
    SU_STATE(io, cluster->clock);
    SU_STATE(io, cluster->stats);
    return io->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}
