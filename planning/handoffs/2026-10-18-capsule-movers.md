<!-- SPDX-FileCopyrightText: 2026 admiraly -->
<!-- SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 -->
# Handoff: capsule movers (`space3d.query_shape`)

Branch `feat/capsule-movers` (worktree `engines/sutekh-worktrees/capsule-movers`), from `origin/main` fec0159.
Scope: amendment `planning/amendments/2026-10-18-downstream-host-work.md`, extension item 17.

## What changed

1. **Narrow phase** (`profiles/space3d/native/space3d_geom.{c,h}`): `s3_sweep_capsule_sphere`, `_capsule`, `_obb`
   and `s3_dist_seg_seg`, `s3_dist_seg_obb`. Capsule against segment cores = point against the rounded
   parallelogram (face slab + four edge capsules through the existing `ray_capsule`); box = start crossing (slab
   clip, toi 0), else the two end-sphere sweeps and the twelve edge sweeps. The sphere functions are untouched.
2. **Operator** (`space3d_ops.{c,h}`): descriptor `g_space3d_query_shape_desc` = prox table + `z0` (72), `z1` (74),
   constants hash fb8287e359d78662d0ea1fd2b778c9ec0d0349bc343250eda61b91a74ba97ab8. `mover_seg` returns the world
   offsets only for query_shape and a mover with a nonzero lane; every other case runs the previous code. Bounds:
   union of the end spheres at prev and pos. Gate crossing stays on the reference point.
3. **Record** `profiles/space3d/space3d_query_shape.capsule.json`; registered in `host_registry.c` (worker) and
   `su_host_operators.c` (host API, appended as index 2).
4. **Rotation choice**: end-of-tick orientation (`rot` lanes) held for the tick; translation-only sweep. Error up to
   2 |z| sin(a / 2) at the start of the tick for a turn of a per tick; documented in `docs/SPACE3D.md`, with the
   conservative workaround (pad `radius`). A two-orientation conservative sweep would need a `prev_rot` lane.
5. **Docs**: `docs/SPACE3D.md` section 2 (capsule movers); `PACK_VALIDATION.json` regenerated (425 JSON files).

## Commands and results (Windows, 2026-10-18)

- `. tools\env-windows.ps1; cmake -S . -B build-shape -G Ninja -DCMAKE_BUILD_TYPE=Release; cmake --build build-shape`
  (clang, -Wall -Wextra -Werror -pedantic): clean.
- `ctest --test-dir build-shape -j 8`: 100% passed, 98 tests (2 skipped as before: path_fifo, containment), before
  the host/serve additions; after them `ctest -R "host_ops|space3d_serve"`: passed.
- `build-shape\test_space3d_geom.exe`: 108 checks, 0 failures; capsule_sphere / capsule_capsule / capsule_obb 6000
  cases each, 0 mismatches, worst toi error 2.9e-7 / 1.2e-7 / 7.3e-7 m, worst normal error 1.8e-7 / 6.9e-8 /
  3.4e-7. Mutant (face slab disabled): 262 + 500 mismatches, 3 failures.
- `build-shape\test_space3d_ops.exe`: 18916 checks, 0 failures ("space3d shape: 22 capsule scenes equal to brute
  force, 8 truncated (repeat-only), 2534 contacts"). Mutant (sphere bounds for capsule movers): 17 failures.
- `build-shape\test_host_ops.exe build-shape\host_ops_racer_n8.json`: 3072 checks, 0 failures (check 8: collisions
  counted by racer.race, spheres 1 by tick 720, capsules 2 by tick 720).
- `space3d_serve` (verbose): "capsule movers: ok (z lanes zero equal space3d.query: True; capsules z +/-20:
  restore re-step equal, 27 lanes differ from spheres)"; 17 checks, 0 failures.
- MSVC: vcvars64 + `cmake -S . -B build-msvc -G Ninja -DCMAKE_C_COMPILER=cl -DSUTEKH_NATIVE=OFF -DSUTEKH_RENDER=OFF`
  (/W4 /WX): clean; `ctest -R "space3d|host_ops|example_arena"`: 12/12 passed.
- Existing pinned digests (host_ops, example_arena_host*, space3d_racer_*) pass unchanged.

## Not run / limits

- Linux GCC and the native tier (SUTEKH_NATIVE=ON) were not built locally; CI covers them. The operator is not
  lowered by the native backend (compiled C under every tier), so there is no differential native path to add.
- The racer microcosm rarely collides, so the host-API rollback window is evidence of determinism with capsule
  geometry evaluated each tick, not of many contacts; contact-heavy coverage is in `test_space3d_ops`.
- Rotation is an approximation (above), not a conservative bound.

## Next

- If the downstream needs conservative turning, add a `prev_rot` lane variant and sweep the union of the start and
  end segments (or pad by the bound above automatically).
- The arena example could gain a capsule-ship mode with a pinned digest.
