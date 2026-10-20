/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * space3d static meshes (docs/SPACE3D.md, "Static meshes"; 2026-10-20). A mesh set is immutable data handed to a
 * space3d operator instance once, before its world first steps (su_host_set_operator_data): triangle geometries,
 * parts (a geometry placed in a mesh frame by position, rotation and uniform scale) and meshes (lists of parts). A
 * world row of kind SPACE3D_KIND_MESH places mesh `gate_index` at its pos/rot; movers sweep against its triangles
 * exactly like against the analytic shapes (space3d_geom.h, s3_sweep_*_tri).
 *
 * Everything is Q32.32 and integer: geometry BVHs and the per-mesh part BVHs are built once at load by a
 * deterministic median split (stable order by centroid key and index), and every query result is defined
 * independently of the traversal: the sweep returns the minimum (toi, part index, triangle index) over every
 * triangle of the mesh, the distance the minimum over every triangle. Node culling only skips triangles that
 * cannot reach that minimum (bounds padded by S3MESH_PAD), which tests check against brute force.
 *
 * Blob format "S3MESH01" (little-endian; sizes exact, nothing after the end):
 *   magic[8] "S3MESH01"
 *   u32 geometries G, u32 parts P, u32 meshes M
 *   G x { u32 vertices V, u32 triangles T }
 *   G x { V x i64[3] vertex (q32 raw, geometry frame), T x u32[3] vertex indices }
 *   P x { u32 geometry, i64[3] position, i64[4] rotation w x y z (unit), i64 scale (q32, > 0) }   (mesh frame)
 *   M x { u32 first part, u32 part count (0: an empty mesh, never collides) }
 * Limits: G <= 65536, P and M <= 2^20, total vertices and triangles <= 2^24, every geometry has >= 1 triangle,
 * |coordinate| <= 2^20 m in the geometry and in the mesh frame (scaled bounds), |rotation|^2 within 2^-16 of 1,
 * 2^-16 <= scale <= 2^16. Degenerate triangles are allowed (only their edges collide). */
#ifndef SU_PROFILES_SPACE3D_MESH_H
#define SU_PROFILES_SPACE3D_MESH_H

#include <stddef.h>
#include <stdint.h>

#include "space3d_geom.h"

#define S3MESH_MAGIC "S3MESH01"
#define S3MESH_MAX_GEOMS 65536u
#define S3MESH_MAX_PARTS (1u << 20)
#define S3MESH_MAX_MESHES (1u << 20)
#define S3MESH_MAX_VERTS (1u << 24)
#define S3MESH_MAX_TRIS (1u << 24)
#define S3MESH_LEAF 4u                         /* items per BVH leaf (at most) */
#define S3MESH_PAD (INT64_C(1) << 26)          /* 1/64 m: culling slack (bounds, quick plane reject) */

typedef struct S3MeshSet S3MeshSet;

/* Parses and builds a set (refcount 1). Returns SU_OK, SU_E_PARSE (malformed blob: why says what and where),
 * SU_E_RESOURCE_LIMIT (a limit or out of memory) or SU_E_INVALID_ARGUMENT. */
uint32_t s3mesh_set_create(const uint8_t *data, size_t len, S3MeshSet **out, char *why, size_t why_cap);
void s3mesh_set_retain(S3MeshSet *s);
void s3mesh_set_release(S3MeshSet *s); /* NULL-safe; frees at refcount 0 */

uint32_t s3mesh_count(const S3MeshSet *s);
/* 1 when mesh exists and has at least one part. */
int s3mesh_valid(const S3MeshSet *s, uint32_t mesh);
/* Mesh-frame bounds of a valid mesh (inclusive, padded by S3MESH_PAD). */
void s3mesh_bounds(const S3MeshSet *s, uint32_t mesh, SuV3 *lo, SuV3 *hi);

/* Earliest contact of a mover against mesh `mesh` placed at pos with unit orientation rot (mesh -> world): the
 * mover's centre goes from c0 to c0 + v with radius r; e0, e1 are its core segment's world offsets (both zero: a
 * sphere, s3_sweep_sphere_tri; else s3_sweep_capsule_tri). The result is the hit with the smallest (toi, part
 * index, triangle index); the normal (world, unit) is that triangle's, rotated by the part and the mesh. */
typedef struct S3MeshStats {
    uint64_t nodes;     /* BVH nodes visited */
    uint64_t tri_tests; /* exact triangle tests run */
} S3MeshStats;
/* st (may be NULL) accumulates counters. */
S3Hit s3mesh_sweep(const S3MeshSet *s, uint32_t mesh, SuV3 pos, SuQuat rot, SuV3 c0, SuV3 v, int64_t r, SuV3 e0,
                   SuV3 e1, S3MeshStats *st);
/* Smallest core distance (no radius) from the segment p + e0 .. p + e1 to the mesh's triangles, measured in the
 * mesh frame (part distances times the part scale). Returns a value > limit when no triangle is within limit
 * (limit >= 0). */
int64_t s3mesh_dist(const S3MeshSet *s, uint32_t mesh, SuV3 pos, SuQuat rot, SuV3 p, SuV3 e0, SuV3 e1,
                    int64_t limit, S3MeshStats *st);

/* Test seam: brute != 0 visits every part and triangle (no culling); results must not change. */
void s3mesh_set_brute(S3MeshSet *s, int brute);
uint32_t s3mesh_triangles(const S3MeshSet *s); /* total over geometries */

#endif
