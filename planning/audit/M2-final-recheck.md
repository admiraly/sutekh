# M2-01 final independent re-check (fixes 2)

Auditor: independent re-checker (did not write the fixes). Subject: fixes 2 at `86d1f17` (worktree HEAD `ab05845`,
which only records M2 status). Inputs: `planning/audit/M2-reaudit.md`, `planning/handoffs/2026-10-12-m2-01-fixes2.md`,
`planning/contracts/M2_MIGRATION_CONTRACT.md` sections 5 and 10. Product code was not edited. Builds, sanitizer trees,
mutant copies and logs were made in a scratch directory under the session scratchpad (`final/`), and deleted at the end
(section 7). Only this file was added to the worktree.

## 1. Verdict: NOT ACCEPT

M2-01 stays not accepted. Status by item:

| Item | Verdict | Basis |
|---|---|---|
| BLK-3 (iii) native in-flight migration | **Partly resolved.** The in-flight install refusal, the VM tier after commit, the gen-2 install, the idle kill and the step death are verified. "Old artifact released" is **not** verified (mutant X14 survives). "Bindings regenerated" is not checked | 3.3 |
| N1 pin kept on failed second commit | **Resolved** | 3.1 |
| N2 allocation failure at every allocation point (contract section 10) | **Not resolved.** The sweep covers only the add-field path. Transform-path allocations are never injected (X7, X8 survive). The static check misses other allocator names (X4 survives) | 3.2 |
| N3 TSan | **Resolved for the migration code.** Migration tests and `specialize_inflight` are clean under TSan. Three non-migration tests fail under TSan in worker children (environment, not investigated) | 3.4 |
| Old artifact released (unchecked item) | **Not verified.** No test reads release state on the migration path | 3.5 |
| docs/AGENT_RPC.md | **Does not reflect the new behaviour.** One statement is now wrong (section 5, gap 1) | 5 |

Path to "accept with waivers listed" (owner decides; I did not choose):
- (a) Extend the N2 sweep to the transform, rename and remove documents (`transform.json`, `m_mixed.json`,
  `m_rename.json`, `m_remove.json`), and extend `check_alloc_points.py` to `strdup`, `strndup`, `aligned_alloc`,
  `posix_memalign`, `reallocarray`, `asprintf`, or record a written waiver of the transform-path points.
- (b) Expose the native registry state for the migrated world (a test-only `internal.stats` field from
  `su_nv_state` or `su_nv_live_count`), or record a written waiver that release is evidenced only by the tier and the
  step-death check. Also record a waiver for bindings regeneration.
- (c) Fix the docs (section 5).
- (d) Owner decisions still open from fixes 2: N4 (recovery exhaustion inside an undo), N5 (IDs of an undone commit
  are reusable), and the reversal of handoff deviation 2 (rollback stays available after a failed commit).

## 2. Commands and results

All builds from the worktree source with out-of-source build directories under the scratch directory. Scratch
paths are abbreviated below as `<S>`.

1. `python3 tools/bootstrap_asmjit.py`: `"ok": true`, `"zlib_text_present": true`.
2. Default Release: `cmake -S <wt> -B <S>/rel -DCMAKE_BUILD_TYPE=Release`, then `cmake --build <S>/rel -j4`. Exit 0,
   0 warnings.
3. Native Release: `cmake -S <wt> -B <S>/nat -DCMAKE_BUILD_TYPE=Release -DSUTEKH_NATIVE=ON`, then build. Exit 0,
   0 warnings.
4. `ctest -j4` in `rel`: **100% passed, 21/21** (11.3 s).
5. `ctest -j4` in `nat`: **100% passed, 33/33** (36.6 s). Includes `specialize_inflight` (78 checks, 0 failures),
   `migration_alloc_points`, `migration_runtime`, `migration`, `migration_model`.
6. Harness, native: `python3 -I tests/harness/run_m0.py --binary <nat>/sutekh --no-build`: **passed=26, failed=0,
   not_run=1 (T00), unavailable=0**.
7. Harness, default: same with `<rel>/sutekh`: **passed=23, failed=0, not_run=1 (T00), unavailable=3** (native build
   required, as in the handoff).
8. Source hashes (sha256, first 20 hex) match the handoff: `su_agent.c` `7b1930faad0b4604dc75`, `worker.c`
   `267902d978aab26688a5`, `su_migrate.c` `d38ee8b0d95bf67af525`, `su_migrate.h` `d70d5852f24b48a9e6ee`,
   `CMakeLists.txt` `c1517d94df32d9030497`, `test_migrate_runtime.c` `d0b09e704c464716f5c8`, `run_mutants.sh`
   `80631d7d639b0bcca0d6`, `check_alloc_points.py` `ca6d060dc9d1f3f9fead`, `test_specialize_inflight.c`
   `11f956cc57079c539ef7`.
9. TSan tree: `cmake -S <wt> -B <S>/tsan -DCMAKE_BUILD_TYPE=Debug -DSUTEKH_NATIVE=ON` with
   `CMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g -O1"` and the same flag in the exe and shared linker
   flags. Built, exit 0.
   - `ctest -R migration`: **4/4 passed** (`migration`, `migration_model`, `migration_runtime`,
     `migration_alloc_points`).
   - `test_specialize_inflight` direct run under TSan: **78 checks, 0 failures**, no ThreadSanitizer report.
   - Full `ctest -j2` under TSan: **29/33 passed**. Failed: `control`, `reload`, `snapshot_cp`, `containment`.
     `control`, `reload` and `snapshot_cp` fail with `ThreadSanitizer: CHECK failed: sanitizer_allocator_primary64.h:131`
     in the worker children. They still fail with `SUTEKH_WORKER_AS_BYTES=0` and with `SUTEKH_WORKER_SANDBOX=0`.
     `containment` failed once in the parallel run and passed on rerun alone. Not investigated further.
10. ASan + UBSan tree: Debug with `-fsanitize=address,undefined -fno-sanitize-recover=undefined`, native on. Run with
    `ASAN_OPTIONS=detect_leaks=0 SUTEKH_WORKER_AS_BYTES=0`.
    - `ctest -R migration`: **4/4 passed**.
    - `test_specialize_inflight` direct run: **78 checks, 0 failures**, no sanitizer report.
    - Full `ctest -j2`: 32/33 on the first run. The one failure, `containment`, was caused by my extra
      `SUTEKH_WORKER_SANDBOX=0` override, which the test itself is sensitive to. Rerun with defaults: **passed**,
      so the effective result is **33/33**.
    - LeakSanitizer off (ptrace), as in the handoff.
11. Existing mutant suite on a copy of the worktree: `sh tests/migration-runtime/run_mutants.sh <copy> <build>`:
    **21 run, 19 killed, 2 survived** (M02 "plan validity ignored", M03 "stale plan not refused"). Same as the
    handoff. I did not re-prove that M02 and M03 are equivalent.
12. My own mutants (section 4), applied to a second copy, one edit each, each old text required to occur exactly once.

## 3. Item detail

### 3.1 N1: a failed second commit keeps the rollback target (resolved)

Read: `su_agent.c` `migrate_commit_run` stages `pin_try` before prepare (line ~5274), promotes it to `pin` only after the
post-commit checkpoint succeeds (line ~5435), and calls `nat_clear` there. The wrapper `h_world_migrate_commit` clears
`pin_try_has` on every exit. `undo_migration` no longer clears the committed pin.

Mutants:
- X1 (promote the staged pin before the post-commit checkpoint): **killed**, 2 failures in `migrate_runtime`.
- M17, M19, M21 (from the existing suite): killed.
- `run_failed_second_commit` covers both the prepare refusal and the no-room refusal: the rollback restores the first
  commit's pin (`restored_from` equals the first plan hash, and the schema has no `health` or `shield` field).

### 3.2 N2: allocation failure at every allocation point (not resolved)

Contract section 10 says "memory failure injected at each allocation point". What exists:
- Every `mig_malloc`, `mig_calloc` and `mig_realloc` call in `su_migrate.c` counts an injection point (23 sites).
- `run_alloc_faults` sweeps the points of `m_add.json` only (plan, then prepare with the commit). The seam is set only in
  that test (`SUTEKH_TEST_MIG_ALLOC_TOKEN`). No other test sets it.
- `build_capsule` is the only user of `Sb`. It runs only for the transform path. Transform-path allocations (writes,
  lir, capsule synthesis, transform outputs) are reached by no injection test. The `transform.json`, `m_mixed.json`,
  `m_rename.json` and `m_remove.json` fixtures are used only by `tests/migration/test_migrate.c`, which has no injection.

Mutants:
- X6 (add_field name allocation: NULL check removed): **killed** (1185 failures).
- X7 (transform `writes` allocation: NULL check removed): **survived**. The point is not in the sweep.
- X8 (`sb_put` realloc: NULL check removed): **survived**. Same cause (transform path only).
- X4 (`aligned_alloc` in plan, uncounted, not a raw `malloc`): **survived both** the runtime test and
  `check_alloc_points.py`. The static check matches only `malloc`, `calloc` and `realloc`.
- X5 (raw `calloc` in plan): **killed** by the static check. The runtime test cannot see it.

So the claim "every allocation in the migration engine's plan and prepare is an injection point" holds for the counted
sites, but the test does not inject at the transform-path points, and the static check has a gap.

### 3.3 BLK-3 (iii): native layout invalidation (partly resolved)

Evidence that holds:
- `scenario_interleave_migrate` (in `test_specialize_inflight.c`, via `SU_TEST_INTERLEAVE=migrate`): the install is
  refused with `SU_E_STALE_REVISION`; the tier after commit is `vm`; `layout_generation` is 2; a gen-2 specialization
  installs; ticks after commit equal a VM-only control on the unaffected columns.
- `scenario_migrate_step_death`: a worker death after a commit recovers without a `native` member and with no
  quarantine.
- Mutant X9 (commit does not clear the supervisor's native records): **killed** (1 failure, the `native` member check at
  line 829).
- Mutant X12 (install ignores the world revision): **killed** (the refusal code is not `SU_E_STALE_REVISION`).
- Mutant X13 (install ignores both revision and generation): **killed** (the refusal code is not `SU_E_STALE_REVISION`).
  The refusal came from another guard. I read it as the artifact-hash check; that is by code path, not confirmed from the
  response.
- Mutant X11 (install ignores only the generation): **survived**. The revision check refuses first, so the generation
  check is redundant on this path. Contract B13 names `su_nv_install` for the generation refusal, but the check is one
  level up in `h_internal_specialize_install`. That is sound, but the contract wording and the code differ.

Evidence that does not hold:
- Old artifact released: see 3.5.
- Bindings regenerated: not checked explicitly (the handoff says so).
- Rollback and native records: X10 (rollback does not clear the records) **survived**. The native lane never runs a
  rollback. The rollback clearing is untested in the native lane.

### 3.4 N3: TSan (resolved for the migration code)

Clean under TSan: the four migration ctest tests, `specialize_inflight` (direct run, 78 checks), and `tick_workers`,
`specialize_churn`, `entity_tick`, `recovery` and `snapshot` in the full TSan run. Not clean: `control`, `reload` and
`snapshot_cp` (TSan allocator CHECK in the worker children, unchanged with `SUTEKH_WORKER_SANDBOX=0` and
`SUTEKH_WORKER_AS_BYTES=0`), and `containment` on one run out of two. These are not migration tests. The
environment cause is not established. A waiver of the full-suite TSan run is needed, or a fix to that environment.

### 3.5 Old artifact released (not verified)

No test reads release state on the migration path. The in-process registry tests (`test_native_versions`,
`test_safe_retire`) check `su_nv_state` and `su_nv_live_count` for release in isolation, not through the migration swap.
The worker runs the migration (`h_internal_world_migrate_swap`, which frees the old schedule at line ~4529 through
`su_sched_free`), and the test runs in another process. No worker stat exposes the state (`internal.stats` has none).

Mutant X14 (the swap does not free the old schedule, so its kernels are never released): **survived** `specialize_inflight`
(78 checks, 0 failures). So the observed evidence (the tier and the step-death check) does not distinguish a released
kernel from a leaked one. The doc says "observed through the tier", which is not evidence of release.

### 3.6 Not re-run here

- BLK-3 (ii), 1,000,000-row timing and admission: not re-run (handoff observation only).
- The undo path's native records (`undo_migration` keeps `nat[]`, and no test runs a no-room undo with a kernel
  installed): not tested. I did not claim a defect.

## 4. Mutants (own, applied to a second copy of the worktree)

| id | mutant | target | result |
|---|---|---|---|
| X1 | commit promotes the staged pin before the post-commit checkpoint | migrate_runtime | KILLED (2 failures) |
| X2 | `spool_referenced` does not protect `pin_try` | migrate_runtime | SURVIVED (equivalent: the checkpoint list protects the same file) |
| X3 | wrapper does not clear `pin_try_has` on refusal | migrate_runtime | SURVIVED (equivalent: promotion sets the flag fresh) |
| X4 | `aligned_alloc` in plan (uncounted) | migrate_runtime and static | SURVIVED both (gap in the static check) |
| X5 | raw `calloc` in plan | static | KILLED |
| X6 | add_field name allocation: NULL check removed | migrate_runtime | KILLED (1185 failures) |
| X7 | transform `writes` allocation: NULL check removed | migrate_runtime | SURVIVED (point not swept) |
| X8 | `sb_put` realloc: NULL check removed | migrate_runtime | SURVIVED (point not swept; `Sb` is used only by the transform path) |
| X9 | commit does not clear native records | native | KILLED (1 failure) |
| X10 | rollback does not clear native records | native | SURVIVED (no rollback in the native lane) |
| X11 | install ignores the generation | native | SURVIVED (revision check refuses first) |
| X12 | install ignores the revision | native | KILLED (refusal code differs) |
| X13 | install ignores revision and generation | native | KILLED (refused by another guard) |
| X14 | swap does not free the old schedule | native | SURVIVED (release unobservable) |

Mutant X1 to X8 used the migration runtime test binaries, and X9 to X14 the native test binaries, both from scratch
builds of the copy (Release).

## 5. Documentation gaps (`docs/AGENT_RPC.md`)

1. Section 17, the no-room and double-fault paragraph (about line 537) says "the pin is then no longer a rollback
   target". This is wrong after N1: the last committed pin is kept, and a failed commit leaves it as the rollback target.
   Must be changed.
2. Section 17, "Not in this section" (about lines 567 to 569) says "the native build lane is not exercised by the
   migration tests" and "installed kernels of the old schedule are released when the schedule is replaced". The first
   is stale (`specialize_inflight` covers commit in the native lane). The second is not verified (3.5).
3. Section 17 does not say that the native records are cleared on a successful commit and on rollback, and kept on an
   undo. The contract does not say this either. It is in the handoff only.
4. Section 17 does not say that a failed commit (any stage) keeps the last committed pin. The handoff's deviation 2
   is reversed in code, and the doc still gives no statement for it.
5. Section 17 (about lines 555 to 556): "A rollback keeps the retired IDs of the undone lineage except those that are
   live in the restored schema". The contract (section 5) says `retired_field_ids` is not shrunk. The exception clause
   and N5 (IDs of an undone commit are reusable) are not reconciled. Needs the owner's decision.
6. Section 9 test-seam list (about line 263) lists `SU_TEST_INTERLEAVE=candidate|close` only. `migrate` is missing.
7. Section 17 test seams: `SUTEKH_TEST_MIG_ALLOC_TOKEN` (the allocation-failure seam), the sweep, the static check
   `check_alloc_points.py` and ctest `migration_alloc_points` are not documented.
8. The N4 policy (three worker deaths during the undo's rebuild: `SU_E_INTERNAL` in a rebuild request, or
   `SU_E_RESOURCE_LIMIT` in `seed_worker`) is in the handoff only. Section 17 does not give it.

## 6. Unrun or failing checks

- Full TSan suite: `control`, `reload`, `snapshot_cp` failing in worker children (3.4). `containment` flaky once.
- LeakSanitizer and valgrind: not run.
- BLK-3 (ii) timing: not re-run.
- Old artifact release, bindings regeneration, native records after an undo or a rollback: no test (3.3, 3.5, 3.6).
- N4 policy: no test (handoff decision only).
- Transform-path allocation injection: no test (3.2).
- Mutants M02 and M03 (existing suite): survive as in the handoff; not re-proved.
- Harness TX3, TX4, TX10 on the default build: unavailable by design (native build required).

## 7. Hygiene

- Product code: not edited. Only this file is added to the worktree (committed, not pushed).
- Scratch builds, copies and logs under the session scratchpad (`final/`) were deleted at the end.
- `build/` in the worktree (git-ignored) holds harness output from several runs, including outputs that predate this
  check. Not deleted. `third_party/asmjit` (git-ignored) left in place, as `tools/bootstrap_asmjit.py` expects.
- Sanitizer lanes used `SUTEKH_WORKER_AS_BYTES=0` and `ASAN_OPTIONS=detect_leaks=0` (ASan and TSan), as in the handoff.
  The one ASan failure (`containment`) came from my extra `SUTEKH_WORKER_SANDBOX=0`, and is not a product result.
