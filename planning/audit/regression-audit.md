# Regression audit: M0 baseline vs HEAD

Date: 2026-10-12. Auditor: read-only regression pass (no product code changed). Scope: whether M1 (native lane, containment, scheduler), M2 (entities, migrations) and M3-03a/04a (host entries, schema edit) worsened M0-era behaviour, ABI or performance.

## 0. Baseline and HEAD

| Item | Baseline | HEAD |
|---|---|---|
| Commit | `9af40cc` (= `accepted_source_revision` in `planning/status.json`; source tree identical to M0 source `d352b54`, the rewritten `c0e8335`, except `tools/validate_pack.py`) | `b839066` (worktree HEAD at audit start) |
| Release binary `build/sutekh` sha256 | `d5714cb06c351ec16a2709ff2bd9c12eaf7b3bb8cbafa7148980ca7fd7fa8acf` (matches `accepted_artifact_hash` and `planning/evidence/m0-artifact-hashes.txt`) | `95d6593be353449efbb141a566e1a07b5edaf03c956e6b9bdb944b2ff0412d02` |
| Binary size | 177,168 bytes | 452,088 bytes (+155%) |
| Harness script sha256 (`tests/harness/run_m0.py`) | `a03512a05e20...` (M0 harness) | `2fc4b8156c6d...` (changed: T02 24→36 checks, T03 12→18, T10 15→16, extra TX1–TX11) |
| Build | `python3 tools/dev.py build --profile release` (gcc 13.3, cmake 3.28.3, ninja), `SUTEKH_NATIVE` off (default) | same command, same toolchain |

Scratch: `git worktree add --detach` at `scratchpad/base` (9af40cc) and `scratchpad/head` (HEAD). Both removed at the end of the audit (section 9).

## 1. M0 acceptance harness T00–T15 (and extra TX checks)

Commands (run from each scratch tree, absolute paths):
`python3 -I <tree>/tests/harness/run_m0.py --binary <tree>/build/sutekh --no-build --report-dir <dir>`

T00 is `not_run` in every invocation because `--no-build` skips the clean bootstrap (by design). It was not run with a build, because a build would clean the scratch `build/` directory.

| Harness | Binary | T01–T15 | Totals (passed / failed / not_run / unavailable) | Notes |
|---|---|---|---|---|
| baseline (a03512a05e20) | baseline (d5714cb0) | 15/15 passed | 15 / 0 / 1 / 0 | `ra/bb.log` |
| HEAD (2fc4b8156c6d) | HEAD (95d6593b) | 15/15 passed | 23 / 0 / 1 / 3 | TX1–TX11: 8 passed; TX3, TX4, TX10 unavailable (`world.specialize` not advertised, non-native build) |
| baseline harness | HEAD binary | 15/15 passed | 15 / 0 / 1 / 0 | `ra/bh.log` |
| HEAD harness | baseline binary | 13/15 passed, T02 and T03 unavailable | 13 / 0 / 1 / 13 | Baseline binary lacks `world.close`, which the HEAD harness uses for T02/T03 and TX1–TX11 (expected, not a regression) |

ctest (release, both trees): baseline 7/7 passed (`ra/ctest-base-release.log`); HEAD 24/24 passed (`ra/ctest-head-release.log`).

Timing targets from the harness (provisional, shared VM; not qualification):

| Target | baseline harness / baseline bin | baseline harness / HEAD bin | HEAD harness / HEAD bin | HEAD harness / baseline bin |
|---|---|---|---|---|
| ir_prepare p95 (≤20 ms) | 10.9 | 11.1 | 14.0 | 12.5 |
| preview_response p95 (not_comparable) | 8.2 | 8.6 | 9.2 | 8.2 |
| install_critical_section p95 (≤1 ms) | 0.001 | 0.001 | 0.001 | 0.001 |
| warm_edit_to_first_effect p95 (≤100 ms) | 12.8 | 11.8 | 12.1 | 12.5 |
| native_incremental_check p95 (not_comparable) | 368 | 479 | 793 | 921 |

The native_incremental_check values swing by more than 2x across identical configurations (baseline harness on baseline binary 368 vs. 479 on HEAD binary; HEAD harness 793 on HEAD vs. 921 on baseline). This is shared-VM noise and it is not evidence either way.

## 2. Protocol transcript, digests, snapshots, status codes, capsule schema

### 2a. Identical request transcript (`scratchpad/transcript.py`, 22 requests + 11 raw protocol fixtures from `tests/fixtures/protocol/`)

Requests: `engine.info`; `world.create` (t02/world_8 + movement); `world.inspect`; `world.step` (3 ticks), stale step; `world.snapshot`; `world.restore` (good and missing id); `capsule.submit`; `candidate.status`; `candidate.evaluate`; `candidate.install` (stale rev); `events.read`; `tests.run`; `no.such.method`; path traversal; missing fixture; unknown world; out-of-range ticks; the protocol fixtures.

Normalised (worker_pid and pid values masked) diff of the two transcripts:

| Method | Result |
|---|---|
| engine.info | additive keys only: `worker_generation`, `recoveries`, `recovery`, `tick_workers` |
| world.create | identical (world ids, revisions, hashes) |
| world.inspect | `columns` unchanged; additive keys `entities`, `layout_generation`, `migration_lineage`, `schema`, `needs_resubmission` |
| world.step (incl. stale SU_E_STALE_REVISION status 10) | identical |
| world.snapshot | format 0.1 → 0.2 (`bytes` 565 → 784, sha256 changed for identical state); `capture_ns` is **null in HEAD (3/3 repeats) and an integer in baseline** (see R4) |
| world.restore | identical except the snapshot id it refers to |
| capsule.submit, candidate.status, candidate.install | identical hashes (`source_hash`, `capsule_hash`, `semantic_hash`); additive `install.reason: null` |
| candidate.evaluate | additive `containment` object |
| events.read | baseline SU_E_UNSUPPORTED_METHOD (12); HEAD implements it and rejects my parameter names with SU_E_TYPE (2). Intended (M1/M2 method), not a regression |
| tests.run, no.such.method, path traversal, missing fixture, unknown world, out-of-range ticks, all 11 protocol fixtures | identical codes and messages |

Status codes: `include/sutekh/status.h` numeric values 0–20 unchanged (see section 3). All error `status` numbers in the transcript are identical.

### 2b. Canonical digests and snapshot loadability

- Candidate/semantic/capsule/source hashes: identical across binaries (verified field by field).
- World state digest: the canonical state encoding changed from `SUWSTATE` version 1 to version 2 (`src/world/su_world.h`, M2-00c1). Version 1 is still accepted by `su_world_restore_canonical`. The canonical bytes and any digest of the same state therefore differ between M0 and HEAD. The world.snapshot sha256 change in 2a is a consequence of this and of format 0.2.
- Snapshot 0.1 loadability in HEAD: `tests/fixtures/snapshot01/golden_0_1.json` (M0-era 0.1 golden) is parsed and restored by `test_format01_golden` in `tests/snapshot/test_snapshot.c`; the `snapshot` ctest passed in release and under ASan. HEAD writes 0.2 and accepts 0.1 and 0.3 (`SU_SNAPSHOT_FORMAT_01` in `src/world/snapshot.h`).
- Snapshot produced by the baseline binary and restored by HEAD: **not run**. The control protocol does not return snapshot documents (`world.snapshot` returns only a summary), so cross-binary loading needs a library driver (unrun).

### 2c. Capsule schema verdicts (`tests/schema/test_capsule_schema.py`, HEAD tree)

| Schema used as `--baseline-schema` | files compared | mismatches | Result |
|---|---|---|---|
| Pre-M3-03a schema (`git show 1784fd6^:schemas/capsule.schema.json`), the checker's intended baseline | 92 | 0 | status `passed`, 35 negative cases, exit 0 (`ra/schema-pre-m3.log`) |
| M0 schema (`base/schemas/capsule.schema.json`) | 92 | 27 | status `failed` (`ra/schema.log`) |

All 27 mismatches under the M0 schema are in `tests/fixtures/entities/*` and `tests/fixtures/ir02/*`. None of them exists in the baseline tree (checked: 0 of 27 present in `9af40cc`). The M0-era capsule files are verdict-stable under both schemas. The `failed` status with the M0 schema is an artifact of using the M0 schema on IR 0.2 fixtures, not a regression.

## 3. Public header and ABI diff (`include/sutekh/*.h`, `native/abi.h`)

`diff -rq base/include head/include`:

| File | Change | Compatibility |
|---|---|---|
| `include/sutekh/abi.h` | identical | — |
| `include/sutekh/status.h` | `SU_E_STALE_HANDLE` = 21 added; `SU_STATUS_COUNT` 21 → 22; name table extended | Additive. Values 0–20 unchanged. Code that sizes arrays by `SU_STATUS_COUNT` or indexes the name table sees one new entry (R7) |
| `include/sutekh/ir.h` | new opcodes `SU_OP_SLOT_U32` 10 … `SU_OP_EMIT_EVENT` 14 (IR 0.2 only), `SU_IR_VERSION_STRING_0_2`, `SU_ARTIFACT_FORMAT_VERSION_0_2` = 2 | Additive. Opcodes 0–9 unchanged. 0.1 modules still reject 10–14 (`su_insn_known` minor gate; mutant C confirms this is covered only by `test_vm`) |
| `include/sutekh/entities.h` | new | Additive (M2) |
| `include/sutekh/native/abi.h` | new | Additive (M1); native lane not part of M0 |

Removed or changed public prototypes or structs: none found in `include/` or `src/**/*.h` (`git diff 9af40cc HEAD -- include src backends | grep '^-' | grep '('` shows only static functions in `src/agent/*.c` and the `SU_STATUS_COUNT` macro). `SuWorldDesc` keeps its layout; only the `row_count` comment changed. `SuWorldSchemaExt`, `su_world_create_ex` and `su_world_create_ext` are new. `SuVmEmitter` and `SuExecContext` gained fields, but `SuExecContext` is opaque and internal.

Behaviour-visible ABI note: `su_world_restore_canonical` and the snapshot reader accept version 1 and 0.1, so M0 data remains readable (see 2b).

## 4. Performance (same machine, n = 5 per row, median [min, max])

Machine: 4 vCPU Intel Xeon 2.10 GHz shared VM, 15 GiB, kernel 6.18.44-fc-v114. Observation only (constitution C10). Distinguish: the "signal" column says whether the baseline and HEAD ranges overlap.

### 4a. Live reload benchmark (`bench/reload/bench_reload.py live --skip-stress`, default 60 epochs × 12 edits, 60 cold trials; 5 runs per tree)

Output JSON: `planning/evidence/m0-latency-release.json` in each scratch tree (copied per run).

| Metric (p95 of run) | baseline | HEAD | change | signal |
|---|---|---|---|---|
| warm total_edit_to_first_effect_ms | 10.19 [8.98, 10.72] | 14.14 [12.21, 15.81] | +39% | ranges do not overlap, but see R1 (harness disagrees) |
| warm candidate_evaluate_ms | 8.41 [7.66, 9.53] | 9.32 [8.61, 11.84] | +11% | overlapping |
| warm candidate_install_rpc_ms | 0.69 [0.56, 0.81] | 2.48 [2.33, 2.92] | +258% | non-overlapping (R1) |
| warm capsule_submit_ms | 0.39 [0.37, 0.54] | 0.47 [0.32, 0.57] | +21% | overlapping |
| warm world_step_first_effect_ms | 0.43 [0.34, 0.56] | 1.12 [0.94, 1.47] | +159% | non-overlapping (R1) |
| cold first_edit_total_ms | 9.54 [8.87, 10.95] | 13.27 [11.58, 15.88] | +39% | non-overlapping (R2) |
| cold world_create_ms | 3.68 [2.82, 3.88] | 6.62 [5.01, 10.75] | +80% | non-overlapping (R2) |
| cold process_first_request_ms | 1.12 [0.92, 1.47] | 1.37 [1.16, 1.76] | +22% | overlapping |

### 4b. Micro benchmark (`scratchpad/perf_micro.py`, 1 world of `t02/world_1024` + movement, 5 runs per tree)

| Metric | baseline median [min, max] | HEAD median [min, max] | change | signal |
|---|---|---|---|---|
| engine.info first reply (spawn to reply), ms | 1.77 [1.56, 1.81] | 1.82 [1.71, 2.05] | +3% | none |
| world.create (spawns worker), ms | 2.37 [2.03, 2.49] | 5.27 [3.80, 7.86] | +122% | non-overlapping (R2) |
| world.step 200 ticks, ticks/s | 66,467 [38,521, 116,027] | 54,275 [47,712, 68,126] | −18% | overlapping, not significant |
| world.step 5 ticks, p50 call latency, ms | 0.126 [0.109, 0.163] | 0.613 [0.474, 0.911] | +386% | non-overlapping (R1) |
| worker RSS, 1 world, kB | 2,224 [2,220, 2,228] | 2,564 [2,560, 2,564] | +15% | non-overlapping |
| worker RSS per additional 1,024-row world, kB | 108 | 153 | +42% | constant across runs |
| binary size, bytes | 177,168 | 452,088 | +155% | single build |

Not run: `bench/native/*` (requires a `SUTEKH_NATIVE=ON` build; the native lane is not in the M0 build), `bench/ir02_entities_bench.py` (M2 bench; no baseline counterpart), the 1,000,000-row stress passes of `bench_reload` (see section 8).

### 4c. Attribution (diff read; not profiled)

- R1 (per-tick and install cost). In `src/runtime/schedule.c`, `su_sched_run_ticks_opt` now calls `su_world_zero_dead` after every system and `su_world_publish` zeroes dead rows and swaps an entity table (`src/world/su_world.c` ~L535–625). `su_world_begin_tick` copies the entity table every tick. `su_vm_run_system` now checks `ctx->alive[r]` per row (`src/vm/su_vm.c` `su_exec_rows`). `h_candidate_install` (`src/runtime/worker.c`) runs `su_reload_install` → `su_reload_load` (file read, hash, schema check) → `su_sched_check_candidate`, which has additional M2 checks. These are the likely sources. Attribution is not confirmed; a `perf` profile is needed.
- R2 (creation). `h_world_create` now builds schema extensions and an entity table of `max(row_count, max_entities)` capacity (`su_world_create_ex`/`_ext`). The worker also starts a checkpoint store and sandbox state (`src/runtime/worker.c` ~L5417–5422). Not profiled.
- Per-world RSS grows by about 45 kB per 1,024-row world. The entity table and per-world structures are the likely source; not confirmed.

## 5. Robustness: sanitizers, mutation spot checks, diff read

### 5a. ASan + UBSan ctest on HEAD

Build: `cmake -S <head> -B <scratch>/head-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"`, then `cmake --build`. Logs: `ra/asan-*.log`.

| Run | Result |
|---|---|
| `ctest -j2` (default env) | 21/24 passed. Failed: `control` (LeakSanitizer fatal "does not work under ptrace"), `containment` (ASan shadow memory cannot be mapped under the 4 GiB RLIMIT_AS that the worker sets by default; `ReserveShadowMemoryRange failed`), `migration_runtime` (LeakSanitizer, 65,699 bytes in 3 allocations) |
| `SUTEKH_WORKER_AS_BYTES=0 ctest -j2` | 22/24 passed. `containment` passes. Still failed: `control` (LSan/ptrace) and `migration_runtime` (LSan leak) |
| `ASAN_OPTIONS=detect_leaks=0 ctest -R control` | passed |
| `ASAN_OPTIONS=detect_leaks=0 ctest -R migration_runtime` | passed |
| UBSan `runtime error` in any log | none |

The `migration_runtime` leak is in the test helper (`tests/migration-runtime/test_migrate_runtime.c` lines 196 and 205: `malloc` of the response text and `su_json_parse` of it, not released on the injected-failure path of `run_alloc_faults`). It is test hygiene in an M2 test, not an engine behaviour change (R8).

### 5b. Mutation spot checks (scratch HEAD release tree; each mutant reverted by copy-compare against the repository file)

| Mutant | Change | Caught by |
|---|---|---|
| A | `su_exec_rows` no longer skips dead rows (`src/vm/su_vm.c`) | `vm` FAILED (test_vm.c:1005, 1218, 1219, 1366). Caught |
| B | `su_world_publish` no longer zeroes dead rows (`src/world/su_world.c`) | `world_entities` FAILED. Caught |
| C | `su_insn_known` accepts IR 0.2 opcodes in 0.1 modules (`src/vm/su_vm.c`) | `vm` FAILED; `ir` passed. Caught by one test only; the IR verifier's own 0.1 gate was not mutated |

Command: `bash scratchpad/mut_test.sh <A|B|C>` (builds, then runs `vm ir entities world_entities entity_tick tick_workers scheduler world reload churn snapshot`).

### 5c. Diff read of M0-era files changed later

Files with changes after the baseline: `src/vm/su_vm.c` (+115/−: dead-row skip, row_seq, emitter, opcodes 10–14, `needs_emitter` check), `src/world/su_world.c` (+708/−: entity table, zero_dead, publish, canonical v2, schema extensions), `src/world/su_world.h` (canonical v2 layout, schema ext API), `src/runtime/schedule.c` (zero_dead in tick loop, entity path), `src/runtime/worker.c` (+4,291: world.step/install/create/snapshot all rewritten to go through tick opts, checkpoint, recovery), `src/runtime/checkpoint.c` (create_ex, capture_ns), `src/agent/su_agent.c`, `su_proc.c` (containment), `src/platform/sutekh_main.c` (+51: RLIMIT_AS 4 GiB default, spool 512 MiB default, tick worker env).

Behaviour changes not covered by an M0 test (findings):

- `sutekh_main.c`: the worker now runs under RLIMIT_AS = 4 GiB by default. The baseline left it unchanged (`worker_address_space_bytes` = 0). A workload that needs more than 4 GiB of address space is now refused in HEAD (R5). Intended by constitution C07, but a behaviour change.
- `checkpoint.c:133` `ns_known = t0_ok && t1_ok && t1 >= t0` and `world.snapshot` reply `capture_ns: null` in HEAD (R4). Not isolated.
- `su_vm.c` `su_exec_rows`: `row_seq` reset per row and the dead-row check are inert for M0 worlds (`alive` NULL, all rows alive), so VM results for M0 modules are unchanged. Confirmed by the byte-identical `candidate` hashes in 2a and by T02–T05 passing.

## 6. Regression list (behaviour, perf, ABI), severity and remedy

| ID | Area | Finding | Severity | Remedy |
|---|---|---|---|---|
| R1 | Perf | Per-tick and install latency up on the same machine: step-5 p50 0.13 → 0.61 ms (×4.9, non-overlapping ranges); candidate.install RPC 0.69 → 2.48 ms (×3.6); bench warm total_edit_to_first_effect p95 10.2 → 14.1 ms (+39%). The harness T13 `warm_edit_to_first_effect` shows no change (12.8/11.8 baseline vs 12.1/12.5 HEAD), so the evidence is mixed | Medium | Profile (`perf record`) `world.step` and `candidate.install`; make `zero_dead` and the entity-table copy incremental or skip them for M0-only worlds; keep the dead-row check out of the M0 path; add an n ≥ 5 perf gate that compares against this baseline |
| R2 | Perf | Cold `world.create` (spawns worker) 2.4 → 5.3 ms median (+122%); cold first-edit p95 9.5 → 13.3 ms; worker RSS 2.2 → 2.6 MB base and +45 kB per 1,024-row world | Medium | Profile the create path (schema extension, entity table capacity, checkpoint store/sandbox setup); keep per-world memory proportional to rows |
| R3 | Digest/format | Canonical state encoding moved from v1 to v2; world.snapshot writes format 0.2 (bytes 565 → 784, sha256 changed for identical state). Reads of 0.1 and v1 are kept. Intended by contract | Low (documented) | Record in the M0 compatibility notes that snapshot and state digests of identical states changed; keep the 0.1 golden test in ctest |
| R4 | Behaviour | `world.snapshot` reply `capture_ns` is `null` in HEAD (3/3 repeats) and an integer in baseline. Type change in an M0 reply field | Low | Find why `su_platform_monotonic_ns` fails or `t1 < t0` in the worker path (`src/runtime/checkpoint.c:127–133`); restore an integer or document null |
| R5 | Behaviour | Worker RLIMIT_AS 4 GiB and spool 512 MiB are now defaults (baseline: unlimited / no spool limit). Intended by C07 | Low | Document the change; `SUTEKH_WORKER_AS_BYTES=0` restores the baseline behaviour |
| R6 | Protocol | Additive keys in engine.info, world.inspect, candidate.evaluate, candidate.install (`reason`). `events.read` now implemented (was SU_E_UNSUPPORTED_METHOD) | Info | None needed; clients that reject unknown keys would break (none known) |
| R7 | ABI | `SU_STATUS_COUNT` 21 → 22 with new `SU_E_STALE_HANDLE`=21. Values 0–20 unchanged | Info | Keep the additive rule; note in the ABI section |
| R8 | Sanitizer | `migration_runtime` leaks in the test helper under the fault-injection path (test code, not engine). `control` fails under ASan only because LSan does not run under ptrace. `containment` needs `SUTEKH_WORKER_AS_BYTES=0` under ASan | Low (test hygiene) | Free the response text and the parsed document on every path in `test_migrate_runtime.c`; run ASan ctest with `SUTEKH_WORKER_AS_BYTES=0` and `ASAN_OPTIONS=detect_leaks=0` for `control` only if LSan cannot run |
| R9 | Harness | HEAD harness changed (T02/T03 checks, TX1–TX11). Cross-harness comparisons need the baseline harness | Info | Keep the baseline harness as the reference for M0 comparisons |
| R10 | Checker | `tests/schema/test_capsule_schema.py` with the M0 schema as baseline reports 27 mismatches, all on post-M0 fixtures. With its intended pre-M3-03a baseline it passes (92 files, 0 mismatches) | Info | Document which baseline the checker expects |

No behaviour regression was found in the M0 acceptance cases T01–T15, the M0 protocol error codes, or the candidate/semantic hashes.

## 7. Unrun checks

- T00 (clean bootstrap) for both trees: skipped by `--no-build` (not run in this audit).
- Cross-binary snapshot load: a snapshot document produced by the baseline binary restored by HEAD (no RPC returns the document; needs a library driver).
- TSan, clang builds, Windows/mingw builds.
- Native lane: `SUTEKH_NATIVE=ON` builds, `bench/native/*`, TX3/TX4/TX10 (`world.specialize`).
- Reload bench stress passes at 1,000,000 and 250,000 rows (`--stress-rows`), and `bench/ir02_entities_bench.py`.
- Profiling of R1/R2 (no `perf` run; attribution is by diff reading only).
- Mutation spot checks beyond A–C; no mutant on `src/runtime/worker.c`, `checkpoint.c`, `sutekh_main.c` (R4, R5 unverified by mutation).
- `planning/audit/M1` mutants INSTALL-REV, DIGEST-TICK-RERUN, QUAR-FIND (listed as unrun by `planning/status.json`).

## 8. Reproduction

- Baseline/HEAD worktrees: `git worktree add --detach <scratch>/base 9af40cc`; `git worktree add --detach <scratch>/head HEAD`.
- Builds: `python3 tools/dev.py build --profile release` in each scratch tree.
- Harness: `python3 -I tests/harness/run_m0.py --binary <tree>/build/sutekh --no-build --report-dir <dir>`.
- Transcript: `python3 -I <scratch>/transcript.py <tree> <tree>/build/sutekh <out>` (scripts are not committed).
- Schema: `python3 -I tests/schema/test_capsule_schema.py --baseline-schema <schema>`.
- Reload bench: `python3 -I bench/reload/bench_reload.py live --binary ... --build-dir ... --profile-label release --skip-stress` (5 runs per tree).
- Micro bench: `scratchpad/perf_micro.py` (5 runs per tree).
- ASan: section 5a.

## 9. Cleanup

Scratch worktrees removed (`git worktree remove --force`), scratch ASan build removed. Logs and raw JSON remain in the audit scratch directory and are not part of the repository.
