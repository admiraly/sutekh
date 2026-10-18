<!-- SPDX-FileCopyrightText: 2026 admiraly -->
# Evidence: numeric profile fixed_q32_v1, IR 0.3, space3d, 6dof_racer_microcosm

`racer-bench.json` is the output of
`python -I bench/space3d/run_bench.py --driver build-win-rel/racer_microcosm.exe --evidence planning/evidence/numeric-3d --ticks 1200 --repeat 3`
at source `54c2377` (clean worktree; `feat/numeric-3d` after merging `origin/main` 641b450), driver SHA-256
`62d5a3bd...6389` (clang 23.1.3, Release), on an i5-12400F (12 logical CPUs), Windows 11 10.0.26200. Each
configuration ran three times in separate processes, 120 warm-up ticks then 1,200 timed ticks, one tick per
`su_sched_run_ticks` call, wall time on the monotonic clock. The machine was shared with other agent work: these
are observations (constitution C10), not performance qualification.

| N | query | rows | p50 us (3 runs) | p99 us | worst p99 / 8.33 ms | gates passed | contact ticks | events |
|---|---|---|---|---|---|---|---|---|
| 8 | yes | 72 | 173.2..175.0 | 215.5..441.7 | 5.30% | 24 | 1 | 25 |
| 8 | no | 72 | 155.6..157.0 | 262.2..313.6 | 3.76% | 0 | 0 | 0 |
| 32 | yes | 96 | 252.0..254.2 | 435.2..474.5 | 5.69% | 97 | 7 | 104 |
| 32 | no | 96 | 219.4..221.1 | 280.9..440.1 | 5.28% | 0 | 0 | 0 |
| 256 | yes | 320 | 903.9..907.0 | 1481.6..1752.0 | 21.02% | 574 | 800 | 1374 |
| 256 | no | 320 | 789.0..793.0 | 1182.5..1943.0 | 23.32% | 0 | 0 | 0 |

"no" runs the two IR systems without `space3d.query` (the query's cost is the difference). Maximum single-tick
times include one 37.4 ms outlier (N = 256, one of three runs); p99 is not affected. Every run reproduced its
digests from scratch and from a mid-run canonical checkpoint, and the three repeats of each configuration have
identical state and event-log digests.

Cross-compiler and cross-platform identity (the determinism claim of the profile):
- clang 23.1.3 Debug and Release, and MSVC 19.44 Release (scratch build with `/W3` and `/experimental:c11atomics`,
  because pre-existing code does not build under `/W4 /WX`), produce identical results for `test_num`,
  `q32_model` (2,460,000 records), `ir03_diff` (300 programs), `space3d_geom`, `space3d_ops`, `host03`, the pinned
  racer digests (`bench/space3d/expected.json`) and an N = 256 x 1,200-tick racer run
  (state `e1b6bbda6831...`, events `e55304148d33...`, 1,316 events).
- GCC 13.3 on Linux (GitHub Actions, draft PR #30, run 39118750375): ctest 72/72 passed, including
  `space3d_racer_replay` against the digests pinned on Windows.

Numeric microbenchmark (clang -O2, observation): q32 add 1.1 ns, mul 8 ns, div 20 ns, sqrt 71 ns, v3_len 80 ns,
v3_normalize 134 ns, quat_rotate 88 ns, quat_integrate 253 ns.
