# M0 independent closure re-audit (M0-reaudit)

Auditor: independent re-auditor, worktree `agent-a908fc9f47c512a41`, HEAD `ecbdc07` (branch `worktree-agent-a908fc9f47c512a41`).
Scope: reproduction, blocker verification, mutation testing and sanitizer/fuzz runs in scratch copies outside the repository. No engine, harness, or status-doc source was edited. Scratch root: `/tmp/claude-0/-home-user-sutekh/82533bc6-e6ca-564a-ba43-39dd3091bafa/scratchpad/re/` (not in the repo). Evidence in `planning/evidence/` and `docs/IMPLEMENTATION_STATUS.md` was not trusted until reproduced.

## 1. Verdict

**M0 cannot close.**

What blocks, exactly:

1. **Timing-target comparison is not satisfied under the specified conditions.** MILESTONES M0 requires "genuine results and explicit timing target comparison". The comparison now exists (`compare_target`, `tests/harness/run_m0.py` around lines 378-436; verdicts emitted by T13). But in both profiles 3 of 5 targets are `not_comparable`:
   - `ir_prepare` (target p95 <= 20 ms) is measured on a **4-row** fixture. Its own gate reads "fixture has 4 rows; the target names a 1,024-row preview". MILESTONES says "M0 begins at 1,024 rows".
   - `install_critical_section` (target p95 <= 1 ms) is measured as the whole install RPC, which the harness itself labels an upper bound, not the critical section. Its measured RPC p95 is above 1 ms in both profiles.
   - `native_incremental_check` (target p95 <= 2 s) is not measured by T13 at all.
   The two "met" targets (preview 100 ms, warm edit 100 ms) are the only compliant rows.
2. **T13 cannot fail on any timing verdict.** The T13 check at the end of `t13` accepts any verdict in `{met, not_met, not_comparable}`. Several gate facts are hard-coded `True` in the gate table (warm process, reusable worker, "excludes long qualification suites", "bindings already prepared"), so those gates cannot show a violated condition.

Blockers 1 and 2 are one defect: no timing target is evaluated at its declared conditions, and the test that reports them cannot go red.

Everything else in the closure checklist passes or is partly met (section 5).

## 2. Reproduction (both profiles, HEAD ecbdc07)

| Check | Command (run from worktree root) | Return code | Result |
|---|---|---|---|
| Clean dev build | `rm -rf build && python3 tools/dev.py build --profile dev` | 0 | build/sutekh sha256 `2a8ba85f6930776bf7f5f59aab1241aa78604b755c1353359122c19583a5c7fc` |
| dev ctest | `ctest --test-dir build --output-on-failure` | 0 | 7/7 |
| Harness self-test | `python3 -I tests/harness/run_m0.py --self-test` | 0 | 138 checks, 0 failures |
| Release build | `python3 tools/dev.py build --profile release` | 0 | Release in the same `build/` dir (dev.py has one build dir; reconfigured from Debug, not deleted). Sha256 `c99583976107a8b96fe8d9bcdedc89503de0a3af49e5f3db98954206b1387717` |
| release ctest | `ctest --test-dir build --output-on-failure` | 0 | 7/7 |
| Harness, release | `python3 -I tests/harness/run_m0.py --binary build/sutekh --report-dir <scratch>/h-rel` | 0 | 16 passed, 0 failed, 0 not_run |
| Dev rebuild | `python3 tools/dev.py build --profile dev` | 0 | Debug binary above |
| Harness, dev | same, `--report-dir <scratch>/h-dev` | 0 | 16 passed, 0 failed, 0 not_run |
| Demo, dev | `python3 tools/demo_zero_wait.py --binary build/sutekh --out <scratch>/demo-dev` | 0 | status passed |
| Demo, release | same with release binary copy | 0 | status passed |
| Probe, dev | `python3 -I tests/e2e/no_build_probe.py --binary build/sutekh` | 0 | verdict pass; 43 traced exec attempts (7 successful); build-tool execs in edit window 0, whole run 0; both positive controls detected |
| Probe, release | same, release binary | 0 | verdict pass; 0 build-tool execs; positive controls detected |

Notes: the first release run used `build/` after a dev configure, not a fresh tree. The harness T00 separately builds a clean reference tree (`/tmp/m0-clean-build-*`, rc 0 in both reports), so T00 is independent of this. Harness self-test count is 138 (first audit: 82).

## 3. Blocker verification (first audit M0-audit.md)

| # | First-audit blocker | Status | Evidence |
|---|---|---|---|
| 1 | T13 compares no timing target | **Partly resolved** | Verdicts exist for all five targets (`planning/evidence/m0-harness-dev-summary.txt`, release report `timing_targets`). Dev: ir_prepare not_comparable (4-row), preview met (8.526 ms), install not_comparable (1.406 ms RPC), warm met (12.933 ms), native not_comparable. Still blocks (section 1). |
| 2 | T11 is 200 iterations, no failed or reverted installs | **Resolved for count; partly for content** | `DEFAULT_RELOAD_ITERATIONS = 1000` (run_m0.py line 545). Report counters: 1000 operations, `successful_install` 400 (200 `revert`), `failed_install` 600 (200 each stale revision, past tick, layout generation), `invalid_refused` 1000. Gap: the loop never installs a candidate whose evaluation failed; that path is tested once in T06 (mutant MJ is red only in T06). |
| 3 | T04 misses oversize, parse, missing-store, schema cases; M08 survives T04 | **Partly resolved** | T04 now has 28 negative cases (`negative_case_count` 28): oversize IR, depth-40 IR and capsule, truncated JSON, invalid UTF-8, duplicate key, four schema-mismatch cases, missing store, 1 MiB+1 protocol line. Every TEST_SPEC T04 item (unknown op, bad type, duplicate key/register, undefined register, denied read/write, oversize) is covered. Still open: no capsule `limits.max_rows` case in T04; mutant MA (ignore max_rows) is red only in T09. |
| 4 | Status doc and evidence stale; reproduction omits demo and probe | **Partly resolved** | Demo and probe are in `docs/IMPLEMENTATION_STATUS.md` section 6 (lines 193-196) and run correctly (table above). Still wrong: header says "as committed at 8108166" (HEAD is ecbdc07); dev hash `fc8149df...` is not reproduced (fresh dev build `2a8ba85f...`; cause not verified); release section 2 T13 column does not match its evidence file (section 6 below). |

Hygiene items from the first audit, checked individually:

| First-audit item | Status | Evidence |
|---|---|---|
| T00 incremental build only | Resolved | T00 builds a clean reference tree in a temp dir (run_m0.py ~lines 984, 1035); `clean_build` rc 0 in both reports. |
| T12 blind to absolute-path `cc` (mutant M13 green) | Resolved | Mutant MB (`system("/usr/bin/cc ...")` in install) is red in T12 through the strace probe ("strace probe verdict is pass" fails). |
| T05, T10, T11 trust engine-reported pids | Resolved for OS identity | `os_identity` checks at run_m0.py lines ~1293, 1309, 1552, 1692-1693. Gap still open for candidate-record pid (mutant ML). |
| Runner passes zero-check bodies | Resolved | Guard at run_m0.py line 2096: passed with 0 checks becomes not_run. |
| Recorded checks cap silent | Resolved | `MAX_RECORDED_CHECKS = 20000`; report has `checks_dropped` (line 2101). |
| su_proc.c non-CLOEXEC duplicated pipe ends (defect 11) | Resolved | Child closes both duplicated descriptors and every pipe end above fd 2 (`src/agent/su_proc.c` lines 103-108). |
| su_proc.c unchecked `req_len + 1u` (defect 12) | Resolved | `req_len == SIZE_MAX` rejected (`su_proc.c` line 148); framing size guard landed in 5619a58. |
| T11 RSS bound fails under ASan | Partly | Harness sets `quarantine_size_mb=1` for ASan binaries; T11 passed under ASan in section 7. Bound is still harness-chosen. |
| T09 docstring says runtime part unavailable | Not re-checked | Runtime part runs (MF/MK red in T09). |

## 4. Test table T00-T15 (checks total; failed; status)

Source: `<scratch>/h-dev/m0_report.json` and `<scratch>/h-rel/m0_report.json`, both produced by this audit at HEAD ecbdc07.

| ID | Name | Dev (Debug) | Release | Note |
|---|---|---|---|---|
| T00 | Clean bootstrap | passed (9, 0) | passed (9, 0) | clean temp build, rc 0 |
| T01 | Reference movement | passed (6, 0) | passed (6, 0) | |
| T02 | Empty and awkward counts | passed (24, 0) | passed (24, 0) | |
| T03 | Numeric edges | passed (12, 0) | passed (12, 0) | |
| T04 | Invalid input matrix | passed (33, 0) | passed (33, 0) | 28 negative cases; no max_rows case |
| T05 | Code-only swap | passed (22, 0) | passed (22, 0) | |
| T06 | Failed swap | passed (7, 0) | passed (7, 0) | |
| T07 | Stale candidate | passed (7, 0) | passed (7, 0) | |
| T08 | Snapshot round trip | passed (10, 0) | passed (10, 0) | |
| T09 | Working-root failure | passed (11, 0) | passed (11, 0) | |
| T10 | Candidate isolation | passed (15, 0) | passed (15, 0) | |
| T11 | Repeated reload | passed (6413, 0) | passed (6413, 0) | 1000 operations |
| T12 | No native gameplay build | passed (11, 0) | passed (11, 0) | strace probe primary |
| T13 | Latency decomposition | passed (93, 0) | passed (93, 0) | verdicts not gating (section 1) |
| T14 | Deterministic fixture repeat | passed (8, 0) | passed (8, 0) | |
| T15 | Structured protocol | passed (15, 0) | passed (15, 0) | |
| Total | | 16 passed | 16 passed | 0 failed, 0 not_run |

Note: T12 under ASan is `not_run` (probe inconclusive, section 7); T00 is `not_run` in mutant runs (`--no-build`).

## 5. Closure checklist (MILESTONES.md, M0)

| Item | Result | Basis |
|---|---|---|
| T00-T15 genuine results with explicit timing target comparison | **FAIL** | 16/16 in both profiles, and 11 of 14 mutants are caught by at least one test (ME, MG and ML survive). Timing comparison exists but 3/5 targets are `not_comparable` and the ir_prepare measurement is not at 1,024 rows (section 1). |
| Instrumented capsule edit invoking no native compiler/assembler/linker | PASS | Probe rc 0 in both profiles, 0 build-tool execs in the window and whole run, positive controls (bare and absolute-path `cc`) detected. Mutant MB caught by T12. Under ASan the probe is inconclusive. |
| Preserved PIDs and state for code-only swaps | PASS, with gap | T05, T11 check OS identity (start time). Demo rc 0 both profiles. Gap: a wrong `worker_pid` recorded at install is not detected (mutant ML green everywhere). |
| Rejection without active-state change | PASS | T04, T06, T07, T11 verify tick, revision, columns and manifest. MJ (failed-evaluation install accepted) red in T06. |
| Stored commands for a new developer | PARTIAL | Section 6 commands all run (table above). Doc header and dev hash are wrong (section 6). |
| T11 at 1,000 successful/failed/reverted revisions | PASS, with interpretation | 1000 operations (400 successful, 600 refused, 200 reverts). "Failed" means refused installs of valid candidates, not failed-evaluation candidates. |
| T04 negative matrix incl. oversize input | PASS (TEST_SPEC list) | All listed items covered. max_rows not covered by T04. |

## 6. Status-document spot check (`docs/IMPLEMENTATION_STATUS.md`)

Checked against raw files and fresh runs:

| Claim | Source | Result |
|---|---|---|
| ctest 7/7, both profiles | `m0-ctest-*.log`, fresh runs | MATCH |
| Self-test 138 checks, 0 failures | `m0-harness-self-test.log`, fresh | MATCH |
| T04: 33 checks, 28 negative cases | dev report | MATCH |
| T11: 400 successful (200 reverts), 600 refused, 1,000 refused submissions | dev report counts | MATCH |
| Demo/probe: 43 exec attempts, 7 successful, 0 build execs | `m0-probe-dev.json`, fresh probe log | MATCH |
| Warm edit dev p95 total 10.406, evaluate 8.163, install 0.843, step 0.618, submit 0.480; release total 9.325 | recomputed from `m0-latency-{dev,release}-raw.json` (n=720, nearest-rank) | MATCH |
| Warm medians dev 8.614, release 8.094 | recomputed | MATCH |
| Cold p95 dev 9.153, release 9.003 (n=60) | recomputed | MATCH |
| Failed candidates: 0 of 120 published (both) | recomputed from `warm_failed_candidates` | MATCH |
| Native build 26 compile, 15 link; configure 201.5 to 225.2 ms; build 591.7 to 687.3 ms | `m0-native-build.json` | MATCH |
| Release binary sha256 prefix `c9958397` | fresh release build | MATCH (full hash equal) |
| Dev binary sha256 prefix `fc8149df` | fresh dev build sha `2a8ba85f...`; artifact file | **MISMATCH** (not reproduced; build-path dependence not verified) |
| Header "as committed at 8108166" | HEAD is ecbdc07; 16c3393 and ecbdc07 changed after it | **MISMATCH** (stale) |
| T13 dev table: IR 10.652, preview 8.526, install 1.406, warm 12.933 | `m0-harness-dev-summary.txt` | MATCH |
| T13 release table: IR 10.525, preview 8.993, install 1.604, warm 12.977 (section 2 and section 5) | `m0-harness-release.json` and `-summary.txt`: IR 9.665, preview 11.736, install 1.864, warm 11.391 | **MISMATCH** |
| Stress fixture bytes 27,833,861 (1M) and 6,875,843 (250k) | both raw files | MATCH |
| Stress restore 1,313.6 / 1,042.7 ms | not re-derived | UNCHECKED |
| WORKER_MAX_WORLDS 4 | `src/runtime/worker.c` line 26 | MATCH |

Mismatches: 3 (dev hash; header commit; release T13 column in two sections).

## 7. Mutation testing (scratch copies, Debug build, `--no-build` harness, full suite)

Driver: scratch `re/mutate.py`. Each mutant is a copy of the worktree with one edit set; every anchor is asserted to occur exactly once. Control: the unmodified tree passed 16/16 (section 4). Mutant builds rc 0 for all 14.

| ID | Mutation (scratch copy only) | Red tests | Survived | Notes |
|---|---|---|---|---|
| MA | ignore capsule `limits.max_rows` (`su_vm.c` row_limit) | T09 | T04 | T04 blind spot (see defects) |
| MB | install runs `system("/usr/bin/cc --version")` by absolute path | T12 (probe) | none | Caught; first-audit M13 survival resolved |
| MC | VM add_u32 computes subtraction | T01,T02,T03,T05,T06,T07,T08,T10,T11,T12,T13,T14 | none | |
| MD | VM add_u32 saturates instead of wrapping | same 12 | none | |
| ME | stale expected-revision check skipped (step 2 only) | none | **T07 and all** | Redundant with step 8 (same comparison, `reload.c` ~line 186); observable codes identical; the cheap-check-before-file-read ordering is untested |
| MF | failed system publishes its partial working root, returns error | T09 | none | |
| MG | layout-generation check at install admission skipped | none | **all** | Redundant with `schedule.c` line 176 (same layout compare); no test distinguishes the two |
| MH | snapshot restore lands at tick+1 | T08 | none | |
| MI | snapshot emission omits last column | T08 | none | |
| MJ | candidate install accepts a failed-evaluation candidate (evidence gate off) | T06 | none | T11 never installs failed-evaluation candidates |
| MK | system failure swallowed, success reported with partial tick | T09 | none | |
| ML | install records pid+1 as candidate `worker_pid` (`su_agent.c` `note_install`) | none | **all** (demo too) | Blind spot: no check compares the candidate-record pid with the live worker's OS identity; demo reads pid from `world.inspect` |
| MM | VM lt_u32 compares signed | T03 | none | |
| MN | VM row loop skips last row | T01,T02,T03,T05,T06,T07,T08,T11,T12,T13,T14 | none | |

Survivors (blind spots): ME, MG (both redundant checks; equivalent in observable codes, not a proven escape of a test), ML (real blind spot). MA is caught only by T09.

## 8. Sanitizer pass and protocol fuzzing

Build: scratch copy `re/san/repo`, `cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g"`.

| Step | Return code | Result |
|---|---|---|
| cmake configure | 0 | |
| cmake build (engine and tests) | 0 | |
| ctest | 0 | 7/7; no sanitizer lines in log |
| harness, ASan binary, `--no-build` | 0 | 14 passed, 0 failed; T00 not_run (`--no-build`), T12 not_run (strace probe inconclusive under ASan: LeakSanitizer and ptrace; the demo itself passes under ASan when run directly, rc 0) |

Fuzzing (`re/fuzz.py`): one fresh ASan `serve` per round, 60 rounds x 30 requests per seed, seeds 1-3. Each round starts with a valid flow (world.create, capsule.submit, candidate.evaluate, world.step, candidate.install, world.snapshot) and tracks revisions from replies. Requests: 50% unmodified valid, 15% raw byte edits, 35% structural mutations (value swaps, deletions, extra keys, envelope and method changes, stale revisions).

| Metric | Value |
|---|---|
| Requests total | 5,400 (1,800 per seed) |
| Overall success (`ok:true`) | 2,457 / 5,400 = 45.5% |
| Unmodified valid requests | 2,207 / 2,695 = 81.9% |
| Structural mutations | 232 / 1,875 = 12.4% |
| Raw byte edits | 18 / 830 = 2.2% |
| Successful deep-path replies | candidate.install 200, world.step 214, world.restore 156, world.snapshot 210, candidate.evaluate 339 |
| Crashes, process deaths, no-reply, non-JSON reply | 0, 0, 0, 0 |
| Sanitizer lines (ASan/UBSan/LSan) | 0 |

Caveat: the valid-class failures (about 18%) are mostly `SU_E_STALE_REVISION` from snapshot and install replays and capsule-state errors. Mutated requests mostly fail at parse or type checks, so the fuzz success ratio is low for structural edits.

## 9. Defects, ranked

1. **High (closure blocker):** timing verdicts do not evaluate targets at their declared conditions. ir_prepare measured on a 4-row fixture; install not isolated; native not measured. T13 accepts any verdict (`t13` final check).
2. **High (closure blocker):** T13 gate facts hard-coded `True` for warm process, reusable worker, "excludes long qualification suites" and "bindings already prepared" (t13 gates table). A violated condition cannot be reported.
3. **High (blind spot):** candidate-record `worker_pid` after install (mutant ML) is not compared to the live worker's OS identity; harness and demo stay green.
4. **Medium:** T04 has no capsule `limits.max_rows` case; MA is caught only by T09.
5. **Medium:** no test installs a failed-evaluation candidate beyond the single T06 case; T11 "failed" revisions are refusals of valid candidates.
6. **Medium:** ME and MG survive because the stale and layout checks are repeated (reload.c and schedule.c). Ordering of the cheap stale check before file reads is untested.
7. **Medium (docs):** `docs/IMPLEMENTATION_STATUS.md` release T13 column does not match `planning/evidence/m0-harness-release.json` (three figures in two sections); header commit is stale; dev hash does not reproduce.
8. **Medium:** under ASan, T12 is `not_run` (probe inconclusive), so the no-build claim has no sanitizer-instrumented evidence.
9. **Low:** 18 commits between e1f63c7 and e36e5ca carry `Co-Authored-By` trailers; none of the 22 commits after e36e5ca carry one.
10. **Low:** T11 RSS bound is harness-chosen (8 MiB window); ASan pass depends on `quarantine_size_mb=1` set by the harness.

## 10. Commit identity, licenses, vendored code, secrets

- Commits after 20f2625: 45, all authored and committed as `admiraly <249692417+admiraly@users.noreply.github.com>` (`git log --format='%an %ae %cn %ce' 20f2625..HEAD`). Identity is uniform.
- `Co-Authored-By` in commit messages after 20f2625: 18 commits (all between e1f63c7 and e36e5ca). 0 after e36e5ca. Across all history: 38. `Claude-Session` trailers after 20f2625: 39.
- License files (`LICENSE`, `COMMERCIAL_LICENSE.md`, `CLA.md`, `NOTICE.md`, `LICENSES/`, `THIRD_PARTY_NOTICES.md`): 0 commits after 20f2625.
- Vendored or downloaded code: `git ls-files` matches only `THIRD_PARTY_NOTICES.md` for vendor/third_party/external/deps/subprojects (315 tracked files).
- Secrets: `git grep` over HEAD for private-key headers, `ghp_`, `AKIA`, `xox*`, `sk-ant-`, and key assignments: no matches. No occurrence of the user's email in tracked files.

## 11. Unrun checks

- Clang build; TSan and MSan; sanitizers on the release profile.
- Harness under ASan with `--no-build` off (T00 clean build under sanitizers); T12 under sanitizers (probe inconclusive).
- Mutants for T00, T10, T13, T14 (T10 and T14 not mutated; T13 mutants limited to the existing verdict logic).
- Fuzzing covered world.create, world.inspect and the other templates, but not malformed paging of world.inspect beyond the template's offsets, and not a long-lived session with many worlds (a worker holds 4).
- The harness ran with its default of 1,000 reload iterations (no override).
- `tools/validate_pack.py` and `tools/check_repository_license.py` not run.
- Performance qualification on a reference machine (`docs/PERFORMANCE.md`); not applicable to closure claims.
- Stress restore figures in status doc section 2.3 (1,313.6 / 1,042.7 ms) not re-derived.
- Demo under ASan run only directly (rc 0), not under the probe's strace harness.

## 12. Commands and return codes (summary)

```
rm -rf build && python3 tools/dev.py build --profile dev                 rc 0
ctest --test-dir build --output-on-failure                               rc 0 (dev, 7/7)
python3 -I tests/harness/run_m0.py --self-test                           rc 0 (138 checks)
python3 tools/dev.py build --profile release                             rc 0
ctest --test-dir build --output-on-failure                               rc 0 (release, 7/7)
python3 -I tests/harness/run_m0.py --binary build/sutekh --report-dir <scratch>/h-rel    rc 0 (16/16)
python3 tools/dev.py build --profile dev                                 rc 0
python3 -I tests/harness/run_m0.py --binary build/sutekh --report-dir <scratch>/h-dev    rc 0 (16/16)
python3 tools/demo_zero_wait.py --binary build/sutekh --out <scratch>/demo-dev           rc 0
python3 tools/demo_zero_wait.py --binary <scratch>/sutekh-rel --out <scratch>/demo-rel.json  rc 0
python3 -I tests/e2e/no_build_probe.py --binary build/sutekh             rc 0 (dev, pass)
python3 -I tests/e2e/no_build_probe.py --binary <scratch>/sutekh-rel     rc 0 (release, pass)
python3 -I <scratch>/re/mutate.py                                        rc 0 (14 mutants built rc 0; harness rc per table)
python3 -I <scratch>/re/san.py                                           rc 0 (ASan build rc 0, ctest 7/7 rc 0, harness rc 0)
python3 -I <scratch>/re/fuzz.py <san repo> <san binary> {1,2,3} 60 30   rc 0 each (5,400 requests, 0 sanitizer lines)
python3 tools/demo_zero_wait.py --binary <scratch>/.../mut/ML/... (demo on ML mutant)  rc 0 (not detected)
git log --format='%an %ae %cn %ce' 20f2625..HEAD                         rc 0
git grep -n -E '<secret patterns>' HEAD -- .                             no matches
```

## 13. Recommended next work (not performed; no code changed)

- Measure ir_prepare at 1,024 rows; isolate the install critical section (or record that the target cannot be measured); make T13 gate facts measured, not hard-coded; make at least the declared ceilings gating or report them as provisional by owner decision.
- Add a candidate-record pid check against OS identity after install; add a capsule `max_rows` case to T04.
- Add a failed-evaluation install and a bypass test for the layout and stale checks inside `schedule.c` and `reload.c`.
- Correct the release T13 column and header of `docs/IMPLEMENTATION_STATUS.md`; re-derive the dev hash from a stable build path or record the path.
