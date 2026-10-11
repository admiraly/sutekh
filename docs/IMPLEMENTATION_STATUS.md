# Implementation status (M0)

Status as of M0-13 (2026-10-11). This document describes the repository as committed at `3feeacf` (source tree identical to `b5bddbd`, the last source-affecting commit; the later commit only adds evidence). Every figure below comes from a file under `planning/evidence/`; the file is named next to the figure. M0 is not closed: the M0-11 closure audit (`planning/audit/M0-audit.md`) is the reference for the open items in section 4. `planning/tasks.json` and `planning/status.json` belong to the orchestrator and were not edited here.

Shared-VM timing is observation, not performance qualification (ENGINE_CONSTITUTION C10). Targets are the proposed values in `docs/PERFORMANCE.md` section 2; no target is declared met by any spec.

## 1. Per-task status

| Task | Status | Commits (git log) | What exists |
|---|---|---|---|
| M0-00 | submitted; reviewed | 22f7950 (freeze), bd1288f (review), 1b6d55a and 735d698 (r2), 200aa68 (r3) | ABI v1 headers and M0 contract |
| M0-01 | implemented | 114900c | C17 CMake skeleton, platform clock and pid, `tools/dev.py` (`configure`, `build`, `clean`; `--profile dev\|release`) |
| M0-02 | implemented | 2c99818, merge 5e76608 | strict JSON reader, SU-LIR verifier, SHA-256 (`test_ir`) |
| M0-03 | implemented | 98dbab8, merge 914ac34 | fixed-row SoA u32 world; committed, working and staging roots (`test_world`) |
| M0-04 | implemented | bfeae5d, wiring a6b14a8 | checked scalar reference VM (`test_vm`) |
| M0-05 | implemented | 268753a, merge c7ccfce | POSIX supervisor/worker control plane, `serve` and `worker` roles (`test_control`) |
| M0-06 | implemented | 373de54, 2c31bda, b0623b5, 831e9a5 (contract r3.2), 3da93ec, 6058752 | fixtures and stdlib harness `tests/harness/run_m0.py` (T00 to T15) |
| M0-07 | implemented | ce8ae87 | revision registry, serial schedule, safe-point `candidate.install` with evidence gate (`test_reload`) |
| M0-08 | implemented (library half) | 2a579fc | canonical snapshot capture and restore, bounded checkpoint store (`test_snapshot`) |
| M0-08b | implemented | ceeb78f | `world.snapshot` and `world.restore` in the control plane; capsule `max_rows` enforced in the VM (`test_snapshot_cp`) |
| M0-09 | implemented | 80face8 | `tools/demo_zero_wait.py` and `tests/e2e/no_build_probe.py` |
| M0-10 | implemented; superseded by M0-13 | 1d119ec | live-edit and native build bench; its evidence is replaced by M0-13 |
| M0-11 | audit (verdict: cannot close yet) | c81d023 | `planning/audit/M0-audit.md` |
| M0-12 | implemented | 0a26b46 | harness hardening after the audit (T04 matrix, T11 1,000 iterations, T13 per-target verdicts, strace-based T12, runner hygiene) |
| M0-13 | implemented (this pass) | f8583b9, b5bddbd, 3feeacf | bench re-run on the real release build; 1,000,000-row stress run; evidence and this document |

Engine fixes that landed between the M0-10 measurements and M0-13 (commit hashes from git log): 93db0d6 (release-profile `-Werror` format-truncation fixed in `reload.c` and `test_control.c`), d005726 (oldest candidate record evicted when the table is full), d87fcf0 (world fixture cap and snapshot cap raised to 64 MiB), 74da737 and 0acd478 (`capsule.submit` optional `source_path`; T04 uses it), 324dfee (inherited pipe ends closed in spawned workers; request framing size guard), 8675a08 (earlier status doc update).

Engine capabilities advertised by `build/sutekh serve` (`engine.info`): `engine.info`, `capsule.submit`, `candidate.status`, `candidate.evaluate`, `candidate.install`, `world.create`, `world.inspect`, `world.step`, `world.snapshot`, `world.restore`. There is no RPC that closes or destroys a world.

## 2. Build and test results

Platform: Linux 6.18 (container), 4 vCPU Intel Xeon (model string "@ 2.10GHz", hypervisor flag set), 16 GB, GCC 13.3.0, CMake 3.28.3, Ninja 1.11.1, Python 3.13.16.

Builds (`planning/evidence/m0-release-build.log`, `planning/evidence/m0-ctest-dev.log`):

- `python3 tools/dev.py build --profile dev`: exit 0, no warnings. Binary `build/sutekh`, CMAKE_BUILD_TYPE=Debug, SHA-256 `fc8149df...` (full hash in `planning/evidence/m0-artifact-hashes.txt`).
- `python3 tools/dev.py build --profile release`: exit 0, no warnings, with the default `-Werror` (no waiver). Binary CMAKE_BUILD_TYPE=Release, SHA-256 `c9958397...`.

ctest (`planning/evidence/m0-ctest-dev.log`, `planning/evidence/m0-ctest-release.log`): 7 of 7 passed in both profiles (world, ir, vm, control, snapshot, reload, snapshot_cp).

Harness self-test (`planning/evidence/m0-harness-self-test.log`): 138 checks, 0 failures.

Harness, `python3 -I tests/harness/run_m0.py --binary build/sutekh --report-dir <dir>`:

| Test | Dev (Debug) | Release | Notes |
|---|---|---|---|
| T00 Clean bootstrap | passed | **failed** | Compares the binary's version fields with a clean Debug build. The release binary reports `build_type Release`, so the field differs. This is a harness defect (the reference build is always Debug); see section 4. |
| T01 Reference movement | passed | passed | |
| T02 Empty and awkward counts | passed | passed | |
| T03 Numeric edges | passed | passed | |
| T04 Invalid input matrix | passed (33 checks, 28 negative cases) | passed (33 checks) | Coverage guard: required codes and stages present. max_rows is not covered (section 4). |
| T05 Code-only swap | passed | passed | |
| T06 Failed swap | passed | passed | |
| T07 Stale candidate | passed | passed | |
| T08 Snapshot round trip | passed | passed | |
| T09 Working-root failure | passed | passed | |
| T10 Candidate isolation | passed | passed | |
| T11 Repeated reload | passed, 1,000 iterations | passed, 1,000 iterations | 400 successful installs (200 of them reverts), 600 refused installs (200 each of stale revision, past tick, layout generation), 1,000 refused invalid submissions; 0 failed checks; RSS window medians flat (8 MiB bound). |
| T12 No native gameplay build | passed | passed | Primary evidence is the strace probe (section 2.1). PATH shims and `/proc` polling are secondary. |
| T13 Latency decomposition | passed (measurement; verdicts below) | passed (measurement; verdicts below) | The test status does not depend on the verdicts. |
| T14 Deterministic fixture repeat | passed | passed | |
| T15 Structured protocol | passed | passed | |

Totals: dev 16 passed, 0 failed (`planning/evidence/m0-harness-dev-summary.txt`, `planning/evidence/m0-harness-dev.json`). Release 15 passed, 1 failed (T00) (`planning/evidence/m0-harness-release-summary.txt`, `planning/evidence/m0-harness-release.json`).

Harness report, T13 per-target verdicts (provisional, shared VM, C10; `timing_targets` field of the report):

| Target (docs/PERFORMANCE.md section 2) | Dev p95 | Dev verdict | Release p95 | Release verdict | Reason |
|---|---|---|---|---|---|
| IR prepare p95 <= 20 ms | 10.652 ms | not_comparable | 10.525 ms | not_comparable | fixture has 4 rows; target names a 1,024-row preview |
| Isolated preview response p95 <= 100 ms | 8.526 ms | met | 8.993 ms | met | reusable worker, 4-row fixture |
| Install critical section p95 <= 1 ms | 1.406 ms | not_comparable | 1.604 ms | not_comparable | the measured value is the install RPC round trip; the critical section is not isolated |
| Warm edit to first effect p95 <= 100 ms | 12.933 ms | met | 12.977 ms | met | queue, boundary and worker delays are not separable |
| Native targeted incremental check p95 <= 2 s | n/a | not_comparable | n/a | not_comparable | not measured by the harness; see section 2.3 for the bench figures |

Verdicts are recorded by the harness and are provisional (C10). They do not make T13 pass or fail.

### 2.1 No-build probe and demo

`python3 tools/demo_zero_wait.py --binary build/sutekh --out <file>` (`planning/evidence/m0-demo-dev.json`): status passed on the dev binary.

`python3 -I tests/e2e/no_build_probe.py --binary build/sutekh --out <file>` (`planning/evidence/m0-probe-dev.json`, dev binary): verdict pass. Both positive controls were detected (bare `cc` through PATH, and `/usr/bin/cc` by absolute path). 43 exec attempts traced, 7 successful; executed basenames git, python3, sutekh, uname. Build-tool execs in the edit window: 0. In the whole run: 0.

The demo and probe were run on the dev binary only in this pass. The release binary passed the harness T12 check, which includes the probe.

### 2.2 Live-edit bench (`bench/reload/bench_reload.py live`)

Both profiles: 720 warm edits (60 epochs of 12 timed edits, each verified against an independent Python model after its timed window), 60 cold trials, 120 failed-candidate trials (0 published), 1,000,000-row and 250,000-row stress runs. Each warm epoch uses a fresh `serve` process (section 4 explains why). Status `passed` in both raw files.

Warm edit (`capsule.submit` + `candidate.evaluate` + `candidate.install` + first `world.step`), host clock, n=720, p95 (median):

| Measure | Dev p95 (median) | Release p95 (median) |
|---|---|---|
| total edit to first effect | 10.406 ms (8.614) | 9.325 ms (8.094) |
| capsule.submit | 0.480 ms | 0.371 ms |
| candidate.evaluate (4-row world) | 8.163 ms | 7.594 ms |
| candidate.install RPC | 0.843 ms | 0.688 ms |
| first world.step after install | 0.618 ms | 0.473 ms |

Cold start (new process, first edit), n=60: total p95 9.153 ms (dev), 9.003 ms (release).

Failed candidates: 0 of 120 published in both profiles (each refused, state verified unchanged after the next tick).

Sources: `planning/evidence/m0-latency-dev.json`, `planning/evidence/m0-latency-dev-raw.json`, `planning/evidence/m0-latency-release.json`, `planning/evidence/m0-latency-release-raw.json`. Run logs: `planning/evidence/m0-latency-dev-run.log`, `planning/evidence/m0-latency-release-run.log`.

### 2.3 Stress (diagnostic, not a game-scale claim; no target applies)

One world per size, 20 steps, 6 edits, one snapshot and restore. Dev / release:

| Measure | 1,000,000 rows | 250,000 rows |
|---|---|---|
| Status | passed / passed | passed / passed |
| Fixture bytes | 27,833,861 | 6,875,843 |
| world.create host time | 1,330.1 / 853.0 ms | 96.7 / 93.0 ms |
| world.step p95 (median), n=20 | 64.03 (42.19) / 22.41 (12.49) ms | 12.44 (9.68) / 5.25 (4.95) ms |
| snapshot bytes | 28,092,684 | 6,940,752 |
| snapshot host time (engine capture) | 550.4 (549.6) / 194.6 (193.9) ms | 188.6 (188.1) / 63.2 (62.4) ms |
| restore host time (to tick 26) | **1,313.6 / 1,042.7 ms** | 148.6 / 111.4 ms |
| median total edit to first effect (6 edits) | 66.96 / 24.43 ms | 26.82 / 14.53 ms |
| worker VmHWM after create | 178,536 / 178,608 KiB | 46,348 / 46,424 KiB |
| worker VmHWM at end of run | 267,344 / 267,468 KiB | 68,800 / 68,748 KiB |

The 1,000,000-row run passed with every edit verified against the reference model. Restore is slow at this size (section 4: O(tick) restore). The fixture is generated under `build/bench-reload/` and removed after the run.

### 2.4 Native build latency (`bench/reload/bench_reload.py native`, dev profile, Debug, n=3)

Source: `planning/evidence/m0-native-build.json`, log `planning/evidence/m0-native-build-run.log`. Scratch build outside the repository.

- Clean: 26 compile steps, 15 link steps. Configure 201.5 to 225.2 ms; build 591.7 to 687.3 ms.
- Incremental after touching `src/vm/su_vm.c`: 1 compile, 7 links. Build 183.3 to 189.3 ms.
- Incremental after touching `include/sutekh/ir.h`: 17 compiles, 14 links. Build 549.2 to 572.8 ms.

These are three samples each; no p95 is reported. There are no dependency builds.

## 3. Not implemented

- Native code generation or a native backend. M0 runs only the checked scalar reference VM.
- SIMD and any assembly kernel.
- Dynamic schemas. The schema is fixed per capsule; code-only installs cannot change layout (`SU_E_SCHEMA_MISMATCH`).
- Windows. The platform layer is POSIX only.
- Worker crash recovery. Killing the active worker loses its worlds. Checkpoints exist but are not restored into a replacement worker.
- Incremental or tick-journal restore. `world.restore` rebuilds the whole world from a canonical snapshot.
- A world-close RPC. A worker holds at most 4 worlds (`WORKER_MAX_WORLDS` in `src/runtime/worker.c`); nothing closes one.
- Separate timing of queue, tick-boundary wait and publish. `candidate.install` is synchronous.

## 4. Known limitations and gaps

- **T00 fails on the release binary.** The clean-build reference in T00 is always Debug, so the `build_type` field differs. This is a harness defect (`tests/harness/run_m0.py`, outside M0-13's write scope). The release binary passes the rest of the suite. The defect needs an owner.
- **Warm sessions restart per epoch.** The bench still starts one `serve` process per warm epoch. `--share-serve` (one process for all 60 epochs) was tried and failed on the fifth `world.create` in one process with `SU_E_RESOURCE_LIMIT`, "worker already holds the maximum number of worlds" (a worker holds 4) (`planning/evidence/m0-latency-dev-shared-serve-attempt.log`). Candidate-record eviction (d005726) is not the limit; the world cap is. Removing the restart needs a world-close RPC or a per-epoch world reuse design.
- **O(tick) restore is not implemented.** `world.restore` at 1,000,000 rows took 1,313.6 ms (dev) and 1,042.7 ms (release) on this host. Only two sizes were measured (250,000 and 1,000,000 rows).
- **Worker death loses worlds.** See section 3. The 1,000,000-row worker peaks at about 267 MiB VmHWM.
- **Provisional target verdicts.** The T13 verdicts are recorded by the harness and are not a pass or fail gate. Two targets are not comparable (IR prepare on a 4-row fixture; install without an isolated critical section). The native incremental target is not measured by the harness.
- **Shared VM.** Not a reference machine (`docs/PERFORMANCE.md` section 3). Numbers are not qualification. PMU counters were not collected (recorded as unavailable in the bench raw files).
- **Sanitizers.** The strace probe is inconclusive under AddressSanitizer: the ASan `serve` exits 1 at shutdown when LeakSanitizer runs under ptrace (recorded in `planning/handoffs/2026-10-11-m0-12.md`). T11 passes under ASan with `quarantine_size_mb=1`. The probe was not re-run under ASan in this pass.
- **T04 gap: capsule `limits.max_rows`.** No T04 negative case sends a capsule whose `limits.max_rows` is exceeded. T09 covers the runtime failure; M08 (ignore `max_rows`) is caught only by T09. Needs a case with its expected code from the contract.
- **T04 gap: `world.create` or `world.step` row-limit cases** are not in T04 either.
- **`snapshot_format` is not verified.** It sits inside the snapshot document, which no RPC returns.
- **Stage 4 vs 5 for numeric profile** (from M0-12): a mismatch cannot be generated while M0 supports one profile; the expected code is TYPE. The spec owner should confirm.
- **`su_proc.c` (audit defects 11 and 12):** non-CLOEXEC duplicated pipe ends in the worker and an unchecked `req_len + 1u`. Not changed here; they are in `src/`.

## 5. Measured versus proposed targets

Proposed targets are from `docs/PERFORMANCE.md` section 2. Measured values are from `planning/evidence/` (dev and release, as labelled). p95 is nearest-rank.

| Target (proposed) | Measured dev / release | Verdict | Basis |
|---|---|---|---|
| IR prepare p95 <= 20 ms (<=64 KiB source, 1,024-row preview) | capsule.submit p95 0.480 / 0.371 ms; harness prepare p95 10.652 / 10.525 ms | not_comparable | the 1,024-row preview condition is not exercised; the harness fixture has 4 rows |
| Isolated preview response p95 <= 100 ms | candidate.evaluate (reusable worker) p95 8.526 / 8.993 ms (harness); 8.163 / 7.594 ms (bench) | met | conditions match (4-row fixture); shared VM |
| Code installation critical section p95 <= 1 ms | install RPC p95 1.406 / 1.604 ms (harness); 0.843 / 0.688 ms (bench) | not_comparable | the RPC contains queue, boundary and publish; the critical section is not isolated |
| Warm edit to first preview effect p95 <= 100 ms | total p95 10.406 / 9.325 ms (bench); 12.933 / 12.977 ms (harness) | met | queue, boundary and worker delays are not separable |
| Targeted native incremental check p95 <= 2 s | su_vm.c touch build 183.3 to 189.3 ms; ir.h touch 549.2 to 572.8 ms (n=3, dev) | not_comparable | target is defined for the reference machine |
| First-party clean dev build aim <= 10 s | configure 201.5 to 225.2 ms + build 591.7 to 687.3 ms (n=3, dev) | not_comparable | target is defined for the reference machine |
| Failed candidate publication: exactly zero | 0 of 120 / 0 of 120 published | met | observed in these trials only |
| Memory retired after repeated swaps: bounded | epoch worker VmRSS 2,276 KiB first, 2,280 KiB last (dev); 2,288 and 2,284 KiB (release) | not_comparable | 60 epochs of 15 submissions each; long-run growth is not measured |

## 6. Reproduction

Run from the repository root. Each command writes its output to the path shown; the live bench overwrites the `planning/evidence/m0-latency-<label>*` files, so run it on a clean tree or restore them with git afterwards.

```sh
# 1. Clean builds (both profiles; the release build has no warning waiver)
python3 tools/dev.py clean
python3 tools/dev.py build --profile dev                    # build/sutekh (Debug)
ctest --test-dir build --output-on-failure                  # 7/7
python3 tools/dev.py build --profile release                # build/sutekh (Release)
ctest --test-dir build --output-on-failure                  # 7/7

# 2. Harness (T00-T15) and its self-test
python3 -I tests/harness/run_m0.py --binary build/sutekh --report-dir <scratch>/harness
python3 -I tests/harness/run_m0.py --self-test

# 3. Zero-wait live-edit demo and no-build strace probe (dev binary shown)
python3 tools/dev.py build --profile dev
python3 tools/demo_zero_wait.py --binary build/sutekh --out <scratch>/demo.json
python3 -I tests/e2e/no_build_probe.py --binary build/sutekh --out <scratch>/probe.json

# 4. Live-edit bench, including the 1,000,000-row stress (about 20 s warm and cold; the stress runs take minutes)
python3 -I bench/reload/bench_reload.py live --binary build/sutekh --build-dir build --profile-label dev --stress-cap-s 600
python3 tools/dev.py clean && python3 tools/dev.py build --profile release
python3 -I bench/reload/bench_reload.py live --binary build/sutekh --build-dir build --profile-label release --stress-cap-s 600

# 5. Native build latency (outside the repository)
python3 -I bench/reload/bench_reload.py native --scratch <scratch> --repeats 3
```

Evidence files (`planning/evidence/`):

- Harness: `m0-harness-dev.json`, `m0-harness-dev-summary.txt`, `m0-harness-release.json`, `m0-harness-release-summary.txt`, `m0-harness-self-test.log`
- Build and tests: `m0-release-build.log`, `m0-ctest-dev.log`, `m0-ctest-release.log`
- Demo and probe: `m0-demo-dev.json`, `m0-probe-dev.json`
- Live edit: `m0-latency-dev.json`, `m0-latency-dev-raw.json`, `m0-latency-dev-run.log`, `m0-latency-release.json`, `m0-latency-release-raw.json`, `m0-latency-release-run.log`, `m0-latency-dev-shared-serve-attempt.log`
- Native build: `m0-native-build.json`, `m0-native-build-run.log`
- Hashes: `m0-artifact-hashes.txt` (binary and script SHA-256, and the run-to-binary mapping)
