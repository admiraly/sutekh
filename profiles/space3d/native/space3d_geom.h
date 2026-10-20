/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * space3d narrow phase (docs/SPACE3D.md section 3): swept sphere against sphere, capsule and oriented box, and
 * segment against an oriented finite gate (rectangle, disc or ellipse). Every value is Q32.32 (src/ir/su_num.h) and every
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
/* Gate shapes in the gate plane (local x, y). */
#define S3_GATE_RECT 0    /* |lx| <= half_x and |ly| <= half_y */
#define S3_GATE_DISC 1    /* lx^2 + ly^2 <= radius^2 */
#define S3_GATE_ELLIPSE 2 /* (lx / half_x)^2 + (ly / half_y)^2 <= 1 (q32 div, mul, add); half_x or half_y <= 0: none */

/* Gate: centre c, orientation q, shape S3_GATE_*. Returns dir and fills *out (out->dir == 0 when there is no
 * crossing). */
int s3_segment_gate(SuV3 p0, SuV3 p1, SuV3 c, SuQuat q, int64_t half_x, int64_t half_y, int64_t radius, int shape,
                    S3GateHit *out);

/* Capsule movers (2026-10-18). The moving shape is the capsule whose core segment runs from c + e0 to c + e1 (world
 * offsets from the reference point c, constant during the sweep: translation only) with radius r; c moves from c0 to
 * c0 + v. Same toi / normal contract as the sphere sweeps (normal from the obstacle towards the mover, from the
 * closest pair of core points at the contact). The operator uses these only when a capsule mover is involved; a
 * sphere mover keeps the sphere functions bit for bit. The problem is the point c against the Minkowski sum of the
 * obstacle core and the reversed segment (a rounded parallelogram for two segments): its four edge capsules and its
 * face slab. */
S3Hit s3_sweep_capsule_capsule(SuV3 c0, SuV3 v, int64_t r, SuV3 e0, SuV3 e1, SuV3 a, SuV3 b, int64_t rc);
/* A sphere obstacle is the capsule with a == b. */
S3Hit s3_sweep_capsule_sphere(SuV3 c0, SuV3 v, int64_t r, SuV3 e0, SuV3 e1, SuV3 s, int64_t rs);
/* Oriented box: the earliest of the two end-sphere sweeps (s3_sweep_sphere_obb, in that order) and the twelve
 * box-edge segment sweeps (edges along x, then y, then z); a core segment that crosses the box at the start reports
 * toi 0 with the box normal at the middle of the crossing chord. Equal toi keeps the earlier feature. */
S3Hit s3_sweep_capsule_obb(SuV3 c0, SuV3 v, int64_t r, SuV3 e0, SuV3 e1, SuV3 center, SuQuat q, SuV3 half);
/* Core distances (no radii) from the segment p + e0 .. p + e1 to a segment a-b / an oriented box. */
int64_t s3_dist_seg_seg(SuV3 p, SuV3 e0, SuV3 e1, SuV3 a, SuV3 b);
int64_t s3_dist_seg_obb(SuV3 p, SuV3 e0, SuV3 e1, SuV3 center, SuQuat q, SuV3 half);

/* Triangles (static meshes, 2026-10-20). A triangle a-b-c with its unit normal n = normalize((b - a) x (c - a))
 * (zero for a degenerate triangle: only its edges collide). The contact sets are the closed triangle (both sides:
 * a mesh is a surface, not a solid). Same toi / normal contract as the sweeps above; the normal comes from the
 * closest pair of core points at the contact (triangle point to mover centre or core segment), falling back to the
 * face normal on the mover's side when that pair coincides. */
typedef struct S3Tri {
    SuV3 a, b, c, n;
} S3Tri;
S3Tri s3_tri_make(SuV3 a, SuV3 b, SuV3 c);
/* Sphere mover: the earliest of the face slab (entry through the plane at distance r, hit point inside the
 * triangle) and the three edge capsules (ab, bc, ca); equal s keeps the earlier feature. */
S3Hit s3_sweep_sphere_tri(SuV3 c0, SuV3 v, int64_t r, const S3Tri *t);
/* Capsule mover (core c + e0 .. c + e1): a core segment that crosses the triangle at the start reports toi 0 with
 * the face normal on the reference point's side; otherwise the earliest of the two end-sphere sweeps
 * (s3_sweep_sphere_tri at c0 + e0, then c0 + e1) and the three edge sweeps (rounded parallelograms of the edges
 * ab, bc, ca against the segment), equal toi keeping the earlier feature. Exact: the first touch of a segment and
 * a triangle is at an end point of the segment or between the segment and a triangle edge. */
S3Hit s3_sweep_capsule_tri(SuV3 c0, SuV3 v, int64_t r, SuV3 e0, SuV3 e1, const S3Tri *t);
/* Core distance (no radius) from the segment p + e0 .. p + e1 (e0 = e1: a point) to the triangle. */
int64_t s3_dist_seg_tri(SuV3 p, SuV3 e0, SuV3 e1, const S3Tri *t);
/* P minus its closest point on the triangle. */
SuV3 s3_tri_offset(const S3Tri *t, SuV3 P);

/* World-space axis-aligned bounds of the shapes (inclusive; used by the broadphase). */
void s3_bounds_sphere(SuV3 c, int64_t r, SuV3 *lo, SuV3 *hi);
void s3_bounds_obb(SuV3 c, SuQuat q, SuV3 half, SuV3 *lo, SuV3 *hi);

#endif
