/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * space3d narrow phase (docs/SPACE3D.md section 3): swept sphere against sphere, capsule and oriented box, and
 * segment against an oriented finite gate (rectangle or disc). Every value is Q32.32 (src/ir/su_num.h) and every
 * step is a profile operation in a fixed order, so results are bit-exact on every compiler and target. These
 * functions are the normative definition of the queries; the text in SPACE3D.md describes them.
 *
 * Sweep convention: the moving sphere's centre goes from c0 to c0 + v during the tick (t in [0, 1]). A hit
 * reports the earliest t (toi, q32 in [0, ONE]) at which the sphere touches the obstacle, and the unit contact
 * normal pointing from the obstacle towards the sphere's centre. A sphere already touching at t = 0 reports toi 0.
 * Accuracy domain (not a determinism condition): relative coordinates within about 16 km and radii/extents
 * within about 4 km keep every intermediate square inside Q32.32; outside it results stay deterministic but
 * saturate. */
#ifndef SU_PROFILES_SPACE3D_GEOM_H
#define SU_PROFILES_SPACE3D_GEOM_H

#include <stdint.h>

#include "ir/su_num.h"

typedef struct S3Hit {
    int hit;      /* 1 when the sphere touches the obstacle during [0, 1] */
    int64_t toi;  /* q32 in [0, ONE] */
    SuV3 n;       /* unit normal from the obstacle towards the sphere (q32) */
} S3Hit;

/* Gate crossing of the segment p0 -> p1. dir: 0 none, +1 forward (from the back side, signed distance < 0, to
 * the front side, >= 0), -1 backward. The gate normal is the gate's local +z axis rotated by q; local x and y
 * span the gate. toi = d0 / (d0 - d1) (q32 div); lx, ly are the crossing point in gate coordinates. */
typedef struct S3GateHit {
    int dir;
    int64_t toi;
    int64_t lx, ly;
} S3GateHit;

S3Hit s3_sweep_sphere_sphere(SuV3 c0, SuV3 v, int64_t r, SuV3 s, int64_t rs);
/* Capsule: segment a-b with radius rc. */
S3Hit s3_sweep_sphere_capsule(SuV3 c0, SuV3 v, int64_t r, SuV3 a, SuV3 b, int64_t rc);
/* Oriented box: centre, unit orientation q (local -> world), half extents half (all >= 0). */
S3Hit s3_sweep_sphere_obb(SuV3 c0, SuV3 v, int64_t r, SuV3 center, SuQuat q, SuV3 half);
/* Gate: centre c, orientation q; rectangle |lx| <= half_x and |ly| <= half_y when disc == 0, else the disc
 * lx^2 + ly^2 <= radius^2. Returns dir and fills *out (out->dir == 0 when there is no crossing). */
int s3_segment_gate(SuV3 p0, SuV3 p1, SuV3 c, SuQuat q, int64_t half_x, int64_t half_y, int64_t radius, int disc,
                    S3GateHit *out);

/* World-space axis-aligned bounds of the shapes (inclusive; used by the broadphase). */
void s3_bounds_sphere(SuV3 c, int64_t r, SuV3 *lo, SuV3 *hi);
void s3_bounds_obb(SuV3 c, SuQuat q, SuV3 half, SuV3 *lo, SuV3 *hi);

#endif
