#ifndef SU_RENDER_HAZE_H
#define SU_RENDER_HAZE_H
#include "sutekh/render.h"
typedef struct HazePush {
    float row[3][4];
    float color_density[4];
    float params[4];
    float response[4];
} HazePush;
int su_haze_prepare(const SuHazeVolume *volume, SuWorldPos eye, HazePush *push);
#endif
