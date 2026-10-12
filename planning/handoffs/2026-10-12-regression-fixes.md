# Handoff: regression fixes from planning/audit/regression-audit.md

Date: 2026-10-12. Scope: R1/R2 (performance), R4 (world.snapshot capture_ns), the migration-runtime LSan leak, and two IR-suite test gaps. Baseline for every comparison: `9af40cc` (M0). Worktree base: HEAD `edbbbfb` (branch `claude/zealous-dirac-3xlco1`). Concurrent worker.c / su_lir.c / su_effects / capsule-loader work was not touched.

## 1. Status summary

| Item | Status | Evidence |
|---|---|---|
| R4 `world.snapshot` `capture_ns` null | Fixed. Cause: `h_world_snapshot` (src/agent/su_agent.c) hard-coded `"capture_ns":null`; the supervisor never read the timing. Now times `checkpoint_world` with the monotonic clock; null only if the clock is unavailable. | `tests/control/test_control.c` (new CHECK after `sa`); control ctest passes; raw reply 280,378 ns (HEAD) vs 17,993 ns (baseline). Scope differs: HEAD times the worker round trip plus spool write, baseline timed only the worker capture. |
| Migration runtime leak (audit R8) | Fixed. Cause: in `run_alloc_faults`, the n==1 branch sent a rollback into `Resp r` still holding the failed commit reply, so `call()` overwrote it. `resp_free(&r)` added before. | valgrind `--leak-check=full` on `test_migrate_runtime`: before 139 B definitely + 65,560 B indirect lost; after `in use at exit: 0 bytes`, 0 errors. ctest `migration_runtime` passes. |
| IR minor gate test gap (audit mutant C) | Fixed. New `test_ir02_minor_gate` in `tests/ir/test_ir.c`: verifier refuses 0.2 opcodes in 0.1 text (5 names), and `su_vm_context_create` refuses each 0.2 opcode substituted into a verified 0.1 module (positive control first). `test_ir` now links `su_vm`. | Mutant C (`su_insn_known` accepts 0.2 opcodes) applied to `src/vm/su_vm.c`: `test_ir` 191 passed, 4 failed (before: 195 passed). File restored byte-identical (`cmp`). |
| Cross-binary snapshot test | Added. Provenance of the 0.1 fixture: the `9af40cc` build of `tests/snapshot/test_snapshot.c` (scratch worktree only) with a scratch-only `SU_DUMP_SNAP` hook that wrote the tick-3 round-trip bytes; the hook is not committed. The HEAD 0.2 fixture is written by `build/test_snapshot --regen-cross <file>`. `tests/snapshot/test_snapshot.c` `test_cross_binary`: loads `tests/fixtures/snapshot01/baseline_9af40cc_example_0_1.json` (0.1, written by the baseline build) and `tests/fixtures/snapshot02/head_example_0_2.json` (0.2, written by HEAD); both must parse, restore to the example world at tick 3, and equal the live world. | snapshot suite 444 checks, 0 failures. Baseline parser on the HEAD 0.2 file: `SU_E_TYPE` (refused, expected); on the 0.1 file: `SU_OK`. |
| Boundary decimal test | Added `test_decimal_boundaries` (u32 digit-count boundaries, 4294967295, entity ids) because the encoder changed. | Included in the 444-check snapshot suite. |
| R1 (per-step, install) | Partly reduced; not closed. See section 4. | Instruction counts and wall-clock tables in section 3. |
| R2 (cold world.create) | Partly reduced (the same encoder path); not closed. | Section 3. |
| R3, R5, R6, R7, R9, R10 | Unchanged (documentation items of the audit). | - |

## 2. Modified paths (this change set)

Modified: `CMakeLists.txt` (one line: `test_ir` links `su_vm`), `src/agent/su_agent.c` (R4), `src/vm/su_vm.c` (row-loop checks hoisted; M0 path no longer does a per-row alive load or an unconditional `row_seq` store), `src/world/snapshot.c` (encoder: in-place decimal writer, inline single-byte append), `tests/control/test_control.c`, `tests/ir/test_ir.c`, `tests/migration-runtime/test_migrate_runtime.c`, `tests/snapshot/test_snapshot.c`.
Added: `tests/fixtures/snapshot01/baseline_9af40cc_example_0_1.json`, `tests/fixtures/snapshot02/head_example_0_2.json`, this file.
Not touched (the concurrent agent's scope): `src/runtime/worker.c`, `src/ir/su_lir.c`, `src/runtime/jobs/su_effects.c`, the capsule loader.
Not touched but checked: `src/ir/su_sha256.c` (an unrolled round function was tried and reverted; no gain, section 4).

Source hashes (sha256, first 16 hex):

| Path | sha256 (16) |
|---|---|
| src/agent/su_agent.c | 9db9fd1b3ef0d699 |
| src/vm/su_vm.c | b9ed63e9f2eb2a63 |
| src/world/snapshot.c | 324701ee1cc23fb2 |
| tests/control/test_control.c | a2c7a5bbf1fb9794 |
| tests/ir/test_ir.c | bec137d45e1fa5a4 |
| tests/migration-runtime/test_migrate_runtime.c | e618ad3833bbc0a0 |
| tests/snapshot/test_snapshot.c | 992644c477e3d5ce |
| CMakeLists.txt | 9c2b33df6c28fd15 |
| tests/fixtures/snapshot01/baseline_9af40cc_example_0_1.json | b1b310288513e91b |
| tests/fixtures/snapshot02/head_example_0_2.json | 7ec9781319a13f21 |
| tests/harness/run_m0.py (unchanged) | 2fc4b8156c6d3089 |

Binaries (release, `python3 tools/dev.py build --profile release`, gcc 13.3.0, cmake 3.28.3, ninja):

| Binary | sha256 (16) | size (bytes) |
|---|---|---|
| baseline `9af40cc` `build/sutekh` | d5714cb06c351ec1 (matches audit and `planning/evidence/m0-artifact-hashes.txt`) | 177,168 |
| HEAD `edbbbfb` unmodified (`head_before`) | ab0c544328a36d17 | 452,256 |
| This change set (`build/sutekh`, final) | 6a8a044e345bd921 | 448,224 |

The HEAD hash differs from the audit's `95d6593b` because the worktree is at a later commit than `b839066`.

## 3. Measurements

Environment: 4 vCPU Intel Xeon 2.10 GHz shared VM, 15 GiB, kernel 6.18.44-fc-v114, gcc 13.3.0, release (-O3 -DNDEBUG). Shared-VM observation only (C10). No `perf` available; instruction counts come from `valgrind --tool=callgrind` (deterministic; the primary evidence). Workloads (driver: `drive.py`, not committed): **step** = create `t02/world_1024` + movement, 60 × 5-tick steps (first 5 discarded from the timing sample) and 8 × 200-tick steps (first discarded); **install** = create, `capsule.submit`, `candidate.evaluate` (movement_1024), `candidate.install`; **cold** = `engine.info` then `world.create`.

### 3a. Deterministic instruction counts (callgrind, per process)

Step workload, worker process:

| Build | worker total Ir | su_vm_invoke_v1 self | su_sha256_update self | buf_dec + buf_bytes + emit_body + memcpy (encoder, self) |
|---|---|---|---|---|
| baseline 9af40cc | 300,231,841 | 290,255,400 | 0 (not called) | 0 |
| HEAD before | 644,087,605 | 311,664,600 | 188,248,610 | ~97,300,000 (47.1M + 39.4M + 10.8M) |
| this change set | 600,261,256 | 305,831,600 | 188,248,610 | ~57,100,000 (35.5M + 3.1M + 10.2M + 8.3M; `buf_ch` now inlined into callers) |

Install workload, all processes (serve + workers):

| Build | serve | worker | worker | total |
|---|---|---|---|---|
| baseline 9af40cc | 5,024,301 | 3,715,083 | 10,675,923 | 19,415,307 |
| HEAD before | 8,701,888 | 18,633,039 | 11,128,164 | 38,463,091 |
| this change set | 8,701,473 | 17,225,916 | 11,115,865 | 37,043,254 |

The step total falls 6.8% (644.1M to 600.3M) and the install total 3.7%. The remaining gap is the supervisor's per-operation digest (section 4).

### 3b. Wall clock (5 alternating runs per variant; median of per-run medians [min, max]; ms)

| Metric | baseline | HEAD before | this change set |
|---|---|---|---|
| step, 5-tick per-step median | 0.106 [0.094, 0.113] | 0.551 [0.503, 0.630] | 0.465 [0.409, 0.777] |
| step, 200-tick per-step median | 2.125 [1.543, 2.160] | 3.267 [3.165, 5.250] | 2.852 [2.328, 5.120] |
| cold `world.create` | 2.182 [1.763, 3.520] | 4.325 [3.247, 5.142] | 4.282 [3.270, 4.496] |
| cold `engine.info` first reply | 0.986 [0.822, 1.189] | 1.197 [1.057, 2.074] | 1.142 [0.863, 1.217] |
| `candidate.install` | 0.503 [0.455, 0.965] | 1.458 [1.083, 2.379] | 1.687 [1.408, 2.016] |
| serve + worker RSS after one cold create (kB) | 4,280 to 4,300 | 5,000 to 5,024 | 4,992 to 5,012 |

The wall-clock differences between HEAD before and this change set lie within the ranges and are not significant. The baseline's own 5-tick median moved from 0.126 ms in an earlier run to 0.106 ms here, so the VM drift is larger than the effect. The regression is real and reproduces (about 4-5x on the 5-tick step and 3x on install, with non-overlapping ranges for step and create).

### 3c. Protocol byte-stability (M0-era requests)

Fixed sequence (`engine.info`, `world.create` world_8, `world.inspect`, `world.step` 3 ticks, stale step, `world.snapshot`, `world.restore`, `capsule.submit`, `candidate.status`, `candidate.evaluate`, `world.inspect`, `candidate.install`, `world.inspect` paged, unknown method, unknown world; 15 replies) run against HEAD-before and this change set. Masked only pid and timing fields (`worker_pid`, `pid`, `prepare_ns`, `critical_section_ns`, `capture_ns`). Result: identical (`diff` exit 0). Snapshot document bytes are identical as well (the golden byte tests in `test_snapshot` cover the 0.2 encoder, including the new boundary test).

## 4. What was reduced, and what was not

Reduced (semantics unchanged; each change was checked by the snapshot or VM suites and the digests):
- `snapshot.c`: decimal digits written in place (two digits per step from a table, digit count first) and single-byte appends inlined. Output bytes identical (golden tests plus `test_decimal_boundaries`). Encoder self cost about 97M to 57M instructions in the step workload.
- `su_vm.c`: M0 path (alive view NULL) no longer loads `ctx->alive[r]` per row, and `row_seq` is reset only for modules that can emit (its only readers are the despawn and emit opcodes, which set `needs_emitter`). VM test: 573 checks passing.
- Supervisor `world.snapshot`: timing only (R4).

Not reduced, with reasons:
1. **Per-operation digest (main cause of R1 and R2).** Every acknowledged `world.step`, `candidate.install` and create runs `sync_after` → `read_digest` → `internal.world.digest`, which is a full canonical capture plus SHA-256 of the whole state (about 2.3M instructions for a 1,024-row world, roughly 0.4 ms). The M0 baseline did no per-op digest at all. This is the replay-verification design of M1-02, not a regression in the tick. Removing it changes recovery semantics and needs owner sign-off.
2. **Duplicate capture when a checkpoint is due.** `sync_after` calls `checkpoint_world` (`internal.world.capture`, which captures again and writes the spool) after the digest has already captured the same state. This is 12 of 81 captures in the step workload (about 15% of capture work). Merging them needs either a worker change (`internal.world.digest` returning the spool, in `worker.c`, which is the other agent's file) or a restructure of `sync_after` that must preserve the no-change early return and the NOROOM policy exactly. Deferred.
3. **SHA-256 throughput.** `su_sha256.c` runs at about 240 MB/s here (about 8.7 cycles per byte; `gcc -O2`). An eight-round unrolled variant produced identical digests but ran at 237 MB/s, so it was reverted. Only SHA-NI or a native-lane implementation would materially reduce this; not done.
4. **VM on M0 worlds, +5.4%** (self cost 290.3M to 305.8M). The alive and emitter hoists recovered about 6M (they had briefly cost more before the emitter flag was hoisted). The remainder is not isolated: the switch in `su_exec_insn` gained five IR 0.2 cases, and the callgrind inlining makes per-line attribution unreliable without debug-info builds. Not reduced.
5. **Schedule and tick-loop copies (audit R1 suspicion: `zero_dead`, entity-table copy and swap).** `su_world_zero_dead`, `su_world_publish` and `su_world_begin_tick` do not appear as separate symbols in the step profile (below the reporting threshold or inlined). They are not shown to be a cost, so nothing was changed there.
6. **Install file loads.** `su_reload_load` is 2.8% of the install worker (about 0.49M instructions). The audit's "avoid file loads on install" is not the driver; the install cost is the digest and checkpoint captures after it (item 1).
7. **Creation (R2).** `world.open` parses the fixture (18% of create instructions) and the initial creation capture is required: it sets the acknowledged digest and the first checkpoint (required by the design).
8. **Binary size (177 KB to 448 KB).** Reachable code (the supervisor and worker share one executable: migration, recovery, entities, native hooks). `-ffunction-sections -fdata-sections -Wl,--gc-sections` saves only 9 KB (2%; scratch build, not adopted). Not reducible without removing features.
9. **RSS (+0.7 MB at create, serve plus worker).** Breakdown (`rss.py`, one world_1024 create): serve RssAnon 180 to 432 kB (of which about 252 kB is the supervisor's fixture read and SHA-256 in `h_world_create`, `su_agent.c` ~2980: the whole fixture buffer is freed but its heap pages stay resident; streaming the hash would remove it, but that changes the path-policy/size checks, not done); serve RssFile 1.89 to 2.03 MB and worker RssFile 1.77 to 2.09 MB (file-backed text pages touched, from the larger binary); worker RssAnon 452 to 472 kB. Per-world growth is therefore mostly transient fixture buffers and code pages, not per-row state.

## 5. Verification

All commands run from the worktree unless noted. Results are exact.

| Check | Command | Result |
|---|---|---|
| Release build, -Werror | `python3 tools/dev.py build --profile release` | rc 0, 0 warnings |
| Debug build, -Werror | `cmake -S . -B <scratch>/rf_dbg -G Ninja -DCMAKE_BUILD_TYPE=Debug` then build | rc 0, 0 warnings |
| ctest release | `cd build && ctest -j4` | 25/25 passed |
| ctest debug | ctest in `rf_dbg` | 25/25 passed |
| M0 harness | `python3 -I tests/harness/run_m0.py --binary build/sutekh --no-build` | totals passed 23, failed 0, not_run 1 (T00, skipped by `--no-build`), unavailable 3 (TX3, TX4, TX10: `world.specialize` absent without native) |
| Harness self-test | `python3 -I tests/harness/run_m0.py --self-test` | 261 checks, 0 failures |
| ASan+UBSan ctest, default env | cmake with `-fsanitize=address,undefined`, `ctest -j2` | 23/25: `control` (LSan "does not work under ptrace"), `containment` (ASan shadow vs the 4 GiB worker RLIMIT_AS) |
| ASan+UBSan, `SUTEKH_WORKER_AS_BYTES=0` | same | 24/25: `control` only (LSan) |
| `control` under ASan, LSan off | `ASAN_OPTIONS=detect_leaks=0 SUTEKH_WORKER_AS_BYTES=0 ctest -R control` | passed |
| ASan+UBSan, UBSan halt on error, LSan off, AS=0 | `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ASAN_OPTIONS=detect_leaks=0 SUTEKH_WORKER_AS_BYTES=0 ctest -j2` | 25/25 passed (no UBSan report) |
| Native bootstrap | `python3 tools/bootstrap_asmjit.py` | ok (fetch; `third_party/asmjit/` is gitignored; lock file unchanged) |
| Native build and ctest | `cmake -S . -B <scratch>/rf_native -DSUTEKH_NATIVE=ON`, build, `ctest -j4` | build rc 0, 0 warnings; 37/37 passed |
| Native harness | harness against the native binary, `--no-build` | passed 26, failed 0, not_run 1 |
| Mutant C (IR gate) | `src/vm/su_vm.c` `su_insn_known` accepts 0.2 opcodes (warning-free form) | `test_ir` FAIL x4, exit 1; file restored (`cmp`), rebuilt, 195 passed |
| Leak | `valgrind --leak-check=full build/test_migrate_runtime build/recovery_worker <root> <scratch>` | 0 bytes in use at exit, 0 errors |
| Snapshot cross-binary | `build/test_snapshot` from the repo root | 444 checks, 0 failures |

Note: ctest runs `test_snapshot` from the source root (fixture paths are relative); run it from the root, not from `build/`.

## 6. Unrun checks and limits

- T00 clean bootstrap of the harness (skipped by `--no-build`; the `dev.py build` above was the clean build).
- TSan, clang builds, Windows/mingw.
- `bench/native/*`, the 1,000,000-row and 250,000-row reload stress passes, `bench/ir02_entities_bench.py`.
- Protocol transcript byte comparison of baseline versus HEAD was not repeated here (section 3c compares HEAD-before with HEAD-after only). Baseline versus HEAD differences are additive keys, the format 0.2 document and the state encoding v2, as recorded in the audit (section 2a, 2b).
- LSan under ptrace (no ASan leak report for `control`; the leak-free property is covered by valgrind for `migration_runtime` only).
- Callgrind attribution is by symbol. Inlining hides some functions; per-line attribution needs a `-g` build (not done).
- The cross-binary test uses the example world only (`examples/world.json`), at tick 3.
- No `perf` counters on this VM (C10: counters null).

## 7. Next work (in order)

1. Owner decision on R1/R2: replace the per-op full digest (or make `internal.world.digest` return the spool so the checkpoint reuses it). This needs `worker.c` (the other agent's scope) and `sync_after` recovery review. It is the only change that moves step and install costs toward baseline.
2. Streaming SHA-256 for the fixture in `h_world_create` (RSS about 0.22 MB anon), after the path-policy checks are kept intact.
3. SHA-NI or an optimized native SHA-256 kernel, behind the native lane's capability check (C12).
4. Per-line profile of the VM (`-g` build) to attribute the remaining +5%.

Scratch artifacts (drivers, callgrind outputs, the baseline worktree and its build, sanitizer and native builds) were in the session scratchpad and are deleted with the scratch worktree; the baseline worktree was removed with `git worktree remove --force`.
