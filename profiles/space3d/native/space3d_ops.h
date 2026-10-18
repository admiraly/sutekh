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
 * SPACE3D_MAX_ROWS rows; larger worlds are refused with SU_E_RESOURCE_LIMIT); reads only its declared read
 * views and the entity table (alive, generation); stores every declared write for every row; not reentrant. */
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

/* Instrumentation (tests): counters of the last run. */
typedef struct Space3dStats {
    uint32_t rows, movers, collidables, gates;
    uint32_t sweep_axis;      /* 0, 1 or 2 */
    uint64_t sweep_pairs;     /* overlapping-interval pairs examined */
    uint64_t candidates;      /* mover candidates kept */
    uint64_t narrow_tests;    /* narrow-phase calls */
    uint64_t contacts;        /* contacts found (before the per-mover cap) */
} Space3dStats;

typedef struct Space3dCtx {
    Space3dStats last;
    int brute_force;          /* test seam: nonzero skips the broadphase and tests every mover pair (O(n^2)) */
} Space3dCtx;

extern const SuHostOpDesc g_space3d_query_desc;
Space3dCtx *space3d_query_ctx(void);
/* Canonical constants text (its SHA-256 is the descriptor's constants_hash). */
const char *space3d_constants_text(void);

#endif
