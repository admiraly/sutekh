#include "render/su_render_haze.h"

#include <float.h>
#include <math.h>
#include <string.h>

int su_haze_prepare(const SuHazeVolume *volume, SuWorldPos eye, HazePush *push) {
    double matrix[3][3], inverse[3][3], translation[3], determinant;
    SuVec3f offset;
    int row, column;
    if (volume == NULL || push == NULL || volume->shape > SU_HAZE_BOX || volume->blend > SU_HAZE_OVER ||
        (volume->flags & ~SU_HAZE_SHADOW) != 0u || !isfinite(volume->density) || volume->density < 0.0f ||
        !isfinite(volume->falloff) || volume->falloff < 0.0f || volume->falloff > 1.0f ||
        !isfinite(volume->sun) || volume->sun < 0.0f || volume->response > SU_HAZE_CHORD ||
        !isfinite(volume->chord_power) || volume->chord_power < 0.0f ||
        !isfinite(volume->source_brightness) || volume->source_brightness < 0.0f ||
        !isfinite(volume->source_size) || volume->source_size < 0.0f) return 0;
    for (row = 0; row < 16; row++) {
        if (!isfinite(volume->transform.m[row])) return 0;
    }
    if (volume->transform.m[3] != 0.0f || volume->transform.m[7] != 0.0f ||
        volume->transform.m[11] != 0.0f || volume->transform.m[15] != 1.0f) return 0;
    offset = su_wpos_delta(volume->pos, eye);
    translation[0] = (double)offset.x + volume->transform.m[12];
    translation[1] = (double)offset.y + volume->transform.m[13];
    translation[2] = (double)offset.z + volume->transform.m[14];
    for (row = 0; row < 3; row++) {
        if (!isfinite(volume->color[row]) || volume->color[row] < 0.0f || !isfinite(translation[row])) return 0;
        for (column = 0; column < 3; column++) matrix[row][column] = volume->transform.m[column * 4 + row];
    }
    for (row = 0; row < 3; row++) {
        for (column = 0; column < 3; column++) {
            inverse[column][row] = matrix[(row + 1) % 3][(column + 1) % 3] *
                                       matrix[(row + 2) % 3][(column + 2) % 3] -
                                   matrix[(row + 1) % 3][(column + 2) % 3] *
                                       matrix[(row + 2) % 3][(column + 1) % 3];
        }
    }
    determinant = matrix[0][0] * inverse[0][0] + matrix[0][1] * inverse[1][0] + matrix[0][2] * inverse[2][0];
    if (!isfinite(determinant) || determinant == 0.0) return 0;
    memset(push, 0, sizeof *push);
    for (row = 0; row < 3; row++) {
        double origin = 0.0;
        for (column = 0; column < 3; column++) {
            double value = inverse[row][column] / determinant;
            if (!isfinite(value) || fabs(value) > FLT_MAX) return 0;
            push->row[row][column] = (float)value;
            origin -= value * translation[column];
        }
        if (!isfinite(origin) || fabs(origin) > FLT_MAX) return 0;
        push->row[row][3] = (float)origin;
        push->color_density[row] = volume->color[row];
    }
    push->color_density[3] = volume->density;
    push->params[0] = (float)volume->shape;
    push->params[1] = volume->falloff;
    push->params[2] = volume->sun;
    push->params[3] = (volume->flags & SU_HAZE_SHADOW) != 0u ? 1.0f : 0.0f;
    push->response[0] = volume->chord_power > 0.0f ? volume->chord_power : 1.0f;
    push->response[1] = volume->source_brightness;
    push->response[2] = volume->source_size;
    push->response[3] = (float)volume->response;
    return 1;
}

float su_haze_optical_depth(const SuHazeVolume *volume, SuWorldPos eye, SuVec3f direction, float max_distance) {
    static const double nodes[4] = {-0.8611363115940526, -0.3399810435848563, 0.3399810435848563, 0.8611363115940526};
    static const double weights[4] = {0.3478548451374539, 0.6521451548625461, 0.6521451548625461, 0.3478548451374539};
    HazePush push;
    double origin[3], ray[3];
    double length = sqrt((double)direction.x * direction.x + (double)direction.y * direction.y +
                         (double)direction.z * direction.z);
    double entry = 0.0, exit = max_distance, integral = 0.0;
    int axis, sample;
    if (!su_haze_prepare(volume, eye, &push) || !isfinite(length) || length <= 0.0 ||
        !isfinite(max_distance) || max_distance <= 0.0f) return 0.0f;
    for (axis = 0; axis < 3; axis++) {
        origin[axis] = push.row[axis][3];
        ray[axis] = (push.row[axis][0] * direction.x + push.row[axis][1] * direction.y +
                     push.row[axis][2] * direction.z) / length;
    }
    if (volume->shape == SU_HAZE_ELLIPSOID) {
        double speed = 0.0, projection = 0.0, radius = 0.0, middle, half_chord;
        for (axis = 0; axis < 3; axis++) {
            speed += ray[axis] * ray[axis];
            projection += origin[axis] * ray[axis];
        }
        middle = -projection / speed;
        for (axis = 0; axis < 3; axis++) {
            double closest = origin[axis] + middle * ray[axis];
            radius += closest * closest;
        }
        if (radius >= 1.0) return 0.0f;
        half_chord = sqrt((1.0 - radius) / speed);
        entry = fmax(entry, middle - half_chord);
        exit = fmin(exit, middle + half_chord);
    } else {
        for (axis = 0; axis < 3; axis++) {
            double first, last;
            if (ray[axis] == 0.0) {
                if (fabs(origin[axis]) > 1.0) return 0.0f;
                continue;
            }
            first = (-1.0 - origin[axis]) / ray[axis];
            last = (1.0 - origin[axis]) / ray[axis];
            entry = fmax(entry, fmin(first, last));
            exit = fmin(exit, fmax(first, last));
        }
    }
    if (exit <= entry) return 0.0f;
    for (sample = 0; sample < 4; sample++) {
        double distance = (entry + exit) * 0.5 + (exit - entry) * 0.5 * nodes[sample];
        double profile = volume->shape == SU_HAZE_BOX ? 1.0 : 0.0;
        for (axis = 0; axis < 3; axis++) {
            double position = origin[axis] + distance * ray[axis];
            if (volume->shape == SU_HAZE_BOX) profile *= fmax(0.0, 1.0 - position * position);
            else profile += position * position;
        }
        if (volume->shape == SU_HAZE_ELLIPSOID) profile = fmax(0.0, 1.0 - profile);
        integral += weights[sample] * (1.0 - volume->falloff + volume->falloff * profile);
    }
    integral *= 0.5 * (exit - entry) * volume->density;
    return (float)fmin(integral, FLT_MAX);
}
