#include "sutekh/render.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "line %d: %s\n", __LINE__, #condition); failures++; } } while (0)

static float numerical(const SuHazeVolume *volume, SuVec3f eye, SuVec3f ray, float end) {
    double integral = 0.0;
    double length = sqrt((double)ray.x * ray.x + (double)ray.y * ray.y + (double)ray.z * ray.z);
    int step;
    for (step = 0; step < 20000; step++) {
        double distance = ((double)step + 0.5) * end / 20000.0;
        double point[3] = {eye.x + ray.x / length * distance, eye.y + ray.y / length * distance,
                           eye.z + ray.z / length * distance};
        double radius = point[0] * point[0] + point[1] * point[1] + point[2] * point[2];
        int inside = volume->shape == SU_HAZE_ELLIPSOID ? radius < 1.0 :
                     fabs(point[0]) <= 1.0 && fabs(point[1]) <= 1.0 && fabs(point[2]) <= 1.0;
        if (inside) {
            double profile = volume->shape == SU_HAZE_ELLIPSOID ? 1.0 - radius :
                             (1.0 - point[0] * point[0]) * (1.0 - point[1] * point[1]) * (1.0 - point[2] * point[2]);
            integral += 1.0 - volume->falloff + volume->falloff * profile;
        }
    }
    return (float)(integral * end / 20000.0 * volume->density);
}

int main(void) {
    SuHazeVolume volume;
    SuWorldPos eye = su_wpos(0.0, 0.0, 3.0);
    SuVec3f ray = {0.0f, 0.0f, -1.0f};
    uint32_t shape, sample;
    memset(&volume, 0, sizeof volume);
    volume.transform = su_mat4f_identity();
    volume.density = 0.5f;
    CHECK(fabsf(su_haze_optical_depth(&volume, eye, ray, 10.0f) - 1.0f) < 1e-6f);
    CHECK(su_haze_optical_depth(&volume, eye, ray, 1.0f) == 0.0f);
    CHECK(fabsf(su_haze_optical_depth(&volume, eye, ray, 3.0f) - 0.5f) < 1e-6f);
    CHECK(fabsf(su_haze_optical_depth(&volume, volume.pos, ray, 10.0f) - 0.5f) < 1e-6f);
    volume.falloff = 1.0f;
    CHECK(fabsf(su_haze_optical_depth(&volume, eye, ray, 10.0f) - 2.0f / 3.0f) < 1e-6f);
    for (shape = 0; shape <= SU_HAZE_BOX; shape++) {
        volume.shape = shape;
        for (sample = 0; sample < 50u; sample++) {
            SuVec3f local_eye = {(float)sample * 0.042f - 1.05f, 0.17f, 2.0f};
            SuVec3f local_ray = {0.11f, -0.08f, -1.0f};
            float end = 1.0f + (float)sample * 0.061f;
            float actual;
            volume.falloff = (float)(sample % 5u) * 0.25f;
            eye = su_wpos(local_eye.x, local_eye.y, local_eye.z);
            actual = su_haze_optical_depth(&volume, eye, local_ray, end);
            CHECK(fabsf(actual - numerical(&volume, local_eye, local_ray, end)) < 0.0002f);
        }
    }
    volume.shape = SU_HAZE_ELLIPSOID;
    volume.falloff = 0.0f;
    volume.transform.m[0] = 2.0f;
    volume.transform.m[5] = 3.0f;
    volume.transform.m[10] = 4.0f;
    volume.transform.m[4] = 0.5f;
    volume.transform.m[12] = 0.25f;
    volume.pos = su_wpos(50000.0, -20000.0, 70000.0);
    eye = su_wpos(50000.25, -20000.0, 70010.0);
    CHECK(fabsf(su_haze_optical_depth(&volume, eye, ray, 20.0f) - 4.0f) < 1e-5f);
    volume.transform.m[0] = -2.0f;
    CHECK(fabsf(su_haze_optical_depth(&volume, eye, ray, 20.0f) - 4.0f) < 1e-5f);
    volume.transform.m[0] = 0.0f;
    CHECK(su_haze_optical_depth(&volume, eye, ray, 20.0f) == 0.0f);
    CHECK(su_haze_optical_depth(NULL, eye, ray, 20.0f) == 0.0f);
    volume.transform = su_mat4f_identity();
    volume.density = NAN;
    CHECK(su_haze_optical_depth(&volume, eye, ray, 20.0f) == 0.0f);
    volume.density = 0.5f;
    volume.falloff = 2.0f;
    CHECK(su_haze_optical_depth(&volume, eye, ray, 20.0f) == 0.0f);
    volume.falloff = 0.0f;
    ray.x = NAN;
    CHECK(su_haze_optical_depth(&volume, eye, ray, 20.0f) == 0.0f);
    printf("haze_math: %d failures\n", failures);
    return failures != 0;
}
