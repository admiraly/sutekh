<!-- SPDX-FileCopyrightText: 2026 admiraly -->
# Evidence: rollback resimulation, planned VM tier and native IR 0.3 kernels (sim-runtime)

Produced at source `c8d7cb3` (clean worktree, `origin/main` 15b0dd7 merged), clang 23.1.3 Release with
`SUTEKH_NATIVE=ON`, driver `build-nat-rel/racer_microcosm.exe` SHA-256 `58040ddd...aa3d`, on an i5-12400F (12 logical
CPUs), Windows 11 10.0.26200, with:

    python -I bench/space3d/run_bench.py --driver <abs>/build-nat-rel/racer_microcosm.exe --rollback-evidence planning/evidence/sim-runtime --repeat 3 --iters 200 --tiers reference,planned,native
    python -I bench/space3d/run_bench.py --driver <abs>/build-nat-rel/racer_microcosm.exe --evidence planning/evidence/sim-runtime --ticks 1200 --repeat 3

Observations on one machine (constitution C10), not performance qualification. The machine was shared with other
work; p99 and max carry that noise (one planned-tier repeat at N = 8 shows it in its p50 too).

## Rollback resimulation (`rollback-bench.json`)

After a 120-tick warm-up the world is saved as canonical bytes; per K the state is restored
(`su_world_restore_canonical`, a new world) and re-stepped K ticks with one `su_sched_run_ticks` call, 200 times.
All 3 x 3 x 3 x 3 x 200 resimulations reproduced their reference digests and the three tiers produced the same
digests (`failures` is empty; the script exits 1 otherwise). Restore + resimulation, p50 of 200, worst of 3 processes:

| N | VM tier | K | restore p50 us | resim p50 us (min..max of 3 runs) | per tick p50 us | total p50 / 8.33 ms | total p99 / 8.33 ms | save p50 us |
|---|---|---|---|---|---|---|---|---|
| 8 | reference | 1 | 26.8 | 179.8..182.4 | 182.4 | 2.5% | 10.2% | 2.2 |
| 8 | reference | 6 | 30.5 | 1062.0..1122.2 | 187.0 | 13.9% | 27.4% | 2.2 |
| 8 | reference | 12 | 30.7 | 2111.0..2193.8 | 182.8 | 26.7% | 51.1% | 2.2 |
| 8 | planned | 1 | 38.0 | 25.5..37.7 | 37.7 | 0.9% | 1.3% | 2.1 |
| 8 | planned | 6 | 46.3 | 138.3..261.2 | 43.5 | 3.7% | 9.1% | 2.1 |
| 8 | planned | 12 | 26.9 | 274.3..276.6 | 23.1 | 3.6% | 6.7% | 2.1 |
| 8 | native | 1 | 24.4 | 24.0..25.6 | 25.6 | 0.6% | 1.2% | 3.2 |
| 8 | native | 6 | 26.0 | 121.1..124.7 | 20.8 | 1.8% | 3.1% | 3.2 |
| 8 | native | 12 | 27.3 | 234.5..244.8 | 20.4 | 3.3% | 4.5% | 3.2 |
| 32 | reference | 1 | 29.3 | 242.7..246.3 | 246.3 | 3.3% | 6.1% | 3.1 |
| 32 | reference | 6 | 34.0 | 1507.9..1631.7 | 271.9 | 20.0% | 32.5% | 3.1 |
| 32 | reference | 12 | 32.5 | 2982.0..2999.8 | 250.0 | 36.4% | 83.4% | 3.1 |
| 32 | planned | 1 | 27.9 | 45.8..47.9 | 47.9 | 0.9% | 1.3% | 2.7 |
| 32 | planned | 6 | 31.1 | 302.7..316.6 | 52.8 | 4.2% | 7.3% | 2.7 |
| 32 | planned | 12 | 31.2 | 613.3..621.2 | 51.8 | 7.8% | 14.3% | 2.7 |
| 32 | native | 1 | 27.5 | 36.7..37.8 | 37.8 | 0.8% | 2.2% | 4.1 |
| 32 | native | 6 | 30.7 | 217.9..228.6 | 38.1 | 3.1% | 5.7% | 4.1 |
| 32 | native | 12 | 31.0 | 446.4..459.9 | 38.3 | 5.9% | 10.2% | 4.1 |
| 256 | reference | 1 | 55.7 | 837.3..863.5 | 863.5 | 11.1% | 21.1% | 10.0 |
| 256 | reference | 6 | 59.5 | 5048.8..5228.7 | 871.5 | 63.5% | 97.8% | 10.0 |
| 256 | reference | 12 | 60.2 | 10115.6..10304.9 | 858.7 | 124.4% | 173.7% | 10.0 |
| 256 | planned | 1 | 53.0 | 286.9..289.1 | 289.1 | 4.1% | 8.8% | 8.4 |
| 256 | planned | 6 | 56.3 | 1812.9..1816.2 | 302.7 | 22.5% | 40.5% | 8.4 |
| 256 | planned | 12 | 57.6 | 3626.8..3630.1 | 302.5 | 44.3% | 81.9% | 8.4 |
| 256 | native | 1 | 54.2 | 182.1..183.6 | 183.6 | 2.9% | 4.7% | 8.1 |
| 256 | native | 6 | 57.5 | 1181.6..1183.2 | 197.2 | 14.9% | 26.3% | 8.1 |
| 256 | native | 12 | 58.2 | 2360.4..2363.3 | 196.9 | 29.1% | 45.2% | 8.1 |

Tiers: "reference" is the reference interpreter (`SUTEKH_VM_TIER=reference`); "planned" the default VM tier
(`src/vm/su_vm_plan.h`, exact fast numerics `src/ir/su_num_fast.h`); "native" runs racer.move as an IR 0.3 scalar
kernel (`racer_microcosm --native`: compiled, admitted against the VM by `su_nv_admit`, installed), racer.race
(entity operations) on the planned tier. All three include the space3d query (fast numerics) and the entity runtime's
per-call state reuse. The restore is a new-world restore; the presentation stream's in-place restore is not used here.

## Per-tick timing (`racer-bench.json`, planned tier)

| N | query | rows | p50 us (min..max of 3 runs) | p99 us | max us | worst p99 / 8.33 ms | gates | collision ticks | events | digests equal across runs | replay start/ckpt |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 8 | yes | 72 | 24.0..24.5 | 27.5..50.0 | 55.2..189.8 | 0.60% | 24 | 1 | 25 | True | True/True |
| 8 | no | 72 | 15.5..15.7 | 22.0..29.0 | 31.3..92.1 | 0.35% | 0 | 0 | 0 | True | True/True |
| 32 | yes | 96 | 52.4..53.3 | 91.0..112.6 | 154.6..187.9 | 1.35% | 97 | 7 | 104 | True | True/True |
| 32 | no | 96 | 39.8..41.3 | 69.5..79.6 | 76.0..1339.0 | 0.96% | 0 | 0 | 0 | True | True/True |
| 256 | yes | 320 | 320.8..337.7 | 483.0..545.4 | 587.5..640.8 | 6.54% | 574 | 800 | 1374 | True | True/True |
| 256 | no | 320 | 260.4..260.9 | 449.2..458.7 | 501.4..953.2 | 5.50% | 0 | 0 | 0 | True | True/True |

Before this work (`planning/evidence/numeric-3d/README.md`), with the query: N = 8 / 32 / 256 p50 173.2..175.0 /
252.0..254.2 / 903.9..907.0 us. Every run reproduced its digests from scratch and from a mid-run checkpoint, and the
digests equal the reference tier's.
