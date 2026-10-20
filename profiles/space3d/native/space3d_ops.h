/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * space3d.query host operator, space3d_ops_v1 (docs/SPACE3D.md). One phase operator, run as a host entry
 * (src/runtime/schedule.h, SU_SYS_KIND_HOST) over a world whose schema follows profiles/space3d/schema:
 *   - broadphase: sort and sweep over world AABBs on one axis (the axis of largest centre spread, ties to x),
 *     order (min on that axis, slot); every mover-involving overlapping pair becomes a candidate;
 *   - narrow phase (space3d_geom.h): swept sphere against sphere, oriented box, capsule and other movers;
 *   - gate query: the mover's centre segment prev -> pos against the gate whose gate_index equals the mover's
 *     next_gate (lowest slot when several gates share an index).
 * Results are per mover row, bounded and canonically ordered: up to SPACE3D_CONTACTS contacts sorted by
 * (toi, slot), counts, truncation flags, and the gate crossing. Capsules consume them in a later system.
 *
 * Contract (same discipline as the RTS operators): integer only; no allocation in the tick (static scratch for
 * SPACE3D_MAX_ROWS rows, or an instance's scratch allocated at world creation; larger worlds are refused with
 * SU_E_RESOURCE_LIMIT); reads only its declared read views and the entity table (alive, generation); stores
 * every declared write for every row; one context is not reentrant (distinct instances are independent). */
#ifndef SU_PROFILES_SPACE3D_OPS_H
#define SU_PROFILES_SPACE3D_OPS_H

#include <stddef.h>
#include <stdint.h>

#include "runtime/schedule.h"

#define SPACE3D_OPS_VERSION 1u
#define SPACE3D_SCHEMA_ID "space3d"
#define SPACE3D_MAX_ROWS 65536u
#define SPACE3D_CAND_MAX 32u            /* broadphase candidates kept per mover per tick */
#define SPACE3D_CONTACTS 2u             /* contacts reported per mover */
#define SPACE3D_SWEEP_BUDGET (1u << 22) /* overlapping-interval pairs examined per tick */

/* kind values */
#define SPACE3D_KIND_NONE 0u
#define SPACE3D_KIND_MOVER 1u      /* sphere swept from prev to pos */
#define SPACE3D_KIND_SPHERE 2u     /* static sphere at pos */
#define SPACE3D_KIND_BOX 3u        /* static oriented box: pos, rot, half */
#define SPACE3D_KIND_CAPSULE 4u    /* static capsule: segment pos -/+ rot*(0,0,half_z), radius */
#define SPACE3D_KIND_GATE_RECT 5u  /* gate rectangle: pos, rot (normal = local +z), half_x, half_y */
#define SPACE3D_KIND_GATE_DISC 6u  /* gate disc: pos, rot, radius */
#define SPACE3D_KIND_GATE_ELLIPSE 7u /* gate ellipse: pos, rot, semi-axes half_x (local x), half_y (local y) */
#define SPACE3D_KIND_MESH 8u       /* static mesh: pos, rot, mesh index in gate_index (2026-10-20; collides only
                                    * when the operator instance holds a mesh set with that mesh, else ignored) */

/* hit_flags bits */
#define SPACE3D_FLAG_CAND_TRUNCATED 1u    /* more than SPACE3D_CAND_MAX candidates overlapped */
#define SPACE3D_FLAG_CONTACT_TRUNCATED 2u /* more than SPACE3D_CONTACTS contacts were found */
#define SPACE3D_FLAG_SWEEP_BUDGET 4u      /* the tick's sweep budget ran out (set on every mover) */

/* gate_dir values */
#define SPACE3D_GATE_NONE 0u
#define SPACE3D_GATE_FORWARD 1u
#define SPACE3D_GATE_BACKWARD 2u

/* Field IDs of the space3d schema (64-bit fields occupy ID and ID+1). */
#define SPACE3D_F_KIND 1u
#define SPACE3D_F_NEXT_GATE 2u
#define SPACE3D_F_GATE_INDEX 3u
#define SPACE3D_F_POS_X 10u
#define SPACE3D_F_POS_Y 12u
#define SPACE3D_F_POS_Z 14u
#define SPACE3D_F_PREV_X 16u
#define SPACE3D_F_PREV_Y 18u
#define SPACE3D_F_PREV_Z 20u
#define SPACE3D_F_ROT_W 22u
#define SPACE3D_F_ROT_X 24u
#define SPACE3D_F_ROT_Y 26u
#define SPACE3D_F_ROT_Z 28u
#define SPACE3D_F_RADIUS 30u
#define SPACE3D_F_HALF_X 32u
#define SPACE3D_F_HALF_Y 34u
#define SPACE3D_F_HALF_Z 36u
#define SPACE3D_F_HIT_COUNT 40u
#define SPACE3D_F_HIT_FLAGS 41u
#define SPACE3D_F_C0_SLOT 42u /* contact k: slot 42+10k, gen 43+10k, toi 44+10k, nx 46+10k, ny 48+10k, nz 50+10k */
#define SPACE3D_F_GATE_DIR 62u
#define SPACE3D_F_GATE_SLOT 63u
#define SPACE3D_F_GATE_TOI 64u
/* space3d.query_prox (2026-10-18): the base lanes plus a per-mover proximity output. */
#define SPACE3D_F_PROX_MARGIN 66u /* q32 read: mover's proximity margin (<= 0: off) */
#define SPACE3D_F_PROX_SLOT 68u   /* u32 write: closest collidable within the margin (0xFFFFFFFF when none) */
#define SPACE3D_F_PROX_GEN 69u    /* u32 write: its generation (0 when none) */
#define SPACE3D_F_PROX_DIST 70u   /* q32 write: its surface distance, [0, margin] (0 when none) */
/* space3d.query_shape (2026-10-18): the proximity lanes plus the capsule-mover shape. */
#define SPACE3D_F_Z0 72u /* q32 read: mover core segment start on local +z (z0 = z1 = 0: sphere) */
#define SPACE3D_F_Z1 74u /* q32 read: mover core segment end on local +z */
/* space3d.query_compound (2026-10-20): the query_shape lanes plus compound movers and turning during the tick. */
#define SPACE3D_F_COMPOUND 76u /* u32 read: mover's compound index in the instance's set (0xFFFFFFFF: none) */
#define SPACE3D_F_C0_SUB 77u   /* u32 write: contact 0 sub-shapes (low 16: the mover's, high 16: the other mover's) */
#define SPACE3D_F_C1_SUB 78u   /* u32 write: contact 1 sub-shapes */
#define SPACE3D_F_ROT0_W 80u   /* q32 read: mover orientation at the start of the tick (all four zero: not given) */
#define SPACE3D_F_ROT0_X 82u
#define SPACE3D_F_ROT0_Y 84u
#define SPACE3D_F_ROT0_Z 86u
#define SPACE3D_SUB_NONE 0xFFFFu /* sub-shape half of c*_sub when that side is not a compound mover */

/* Instrumentation (tests): counters of the last run. */
typedef struct Space3dStats {
    uint32_t rows, movers, collidables, gates;
    uint32_t sweep_axis;      /* 0, 1 or 2 */
    uint64_t sweep_pairs;     /* overlapping-interval pairs examined */
    uint64_t candidates;      /* mover candidates kept */
    uint64_t narrow_tests;    /* narrow-phase calls */
    uint64_t contacts;        /* contacts found (before the per-mover cap) */
    uint64_t mesh_nodes;      /* static meshes: BVH nodes visited (sweeps and distances) */
    uint64_t mesh_tri_tests;  /* static meshes: exact triangle tests */
} Space3dStats;

/* Per-row scratch of one operator instance (sized for max_rows rows). The temporal-coherence cache (prev_*) only
 * seeds the sweep sort; the sort's result is the same permutation without it, so it is not world state. */
typedef struct Space3dScratch {
    uint32_t max_rows;
    uint8_t *alive;
    uint32_t *gen;
    int64_t *lo[3], *hi[3];
    uint32_t *coll, *tmp;
    uint32_t (*cand)[SPACE3D_CAND_MAX];
    uint8_t *ncand, *flags;
    uint32_t *gate_slot;
    uint32_t *prev_coll, *prev_order;
    uint32_t prev_n, prev_axis;
} Space3dScratch;

struct S3MeshSet;
typedef struct Space3dCtx {
    Space3dStats last;
    int brute_force;          /* test seam: nonzero skips the broadphase and tests every mover pair (O(n^2)) */
    Space3dScratch *scratch;  /* never NULL for a usable context */
    struct S3MeshSet *meshes; /* static meshes (instances only; NULL: kind 8 rows are ignored) */
} Space3dCtx;

extern const SuHostOpDesc g_space3d_query_desc;
/* space3d.query_prox: every lane and result of space3d.query, plus per mover the closest other collidable whose
 * surface distance at the end positions is within the mover's prox_margin (ties: lowest slot). Movers with a
 * positive margin have their broadphase bounds widened by it; proximity is taken over the same kept candidates
 * (so SPACE3D_FLAG_CAND_TRUNCATED also covers it). docs/SPACE3D.md section 3. */
extern const SuHostOpDesc g_space3d_query_prox_desc;
/* space3d.query_shape: every lane and result of space3d.query_prox, plus an optional capsule shape per mover: the
 * core segment from local (0, 0, z0) to (0, 0, z1), rotated by the mover's rot lanes (its orientation at the end of
 * the tick, held for the whole tick: the sweep is translation only), with radius `radius`. A mover with z0 = z1 = 0
 * is the sphere of space3d.query_prox bit for bit. Gate crossings stay on the reference point's segment.
 * docs/SPACE3D.md section 3. */
extern const SuHostOpDesc g_space3d_query_shape_desc;
/* space3d.query_compound: every lane and result of space3d.query_shape, plus
 *   - compound movers: a mover whose `compound` lane names a compound of the instance's set (space3d_mesh.h,
 *     S3MESH02) is the union of up to 8 capsules/spheres fixed in its local frame (rotated by its rot lanes); its
 *     radius, z0 and z1 lanes are not used for contacts (proximity measures that shape: the envelope). Contacts against it (or by it) are the earliest over its
 *     sub-shapes, ties to the lower sub-shape index, and c*_sub report which sub-shapes touched;
 *   - turning: when a mover's rot0 lanes (start-of-tick orientation) are nonzero, every core point at distance d
 *     from the reference point is padded by 2 d sin(a / 2) (rounded up), a the angle between rot0 and rot, so the
 *     translation-only sweep at the end orientation contains the shape at every intermediate orientation of the
 *     shortest rotation from rot0 to rot (a conservative bound; proximity is measured without the pad).
 *   - proximity: every mover is measured by its radius/z0/z1 shape exactly as in space3d.query_shape (a game keeps
 *     an enveloping sphere or capsule there), never by its compound (an exact search per sub-shape costs about n
 *     times as much); compound movers' bounds include that shape.
 * A mover with compound = 0xFFFFFFFF (or an unknown index, or no set) and rot0 all zero is the mover of
 * space3d.query_shape bit for bit. docs/SPACE3D.md "Compound movers". */
extern const SuHostOpDesc g_space3d_query_compound_desc;
/* The descriptor's static context (process-wide scratch for SPACE3D_MAX_ROWS rows; one world per worker
 * process, not reentrant). */
Space3dCtx *space3d_query_ctx(void);

/* Instances (2026-10-18, in-process host API): a context with its own scratch for max_rows rows (1..
 * SPACE3D_MAX_ROWS), allocated once here and never in the tick. A schedule entry that holds an instance
 * (su_sched_set_host_instance) runs the operator on it instead of the static context, so worlds in one process
 * are independent and may step on different threads. Results are identical to the static context's for every
 * input (same code, same order); a world with more rows than max_rows is refused with SU_E_RESOURCE_LIMIT.
 * Returns SU_OK, SU_E_INVALID_ARGUMENT or SU_E_RESOURCE_LIMIT. */
uint32_t space3d_query_instance_create(uint32_t max_rows, Space3dCtx **out);
void space3d_query_instance_destroy(void *ctx); /* NULL-safe; matches su_sched_set_host_instance's free_fn */
/* Static meshes (2026-10-20, docs/SPACE3D.md "Static meshes"): gives an instance a mesh set (space3d_mesh.h;
 * retained, released with the instance). Rows of kind SPACE3D_KIND_MESH then collide as mesh gate_index of the
 * set, placed at the row's pos/rot: swept contacts and proximity like any collidable. An instance holds one set,
 * given once (SU_E_INVALID_ARGUMENT for NULL or a second set); the static context never holds one. Without a set,
 * results are those of the operator without meshes, bit for bit. */
uint32_t space3d_query_instance_set_meshes(Space3dCtx *ctx, struct S3MeshSet *set);
/* Canonical constants text (its SHA-256 is the descriptor's constants_hash). */
const char *space3d_constants_text(void);
const char *space3d_prox_constants_text(void);
const char *space3d_shape_constants_text(void);
const char *space3d_compound_constants_text(void);

#endif
