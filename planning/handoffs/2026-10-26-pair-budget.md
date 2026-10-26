<!-- SPDX-FileCopyrightText: 2026 admiraly -->
<!-- SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 -->
# Issue 163: space3d pair-budget overflow must not switch collision off world-wide

## State and scope

Branch `fix/issue-163-pair-budget`, based on `origin/main` `66e2e75` (merge of PR #154). Locally verified; integration
pending. Issue #161 (collision group/mask, configurable contacts, larger row limit) is not implemented here; the design
leaves room for it (below).

Exact modified paths:

- `profiles/space3d/native/space3d_ops.c`, `space3d_ops.h`: the space3d_ops_v2 descriptors `space3d.query2`,
  `space3d.query2_prox`, `space3d.query2_shape`, `space3d.query2_compound` (operator version 2, the v1 field tables),
  their layered constants texts and pinned hashes, the v2 broadphase (`v2_broadphase`: sweep or hierarchical grid,
  top-32 candidates by rank, water-filled pair budget with cut marks), v2 scratch (static and per instance),
  `Space3dStats` budget/truncation fields, `Space3dCounters` (cumulative, in `Space3dCtx`), test seam
  `space3d_test_force_grid`. The v1 code path is unchanged apart from counting flagged rows into the stats.
- `profiles/space3d/space3d_query2{,_prox,_shape,_compound}.capsule.json` (new): the v2 host records.
- `include/sutekh/host.h`, `src/host/su_host.c`, `src/host/su_host_internal.h`, `src/host/su_host_operators.c`:
  `SuHostOperatorCounters` and `su_host_operator_counters` (additive; built-in space3d operators of both families;
  `SU_E_UNSUPPORTED_METHOD` for registered operators); the four v2 operators in the built-in table.
- `src/runtime/rts/host_registry.c`: the v2 descriptors in the worker's production table (`sutekh serve`).
- `tests/space3d/test_space3d_ops.c`: v2 descriptor checks; every scene test rerun on v2, then on v2 with the grid
  forced. `tests/space3d/test_space3d_dense.c` (new) and `cmake/SutekhHostOps.cmake` (ctest `space3d_dense`).
- `tests/host/test_host_opstate.c`: counters refused for a registered operator and an IR system.
- `tests/determinism/det_matrix.c`, `expected_digests.txt` (13 lines appended, the 191 existing lines unchanged),
  `expected_tiers.txt` (5 lines): workload `s3_dense`.
- `docs/SPACE3D.md` (flags row, section 3 "Broadphase v2", section 4), `docs/DETERMINISM_MATRIX.md`,
  `docs/IMPLEMENTATION_STATUS.md`, this handoff.

## Design

- **Opt-in, v1 byte-identical.** Changing v1 would change its constants hashes and every record and replay that names
  them, and its results in every world that ever hit flag 1 (candidate choice) or flag 4. v1 descriptors, hashes,
  records and the determinism matrix's existing 191 lines are unchanged. Worlds opt in by listing the `space3d.query2*`
  records. Every v2 variant's constants text contains the full text of the variants below it, so a change in the base
  changes every v2 hash (v1's variant texts only name their base).
- **Result defined independently of the method.** Every mover keeps the 32 best of all collidables whose bounds
  overlap its own on three axes, by (rank, slot): rank = (63 - grid level) << 58 | min(L1 distance from the mover's
  bounds centre to the other's bounds, 2^58 - 1). Larger bodies first, then nearest, so a target in a swarm survives
  candidate truncation (v1 kept the first 32 in sweep order, and with an unlimited budget still loses 77 of the
  15,000 converging contacts to that rule). Selection is order-independent (bounded sorted insert, cached worst rank).
- **Two methods, chosen by an exact count.** The sweep (v1's) runs when its interval-overlap pair count, counted by
  one binary search per row before sweeping, is at most 512 per row; otherwise a hierarchical grid (power-of-two
  levels from bounds extent, entry at the lower-corner cell, per-level per-axis max extent to tighten query ranges,
  64-bit cell hash with exact cell check, open-addressing bucket table). Both give the same result unless the grid
  cuts; the grid-forced test pass checks that.
- **Budget: declared capacity, local degradation.** max(2^22, 1024 x capacity) grid entries visited per tick
  (`pair_budget=...,max_4194304_1024_per_row` in the constants; capacity is in the world identity). It costs no memory
  (it bounds time) and grows with the world: 67,108,864 at 65,536 rows. Over budget, the cut is the largest value with
  sum(min(count, cut)) <= budget over every row's grid count, so only the most crowded rows are cut; queries visit
  coarser levels first. A cut mover gets flag 4; coarser-level cells a cut query did not finish are marked and their
  movers get flag 4 too, so no pair is lost without a flag.
- **Counters.** `su_host_operator_counters`: last run (movers with flag 1, 2, 4, any; pairs examined; pair budget;
  entries skipped; budget hit) and totals since world creation (process-local diagnostics, not world state).
- **#161 compatibility.** Group/mask filtering fits at the single place a pair becomes an offer (`s3_visit`,
  `s3_sweep`), before candidate ranking, so filtered pairs never consume the 32 slots; configurable contacts and a
  larger row limit need new field tables, so they will be new descriptors or a v3 built on these texts.
- **Memory.** About 132 bytes per row more scratch (static context: 65,536 rows, virtual until touched).

## Commands and results

Hardware: Intel Core i7-14700K, Linux 7.2.9, GCC 16.2.1, Clang 22.1.8; out-of-tree build directories
(shared machine, other jobs running).

Full suites (`ctest -j 6 --output-on-failure`; skips are environment skips present on main: `audio_device` (no audio
device), `native_unwind` (needs ptrace)):

| Lane | Commit | Result |
|---|---|---|
| GCC 16.2 Release | `f404ff8` | 162/162 passed, 1 skipped |
| GCC Debug | `806cc32` | 162/162 passed, 1 skipped; again with `SUTEKH_VM_TIER=reference`: 162/162 |
| GCC Debug, `SUTEKH_NATIVE=ON` | `806cc32` | 180/180 passed, 2 skipped |
| Clang 22 Release, `SUTEKH_NATIVE=ON` | `806cc32` | 179/180: `space3d_dense` failed on its sparse-scene digests. Cause: the test's scene builder drew two random numbers inside one function call's arguments, whose evaluation order differs between GCC and Clang (the operator was not involved: v1 and v2 differed alike). Fixed in `15fcdb4` (one draw per statement, GCC's order kept, pinned digests unchanged); rerun on GCC and Clang: 0 failures, identical digests |
| Clang Debug ASan+UBSan, `SUTEKH_NATIVE=ON` (the CI sanitizer configuration) | `806cc32` | 179/180, the same `space3d_dense` failure; rerun at `15fcdb4` (`space3d_dense`, `host_opstate*`): 10/10; `scan_sanitizer_logs.py`: 0 reports (leak checks not run: no ptrace here). The determinism matrix ran reduced (120 ticks), as configured |

Pack and license checks: `tools/validate_pack.py --check-report` passed after `--write-report` (476 strict JSON files:
the four new records); `tools/check_repository_license.py` passed. Not run locally: sanitizer lane with native OFF,
Windows legs (CI).

Determinism: `expected_digests.txt` regenerated with GCC 16.2.1 Release, reference tier
(`det_matrix --fixtures <build>/det_fixtures --tier reference --out ...`); the 191 existing lines are unchanged
(diff: only the 13 `s3_dense` lines appended), so no existing workload was re-pinned. Every lane above ran every
determinism ctest (reference, planned, mixed, native where built, DLL, play-static) against it and passed. `s3_dense`:
3 ticks cut, 5,345 flag-4 movers, 2,663,230 entries skipped over 300 ticks.

Performance (`test_space3d_dense --bench 31`, GCC 16.2 -O3, i7-14700K, one core, shared machine, load average about 4;
ms per tick of `su_host_step` with the operator only; median and minimum of 30 ticks; source `15fcdb4`):

| Scene | v1 `space3d.query` (as main) | v2 `space3d.query2` | v1 with the sweep budget set to 2^40 (scratch build, not shipped) |
|---|---|---|---|
| converge: 15,000 movers on 4 targets | 31.4 / 31.3, 7,552 of 15,000 certain contacts lost (flagged) | 82.8 / 82.5, 0 lost | 83.6 / 83.4, 77 lost (first-32 rule) |
| pile-up: 3,000 from one point + 1,000 converging | 19.5 / 19.2, every mover flagged 4 | 43.3 / 43.1, only the 3,000 piled movers flagged 4 | 20.6 / 20.2 |
| sparse: 15,000 movers + 400 spheres in 400 m | 4.50 / 4.44 | 5.24 / 5.19 (sweep path); 24.4 / 24.2 with the grid forced | n/a |

v1 is fast in the dense scenes only because it stops after 2^22 pairs and drops the rest. Against v1 with an unlimited
budget, v2 costs the same in the converging scene and about twice as much in the pile-up (every visit there is an
overlapping pair ranked for the top 32); the sparse scene costs about 16% more (rank keeping). The v1 path in this
branch is main's code plus flag counting into the stats.

## Unrun or not covered

- The v1 world-wide flag-4 path is not in the determinism matrix (it is covered by `space3d_dense` on each build).
- Only `space3d.query2` runs in the matrix; the other three v2 variants are checked against the brute force in
  `space3d_ops` (sweep and grid paths), not across builds.
- Windows and MSVC legs: CI only (see the PR).
- Allocation counting in `space3d_dense` is glibc-only and off under AddressSanitizer.

## Next work

- #161 on top of v2: group/mask lanes applied before ranking; contacts per mover; row limit as a declared capacity.
- If downstream scenes regularly need the grid, a grid that keeps its table across ticks (temporal coherence) would
  cut the count pass; not needed for the measured scenes.
