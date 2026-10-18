<!-- SPDX-FileCopyrightText: 2026 admiraly -->
# Evidence: rollback resimulation and the planned VM tier (sim-runtime)

Produced at source `45fdaea` (clean worktree; the driver was built from the same sources: `45fdaea` changed only
documentation after `9dbb06f`), clang 23.1.3 Release, driver `build-rel/racer_microcosm.exe` SHA-256
`7a8c3b40...c3b4`, on an i5-12400F (12 logical CPUs), Windows 11 10.0.26200, with:

    python -I bench/space3d/run_bench.py --driver <abs>/build-rel/racer_microcosm.exe --rollback-evidence planning/evidence/sim-runtime --repeat 3 --iters 200
    python -I bench/space3d/run_bench.py --driver <abs>/build-rel/racer_microcosm.exe --evidence planning/evidence/sim-runtime --ticks 1200 --repeat 3

Observations on one machine (constitution C10), not performance qualification. The machine also ran other agent
work; p99 and max values carry that noise.

## Rollback resimulation (`rollback-bench.json`)

After a 120-tick warm-up the world is saved as canonical bytes; per K the state is restored
(`su_world_restore_canonical`, a new world) and re-stepped K ticks with one `su_sched_run_ticks` call, 200 times.
Every one of the 3 x 3 x 2 x 3 x 200 resimulations reproduced its reference digest, and both VM tiers produced
the same digests (the script exits 1 otherwise). Restore + resim, p50 of 200 iterations, worst of 3 processes:

|---|---|---|---|---|---|---|---|---|
| 8 | reference | 1 | 25.9 | 176.6..177.6 | 177.6 | 2.4% | 5.0% | 2.8 |
| 8 | reference | 6 | 28.4 | 1062.6..1069.3 | 178.2 | 13.2% | 24.7% | 2.8 |
| 8 | reference | 12 | 29.2 | 2109.8..2135.3 | 177.9 | 26.0% | 54.0% | 2.8 |
| 8 | planned | 1 | 23.7 | 26.0..26.2 | 26.2 | 0.6% | 1.0% | 2.0 |
| 8 | planned | 6 | 25.6 | 146.5..147.6 | 24.6 | 2.1% | 3.7% | 2.0 |
| 8 | planned | 12 | 26.7 | 292.3..296.5 | 24.7 | 3.9% | 8.7% | 2.0 |
| 32 | reference | 1 | 30.1 | 245.8..254.0 | 254.0 | 3.4% | 12.2% | 2.6 |
| 32 | reference | 6 | 31.9 | 1510.1..1514.2 | 252.4 | 18.6% | 33.0% | 2.6 |
| 32 | reference | 12 | 32.3 | 2995.9..3025.2 | 252.1 | 36.7% | 64.1% | 2.6 |
| 32 | planned | 1 | 26.9 | 46.9..48.6 | 48.6 | 0.9% | 1.7% | 2.6 |
| 32 | planned | 6 | 29.9 | 311.3..321.6 | 53.6 | 4.2% | 10.9% | 2.6 |
| 32 | planned | 12 | 31.0 | 628.6..651.2 | 54.3 | 8.2% | 15.6% | 2.6 |
| 256 | reference | 1 | 54.5 | 832.7..835.3 | 835.3 | 10.7% | 18.9% | 8.3 |
| 256 | reference | 6 | 60.5 | 5031.1..5056.9 | 842.8 | 61.4% | 104.0% | 8.3 |
| 256 | reference | 12 | 61.8 | 10110.8..10135.5 | 844.6 | 122.4% | 204.0% | 8.3 |
| 256 | planned | 1 | 53.1 | 291.2..309.7 | 309.7 | 4.4% | 7.9% | 8.1 |
| 256 | planned | 6 | 57.1 | 1844.0..1912.0 | 318.7 | 23.6% | 54.2% | 8.1 |
| 256 | planned | 12 | 58.3 | 3691.9..3834.6 | 319.6 | 46.7% | 87.8% | 8.1 |

"reference" is the reference interpreter (`SUTEKH_VM_TIER=reference`); "planned" is the default tier
(`src/vm/su_vm_plan.h`) with the exact fast numerics of `src/ir/su_num_fast.h`. Both include the space3d query
(which uses the fast numerics in both rows) and the per-call state reuse of the entity runtime. The restore is a
new-world restore; an in-place restore belongs to the presentation work stream and would lower the restore
column. The resimulation of K = 12 at N = 256 fits the 8.33 ms budget at p50 (46.7 %), not with margin at p99.

## Per-tick timing (`racer-bench.json`, planned tier)

|---|---|---|---|---|---|---|---|---|---|---|---|
| 8 | yes | 72 | 24.9..26.1 | 30.5..59.3 | 70.3..315.1 | 0.71% | 24 | 1 | 25 | True | True/True |
| 8 | no | 72 | 16.0..16.4 | 17.8..23.1 | 52.9..85.2 | 0.28% | 0 | 0 | 0 | True | True/True |
| 32 | yes | 96 | 53.2..53.9 | 78.8..136.4 | 98.9..692.7 | 1.64% | 97 | 7 | 104 | True | True/True |
| 32 | no | 96 | 40.4..40.7 | 45.7..90.7 | 205.3..895.7 | 1.09% | 0 | 0 | 0 | True | True/True |
| 256 | yes | 320 | 325.8..326.5 | 551.8..651.9 | 823.1..1156.5 | 7.82% | 574 | 800 | 1374 | True | True/True |
| 256 | no | 320 | 258.4..260.1 | 481.3..619.8 | 908.9..4414.0 | 7.44% | 0 | 0 | 0 | True | True/True |

The same table before this work (`planning/evidence/numeric-3d/README.md`): N = 8 / 32 / 256 with the query,
p50 173.2..175.0 / 252.0..254.2 / 903.9..907.0 us. Every run reproduced its digests from scratch and from a
mid-run checkpoint; the state and event-log digests equal those of the reference tier.
