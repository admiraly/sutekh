# M2 independent adversarial audit: M2-00 (entities) and M2-01 (migrations)

Auditor: independent adversarial auditor (did not write this code). Base: HEAD `dd236577a44755b2c2c26cdec62deabc4baf925f`
(`M2-01b: migration runtime`), worktree `agent-ab84b345d1fc5faf6`. Product code was not edited. Mutants and fault
injections were applied only to scratch copies of `git archive HEAD` (outside the repository). Scratch root:
`/tmp/claude-0/-home-user-sutekh/82533bc6-e6ca-564a-ba43-39dd3091bafa/scratchpad/` (`audit/` holds the drivers).

Read: AGENTS.md, ENGINE_CONSTITUTION.md, `planning/contracts/M2_ENTITIES_CONTRACT.md` (r2.2),
`M2_MIGRATION_CONTRACT.md` (r2), `planning/audit/M2-00-audit.md`, `planning/handoffs/2026-10-12-m2-00-fixes.md`,
`2026-10-12-m2-01a.md`, `2026-10-12-m2-01b.md`. The M2-01a handoff was not re-read in full; its snapshot 0.3 claims were
checked only through the runtime.

## 1. Verdict

- **M2-01 (migrations): not accepted.** The commit, rollback, recovery and refusal paths behave as the contract says on
  every path I could drive, with two exceptions that are contract violations: the double fault in the post-commit
  checkpoint retry (blocker 1) and identity of field ids after a rollback (blocker 2). Three acceptance-test items in
  contract section 10 are absent (blocker 3).
- **M2-00 (entities): conditionally accepted.** The four blockers of the earlier audit (`planning/audit/M2-00-audit.md`)
  are addressed in code and tests. I re-killed MX18 and MX19 on the current tree. The remaining condition is native
  evidence (TX10, native ctest), which I could not reproduce here because AsmJit is absent.

## 2. Blockers

**BLK-1. Failure reply while the migration is durable (contract gap (a)). Blocker.**
Contract section 1 says a migration "fail[s] with the old world untouched", and section 4(5) says a commit without a
durable post-commit checkpoint is never acknowledged. With the post-commit checkpoint failing twice, the reply is
`SU_E_WORKER_FAILED` with `retry: true`, yet the world is migrated and stays migrated.
- Evidence: `exp_fault.py died2` (fault-injected build; the injection now runs before the capture and retake, so no
  checkpoint is recorded, which is the faithful double-fault state). Reply: `SU_E_WORKER_FAILED`, message "the migration
  stays in the replay log". After a worker kill and recovery: `layout_generation 2`, `health` present, lineage 1, equal to
  the post-reply state, not the pre-migration state. A client that retries gets `SU_E_STALE_REVISION`, since the revision
  moved.
- Fix options (owner decision): on a second failure, roll back to the pin (`LOG_ROLLBACK`) so the reply can truthfully
  say "rolled back", or return an explicit outcome-unknown error that names the migration hash. Add a forced double-fault
  test. The handoff (deviation 6) names this gap; I agree it is a blocker.

**BLK-2. Field ids are reusable after rollback (contract B5 and section 5). Blocker.**
Contract B5: field ids "never reused within a world lineage". Section 5: "a rolled-back lineage keeps the ids retired by
the undone migration (monotonic lineage)". The implementation keeps only ids retired by `remove_field`. Add-field ids
are forgotten on rollback, and the lineage is reset.
- Evidence: `exp_cycles.py` (25 commit/rollback cycles): after each rollback `migration_lineage` is empty again. Then
  `world.migrate_plan` with `add_field {id:3, name:"shield"}` returns `valid: true`, although `id 3` ("health") was part
  of the rolled-back lineage. The rollback reply says `"lineage":"retained"` (`su_agent.c`, `h_world_migrate_rollback`),
  which contradicts `world.inspect`. (I did not run a commit of the "shield" plan.)
- Fix: keep a monotonic ledger of every field id used in a lineage (add, rename, remove) across rollback; keep the lineage
  monotonic or stop reporting "retained"; add a test for re-add after rollback.

**BLK-3. Contract section 10 acceptance evidence missing. Blocker (owner may waive part of the native item).**
- (i) "post-commit checkpoint failure rolls back". Behaviour is correct (see gap (b) below), but no test forces it. Mutant
  B3 (no rollback on no-room) survives every migration/recovery ctest. A binary budget scan (`SUTEKH_SPOOL_BYTES_MAX`
  from 110 KB to 4 MB) never reached the branch: the plan's bound covers the post checkpoint. The fault-injected run
  shows the path is correct (state equal to pre-migration, also after kill and recovery).
- (ii) "1,000,000-row migration timing reported separately" and "exact admission numbers ... 1,000,000-row case reported
  as admitted or refused with the bound used". Not run (handoff: "not measured").
- (iii) Native layout invalidation (native artifacts retired, in-flight specialization cannot install, bindings
  regenerated; section 4 B13). Not run: AsmJit absent, so TX3, TX4 and TX10 are `unavailable`. This needs either the
  native build or an owner waiver for M2-01.

## 3. Gap verdicts (the three named gaps)

- **(a) Double fault during the post-commit checkpoint retry.** Confirmed, blocker BLK-1.
- **(b) Post-commit no-room rollback untested.** Behaviour correct when triggered. Verified with the fault-injected
  build (`AUDIT_POST_NOROOM`, injected before the capture): reply `SU_E_RESOURCE_LIMIT`, detail
  `{reason: post_commit_checkpoint, rolled_back_to: pinned_pre_migration_checkpoint}`, state equal to pre-migration
  (layout 1, lineage 0, no `health`), and equal again after a worker kill and recovery. Not testable through the real
  binary budget (see BLK-3 (i)). Missing acceptance test: blocker.
- **(c) Rollback not digest-checked.** Confirmed by reading code. `sync_after` (`su_agent.c`, around line 1158) records the
  worker's digest after `LOG_ROLLBACK` as the acknowledged state without comparing it to anything. `Ckpt` stores only the
  snapshot file's sha256, not a state digest, so the pin cannot be compared. Correctness rests on the snapshot's file sha
  and on the column, schema and tick comparisons in TX11 and the C test. **Non-blocker**; recommend storing the pin's
  digest and comparing after restore.

## 4. Non-blockers (fix or record)

- **NB-1. Survivors of my mutants (negative paths untested).** C1 `systems_unresolved` (installed system neither replaced
  nor dropped) survives; I did not establish whether a multi-system world can be created through the control plane, so
  reachability is open. C2 access growth refusal survives (not in the contract; handoff deviation 2). C3 replacement
  generation check survives, but the binary refuses a generation-1 replacement with `SU_E_SCHEMA_MISMATCH` and the world
  stays at layout 1 (checked). B4 unevaluated candidate survives, but the binary refuses it with
  `SU_E_UNSUPPORTED_MIGRATION` (checked). B1 (migrated flag after rollback) survives, but recovery after a rollback
  followed by 70 ticks (a post-rollback checkpoint) reproduces the exact state (checked).
- **NB-2. B2 (LOG_ROLLBACK tick exemption removed)** survives the ctest subset but is killed by harness TX11
  (`rolled-back state diverged from the worker`), which I re-ran on the mutant.
- **NB-3. Latent order defect in swap.** `h_internal_world_migrate_swap` (`src/runtime/worker.c`, around line 4438)
  appends `SU_EVENT_MIGRATED` to the entity event log before `su_migrate_swap` runs its refusal checks. A refused swap
  would leave the event in the log with the world unchanged. Not reachable today (the checks repeat prepare's), but the
  order should change.
- **NB-4. Commit reply shape.** Contract section 4 lists `timings:{plan_ns, prepare_ns, transform_ns, swap_ns,
  checkpoint_ns, total_ns}` and `bytes:{new_columns, peak}`. The reply has only `prepare_ns`, `transform_ns` and
  `checkpoint_ns`, and no `bytes`. Not listed in the handoff's deviations.
- **NB-5. Transform source not matched.** A `transform_format` document whose `source` names `other.lir.json` is accepted
  when a different LIR is supplied through `lir_path` (plan valid). Contract section 2 implies the names must agree.
- **NB-6. Zero-op migration accepted.** `ops: []` with `to = from + 1` is `valid: true`. The contract is silent. Decide
  and record; it forces a full resubmission.
- **NB-7. Fail-closed corruption with older checkpoints available.** A corrupted post-migration snapshot in the spool
  makes the world unrecoverable (`snapshot file does not match its recorded sha256`), although the pin and the log could
  rebuild it. Fail-closed is consistent with the M1 policy, but the world is lost. Record as a durability weakness.
- **NB-8. Inline migration form not implemented.** The contract's `migration_path | migration (inline)` alternative; only
  the path form exists (handoff deviation 1). Acceptable if the contract allows either.
- **NB-9. Misleading error.** A missing `migration_path` file returns `SU_E_IO` with the message "rejected by the project
  path policy".
- **NB-10. Defence-in-depth self-checks are equivalent.** Mutants A2 (dead rows copied from the old column), A5 (successor
  schema digest check) and A6 (buffer dead-row self-check) all survive. Despawn zeroes the columns and `put` zeroes dead
  rows, so the outcome is unchanged. The checks are still required by the contract's self-check list; add a test that
  puts a nonzero value in a dead row before migrating.

## 5. M2-00 (entities) verdict and evidence

- Earlier blockers: (1) contract section 6 partial items: N=8 merge (`test_entity_tick.c` `test_workers_equal` now covers
  N=8; harness TX8 covers N=1,2,4,8); dead slots after native kernels (TX10, native only, see unrun); dumps (changed
  expectations per the r2.2 ruling); despawn history recovery (MX18). (2) admission through the control plane (MX19,
  TX9). (3) MILESTONES record: not checked by me. (4) recovery with despawn history (MX18).
- Re-killed by me on HEAD (scratch copy `mutrepo2`, one edit each, restored after each run):
  - MX18 (`su_agent.c` `LOG_DESPAWN` -> `LOG_NONE`): `ctest -R recovery` fails (`rc 8`).
  - MX19 (`worker.c` admission call -> `SU_OK`): `TX9 failed: accepted, expected rejection` (`checks 8, failed 1`).
- Passing on HEAD (Release and Debug): `entities`, `world_entities`, `churn`, `entity_tick`, `cmdq`, `snapshot`,
  `snapshot_cp`, `control`, `recovery` (ctest). Harness TX7 (60 checks), TX8 (50), TX9 (15 self-tests and binary).
- Not reproduced: native lane (TX3 144 checks, TX10 20 checks, native ctest 29/29 claimed by the handoff); MX21 and the
  N=8/dead-slot mutants. Condition: reproduce the native lane on a machine with AsmJit before accepting M2-00.

## 6. Commands and results (exact)

Builds (all from the worktree; build directories in scratch; warnings grep = 0 for Release):
- `cmake -S <wt> -B <scratch>/rel -DCMAKE_BUILD_TYPE=Release && cmake --build <scratch>/rel -j8`: clean.
  `sha256(<scratch>/rel/sutekh) = e3c0cda73d2fa8cb1f11ddecd443c52f71e52aca6a5da76df5f8053d98210553`, identical to the
  M2-01b handoff.
- `ctest -j8` in `rel`: **100% passed, 20/20** (11.33 s).
- `cmake -S <wt> -B <scratch>/debugbuild -DCMAKE_BUILD_TYPE=Debug`, build clean, `ctest -j8`: **20/20** (13.72 s).
- `python3 -I tests/harness/run_m0.py --binary <rel>/sutekh --no-build`: **totals passed=23, failed=0, not_run=1, unavailable=3**
  (TX3, TX4, TX10: native build). TX11 22 checks passed. Timing lines: ir_prepare met, warm_edit_to_first_effect met.
- `python3 -I tests/harness/run_m0.py --self-test`: **259 checks, 0 failures**.
- Existing mutant script `tests/migration-runtime/run_mutants.sh` run on a scratch copy: **10 run, 8 killed**; M02 and
  M03 survive, as the handoff states (equivalent under the worker's own checks).
- Sanitizers, Debug build with `-fsanitize=address,undefined -fno-omit-frame-pointer`: `ctest -R "migration|recovery|reload|snapshot"`
  **7/7 pass**; no `AddressSanitizer` or `runtime error:` report. LeakSanitizer is unusable here ("does not work under
  ptrace"), so leak status is not established by ASan.
- Valgrind memcheck with leak check and child tracing on the Debug `test_migrate_runtime`: **77 checks, 0 failures**; all
  traced processes "All heap blocks were freed". One error: the worker sandbox's deliberate `execve` probe
  (`su_sandbox.c:154`) under valgrind, not a migration defect.

Refusal matrix (`exp_refuse.py`, binary, state compared before and after each request): 29 document cases and 3 path
cases. Every refusal left the world unchanged. Codes: oversize `SU_E_RESOURCE_LIMIT`; 65 ops `SU_E_RESOURCE_LIMIT`;
depth 20 `SU_E_RESOURCE_LIMIT`; duplicate key `SU_E_DUPLICATE_KEY`; unknown key and malformed/trailing JSON `SU_E_TYPE` and
`SU_E_PARSE`; unknown op `SU_E_UNSUPPORTED_MIGRATION`; wrong keys/types `SU_E_TYPE`; id 0 `SU_E_TYPE`; id/name/rename/remove
conflicts `SU_E_SCHEMA_MISMATCH`; `remove_field` of a used field: accepted with `valid: false, reason: field_in_use`; `to` not
`from+1` `SU_E_TYPE`; `migration_format 0.2` `SU_E_TYPE`; bad id characters `SU_E_TYPE`; traversal and absolute paths
`SU_E_CAPABILITY_DENIED`. Exception: `ops: []` is accepted (NB-6).

Transforms (`exp_xform.py`, `exp_xform2.py`, binary, `transform_path` and `lir_path`): stale generation 2->3
`SU_E_STALE_REVISION`; `writes:[99]` `SU_E_SCHEMA_MISMATCH`; duplicate writes `SU_E_TYPE`; malformed LIR and a store to
an undeclared field `SU_E_CANDIDATE_FAILED`; transform without `lir_path` `SU_E_TYPE`. A transform `health = position *
velocity` committed and matched the Python model on the pre-commit snapshot (`[2,10,1,0]`, MATCH). Note: wrap-around was
not exercised through the runtime (values are small).

Dead rows and spawn (`exp_dead.py`): despawned slot 1, then `add_field health default 100`: dead row `health = 0`, alive
rows `100`; `world.spawn` after the migration gives the new row `health = 100` (contract section 2).

Cycles (`exp_cycles.py 25`): 25 commit/rollback cycles, no state violations, no recoveries, worker PID constant, committed
bytes constant (110305), spool bytes stable at 5265 from cycle 5, spool files stable at 8. Only the re-add of id 3 is
reported (BLK-2).

Stage kills on the real hooked binary (`SU_TEST_FAULT_HOOKS` build, existing seams, `exp_stages.py`):
| stage | commit / op reply | state after fault | final after retry |
|---|---|---|---|
| migrate.prepare | `SU_E_WORKER_FAILED` | pre | commit ok, post |
| migrate.swap (published, reply not sent) | `SU_E_WORKER_FAILED` | pre | commit ok, post |
| migrate.capture | ok with `recovery` | post | post |
| migrate.step | step `SU_E_WORKER_FAILED` | post | post |
| migrate.rollback | rollback `SU_E_WORKER_FAILED` | post | rollback ok, pre |

Post-commit fault branches (`exp_fault.py`, fault-injected build from `git archive HEAD` plus env-gated injections,
injected before the checkpoint call so that no checkpoint is recorded): control ok; died1 (one death before the capture)
ok with `ops_replayed: 1`; died2 (BLK-1); noroom (gap (b), correct).

Random kills (`exp_random.py 80`, release binary, SIGKILL to the worker in a random sub-millisecond window): 80/80
trials ended `ok|post`, zero partial states. The window did not reliably land inside the commit (kills hit before or
after it), so this is a weak check; the stage kills above are the stronger evidence.

Corruption (`exp_corrupt.py`, `exp_corrupt2.py`): corrupted spool snapshot (post checkpoint): unrecoverable, fail closed
(NB-7). Corrupted capsule: unrecoverable, fail closed (reload verification). Corrupted logged migration document, replayed
after a double fault: `replay of a logged migration was rejected at prepare`, unrecoverable (fail closed, correct).
Corrupting the document when the post checkpoint exists does not exercise the replay (the checkpoint is the base).

Mutants (`mutants.py`, one edit per mutant on a scratch copy, rebuild, `ctest -R "migration|recovery|reload"`):
| mutant | result |
|---|---|
| A1 new column default ignored at prepare | killed |
| A2 dead rows copied from old column | survived (equivalent, NB-10) |
| A3 swap does not publish | killed (segfault) |
| A4 spool admission always admits | killed |
| A5 successor schema digest check removed | survived (equivalent, NB-10) |
| A6 buffer dead-row self-check removed | survived (equivalent, NB-10) |
| B1 rollback does not set migrated flag | survived (checked, NB-1) |
| B2 LOG_ROLLBACK tick exemption removed | survived ctest; killed by TX11 (NB-2) |
| B3 no-room after commit: no rollback | survived (BLK-3 (i)) |
| B4 unevaluated candidate accepted | survived (checked binary, NB-1) |
| B5 post-commit checkpoint skipped | killed |
| C1 installed system neither replaced nor dropped accepted | survived (NB-1) |
| C2 access growth accepted | survived (NB-1) |
| C3 replacement generation not checked | survived (checked binary, NB-1) |
A5 was first built with a mutation that left `nd` unused; that build failed (cause not captured). The re-run uses
`memcmp(nd, nd, 32u)`, which keeps `nd` used.

Re-checks of M2-00 mutants: MX18 killed (`ctest -R recovery`), MX19 killed (`TX9`).

Source hashes (first 20 hex of sha256, matching the M2-01b handoff): `src/world/migration/su_migrate.c`
`4f561191f651445d766a`; `src/agent/su_agent.c` `c134aecbcd18123523df`; `src/runtime/worker.c`
`c9ec49142e0c98ef5a7f`.

## 7. Unrun checks (not claimed)

- Native lane: TX3, TX4, TX10, native ctest (29/29 per handoff), native artifact retirement on migration (BLK-3 (iii)).
- 1,000,000-row migration timing and admission numbers (BLK-3 (ii)).
- TSan on migration code (contract section 10 lists ASan/UBSan/TSan).
- Snapshot 0.3 golden round trip and rename-only digest sensitivity (the migration ctests pass; I did not run these
  checks directly).
- Replay on a build that cannot reproduce the schema must fail closed (contract section 6): not tested.
- Transform wrap-around (u32 overflow) through the runtime.
- Mid-commit random kills with a reliable window (use a seam, as the stage kills do).
- LeakSanitizer (unavailable under ptrace); valgrind memcheck used instead.
- Full M2-00 mutant set (MX21, N=8 and dead-slot mutants).

## 8. Notes on the scratch drivers

Drivers are in `<scratch>/audit/` (`drv.py`, `exp_*.py`, `mutants.py`, `m200.py`, `b2_tx11.py`, `exp_random.py`).
Injected faults exist only in `<scratch>/faultrepo` (env-gated: `AUDIT_POST_NOROOM`, `AUDIT_POST_DIED1`,
`AUDIT_POST_DIED2`); the product tree is untouched. The double-fault result depends on those injections, but the reply
code path under test is the same as for a real death. The existing hooks cannot produce a double death.
