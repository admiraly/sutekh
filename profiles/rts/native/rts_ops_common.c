/* Shared helpers and the canonical constants texts of rts_ops_v2 (M3-04). See rts_ops.h. */
#include "rts_ops.h"

/* The texts below are the operator constants of 4.4. Changing any line changes the hash in the descriptor
 * (a new constants_hash, and a native build plus a new operator_version, as 4.4 requires). */
static const char k_spatial_text[] =
    "rts_ops_v2 rts.spatial constants (M3-04, contract r4 sections 4.1, 4.2, 4.4)\n"
    "operator_version 2\n"
    "NONE 0xFFFFFFFF\n"
    "domain 0..32767 for positions; out of domain is SU_E_INTERNAL\n"
    "capacity limit 1000000 rows; range_sq limit 262144 (SU_E_INTERNAL above)\n"
    "gather valid iff formation_slot != NONE and slot < capacity and alive(slot) and generation(slot) == formation_gen\n"
    "grid: rows with alive and kind 1 and hp > 0; cell = (y >> 6) * 512 + (x >> 6); 512 x 512 cells\n"
    "grid entries in ascending slot order within a cell\n"
    "query: alive rows with kind 1 and range_sq > 0 only\n"
    "R = isqrt(range_sq) (floor); rc = (R + 63) >> 6; rc > 8 is SU_E_INTERNAL\n"
    "ring order: ring d = 0..rc, cells row-major (cy ascending, cx ascending), clipped to the grid\n"
    "budget K = 160 examined entries; self and same-team entries count when examined\n"
    "qualify: entry != querier, team differs, dx*dx + dy*dy <= range_sq\n"
    "best = lexicographic minimum of (dist_sq, slot)\n"
    "truncated = 1 iff indexed entries in the ring box (rings 0..rc) > 160\n"
    "target_gen = generation(best slot); dist and valid are 0 when there is no target\n"
    "target_x, target_y = position of the target when valid, else 0\n";

static const char k_scatter_text[] =
    "rts_ops_v2 rts.scatter constants (M3-04, contract r4 sections 4.3, 4.4)\n"
    "operator_version 2\n"
    "NONE 0xFFFFFFFF\n"
    "shooters: alive rows with dmg_target != NONE and dmg_out != 0, ascending slot order\n"
    "target valid iff target < capacity and alive(target) and generation(target) == dmg_target_gen and kind == 1\n"
    "incoming = sat_add(incoming, dmg_out), sat_add(a, b) = min(a + b, 0xFFFFFFFF) in u32\n"
    "writes: incoming of target rows only\n";

const char *rts_spatial_constants_text(void) {
    return k_spatial_text;
}

const char *rts_scatter_constants_text(void) {
    return k_scatter_text;
}

uint32_t rts_ops_isqrt(uint32_t n) {
    uint32_t res = 0u;
    uint32_t bit = 1u << 30;
    while (bit > n) {
        bit >>= 2;
    }
    while (bit != 0u) {
        if (n >= res + bit) {
            n -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

uint32_t rts_ops_sat_add(uint32_t a, uint32_t b) {
    uint32_t s = a + b;
    return (s < a) ? 0xFFFFFFFFu : s;
}
