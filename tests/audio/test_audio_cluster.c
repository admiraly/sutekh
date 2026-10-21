#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sutekh/audio_cluster.h"

static unsigned checks;
static unsigned failures;
static unsigned long allocations;

#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        failures++; \
        fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
    } \
} while (0)

/* The cluster source is compiled with these allocator names (see CMake). */
void *a08_malloc(size_t size);
void *a08_calloc(size_t count, size_t size);
void a08_free(void *ptr);
void *a08_malloc(size_t size) {
    allocations++;
    return malloc(size);
}
void *a08_calloc(size_t count, size_t size) {
    allocations++;
    return calloc(count, size);
}
void a08_free(void *ptr) {
    free(ptr);
}

#define FRAMES 512u
#define PI_D 3.14159265358979323846

static float signal_a[FRAMES * 2u];
static float signal_b[FRAMES * 2u];
static float stereo_left[FRAMES * 2u];

static SuVec3f azimuth(double radians) {
    SuVec3f v;
    v.x = (float)sin(radians);
    v.y = 0.0f;
    v.z = (float)cos(radians);
    return v;
}

static SuAudioClusterObject object(uint64_t id, double az, float rms) {
    SuAudioClusterObject o;
    memset(&o, 0, sizeof o);
    o.id = id;
    o.direction = azimuth(az);
    o.distance_m = 100.0f;
    o.rms = rms;
    o.channels = 1u;
    o.pcm = signal_a;
    return o;
}

static SuAudioClusterDesc desc_default(void) {
    SuAudioClusterDesc d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.width_rad = 0.3f;
    d.leave_ratio = 1.5f;
    d.hold_frames = 0u;
    d.crossfade_frames = 0u;
    d.proximity_m = 0.0f;
    d.priority_threshold = 0u;
    d.output_limit = 16u;
    d.max_objects = 64u;
    return d;
}

static SuAudioCluster *make(const SuAudioClusterDesc *d) {
    SuAudioCluster *c = NULL;
    CHECK(su_audio_cluster_create(d, &c) == SU_OK);
    return c;
}

static SuAudioClusterAssoc find_assoc(SuAudioCluster *c, uint64_t id) {
    SuAudioClusterAssoc assoc[64];
    SuAudioClusterAssoc none;
    uint32_t n = 0u;
    uint32_t i;
    memset(&none, 0, sizeof none);
    CHECK(su_audio_cluster_assoc(c, assoc, 64u, &n) == SU_OK);
    for (i = 0u; i < n; i++) {
        if (assoc[i].object_id == id) {
            return assoc[i];
        }
    }
    return none;
}

static double energy(const float *pcm, uint32_t frames, double *left, double *right) {
    uint32_t i;
    double l = 0.0;
    double r = 0.0;
    for (i = 0u; i < frames; i++) {
        l += (double)pcm[i * 2u] * pcm[i * 2u];
        r += (double)pcm[i * 2u + 1u] * pcm[i * 2u + 1u];
    }
    if (left != NULL) {
        *left = l;
    }
    if (right != NULL) {
        *right = r;
    }
    return l + r;
}

static void validation(void) {
    SuAudioClusterDesc d = desc_default();
    SuAudioCluster *c = NULL;
    SuAudioClusterObject objs[2];
    float out[FRAMES * 2u];
    d.leave_ratio = 0.5f;
    CHECK(su_audio_cluster_create(&d, &c) == SU_E_INVALID_ARGUMENT && c == NULL);
    d = desc_default();
    d.output_limit = 0u;
    CHECK(su_audio_cluster_create(&d, &c) == SU_E_INVALID_ARGUMENT);
    d = desc_default();
    d.width_rad = 4.0f;
    CHECK(su_audio_cluster_create(&d, &c) == SU_E_INVALID_ARGUMENT);
    d = desc_default();
    d.struct_size = 4u;
    CHECK(su_audio_cluster_create(&d, &c) == SU_E_INVALID_ARGUMENT);
    d = desc_default();
    c = make(&d);
    objs[0] = object(5u, 0.0, 1.0f);
    objs[1] = object(9u, 0.05, 1.0f);
    CHECK(su_audio_cluster_update(c, objs, 2u) == SU_OK);
    objs[1].id = 5u;
    CHECK(su_audio_cluster_update(c, objs, 2u) == SU_E_DUPLICATE_KEY);
    objs[1].id = 9u;
    objs[1].rms = -1.0f;
    CHECK(su_audio_cluster_update(c, objs, 2u) == SU_E_INVALID_ARGUMENT);
    objs[1].rms = 1.0f;
    objs[1].direction.x = (float)NAN;
    CHECK(su_audio_cluster_update(c, objs, 2u) == SU_E_INVALID_ARGUMENT);
    objs[1] = object(9u, 0.05, 1.0f);
    /* Refused updates leave the previous assignment renderable. */
    CHECK(su_audio_cluster_render(c, objs, 2u, FRAMES, out) == SU_OK);
    CHECK(su_audio_cluster_render(c, objs, 1u, FRAMES, out) == SU_E_INVALID_ARGUMENT);
    su_audio_cluster_destroy(c);
}

static void close_merge_and_centroid(void) {
    SuAudioClusterDesc d = desc_default();
    SuAudioCluster *c = make(&d);
    SuAudioClusterObject objs[4];
    SuAudioClusterOutput outputs[8];
    SuAudioClusterStats stats;
    uint32_t n = 0u;
    double x;
    double z;
    double length;
    objs[0] = object(1u, -0.1, 1.0f);
    objs[1] = object(2u, 0.0, 1.0f);
    objs[2] = object(3u, 0.1, 2.0f);
    objs[3] = object(4u, 2.0, 1.0f);
    CHECK(su_audio_cluster_update(c, objs, 4u) == SU_OK);
    CHECK(su_audio_cluster_outputs(c, outputs, 8u, &n) == SU_OK && n == 2u);
    CHECK(outputs[0].kind == SU_AUDIO_CLUSTER_OUTPUT_CLUSTER && outputs[0].members == 3u);
    CHECK(outputs[1].members == 1u && outputs[0].output_id < outputs[1].output_id);
    /* RMS-weighted centroid of the three close directions. */
    x = sin(-0.1) + 0.0 + 2.0 * sin(0.1);
    z = cos(-0.1) + 1.0 + 2.0 * cos(0.1);
    length = sqrt(x * x + z * z);
    CHECK(fabs(outputs[0].direction.x - x / length) < 1e-5 && fabs(outputs[0].direction.z - z / length) < 1e-5);
    CHECK(fabs(outputs[0].rms_weight - 4.0f) < 1e-6f);
    CHECK(find_assoc(c, 1u).output_id == outputs[0].output_id && find_assoc(c, 4u).output_id == outputs[1].output_id);
    su_audio_cluster_stats(c, &stats);
    CHECK(stats.clusters == 2u && stats.outputs == 2u && stats.objects == 4u && stats.infeasible == 0u);
    CHECK(su_audio_cluster_outputs(c, outputs, 1u, &n) == SU_E_RESOURCE_LIMIT && n == 2u);
    su_audio_cluster_destroy(c);
}

static void hero_and_exclusions(void) {
    SuAudioClusterDesc d = desc_default();
    SuAudioCluster *c;
    SuAudioClusterObject objs[5];
    SuAudioClusterOutput outputs[8];
    uint32_t n = 0u;
    d.priority_threshold = 100u;
    d.proximity_m = 5.0f;
    c = make(&d);
    objs[0] = object(1u, 0.0, 1.0f);
    objs[1] = object(2u, 0.02, 1.0f);
    objs[2] = object(3u, 0.01, 1.0f);
    objs[2].priority = 100u;
    objs[3] = object(4u, -0.01, 1.0f);
    objs[3].distance_m = 1.0f;
    objs[4] = object(5u, 0.03, 1.0f);
    objs[4].flags = SU_AUDIO_CLUSTER_EXCLUDE;
    CHECK(su_audio_cluster_update(c, objs, 5u) == SU_OK);
    CHECK(su_audio_cluster_outputs(c, outputs, 8u, &n) == SU_OK && n == 4u);
    CHECK(find_assoc(c, 3u).role == SU_AUDIO_CLUSTER_ROLE_PRIORITY);
    CHECK(find_assoc(c, 4u).role == SU_AUDIO_CLUSTER_ROLE_PROXIMITY);
    CHECK(find_assoc(c, 5u).role == SU_AUDIO_CLUSTER_ROLE_EXCLUDED);
    CHECK(find_assoc(c, 1u).output_id == find_assoc(c, 2u).output_id);
    CHECK(find_assoc(c, 3u).output_id != find_assoc(c, 1u).output_id);
    CHECK(outputs[1].kind == SU_AUDIO_CLUSTER_OUTPUT_DEDICATED || outputs[0].kind == SU_AUDIO_CLUSTER_OUTPUT_DEDICATED);
    /* The hero stays distinct on later updates even under a tight budget. */
    d.output_limit = 1u;
    su_audio_cluster_destroy(c);
    c = make(&d);
    CHECK(su_audio_cluster_update(c, objs, 5u) == SU_OK);
    CHECK(find_assoc(c, 3u).role == SU_AUDIO_CLUSTER_ROLE_PRIORITY);
    su_audio_cluster_destroy(c);
}

static void downmix_and_beds(void) {
    SuAudioClusterDesc d = desc_default();
    SuAudioCluster *c = make(&d);
    SuAudioClusterObject obj = object(1u, 0.0, 1.0f);
    SuAudioClusterObject bed = object(2u, 0.0, 1.0f);
    float out[FRAMES * 2u];
    uint32_t i;
    int ok = 1;
    obj.channels = 2u;
    obj.pcm = stereo_left;
    CHECK(su_audio_cluster_update(c, &obj, 1u) == SU_OK);
    CHECK(su_audio_cluster_render(c, &obj, 1u, FRAMES, out) == SU_OK);
    /* L=1, R=0 at -3 dB is 0.7071 mono, centered at 0.7071 per channel: 0.5 each. */
    for (i = 0u; i < FRAMES; i++) {
        if (fabsf(out[i * 2u] - 0.5f) > 1e-6f || fabsf(out[i * 2u + 1u] - 0.5f) > 1e-6f) {
            ok = 0;
        }
    }
    CHECK(ok);
    su_audio_cluster_destroy(c);
    c = make(&d);
    bed.channels = 2u;
    bed.pcm = signal_b;
    bed.flags = SU_AUDIO_CLUSTER_BED;
    CHECK(su_audio_cluster_update(c, &bed, 1u) == SU_OK);
    CHECK(su_audio_cluster_render(c, &bed, 1u, FRAMES, out) == SU_OK);
    CHECK(memcmp(out, signal_b, sizeof out) == 0);
    CHECK(find_assoc(c, 2u).role == SU_AUDIO_CLUSTER_ROLE_BED && find_assoc(c, 2u).output_id == 0u);
    {
        SuAudioClusterStats stats;
        su_audio_cluster_stats(c, &stats);
        CHECK(stats.beds == 1u && stats.outputs == 0u);
    }
    su_audio_cluster_destroy(c);
}

static void cancelled_directions(void) {
    SuAudioClusterDesc d = desc_default();
    SuAudioCluster *c;
    SuAudioClusterObject objs[2];
    SuAudioClusterOutput outputs[4];
    float out[FRAMES * 2u];
    uint32_t n = 0u;
    uint32_t i;
    int ok = 1;
    d.width_rad = (float)PI_D;
    c = make(&d);
    objs[0] = object(1u, PI_D * 0.5, 1.0f);
    objs[1] = object(2u, -PI_D * 0.5, 1.0f);
    CHECK(su_audio_cluster_update(c, objs, 2u) == SU_OK);
    CHECK(su_audio_cluster_outputs(c, outputs, 4u, &n) == SU_OK && n == 1u);
    CHECK(outputs[0].members == 2u && outputs[0].direction.x == 0.0f && outputs[0].direction.z == 0.0f);
    CHECK(su_audio_cluster_render(c, objs, 2u, FRAMES, out) == SU_OK);
    for (i = 0u; i < FRAMES; i++) {
        if (!isfinite(out[i * 2u]) || out[i * 2u] != out[i * 2u + 1u]) {
            ok = 0;
        }
    }
    CHECK(ok);
    /* Repeated updates keep the non-directional cluster stable. */
    CHECK(su_audio_cluster_update(c, objs, 2u) == SU_OK);
    CHECK(find_assoc(c, 1u).output_id == outputs[0].output_id && find_assoc(c, 2u).output_id == outputs[0].output_id);
    su_audio_cluster_destroy(c);
}

static void metadata_and_infeasible(void) {
    SuAudioClusterDesc d = desc_default();
    SuAudioCluster *c;
    SuAudioClusterObject objs[5];
    SuAudioClusterStats stats;
    float out[FRAMES * 2u];
    float single[FRAMES * 2u];
    double sum = 0.0;
    uint32_t i;
    d.output_limit = 1u;
    c = make(&d);
    objs[0] = object(1u, 0.0, 1.0f);
    objs[1] = object(2u, 0.01, 1.0f);
    objs[1].metadata = 7u;
    CHECK(su_audio_cluster_update(c, objs, 2u) == SU_OK);
    CHECK(find_assoc(c, 1u).output_id != find_assoc(c, 2u).output_id);
    su_audio_cluster_stats(c, &stats);
    CHECK(stats.infeasible == 1u && stats.outputs == 2u);
    su_audio_cluster_destroy(c);
    /* All-excluded inputs: the budget cannot be met, nothing is dropped. */
    d.output_limit = 2u;
    c = make(&d);
    for (i = 0u; i < 5u; i++) {
        objs[i] = object(10u + i, -1.0 + 0.5 * (double)i, 1.0f);
        objs[i].flags = SU_AUDIO_CLUSTER_EXCLUDE;
    }
    CHECK(su_audio_cluster_update(c, objs, 5u) == SU_OK);
    su_audio_cluster_stats(c, &stats);
    CHECK(stats.infeasible == 1u && stats.dedicated == 5u && stats.outputs == 5u && stats.merges == 0u);
    CHECK(su_audio_cluster_render(c, objs, 5u, FRAMES, out) == SU_OK);
    su_audio_cluster_destroy(c);
    for (i = 0u; i < 5u; i++) {
        SuAudioCluster *one;
        d.output_limit = 16u;
        one = make(&d);
        CHECK(su_audio_cluster_update(one, &objs[i], 1u) == SU_OK);
        CHECK(su_audio_cluster_render(one, &objs[i], 1u, FRAMES, single) == SU_OK);
        sum += single[100u * 2u] + single[100u * 2u + 1u];
        su_audio_cluster_destroy(one);
    }
    CHECK(fabs((out[200] + out[201]) - sum) < 1e-5);
}

static void budget_merges(void) {
    SuAudioClusterDesc d = desc_default();
    SuAudioCluster *c;
    SuAudioClusterObject objs[6];
    SuAudioClusterStats stats;
    static const double az[6] = {-2.0, -1.0, -0.5, 0.5, 1.4, 2.4};
    uint32_t i;
    d.output_limit = 3u;
    d.width_rad = 0.1f;
    c = make(&d);
    for (i = 0u; i < 6u; i++) {
        objs[i] = object(i + 1u, az[i], 1.0f);
    }
    CHECK(su_audio_cluster_update(c, objs, 6u) == SU_OK);
    su_audio_cluster_stats(c, &stats);
    CHECK(stats.outputs == 3u && stats.clusters == 3u && stats.infeasible == 0u);
    /* Every object stays associated; the closest pair (-1.0, -0.5) shares an output. */
    for (i = 0u; i < 6u; i++) {
        CHECK(find_assoc(c, i + 1u).output_id != 0u);
    }
    CHECK(find_assoc(c, 2u).output_id == find_assoc(c, 3u).output_id);
    su_audio_cluster_destroy(c);
}

static void energy_and_direction(void) {
    SuAudioClusterDesc d = desc_default();
    SuAudioCluster *clustered;
    SuAudioCluster *separate;
    SuAudioClusterObject objs[3];
    float a[FRAMES * 2u];
    float b[FRAMES * 2u];
    static float sig[3][FRAMES];
    double la;
    double ra;
    double lb;
    double rb;
    double ea;
    double eb;
    uint32_t i;
    uint32_t k;
    for (k = 0u; k < 3u; k++) {
        for (i = 0u; i < FRAMES; i++) {
            sig[k][i] = (float)(0.3 * sin(2.0 * PI_D * (double)(i * (3u + 4u * k)) / (double)FRAMES));
        }
    }
    clustered = make(&d);
    d.width_rad = 0.001f;
    separate = make(&d);
    for (k = 0u; k < 3u; k++) {
        objs[k] = object(k + 1u, 0.6 + 0.08 * (double)k, 0.3f);
        objs[k].pcm = sig[k];
    }
    CHECK(su_audio_cluster_update(clustered, objs, 3u) == SU_OK);
    CHECK(su_audio_cluster_update(separate, objs, 3u) == SU_OK);
    CHECK(su_audio_cluster_render(clustered, objs, 3u, FRAMES, a) == SU_OK);
    CHECK(su_audio_cluster_render(separate, objs, 3u, FRAMES, b) == SU_OK);
    ea = energy(a, FRAMES, &la, &ra);
    eb = energy(b, FRAMES, &lb, &rb);
    /* Energy within 0.25 dB; inter-channel level difference within 0.5 dB. */
    CHECK(fabs(10.0 * log10(ea / eb)) < 0.25);
    CHECK(fabs(10.0 * log10(ra / la) - 10.0 * log10(rb / lb)) < 0.5);
    printf("cluster energy delta %.4f dB, ILD %.3f vs %.3f dB\n", 10.0 * log10(ea / eb),
           10.0 * log10(ra / la), 10.0 * log10(rb / lb));
    su_audio_cluster_destroy(clustered);
    su_audio_cluster_destroy(separate);
}

static void moving_source(void) {
    SuAudioClusterDesc d = desc_default();
    SuAudioCluster *c;
    SuAudioClusterObject objs[3];
    SuAudioClusterAssoc assoc;
    float out[64u * 2u];
    uint32_t step;
    uint32_t a_id;
    uint32_t b_id;
    uint32_t switch_step = 0u;
    uint64_t switch_clock = 0u;
    int complementary = 1;
    int continuous = 1;
    int flips = 0;
    uint32_t last_output;
    float prev_l = 0.0f;
    float prev_r = 0.0f;
    static float constant[64u];
    uint32_t i;
    for (i = 0u; i < 64u; i++) {
        constant[i] = 0.5f;
    }
    d.width_rad = 0.4f;
    d.leave_ratio = 1.5f;
    d.hold_frames = 256u;
    d.crossfade_frames = 128u;
    c = make(&d);
    objs[0] = object(1u, -0.6, 10.0f);
    objs[1] = object(2u, 0.35, 10.0f);
    objs[2] = object(3u, -0.6, 0.1f);
    for (i = 0u; i < 3u; i++) {
        objs[i].pcm = constant;
    }
    CHECK(su_audio_cluster_update(c, objs, 3u) == SU_OK);
    a_id = find_assoc(c, 1u).output_id;
    b_id = find_assoc(c, 2u).output_id;
    last_output = find_assoc(c, 3u).output_id;
    CHECK(last_output == a_id && a_id != b_id);
    for (step = 0u; step < 60u; step++) {
        SuAudioClusterStats stats;
        objs[2].direction = azimuth(-0.6 + 0.025 * (double)step);
        CHECK(su_audio_cluster_update(c, objs, 3u) == SU_OK);
        assoc = find_assoc(c, 3u);
        if (assoc.output_id != last_output) {
            flips++;
            if (switch_step == 0u) {
                su_audio_cluster_stats(c, &stats);
                switch_step = step;
                switch_clock = stats.clock;
            }
            last_output = assoc.output_id;
        }
        if (fabsf(assoc.gain + assoc.previous_gain - 1.0f) > 1e-6f) {
            complementary = 0;
        }
        CHECK(su_audio_cluster_render(c, objs, 3u, 64u, out) == SU_OK);
        for (i = 0u; i < 64u; i++) {
            if ((step != 0u || i != 0u)
                && (fabsf(out[i * 2u] - prev_l) > 0.01f || fabsf(out[i * 2u + 1u] - prev_r) > 0.01f)) {
                continuous = 0;
            }
            prev_l = out[i * 2u];
            prev_r = out[i * 2u + 1u];
        }
    }
    /* Leaves A only beyond width*leave_ratio (0.6 rad from A), then joins B once. */
    CHECK(switch_step != 0u);
    CHECK(-0.6 + 0.025 * (double)switch_step > 0.0 - 0.025);
    CHECK(flips == 1 && last_output == b_id);
    CHECK(switch_clock >= 256u);
    CHECK(complementary);
    CHECK(continuous);
    /* After the crossfade the association is settled. */
    assoc = find_assoc(c, 3u);
    CHECK(assoc.previous_output_id == 0u && assoc.gain == 1.0f);
    /* Hysteresis: a small move back does not flip. */
    objs[2].direction = azimuth(0.2);
    CHECK(su_audio_cluster_update(c, objs, 3u) == SU_OK);
    CHECK(find_assoc(c, 3u).output_id == b_id);
    su_audio_cluster_destroy(c);
}

static void hold_and_crossfade_gains(void) {
    SuAudioClusterDesc d = desc_default();
    SuAudioCluster *c;
    SuAudioClusterObject objs[3];
    SuAudioClusterAssoc assoc;
    float out[128u * 2u];
    uint32_t old_id;
    d.hold_frames = 128u;
    d.crossfade_frames = 100u;
    c = make(&d);
    objs[0] = object(1u, 1.5, 1.0f);
    objs[1] = object(2u, -1.5, 0.1f);
    objs[2] = object(3u, -1.5, 10.0f);
    CHECK(su_audio_cluster_update(c, objs, 3u) == SU_OK);
    old_id = find_assoc(c, 2u).output_id;
    CHECK(old_id == find_assoc(c, 3u).output_id);
    objs[1].direction = azimuth(1.5);
    /* Held: membership is kept for hold_frames even far outside the leave angle. */
    CHECK(su_audio_cluster_update(c, objs, 3u) == SU_OK);
    CHECK(find_assoc(c, 2u).output_id == old_id);
    CHECK(su_audio_cluster_render(c, objs, 3u, 64u, out) == SU_OK);
    CHECK(su_audio_cluster_update(c, objs, 3u) == SU_OK);
    CHECK(find_assoc(c, 2u).output_id == old_id);
    CHECK(su_audio_cluster_render(c, objs, 3u, 64u, out) == SU_OK);
    CHECK(su_audio_cluster_update(c, objs, 3u) == SU_OK);
    assoc = find_assoc(c, 2u);
    CHECK(assoc.output_id == find_assoc(c, 1u).output_id && assoc.previous_output_id == old_id);
    CHECK(assoc.gain == 0.0f && assoc.previous_gain == 1.0f);
    {
        SuAudioClusterStats stats;
        su_audio_cluster_stats(c, &stats);
        CHECK(stats.draining == 0u && stats.fading == 1u && stats.clusters == 2u && stats.moves == 1u);
    }
    CHECK(su_audio_cluster_render(c, objs, 3u, 50u, out) == SU_OK);
    assoc = find_assoc(c, 2u);
    CHECK(assoc.gain == 0.5f && assoc.previous_gain == 0.5f);
    CHECK(su_audio_cluster_render(c, objs, 3u, 50u, out) == SU_OK);
    assoc = find_assoc(c, 2u);
    CHECK(assoc.previous_output_id == 0u && assoc.gain == 1.0f);
    /* A cluster whose last member moves away drains until its fade ends. */
    objs[2].direction = azimuth(1.45);
    objs[2].rms = 0.1f;
    CHECK(su_audio_cluster_render(c, objs, 3u, 64u, out) == SU_OK);
    CHECK(su_audio_cluster_update(c, objs, 3u) == SU_OK);
    {
        SuAudioClusterStats stats;
        su_audio_cluster_stats(c, &stats);
        CHECK(stats.draining == 1u && stats.clusters == 1u && stats.fading == 1u);
    }
    CHECK(su_audio_cluster_render(c, objs, 3u, 100u, out) == SU_OK);
    CHECK(su_audio_cluster_update(c, objs, 3u) == SU_OK);
    {
        SuAudioClusterStats stats;
        su_audio_cluster_stats(c, &stats);
        CHECK(stats.draining == 0u && stats.clusters == 1u && stats.fading == 0u);
    }
    /* Departure releases everything. */
    CHECK(su_audio_cluster_update(c, objs, 0u) == SU_OK);
    {
        SuAudioClusterStats stats;
        su_audio_cluster_stats(c, &stats);
        CHECK(stats.clusters == 0u && stats.outputs == 0u && stats.draining == 0u);
    }
    su_audio_cluster_destroy(c);
}

static uint32_t lcg(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

static void permutation_determinism(void) {
    SuAudioClusterDesc d = desc_default();
    SuAudioClusterObject base[40];
    SuAudioClusterObject shuffled[40];
    SuAudioClusterAssoc first[40];
    SuAudioClusterAssoc second[40];
    SuAudioClusterOutput out_a[64];
    SuAudioClusterOutput out_b[64];
    float pcm_a[FRAMES * 2u];
    float pcm_b[FRAMES * 2u];
    uint32_t seed = 7u;
    uint32_t i;
    uint32_t round;
    uint32_t na = 0u;
    uint32_t nb = 0u;
    SuAudioCluster *a;
    SuAudioCluster *b;
    int same = 1;
    d.output_limit = 6u;
    d.crossfade_frames = 64u;
    d.hold_frames = 64u;
    a = make(&d);
    b = make(&d);
    for (i = 0u; i < 40u; i++) {
        base[i] = object(1000u + (uint64_t)lcg(&seed) % 100000u * 40u + i,
                         ((double)(lcg(&seed) % 6283u) / 1000.0) - PI_D, (float)(lcg(&seed) % 100u) / 50.0f);
        base[i].metadata = lcg(&seed) % 2u;
        base[i].pcm = (i % 2u) ? signal_a : signal_b;
    }
    for (round = 0u; round < 6u; round++) {
        for (i = 0u; i < 40u; i++) {
            base[i].direction = azimuth(atan2(base[i].direction.x, base[i].direction.z) + 0.05 * (double)(i % 5u));
        }
        memcpy(shuffled, base, sizeof base);
        for (i = 39u; i > 0u; i--) {
            uint32_t j = lcg(&seed) % (i + 1u);
            SuAudioClusterObject t = shuffled[i];
            shuffled[i] = shuffled[j];
            shuffled[j] = t;
        }
        CHECK(su_audio_cluster_update(a, base, 40u) == SU_OK);
        CHECK(su_audio_cluster_update(b, shuffled, 40u) == SU_OK);
        CHECK(su_audio_cluster_assoc(a, first, 40u, &na) == SU_OK);
        CHECK(su_audio_cluster_assoc(b, second, 40u, &nb) == SU_OK);
        CHECK(na == 40u && nb == 40u);
        same &= memcmp(first, second, sizeof first) == 0;
        CHECK(su_audio_cluster_outputs(a, out_a, 64u, &na) == SU_OK);
        CHECK(su_audio_cluster_outputs(b, out_b, 64u, &nb) == SU_OK);
        same &= na == nb && memcmp(out_a, out_b, na * sizeof out_a[0]) == 0;
        CHECK(su_audio_cluster_render(a, base, 40u, FRAMES, pcm_a) == SU_OK);
        CHECK(su_audio_cluster_render(b, shuffled, 40u, FRAMES, pcm_b) == SU_OK);
        /* Object accumulation follows ID order, so PCM is bit-identical too. */
        same &= memcmp(pcm_a, pcm_b, sizeof pcm_a) == 0;
    }
    CHECK(same);
    {
        SuAudioClusterStats stats;
        su_audio_cluster_stats(a, &stats);
        CHECK(stats.outputs <= 6u || stats.infeasible == 1u);
    }
    su_audio_cluster_destroy(a);
    su_audio_cluster_destroy(b);
}

static void steady_state_and_cost(void) {
    SuAudioClusterDesc d = desc_default();
    SuAudioCluster *c;
    SuAudioClusterObject *objs = (SuAudioClusterObject *)malloc(256u * sizeof *objs);
    float *out = (float *)malloc(FRAMES * 2u * sizeof *out);
    SuAudioClusterStats stats;
    uint32_t seed = 3u;
    uint32_t i;
    uint32_t iter;
    unsigned long before;
    clock_t begin;
    clock_t update_ticks = 0;
    clock_t render_ticks = 0;
    CHECK(objs != NULL && out != NULL);
    if (objs == NULL || out == NULL) {
        free(objs);
        free(out);
        return;
    }
    d.max_objects = 256u;
    d.output_limit = 16u;
    d.width_rad = 0.25f;
    d.hold_frames = 1024u;
    d.crossfade_frames = 256u;
    before = allocations;
    c = make(&d);
    CHECK(allocations > before);
    for (i = 0u; i < 256u; i++) {
        objs[i] = object(i + 1u, ((double)(lcg(&seed) % 6283u) / 1000.0) - PI_D, 0.5f);
        objs[i].pcm = signal_a;
    }
    before = allocations;
    for (iter = 0u; iter < 200u; iter++) {
        for (i = 0u; i < 256u; i++) {
            objs[i].direction = azimuth(atan2(objs[i].direction.x, objs[i].direction.z) + 0.01);
        }
        begin = clock();
        CHECK(su_audio_cluster_update(c, objs, 256u) == SU_OK);
        update_ticks += clock() - begin;
        begin = clock();
        CHECK(su_audio_cluster_render(c, objs, 256u, FRAMES, out) == SU_OK);
        render_ticks += clock() - begin;
    }
    CHECK(allocations == before);
    su_audio_cluster_stats(c, &stats);
    CHECK(stats.outputs <= 16u && stats.objects == 256u);
    printf("cluster: 256 objects, limit 16: update %.1f us, render %.1f us per %u frames "
           "(200 iterations, %llu payload bytes, %lu steady-state allocations)\n",
           (double)update_ticks * 1e6 / (double)CLOCKS_PER_SEC / 200.0,
           (double)render_ticks * 1e6 / (double)CLOCKS_PER_SEC / 200.0, FRAMES,
           (unsigned long long)stats.runtime_bytes, allocations - before);
    su_audio_cluster_destroy(c);
    free(objs);
    free(out);
}

static void speaker_cluster_fades(void) {
    SuAudioClusterDesc desc = desc_default();
    SuAudioSpeakerLayout layout;
    SuAudioCluster *cluster;
    SuAudioCluster *chunked;
    SuAudioClusterObject objects[3];
    SuAudioClusterAssoc assoc;
    float pcm[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    float result[8];
    float split[8];
    uint32_t index;
    desc.crossfade_frames = 4u;
    cluster = make(&desc);
    chunked = make(&desc);
    memset(&layout, 0, sizeof layout);
    layout.count = 2u;
    layout.bed_right = 1u;
    layout.directions[0].x = -1.0f;
    layout.directions[1].x = 1.0f;
    objects[0] = object(1u, -PI_D * 0.5, 1.0f);
    objects[1] = object(2u, -PI_D * 0.5, 1.0f);
    objects[2] = object(3u, PI_D * 0.5, 1.0f);
    for (index = 0u; index < 3u; index++) objects[index].pcm = pcm;
    CHECK(su_audio_cluster_update(cluster, objects, 3u) == SU_OK);
    CHECK(su_audio_cluster_update(chunked, objects, 3u) == SU_OK);
    objects[1].direction = objects[2].direction;
    CHECK(su_audio_cluster_update(cluster, objects, 3u) == SU_OK);
    CHECK(su_audio_cluster_update(chunked, objects, 3u) == SU_OK);
    assoc = find_assoc(cluster, 2u);
    CHECK(assoc.previous_output_id != 0u);
    CHECK(su_audio_cluster_render_speakers(cluster, objects, 3u, 4u, &layout, result) == SU_OK);
    CHECK(su_audio_cluster_render_speakers(chunked, objects, 3u, 1u, &layout, split) == SU_OK);
    for (index = 0u; index < 3u; index++) objects[index].pcm++;
    CHECK(su_audio_cluster_render_speakers(chunked, objects, 3u, 3u, &layout, split + 2u) == SU_OK);
    CHECK(memcmp(result, split, sizeof result) == 0);
    CHECK(result[0] == 2.0f && result[1] == 1.0f);
    CHECK(result[6] == 1.25f && result[7] == 1.75f);
    CHECK(find_assoc(cluster, 2u).previous_output_id == 0u);
    su_audio_cluster_destroy(cluster);
    su_audio_cluster_destroy(chunked);
    desc.crossfade_frames = 0u;
    desc.width_rad = 2.0f;
    cluster = make(&desc);
    objects[0].direction = layout.directions[0];
    objects[1].direction.x = 0.0f;
    objects[1].direction.y = 1.0f;
    objects[1].direction.z = 0.0f;
    layout.directions[1] = objects[1].direction;
    CHECK(su_audio_cluster_update(cluster, objects, 2u) == SU_OK);
    CHECK(find_assoc(cluster, 1u).output_id == find_assoc(cluster, 2u).output_id);
    CHECK(su_audio_cluster_render_speakers(cluster, objects, 2u, 1u, &layout, result) == SU_OK);
    CHECK(fabsf(result[0] - 1.41421356f) < 1e-6f && result[0] == result[1]);
    su_audio_cluster_destroy(cluster);
}

static void speaker_renderer(void) {
    SuAudioSpeakerLayout layout;
    SuAudioClusterDesc desc = desc_default();
    SuAudioCluster *cluster = make(&desc);
    SuAudioCluster *chunked = make(&desc);
    SuAudioClusterObject objects[2];
    SuAudioClusterOutput outputs[4];
    SuAudioClusterStats before;
    SuAudioClusterStats after;
    float pcm[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    float result[32];
    float split[32];
    uint32_t count = 0u;
    uint32_t frame;
    unsigned allocated = allocations;
    memset(&layout, 0, sizeof layout);
    layout.count = 4u;
    layout.directions[0].x = -1.0f;
    layout.directions[1].x = 1.0f;
    layout.directions[2].y = 1.0f;
    layout.directions[3].z = 1.0f;
    layout.bed_right = 1u;
    CHECK(su_audio_speaker_layout_validate(&layout) == SU_OK);
    objects[0] = object(1u, 0.0, 1.0f);
    objects[0].direction.x = 0.0f;
    objects[0].direction.y = 1.0f;
    objects[0].direction.z = 0.0f;
    objects[0].pcm = pcm;
    CHECK(su_audio_cluster_update(cluster, objects, 1u) == SU_OK);
    CHECK(su_audio_cluster_outputs(cluster, outputs, 4u, &count) == SU_OK);
    CHECK(count == 1u && outputs[0].direction.y == 1.0f);
    CHECK(su_audio_cluster_render_speakers(cluster, objects, 1u, 4u, &layout, result) == SU_OK);
    for (frame = 0u; frame < 4u; frame++) {
        CHECK(result[frame * 4u] == 0.0f && result[frame * 4u + 1u] == 0.0f);
        CHECK(result[frame * 4u + 2u] == 1.0f && result[frame * 4u + 3u] == 0.0f);
    }
    objects[1] = object(2u, 0.0, 1.0f);
    objects[1].flags = SU_AUDIO_CLUSTER_BED;
    objects[1].channels = 2u;
    objects[1].pcm = pcm;
    CHECK(su_audio_cluster_update(cluster, objects, 2u) == SU_OK);
    CHECK(su_audio_cluster_render_speakers(cluster, objects, 2u, 4u, &layout, result) == SU_OK);
    CHECK(result[0] == 1.0f && result[1] == 1.0f && result[2] == 1.0f && result[3] == 0.0f);
    objects[0].direction.y = 0.0f;
    objects[0].direction.z = 1.0f;
    CHECK(su_audio_cluster_update(cluster, objects, 2u) == SU_OK);
    CHECK(su_audio_cluster_update(chunked, objects, 2u) == SU_OK);
    CHECK(su_audio_cluster_render_speakers(cluster, objects, 2u, 4u, &layout, result) == SU_OK);
    CHECK(su_audio_cluster_render_speakers(chunked, objects, 2u, 1u, &layout, split) == SU_OK);
    objects[0].pcm++;
    objects[1].pcm += 2;
    CHECK(su_audio_cluster_render_speakers(chunked, objects, 2u, 3u, &layout, split + 4u) == SU_OK);
    CHECK(memcmp(result, split, 16u * sizeof(float)) == 0);
    su_audio_cluster_stats(cluster, &before);
    layout.directions[2].y = NAN;
    result[0] = 123.0f;
    CHECK(su_audio_cluster_render_speakers(cluster, objects, 2u, 1u, &layout, result) == SU_E_INVALID_ARGUMENT);
    CHECK(result[0] == 123.0f);
    su_audio_cluster_stats(cluster, &after);
    CHECK(before.clock == after.clock);
    layout.directions[2].y = 0.0f;
    CHECK(su_audio_speaker_layout_validate(&layout) == SU_E_INVALID_ARGUMENT);
    layout.directions[2].y = 1.0f;
    layout.bed_right = 0u;
    CHECK(su_audio_speaker_layout_validate(&layout) == SU_E_INVALID_ARGUMENT);
    CHECK(allocations == allocated);
    su_audio_cluster_destroy(cluster);
    su_audio_cluster_destroy(chunked);
}

int main(void) {
    uint32_t i;
    for (i = 0u; i < FRAMES; i++) {
        signal_a[i] = (float)(0.25 * sin(2.0 * PI_D * (double)i / 64.0));
        signal_b[i * 2u] = (float)(0.2 * cos(2.0 * PI_D * (double)i / 32.0));
        signal_b[i * 2u + 1u] = (float)(0.1 * sin(2.0 * PI_D * (double)i / 16.0));
        stereo_left[i * 2u] = 1.0f;
        stereo_left[i * 2u + 1u] = 0.0f;
    }
    validation();
    speaker_renderer();
    speaker_cluster_fades();
    close_merge_and_centroid();
    hero_and_exclusions();
    downmix_and_beds();
    cancelled_directions();
    metadata_and_infeasible();
    budget_merges();
    energy_and_direction();
    moving_source();
    hold_and_crossfade_gains();
    permutation_determinism();
    steady_state_and_cost();
    printf("audio_cluster: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
