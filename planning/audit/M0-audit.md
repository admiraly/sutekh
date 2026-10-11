# M0 independent closure audit (M0-11)

Auditor: independent closure auditor, worktree `agent-ac87c1936bea31207`, HEAD `22e28bd` at start of audit.
Scope: read-only review and reproduction. No engine, harness, or doc source was edited. Mutations ran only in scratch copies outside the repository. Raw logs are under the session scratchpad (`/tmp/claude-0/-home-user-sutekh/82533bc6-e6ca-564a-ba43-39dd3091bafa/scratchpad/`), not in the repo.

## Verdict

**M0 cannot close yet.** Everything the harness reports as passed reproduces from a clean build in both profiles, and the no-build demo and probe pass. The gaps are in the closure criteria, not in the basic 16/16 result.

Blockers, exactly:

1. **Timing targets are not compared.** MILESTONES closure requires T00-T15 "with explicit timing target comparison". T13 in `tests/harness/run_m0.py` (around line 1439-1475) records latency and states "No latency target is declared ... comparison ... is not claimed here", yet reports `passed`. The status doc compares against proposed targets, and 5 of its 8 target rows are "not comparable".
2. **T11 does not meet TEST_SPEC.** TEST_SPEC requires 1,000 successful, failed and reverted revisions. The harness default is `--reload-iterations 200` (line ~1654). The loop installs only valid candidates, and failed candidates are only rejected at submission, never installed.
3. **T04 does not cover the required matrix.** TEST_SPEC requires "oversize input rejected" and the matrix is incomplete (no oversize IR, no `SU_E_PARSE`, no `SU_E_MISSING_STORE`, no `SU_E_SCHEMA_MISMATCH`). Mutation M08 (max_rows ignored) passes T04.
4. **Stored evidence and the reproduction guide are stale.** `docs/IMPLEMENTATION_STATUS.md` reports T04 unavailable, a release build that fails, old harness and binary hashes, and omits the demo and probe commands from its reproduction section.

## Reproduction (clean builds, HEAD 22e28bd)

| Check | Result |
|---|---|
| `rm -rf build && python3 tools/dev.py build --profile dev` | rc 0 |
| `rm -rf build && python3 tools/dev.py build --profile release` | rc 0 |
| `ctest --output-on-failure` (dev, release, ASan) | rc 0, 7/7 each |
| `python3 -I tests/harness/run_m0.py --self-test` | rc 0, 82 checks, 0 failures |
| `python3 -I tests/harness/run_m0.py --binary build/sutekh --report-dir <scratch>/audit/report_dev` (dev) | rc 0, T00-T15 all passed |
| same, release binary | rc 0, T00-T15 all passed |
| same, `--no-build` (release) | rc 0 |
| `python3 tools/demo_zero_wait.py` (dev, release) | rc 0, status passed |
| `python3 -I tests/e2e/no_build_probe.py` (dev, release) | rc 0, verdict pass, 0 build-tool execs in run, positive control detected |

Hashes (dev build at HEAD 22e28bd): `build/sutekh` sha256 `184ab82b839cc793dd786caa1b22f85852c0ab70978081bf7df185cd2f7b3a6e`; `run_m0.py` sha256 `2263a43d5dc3cea6c6eb607bcb8967e397d3aab949f0b551a2e2e899a43b48b8`; `reference_eval.py` sha256 `b9919314fd9ee2526d163e27f5a698bf8e086d3b9497e0824fd421790eec04dc`.

## Test table (T00-T15)

Status = result reproduced now. Mutation = scratch-copy mutant (M-ids in the mutation section); "red" means the targeted test failed.

| ID | Status (reproduced) | Adversarial assessment | Mutation result |
|---|---|---|---|
| T00 | passed | Runs incremental `dev.py build`, no clean step. A stale binary from older source would pass. Only checks that the binary exists and `--version` matches headers. Weak. | not mutated |
| T01 | passed | Expected values come from `reference_eval.py`, cross-checked against the fixture before use. Strong. | M01 (add->sub) red |
| T02 | passed | Counts 0,1,7,8,9,1024 with two tick checks each. Strong for row handling. | M04 (skip last row) red |
| T03 | passed | Six edge cases with an independent reference. Wrap, lt, eq, mul and sub edges covered. | M02 (signed lt) red, M03 (saturating mul) red |
| T04 | passed (14/14 cases) | Each negative is exact-code and no-state-change checked. Coverage gap: no oversize IR bytes, no `SU_E_PARSE`, `SU_E_MISSING_STORE`, `SU_E_SCHEMA_MISMATCH`. max_rows is not exercised. | M08 (ignore max_rows) **survives T04** (caught by T09 only) |
| T05 | passed | Behaviour change checked tick by tick against the reference. PID preservation uses the engine's own `worker_pid` and `engine.info` pid, not OS state. The "manifest changes at install" check passes trivially because install-history fields always change. | M01 red, M12 (install reports success, no swap) red |
| T06 | passed | Failed-evaluation install refused with exact code, state unchanged, next tick still v1. Good. | M06 (evidence gate off) red |
| T07 | passed | Stale revision refused and state unchanged. Control install at current revision. Good. | M05 (stale checks off) red |
| T08 | passed | Snapshot and restore checked against the reference. Two checks are optional and pass silently when absent: `snapshot_format` and `restored_from` (`if key in res`). | M11 (restore writes column 0 everywhere) red |
| T09 | passed | Runtime part is real (fixture B exceeds `max_rows`), and a column and tick check after failure. Docstring still says the runtime part is "unavailable"; the code runs it. | M07 (publish after failure) red, M08 red |
| T10 | passed (first attempt) | Kill issued from a thread watching children of the supervisor. Active-worker and supervisor survival checked through engine-reported pids. Not mutation-tested. | not mutated (gap) |
| T11 | passed at 200 iterations | Compares state per iteration against the reference. RSS bound (8 MiB after 10 warm-ups) is allocator-dependent: under ASan default options it fails with about 87 MB growth, but with `quarantine_size_mb=1` it passes and LeakSanitizer reports nothing. Silent cap at 300 recorded checks. | M05 red, M09 (256 KiB leak per install) red, M12 red |
| T12 | passed | Detection is PATH shims plus `/proc` child polling. Blind to a `cc` invoked by absolute path. | M13 (install runs `/usr/bin/cc`) **survives T12**; the strace probe catches it (verdict fail, rc 1) |
| T13 | passed (measurement only) | No target comparison in the harness, so it cannot fail on latency. Only 30 samples; "queue", "boundary" and "publish" are not separated, as the harness itself states. Fails the closure criterion. | M06 not applicable to T13 |
| T14 | passed | Two fresh sessions compared canonically and against the reference. Only two runs. Not mutation-tested. | not mutated |
| T15 | passed (7 cases) | Oversize, invalid UTF-8, duplicate key, bad protocol, unknown method, traversal, absolute path. Session-survival checked after each. | M10 (1 MiB limit raised to 4 MiB) red, M14 (containment off) red, M15 (dotdot allowed) red |

## Mutation results

Control (unmodified copy) is green on all targeted tests. Each mutant is a single scripted edit with an anchor count assertion, built with `tools/dev.py build --profile dev` in its own copy, then run with `--only`.

| ID | Edit (in scratch copy) | Target tests | Result |
|---|---|---|---|
| M01 | `src/vm/su_vm.c` add_u32 computes subtraction | T01,T03,T05 | red (T01,T03,T05) |
| M02 | `su_vm.c` lt_u32 compares as signed | T03 | red |
| M03 | `su_vm.c` mul_u32 saturates | T03 | red |
| M04 | `su_vm.c` row loop skips last row | T02 | red |
| M05 | `src/runtime/reload.c` both stale-revision checks disabled | T07,T11 | red |
| M06 | `reload.c` evidence gate disabled | T06,T13 | red (T06); T13 not designed to catch it |
| M07 | `src/runtime/schedule.c` publish after a failed system | T09 | red |
| M08 | `su_vm.c` ignore `limits.max_rows` | T09,T04 | red (T09); **T04 green** |
| M09 | `reload.c` leak 256 KiB per successful install | T11 | red |
| M10 | `include/sutekh/ir.h` protocol limit to 4 MiB | T15 | red |
| M11 | `src/world/snapshot.c` restore copies column 0 into all columns | T08 | red |
| M12 | `reload.c` install skips the swap | T05,T11 | red |
| M13 | `reload.c` install runs `/usr/bin/cc --version` (absolute path) | T12 | **green** (blind spot); strace probe red (rc 1) |
| M14 | `src/agent/su_path.c` containment check disabled | T15,T04 | red (T15) |
| M15 | `su_path.c` dotdot check disabled | T15,T04 | red (T15, wrong code) |

Survivors (harness blind spots): M08 for T04, M13 for T12. Neither survives the whole suite: M08 is caught by T09, and M13 is caught by the strace probe. M15's red is a code mismatch (`SU_E_IO` instead of `SU_E_CAPABILITY_DENIED`), not a proven escape.

## Closure-evidence checklist (MILESTONES.md M0)

| Item | Result | Basis |
|---|---|---|
| T00-T15 genuine results with explicit timing target comparison | **FAIL** | 16/16 reproduce, but T13 compares no target and the status doc's target table is mostly "not comparable" against proposed targets (C10: proposed budgets are not measurements). |
| Instrumented capsule edit invoking no native compiler, assembler or linker | PASS (conditional) | strace probe: 0 build-tool execs in the whole run and in the edit window; positive control detected. T12 alone is insufficient (M13). |
| Preserved PIDs and state for code-only swaps | PASS | T05 and T11 check engine-reported pids; `demo_zero_wait.py` also checks the worker process start time from the OS. |
| Rejection without active-state change | PASS | T04, T06, T07, T11 check tick, revision, columns and manifest unchanged. |
| Stored commands for a new developer | PARTIAL | `docs/IMPLEMENTATION_STATUS.md` section 8 omits the demo and probe commands, and its release command is documented as failing (it now passes). The demo and probe commands appear only in `planning/handoffs/2026-10-11-m0-09.md` and `planning/tasks.json`. |

Additional MILESTONES and TEST_SPEC conditions: T11 at 1,000 revisions (**FAIL**, see blocker 2); T04 negative matrix including oversize input (**FAIL**, blocker 3).

## Rule compliance (AGENTS.md, ENGINE_CONSTITUTION.md)

- **No fake backends**: grep for stub, fake, placeholder and mock found only the intended `SU_E_UNSUPPORTED_METHOD` replies and an unrelated fixture placeholder. PASS.
- **No unconditional PASS**: the runner marks a test passed whenever its body returns without an exception and has no minimum check count, so a body with zero checks would pass. The demo and probe derive status from real comparisons. T13 is passed without any target, so it cannot fail on latency. PARTIAL.
- **No fabricated benchmarks**: spot-checked 8 figures in `docs/IMPLEMENTATION_STATUS.md` against `planning/evidence/m0-latency-dev-raw.json` and `...-release-waiver-raw.json`, all reproduced: dev warm p95 submit 0.633, evaluate 9.023, install RPC 1.014, edit-to-first-effect 11.759 ms (720 samples, nearest-rank); release evaluate 8.564 and install 0.765 ms; cold first-edit p95 11.029 ms (n=60); failed-candidate published count 0 of 120; 1M-row fixture 27,833,861 bytes; native build 26 compile and 15 link steps. The doc's median 9.757 versus my 9.758 is a rounding difference in the median method. PASS for numbers; the doc itself is stale (see defects).
- **Commit identity**: recent commits are by `admiraly <249692417+admiraly@users.noreply.github.com>`. No trailers were added by this audit. PASS for this audit's commit.
- **License files untouched**: `LICENSE`, `COMMERCIAL_LICENSE.md`, `CLA.md`, `NOTICE.md`, `LICENSES`, `THIRD_PARTY_NOTICES.md` last changed in `20f2625` and `c0b84f6`, both ancestors of M0-00 (`e1f63c7`). PASS.
- **No vendored or downloaded code**: `git ls-files` matching vendor, third_party, external or deps lists only `THIRD_PARTY_NOTICES.md`. No downloads in tools, bench, tests or src. PASS.

## Memory-safety review and fuzzing

Read: `src/ir/su_json.c`, `src/ir/su_lir.c`, `src/agent/su_proto.c`, `src/agent/su_path.c`, `src/agent/su_proc.c`, `src/runtime/worker.c` (allocation and parsing sites), `src/world/snapshot.c` (decode and restore). Specific checks:

- JSON: surrogate pairs and lone surrogates handled (`su_json.c:336-356`); NUL via `\u0000` rejected (`:359-361`); depth checked (`:217`, `:673`); member lookup is length-based, so embedded NULs cannot truncate keys. No defect found.
- Protocol buffers: growth capped at 1 MiB (`su_proto.c:38-60`, `:190-205`). No defect found.
- Paths: `strcpy` at `su_path.c:29` and `:83` is preceded by capacity checks. No defect found.
- Snapshot: column allocations are bounded by row and field limits (`snapshot.c:194`, `:919`). No defect found.

Concrete defects found by reading (both low severity; neither triggered):

- `src/agent/su_proc.c:94-95`: the child's `F_DUPFD` copies (fd 10 and above) are not `FD_CLOEXEC`, so each worker keeps extra copies of its pipe ends. Triggering needs a grandchild process, which the M0 worker does not spawn. Not demonstrated.
- `src/agent/su_proc.c:139`: `malloc(req_len + 1u)` is unchecked against wraparound. Safe only if all callers bound `req_len`; I did not verify the callers.

Fuzzing (ASan + UBSan with `-fno-sanitize-recover=undefined`, Debug, `build` in scratch):

- ctest under sanitizers: 7/7 rc 0, no reports.
- Harness under sanitizers: 15 passed, T11 failed on the RSS bound (allocator quarantine, see T11). With `quarantine_size_mb=1`, T11 passes and LeakSanitizer is silent.
- Seeded mutation fuzz (`scratchpad/fuzz/fuzz.py`): seeds 1, 2 and 3, totalling 5,700 requests (capsule and LIR bytes through `capsule.submit`, world fixtures through `world.create`, and mutated protocol lines). Results: 0 sanitizer reports, 0 signal deaths, 0 missing replies. Caveat: most replies are errors (seed 1: 11 of 900 succeeded), so the deeper install and step paths are under-exercised. No memory-safety defect was triggered.

## Defects, ranked

1. **High (closure)**: T13 compares no timing target and reports passed. Blocks closure.
2. **High (closure)**: T11 runs 200 iterations, not 1,000, and never installs a failed candidate or revert. Blocks closure.
3. **High (harness)**: T12 does not detect a compiler invoked by absolute path (M13 green). The strace probe catches it, so the claim holds only with the probe.
4. **Medium**: T04 misses oversize, parse, missing-store and schema-mismatch cases; M08 passes T04.
5. **Medium**: runner passes zero-check bodies and records no minimum; T08 optional checks and T13 are effectively unconditional.
6. **Medium**: PID and state preservation in T05, T10 and T12 rely on the engine's self-reported pids; only the demo checks OS process start times.
7. **Medium**: T00 runs an incremental build with no clean; a stale binary passes.
8. **Medium**: T11 RSS bound fails under the default ASan allocator (false red).
9. **Medium**: recorded checks are capped at 300 (`MAX_RECORDED_CHECKS`), `checks_dropped` is counted but never written to the report, so truncation is silent.
10. **Medium (docs)**: `docs/IMPLEMENTATION_STATUS.md` is stale and incomplete: T04 "unavailable", release "exits 1", harness hash `43f3e71e...` and binary hash `f00aa1fe...` do not match HEAD, reproduction section lacks demo and probe. T09 docstring says the runtime part is unavailable.
11. **Low**: `su_proc.c:94-95` non-CLOEXEC duplicated pipe ends in the worker.
12. **Low**: `su_proc.c:139` unchecked `req_len + 1u`; caller bound unverified.

## Unrun checks

- Clang build, TSan, MSan, and sanitizers on the release profile.
- `python3 -I tests/harness/run_m0.py --reload-iterations 1000` (spec count).
- Harness T10 and T14 under sanitizers, and the full harness under ASan with default quarantine beyond T11.
- Mutants for T00, T10, T13 and T14.
- Fuzzing that reaches successful install, step, snapshot and restore paths (world.snapshot and world.restore never exercised by fuzz).
- `tools/validate_pack.py` and `tools/check_repository_license.py`.
- Performance qualification on the reference machine (PERFORMANCE.md).
- The `--no-build` harness output was checked only for rc 0.
- Stale-build T00 scenario was reasoned from code, not run.

## Commands run (return codes)

```
rm -rf build && python3 tools/dev.py build --profile dev          # rc 0
rm -rf build && python3 tools/dev.py build --profile release      # rc 0
python3 tools/dev.py build --profile dev                          # rc 0 (restored dev build)
(cd build && ctest --output-on-failure)                           # rc 0, 7/7 (dev); release rc 0
python3 -I tests/harness/run_m0.py --self-test                    # rc 0, 82 checks
python3 -I tests/harness/run_m0.py --binary build/sutekh --report-dir <scratch>/audit/report_dev     # rc 0
python3 -I tests/harness/run_m0.py --binary build/sutekh --report-dir <scratch>/audit/report_rel     # rc 0 (release)
python3 -I tests/harness/run_m0.py --binary build/sutekh --report-dir <scratch>/audit/report_rel_nb --no-build  # rc 0
python3 tools/demo_zero_wait.py                                   # rc 0 (dev and release)
python3 -I tests/e2e/no_build_probe.py                            # rc 0, verdict pass (dev and release)
cmake -S <scratch>/mut/M00 -B <scratch>/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"        # rc 0
cmake --build <scratch>/asan                                      # rc 0
(cd <scratch>/asan && ctest)                                      # rc 0, 7/7
python3 -I tests/harness/run_m0.py --binary <scratch>/asan/sutekh --report-dir <scratch>/audit/rep_asan   # rc 1 (T11 RSS)
ASAN_OPTIONS=quarantine_size_mb=1:detect_leaks=1 python3 -I tests/harness/run_m0.py --binary <scratch>/asan/sutekh --only T11   # rc 0
python3 -I <scratch>/mut/run_mut.py                              # rc 0, control green, M01-M15 as tabled
python3 -I tests/e2e/no_build_probe.py   (on M13 mutant)          # rc 1, verdict fail (build tool exec in window)
python3 -I <scratch>/fuzz/fuzz.py <scratch>/mut/M00 <scratch>/asan/sutekh {1,2,3} {300|800} {300|800}   # rc 0 each, 0 findings, 5,700 requests
```

Scratch paths: mutants `<scratch>/mut/M00`..`M15`, ASan build `<scratch>/asan`, raw logs `<scratch>/audit/`, results `<scratch>/mut/results.json`.
