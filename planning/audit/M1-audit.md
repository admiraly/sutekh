# M1 independent closure audit (M1-audit)

Auditor: independent closure auditor, worktree `agent-abb2fd2a4c686b3e8`, HEAD `01642ab` at start.
Scope: reproduction of builds, tests, harness, probes and evidence; mutation testing and a bounded protocol fuzz in scratch copies outside the repository. No engine, harness, status-doc or evidence source was edited. Scratch root: `/tmp/claude-0/-home-user-sutekh/82533bc6-e6ca-564a-ba43-39dd3091bafa/scratchpad/` (outside the repo). Handoffs, status doc and evidence were not trusted until reproduced.

Note on the worktree: `tests/platform/run_platform.py` rewrites three tracked evidence files on every run (`planning/evidence/m1/platform-{linux,matrix,windows-cross}.json`). The auditor's run changed only `generated_utc`/duration fields; the files were restored with `git checkout`.

## 1. Verdict

**M1 cannot close.**

What blocks, exactly:

1. **Closure item "real native build latency measurements" has no evidence.** No M1 file in `planning/evidence/m1/` contains build latency. The only numbers are prose observations in handoffs (M1-00: g++ 7.81 s, clang++ 6.44 s for the AsmJit build; M1-01: scalar median 0.80–0.97 ms; M1-03: throughput observations). The auditor did not time a native build (see section 6).
2. **M1-03 acceptance "real isolated throughput and end-to-end cost" is not evidenced.** The only throughput data is a non-gating handoff observation (21 samples on a shared VM, handoff M1-03 section 9).
3. **M1-04 scheduler is not integrated.** `src/runtime/jobs/su_jobs.c` and `su_effects.c` are included only by `tests/scheduler/test_scheduler.c` (grep over the tree). `su_sched_run_ticks` is not changed. The M1-04 handoff says so ("Runtime integration ... was not changed"). The M1-04 acceptance is "add disjoint chunk scheduling", which in the shipped runtime is not met.
4. **C07 containment is weaker than the constitution requires (section 7, defect D1).** ENGINE_CONSTITUTION.md line 36 requires disposable workers with "bounded CPU, memory, file, and network access". Production `sutekh serve` sets no address-space limit, no file or network limit, and passes the full environment to disposable children.
5. **Stale-artifact rejection is only partly tested.** Three install/admission checks (artifact-hash equality, install-time layout check, install-time semantic check) survive every ctest run when removed (section 4, H1/L1/L2).
6. **Multi-agent experiment is observational and incomplete.** Integration latency and token/tool-call metrics are null for every row, and the outcome classes are the orchestrator's own judgement (section 5).

Non-blocking but must be corrected: `tasks.json` still lists all M1-00..M1-05 as `planned`; the M0 status doc states a harness self-test of 150 checks while the current self-test reports 176; `planning/evidence/m1/agent_workflow.json` shows M1-05a as `in_progress` with no commit although it is committed (`head` in that file predates three commits).

## 2. Reproduction (commands, return codes)

Build and test outputs are under the scratch `logs/` directory.

| Check | Command (cwd = worktree unless noted) | rc | Result |
|---|---|---|---|
| AsmJit bootstrap | `python3 tools/bootstrap_asmjit.py` | 0 | `ok: true`, `zlib_text_present: true` |
| Default dev build | `python3 tools/dev.py build --profile dev` | 0 | 52/52 steps |
| Default release build | `python3 tools/dev.py build --profile release` | 0 | 52/52 steps |
| Native gcc Release | `cmake -S <wt> -B <scratch>/native-gcc -G Ninja -DSUTEKH_NATIVE=ON -DCMAKE_BUILD_TYPE=Release` then `cmake --build` | 0 / 0 | 134/134 steps |
| Native clang Release | same with `CC=clang CXX=clang++` | 0 / 0 | 134/134 steps |
| ASan+UBSan native (Debug, gcc, `-fno-sanitize-recover=all`) | `cmake ... -DCMAKE_C_FLAGS="-fsanitize=address,undefined ..."` then build | 0 / 0 | built outside repo |
| ctest default (release, `build/`) | `ctest --test-dir build -j4 --timeout 600` | 0 | 9/9, 8 s |
| ctest native gcc | `ctest --test-dir <scratch>/native-gcc -j4` | 0 | 19/19, 27 s |
| ctest native clang | same | 0 | 19/19, 29 s |
| ctest ASan native | `ctest --test-dir <scratch>/asan-native -j2` | 8 | 18/19; `specialize_churn` failed (RSS bound, see below) |
| `specialize_churn` ASan with `ASAN_OPTIONS=quarantine_size_mb=1` | `ctest -R specialize_churn` | 0 | passed, 37 s |
| Harness self-test | `python3 -I tests/harness/run_m0.py --self-test` | 0 | 176 checks, 0 failures |
| Harness, default binary | `python3 -I tests/harness/run_m0.py --binary build/sutekh --report-dir <scratch>/h-default` | 0 | passed 18, failed 0, not_run 0, unavailable 2 (TX3, TX4 expected) |
| Harness, native binary | same with native binary | 0 | passed 20, failed 0, not_run 0, unavailable 0 |
| Demo, native | `python3 tools/demo_zero_wait.py --binary <native> --out <scratch>/demo-native` | 0 | status passed |
| Demo, default | same, default binary | 0 | status passed |
| No-build probe (native) | `python3 -I tests/e2e/no_build_probe.py --binary <native>` | 0 | verdict pass; 0 build-tool execs in the whole run; both positive controls detected |
| Native spawn probe | `python3 -I tests/e2e/native_spawn_probe.py --binary <native>` | 0 | 7/7 exec attempts successful, no build tool |
| Broker unit tests | `python3 -m unittest tests/broker/test_broker.py` | 0 | Ran 17 tests, OK |
| Scheduler mutants (repo script) | `bash tests/scheduler/run_mutants.sh <scratch>/sched-mut` | 0 | 6/6 killed |
| Platform lanes | `python3 -I tests/platform/run_platform.py` | 1 | counts: passed 17, failed 1, not_run 4, unavailable 2 (the failure is the full Windows graph inventory, expected) |

Native ctest with check counts (verbose run, same binaries): `test_native_avx2` corpus 5000 programs verified, 245,497 AVX2 differential runs, 142 checks, 0 failures; `test_native_scalar` 183 checks; `test_native_fault` 219 checks; `test_specialize_admission` 259 checks. These match the handoffs.

## 3. Per-closure-item table (MILESTONES.md M1 "Closure evidence")

| Item | Verdict | Proof (exact command or file) |
|---|---|---|
| Opcode/tail/alignment/ABI differential corpus | **PASS (Linux SysV; gcc and clang)** | `ctest -R native_scalar` (183 checks, corpus generator seed 20261011, digest `tests/native/scalar/gen/corpus.sha256` checked); `ctest -R native_avx2` (142 checks, 5000 programs x 49 row counts, 245,497 runs; `tests/native/avx2/gen/corpus.sha256` digest checked); hand-written KATs in both files (`test_native_avx2.c` section 2, `test_native_scalar.c` section 1); SysV callee-saved clobber trampoline (`test_native_scalar.c` section 5). Generator covers const, load, add, sub, mul, eq, lt, select, store (`tests/native/scalar/gen/gen_corpus.py`). Win64 ABI: wine only (section 8). |
| Capability fallback | **PASS (forced-disable hook only)** | `tests/native/avx2/test_native_avx2.c` lines 431-443: `su_native_test_disable_caps` makes AVX2 requests return `SU_E_CAPABILITY_DENIED` and scalar still works; `su_native_best_backend()` returns scalar. This host has AVX2, so a real no-AVX2 host was not run. |
| Stale artifact rejection | **PARTIAL** | Layout generation: `backends/native/su_native.c:598` and `:477`; killed by mutant W2 (native_scalar, native_avx2 red). Semantic-hash and layout install checks (`src/runtime/worker.c` ~2233, ~2249) and artifact-hash equality (`src/runtime/native_versions.c:128`): mutants H1, L1, L2 **survive** all 19 ctest suites. |
| Safe retirement | **PASS** | `test_specialize_inflight` (26 checks), `test_specialize_churn` (200 checks, 1000 cycles), `test_native_versions` (retire with inflight); ctest green. The churn RSS bound is sensitive to the ASan quarantine setting (section 4). |
| Worker-fault recovery | **PASS** | `ctest -R recovery` (3459 checks, default build and native); `test_native_fault` 219 checks; harness TX2 24 checks (kill from outside). Mutants R1 (replay digest check off) red in `recovery`; C2 red in `control`. |
| Real native build latency measurements | **FAIL (missing)** | No M1 build-latency evidence file. Only handoff observations (`planning/handoffs/2026-10-12-m1-00.md` line 27-28). The auditor's own build times were not recorded. |
| Small multi-agent experiment, measured rejection/rework | **PARTIAL** | `planning/evidence/m1/agent_workflow.json` (33 rows: 29 impl, 4 review). Recomputed in section 5. Integration latency, tokens and tool calls are null. Outcome classes are the orchestrator's judgement (`agent_outcomes.json`). |

Per-task acceptance (`planning/tasks.json`):

| Task | Acceptance item | Verdict | Proof |
|---|---|---|---|
| M1-00 | Durable deduplicated jobs and stale-result rejection | **PASS** | `python3 -m unittest tests/broker/test_broker.py` rc 0 (17 tests); mutants B1 (dedupe off) and B2 (stale check off) each red (3 and 2 failing tests). |
| M1-00 | ABI tests and compilation, no gameplay build dependency | **PASS** | native ctest 19/19; harness T12 and no-build probe rc 0. |
| M1-01 | All supported opcodes match reference corpus | **PASS** | section 3 row 1. |
| M1-01 | W^X, failure handling, source maps, platform convention | **PASS on Linux; Win64 partial** | W^X check `test_native_scalar.c` section 6 (mutant P1 red in 4 suites); injected mmap/mprotect failures (`test_native_scalar.c` ~line 986, `test_native_avx2.c` ~1353); source map (`test_native_scalar.c` section 8). Win64 only under wine. |
| M1-02 | No stale pointers after repeated swaps or in-flight jobs | **PASS** | `specialize_inflight`, `specialize_churn`, `native_versions` green. |
| M1-02 | Crash recovery records replaced PID and correct checkpoint replay | **PASS** | `recovery` green, TX2 (harness, native and default), `test_native_fault`. |
| M1-03 | Zero/tails/unaligned/overflow corpus exact | **PASS** | corpus rows 0..1000 with unaligned columns (`test_native_avx2.c`), 3000-register chain returns `SU_E_RESOURCE_LIMIT` (probe). |
| M1-03 | Real isolated throughput and end-to-end cost; scalar fallback works | **PARTIAL** | Fallback PASS (section 3). Throughput is observation only (handoff M1-03 section 9). |
| M1-04 | Serial/parallel canonical results match for varied completion orders | **PASS (library only)** | `test_scheduler`: checks 852844, 0 failures (run_mutants baseline); 200 seeds (handoff M1-04). |
| M1-04 | Conflicts and ordering cycles rejected | **PASS (library only)** | Repo mutants `no_conflict_check` and `no_cycle_check` killed (6 and 6 failures). |
| M1-04 | (integration) chunked runner in the runtime | **FAIL** | No caller in `src/` (section 1, item 3). |
| M1-05 | Actual Linux/Windows results distinguished from not-run | **PASS** | `planning/evidence/m1/platform-matrix.json` lanes: `linux-native`, `win64-cross` (wine), `win64-native` not_run, `macos` and `arm64` unsupported/unavailable. |
| M1-05 | Queue/rework/accepted-task metrics before increasing agent count | **PARTIAL** | Collector `bench/agent_workflow/collect.py` and `agent_workflow.json` exist; integration latency null. |

## 4. Test table by lane

Native ctest, gcc Release (19/19 rc 0) and clang Release (19/19 rc 0): all suites passed. Times over 10 s: `native_avx2` 25 s (gcc), 84 s (ASan, -j2); `native_scalar` 6.7 s (gcc), 43 s (ASan); `specialize_admission` 10 s; `specialize_churn` 8 s (gcc), 42 s (ASan, failed); `scheduler` 5.6 s (gcc), 19 s (ASan). No suite exceeded 2 min; no timeouts.

| Lane | Result |
|---|---|
| ASan+UBSan native ctest (`-j2`, no quarantine env) | 18/19 passed, rc 8. `specialize_churn` failed `rss <= base_rss + 16384u` (base 28,784 kB, end 114,404 kB). Re-run with `ASAN_OPTIONS=quarantine_size_mb=1` passed rc 0. The 16 MB bound is test-chosen and depends on the quarantine setting. No sanitizer report in the other 18 suites. |
| Default ctest (release) | 9/9, rc 0 |
| Harness default (dev/release binary) | 18 passed, 0 failed, 2 unavailable (TX3/TX4 are native-only) |
| Harness native | 20 passed, 0 failed |
| Platform matrix | 17 passed, 1 failed (full Windows graph), 4 not_run, 2 unavailable |

## 5. Multi-agent experiment (`planning/evidence/m1/agent_workflow.json`)

Recomputed from the JSON rows (the file's own summary is consistent when the in-progress row is excluded):

- Implementation rows: 29; `accepted_as_is` 19; `accepted_with_integrator_fix` 7; `partial` 2; `in_progress` 1 (M1-05a). Excluding the in-progress row: first-pass acceptance 19/28 = **0.679**; rework (7+2)/28 = **0.321**. Including it in the denominator: 19/29 = 0.655.
- Agent wall time (32 non-null rows): median **551.5 s**, max 2241 s.

Derivation check: launch proxy is the birth time of `/home/user/sutekh/.git/worktrees/agent-<id>`, finish is the last commit. Verified for six M1 rows: M1-03 (birth 20:15:40, last 20:36:26, 1246 s), M1-02a (20:15:16 to 20:52:37, 2241 s), M1-01 (19:55:59 to 20:13:42, 1063 s), M1-04 (19:48:03 to 20:04:00, 957 s), M1-00 (19:48:03 to 19:54:51, 408 s), M1-02b (20:55:48 to 21:08:56, 788 s). All match. Other defects: M1-05a row has `in_progress` and null wall time although git has its commit at 22:32:32; the file head `a628edf` predates later commits. Integration latency, tokens and tool calls are null for all 33 rows. This is one session, one tier, 4 review rows, and not a controlled comparison. It does not meet "measured rejection/rework" with integration cost.

## 6. Build and latency

The auditor did not record any native build timing; the clean native builds above were not timed. M1 native build latency is therefore **not measured by this audit** and has no evidence file.

## 7. C07 probe and defects

### C07 attack attempts

- **Descriptors:** a live probe (`scratchpad/c07_probe.py`, native release) sampled the child of `sutekh serve` during `world.specialize`. The child held only fds 0, 1 and 2 (pipes and `/dev/null`). Parent pipe ends are close-on-exec (`src/agent/su_proc.c:138-146`, dup2 in child at 168-178). **No descriptor leak found.**
- **Rlimits:** the sampled child showed `Max address space unlimited` and `Max cpu time unlimited` in `/proc/<pid>/limits`. Cause: `src/platform/sutekh_main.c:70-73` builds `SuAgentConfig cfg = {0}` and sets only `project_root` and `worker_argv`. `src/agent/su_agent.c:3933-3944` (`config_limits`) gives defaults for checkpoints, log, spool and quarantine but **none for `worker_address_space_bytes`**. `src/agent/su_proc.c:179` applies RLIMIT_AS only when the value is > 0. The disposable admission child is spawned with the same zero value (`su_agent.c:3612-3614`), so **no memory limit applies to the disposable child in production**. CPU is limited (`timeout/1000 + 2` seconds, `su_agent.c:3613`). Wall timeout exists.
- **Environment:** `execv` (`su_proc.c:191`) passes the full environment. Keys visible in the child's `/proc/<pid>/environ` include credential-named variables (names only recorded here; values not reproduced).
- **Temp files and spool:** spool is `mkdtemp` (0700) under `$TMPDIR` or `/tmp` (`src/agent/su_spool.c:104`), files are created `O_EXCL` with mode 0600 (`su_spool.c:181`). A config-supplied spool dir is checked only for existence and directory type (`su_spool.c:85-90`), not owner or mode. Production passes no spool dir.
- **Supervisor kill:** the child runs as the same uid, has no seccomp filter and no `setsid` separation. A kernel that executed arbitrary machine code could signal the supervisor. The kernel generator emits no syscalls (`backends/native/*.cpp`), so this needs a codegen or LIR-driven bug first. **Not demonstrated.**
- Frame arithmetic: `SU_LIMIT_REGISTERS` is 4096 (`include/sutekh/ir.h:18`), so `kSlotBytes * nregs` cannot overflow in either lowering. Alias check order: `row_limit` is checked before `check_alias` (`su_native.c:615-618`).

### Defects, ranked

| # | Severity | Location | Defect and trigger |
|---|---|---|---|
| D1 | High (C07) | `src/platform/sutekh_main.c:70-73`, `src/agent/su_agent.c:3933-3944`, `src/agent/su_proc.c:179-191`, `docs/AGENT_RPC.md:169` | No RLIMIT_AS in production; full environment inherited; no file or network restriction, contrary to ENGINE_CONSTITUTION.md:36. Trigger: any admission. Observed: `Max address space unlimited` on the live worker. |
| D2 | Medium | `src/agent/su_agent.c` (3 mutants, install path), `tests/native/qualification/*` | Install-time artifact-hash equality, layout and semantic checks have no failing test. Mutants H1, L1, L2 survive ctest. |
| D3 | Medium | `src/runtime/jobs/*`, `src/platform/` (M1-04 integration) | Chunked scheduler not called by any runtime path. |
| D4 | Low | `tests/native/qualification/test_specialize_churn.c:609` | RSS bound of 16 MB fails under ASan without `quarantine_size_mb=1` (passes with it). |
| D5 | Low | `src/agent/su_path.c:124` | `open(path, O_RDONLY)` without `O_CLOEXEC`. Transient; no leak seen in the child. |
| D6 | Low | `tests/platform/run_platform.py` | Rewrites tracked evidence on every run (timestamps only). |
| D7 | Low (records) | `planning/evidence/m1/agent_workflow.json`, `tasks.json`, `docs/IMPLEMENTATION_STATUS.md` | M1-05a `in_progress` while committed; all M1 tasks `planned`; M0 status doc says self-test 150 checks (current 176). |

Not a defect: the AVX2 vector-loop bool representation (all-ones mask) differs from the scalar 0/1 bool, but the verifier forbids bool-to-u32 coercion (`include/sutekh/ir.h:34`), so bools only feed SELECT conditions and never reach an output.

Not a defect: declared-vs-IR effect check exists (`src/runtime/jobs/su_effects.c:63-81`, used at 254).

## 8. Win64 claims

Every Win64 statement in `docs/IMPLEMENTATION_STATUS.md` (M1 platform section) and `planning/contracts/M1_NATIVE_CONTRACT.md` section 2 is limited to wine 9.0 and mingw cross evidence: `native_abi_probe` 597 checks, 0 failures (`platform-windows-cross.json`, `checks: 597`). Windows-native is marked not run, and the full Windows graph fails (22 objects). The status-doc claims match the evidence. No Windows-native claim was found.

## 9. Fuzz (C07 and control protocol)

Target: `sutekh` built from the worktree source with `gcc -fsanitize=address,undefined -fno-sanitize-recover=all` (Debug, `-DSUTEKH_NATIVE=ON`). Driver: `scratchpad/fuzz_ctl.py`, valid session (`engine.info`, `world.create`, `capsule.submit`, `candidate.evaluate`) followed by random requests over `world.specialize`, `candidate.install`, `world.snapshot`, `world.restore`, `world.step`, `world.inspect`, `world.close`, `candidate.status`, `world.create`, `candidate.evaluate`. About 60% of requests carry mutated fields (drop, null, wrong type, huge or negative ints, long strings, path escapes, unknown ids, stale revisions); 2% are structurally invalid lines. Environment `ASAN_OPTIONS=quarantine_size_mb=1:halt_on_error=1`.

| Seed | Sessions | Requests | ok:true | ok:false | Timeouts | EOF | Bad JSON | Crashes | Sanitizer hits |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 30 | 1320 | 430 | 890 | 0 | 0 | 0 | 0 | 0 |
| 2 | 30 | 1319 | 456 | 863 | 0 | 0 | 0 | 0 | 0 |
| 3 | 30 | 1320 | 430 | 890 | 0 | 0 | 0 | 0 | 0 |
| Total | 90 | 3959 | 1316 (33.2%) | 2643 | 0 | 0 | 0 | 0 | 0 |

Error codes: `SU_E_TYPE` 1432, `SU_E_INVALID_ARGUMENT` 804, `SU_E_STALE_REVISION` 256, `SU_E_PARSE` 70, `SU_E_IO` 63, `SU_E_CAPABILITY_DENIED` 13, `SU_E_RESOURCE_LIMIT` 5. Requests per method: `world.specialize` 1071, `world.snapshot` 313, `world.step` 291, `world.close` 290, `candidate.install` 279, `world.inspect` 284, `world.restore` 270, `candidate.status` 270, `candidate.evaluate` 362, `world.create` 349. Caveat: per-method success counts were not recorded, so the fraction of successful `world.specialize` and `candidate.install` calls is unknown. Caveat: the fuzz is not coverage-guided.

## 10. Mutation testing

Driver: `scratchpad/mutate.py` and `scratchpad/mutate2.py`, operating on a copy of the tracked and fetched tree (`scratchpad/mut/src`), native gcc Release, ctest after each single edit, file restored after each mutant. Anchors asserted to occur exactly once. Baseline: ctest 19/19 rc 0. Killed means at least one suite red.

| ID | Group | Mutation | Red suites | Result |
|---|---|---|---|---|
| S1 | scalar | ADD emits `sub` (`su_native_asmjit.cpp`) | native_avx2, native_fault, native_scalar, native_versions, specialize_admission, specialize_churn, specialize_inflight | killed |
| S2 | scalar | SUB emits `add` | native_avx2, native_scalar, specialize_admission | killed |
| S3 | scalar | MUL emits `add` | native_avx2, native_scalar, specialize_admission, specialize_churn, specialize_inflight | killed |
| S4 | scalar | LT uses signed `setl` | native_avx2, native_scalar, specialize_admission | killed |
| S5 | scalar | SELECT `cmove` (inverted) | native_avx2, native_scalar, specialize_admission | killed |
| S6 | scalar | EQ `setne` (inverted) | native_avx2, native_scalar, specialize_admission | killed |
| S7 | tail | AVX2 scalar epilogue stride 2 | native_avx2, native_fault, native_versions, specialize_admission, specialize_churn | killed |
| A1 | AVX2 signed compare | LT without sign flip | native_avx2, specialize_admission | killed |
| A2 | AVX2 blend | `vpblendvb` operands swapped | native_avx2, specialize_admission | killed |
| A3 | AVX2 tail count | vector loop bound `& -4` | native_avx2, native_fault, native_versions, specialize_admission, specialize_churn | killed |
| A4 | AVX2 vzeroupper | `vzeroupper` removed before epilogue return | native_avx2 | killed (only one suite) |
| A5 | AVX2 add | `vpaddd` emits `vpsubd` | native_avx2, native_fault, native_versions, specialize_admission, specialize_churn | killed |
| W1 | wrapper alias | `check_alias` result overwritten with OK | native_avx2, native_scalar | killed |
| W2 | wrapper generation | input layout-generation check disabled | native_avx2, native_scalar | killed |
| P1 | W^X | publication mapping `RWX` | native_avx2, native_scalar, native_versions, specialize_churn | killed |
| H1 | artifact hash | `strcmp(artifact_hash)` check disabled (`native_versions.c:128`) | none | **SURVIVED** |
| L1 | install staleness | layout check at install disabled (`worker.c` ~2233) | none | **SURVIVED** |
| L2 | install staleness | semantic-hash check at install disabled (`worker.c` ~2249) | none | **SURVIVED** |
| Q1 | quarantine | quarantine lookup disabled (`su_agent.c` `quar_find`) | native_fault | killed (one suite) |
| R1 | recovery state | replay digest verification disabled (`su_agent.c` ~1230) | recovery | killed (one suite) |
| B1 | broker dedupe | dedupe disabled (`broker.py` submit) | test_broker (3 failing tests) | killed |
| B2 | broker stale | stale check disabled (`broker.py` validate_result) | test_broker (2 failing tests) | killed |
| C1 | world.close | checkpoint release skipped (`worker.c` `ckpt_release_world`) | control | killed |
| C2 | world.close | registry release skipped (`worker.c` ~1923) | control | killed |

Summary: 24 mutants, 21 killed, **3 survived (H1, L1, L2)**. Two mutants (W1 first attempt, Q1 first attempt, C1 first attempt) failed to build under `-Werror` because the edit left a parameter or function unused; they were re-run with edits that keep the symbols used. The B1/B2 first attempts used a discover command that ran zero tests; re-run with `python3 -m unittest tests/broker/test_broker.py`.

Scheduler mutants (repo script, not mine): `last_writer_wins_on_failure`, `chunk_boundary_off_by_one`, `commit_on_failure`, `no_completion_gate`, `no_conflict_check`, `no_cycle_check`: all killed, rc 0. The script covers six mutants; there is no mutant for chunk merge specifically.

Mutation caveat: the scheduler and broker mutants were not run in the harness; the three survivors were not tested against the harness T07/T06 either.

## 11. Honesty checks

- **Commit identity:** 78 commits after `20f2625`; authors and committers all `admiraly <249692417+admiraly@users.noreply.github.com>`. Co-Authored-By trailers after `20f2625`: **0**.
- **Licence files:** `git diff 20f2625 HEAD -- LICENSE LICENSES COMMERCIAL_LICENSE.md CLA.md NOTICE.md SECURITY.md` is empty. `THIRD_PARTY_NOTICES.md` gains one line (AsmJit zlib entry, commit `0d7f0105...`, licence sha256 `13870bd5...`).
- **AsmJit:** not vendored. Tracked: `third_party/asmjit.lock.json`, `backends/native/su_native_asmjit.cpp`, `tools/bootstrap_asmjit.py`. `.gitignore:22` ignores `/third_party/asmjit/`.
- **Spot-checked numbers (9 items, all matched):** AVX2 142 checks and 245,497 runs; scalar 183 checks; native_fault 219; specialize_admission 259; TX2 24 and TX3 144 (harness); platform probe 587 (Linux) and 597 (Win64 wine); platform matrix counts 17/1/4/2; workflow median 551.5 s and rates 0.679/0.321; six worktree-derived wall times. One mismatch: `docs/IMPLEMENTATION_STATUS.md` says the M0 harness self-test has 150 checks; the current self-test reports 176 (`planning/evidence/m0-harness-self-test.log` still says 150).
- **Fake PASS paths:** none found in the M1 tests reviewed. The Win64 probe's own mutant (inverted max reference) turned red, per handoff.

## 12. Unrun checks and limits

- Clang sanitizers (ASan/UBSan clang build) not run.
- Windows native (MSVC or clang-cl) not run; no Windows host.
- Native build latency (cold or incremental) not timed by the auditor.
- AVX2 throughput not re-measured.
- ASan ctest only with `-j2` and without `quarantine_size_mb=1` for the full run; the single failing suite was re-run with it.
- Harness mutant runs (T06/T07 against H1/L1/L2) not run.
- Fuzz: per-method success counts not recorded; not coverage-guided; 3 seeds only.
- C07 exploit of a kernel killing the supervisor: not attempted to completion (requires a codegen bug).
- No A/B of the multi-agent workflow (single session, orchestrator judgement).
- Default OFF build did not include `su_native` objects in the ctest run (default ctest 9/9 only).
