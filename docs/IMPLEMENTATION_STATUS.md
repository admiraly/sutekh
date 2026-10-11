# Implementation status (M0)

Status as of M0-10 (2026-10-11). This document reports what the repository does now, what the tests showed, and what is not built. M0 is not closed: M0-11 (independent closure audit) is planned, T04 is still unavailable, and the T13 comparison is against proposed targets only. Update after M0-10: M0-09 landed (80face8), the release-profile warnings were fixed (93db0d6) and the candidate table now evicts its oldest record (d005726); the evidence files below were recorded before those fixes and are noted where they differ. `planning/tasks.json` still lists tasks as `planned` or `submitted`; that file belongs to the orchestrator and was not edited here.

All numbers below come from runs recorded under `planning/evidence/`. Shared-VM timing is observation, not performance qualification (ENGINE_CONSTITUTION C10).

## 1. Per-task status

| Task | Status | Commits (git log) | What exists |
|---|---|---|---|
| M0-00 | submitted; reviewed twice | 22f7950 (freeze), bd1288f (review), 1b6d55a and 735d698 (r2), 200aa68 (r3) | ABI v1 headers and M0 contract, error codes, fixtures plan |
| M0-01 | implemented | 114900c | C17 CMake skeleton, platform clock and pid, `tools/dev.py` (`configure`, `build`, `clean`; `--profile dev|release`) |
| M0-02 | implemented | 2c99818; wired 5e76608 | strict JSON reader, SU-LIR verifier, SHA-256 (`test_ir`) |
| M0-03 | implemented | 98dbab8; wired 914ac34 | fixed-row SoA u32 world; committed, working and staging roots (`test_world`) |
| M0-04 | implemented | bfeae5d; wired a6b14a8 | checked scalar reference VM and world run helper (`test_vm`) |
| M0-05 | implemented | 268753a; merged c7ccfce | POSIX supervisor/worker control plane, `serve` and `worker` roles, `test_control` |
| M0-06 | implemented | 373de54, 2c31bda, b0623b5, 831e9a5 (contract r3.2), 3da93ec | fixtures and stdlib harness `tests/harness/run_m0.py` (T00 to T15) |
| M0-07 | implemented | ce8ae87 | version registry, serial schedule, safe-point `candidate.install` with evidence gate (`test_reload`) |
| M0-08 | implemented (library half) | 2a579fc | canonical snapshot capture and restore, bounded checkpoint store (`test_snapshot`) |
| M0-08b | implemented | ceeb78f | `world.snapshot` and `world.restore` in the control plane; capsule `max_rows` enforced in the VM (`test_snapshot_cp`) |
| M0-09 | implemented | 80face8 | `tools/demo_zero_wait.py` live-edit demo and `tests/e2e/no_build_probe.py` (strace exec probe with positive control); 6058752 is a separate harness T09 fix |
| M0-10 | implemented (this task) | e1af932, a678b87, b795879, and the commit of this file | live edit and native build latency bench, evidence, this status document |

Engine capabilities advertised by `build/sutekh serve` (`engine.info`): `engine.info`, `capsule.submit`, `candidate.status`, `candidate.evaluate`, `candidate.install`, `world.create`, `world.inspect`, `world.step`, `world.snapshot`, `world.restore`.

## 2. Tests and latest results

Platform: Linux 6.18, 4 vCPU Intel Xeon 2.80 GHz (hypervisor flag set), 16 GB, GCC 13.3.0, CMake 3.28.3, Ninja 1.11.1, Python 3.13.16. Binary `build/sutekh` built with `tools/dev.py build --profile dev` (CMAKE_BUILD_TYPE=Debug), SHA-256 `f00aa1fe3a41b005...`, source trees `HEAD:src` `ac7b061d...`, `HEAD:include` `bb3fc5c4...`.

- **ctest** (`ctest --test-dir build --output-on-failure`): 7 of 7 passed (world, ir, vm, control, snapshot, reload, snapshot_cp), 1.14 s.
- **Harness** (`python3 -I tests/harness/run_m0.py --binary build/sutekh --report-dir <scratch>`): at M0-10 time passed 15, failed 0, not_run 0, unavailable 1. A later run with `--no-build` reports T00 as `not_run` by design, so it shows 14 passed.
  - T04 unavailable: 7 of 14 negative cases cannot be expressed through `capsule.submit`, which takes only `capsule_path`; the engine binds each capsule to its own `source`. The other 7 were checked and rejected with the expected codes.
  - Copies: `planning/evidence/m0-harness-dev.json` and `planning/evidence/m0-harness-dev-summary.txt`. Harness script SHA-256 `43f3e71e13348...`.
- **Harness self-test** (`python3 -I tests/harness/run_m0.py --self-test`): 82 checks, 0 failures.
- The task named `--report`; the harness flag is `--report-dir`.
- Not run: Clang and sanitizer builds of the full tree (M0-07 and M0-08b ran sanitizer builds of their own test targets in their handoffs).

## 3. Not implemented

- Native code generation or a native backend. M0 runs only the checked scalar reference VM.
- SIMD and any assembly kernel.
- Dynamic schemas. The schema is fixed per capsule; code-only installs cannot change layout (SU_E_SCHEMA_MISMATCH).
- Windows. The platform layer is POSIX only.
- Worker crash recovery. Killing the active worker loses its worlds; checkpoints exist but are not restored into a replacement worker (M1, per M0-07 and M0-08b handoffs).
- O(tick) restore. `world.restore` rebuilds the whole world from a canonical snapshot; there is no incremental or tick-journal restore.
- Separate timing of queue, tick-boundary wait and publish. `candidate.install` is synchronous.
- A 1,000,000-row world. The engine refuses it (see section 5).

## 4. Known limitations

- **(Fixed in 93db0d6) Release profile did not build.** `python3 tools/dev.py build --profile release` exits 1 under `-O3 -Werror`: `-Werror=format-truncation` at `src/runtime/reload.c:90` and `tests/control/test_control.c:300-301` (log: `planning/evidence/m0-release-build-attempt.log`). The release numbers below come from a scratch build outside the repository with `-DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-Wno-error=format-truncation`. This waives that one warning class only. It is labelled `release-waiver` everywhere. These defects are in files outside M0-10's write scope and need an owner.
- **(Fixed in d005726) Warm sessions restarted often.** Candidate slots were 16 and never evicted, so each warm epoch is a fresh `serve` process with one warm-up edit, 12 timed edits, and 2 failed-candidate trials (15 submissions). The worker is spawned once per epoch and warm before timing.
- **Host clock.** Each call is timed from request write to reply read, including JSON encode and decode and Python overhead.
- **Samples.** 720 warm edits per profile (60 epochs), 60 cold trials, 20 stress steps, 6 stress edits, one stress snapshot and restore. Native build: n=3 per case.
- **No PMU counters.** Not collected; recorded as unavailable with reason.
- **Verification.** Every timed edit is checked against an independent Python model after its timed window (v1 `p += v`, v2 `p += 2v`, mod 2^32). Every failed candidate was refused, and the active state was verified unchanged after the next tick.
- **Shared VM.** Not a reference machine (PERFORMANCE.md section 3). Numbers are not qualification.

## 5. Stress results

- **1,000,000 rows: refused.** The bench generated a 27,833,861-byte fixture. `world.create` returned `SU_E_RESOURCE_LIMIT`, "cannot read world fixture". The worker reads fixtures up to `WORKER_FIXTURE_MAX` = 8 MiB (`src/runtime/worker.c:27`). This is the recorded result for 1M rows, not a timing.
- **250,000 rows: completed** (largest size that fits the cap, about 27 bytes per row). Dev and release-waiver runs both passed their checks; sampled rows were verified at each step.

## 6. Measured versus proposed targets

Targets are the proposed values in `docs/PERFORMANCE.md` section 2. They are not measurements and no target is declared as met in any spec. p95 is nearest-rank. Source: `planning/evidence/m0-latency-dev-raw.json` and `planning/evidence/m0-latency-release-waiver-raw.json`.

| Target (proposed) | Measured p95 dev / release-waiver | Verdict | Reason |
|---|---|---|---|
| IR prepare p95 <= 20 ms (<=64 KiB source, 1,024-row preview, warm) | capsule.submit 0.633 / 0.438 ms (source 657 bytes) | not comparable | submit is well under 20 ms, but the 1,024-row preview condition is not exercised; `candidate.evaluate` uses the 4-row test world |
| Isolated preview response p95 <= 100 ms (reusable worker, small fixture) | candidate.evaluate 9.023 / 8.564 ms | met | conditions match (4-row fixture); shared-VM observation only |
| Code installation critical section p95 <= 1 ms | candidate.install RPC 1.014 / 0.765 ms | not comparable | the RPC round trip includes queue, boundary and publish; the critical section is not instrumented separately |
| Warm edit to first preview effect p95 <= 100 ms | total 11.759 / 10.410 ms (median 9.757 / 8.948 ms) | met | queue, boundary and worker delays are not separable (see section 3) |
| Targeted native incremental check aim p95 <= 2 s (small target, cached deps, reference machine) | su_vm.c touch: 0.263 to 0.292 s (n=3); ir.h touch: 0.766 to 0.890 s (n=3) | not comparable | target is defined for the reference machine; both are under 2 s here |
| First-party clean dev build aim <= 10 s | configure 0.28 to 0.29 s + build 0.92 to 1.09 s (n=3) | not comparable | observed under 2 s here, but the target is for the reference machine; no dependency builds exist to report separately |
| Failed candidate publication: exactly zero | 0 of 120 failed candidates published (each verified after the next tick) | met | observed in these trials only |
| Memory retired after repeated swaps: bounded | worker RSS change per warm epoch: +4 KiB in every epoch (15 submissions per process) | not comparable | long-run growth not measured; registry (128 records) and slots (16) are bounded by design |

Cold start (new process, first edit), dev / release-waiver, n=60:

| Phase | dev p95 | release-waiver p95 |
|---|---|---|
| first edit total (1,024 rows) | 11.029 ms | 10.574 ms |
| engine.info as first request (median) | 0.909 ms | 1.185 ms |
| world.create (spawns worker; median) | 2.979 ms | 3.086 ms |

Warm per-phase p95, dev / release-waiver, n=720: submit 0.633 / 0.438; evaluate 9.023 / 8.564; install RPC 1.014 / 0.765; step first effect 0.640 / 0.446; tick-boundary wait not separable. Medians and max are in the raw files.

250,000-row stress diagnostic (not a game-scale claim; no target applies), dev / release-waiver:

| Measure | dev | release-waiver |
|---|---|---|
| world.step p95 (n=20) | 21.03 ms | 9.57 ms |
| snapshot host round trip | 305.4 ms | 88.5 ms |
| snapshot size (engine-reported bytes) | 6,940,752 | 6,940,752 |
| restore host round trip | 259.4 ms | 113.9 ms |
| edit total median (install includes a step over 250k rows) | 33.25 ms | 18.26 ms |
| worker VmHWM at end | 68,796 KiB | 68,868 KiB |

## 7. Native build latency (dev profile, Debug, separate from live edit)

Scratch build directory outside the repository, `cmake -S <repo> -B <scratch> -G Ninja -DCMAKE_BUILD_TYPE=Debug`, then `cmake --build --parallel`. Source: `planning/evidence/m0-native-build.json`.

- Clean build: 26 compile steps and 15 link steps. Configure 284 to 292 ms; build 921, 966 and 1,090 ms (n=3).
- Incremental after touching `src/vm/su_vm.c`: 1 compile and 7 links; 292, 263 and 282 ms.
- Incremental after touching `include/sutekh/ir.h`: 17 compiles and 14 links; 890, 766 and 807 ms.
- `touch` changes mtime only; no file content changed.
- No dependency builds exist. p95 is not reported at n=3.

## 8. Reproduction

```sh
python3 tools/dev.py build --profile dev                    # build/sutekh (Debug)
ctest --test-dir build --output-on-failure
python3 -I tests/harness/run_m0.py --binary build/sutekh --report-dir <scratch>
python3 -I tests/harness/run_m0.py --self-test
python3 tools/dev.py build --profile release                 # currently exits 1 (section 4)

# live edit latency, dev profile (about 25 s; writes planning/evidence/m0-latency-dev*.json)
python3 -I bench/reload/bench_reload.py live --binary build/sutekh --profile-label dev --stress-cap-s 600

# release-waiver: scratch build outside the repo, then the same bench
cmake -S . -B <scratch>/release-waiver -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-Wno-error=format-truncation
cmake --build <scratch>/release-waiver --parallel
python3 -I bench/reload/bench_reload.py live --binary <scratch>/release-waiver/sutekh \
  --build-dir <scratch>/release-waiver --profile-label release-waiver --stress-cap-s 600

# native build latency (outside the repo; touches only mtimes)
python3 -I bench/reload/bench_reload.py native --scratch <scratch> --repeats 3
```

The bench creates generated worlds under `build/bench-reload/` (ignored by git). The 1,000,000-row world is deleted after its run.

Evidence files:
- `planning/evidence/m0-latency-dev.json`, `planning/evidence/m0-latency-dev-raw.json`
- `planning/evidence/m0-latency-release-waiver.json`, `planning/evidence/m0-latency-release-waiver-raw.json`
- `planning/evidence/m0-native-build.json`
- `planning/evidence/m0-harness-dev.json`, `planning/evidence/m0-harness-dev-summary.txt`
- `planning/evidence/m0-release-build-attempt.log`
