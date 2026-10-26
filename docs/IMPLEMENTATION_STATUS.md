# Implementation status

## Current checkpoint — 14 October 2026

M0–M2 are owner-accepted with the caveats retained in `planning/status.json`. M3 remains in progress. Current work includes the six-system RTS runtime, atomic batch control methods, per-system structural queues, and replacement-worker recovery after actual host faults.

Independent model comparison now reproduces snapshot 0.2 and retained-event digests: all seven committed scenes through four ticks (up to 65,536 rows), and three small scenes through 64 ticks, match engine state/events/counts. Five RPC edge slices pass with worker counts 1/2. M3-M27 is detected as a semantic divergence at tick 3; host mutation refresh detects 23 of 25 variants, with two documented equivalents. See `planning/handoffs/2026-10-14-m3-05-model-digest.md` and `planning/handoffs/2026-10-14-m3-06-edges-targeting.md`.

Owner-authorized runtime limits v2 resolves the measured K128 targeting failure and incompatible scale limits: K160 passed both unchanged committed targeting scenes, with up to 1,024 unique eligible queriers. A real 1,000,000-row supervisor run passed load, 127,345,544-byte canonical capture, atomic capacity refusal, ceiling reuse, replacement-worker recovery and the next tick. The restore memory repair passed independent review and sanitizer checks; all 49 default tests passed. These are checked-mode correctness results, not performance qualification. See `planning/handoffs/2026-10-14-m3-limits-v2.md`. M3 behavior experiments, stress curves and optimization gates remain in progress.

Default integration now passes 49/49; focused native-enabled checks pass 6/6. The reference mutation refresh detects 32 of 33 variants, with one documented equivalent. Exact commands, hashes and logs are in `planning/evidence/m3-model-digest/`, `m3-load-limits/`, `m3-e2e-targeting/`, and `m3-native-mutation-refresh/`. These are correctness observations, not timing qualification. Native Windows and independent review are unrun; the existing WSL AVX2 XSAVE feasibility failure remains documented and unwaived.

## Historical M0 evidence


Status as of M0-14 (2026-10-11). This document describes the source tree at commit `d352b54` (the last M0-14 source commit; M0-14 source commits are `8a2f68c`, `5cad0d3`, `d352b54`). Every harness report in `planning/evidence/` records `repo_head` `d352b54`. The evidence commit that adds `planning/evidence/` and this document changes no source. Every figure below comes from a file under `planning/evidence/`; the file is named next to the figure. M0 was CLOSED by owner decision on 2026-10-11 at accepted revision 9af40ccee115231b5682e7f0248c9cbd1729ed6b (release binary sha256 d5714cb06c351ec16a2709ff2bd9c12eaf7b3bb8cbafa7148980ca7fd7fa8acf) with the caveats recorded in planning/status.json. Historical note from the earlier text: M0 was not closed: the M0-11 closure audit (`planning/audit/M0-audit.md`) and the M0-reaudit (`planning/audit/M0-reaudit.md`) are the references for the open items in section 4. `planning/tasks.json` and `planning/status.json` belong to the orchestrator and were not edited here.

Shared-VM timing is observation, not performance qualification (ENGINE_CONSTITUTION C10). Targets are the proposed values in `docs/PERFORMANCE.md` section 2; no target is declared met by any spec. Sections 2.2 to 2.4 are M0-13 bench figures from an earlier tree (not re-run in M0-14).

## 1. Per-task status

| Task | Status | Commits (git log) | What exists |
|---|---|---|---|
| M0-00 | submitted; reviewed | e1f63c7 (freeze), d7c935b (review), c6d4f59 and 498470f (r2), f51a38a (r3) | ABI v1 headers and M0 contract |
| M0-01 | implemented | 267f9be | C17 CMake skeleton, platform clock and pid, `tools/dev.py` (`configure`, `build`, `clean`; `--profile dev\|release`) |
| M0-02 | implemented | 895e433, merge 7f821c6 | strict JSON reader, SU-LIR verifier, SHA-256 (`test_ir`) |
| M0-03 | implemented | 399b00c, merge 6a3e843 | fixed-row SoA u32 world; committed, working and staging roots (`test_world`) |
| M0-04 | implemented | 1361f93, wiring 75c6713 | checked scalar reference VM (`test_vm`) |
| M0-05 | implemented | f4cd562, merge eafbe7a | POSIX supervisor/worker control plane, `serve` and `worker` roles (`test_control`) |
| M0-06 | implemented | 2109989, 50b2a6f, e36e5ca, 4d8150c (contract r3.2), feddb56, 8e6c8a2 | fixtures and stdlib harness `tests/harness/run_m0.py` (T00 to T15) |
| M0-07 | implemented | 77fc90f | revision registry, serial schedule, safe-point `candidate.install` with evidence gate (`test_reload`) |
| M0-08 | implemented (library half) | cc50278 | canonical snapshot capture and restore, bounded checkpoint store (`test_snapshot`) |
| M0-08b | implemented | 4928890 | `world.snapshot` and `world.restore` in the control plane; capsule `max_rows` enforced in the VM (`test_snapshot_cp`) |
| M0-09 | implemented | 0630e77 | `tools/demo_zero_wait.py` and `tests/e2e/no_build_probe.py` |
| M0-10 | implemented; superseded by M0-13 | 369583f | live-edit and native build bench; its evidence is replaced by M0-13 |
| M0-11 | audit (verdict: cannot close yet) | 465d5fa | `planning/audit/M0-audit.md` |
| M0-12 | implemented | 22e112c | harness hardening after the audit (T04 matrix, T11 1,000 iterations, T13 per-target verdicts, strace-based T12, runner hygiene) |
| M0-13 | implemented | 8b94674, aae7112, 8108166 | bench re-run on the real release build; 1,000,000-row stress run |
| M0-14 | implemented (this pass; M0 not closed) | 8a2f68c, 5cad0d3, d352b54 | install critical section measured in the engine; T13 gates observed, not assumed; T11 failed-evaluation installs; T04 runtime `max_rows` case; candidate worker record checked against the OS; native incremental check measured by the harness |

Engine fixes that landed between the M0-10 measurements and M0-13 (commit hashes from git log): 4c64deb (release-profile `-Werror` format-truncation fixed in `reload.c` and `test_control.c`), c811a6d (oldest candidate record evicted when the table is full), d8ddcf8 (world fixture cap and snapshot cap raised to 64 MiB), c14010e and 22e28bd (`capsule.submit` optional `source_path`; T04 uses it), 5619a58 (inherited pipe ends closed in spawned workers; request framing size guard), 9ec6106 (earlier status doc update).

M0-14 engine change (8a2f68c): `candidate.install` returns `bindings_prepared`, `prepare_ns` (install entry to the start of the publish) and `critical_section_ns` (registry publish through the new schedule entry in place), on the monotonic clock; both timings are `null` if the clock is unreadable. The old entry is retired and freed after the section.

Engine capabilities advertised by `build/sutekh serve` (`engine.info`): `engine.info`, `capsule.submit`, `candidate.status`, `candidate.evaluate`, `candidate.install`, `world.create`, `world.inspect`, `world.step`, `world.snapshot`, `world.restore`. There is no RPC that closes or destroys a world.

## 2. Build and test results

Platform: Linux 6.18 (container), 4 vCPU Intel Xeon (model string "@ 2.10GHz", hypervisor flag set), GCC 13.3.0, CMake 3.28.3, Ninja 1.11.1, Python 3.13.16. This is not the reference machine (section 5).

Builds, each after `python3 tools/dev.py clean` (`planning/evidence/m0-dev-clean.log`, `planning/evidence/m0-release-clean.log`):

- `python3 tools/dev.py build --profile dev`: exit 0 (`planning/evidence/m0-dev-build.log`). Binary `build/sutekh`, CMAKE_BUILD_TYPE=Debug, SHA-256 `eb121da9db55754a37e9505b0a338bdb1dd473d2c3fe6fa62d35fd61768d0939`.
- `python3 tools/dev.py build --profile release`: exit 0 (`planning/evidence/m0-release-build.log`), default `-Werror`, no waiver. Binary CMAKE_BUILD_TYPE=Release, SHA-256 `d5714cb06c351ec16a2709ff2bd9c12eaf7b3bb8cbafa7148980ca7fd7fa8acf`.

ctest: 7 of 7 passed in both profiles (`planning/evidence/m0-ctest-dev.log`, `planning/evidence/m0-ctest-release.log`): world, ir, vm, control, snapshot, reload, snapshot_cp.

Harness self-test (`planning/evidence/m0-harness-self-test.log`, recorded at M0 closure): 150 checks, 0 failures. The current self-test has 218 checks, 0 failures (M1 closure D, `run_m0.py --self-test` on the release binary).

Harness, `python3 -I tests/harness/run_m0.py --binary build/sutekh --report-dir <dir>` (T00 included; the report records `repo_head`, binary SHA-256 and the harness SHA-256):

| Test | Dev (Debug) | Release | Notes |
|---|---|---|---|
| T00 Clean bootstrap | passed (9 checks) | passed (9 checks) | Clean reference build with the binary's build type. |
| T01 Reference movement | passed | passed | |
| T02 Empty and awkward counts | passed | passed | |
| T03 Numeric edges | passed | passed | |
| T04 Invalid input matrix | passed (37 checks; 28 negative cases; runtime `max_rows` case) | passed (37 checks) | `max_rows` case: `world_8` (8 rows) with a capsule of `limits.max_rows` 4 returns `SU_E_RESOURCE_LIMIT`; the committed root is unchanged; an unmodified control steps. |
| T05 Code-only swap | passed (23 checks) | passed | Candidate record `worker_pid` must name the live active worker (OS ppid and identity). |
| T06 Failed swap | passed | passed | |
| T07 Stale candidate | passed | passed | |
| T08 Snapshot round trip | passed | passed | |
| T09 Working-root failure | passed | passed | |
| T10 Candidate isolation | passed | passed | |
| T11 Repeated reload | passed, 1,000 iterations (6,350 checks) | passed, 1,000 iterations (6,350 checks) | Six-kind cycle. Successful installs 334 (167 installs, 167 reverts). Refused installs 666 (167 stale revision, 167 past tick, 166 layout generation, 166 failed evaluation: `SU_E_CANDIDATE_FAILED`). 1,000 refused invalid submissions. RSS window medians within the 8 MiB harness bound. |
| T12 No native gameplay build | passed | passed | Primary evidence is the strace probe. |
| T13 Latency decomposition | passed (192 checks; verdicts in section 2.1) | passed (192 checks) | The test status does not depend on the verdicts. |
| T14 Deterministic fixture repeat | passed | passed | |
| T15 Structured protocol | passed | passed | |

Totals: dev 16 passed, 0 failed (`planning/evidence/m0-harness-dev-summary.txt`, `planning/evidence/m0-harness-dev.json`, `repo_head` d352b54, binary eb121da9). Release 16 passed, 0 failed (`planning/evidence/m0-harness-release-summary.txt`, `planning/evidence/m0-harness-release.json`, binary d5714cb0).

### 2.1 T13 timing targets (provisional; shared VM, C10)

Warm series run in one serve process after 3 untimed warm-up cycles; the process identity is read from the OS on every cycle. Each gate is a fact observed by the harness or reported by the engine. Cold series (fresh process per sample) are reported in the report's `latency_ms` but are not compared. n = 30 timed samples per series; p95 is nearest-rank, in ms. Source: `planning/evidence/m0-harness-dev.json` and `planning/evidence/m0-harness-release.json`, field `timing_targets`.

| Target (docs/PERFORMANCE.md section 2) | Dev p95 | Dev verdict | Release p95 | Release verdict | Reason (both profiles) |
|---|---|---|---|---|---|
| IR prepare p95 <= 20 ms (1,024-row preview, warm) | 12.960 | met | 10.442 | met | All four gates observed true: source 483 bytes; capsule schema equals the fixture's; one serve process and worker kept their identity; the evaluation resolves `world_1024.json` (1,024 rows). Timed: `capsule.submit` plus `candidate.evaluate`. |
| Isolated preview response p95 <= 100 ms | 8.641 | not_comparable | 7.991 | not_comparable | Gate "reusable worker" observed false: `candidate.evaluate` spawns a disposable worker per request (`src/agent/su_agent.c`), so 30 evaluations reported 30 different worker pids. The other gates (small fixture; only the edit test file ran) are true. |
| Code installation critical section p95 <= 1 ms | 0.001 | met | 0.001 | met | Measured inside the engine (`critical_section_ns`, monotonic clock): on dev, median 0.642 us and p95 1.153 us in raw nanoseconds (release: median 0.581 us, p95 1.073 us); the reported ms p95 is rounded to 0.001 ms. Gates true: code-only swap; `bindings_prepared` true on all 30 installs; no migration; clock readable. |
| Warm edit to first preview effect p95 <= 100 ms | 13.075 | met | 12.581 | met | Warm process gate true. Median breakdown (dev): submit 0.743 ms, evaluate 7.590 ms, install RPC 0.895 ms, first step 0.565 ms. |
| Targeted native incremental check p95 <= 2 s | 198.721 | not_comparable | 320.486 | not_comparable | Gates true: one unit (`src/vm/su_vm.c`) compiled per incremental build; no reconfigure. Gate "reference machine" observed false: CPU model "Intel(R) Xeon(R) Processor @ 2.10GHz", reference i7-14700K. The p95 is recorded; the aim is not judged on this machine. Scratch copy; n = 30 touch-and-build runs. |

Verdicts are recorded by the harness and are provisional (C10). They do not make T13 pass or fail. The reusable-worker gate is a fact about the engine's design: meeting that target under its stated conditions needs an owner decision on a persistent evaluation worker, which is not implemented.

### 2.2 No-build probe and demo (both profiles)

`python3 tools/demo_zero_wait.py --binary build/sutekh --out <file>`: status passed in both profiles (`planning/evidence/m0-demo-dev.json`, `planning/evidence/m0-demo-release.json`).

`python3 -I tests/e2e/no_build_probe.py --binary build/sutekh --out <file>`: exit 0 in both profiles (`planning/evidence/m0-probe-dev.json`, `planning/evidence/m0-probe-release.json`). Verdicts and counts are in those files; the logs are `planning/evidence/m0-probe-*.log`.

### 2.3 Sanitizers (ASan + UBSan, Debug, scratch build outside the repository)

Sources from `git archive d352b54`; `-fsanitize=address,undefined -fno-sanitize-recover=undefined`.

- ctest: 7 of 7, no sanitizer reports (`planning/evidence/m0-asan-ctest.log`).
- Harness with the ASan binary, `--no-build` (`planning/evidence/m0-asan-harness-summary.txt`): 14 passed, 0 failed. T00 `not_run` (the flag skips the clean build). T12 `not_run`: the strace probe is inconclusive under ASan, because the demo did not complete with status passed under the probe's ptrace (see section 4). T04, T05, T06, T09, T10, T11 (1,000 iterations) and T13 passed under ASan.

### 2.4 Live-edit bench (`bench/reload/bench_reload.py live`), M0-13 tree

Both profiles: 720 warm edits (60 epochs of 12 timed edits), 60 cold trials, 120 failed-candidate trials (0 published), 1,000,000-row and 250,000-row stress runs. Each warm epoch uses a fresh `serve` process (section 4). Status `passed` in both raw files. These were produced at aae7112/8108166 and were not re-run in M0-14.

Warm edit, host clock, n=720, p95 (median):

| Measure | Dev p95 (median) | Release p95 (median) |
|---|---|---|
| total edit to first effect | 10.406 ms (8.614) | 9.325 ms (8.094) |
| capsule.submit | 0.480 ms | 0.371 ms |
| candidate.evaluate (4-row world) | 8.163 ms | 7.594 ms |
| candidate.install RPC | 0.843 ms | 0.688 ms |
| first world.step after install | 0.618 ms | 0.473 ms |

Cold start (new process, first edit), n=60: total p95 9.153 ms (dev), 9.003 ms (release). Failed candidates: 0 of 120 published in both profiles.

Sources: `planning/evidence/m0-latency-dev.json`, `planning/evidence/m0-latency-dev-raw.json`, `planning/evidence/m0-latency-release.json`, `planning/evidence/m0-latency-release-raw.json`. Run logs: `planning/evidence/m0-latency-dev-run.log`, `planning/evidence/m0-latency-release-run.log`.

### 2.5 Stress (diagnostic, not a game-scale claim; no target applies), M0-13 tree

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

The fixture is generated under `build/bench-reload/` and removed after the run.

### 2.6 Native build latency (`bench/reload/bench_reload.py native`, dev profile, Debug, n=3), M0-13 tree

Source: `planning/evidence/m0-native-build.json`, log `planning/evidence/m0-native-build-run.log`. Clean: 26 compile steps, 15 link steps; configure 201.5 to 225.2 ms; build 591.7 to 687.3 ms. Incremental after touching `src/vm/su_vm.c`: 1 compile, 7 links, build 183.3 to 189.3 ms. Incremental after touching `include/sutekh/ir.h`: 17 compiles, 14 links, build 549.2 to 572.8 ms. Three samples each; no p95. The M0-14 native check in section 2.1 supersedes this for the targeted incremental figure.

### 2.7 Scratch mutation checks (M0-14; mutants are not committed)

Each mutant is a copy of the committed tree with one anchored edit, built in Debug outside the repository, and run against the harness.

| Mutant | Red tests | Result |
|---|---|---|
| Ignore capsule `limits.max_rows` (`su_vm.c`) | T04 (new `max_rows` case), and T09 | Caught in T04: "accepted, expected rejection SU_E_RESOURCE_LIMIT" |
| Install records pid+1 as candidate `worker_pid` (`su_agent.c` `note_install`) | T05 and T11 | Caught: "candidate record worker_pid ... is not a live process" in both |
| Evidence gate off (`reload.c` step 7 accepts a failed evaluation) | T06 and T11 | Caught: "install refused with SU_E_CANDIDATE_FAILED: accepted" in both |

## 3. Not implemented

- Native code generation or a native backend. M0 runs only the checked scalar reference VM.
- SIMD and any assembly kernel.
- Dynamic schemas. The schema is fixed per capsule; code-only installs cannot change layout (`SU_E_SCHEMA_MISMATCH`).
- Windows. The platform layer is POSIX only.
- Worker crash recovery. Killing the active worker loses its worlds. Checkpoints exist but are not restored into a replacement worker.
- Incremental or tick-journal restore. `world.restore` rebuilds the whole world from a canonical snapshot.
- A world-close RPC. A worker holds at most 4 worlds (`WORKER_MAX_WORLDS` in `src/runtime/worker.c`); nothing closes one.
- A persistent evaluation worker. `candidate.evaluate` spawns a disposable worker per request.
- Separate timing of queue, tick-boundary wait and publish across the RPC. The engine reports `prepare_ns` and `critical_section_ns`; `candidate.install` is synchronous.

## 4. Known limitations and gaps

- **(Fixed in 16c3393) T00 failed on the release binary** because the reference build was always Debug; it now follows the binary under test.
- **(Fixed in M0-14) T04 gap: capsule `limits.max_rows`.** A runtime case now sends a capsule with `max_rows` 4 against an 8-row world. The mutant that ignores `max_rows` is caught in T04 as well as T09.
- **(Fixed in M0-14) T13 gate facts were hard-coded.** Each gate is now observed; a missing fact gives `not_comparable`. The self-test covers `not_met` on a slow sample and `not_comparable` on a missing fact.
- **(Fixed in M0-14) Candidate record pid.** The candidate record's `worker_pid` is checked against the live active worker (OS ppid and identity) in T05 and T11. The pid+1 mutant is caught.
- **(Fixed in M0-14) Failed-evaluation installs.** T11 installs a failed-evaluation candidate in each cycle and expects `SU_E_CANDIDATE_FAILED`.
- **Preview target not comparable.** The reusable-worker condition is not met by design: each `candidate.evaluate` runs in a new worker. Making it comparable needs an owner decision on a persistent evaluation worker, which is not implemented.
- **Native target not comparable on this machine.** The incremental p95 is recorded (198.7 ms dev, 320.5 ms release), against the 2 s aim, but the reference-machine condition is not met here.
- **Worker death loses worlds.** See section 3. The 1,000,000-row worker peaks at about 267 MiB VmHWM (M0-13 tree).
- **Warm sessions restart per epoch in the bench.** The bench still starts one `serve` process per warm epoch. `--share-serve` (one process for all 60 epochs) failed on the fifth `world.create` with `SU_E_RESOURCE_LIMIT` (`planning/evidence/m0-latency-dev-shared-serve-attempt.log`). The T13 warm series avoids this by using one world.
- **O(tick) restore is not implemented.** `world.restore` at 1,000,000 rows took 1,313.6 ms (dev) and 1,042.7 ms (release) on this host (M0-13 tree).
- **Shared VM.** Not a reference machine (`docs/PERFORMANCE.md` section 3). Numbers are not qualification. PMU counters were not collected.
- **Sanitizers.** The strace probe is inconclusive under AddressSanitizer: the ASan `serve` exits 1 at shutdown when LeakSanitizer runs under ptrace (`planning/handoffs/2026-10-11-m0-12.md`). T12 is `not_run` under ASan; the demo passes under ASan when run directly (not re-checked in M0-14). Under the M0-14 ASan build T11 passes with `quarantine_size_mb=1`.
- **`snapshot_format` is not verified.** It sits inside the snapshot document, which no RPC returns.
- **Stage 4 vs 5 for numeric profile** (from M0-12): a mismatch cannot be generated while M0 supports one profile; the expected code is TYPE. The spec owner should confirm.
- **`su_proc.c` (audit defects 11 and 12):** resolved earlier (child closes duplicated pipe ends; `req_len == SIZE_MAX` rejected). Recorded here for history only.

## 5. Measured versus proposed targets

Proposed targets are from `docs/PERFORMANCE.md` section 2. Measured values are the T13 warm-series p95 from `planning/evidence/m0-harness-{dev,release}.json` (n=30, nearest-rank). Verdicts are the harness's `timing_targets`.

| Target (proposed) | Measured dev / release (p95) | Verdict dev / release | Basis |
|---|---|---|---|
| IR prepare p95 <= 20 ms (<=64 KiB source, 1,024-row preview, warm) | 12.960 / 10.442 ms | met / met | all four conditions observed |
| Isolated preview response p95 <= 100 ms (reusable worker) | 8.641 / 7.991 ms | not_comparable / not_comparable | reusable-worker condition observed false (disposable worker per evaluation) |
| Code installation critical section p95 <= 1 ms | 0.001 / 0.001 ms (engine clock) | met / met | section measured inside the engine; all conditions observed |
| Warm edit to first preview effect p95 <= 100 ms | 13.075 / 12.581 ms | met / met | warm process observed |
| Targeted native incremental check p95 <= 2 s (reference machine) | 198.721 / 320.486 ms | not_comparable / not_comparable | reference-machine condition observed false (this CPU) |
| First-party clean dev build aim <= 10 s | configure 201.5 to 225.2 ms + build 591.7 to 687.3 ms (n=3, dev, M0-13 tree) | not_comparable | target is defined for the reference machine; not re-measured in M0-14 |
| Failed candidate publication: exactly zero | 0 of 120 / 0 of 120 published (M0-13 bench); T11 refused 166 failed-evaluation installs and 666 refused installs in total | met | observed in these trials only |
| Memory retired after repeated swaps: bounded | epoch worker VmRSS 2,276 KiB first, 2,280 KiB last (dev, M0-13 bench) | not_comparable | 60 epochs of 15 submissions each; long-run growth is not measured |

## 6. Reproduction

Run from the repository root. Each command writes its output to the path shown.

```sh
# 1. Clean builds (both profiles; the release build has no warning waiver)
python3 tools/dev.py clean
python3 tools/dev.py build --profile dev                    # build/sutekh (Debug)
ctest --test-dir build --output-on-failure                  # 7/7
python3 tools/dev.py build --profile release                # build/sutekh (Release)
ctest --test-dir build --output-on-failure                  # 7/7

# 2. Harness (T00-T15, T00 builds a clean reference outside the repository) and its self-test
python3 -I tests/harness/run_m0.py --binary build/sutekh --report-dir <scratch>/harness
python3 -I tests/harness/run_m0.py --self-test

# 3. Zero-wait live-edit demo and no-build strace probe (per profile)
python3 tools/demo_zero_wait.py --binary build/sutekh --out <scratch>/demo.json
python3 -I tests/e2e/no_build_probe.py --binary build/sutekh --out <scratch>/probe.json

# 4. Live-edit bench, including the 1,000,000-row stress (about 20 s warm and cold; the stress runs take minutes)
python3 -I bench/reload/bench_reload.py live --binary build/sutekh --build-dir build --profile-label dev --stress-cap-s 600
python3 tools/dev.py clean && python3 tools/dev.py build --profile release
python3 -I bench/reload/bench_reload.py live --binary build/sutekh --build-dir build --profile-label release --stress-cap-s 600

# 5. Native build latency (outside the repository; the T13 native check runs the same kind of build in scratch)
python3 -I bench/reload/bench_reload.py native --scratch <scratch> --repeats 3

# 6. Sanitizers (scratch copy of the committed tree)
cmake -S <scratch>/src -B <scratch>/build -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g"
cmake --build <scratch>/build --parallel && ctest --test-dir <scratch>/build --output-on-failure
python3 -I <scratch>/src/tests/harness/run_m0.py --binary <scratch>/build/sutekh --no-build
```

Evidence files (`planning/evidence/`):

- Harness: `m0-harness-dev.json`, `m0-harness-dev-summary.txt`, `m0-harness-release.json`, `m0-harness-release-summary.txt`, `m0-harness-self-test.log`
- Build and tests: `m0-dev-clean.log`, `m0-release-clean.log`, `m0-dev-build.log`, `m0-release-build.log`, `m0-ctest-dev.log`, `m0-ctest-release.log`
- Demo and probe: `m0-demo-dev.json`, `m0-demo-release.json`, `m0-probe-dev.json`, `m0-probe-release.json` (with `.log` files)
- Sanitizers: `m0-asan-ctest.log`, `m0-asan-harness-summary.txt`
- Live edit (M0-13 tree): `m0-latency-dev.json`, `m0-latency-dev-raw.json`, `m0-latency-dev-run.log`, `m0-latency-release.json`, `m0-latency-release-raw.json`, `m0-latency-release-run.log`, `m0-latency-dev-shared-serve-attempt.log`
- Native build (M0-13 tree): `m0-native-build.json`, `m0-native-build-run.log`
- Hashes: `m0-artifact-hashes.txt` (binary and script SHA-256, and the run-to-binary mapping)

---

## M1 platform status (M1-05 platform half, 2026-10-12)

Scope of this section: what Windows and other platforms actually did in M1-05 on this host. Every
row below is a recorded result, never an inference. Evidence: `planning/evidence/m1/`
(`platform-linux.json`, `platform-windows-cross.json`, `platform-matrix.json`), produced by
`python3 -I tests/platform/run_platform.py`. Handoff: `planning/handoffs/2026-10-12-m1-05a.md`.

### Distinction table

| Platform / lane | Status | What ran (this host: Ubuntu 24.04, x86-64, AVX2) | Not run / limits |
|---|---|---|---|
| Linux x86-64, native (SysV) | **run: passed** | `SUTEKH_NATIVE=ON` release configure; M0 unit tests `test_ir`, `test_world`, `test_vm`, `test_snapshot` (4/4 exit 0); native ABI probe 587 checks, 0 failures, scalar and AVX2 backends | Native differential suite `tests/native/*` is not rerun by this lane; default `ctest` 9/9 passes (unchanged) |
| Windows x64 via wine (mingw-w64 cross build) | **run under wine: passed** (wine 9.0, not Windows) | Cross-built M0 core and 4 unit tests, all exit 0 under wine; `su_native` with the Win64 prologue and `VirtualAlloc`/`VirtualProtect` publication, probe 597 checks, 0 failures (Win64 ABI, rcx argument, callee-saved rbx/rbp/rsi/rdi/r12-r15 and xmm6-15 clobber checks, injected publication failures); `sutekh --version` exit 0; `sutekh serve` returns `SU_E_UNSUPPORTED_METHOD`, exit 1 | wine is a compatibility layer: these results are not Windows-native evidence. Full graph does not build for Windows (see below) |
| Windows x64 via wine, full CMake graph | **failed** (expected: POSIX-only sources) | `cmake --build -- -k 0`: 22 objects fail (list below) | `su_agent`, `sutekh serve`/`worker`, process-isolated workers, POSIX test harnesses are not portable yet |
| Windows native (MSVC or clang-cl on a real Windows host) | **not run** | Nothing. No Windows host in this environment. `tools/env-windows.ps1` and `planning/toolchain-windows.json` (their `native_engine: not_run`) are unchanged | Any statement about native Windows execution needs a run on a Windows host; wine and mingw results must not be cited as that |
| macOS | **not supported** | Nothing. No Mach-O code-publication path; `su_native.c` POSIX branch is mmap/mprotect only | Not built, not tested |
| ARM64 | **not supported** (no backend) | Nothing. AsmJit ARM support is compiled out (`ASMJIT_NO_AARCH64`); `su_native_caps.c` reports scalar only off GCC/Clang x86-64 | No ARM64 native backend exists |

### Inventory of platform-conditional code

Windows branches (`#if defined(_WIN32)` / `_WIN64`, or host-ABI selection):

- `src/platform/su_platform.c`: QueryPerformanceCounter and GetCurrentProcessId on Windows. Compiled and run under wine (via `sutekh --version`).
- `src/platform/sutekh_main.c`: Windows `serve`/`worker` return the structured `SU_E_UNSUPPORTED_METHOD` JSON. Run under wine.
- `backends/native/su_native.c`: `VirtualAlloc`/`VirtualProtect`/`VirtualFree`/`FlushInstructionCache` publication and `SU_NATIVE_HOST_ABI_WIN64`. Run under wine (artifact host_abi = 2).
- `backends/native/su_native_asmjit.cpp` and `backends/native/avx2/su_native_avx2.cpp`: argument register rcx on Win64, rdi on SysV. Both run under wine. The AVX2 kernel uses only ymm0-3, which are volatile on Win64, and pushes rbp, rbx, r12-r14.
- `backends/native/avx2/su_native_caps.c`: CPUID/XGETBV only for GCC and Clang on x86-64. Elsewhere it reports scalar only (MSVC gets no AVX2 detection and is not built).
- `tests/native/scalar/test_native_scalar.c` and `tests/native/avx2/test_native_avx2.c`: the SysV clobber trampoline is guarded `!_WIN32`. Win64 clobber and publication-fault coverage now lives in `tests/platform/native_abi_probe.c` (run under wine), not in these files.

POSIX-only code (no Windows branch; fails to compile for Windows, not edited in M1-05):

- `src/agent/su_proc.c` (fork, sys/wait, rlimit), `su_proto.c` (poll), `su_path.c` (realpath), `su_spool.c` (mkdtemp), `su_agent.c` (sigaction, SIGPIPE), and the supervisor and worker spawn path.
- `src/runtime/worker.c` (sigaction, kill, pause, SIGPIPE): `su_worker_main` and the worker role. Used by `su_agent`, `recovery_worker`, `native_test_worker`.
- Tests: `test_control`, `worker_stub`, `test_reload`, `test_recovery`, `test_snapshot_cp`, `test_scheduler` (sysconf), `test_native_scalar`, `test_native_avx2` (sys/mman, fork).
- The `strace` probes (`tests/e2e/no_build_probe.py`, `native_spawn_probe.py`) need strace and ptrace, which exist only on Linux.

Objects that fail for Windows in the full-graph inventory (22): `su_agent` (5 sources), `su_agent_test` (5), `worker.c` in `su_agent`, `su_agent_test`, `recovery_worker` and `native_test_worker`, and `test_control`, `worker_stub`, `test_reload`, `test_recovery`, `test_snapshot_cp`, `test_scheduler`, `test_native_scalar`, `test_native_avx2`.

Compiles and runs for Windows without change: `su_ir` (SHA-256, JSON, LIR), `su_vm`, `su_world`, `su_snapshot` (`snapshot.c`, `checkpoint.c`), `su_jobs` (pthread through winpthreads), the four M0 unit tests, and the native backend and its probe.

### Win64 unwind information (status, not claimed as complete)

- The compiled C code in the Win64 binaries has `.pdata`/`.xdata` entries (from the compiler).
- The inline-assembly trampoline in the probe has no `.seh_` directives, so it has no unwind entry.
- Generated kernels (AsmJit) are JIT code with no `RtlAddFunctionTable` registration, so they have no unwind information. Win64 uses that table to unwind through frames, so an exception raised inside a kernel cannot be unwound in process. The consequence is that such a fault terminates the process, which is only acceptable because C07 requires worker isolation. The kernel makes no calls, so normal returns do not depend on unwind data.
- Not run: any Windows-native exception test. Under wine, the kernel's normal return and the publication-failure path were exercised; a faulting kernel was not.

### What is still missing for Windows

1. A Windows worker spawn, supervision and checkpoint-recovery path (C07, M1-02 contract). Nothing equivalent exists.
2. A port of the agent and worker sources listed above (or a Windows-specific branch) so `sutekh` can link as in CMake.
3. Unwind registration for generated kernels, if in-process exception handling is ever required.
4. A Windows-native build and test run on a real host (MSVC or clang-cl with `tools/env-windows.ps1`).
5. A portable replacement for the native scalar and AVX2 test files (their POSIX parts).

---

## M1 evidence refresh (M1 evidence worker, 2026-10-12)

Scope: the four audit blockers addressed by this pass (`planning/audit/M1-audit.md`, blockers 1, 2 and 6, plus the stress item). M1 is NOT closed: the audit's other blockers (M1-04 scheduler not integrated, C07 containment defects, stale-artifact mutants surviving) are unchanged. Every figure below is copied from the cited file. All timings are on one shared 4-vCPU VM (Intel Xeon @ 2.10 GHz, AVX2 and AVX-512F flags present, no frequency pinning). They are observations, not qualification (ENGINE_CONSTITUTION C10). `git.head_at_start` in each JSON names the measured tree.

### Native build latency (audit blocker 1) — `planning/evidence/m1/native-build-latency.json`

Cold = fresh build directory with the pinned AsmJit sources compiled. Warm = same directory with AsmJit already built (no-op, or mtime-only touch of one file). n = 7 per case, nearest-rank p95.

| Case | Cold configure (median s) | Cold clean full build, median (p95) s | Compile / link+archive / other steps |
|---|---|---|---|
| gcc 13.3 Release | 0.52 | 12.73 (16.57) | 104 / 30 / 1 |
| gcc Debug | 0.50 | 9.87 (10.45) | 104 / 30 / 1 |
| clang Release | 0.86 | 11.08 (14.69) | 104 / 30 / 1 |
| clang Debug | 0.89 | 7.80 (11.16) | 104 / 30 / 1 |

Warm (gcc Release medians; the other cases are in the JSON): no-op 0.035 s, 0 compile, 0 link, 1 other (CMake glob check). Incremental after an mtime-only touch: `backends/native/su_native_asmjit.cpp` 2.13 s (1 compile, 16 link/archive); `backends/native/su_native.c` 0.60 s (1, 16); `src/vm/su_vm.c` 0.58 s (1, 18); `include/sutekh/native/abi.h` 3.13 s (18, 18); `include/sutekh/abi.h` 4.63 s (37, 28).

Caveats: a first run classified the CMake glob step as a link step; that run was discarded and the measurement repeated after the classifier fix (commit 2fa3a11). Build directories were outside the repository and deleted after each sample. The live gameplay edit path runs none of these builds (ENGINE_CONSTITUTION C01): the only `exec` in `src/` is `src/agent/su_proc.c:237`, the supervisor re-executing its own worker binary; no cmake, ninja, gcc or clang appears in `src/`. No Windows build; no ccache.

### Isolated native throughput (audit blocker 2) — `planning/evidence/m1/native-throughput.json`

Driver: `bench/native/su_native_throughput.c`, built without CMake (compile and link lines recorded in the JSON; `CMakeLists.txt` untouched). Every configuration was verified byte-for-byte against the reference VM before timing. 21 samples per series after 3 warm-up invocations; the 1,024-row series use K back-to-back invocations per sample. Formulas: ns/row = median ns per invocation / rows; GB/s-equivalent = rows x bytes_per_row / median ns (nominal column bytes, 12 per row for both programs, not measured bandwidth). Pinned frequency: unknown.

Median ns/row, 1,000,000 rows, aligned columns:

| Program | VM wrapper | Scalar wrapper | Scalar kernel | AVX2 wrapper | AVX2 kernel |
|---|---|---|---|---|---|
| movement (2 loads, add, store) | 7.31 | 1.17 | 1.20 | 0.548 | 0.553 |
| arith20 (20 instructions) | 53.78 | 6.35 | 6.34 | 3.15 | 2.68 |

Unaligned (1 element) columns differ from aligned by up to about 25 percent in either direction (see the JSON; some scalar cases are faster unaligned, which is noise on this VM). The 1,024-row series fit in L1/L2 (about 12 KB per program's columns), so their GB/s figures are cache-resident and not comparable with the 1,000,000-row figures. The VM has no separate kernel, so no VM kernel figure exists. The brief's movement shape "3 loads/adds/store" was read as the shipped `examples/movement.lir.json` shape (2 loads, 1 add, 1 store); the JSON says so.

### 1,000,000-row stress, current default release binary (audit item) — `planning/evidence/m1/stress-1m-current.json`

Result: passed, on `build/sutekh` built by `tools/dev.py build --profile release` (SUTEKH_NATIVE off). Recorded values: world.create 797 ms; step median 192.5 ms, p95 200 ms (nearest-rank, n = 20, each verified; the median is recomputed from the 20 raw samples in `planning/evidence/m1/stress-1m-current.json`; the stored value was 193.2 ms from an upper-middle index in `bench/native/stress_1m.py`, now fixed); snapshot 203.7 ms host, 28,092,684 bytes; restore 895.7 ms host; worker VmHWM 178.5 MB after create and steps, 262.1 MB at end; RLIMIT_AS 4 GiB and 512 MiB spool are source defaults (`src/platform/sutekh_main.c`). The engine returned `engine_capture_ns: null`, which is recorded as null. This is a diagnostic, not a game-scale claim.

### Multi-agent experiment hygiene (audit blocker 6) — `planning/evidence/m1/agent_workflow.json`

- `agent_outcomes.json`: `a0b496d125408ea84` corrected from `in_progress` to `accepted_as_is`.
- Implementation rows: 33; accepted as is 23; accepted with integrator fix 7; partial 3; no work 0. First-pass acceptance 0.697 (23/33); rework 0.303. Agent wall time (git-derived proxy): median 611 s, p95 1,688 s, n = 39 (nearest-rank; the earlier 1,582 s came from a rounded index in `bench/agent_workflow/collect.py`, now fixed; the p95 is recomputed from the stored per-agent rows). Review/audit rows: 6, all of which found issues. (Regenerated by bench/agent_workflow/collect.py.)
- `planning/evidence/m1/agent_usage.json` exists with a schema note and empty `agents`. Tokens, tool calls and harness duration are therefore null in every row. Integration latency remains null.
- Four worktrees have no outcome class yet and are reported as `unclassified` (the collector exits 1 for them): `abb2fd2a4c686b3e8`, `a2600552f6d79bae8`, `ade4ddc3da6586da0`, `a564d896d74db497b`. They do not affect the implementation rates.

### Not done (honest list)

- M1 closure is not claimed until the independent re-audit. Status of the first M1 audit's blockers (planning/audit/M1-audit.md): (3) chunked scheduler: wired as an OPT-IN (`SUTEKH_TICK_WORKERS`, default serial) in commit a42cce7, VM systems only, native kernels run as one call; (4) C07 containment: workers now run under RLIMIT_AS 4 GiB and a scrubbed environment (commit 4f31ac8), asserted by harness TX6; (5) stale-artifact mutants H1, L1, L2: now killed by `test_specialize_admission` (the audit mislabelled L1/L2 as install-time; they are admission-time checks) with `tests/native/qualification/run_mutants.sh`; (1), (2) build latency and isolated throughput: stored in planning/evidence/m1/; (6) workflow experiment: tokens and integration latency remain null.
- Known gaps in the new opt-in features: native kernels run chunked under tick workers through `su_nv_run_system_chunked` (M1 closure C; opt-in, `SUTEKH_TICK_WORKERS` defaults to 1); installs are not re-checked against the schedule; candidate evaluation stays serial; `world.create` now refuses unordered conflicting systems (every system is treated as declared parallel), a behaviour change from M0 where list order was accepted; the "N>1 row order" mutant is equivalent and a discriminating mutant is still missing.
- No Windows-native build or run; Win64 results remain under wine only.
- No pinned-frequency, PMU, or repeated-run throughput; no AVX-512 backend or measurement.
- Build latency is one host and one tree; no ccache/sccache comparison; no clang sanitizer runs.
- Stress is one run on one VM; no reboot or repeat; not a game-scale claim.
- No token or tool-call values for any agent; no integration-latency values.

## M1 residual evidence (post-containment, 2026-10-12)

Measured on this tree (HEAD `58ee5c1` plus the uncommitted-then-committed mutant and test edits of this pass; see the
handoff `planning/handoffs/2026-10-12-m1-residual.md`). Shared 4-vCPU VM, observation only (C10). Old evidence files were not
overwritten; the new files are `*-post-containment.json`.

- Native build latency, n = 7 per case, cold clean full build median (p95), `planning/evidence/m1/native-build-latency-post-containment.json`:
  gcc Release 14.68 s (15.49 s); gcc Debug 11.88 s (15.84 s); clang Release 13.74 s (14.77 s); clang Debug 7.07 s (8.71 s).
  Previous file (n = 7): gcc Release 12.73 s (16.57 s), gcc Debug 9.87 s (10.45 s), clang Release 11.08 s (14.69 s), clang Debug 7.80 s (11.16 s).
- Isolated native throughput, median ns/row, 1,000,000 rows, aligned, `planning/evidence/m1/native-throughput-post-containment.json`:
  movement: VM wrapper 8.83, scalar wrapper 1.38, scalar kernel 1.42, AVX2 wrapper 0.565, AVX2 kernel 0.567;
  arith20: VM wrapper 50.80, scalar wrapper 6.64, scalar kernel 6.33, AVX2 wrapper 3.41, AVX2 kernel 3.05.
  Previous (`native-throughput.json`): movement 7.31 / 1.17 / 1.20 / 0.548 / 0.553; arith20 53.78 / 6.35 / 6.34 / 3.15 / 2.68.
  The driver runs in-process and does not use the worker containment, so the spread is VM noise, not containment cost.
- 1,000,000-row stress, current default binary (`build/sutekh` equivalent, SUTEKH_NATIVE off), `planning/evidence/m1/stress-1m-post-containment.json`,
  one run, status passed: world.create 906.6 ms (previous 797.2 ms); step median 189.6 ms, p95 263.2 ms, n = 20
  (previous 192.5 ms and 200.2 ms); snapshot 215.2 ms host (previous 203.7 ms), 28,092,684 bytes (unchanged);
  restore 1,042.7 ms host (previous 895.7 ms). Containment (seccomp, self-sandbox, Landlock, rlimits) is applied at worker
  start and in the disposable child, so restore and step timings can move with the shared VM. This single run cannot
  separate containment from noise; the restore and p95 rises are recorded, not explained.
- Full harness, default Release, `run_m0.py --binary <rel>/sutekh`: passed 20, failed 0, unavailable 2 (TX3, TX4 native-only).
  Self-test on the native gcc binary: 220 checks, 0 failures.

Mutation table (repo scripts, scratch copies, native gcc or default Release as noted; each mutant built with -Werror):

| Mutant | Script | Result | Caught by |
|---|---|---|---|
| M-ARCH | recovery | killed | ctest `containment` |
| M-X32 | recovery | killed | ctest `containment` |
| M-SOCKETPAIR | recovery | killed | ctest `containment` |
| M-URING | recovery | killed | ctest `containment` |
| M-EXECVE | recovery | killed | ctest `containment` |
| M-CLONE | recovery | killed | ctest `containment` |
| M-DIGEST-SHA | recovery | killed | ctest `hardening` |
| M-DIGEST-REV | recovery | killed | ctest `hardening` |
| M-DIGEST-TICK-RERUN (closure C M06) | recovery | killed | ctest `hardening` |
| M-SPOOL-MODE | recovery | killed | ctest `hardening` |
| M-INSTALL-REV | recovery | survived on first run; killed after the new entry check in `tests/reload/test_reload.c` | ctest `reload` |
| M-QUAR-FIND | native | killed (first run INVALID: replacement text unescaped `&&`; fixed) | ctest `safe_retire`, `native_fault`, `specialize_*` (2 of 5 fail) |
| M07 (closure C) | native | killed | ctest `safe_retire`, `specialize_inflight` (1 of 2 fails) |
| M10 (closure C) | native | killed | ctest `safe_retire` |
| M12 | native | killed | harness TX6 |

Anchor for M-INSTALL-REV: the anchor occurs twice in `src/runtime/reload.c` (lines 134 and 191, both inside
`su_reload_install`). The mutant targets line 134, the step-2 entry check; the step-8 admission recheck is left in place.
Not run in this pass: M06 closure-C re-run beyond M-DIGEST-TICK-RERUN (the mutant above is that re-run), TSan, clang sanitizers.

## C07 containment coverage (M1 closure D)

Precise statement, detail in `docs/AGENT_RPC.md` section 14.

- **Contained, by test:** socket creation and use (`socket`, `socketpair`, and io_uring including IORING_OP_SOCKET:
  `io_uring_setup` returns `EPERM`, `containment`); the i386 and x32 entries of those calls (the filter kills the
  process, `containment`); process creation and program execution by a worker (`execve`, `fork`, non-thread `clone`;
  the worker self-sandbox, `containment`, TX6 `Seccomp_filters: 2`); ptrace and the other listed calls (the filter
  list, section 14); file creation by a disposable child (Landlock: project root and `/tmp` refused with `EACCES`,
  `containment`; the containment object reports `fs_write_denied`); file size, open files, address space and core
  files (all children); CPU (disposable children).
- **Not contained:** filesystem reads; use of descriptors inherited across exec (the filters deny creation, not use);
  CPU of the active worker (no bound, by design); writes by the active worker (write access kept); side channels;
  executable mappings (mmap PROT_EXEC is allowed); kernel bugs reachable through allowed calls; non-x86-64 Linux
  (no seccomp filters, no self-sandbox); Windows (not implemented).
- **Scope statement:** the filters reduce what a worker can reach. They are not a hostile-code sandbox.

Test-only seams (`internal.test.*` probes, `SU_TEST_*` variables) exist only in `SU_TEST_FAULT_HOOKS` builds; the
release binary is checked for them (`check_release_seams.py`, a `release_seams` test in native builds).


## Numeric profile fixed_q32_v1, IR 0.3, space3d (18 October 2026)

Owner-authorized downstream work outside the milestone order (`planning/amendments/2026-10-18-numeric-3d.md`; M3 gates unchanged). Implemented and tested: numeric profile `fixed_q32_v1` (`docs/NUMERIC_FIXED_Q32.md`), IR 0.3 in the verifier and reference VM (`LIVE_IR_SPEC.md`; native backend refuses 0.3), the `space3d.query` host operator (`docs/SPACE3D.md`), the synthetic `6dof_racer_microcosm` scene and typed host-API helpers. Windows clang ctest 73/73; Linux GCC CI 72/72; MSVC scratch build bit-identical on every numeric, IR and space3d suite and the racer digests. Timing observations: `planning/evidence/numeric-3d/`. Handoff with unrun items: `planning/handoffs/2026-10-18-numeric-3d.md`.

## Host events, rollback state operations, native input, first presentation slice (18 October 2026)

Owner-authorized downstream work outside the milestone order (extension of `planning/amendments/2026-10-18-downstream-host-work.md`; M3 gates unchanged; M5 is not closed). Implemented and tested: host event reading by cursor and by tick, allocation-free in-place snapshot restore (differentially tested against the reference restore on 3,000 corrupted snapshots), a separately named non-canonical XXH64 digest (`include/sutekh/host.h`); the input layer (`include/sutekh/input.h`: device state, Raw Input and XInput on Windows, a data-driven Q16.16 mapping to per-tick records written through `su_host_input_push`); the first Vulkan slice (`include/sutekh/render.h`: run-time loaded Vulkan 1.2+, window swapchain and offscreen readback, instanced procedural meshes, debug lines, cameras, CPU/GPU telemetry) and an IR 0.3 demo with a no-renderer shadow world. Windows clang ctest 78 passed, 2 skipped (POSIX-only) of 80; Linux CI renders offscreen on Mesa lavapipe. The offscreen pixel test and the demo ran on a Quadro P600 over RDP, also under the Khronos validation layer (built from source; synchronization validation found and confirmed the fix of a readback hazard; 0 errors, 0 warnings after it); no Arc A770 run yet. Evidence: `planning/evidence/presentation-input/`. Handoff with unrun items: `planning/handoffs/2026-10-18-presentation-input.md`.

## IR 0.4: bit operations, row filter, cross-row reads, field budget (19 October 2026)

Downstream request work outside the milestone order (M3 gates unchanged). Implemented and tested: `SU_LIMIT_FIELDS` raised from 256 to 1,024 lanes (no persisted format stores it); IR 0.4 (`LIVE_IR_SPEC.md` "IR 0.4", artifact format 4) with u32 `and/or/xor/not/shl/shr/div/rem`, i64 `and/or/xor/not`, a capsule-level `row_filter` (masked u32 equality; filtered rows keep their values and run nothing), `gather_u32/_i32/_i64/_q32` (a field of another row by slot, entry state) and `find_eq_u32` (lowest slot whose field equals a key: first claimant by slot order), with a whole-table view for chunked runs (`su_vm_context_set_table_view`). Both VM tiers implement every operation; the native scalar lowering covers the bit operations and the row filter and refuses cross-row reads (`ir_0_4_gather_not_lowered`). Tests: `ir04`, `host04`, `ir04_diff`, `ir04_native_diff`. Not done: a discard binding for host-operator output lanes, native cross-row reads, reductions other than `find_eq_u32`.

## Registered host operators (26 October 2026)

Downstream request work outside the milestone order (issue 137; M3 gates unchanged). Implemented and tested:
`su_host_register_operator` (`include/sutekh/host.h`, "registered host operators"): a game defines a host operator in
its own code and registers it before creating a world; the record, schedule union table, effect analysis, capsule
order, snapshots, digests, in-place restore, replays and world identity treat it as a built-in operator (the
identity also covers its flags and scratch size). At run time: declared lanes only, read lanes re-hashed after each
call (`SU_E_UNDECLARED_WRITE`), write lanes pre-filled (zero or the input value) so every row is defined, scratch
zeroed before every call, nothing allocated in the tick. In-process only (not in `sutekh serve` or the
worker). Evidence: `tests/host/test_host_registered.c` (static and DLL; cross-process identity checks), handoff
`planning/handoffs/2026-10-26-registered-operators.md`.

## Operator state in snapshots, digests and replays (26 October 2026)

Downstream request work outside the milestone order (issue 139; M3 gates unchanged). Implemented and tested: a
registered host operator may declare bounded variable-size state (`SuHostOperatorDesc` version 2:
`state_max_entries`, `state_max_bytes`, `state_flags`; `include/sutekh/host.h`, "operator state"). The engine owns the
bytes: a map from (entity, sub) keys to values, kept sorted, changed during `run` through `su_host_state_put/alloc/
modify/erase/erase_entity` and read through a view between ticks. It is in the canonical snapshot (version 2 with a
`SUOPSTAT` section, only for worlds with state), so in both digests, both restores and replay keyframes; changes
commit with the tick or are dropped with it; every buffer is allocated at world creation; caps are structured
refusals that depend only on the logical state; `SU_HOST_STATE_DROP_DEAD` erases dead entities' entries at publish.
Worlds without operator state keep byte-identical snapshots, digests and replays. In-process only, like registered
operators. Evidence: `tests/host/test_host_opstate.c` (static and DLL; cross-process identity checks), handoff
`planning/handoffs/2026-10-26-operator-state.md`.

World-level operator-state budget (issue 153): `SuHostWorldDesc.state_budget_bytes` (appended; 0 and the older
descriptor sizes mean `SU_HOST_STATE_BUDGET_DEFAULT`, 1 GiB). World creation sums the exact reservation of every
stateful operator (4 x `state_max_bytes` + 52 x `state_max_entries` + a header) before allocating any state and
refuses the first one that crosses the budget with `SU_E_RESOURCE_LIMIT`. Not part of the identity. Evidence:
`tests/host/test_host_opstate.c` section 11, handoff `planning/handoffs/2026-10-26-state-budget.md`.

## Schedule size and declared world schemas (26 October 2026)

Downstream request work outside the milestone order (issue 144; M3 gates unchanged). Implemented and tested: an
in-process host world (`include/sutekh/host.h`) schedules up to 64 systems (`SU_HOST_MAX_CAPSULES`, IR capsules and
host operators; the 65th is `SU_E_RESOURCE_LIMIT`), with bounded creation costs (the effect analysis compares ID-sorted
field tables per pair) and the existing admission (entity tick budget over every system, the 32-kernel native
registry). A world may declare its schema (`SuHostWorldDesc.schema`, "declared world schemas": fields, IDs, value
types, the i64/q32 two-lane rule, names) independently of its capsules; capsules and operator records bind a subset
under the union-table checks, and fields no system binds are ordinary host-written and host-read columns. The
declared schema is part of the world identity. Worlds without a declared schema are unchanged (pinned identities and
digests from before the change, byte-identical replays and snapshots). The control plane (`sutekh serve`, worker,
agent) keeps 8 systems per world. Evidence: `tests/host/test_host_schedule.c`, `tests/host/test_host_compat.c`, handoff
`planning/handoffs/2026-10-26-schedule-schema.md`.

## space3d compound movers (20 October 2026)

Downstream request work outside the milestone order (M3 gates unchanged). Implemented and tested: a fourth space3d
operator, `space3d.query_compound` (record `profiles/space3d/space3d_query_compound.capsule.json`): movers made of up
to 8 capsules/spheres fixed in their local frame (compound table in the operator data, blob format `S3MESH02`),
swept against every row kind including static meshes and other compound movers, with the sub-shapes of each contact
reported (`c0_sub`, `c1_sub`); a conservative turn bound from the start-of-tick orientation (`rot0` lanes: every
core point padded by 2 d sin(a / 2)), so a craft turning within the tick cannot pass through anything; with no
compound and no `rot0` the operator equals `space3d.query_shape` bit for bit. `s3mesh_sweep_lim` (a sweep limited
to toi <= tmax). Evidence: `docs/SPACE3D.md` "Compound movers" (space3d_mesh, space3d_ops incl. a 4,000-case
no-tunnelling oracle at 1,820 m/s, host_ops check 10).

## space3d static meshes and operator data (20 October 2026)

Downstream request work outside the milestone order (M3 gates unchanged). Implemented and tested: triangle sweeps and
distances in the space3d narrow phase (`s3_sweep_sphere_tri`, `s3_sweep_capsule_tri`, `s3_dist_seg_tri`); immutable
mesh sets (`profiles/space3d/native/space3d_mesh.[ch]`, blob format `S3MESH01`: shared geometries, scaled and rotated
parts, meshes, integer BVHs with traversal-independent results); row kind 8 (static mesh, index in `gate_index`) in
every space3d operator, active only with a set and otherwise bit-identical to the operator without meshes; host API
`su_host_set_operator_data` (operator data before the first step, folded into the world identity). Evidence:
`docs/SPACE3D.md` "Static meshes" (space3d_geom, space3d_mesh, space3d_ops, host_ops check 9).

## Simulation runtime: rollback budget, planned VM tier, native IR 0.3, space3d binding (18 October 2026)

Owner-authorized downstream work outside the milestone order (`planning/amendments/2026-10-18-sim-runtime.md`; M3 gates unchanged). Implemented and tested: a rollback resimulation benchmark (`racer_microcosm --rollback`, `run_bench.py --rollback-evidence`); the planned VM tier (default; bit-identical to the reference interpreter, selectable with `SUTEKH_VM_TIER=reference`) with exact fast numerics (`src/ir/su_num_fast.h`); per-call tick state reuse in the entity runtime; cheaper space3d set-up; the native tier building and passing its tests on Windows x64 (Win64 ABI checked for scalar and AVX2 kernels; the four control-plane qualification drivers ported); a native IR 0.3 scalar lowering differential-tested against the VM; `world.create` of IR 0.3 modules fixed and covered through `sutekh serve` (`space3d_serve`); schema-relative binding of host operators (`su_host_load_bound`, `docs/SPACE3D.md`). Windows clang ctest 84/84 (default) and 97/97 (`SUTEKH_NATIVE=ON`). Observations: `planning/evidence/sim-runtime/` (K = 12 rollback at N = 256: reference 124 %, planned 44 %, native 29 % of the 8.33 ms budget at p50). Handoff with unrun items: `planning/handoffs/2026-10-18-sim-runtime.md`.

## Assets, PBR-lite rendering, large scenes, effects, HUD, audio (18 October 2026)

Owner-authorized downstream work (second extension of `planning/amendments/2026-10-18-downstream-host-work.md`; M3 gates unchanged; M5 is not closed). Implemented and tested: `include/sutekh/asset.h` (cooked SUAPAK v1 packs: content SHA-256 address, stable name-derived ids, per-payload hashes, full validation on open, writer, background loader; a glTF 2.0/GLB importer and cook step on cgltf 1.15 and stb_image 2.30 fetched and pinned by `tools/bootstrap_asset_deps.py`, with KTX2 RGBA8/BCn); `include/sutekh/render.h` API version 2 (generational GPU resources with budgeted asynchronous upload and deferred retirement, `su_render_frame_ex` with Q32.32 camera-relative positions, reversed-Z infinite projection, per-object and per-node culling, distance LOD, instanced batching, metallic-roughness PBR-lite with sun, sky IBL-lite, normal maps and emissive, HDR with ACES tonemapping, additive effects, HUD text with a project-authored font, persistent pipeline cache); `include/sutekh/audio.h` (lock-free voice mixer with panning, attenuation and Doppler, WAV/Ogg and PS-ADPCM (raw frames and VAGp), WASAPI, an event cursor that does not replay re-emitted events). Windows clang ctest 92 passed, 2 skipped (POSIX-only) of 94; Linux CI renders the asset test on Mesa lavapipe. On the Quadro P600 over RDP: pixel-exact asset rendering checks, 0 validation errors or warnings (also with synchronization validation) over the tests, the demo offscreen with rollback drills, window resize/minimize with a model upload in flight and MAILBOX; 1920x1080 offscreen GPU median 1.4 to 2.4 ms for 3,000 to 20,000 instanced asteroids (observations). Evidence: `planning/evidence/presentation-assets/`. Handoff with unrun items: `planning/handoffs/2026-10-18-presentation-assets.md`.

## Linux windowed presentation and native input (26 October 2026)

Issue 138. Implemented and tested: `include/sutekh/window.h` (`su_window`: SDL 3 opened at run time, Wayland or X11
window, native handles for the renderer, keyboard/mouse/gamepad input), `include/sutekh/render.h` API 12
(`window_system`: Wayland and Xlib surfaces created by the renderer; `su_render_resize(0, 0)` pauses presentation;
API 11 descriptors still accepted), `include/sutekh/input.h` API 2 (portable device events: HID usages to scan code
set 1, XInput ranges), and `space_demo` building and running windowed on Linux. Linux GCC 16.2 Debug and Release
`-Werror`: 145 tests, 0 failures, 1 skip (`audio_device`), window and swapchain tests on the session's Wayland
display, under XWayland, and under Xvfb (llvmpipe). Decision record: `docs/BACKENDS_AND_DEPENDENCIES.md` section 8.
Evidence: `planning/evidence/linux-window/`. Handoff with unrun items: `planning/handoffs/2026-10-26-linux-window.md`.
