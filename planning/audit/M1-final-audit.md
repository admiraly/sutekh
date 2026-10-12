# M1 final closure audit (third independent pass)

Auditor: independent re-auditor, worktree `agent-aa917e9146fc85178`, HEAD `8ff0035` at start (`git rev-list 20f2625..HEAD` = 104 commits). Scope: reproduction on HEAD, live containment probes, new mutants in scratch copies, bounded protocol fuzz, hygiene and evidence spot-checks. Scratch root (outside the repo): `/tmp/claude-0/-home-user-sutekh/82533bc6-e6ca-564a-ba43-39dd3091bafa/scratchpad/fa/`. No product, harness, status-doc or evidence file was edited. `tests/platform/run_platform.py` rewrote three tracked evidence files; they were restored with `git checkout --`.

## 1. Verdict

**M1 cannot close.**

Blocking items, exactly:

1. **C07 network denial is bypassable on this kernel (new, verified).** The seccomp filter denies only the socket-creating syscalls. io_uring is enabled here (`/proc/sys/kernel/io_uring_disabled` = 0). A scratch program installing the same filter gets EPERM from `socket()` and a live socket fd from io_uring `IORING_OP_SOCKET` (`fa/uring_bypass.c`). The `unshare(CLONE_NEWNET)` fallback runs only when seccomp fails, so it does not apply. Connect, accept and send via io_uring were not tested.
2. **C07 covers resources, not the whole worker.** Disposable workers have bounded CPU, address space, file size, fds and network (socket family only). The active worker has no CPU bound. Under the filter, ptrace, RWX mmap, openat of any path and execve of any binary all succeed (`fa/allowed_calls.c`). C07 requires "bounded CPU, memory, file, and network access"; network is not bounded (item 1), and the active worker's CPU is not bounded.
3. **Chunked tick scheduling is opt-in.** `SUTEKH_TICK_WORKERS` defaults to 1 (`src/platform/sutekh_main.c:104-116`). Chunked native kernels (`su_nv_run_system_chunked`) are reachable only through it. Whether opt-in satisfies "add disjoint chunk scheduling" is a product decision that is not recorded as made.
4. **Multi-agent experiment is incomplete.** `integration_latency_s`, `tokens` and `tool_calls` have `n = 0` (`planning/evidence/m1/agent_workflow.json`). `planning/tasks.json` lists M1-00..M1-05 as `submitted`, not closed.
5. **Test gaps that let mutants survive** (section 5): `M-X32`, `M-SOCKETPAIR`, `M-ARCH`, `M-DIGEST-SHA`, `M-DIGEST-REV`, `M-SPOOL-MODE` all pass ctest and TX6.

## 2. Reproduction (commands and return codes, HEAD 8ff0035)

| Check | Command | rc |
|---|---|---|
| AsmJit bootstrap | `python3 tools/bootstrap_asmjit.py` | 0 |
| Default Debug build | `cmake -S . -B fa/dev -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build fa/dev` | 0 |
| Default Release build | `cmake -S . -B fa/rel -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build fa/rel` | 0 |
| Native gcc Release build | `cmake -S . -B fa/ngcc -G Ninja -DSUTEKH_NATIVE=ON -DCMAKE_BUILD_TYPE=Release && cmake --build fa/ngcc` | 0 |
| Native clang Release build | `CC=clang CXX=clang++ cmake -S . -B fa/nclang -G Ninja -DSUTEKH_NATIVE=ON -DCMAKE_BUILD_TYPE=Release && cmake --build fa/nclang` | 0 |
| ASan+UBSan native Debug build | `cmake -S . -B fa/asan ... -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"` (+ linker flags) and build | 0 / 0 |
| ctest default (Debug) | `ctest --test-dir fa/dev -j4 --timeout 900` | 0 (11/11) |
| ctest default (Release) | `ctest --test-dir fa/rel -j4 --timeout 900` | 0 (11/11) |
| ctest native gcc | `ctest --test-dir fa/ngcc -j4 --timeout 900` | 0 (23/23) |
| ctest native clang | `ctest --test-dir fa/nclang -j4 --timeout 900` | 0 (23/23) |
| ctest ASan native | `SUTEKH_WORKER_AS_BYTES=0 ASAN_OPTIONS=quarantine_size_mb=1 ctest --test-dir fa/asan -j4 --timeout 900` | 0 (23/23) |
| ctest ASan without the env | same without `SUTEKH_WORKER_AS_BYTES=0` | 8 (containment: ASan cannot reserve shadow under RLIMIT_AS 4 GiB; environment issue, not a product defect) |
| Harness self-test | `python3 -I tests/harness/run_m0.py --binary fa/ngcc/sutekh --self-test` | 0 (213 checks, 0 failures) |
| Harness default binary | `python3 -I tests/harness/run_m0.py --binary fa/rel/sutekh --report-dir fa/h-default` | 0 (passed 20, failed 0, unavailable 2: TX3, TX4 native-only) |
| Harness native gcc binary | `python3 -I tests/harness/run_m0.py --binary fa/ngcc/sutekh --report-dir fa/h-native` | 0 (passed 22, failed 0, unavailable 0) |
| Demo default | `python3 tools/demo_zero_wait.py --binary fa/rel/sutekh --out fa/demo-default.json` | 0 |
| Demo native | `python3 tools/demo_zero_wait.py --binary fa/ngcc/sutekh --out fa/demo-native.json` | 0 |
| No-build probe (strace) | `python3 -I tests/e2e/no_build_probe.py --binary fa/ngcc/sutekh --out fa/nobuild.json` | 0 (pass; zero build-tool execs) |
| Native spawn probe (strace) | `python3 -I tests/e2e/native_spawn_probe.py --binary fa/ngcc/sutekh --out fa/spawn.json` | 0 (7 of 7 exec attempts, no build tool) |
| Broker unittest | `python3 -m unittest tests/broker/test_broker.py` | 0 (17 tests, OK) |
| Repo scheduler mutants | `sh tests/scheduler/run_mutants.sh fa/sched-mut` | 0 |
| Repo native qualification mutants | `sh tests/native/qualification/run_mutants.sh fa/qual-mut` | 0 (C4 and M12 reported killed in the log) |
| Platform lane | `python3 -I tests/platform/run_platform.py` | 1 (counts: passed 17, failed 1 Windows graph, not_run 4, unavailable 2; same as prior audits); evidence restored with `git checkout --`, rc 0 |
| Live containment probe, native gcc | `python3 -I fa/probe_live.py <wt> fa/ngcc/sutekh fa/live scalar` | 0 |
| Live containment probe, default | `python3 -I fa/probe_live.py <wt> fa/rel/sutekh fa/live default` | 0 (world.specialize: SU_E_UNSUPPORTED_METHOD, expected) |
| io_uring bypass experiment | `gcc -o fa/uring_bypass fa/uring_bypass.c && fa/uring_bypass` | 0 |
| Filter allow-list experiment | `gcc -o fa/allowed_calls fa/allowed_calls.c && fa/allowed_calls` | 0 |
| Mutants (new, 21) | `python3 -I fa/mutate2.py ...` | 0 (driver ran to DONE; 18 ran, 3 did not, see section 5) |
| Fuzz (ASan native, 3 seeds) | `python3 -I fa/fuzz2.py <wt> fa/asan/sutekh fa/fz <seed> 40 80` | 0, 0, 0 |

Not reproduced by this pass: the ASan ctest with `SUTEKH_TICK_WORKERS=2` for every test; TSan; clang sanitizers; the 1,000,000-row stress file (harness TX6 runs the 1M world and passed on both binaries); throughput measurements.

## 3. Live containment (native gcc, default binary, SECRET_X in supervisor environment)

Measured from `/proc/<pid>/{limits,status,environ,fd}` during `world.create`, `candidate.evaluate` and `world.specialize` (`fa/live/live-scalar.json`, `fa/live/live-default.json`).

| Process | Address space | CPU | File size | Open files | Core | Seccomp | NoNewPrivs | SECRET_X | fds |
|---|---|---|---|---|---|---|---|---|---|
| Active worker (native and default) | 4294967296 | unlimited | 268435456 | 1024 | 0 | 2 | 1 | absent | [0,1,2] |
| Disposable child during candidate.evaluate (native) | 4294967296 | 12 | 268435456 | 64 | 0 | 2 | 1 | absent | [0,1,2] |
| Disposable child during world.specialize (native) | 4294967296 | 12 | 268435456 | 64 | 0 | 2 | 1 | absent | [0,1,2] |

Transient: a pre-exec fork copy of the supervisor (cmdline `serve`) was sampled holding SECRET_X and extra fds before `execve`. Seccomp and rlimits are applied inside that window; exec replaces the environment. This is not a worker leak.

Seam for a seccomp-denied `socket()` inside the worker: `tests/recovery/test_containment.c` runs a probe child under the same filter and asserts socket EPERM, `Seccomp: 2`, EFBIG at 1 MiB and EMFILE at 64. ctest `containment` passed on native and default builds. TX6 (harness) asserts the limits from outside.

## 4. Re-audit blockers (`planning/audit/M1-reaudit.md`) at HEAD

| # | Blocker | Status | Evidence |
|---|---|---|---|
| 1 | C07 containment | **PARTIAL, worse than stated**: resource bounds applied to disposable and active worker; network denial bypassable by io_uring; ptrace, exec, arbitrary openat and RWX mmap allowed | sections 3 and 6 |
| 2 | Chunked scheduler | **PARTIAL**: wired (`src/runtime/schedule.c:222`, `native_versions.c:695`); default serial; decision open | `sutekh_main.c:104-116` |
| 3 | Throughput evidence | **PARTIAL**: observation only; not rerun | `planning/evidence/m1/native-throughput.json` |
| 4 | Safe-retirement test gap (M07) | **Not reproduced by this pass.** Closure C reports `safe_retire` kills M07, M10, R1, R2. Not rerun here | closure-C handoff |
| 5 | Multi-agent experiment | **PARTIAL**: integration, tokens and tool calls null | `agent_workflow.json` |

Re-audit items at HEAD: M06 rerun was not possible (see section 5). M12 is killed by repo qualification script (`M12: killed (harness TX6 failed...)`) and by TX6 at binary level.

## 5. Mutation testing

### 5a. New mutants (scratch copy of HEAD, native gcc Release, edits outside the repo)

Full ctest (23 tests) per mutant; binary-level mutants also ran harness TX6 when ctest passed.

| ID | Mutant | Result | Caught by |
|---|---|---|---|
| M-ARCH | seccomp arch kill replaced by allow | SURVIVED (ctest 0, TX6 0). Not equivalent on non-x86 hosts; no test exercises a foreign arch | none |
| M-X32 | x32 threshold `0x40000000` -> `0x80000000` (x32 socket numbers not killed) | SURVIVED (ctest 0, TX6 0). Real gap: no test calls an x32 syscall number | none |
| M-SOCKETPAIR | socketpair (53) no longer denied | SURVIVED (ctest 0, TX6 0) | none |
| M-NOFILE-DISP | disposable RLIMIT_NOFILE not applied | KILLED | TX6 |
| M-ENV-WIDEN | env filter passes every variable | KILLED | TX6 |
| M-CORE | RLIMIT_CORE not zero | KILLED | ctest `containment` |
| M-ACTIVE-NET | active worker not network-denied | KILLED | TX6 |
| M-FSIZE-DEF | default RLIMIT_FSIZE removed | KILLED | TX6 |
| M-CHUNK-BASE | chunk base +1 row for k>=1 | KILLED | ctest `native_chunked` |
| M-CHUNK-LEN | chunk drops last row | KILLED | ctest `native_chunked` |
| M-AVX2-TAIL | AVX2 scalar tail skipped | KILLED | `native_chunked`, `native_avx2`, `native_versions`, `native_fault`, `specialize_churn`, `specialize_admission` |
| M-SCALAR-CONST | scalar CONST low bit flipped | KILLED | `native_scalar`, `native_avx2`, `specialize_churn`, `specialize_admission`, `specialize_inflight` |
| M-AVX2-CONST | AVX2 CONST low bit flipped | KILLED | `specialize_churn`, `specialize_admission`, others |
| M-INSTALL-TICK | install ignores allowed_effective_tick | KILLED | ctest `reload` |
| M-INSTALL-EVID | install without passing evaluation | KILLED | ctest `reload` |
| M-DIGEST-SHA | digest SHA compare dropped | SURVIVED (ctest 0, TX6 0). Gap: no test restores a checkpoint with a wrong SHA | none |
| M-DIGEST-REV | digest revision compare dropped | SURVIVED (ctest 0, TX6 0). Gap | none |
| M-SPOOL-MODE | spool group/world-accessibility check removed | SURVIVED (ctest 0, TX6 0). Gap: no test creates a loose spool directory | none |
| M-INSTALL-REV | install ignores stale expected revision | NOT RUN (anchor occurs twice; driver refused to edit) | n/a |
| M-DIGEST-TICK-RERUN | closure-C M06 re-run (digest tick check dropped) | NOT RUN (build failed under `-Werror`: unused parameter `tick`) | n/a |
| M-QUAR-FIND | quarantine lookup always misses | NOT RUN (build failed under `-Werror`: null argument to `memset`/`snprintf` path) | n/a |

Result: 18 ran, 11 killed, 6 survived ctest and TX6. Survivors are not shown equivalent. M-SPOOL-MODE and M-ARCH are not equivalent. The M06 tick check in closure C is argued equivalent (snapshot SHA covers the tick header) but this pass could not run the re-run. Mutants added to repo scripts: none.

### 5b. Closure-C claims (M07, M10, R1, R2, M12) and repo scripts

Not reproduced here, except: repo native qualification script reports C4 and M12 killed; repo scheduler script rc 0. M07, M10, M06 rerun: not done.

## 6. Containment coverage statement (C07)

What the current code bounds for each child (`src/agent/su_agent.c:443-452`, `src/agent/su_proc.c`):
- Address space: RLIMIT_AS 4 GiB (soft = hard). Bounded for all children.
- File size: RLIMIT_FSIZE 256 MiB with SIGXFSZ ignored (writes fail EFBIG). Bounded for all children.
- Open files: RLIMIT_NOFILE 1024 (active), 64 (disposable). Bounded.
- Core dumps: RLIMIT_CORE 0. Bounded (ctest `containment` checks it).
- CPU: disposable child only (`timeout/1000 + 2` s soft, +1 hard). **Active worker unbounded.**
- Network: seccomp denies socket, connect, accept, bind, listen, socketpair, accept4 with EPERM; other arch or x32 kills. **Bypassable through io_uring (IORING_OP_SOCKET verified).** Not verified: IORING_OP_CONNECT/ACCEPT/SEND.
- Environment: allow-list of `SU_TEST_`, `SUTEKH_`, sanitizer `*_OPTIONS=`. Verified: SECRET_X absent.
- File system: unrestricted for the same uid. openat of arbitrary paths allowed (verified).
- Exec: unrestricted. execve of arbitrary binaries allowed (verified).
- ptrace: allowed (verified).
- Memory mappings: mmap PROT_EXEC and RWX allowed (verified).
- Other syscalls: default allow.

Conclusion: C07 "bounded CPU, memory, file, and network access" holds for disposable workers on resource limits, and holds for network only against the socket syscall family. It is not met for network against io_uring, and not met for CPU on the active worker. C07's containment language "not by itself a complete hostile-code sandbox" is accurate; the status docs should say so explicitly.

## 7. Fuzz (ASan+UBSan native, 3 seeds, 120 sessions)

Driver `fa/fuzz2.py`: valid world, capsule and candidate setup; then per request a valid flow (submit, evaluate, install with current revision and tick; step; snapshot and restore; specialize) or one mutated field from a valid template; 3 percent invalid JSON lines; odd sessions use `SUTEKH_TICK_WORKERS=2`.

| Method | Sent | ok | err |
|---|---|---|---|
| candidate.evaluate | 2113 | 1600 | 513 |
| candidate.install | 1973 | 1369 | 604 |
| candidate.status | 500 | 0 | 500 |
| capsule.submit | 2093 | 1600 | 493 |
| engine.info | 490 | 490 | 0 |
| world.close | 528 | 0 | 528 |
| world.create | 120 | 120 | 0 |
| world.inspect | 477 | 102 | 375 |
| world.restore | 1180 | 658 | 522 |
| world.snapshot | 1191 | 658 | 533 |
| world.specialize | 1302 | 717 | 585 |
| world.step | 1442 | 943 | 499 |
| raw invalid line | 169 | 0 | 0 |

Totals: 13,578 requests, 120 sessions, crashes 0, sanitizer hits 0, timeouts 0, EOF 0, bad JSON 0. Coverage gaps: `world.close` and `candidate.status` never succeeded (every instance was mutated, and the valid parameter shape of `candidate.status` was not checked); not coverage-guided. Install succeeded 1,369 of 1,973 times, against 10 of 354 in the prior re-audit.

## 8. Defects, ranked

| # | Severity | Location | Finding |
|---|---|---|---|
| 1 | High (C07) | `src/agent/su_proc.c` seccomp filter | Network denial bypassable via io_uring (IORING_OP_SOCKET verified). Fix: deny `io_uring_setup`/`io_uring_enter`/`io_uring_register` in the filter, or gate with `io_uring_disabled`. |
| 2 | High (C07) | `src/agent/su_agent.c:443-452` | Active worker has no CPU bound; by design for long simulations, but C07 says bounded CPU. Needs a decision and a documented exception or bound. |
| 3 | High (C07 scope) | `src/agent/su_proc.c` | ptrace, execve, arbitrary openat and RWX mmap allowed for children. Acceptable only if documented as out of scope; the status doc does not say so. |
| 4 | Medium | `src/platform/sutekh_main.c:104-116` | Chunked tick scheduling opt-in; default serial. Product decision not recorded. |
| 5 | Medium (test gap) | seccomp arch, x32 and socketpair (M-ARCH, M-X32, M-SOCKETPAIR) | Filter branches untested. |
| 6 | Medium (test gap) | `su_agent.c` `digest_matches` (M-DIGEST-SHA, M-DIGEST-REV) | Checkpoint digest comparison untested. |
| 7 | Medium (test gap) | `su_spool.c:98` (M-SPOOL-MODE) | Spool mode check untested. |
| 8 | Medium (multi-agent) | `planning/evidence/m1/agent_workflow.json`, `planning/tasks.json` | Integration latency, tokens, tool calls null; tasks still `submitted`. |
| 9 | Low (docs stale) | `docs/IMPLEMENTATION_STATUS.md:45` | Self-test "150 checks"; current binary: 213. |
| 10 | Low (docs stale) | `docs/IMPLEMENTATION_STATUS.md:342` | "native kernels are not row-sliced under tick workers"; closure C added `su_nv_run_system_chunked`. |
| 11 | Low (docs) | `docs/IMPLEMENTATION_STATUS.md:335` | Workflow p95 "1,582 s": nearest-rank over n=39 is 1,688 s; linear interpolation 1,592.6 s. Method not stated. |
| 12 | Low (evidence) | `planning/evidence/m1/stress-1m-current.json` | `summary.step_ms.median` 193.204 ms; recomputed from the 20 raw samples: 192.521 ms. |
| 13 | Low (evidence) | `su_path.c:124` | `open()` without `O_CLOEXEC` (first audit D5, unchanged). |
| 14 | Retracted | `su_spool.c:92` | Re-audit defect 9 (no owner or mode check) is stale: `su_spool.c:98` checks owner and mode. |
| 15 | Coverage | fuzz driver | `world.close` and `candidate.status` never succeeded. |

## 9. Test lanes

| Lane | Result | rc |
|---|---|---|
| Default ctest (Debug / Release) | 11/11 / 11/11 | 0 / 0 |
| Native gcc ctest | 23/23 | 0 |
| Native clang ctest | 23/23 | 0 |
| ASan native ctest (with documented env) | 23/23 | 0 |
| Harness self-test | 213 checks, 0 failures | 0 |
| Harness default binary | 20 pass, 0 fail, 2 unavailable | 0 |
| Harness native binary | 22 pass, 0 fail | 0 |
| Demo default / native | pass | 0 / 0 |
| No-build probe | pass | 0 |
| Native spawn probe | pass | 0 |
| Broker | 17 tests OK | 0 |
| Repo scheduler mutants | killed | 0 |
| Repo native qualification mutants | killed (incl. C4, M12) | 0 |
| Platform runner | 17 pass, 1 fail (Windows graph), 4 not_run, 2 unavailable | 1 |
| Fuzz (ASan native, 3 seeds) | 13,578 requests, 0 crashes, 0 sanitizer hits | 0 |
| TSan, clang sanitizers, Windows native | not run | - |

## 10. Closure checklist (MILESTONES.md M1, line by line)

| Item | Verdict | Evidence |
|---|---|---|
| Native adapter, scalar lowering, AVX2 | PASS on Linux (gcc, clang) | ctest native, harness TX5 |
| Exact differential corpus (opcode, tail, alignment, ABI) | PASS on Linux; Win64 only under wine | native suites; new AVX2 tail and scalar CONST mutants killed |
| Safe native code lifetime | PARTIAL: closure-C tests exist; M07, M10, M06 not reproduced by this pass | `safe_retire` per closure C |
| Code-generation cache | not separately evidenced in this pass | not checked |
| Bounded validation/build queues | not separately evidenced in this pass | broker unittest only |
| Disjoint chunk scheduling after effects tests | PARTIAL: opt-in | section 4 |
| Smoke-test Windows ABI/platform paths | PARTIAL: wine only; full graph fails to build for Windows | platform runner rc 1 |
| Native optional; old code live during specialize | PASS | demo, harness TX2/TX3 |
| Reference VM never removed | PASS | harness |
| Capability fallback | PASS (forced hook); real no-AVX2 host not run | native_avx2 |
| Stale artifact rejection | PASS for H1, L1, L2 (repo script) | qualification script |
| Worker-fault recovery | PASS | recovery suite, TX2 |
| Real native build latency | PASS on this VM; reference-machine condition not met | `planning/evidence/m1/native-build-latency.json` |
| Multi-agent experiment with measured rejection and rework | PARTIAL: rework rate reproduced (23/33 first pass, rework 0.303); integration latency, tokens, tool calls null | `agent_workflow.json` |
| Containment (C07) | PARTIAL: network bypassable; active worker CPU unbounded | section 6 |

## 11. Hygiene and honesty

- Commit identity: 104 commits since `20f2625`, all `admiraly <249692417+admiraly@users.noreply.github.com>` (author and committer).
- Co-Authored-By trailers in commits since `20f2625`: 0.
- Licence files (`LICENSE`, `LICENSES`, `COMMERCIAL_LICENSE.md`, `CLA.md`, `NOTICE.md`, `SECURITY.md`): unchanged since `20f2625` (`git diff --stat` empty).
- AsmJit: not tracked (`git ls-files third_party` shows only `asmjit.lock.json`); `.gitignore:22` ignores `/third_party/asmjit/`.
- `THIRD_PARTY_NOTICES.md` gains one AsmJit line (zlib, commit `0d7f010`); no other notice change.
- Win64 claims: wine and mingw cross only; `docs/IMPLEMENTATION_STATUS.md` labels them so. Windows-native not run.
- Spot-checks (9 numbers):
  1. Workflow first-pass 23/33 = 0.697: match.
  2. Workflow rework 0.303: match.
  3. Workflow median agent wall 611 s (n = 39): match.
  4. Workflow p95 1,582 s: **mismatch** (1,688 nearest-rank; 1,592.6 linear).
  5. Stress step median 193.204 ms: **mismatch** (192.521 ms recomputed).
  6. Stress step p95 200.246 ms: match (nearest-rank).
  7. Default ctest 11/11 and native 23/23 (closure-C handoff): match.
  8. Self-test 213 checks (binary) vs docs "150": **docs stale**.
  9. Stress world.create 797 ms, restore 895.7 ms, snapshot 28,092,684 bytes: match the file (not recomputed).

## 12. Unrun checks and limits

- TSan, clang sanitizers, full ASan ctest with `SUTEKH_TICK_WORKERS=2`: not run.
- Windows native (MSVC, clang-cl): not run; no Windows host.
- Throughput and native build latency: not re-measured.
- 1,000,000-row stress file: not rerun; TX6 covers it.
- Re-run of M06, M07, M10, M12 on this tree: not done (M12 seen in the repo script log only).
- Mutants M-INSTALL-REV, M-DIGEST-TICK-RERUN, M-QUAR-FIND: not run (anchor count or `-Werror` failure).
- io_uring connect, accept, send: not tested; only IORING_OP_SOCKET.
- Live io_uring bypass used a copy of the filter program, not the product worker; the product worker was not made to run io_uring code.
- Fuzz: 3 seeds, not coverage-guided; `world.close` and `candidate.status` never succeeded.
- Multi-agent experiment: not re-run.
