# M1 independent closure re-audit (M1-reaudit)

Auditor: independent re-auditor, worktree `agent-a4ab65f18b0c4d289`, HEAD `8a6cfed` at start.
Scope: reproduction of builds, tests, harness, probes and evidence; blocker re-verification; mutation testing and a bounded protocol fuzz in scratch copies outside the repository. No engine, harness, status-doc, handoff or evidence source was edited. Evidence and status docs were not trusted until reproduced.
Scratch root (outside the repo): `/tmp/claude-0/-home-user-sutekh/82533bc6-e6ca-564a-ba43-39dd3091bafa/scratchpad/ra/` (logs, native-gcc / native-clang / asan builds, `mutsrc` scratch copy, `mutbuild`).

Note on tracked evidence: `tests/platform/run_platform.py` rewrites `planning/evidence/m1/platform-{linux,matrix,windows-cross}.json` on every run. Both runs were followed by `git checkout --` of those three files; `git status` is clean apart from this file.

## 1. Verdict

**M1 cannot close.**

What blocks, exactly:

1. **C07 containment is partial (blocker 4).** ENGINE_CONSTITUTION.md C07 requires disposable workers with "bounded CPU, memory, file, and network access". Verified in this pass: address space is bounded (RLIMIT_AS 4 GiB), the environment is scrubbed, and only fds 0-2 are open. Not bounded: file size (`Max file size unlimited`), open files (soft 20000), network (no restriction, no seccomp or namespace), and CPU for the active worker (`Max cpu time unlimited`). The disposable child has a 12 s CPU soft limit.
2. **M1-04 chunked scheduler is opt-in only (blocker 3).** It is called from `src/runtime/schedule.c:233` and reached from `worker.c:1048`, but only when `SUTEKH_TICK_WORKERS` > 1 (default 1 = serial). Native kernels run as one call. Whether opt-in satisfies "add disjoint chunk scheduling" is a decision this pass does not make; it is listed here because the M1-04 task has no accepted closure decision.
3. **Multi-agent experiment incomplete (blocker 6).** `integration_latency_s.n = 0`, `tokens.n = 0`, `tool_calls.n = 0`. The status doc's workflow numbers (first-pass 0.690, 29 implementation rows, p95 1,325 s) disagree with the JSON (0.677, 31 completed implementation rows, p95 1371 s). See section 6.
4. **Safe-retirement test gap (new).** Mutant M07 (the decrement of `a->inflight` on the real run path in `src/runtime/native_versions.c:593`) survives all 20 ctest suites. A retiring artifact whose run completes never unregisters, and no test notices.
5. **Throughput (blocker 2) is observation only.** `planning/evidence/m1/native-throughput.json` is a shared-VM observation with 21 samples and no frequency control. It is not a qualification. The end-to-end cost evidence is the 1,000,000-row stress file (not rerun here).

Non-blocking but must be corrected: `planning/tasks.json` still lists M1-00..M1-05 as `planned`; `planning/evidence/m1/agent_workflow.json` lists the closure-B row as `in_progress` with null end time although `ba8af44` (2026-10-11 23:55:54 UTC) is committed; the status doc and `planning/evidence/m0-harness-self-test.log` say 150 self-test checks (current binary: 198).

## 2. Reproduction (commands and return codes)

All commands from the worktree root unless noted. `<S>` = the scratch root above.

| Check | Command | rc | Result |
|---|---|---|---|
| AsmJit bootstrap | `python3 tools/bootstrap_asmjit.py` | 0 | ok |
| Default dev build | `python3 tools/dev.py build --profile dev` | 0 | `<S>/dev_build.log` |
| Default release build | `python3 tools/dev.py build --profile release` | 0 | `<S>/rel_build.log` |
| Native gcc configure / build | `cmake -S <wt> -B <S>/native-gcc -G Ninja -DSUTEKH_NATIVE=ON -DCMAKE_BUILD_TYPE=Release`; `cmake --build <S>/native-gcc` | 0 / 0 | |
| Native clang configure / build | `CC=clang CXX=clang++ cmake ... -B <S>/native-clang ...`; build | 0 / 0 | |
| ASan+UBSan native build (Debug, `-fno-sanitize-recover=all`) | `cmake -S <wt> -B <S>/asan ... -DCMAKE_C_FLAGS="-fsanitize=address,undefined ..."`; build | 0 / 0 | used for the fuzz |
| ctest default (release `build/`) | `ctest --test-dir build -j4 --timeout 900` | 0 | 10/10 |
| ctest native gcc | `ctest --test-dir <S>/native-gcc -j4 --timeout 900` | 0 | 20/20 (29 s) |
| ctest native clang | same | 0 | 20/20 (30 s) |
| ctest ASan native | not run as a full suite (see section 8) | - | |
| Harness self-test | `python3 -I tests/harness/run_m0.py --self-test` | 0 | 198 checks, 0 failures |
| Harness, default binary | `python3 -I tests/harness/run_m0.py --binary build/sutekh --report-dir <S>/h-default` | 0 | passed 20, failed 0, not_run 0, unavailable 2 (TX3, TX4 native-only) |
| Harness, native gcc binary | `... --binary <S>/native-gcc/sutekh --report-dir <S>/h-native` | 0 | passed 22, failed 0, not_run 0, unavailable 0 |
| TX5 and TX6 (both binaries) | from the two harness runs | 0 | TX5 75 checks passed; TX6 12 checks passed |
| Demo, default | `python3 tools/demo_zero_wait.py --binary build/sutekh --out <S>/demo-default` | 0 | |
| Demo, native | same with native gcc binary | 0 | |
| No-build probe (strace) | `python3 -I tests/e2e/no_build_probe.py --binary <S>/native-gcc/sutekh` | 0 | verdict pass; zero build-tool execs |
| Native spawn probe (strace) | `python3 -I tests/e2e/native_spawn_probe.py --binary <S>/native-gcc/sutekh` | 0 | 7 exec attempts, 7 successful, no build tool |
| Broker unit tests | `python3 -m unittest tests/broker/test_broker.py` | 0 | Ran 17 tests, OK |
| Platform lane | `python3 -I tests/platform/run_platform.py` | 1 | counts passed 17, failed 1 (full Windows graph, expected), not_run 4, unavailable 2; same as the first audit; evidence restored |
| Scheduler mutants (repo) | `sh tests/scheduler/run_mutants.sh <S>/sched-mut` | 0 | 6 of 6 killed (section 9) |
| Native qualification mutants (repo) | `sh tests/native/qualification/run_mutants.sh <S>/qual-mut` | 0 | 8 of 8 killed: H1, L1, L2 (specialize_admission); S1, S3, S6, W2 (native_scalar, native_avx2); P1 (native_scalar, native_versions, specialize_churn) |
| Scratch baseline (unmutated copy) | `ctest --test-dir <S>/mutbuild -j4` | 0 | 20/20 (29 s) |

Harness T00 is included in the default and native runs (both report passed, failed 0).

## 3. Blocker re-verification (first audit, `planning/audit/M1-audit.md`)

| # | First-audit blocker | Status now | Proof |
|---|---|---|---|
| 1 | Native build latency has no evidence | **RESOLVED** | `planning/evidence/m1/native-build-latency.json`. Recomputed from the raw samples: gcc Release clean-build median 12.7315 s (sorted samples, n = 7), p95 nearest-rank 16.5738 s; configure median 0.5203 s; no-op median 0.0347 s; incremental `su_native_asmjit.cpp` median 2.1317 s. All match the file and the status doc table (gcc Debug 9.87/10.45, clang Release 11.08/14.69, clang Debug 7.80/11.16). |
| 2 | Isolated throughput and end-to-end cost not evidenced | **PARTIAL** | `planning/evidence/m1/native-throughput.json` exists (21 samples, shared 4-vCPU VM, frequency not controlled). The JSON states it is not a qualification. Stress evidence: `stress-1m-current.json` (1,000,000 rows, passed) was not rerun here. |
| 3 | M1-04 scheduler not integrated | **PARTIAL** | `src/runtime/schedule.c:233` calls `su_jobs_run_system_chunked`; `su_sched_run_ticks_opt` is used at `worker.c:1048`. TX5 (75 checks, N = 1, 2, 4 against the reference) passes on both binaries; `test_tick_workers` 20/20 in ctest includes the lockstep. But the default is serial (`SUTEKH_TICK_WORKERS` default 1) and native kernels are not chunked. |
| 4 | C07 containment (no RLIMIT_AS, full environment, unbounded children) | **PARTIAL** | Probe `<S>/c07_probe.py` and `<S>/c07_disp.py` (section 7). Verified for active worker and disposable child: RLIMIT_AS soft = hard = 4294967296; environment names empty (SECRET_X absent); fds exactly [0, 1, 2]. Disposable child CPU soft 12 s, hard 13 s; active worker CPU unlimited. Not bounded: file size, open files (20000), network. |
| 5 | Stale-artifact mutants H1, L1, L2 survive | **RESOLVED** | Repo script `tests/native/qualification/run_mutants.sh` in this pass: H1, L1, L2 each killed (`ctest -R specialize_admission` 1 of 1 failed). |
| 6 | Workflow experiment: null tokens/integration, rework unverified | **PARTIAL** | Rates recomputed from rows: impl accepted_as_is 21, with integrator fix 7, partial 3 (31 completed); first-pass 21/31 = 0.677; rework 10/31 = 0.323; median agent wall 581.0 s (n = 36). `tokens.n = 0`, `tool_calls.n = 0`, `integration_latency_s.n = 0`. Status-doc figures do not match (section 6). |

Git-derived wall times (first audit's derivation method): M1-02a `a37e499e` 20:15:16 to last commit 20:52:37 = 2241 s (matches); M1-03 `a2bcf23a` 20:15:40 to 20:36:26 = 1246 s (matches).

## 4. Closure checklist (MILESTONES.md M1, line by line)

| Closure item | Verdict | Evidence |
|---|---|---|
| Opcode / tail / alignment / ABI differential corpus | **PASS on Linux** (gcc and clang) | native_scalar and native_avx2 suites green in both native builds (section 2) and in the scratch baseline. Win64 ABI: wine/cross only (section 8). |
| Capability fallback | **PASS (forced-disable hook)** | `tests/native/avx2/test_native_avx2.c` (forced `SU_E_CAPABILITY_DENIED` and scalar fallback). Real no-AVX2 host not run. |
| Stale artifact rejection | **PASS for H1, L1, L2** (reproduced in this pass) | Repo qualification script: H1, L1, L2 killed by `specialize_admission`. Layout generation check killed by W2 (first audit). Remaining qualification-script rows: section 9. |
| Safe retirement | **PARTIAL** | `specialize_inflight`, `specialize_churn`, `native_versions` green; but M07 (run-path inflight decrement removed) survives all 20 suites (section 5). |
| Worker-fault recovery | **PASS** | `recovery` suite green (native and default); TX2 green in both harness runs. |
| Real native build latency measurements | **PASS** | section 3, item 1. |
| Small multi-agent experiment with independent scopes and measured rejection/rework | **PARTIAL** | Rework rate recomputed (section 3 item 6). Integration latency, tokens and tool calls are null for every row; one row (closure B) is stale. |

## 5. Test lanes

| Lane | Result |
|---|---|
| Default ctest (release) | 10/10 pass, rc 0 |
| Native gcc ctest (Release) | 20/20 pass, rc 0 |
| Native clang ctest (Release) | 20/20 pass, rc 0 |
| Harness default binary | 20 pass, 0 fail, 2 unavailable (TX3/TX4 native-only), rc 0 |
| Harness native binary | 22 pass, 0 fail, 0 unavailable, rc 0 |
| Harness self-test | 198 checks, rc 0 |
| Broker | 17 tests, OK, rc 0 |
| Demo default / native | rc 0 / rc 0 |
| No-build probe (native) | pass, rc 0 |
| Native spawn probe (native) | pass, rc 0 |
| Platform lane | 17 pass, 1 fail (Windows graph), 4 not_run, 2 unavailable; rc 1 |
| Fuzz (ASan+UBSan native, 3 seeds) | 4,500 requests, 0 crashes, 0 sanitizer hits (section 8) |
| ASan ctest (full) | not run |
| TSan, clang sanitizers | not run |

## 6. Evidence and documentation spot-checks

| # | Claim (file) | Recomputed | Verdict |
|---|---|---|---|
| 1 | gcc Release clean build median 12.73 s (p95 16.57) | 12.7315 / 16.5738 from samples | match |
| 2 | gcc Debug 9.87 (10.45), clang Release 11.08 (14.69), clang Debug 7.80 (11.16) | 9.8679 / 10.4547, 11.0804 / 14.6866, 7.799 / 11.1597 | match |
| 3 | Stress step "median 193 ms" (`stress-1m-current.json` summary.step_ms.median = 193.203821) | from the 20 raw `step_ms` values: mean of the two middle values 192.521; nearest-rank 50% 191.838; 193.204 is neither (it is the upper middle value) | **mismatch** (small) |
| 4 | Stress step p95 200 ms | nearest-rank p95 = 200.246 | match |
| 5 | Stress world.create 797 ms; snapshot 203.7 ms, 28,092,684 bytes; restore 895.7 ms | 797.18; 203.67; 28,092,684; 895.68 | match |
| 6 | Stress worker VmHWM 178.5 MB after steps, 262.1 MB at end | 178,532 kB; 262,092 kB | match (kB; MB approximate) |
| 7 | Workflow first-pass acceptance 0.690 (20/29), rework 0.310 (9/29), status doc M1 evidence refresh | JSON: 21/31 = 0.677, rework 10/31 = 0.323 | **mismatch** (doc is stale) |
| 8 | Workflow agent wall median 581 s, p95 1,325 s, n = 36 | median 581.0 (match); p95 in JSON 1371, nearest-rank of the 36 values 1688, doc 1,325 | **mismatch** (the p95 method is not stated anywhere; the doc value matches neither) |
| 9 | Workflow derived wall times M1-02a 2241 s, M1-03 1246 s | from git timestamps | match |
| 10 | Self-test "150 checks" (status doc line 45; `planning/evidence/m0-harness-self-test.log`) | 198 checks now | **mismatch** (stale) |
| 11 | Closure-B handoff: TX5 75, TX6 12, self-test 198, default ctest 10/10, native 20/20 | 75, 12, 198, 10/10, 20/20 | match |
| 12 | Licence files untouched; one AsmJit line added to `THIRD_PARTY_NOTICES.md` | `git diff 20f2625 HEAD -- LICENSE LICENSES COMMERCIAL_LICENSE.md CLA.md NOTICE.md SECURITY.md` is empty; notices gain one zlib line | match |

## 7. C07 probes (containment)

Probe scripts in the scratch root: `c07_probe.py` (active worker), `c07_disp.py` (disposable child, sampled via `/proc/<pid>/task/*/children` while `world.specialize` runs). Binary: native gcc Release. The parent process had `SECRET_X` in its environment.

| Process | `Max address space` (soft / hard) | `Max cpu time` | Env names | `SECRET_X` | fds |
|---|---|---|---|---|---|
| Active worker (world.create) | 4294967296 / 4294967296 | unlimited | none | absent | [0, 1, 2] |
| Disposable worker (world.specialize scalar) | 4294967296 / 4294967296 | 12 / 13 s | none | absent | [0, 1, 2] |

Also seen for the active worker: `Max file size unlimited`, `Max open files 20000`. There is no seccomp filter and no network namespace; the worker runs as the same uid as the supervisor (not re-tested here; unchanged in source).

Other containment statements, source-checked: the scrubbed environment allows only `SU_TEST_`, `SUTEKH_`, and sanitizer `*_OPTIONS=` prefixes (`src/agent/su_proc.c:56-68`); the spool directory check does not verify owner or mode (`src/agent/su_spool.c:92`), first audit unchanged; `open(path, O_RDONLY)` without `O_CLOEXEC` still at `src/agent/su_path.c:124`.

## 8. Fuzz (ASan+UBSan native, 3 seeds)

Driver: `<S>/fuzz.py`, binary `<S>/asan/sutekh` (Debug, `-fsanitize=address,undefined -fno-sanitize-recover=all`), env `ASAN_OPTIONS=quarantine_size_mb=1:halt_on_error=1`, `SUTEKH_WORKER_AS_BYTES=0`. Odd-numbered sessions set `SUTEKH_TICK_WORKERS=2`. Per seed: 30 sessions, 50 requests each. Valid prelude (`engine.info`, `world.create`, `capsule.submit`), then random method choice; about 60% of requests have mutated fields (drop, null, wrong type, huge or negative ints, long strings, path escapes, unknown ids, stale revisions); about 2% of requests are invalid JSON lines.

Totals over seeds 1-3 (per method: sent / ok:true / ok:false):

| Method | Sent | ok:true | ok:false |
|---|---|---|---|
| engine.info | 361 | 312 | 49 |
| candidate.status | 394 | 166 | 228 |
| capsule.submit | 367 | 168 | 199 |
| candidate.evaluate | 353 | 159 | 194 |
| world.create | 418 | 189 | 229 |
| world.snapshot | 388 | 111 | 277 |
| world.close | 344 | 113 | 231 |
| world.step | 377 | 107 | 270 |
| world.inspect | 353 | 105 | 248 |
| world.specialize | 348 | 87 | 261 |
| world.restore | 376 | 25 | 351 |
| candidate.install | 354 | 10 | 344 |
| raw invalid line | 67 | 0 | 0 |
| **Total** | **4,500** | **1,552** | **2,948** |

Sessions 90; crashes 0; sanitizer reports 0; timeouts 0; EOF 0; bad JSON 0. Engine exit codes 0 on every session. Caveats: candidate.install succeeded 10 times in 354 (install is the least-covered path); world.restore 25 of 376; not coverage-guided; 3 seeds only.

## 9. Mutation testing

Method: scratch copy `<S>/mutsrc` (copy of the worktree without `build/`), native gcc Release build `<S>/mutbuild`. Driver `<S>/mutate.py`: per mutant one textual edit, anchor count asserted, build, then full ctest (or the TX6 harness for binary-level mutants), then the original file restored. Baseline `mutbuild` ctest: 20/20 rc 0.

| ID | Target | Mutation | Result | Caught by |
|---|---|---|---|---|
| M01 | AVX2 LT | sign-bit constant `vpslld 31` -> 30 | KILLED | specialize_admission, native_avx2 |
| M02 | AVX2 blend | `vpblendvb` operand order swapped | KILLED | specialize_admission, native_avx2 |
| M03 | AVX2 LT | removed the sign flip on operand b | KILLED | specialize_admission, native_avx2 |
| M04 | scalar SUB | `sub eax, sb` -> `sub eax, sa` | KILLED | specialize_admission, native_scalar, native_avx2 |
| M05 | scalar CONST | immediate truncated to 16 bits | KILLED | native_scalar, native_avx2 (and others) |
| M06 | digest compare | `t == tick` dropped from `digest_matches` (first attempt did not build under `-Werror`; rewritten with `(void)tick,`) | **SURVIVED** (ctest 20/20). Not proven equivalent: `wrev()` encodes only world id and revision, not the tick, so the tick check is defense-in-depth and no ordinary scenario separates it. | none |
| M07 | in-flight counter | first `a->inflight--` (run path, `native_versions.c:593`) replaced by `-= 0u` | **SURVIVED** (ctest 20/20) | none |
| M08 | effect conflicts | `(ra && wb)` term dropped (first attempt did not build under `-Werror`; rewritten as `(ra && 0 && wb)`) | **SURVIVED, EQUIVALENT.** `su_effects.c:362-372` visits every ordered pair (i, j) and sets both `conf[i][j]` and `conf[j][i]`; the swapped iteration yields `(wa && rb)` for the same conflict. | none |
| M09 | chunk merge status | lowest-failing-chunk status replaced by `SU_OK` | KILLED | scheduler, tick_workers |
| M10 | quarantine key | key compare on 8-byte prefix (`strncmp`) | **SURVIVED** (ctest 20/20) | none |
| M11 | env filter | `SUTEKH_` prefix dropped from allowlist | KILLED | recovery |
| M12 | RLIMIT_AS | 4 GiB -> 1 GiB (`sutekh_main.c`) | **SURVIVED** TX6 (12/12, rc 0) | - (TX6 asserts only <= 4 GiB) |
| M13 | spool default | 512 MiB -> 32 MiB | KILLED | TX6 (d), harness rc 1 |
| M14 | W^X final | `mprotect` to RW instead of RX | KILLED | specialize_churn, specialize_admission, native_fault, specialize_inflight |

Summary: 14 mutants; killed 9 (M01-M05, M09, M11, M13, M14); survived 5 (M06, M07, M10, M12, M08). M08 is equivalent (section above). M06 is an unproven survivor (defense-in-depth tick check). M07 and M10 are test gaps. M12 is a weaker-than-claimed bound that TX6 is designed to accept, not a regression detector.

Repo mutant scripts (not written by this auditor):
- `tests/scheduler/run_mutants.sh <S>/sched-mut`: rc 0; all six killed (last_writer_wins_on_failure 8 failures; chunk_boundary_off_by_one 2167; commit_on_failure 16; no_completion_gate 10790; no_conflict_check 6; no_cycle_check 6; each against test_scheduler, 852844 checks).
- `tests/native/qualification/run_mutants.sh <S>/qual-mut`: rc 0; all eight killed: H1, L1, L2 (`specialize_admission`); S1, S3, S6, W2 (`native_scalar|native_avx2`, 2 of 4 failed); P1 (`native_scalar|native_versions|specialize_churn`, 3 of 4 failed). The repo script does not include M07, M10, M12 or M06 (not covered by its list).

## 10. Defects, ranked

| # | Severity | Location | Finding |
|---|---|---|---|
| 1 | High (C07) | `src/platform/sutekh_main.c`, `src/agent/su_proc.c` | No bound on file size, open files, or network; active worker has no CPU bound. Constitution requires all four. |
| 2 | High (test gap) | `src/runtime/native_versions.c:593` (run path) | Retirement completing during a native run is not tested; M07 survives. |
| 3 | Medium | `planning/evidence/m1/agent_workflow.json`, `docs/IMPLEMENTATION_STATUS.md` | Status doc workflow numbers (0.690, 29 rows, p95 1,325 s) do not match the JSON (0.677, 31 rows, p95 1371 s). JSON p95 method unstated; nearest-rank gives 1688. |
| 4 | Medium | `src/runtime/jobs/su_jobs.c`, `schedule.c` | Chunked scheduler opt-in; default serial; native kernels unchunked. |
| 5 | Medium (test gap) | `src/agent/su_agent.c` (`digest_matches`, `quar_find`), `tests/harness/run_m0.py` TX6 | Digest tick check (M06, defense-in-depth, equivalence not proven), quarantine prefix key (M10) and RLIMIT_AS tightness (M12) are not tested. |
| 6 | Low | `planning/evidence/m1/stress-1m-current.json` | Recorded step median 193.204 ms is neither the mean-of-middle (192.521) nor nearest-rank (191.838). |
| 7 | Low (records) | `planning/tasks.json`, `planning/evidence/m1/agent_workflow.json`, `docs/IMPLEMENTATION_STATUS.md` | All M1 tasks still `planned`; closure-B row still `in_progress` (committed `ba8af44`); self-test 150 vs 198. |
| 8 | Low | `src/agent/su_path.c:124` | `open(path, O_RDONLY)` without `O_CLOEXEC` (first audit D5, unchanged). |
| 9 | Low | `src/agent/su_spool.c:92` | Spool directory: no owner or mode check (first audit, unchanged). |
| 10 | Low (coverage) | fuzz | `candidate.install` succeeded 10 of 354 requests; install path under-exercised by fuzz. |

No new memory-safety defect was found by the ASan+UBSan fuzz. No new defect was found by reading `su_proc.c` env scrub, fd closing, `sutekh_main.c` tick-option parsing or the `worker.c` tick handler beyond items 1, 8 and 9.

## 11. Honesty and hygiene

- Commit identity after `20f2625`: 93 commits, all author and committer `admiraly <249692417+admiraly@users.noreply.github.com>`.
- Co-Authored-By trailers after `20f2625`: 0 (`git log 20f2625..HEAD` body search).
- Licence files (`LICENSE`, `LICENSES`, `COMMERCIAL_LICENSE.md`, `CLA.md`, `NOTICE.md`, `SECURITY.md`): unchanged since `20f2625`.
- AsmJit: not tracked (`git ls-files third_party/asmjit` empty); `.gitignore:22` ignores `/third_party/asmjit/`; `third_party/asmjit.lock.json` is tracked; `THIRD_PARTY_NOTICES.md` gains one AsmJit line.
- Win64: not re-verified in this pass. First audit: wine and mingw cross only; status doc claims match.

## 12. Unrun checks and limits

- Full ASan ctest and TSan: not run in this pass (the ASan binary was used for the fuzz only).
- Clang sanitizers: not run.
- Windows native (MSVC / clang-cl): not run; no Windows host.
- Native build latency and AVX2 throughput: not re-measured.
- 1,000,000-row stress: not rerun (TX6 in the harness covers the 1M world; passed on both binaries).
- Win64 claims: not re-verified.
- Fuzz: 3 seeds, not coverage-guided; install path lightly covered.
- Repo mutant scripts (scheduler and native qualification): PENDING at the time of writing.
- M06 and M08: the first attempts did not build under `-Werror`; the rewritten mutants built and are reported in section 9 (M06 survived, M08 survived and is equivalent).
- Multi-agent experiment: not re-run (no A/B).
