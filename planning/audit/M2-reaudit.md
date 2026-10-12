# M2-01 independent re-audit of the fixes (BLK-1, BLK-2, BLK-3)

Auditor: independent re-auditor (did not write the M2-01 fixes). Subject: HEAD `ef9a69a` (`M2-01 fixes: undo failed
post-commit checkpoints, retire rolled-back field IDs, timings`), worktree `agent-a3b9a426450f5cd44`. Product code was
not edited. Fault seams and new mutants were applied only to a scratch copy of `git archive HEAD`. Scratch root (deleted
at the end, see section 9): `/tmp/claude-0/-home-user-sutekh/82533bc6-e6ca-564a-ba43-39dd3091bafa/scratchpad/reaudit/`.

Read: `planning/audit/M2-audit.md`, `planning/handoffs/2026-10-12-m2-01-fixes.md`, `planning/contracts/M2_MIGRATION_CONTRACT.md`
(r2), and the fixed code paths (`src/agent/su_agent.c` `undo_migration`, `h_world_migrate_commit`, `rollback_pinned`,
`recover`, `sync_after`; `src/runtime/worker.c` `snapshot_build`, `undone_ids_retired`, `h_internal_world_restore_schema`,
`h_internal_world_migrate_swap`; `src/world/migration/su_migrate.c` `su_migrate_swap_check`).

Source hashes at HEAD (sha256, first 20 hex) match the handoff: `src/agent/su_agent.c` `a03ccd475db164a1f350`,
`src/runtime/worker.c` `d6e05fa580105ffca8c9`, `src/world/migration/su_migrate.c` `f0d1058f4a7307cda7e2`.

## 1. Verdicts

| Item | Verdict | Basis (section) |
|---|---|---|
| BLK-1 post-commit double fault and no-room | **Resolved.** Residual (recovery exhaustion inside the undo) recorded as non-blocker N4 | 3, 4 |
| BLK-2 retired field IDs after rollback | **Resolved for user rollback.** Undo reuses the IDs of the undone attempt: decision N5 (non-blocker, owner to ratify) | 5 |
| BLK-3 (i) no-room post-commit test | **Resolved.** Mutant M10 (no undo on no-room) killed by the repository test | 5 |
| BLK-3 (ii) 1,000,000-row timing and admission | **Not re-verified** (handoff observation only). Non-blocker as a report, but the numbers are not re-run | 7 |
| BLK-3 (iii) native layout invalidation | **Open. Blocker** against contract section 10 unless the owner waives it in writing. Precise missing test in 6.1 | 6 |
| Gap (c) rollback digest check | **Non-blocker**, but a test-strength gap: the repository ctest does not detect a wrong-checkpoint rollback (mutant M11); harness TX11 does | 6.2 |
| C1 `systems_unresolved` | **Non-blocker.** Negative path now reached and verified (scenario F); not yet in the repository tests | 5 |
| C2 access-growth refusal | **Non-blocker.** Not a contract requirement (handoff deviation 2) | 6.2 |
| N1 (new) rollback target lost when a second commit fails | **Blocker against contract section 5 unless the owner records a waiver.** Low severity, small fix | 5.1 |
| N2 (new) allocation-failure injection (contract section 10) | **Blocker against section 10 unless waived.** No seam and no test exist; not in either earlier audit | 6.3 |
| N3 (new) TSan (contract section 10) | **Unrun.** Needs a run or a written waiver (migration code is single-threaded in the worker) | 7 |

Overall: M2-01 stays **not accepted**. BLK-1 and the rollback half of BLK-2 are resolved on every path I could drive.
BLK-3 (iii), N1 and N2 block acceptance under the contract as written.

## 2. Build and test results (exact)

Build directories: scratch (`reaudit/rel`, `reaudit/nat`, `reaudit/hooks`). Warning-free Release build (`grep -ci warning` = 0).

1. `python3 tools/bootstrap_asmjit.py` (in the worktree): `"ok": true`, `"zlib_text_present": true`. `third_party/asmjit`
   is git-ignored and was left in place.
2. `cmake -S <wt> -B <scratch>/reaudit/rel -DCMAKE_BUILD_TYPE=Release && cmake --build <scratch>/reaudit/rel -j4`: exit 0.
   Binary sha256 `06177ab1444b3d2a6b436d9d8a10996457203c25154ac7f46e04a6a5abb292c0` (equal to the handoff).
3. `cmake -S <wt> -B <scratch>/reaudit/nat -DCMAKE_BUILD_TYPE=Release -DSUTEKH_NATIVE=ON && cmake --build ... -j4`: exit 0.
   Binary sha256 `192acabba3835589a8b821a905180c5521870d952a27798a807dff9098416c79` (equal to the handoff).
4. `ctest -j4` in `rel`: **100% passed, 20/20** (11.41 s). Includes `migration`, `migration_model`, `migration_runtime`,
   `recovery`, `snapshot`, `snapshot_cp`, `reload`.
5. `ctest -j4` in `nat`: **100% passed, 32/32** (32.21 s).
6. `python3 -I tests/harness/run_m0.py --binary <nat>/sutekh --no-build`: **passed=26, failed=0, not_run=1 (T00),
   unavailable=0**. TX3 144 checks, TX4 1, TX10 20, TX11 27 all passed.
7. `python3 -I tests/harness/run_m0.py --binary <rel>/sutekh --no-build`: **passed=23, failed=0, not_run=1 (T00),
   unavailable=3 (TX3, TX4, TX10: native build required)**. TX11 27 checks passed.
8. `python3 -I tests/harness/run_m0.py --self-test`: **261 checks, 0 failures**.

Not run by me: the repository's ASan/UBSan commands and valgrind (the handoff reports them; not re-run here).

## 3. BLK-1: post-commit double fault and no-room (resolved)

### 3.1 Design read (undo versus LOG_ROLLBACK)

`undo_migration` (`su_agent.c`) removes the `LOG_MIGRATE` entry that `sync_after` appended, restores the supervisor
records saved in `PreMig` (revision, tick, digest, hash, migrated flag, systems, counts), clears the pin, and calls
`recover`. `recover` rebuilds from the newest checkpoint (`ck[ck_count-1]`, which is the pin taken at commit start,
because the undo first checks `ck_count == pre.ck_count`) and replays the log. The log now ends before the migration, so
replay stops at the pre-commit state. The checks I could confirm by reading and by driving:

- The pin is the newest checkpoint at the time of the swap; the post-migration checkpoint scratch file is unlinked on
  failure (`checkpoint_world`), so no migrated state can be replayed or rebuilt after the undo.
- The undo runs inside one supervisor request. The supervisor has no threads (`pthread` appears only in
  `src/runtime/jobs/su_jobs.c`, which runs chunked kernels inside a worker tick and joins them before the tick returns).
  `su_migrate_swap_check` refuses a world that is in a tick. So no tick can interleave with the commit or the undo.
- After an undo, steps append to the log with the same revision and tick numbering the undone attempt used. The
  replay of those steps from the pinned base was driven (scenario H, section 5).
- The deviation (undo instead of `LOG_ROLLBACK`) is sound for recovery: a `LOG_ROLLBACK` would move the revision past
  the failed commit's revision, while the undo returns the world to the pre-commit revision and log, so the recovery log
  stays the only source of truth and every logged digest still matches. The costs are the N5 identity question
  (section 5.2) and N1 (rollback target loss, section 5.1).

### 3.2 Fault injection and mutants (scratch copy; exact commands in section 8)

Scratch seams (scratch copy only, not in the worktree): a method-counted worker kill (`SUTEKH_TEST_REAUDIT_KILL_METHOD`
and token file; it kills the worker on the first n requests of one method, n written as a digit), armed by the supervisor
only for the duration of the undo (`SUTEKH_TEST_REAUDIT_UNDO_METHOD`); and a swap refusal injection
(`SUTEKH_TEST_REAUDIT_SWAP_REFUSE`) in `su_migrate_swap_check`. The post-commit no-room comes from the existing supervisor
seam `SUTEKH_TEST_POST_CK_NOROOM_TOKEN`. Note: children receive only `SU_TEST_*`, `SUTEKH_*` and sanitizer variables
(`src/agent/su_proc.c` `env_allowed`), so the seam variables must use the `SUTEKH_` prefix; the supervisor is a forked
child, so the variables must be set before `session_start`.

Kill matrix (`reaudit_migrate`, scenario A): each request method used by the undo's recovery, killed on the first n
replacement-worker requests, n = 1, 2, 3, on a no-room commit. `first` = digest right after the reply; `second` = digest
on the next request; `retry` = a clean commit after the undo, then two ticks; `final` = digest equal to the control.

| method | n | fired | reply | first | second | retry | final |
|---|---|---|---|---|---|---|---|
| internal.world.seed | 1 | yes | RESOURCE_LIMIT | 1 | - | 1 | 1 |
| internal.world.seed | 2 | yes | RESOURCE_LIMIT | 1 | - | 1 | 1 |
| internal.world.seed | 3 | yes | RESOURCE_LIMIT | 0 | 1 | 1 | 1 |
| world.create | 1 | yes | RESOURCE_LIMIT | 1 | - | 1 | 1 |
| world.create | 2 | yes | RESOURCE_LIMIT | 1 | - | 1 | 1 |
| world.create | 3 | yes | **INTERNAL (world unrecoverable)** | 0 | 0 | 0 | 0 |
| internal.world.restore_bytes | 1 | yes | RESOURCE_LIMIT | 1 | - | 1 | 1 |
| internal.world.restore_bytes | 2 | yes | RESOURCE_LIMIT | 1 | - | 1 | 1 |
| internal.world.restore_bytes | 3 | yes | **INTERNAL (world unrecoverable)** | 0 | 0 | 0 | 0 |
| internal.world.digest | 1 | yes | RESOURCE_LIMIT | 1 | - | 1 | 1 |
| internal.world.digest | 2 | yes | RESOURCE_LIMIT | 1 | - | 1 | 1 |
| internal.world.digest | 3 | yes | **INTERNAL (world unrecoverable)** | 0 | 0 | 0 | 0 |
| internal.world.create_from_snapshot | 1-3 | no | RESOURCE_LIMIT | 1 | - | 1 | 1 |
| world.step | 1-3 | no | RESOURCE_LIMIT | 1 | - | 1 | 1 |
| internal.world.capture | 1-3 | no | RESOURCE_LIMIT | 1 | - | 1 | 1 |

"no" means the undo's recovery never sent that method (an undone world is not migrated, so it is rebuilt from the
fixture or the pinned checkpoint, not from a post-migration snapshot). Those rows are plain undos; they do not exercise a kill.

Reading of the matrix:
- Up to two deaths inside the undo are survived in every case (3 recovery attempts, `AGENT_RECOVERY_ATTEMPTS`).
- Three deaths: where the death was inside a rebuild request, the world is marked unrecoverable (`SU_E_INTERNAL`). Where
  the death was in `seed_worker`, `recover` does not mark the world, the reply is `SU_E_RESOURCE_LIMIT` with the pre-state
  in the records, and the next request rebuilds it (`first=0, second=1`). Both outcomes fail closed, but they differ. This
  is the general M1 recovery policy (`recover`), not a migration defect; see N4.
- The `migrate.prepare`, `migrate.swap`, `migrate.step` and `migrate.rollback` stage seams are not reached by the undo,
  because the undo replays no `LOG_MIGRATE` entry. The stage kills in the repository test cover those points for commit.

Repeated fault and retry cycles (scenario B): five no-room commits, then three double-death commits (kill count 2 at
`migrate.capture`), then a clean commit, two ticks, and the final digest. Every faulted commit left the pre-commit digest.
Spool entries after each fault cycle: `6 5 5 5 5 5 5 5` (the first cycle leaves one more file, then the count is
constant), so no growth in spool files across repeated undos. I did not measure supervisor memory or the number of
registry entries.

Replay after undo (scenario H): ten ticks after an undone commit (below the checkpoint interval of 64, so recovery
replays `LOG_STEP` entries from the pinned pre-attempt checkpoint), a worker kill, and recovery: the digest equals the
control world (same setup, ten ticks, no migration attempt). **Pass.**

Ticks across a checkpoint after undo (scenario C): 70 ticks after an undo (crosses one checkpoint), kill, digest equal to the
no-migration control; then a clean commit, two ticks, kill, digest equal to the migrate-after-70 control. **Pass.**

### 3.3 Verdict

BLK-1 is resolved for the reply and the state: the double fault and the no-room both answer `SU_E_WORKER_FAILED` or
`SU_E_RESOURCE_LIMIT` with `rolled_back_to`, the digest equals the pre-commit digest, a retry reaches the control digest,
and recovery after an undo replays the log correctly. The residual is N4 (recovery exhaustion inside the undo; non-blocker).

## 4. BLK-1 residual: recovery exhaustion inside the undo (N4, non-blocker)

When all three replacement workers die during the undo's rebuild, the outcome depends on where the death occurs (table in
3.2). The contract requires the old world untouched (section 1). Neither outcome achieves that when recovery is exhausted,
but both fail closed. I judge it non-blocker because the M1 recovery policy has the same two outcomes for any recovery.
It should be recorded and made consistent (one policy for `seed` deaths and for rebuild deaths).

## 5. BLK-2 and BLK-3 (i)

### 5.1 Rollback target lost when a second commit fails (N1, blocker against section 5 unless waived)

Scenario G: the first commit (`health`) is acknowledged; the second commit (`shield`, generation 3) fails with the
post-commit no-room seam; then `world.migrate_rollback`:

    rollback after failed second commit: ok=0 code=SU_E_STALE_REVISION
    msg=rollback_unavailable: no pinned pre-migration checkpoint is retained

Contract section 5: rollback is "available while the pinned checkpoint is retained (until the next migration commit or
`world.close`)". A failed attempt is not a commit, but `h_world_migrate_commit` overwrites the pin at its start
(`w->pin = w->ck[w->ck_count-1]`), and the undo clears it (`pin_has = 0`). So the rollback target of the acknowledged
first commit is destroyed by a failed second attempt. Section 1 says a failed migration leaves the old world untouched; the
rollback capability is part of that world. The handoff's deviation 2 describes only the undo's pin and says the pin
replaced by the attempt "is not a committed migration", which overlooks that the replaced pin is the rollback target
of the last committed migration. This deviation is not disclosed.

Recommended fix (owner to choose): keep the last committed pin until a commit succeeds (a second pin slot), or record a
written waiver that rollback is unavailable after any failed commit. Severity is low: no state is lost, only the rollback.

### 5.2 BLK-2: retired IDs after rollback (resolved for user rollback)

- Mutant M07 (rollback merge drops the undone schema's field IDs) killed by the repository test (27 check failures).
  Mutant M08 (the same plus the `undone_ids_retired` check removed) killed (8 check failures). Both in
  `tests/migration-runtime/test_migrate_runtime.c` (`run_id_retirement`, `run_id_retirement_after_checkpoint`).
- Harness TX11 (27 checks) includes the refused reuse of ID 3 after rollback and the accepted ID 4.
- Pass on HEAD: repository `migration_runtime`, harness TX11.

Decision N5 (non-blocker, owner to ratify): a commit that is undone (scenario D) leaves its IDs unretired. A fresh
`world.migrate_plan` with `add_field id 3` returns `valid: true` (`SU_E_RESOURCE_LIMIT` was the undone commit's code).
This is consistent with "an unacknowledged attempt never happened", and no durable artifact carries ID 3 after the undo.
Read literally, contract B5 ("never reused within a world lineage") and section 5 would refuse it. The contract should say
which reading is intended, and the handoff should list it as a deviation.

### 5.3 BLK-3 (i): no-room test (resolved)

The repository test `run_undo("noroom")` exercises the no-room path. Mutant M10 (no undo on no-room: the reply is an error
but the migrated state stays) is killed by the repository test (12 check failures) and by scenario A (94 failures).

### 5.4 C1: `systems_unresolved` (non-blocker)

Scenario F: a world created with `capsule_paths` for `demo.movement` and `demo.drift`; the gen-2 candidate for
`demo.movement` is resubmitted and nothing is dropped:

    commit without drop: code=SU_E_UNSUPPORTED_MIGRATION msg=systems_unresolved: an installed system is neither replaced nor dropped
    digest after the refusal equals the pre-commit digest.
    commit with drop_systems ["demo.drift"]: ok.

So the C1 path is reachable through the control plane and works. No repository test covers it. The contract section 4
refusal is implemented (`src/runtime/worker.c`, `systems_unresolved` checks). Non-blocker; add the scenario to the
repository tests.

### 5.5 C2 (non-blocker)

Access-growth refusal is not in the contract (handoff deviation 2). Mutant C2 remains unkilled. Non-blocker.

## 6. Open items: native layout invalidation and rollback test strength

### 6.1 BLK-3 (iii): native layout invalidation (blocker)

Contract section 10 lists, as acceptance evidence, "layout invalidation (native artifacts retired, a specialize in flight
cannot install, bindings regenerated, replacement systems bit-identical to a control world on unaffected data)". Section 4
(B13) requires the commit to release every artifact whose layout generation differs, and requires that a specialization
begun before the commit cannot install after it.

What exists:
- `tests/native/qualification/test_specialize_inflight.c` interleaves `candidate.install` and `world.close` between a
  specialization's admission and its install (`SU_TEST_INTERLEAVE`, seam in `src/agent/su_agent.c`). It does not
  interleave `world.migrate_commit`.
- `candidate.install` of a new semantic revision drops the kernel (harness TX3 "swap").
- Smoke test (scenario-independent, `native_mig.py`, section 8): `world.specialize` (scalar) on a world, then
  `world.migrate_plan`, `capsule.submit`, `candidate.evaluate`, `world.migrate_commit` on that world. The commit was
  acknowledged, the execution tier after the commit is `vm`, and five ticks after the commit the columns equal a VM-only
  control world that received the same migration (`RESULT tick mismatches: 0`). This shows the basic path works; it is
  not the acceptance test.

Missing test (to be written in the native lane, `tests/native/qualification`, modelled on `test_specialize_inflight.c`):
1. A new interleave mode in the test supervisor (for example `SU_TEST_INTERLEAVE=migrate`) that issues
   `world.migrate_commit` of a passed gen-2 candidate between a native admission and its install.
2. Assertions: the install is refused (generation or artifact-hash mismatch, not `ok`); the world's tier after the commit
   is `vm`; the old artifact is released (`su_nv_state` for the old `seq` is `NONE` or `RETIRING`, or an equivalent
   stats counter, which must be exposed through an existing internal stat, since `su_nv_*` is in-process to the worker);
   and a specialization for generation 2 can install afterwards.
3. Ticks after the commit equal a control world that was migrated without a native kernel, column by column, for the
   unaffected fields.

If the owner waives this item for M2-01, the waiver must be written in the contract or the handoff. Note also that
`su_nv_install` checks only the artifact state, not the generation (contract B13 says it refuses a generation mismatch).
The generation check is done one level up, in `h_internal_specialize_install` (`expected_layout_generation` and a recompile
whose hash must equal the admitted hash), so a pre-commit artifact cannot install after the commit. That is sound, but
the contract wording names `su_nv_install`.

### 6.2 Gap (c): rollback digest and test strength (non-blocker)

Mutant M11 (rollback restores the checkpoint before the pin: `w->pin = w->ck[w->ck_count >= 2u ? w->ck_count - 2u : 0u]`):
- Repository `test_migrate_runtime` passes (0 check failures). Its `run_rollback` checks only that the digest after the
  rollback differs from the digest after the commit. It does not compare the restored state.
- Harness TX11 **fails** on the mutant: `python3 -I tests/harness/run_m0.py --binary <S>/hooks/sutekh --no-build` on the
  mutated scratch copy gives `TX11 failed ... (checks 17, failed 1)` and `totals: passed=22, failed=1`. The rolled-back
  state at tick 2 and the pre-migration schema are compared against the reference evaluator. So the exact-state check exists, but
  only in the harness, and the ctest comment ("the world has the pre-migration field table again") claims more than the
  test checks.

Verdict: non-blocker, because harness TX11 compares the columns and schema. Recommend adding the same comparison to the
repository test (the restored digest cannot equal the pre-migration digest because the revision moves; compare columns,
schema and lineage instead).

Mutant M04 (undo leaves the pin armed) is killed by scenario C (13 failures) and survives the repository test: rollback
after an undo would be accepted. This is the same availability question as N1.

### 6.3 N2: allocation-failure injection (blocker against section 10)

Contract section 10 requires "memory failure injected at each allocation point leaving the world bit-identical and
active". There is no allocation-failure seam in `su_migrate.c` (23 `malloc`/`calloc` sites) and no test that injects one
(`grep -rn alloc tests/migration-runtime` finds only a helper). Neither the M2-audit nor the handoff mentions this item.
Verdict: blocker unless the owner waives it in writing. The test needs a seam that fails the n-th allocation in
`su_migrate_plan` and in the prepare path, and a check that the digest equals the pre-commit digest and that a retry succeeds.

## 7. Items not re-verified or not run

- BLK-3 (ii): 1,000,000-row timing and admission numbers. The handoff reports one observation (commit `total_ns`
  1428 to 1635 ms, plan admitted with `bytes_peak` 45,000,000). I did not re-run it.
- N3: TSan on the migration path (contract section 10 lists it). Not run. The migration code runs in one worker thread;
  the chunked-tick threads run inside ticks and are joined, so TSan would have little to check. Needs a run or a waiver.
- ASan/UBSan and valgrind (handoff section 6 of M2-01b and the fixes handoff): not re-run.
- Snapshot 0.3 golden and rename-only digest sensitivity: covered by the `migration` ctest (`tests/migration/test_migrate.c`,
  `test_snapshot03`, and the rename digest check at line 419), which passes in `ctest` (20/20). I did not inspect the
  individual checks beyond that.
- "Field IDs refused across a supervisor restart (snapshot 0.3 + recovery)": the repository tests cover kill and recovery
  (`run_id_retirement_after_checkpoint`), not a new process reading a 0.3 snapshot. Not run.
- Transform wrap-around through the runtime: covered only by the model test (`migration_model`), not through the binary.
- C1 mutant (accepting an unresolved system): not run as a mutant; the refusal is reached and verified by scenario F.
- Mutants M05 and M06 (below) are not killable by any test I have.

## 8. Mutants and seams (exact commands)

Driver: `<S>/mutants.py` (one edit per mutant, sources restored from `<S>/backup/` after each run, hooks build of the
scratch copy, then `<S>/hooks/reaudit_migrate <S>/hooks/recovery_worker <S>/src <run dir>` and
`<S>/hooks/test_migrate_runtime <S>/hooks/recovery_worker <S>/src <run dir>`). `reaudit_migrate` is the new scenario
file (`<S>/src/tests/migration-runtime/reaudit_main.c`, it includes `test_migrate_runtime.c` and reuses its helpers).

Result of the baseline (M00): `reaudit_migrate` 0 failures, repository test 0 failures. Final `reaudit_migrate` run on
the restored baseline: `REAUDIT checks=319 failures=1` (the one failure is N1, scenario G).

| mutant | reaudit_migrate | repository test_migrate_runtime |
|---|---|---|
| M01 undo keeps the `LOG_MIGRATE` entry | killed (103) | killed (26) |
| M02 undo does not rebuild the worker | killed (93) | killed (12) |
| M03 undo keeps the pre-commit revision | killed (103) | killed (26) |
| M04 undo leaves the pin armed | killed (13) | survived |
| M05 undo does not restore system records | survived | survived (equivalent, see below) |
| M06 undo does not restore the migrated flag | survived | survived (equivalent under current recovery) |
| M07 rollback merge drops the undone field IDs | survived (not in scope of my scenarios) | killed (27) |
| M08 M07 plus the `undone_ids_retired` check removed | survived (not in scope) | killed (8) |
| M09 swap check removed before the event append | killed (2; scenario E) | survived |
| M10 no-room after commit does not undo | killed (94) | killed (12) |
| M11 rollback pin is the checkpoint before the pin | survived | survived; harness TX11 killed (1) |
| M12 undo does not restore the hash record | killed (103) | killed (26) |

M05 and M06 are equivalent under the current recovery: `recover` re-derives `w->sys` from the checkpoint manifest
(`memcpy(w->sys, base->sys, ...)` in `rebuild_world`), and `w->migrated` is not read by the rebuild (the rebuild path is
chosen from the checkpoint's own flag). The explicit restores in `undo_migration` are therefore redundant. Record them as
latent, non-blocker. M11 is the rollback test-strength gap (6.2).

Seams (scratch copy only): `SUTEKH_TEST_REAUDIT_KILL_METHOD`/`_TOKEN` (worker, `reaudit_kill_method`, armed by
`SUTEKH_TEST_REAUDIT_UNDO_METHOD`/`_TOKEN` in `undo_migration`), `SUTEKH_TEST_REAUDIT_SWAP_REFUSE`
(`su_migrate_swap_check`). The scratch copy was built with `cmake -S <S>/src -B <S>/hooks -DCMAKE_BUILD_TYPE=Release` and
`cmake --build <S>/hooks --target reaudit_migrate recovery_worker test_migrate_runtime -j4`.

Native smoke test: `python3 -I <S>/native_mig.py` against `<S>/nat/sutekh serve` (native build, cwd = worktree). Output
(abridged): `specialize ... "execution_tier": "native_scalar"`; both commits acknowledged (`layout_generation: 2`);
`tier after commit (native world): ['vm']`; `RESULT tick mismatches: 0`. Its migration document is written under
`<wt>/build/reaudit-native/` (git-ignored) and removed afterwards.

## 9. Scratch and worktree hygiene

- The worktree was changed only by this file (committed). `build/` (harness output, git-ignored) was created by my harness
  runs and removed. `third_party/asmjit` (git-ignored, created by `tools/bootstrap_asmjit.py`) was left in place.
- Scratch build directories, mutant copies and run directories were deleted at the end. The scenario source, the mutant
  driver, the native smoke script and the logs were kept in `<S>/keep/` for reference.

## 10. Next work (for the owner and the M2-01 fixer)

1. Decide N1 (keep the last committed pin until a commit succeeds, or waive in writing) and fix or record it.
2. Write the native migration-interleave test of 6.1 (or waive BLK-3 (iii) in writing).
3. Add an allocation-failure seam and the section 10 test (N2), or waive it in writing.
4. Add the rollback column/schema comparison and the C1 scenario to the repository tests (6.2, 5.4).
5. Record N4 (recovery exhaustion inside the undo, one policy) and N5 (undone IDs reusable) as decisions in the contract
   or the handoff.
6. Re-run BLK-3 (ii) and TSan, or record waivers.
