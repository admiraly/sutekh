# Handoff: spec-conformance fixes (audit items 2, 4, 6, 7)

Date: 2026-10-12. Source audit: `planning/audit/spec-conformance-audit.md` (sections 2 and 5).
Scope: code-level deviations from the original specification. Original normative documents were not
edited; amendments are listed below as proposals for owner approval. Nothing was pushed.

Numbering: the user's task items (1) to (4) are audit items 2, 4, 6 and 7 (with 9 and 14 examined under item 4).

## Commits (branch of worktree `agent-a86297bf77b9c5948`, author admiraly, no co-author trailers)

| Commit | Audit item | Subject |
|---|---|---|
| `58931f5` | 2 | Analyse effects on every installed-set change |
| `41c68e5` | 4 | Enforce capsule capabilities and record system_rev |
| `4172c40` | 6 | Structured verification diagnostics: error.detail |
| `16bf514` | 7 (and 8 note) | Implement tests.run and job.status as synchronous jobs |
| (this commit) | all | This handoff |

### Commit 58931f5: effects on every installed-set change (audit 2)
- Modified: `src/runtime/worker.c` (`check_system_effects_ovr`, wrapper `check_system_effects`, `install_effects_check`,
  call in `h_candidate_install` via `req.effects_check`, call in `h_internal_world_migrate_prepare` after
  `su_sched_finalize(&nsched)`, call at the end of `rebuild_schedule` which serves checkpoint restore),
  `src/runtime/reload.h` (`SuReloadRequest.effects_check`, `effects_ctx`), `src/runtime/reload.c`
  (`EFFECTS_VERIFIED` now reached after the analysis, check placed after the layout check and before preparation).
- Modified tests: `tests/reload/test_reload.c` (`test_install_effects`), `tests/migration-runtime/test_migrate_runtime.c`
  (`run_effects_refusal`), `tests/reload/make_fixtures.py` plus new fixtures `tests/reload/sys_b_conflict.*`,
  `sys_a_cycle.*`, `sys_b_overdecl.*`, `sys_a_gen2_cycle.*`, `sys_b_gen2.*`, `sys_b_gen2_conflict.*`.
- Existing fixtures are byte-identical after regeneration (`git status` showed only additions).
- Refusals: write/write or read/write conflict and ordering cycle use `SU_E_SCHEMA_MISMATCH` and `SU_E_INVALID_ARGUMENT`
  (the create-time codes); declared != derived uses `SU_E_SCHEMA_MISMATCH`. The world is unchanged (revision, tick, PID,
  both semantic hashes checked).
- Host entries: the analysis already used declared sets (`host_declared`); `check_system_effects_ovr` keeps that.
- Specialize install (`h_internal_specialize_install`) does not change the set of systems or their access and was not
  given a new check.

### Commit 41c68e5: capabilities and system_rev (audit 4)
- Modified: `src/ir/su_lir.c` (`check_capsule` accepts optional `capabilities` and `system_rev`; enforcement after the
  per-row limits; `m.has_system_rev`, `m.system_rev`, `m.has_capabilities`, `m.capabilities` set on success),
  `src/ir/su_lir.h` (`SU_LIR_CAP_DESPAWN_SELF`, `SU_LIR_CAP_EMIT_EVENT`, the four fields in `SuLirModule`),
  `src/runtime/worker.c` (`declared_system_rev` in `write_manifest`, present only when declared).
- New fixtures: `tests/fixtures/caps/` (`make_caps.py`, `manifest.json`, `base.lir.json`, 11 capsule variants).
  Test: `tests/ir/test_ir.c` (`test_caps`). Reload: `sys_a_rev.*`, `sys_a_badcap.*`, `test_world_create` and
  `test_submit_and_evaluate` in `tests/reload/test_reload.c`.
- Verdicts: unknown name `SU_E_CAPABILITY_DENIED` (shape stage); used operation without grant `SU_E_CAPABILITY_DENIED`;
  malformed list or `system_rev` `SU_E_TYPE`. Capabilities and `system_rev` are not in the semantic hash
  (the `ok_valid` pin `cbd6d237...` is unchanged).
- Behaviour decision (see amendment A1): enforcement applies only when the capsule carries `capabilities`.

### Commit 4172c40: structured diagnostics (audit 6)
- Modified: `src/ir/su_lir.h` (`SuDiag.hint`, `expected`, `actual`; sizes `SU_DIAG_HINT_BYTES`, `SU_DIAG_VALUE_BYTES`),
  `src/ir/su_lir.c` (`hint_for` one hint per status code, `set_diag_ev`, expected/actual at the undeclared-access,
  capability and resource-limit checks), `src/agent/su_agent.c` (`verify_detail`, attached in `h_capsule_submit`),
  `docs/AGENT_RPC.md` section 5.
- The `error.code`, `error.status` and `error.message` bytes are unchanged; `error.detail` is additive, carrying
  `json_pointer`, `source_path` (project-relative), `source_sha256`, `capsule_sha256` and, when non-empty, `system_id`,
  `expected`, `actual`, `hint`. Hashes are computed over the bytes that failed.

### Commit 16bf514: tests.run and job.status (audit 7)
- Modified: `src/agent/su_agent.c` (the evaluation body is now `candidate_evaluate_run`; `candidate.evaluate` keeps its
  response bytes; new `h_tests_run`, `h_job_status`, `AgentJob` table of 16 with heap results; routing; capability list),
  `tests/reload/test_reload.c` (`test_jobs`, `file_sha256`), `docs/AGENT_RPC.md` section 2.
- Jobs are synchronous: `state` is always `completed`; `cancellable` and `cancelled` are `false`.
- Inputs: `suite_sha256` (test file), `capsule_sha256`, `source_sha256`; checked against hashes computed by the test
  from the files on disk.

## Commands and results

All commands run from the worktree or from scratch copies. Build dirs are outside the repository.

| Check | Command | Result |
|---|---|---|
| Release, `-Werror` | `cmake -S . -B <scratch>/base-rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-Werror` then build | clean (0 warnings) |
| Release ctest | `ctest -j4` in release tree | 25/25 passed (final run on HEAD) |
| Debug, `-Werror` | `cmake -S . -B <scratch>/s-dbg -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS=-Werror` | clean; ctest 25/25 |
| Full harness (release) | `python3 -I tests/harness/run_m0.py --binary <release>/sutekh --no-build` | `totals: passed=23, failed=0, not_run=1, unavailable=3` (final run on HEAD). T00 is `not_run` under `--no-build`; TX3/TX4/TX10 are native-only |
| Harness self-test | `python3 -I tests/harness/run_m0.py --self-test` | 261 checks, 0 failures |
| Capsule schema test | `python3 -I tests/schema/test_capsule_schema.py --baseline-schema <schemas/capsule.schema.json at 49b7303>` | `passed`, 111 files compared, 0 mismatches, 35 negative cases |
| Native build | `python3 tools/bootstrap_asmjit.py` in a scratch copy of the tree (without `.git`); `cmake -DSUTEKH_NATIVE=ON -DCMAKE_C_FLAGS=-Werror` | bootstrap ok; build clean; ctest 37/37 |
| Native harness | `run_m0.py --binary <native>/sutekh --no-build` (scratch copy) | `passed=26, failed=0, not_run=1, unavailable=0` |
| ASan+UBSan build, full ctest | `-fsanitize=address,undefined` (Debug) | 22/25; `control`, `containment`, `migration_runtime` fail |
| Same, unmodified baseline `49b7303` | identical flags, `git archive` extraction | the same three fail: pre-existing |
| New-coverage targets under ASan+UBSan | `ASAN_OPTIONS=detect_leaks=0 ctest -R "^(migration_runtime|control|reload|ir)$"` | 4/4 passed; no sanitizer report |

Test counts at HEAD: `test_reload` 2840 checks (2822 after item 3); `test_ir` 211 passed; `test_migrate_runtime` 412 checks.

### Mutation results (each mutant applied to a copy of the tree; "KILLED" means a test failed, not a build error)

| Mutant | Commit | Result |
|---|---|---|
| e1 install hook not wired (`req.effects_check = NULL`) | 58931f5 | KILLED (`reload`) |
| e2 install analysis is a no-op | 58931f5 | KILLED (`reload`) |
| e3 migration prepare analysis dropped | 58931f5 | KILLED (`migration_runtime`) |
| e4 install analyses the old module, not the candidate | 58931f5 | KILLED (`reload`) |
| e5 checkpoint-restore analysis dropped | 58931f5 | **SURVIVED**, equivalent under the public API (see below) |
| c1 grant enforcement skipped | 41c68e5 | KILLED (`ir`) |
| c2 unknown capability accepted | 41c68e5 | KILLED (`ir`, `reload`) |
| c3 system_rev not recorded | 41c68e5 | KILLED (`ir`, `reload`) |
| c4 despawn grant not enforced | 41c68e5 | KILLED (`ir`) |
| c5 `declared_system_rev` not shown | 41c68e5 | KILLED (`reload`) |
| d1 detail not attached | 4172c40 | KILLED (`reload`) |
| d2 capability hint empty | 4172c40 | KILLED (`ir`, `reload`) |
| d3 source hash is the capsule hash | 4172c40 | KILLED (`reload`) |
| d4 expected value missing | 4172c40 | KILLED (`ir`) |
| d5 `json_pointer` dropped from detail | 4172c40 | KILLED (`reload`) |
| j1 suite hash dropped | 16bf514 | KILLED (`reload`) |
| j2 job not kept | 16bf514 | KILLED (`reload`) |
| j3 job id not advanced | 16bf514 | KILLED (`reload`) |
| j4 capsule and source fingerprints swapped | 16bf514 | KILLED (`reload`) |
| j5 `tests.run` not routed | 16bf514 | KILLED (`reload`) |

e5 (equivalent mutant): the checkpoint restore rebuilds a schedule from revisions that were analysed when installed.
Every install and migration path now refuses a conflicting or cyclic set before it is published, so no checkpoint can
record one through the public API. The check is therefore defence in depth and cannot be reached by a test. It is kept.

## Hashes (source at HEAD of this handoff; SHA-256, first 16 hex characters)

Modified source: `src/runtime/worker.c` ce371d3ddf8659d8, `src/runtime/reload.c` 2c4a5f05cb9f3e6d,
`src/runtime/reload.h` 9597832eb68d4a2c, `src/ir/su_lir.c` e35751e5333f31a7, `src/ir/su_lir.h` b5ea6930696c851b,
`src/agent/su_agent.c` 1d8b08aef2464afb, `docs/AGENT_RPC.md` c134153452bf0859,
`tests/reload/test_reload.c` 0e01f166614012e3, `tests/reload/make_fixtures.py` 534c3c5e41ddcb20,
`tests/ir/test_ir.c` 105e340b6f13f809, `tests/migration-runtime/test_migrate_runtime.c` 4948c3b475aca816,
`tests/fixtures/caps/make_caps.py` 81f9d501a95fde3e, `tests/fixtures/caps/manifest.json` 0a5d022ec7a46cfc.
Full list of the 45 changed paths: `git diff --name-only 49b7303..HEAD`.

Artifacts: release `sutekh` 7563ecf3faa7d282246b... (full hash in the scratch build, not committed);
native `sutekh` 0f28b8655e0233397b00... (scratch copy of HEAD with AsmJit fetched by `tools/bootstrap_asmjit.py`).
AsmJit was not committed; `third_party/asmjit.lock.json` was written only in the scratch copy.

## Unrun checks and gaps

- Dev profile (`tools/dev.py`) and clang builds: not run (same as the audit's section 6).
- Leak checking on the new migration path: LeakSanitizer reports `does not work under ptrace` in this environment, and
  `containment` fails with the ASan shadow reservation under the test's address-space limit. Both fail on the baseline too.
  The new migration and capability tests ran under ASan+UBSan with `detect_leaks=0` only.
- Windows and non-x86-64 hosts: not run.
- Harness reports: the default `--report-dir` is `build/m0_harness` inside the repository; the runs in this pass were
  pointed at scratch directories or removed with the worktree `build/` directory.
- `planning/status.json` and `planning/tasks.json` were not updated. Governance records are owner-controlled.

## Items not done, and why

1. **`world.run` and `world.pause` (audit 7, part).** Both need a free-running loop with pacing policy (AGENT_RPC
   section 2). That is a new subsystem, and a stub would violate the rule against placeholder success. They remain
   `SU_E_UNSUPPORTED_METHOD` (`tests/control/test_control.c` checks this). Proposal A5.
2. **Request-id idempotence (audit 9).** The rule "same id with the same payload returns the previous outcome; a different
   payload is an error" conflicts with existing clients. `tests/reload/test_reload.c` reuses `ins` and `stp` for different
   payloads, and `tests/harness/run_m0.py` reuses ids such as `q-1` and `x`. Implementing the rule as written would change
   M0-era behaviour, which the constraints forbid. Proposal A4.
3. **`replay.compare` (audit 14).** It is listed under "later methods" (AGENT_RPC section 3). Its comparison semantics are
   not defined, and the M2 closure is an owner decision. Not implemented. Proposal A7.
4. **Structured detail for worker-side errors (audit 6, remainder).** `world.create`, `candidate.install` and migration
   verification errors still have no `error.detail`. The worker envelope would need the same members, a change to the
   worker protocol. Only the `capsule.submit` path carries detail now.
5. **Mandatory capability grants (audit 4, remainder).** A capsule without `capabilities` is not checked, so a capsule that
   omits the key can use `despawn_self` or `emit_event`. Making the key mandatory changes the verdicts of about 40 existing
   IR 0.2 fixtures and inline test capsules (`tests/fixtures/ir02`, `tests/fixtures/entities`, `tests/control`,
   `tests/entities`, `bench`). The owner asked that existing verdicts stay unchanged, so this was not done. Proposal A1.
6. **Declared `system_rev` is not checked against the engine counter**, and host records (`capsules/rts/host`) do not
   surface it. Host records are loaded by `su_host_load`, a separate loader. Proposal A3.
7. **Capability naming (audit 4).** The M3 contract names the damage grant `rts.damage`, while the implementation names
   capabilities by IR operation (`despawn_self`, `emit_event`). Proposal A2.
8. **Expected and actual values** are set at the undeclared-access, capability and resource-limit checks only, not at
   every verifier error. Hints are one per status code, not per check.
9. **Job persistence.** Jobs live in the supervisor process, like candidates; a restart loses them. `job.status` on a lost
   id returns `SU_E_INVALID_ARGUMENT`.

## Amendment proposals (owner approval required; original spec files not edited)

- **A1. Capability grants.** Either (a) make `capabilities` mandatory for IR 0.2 operations (absent = none), migrating the
  existing 0.2 capsules to declare their grants, which keeps their accept/refuse verdicts; or (b) amend the schema text
  and LIVE_IR to say that an absent key is "not checked" until migration. Current behaviour is (b) without the amendment.
- **A2. Capability vocabulary.** Name grants by operation (`despawn_self`, `emit_event`) or by system grant (`rts.damage`).
  M3 contract section 5.5 uses `rts.damage`; the engine uses operation names.
- **A3. `system_rev`.** Define it as declared metadata (current behaviour) or as a checked value. Specify whether host
  records carry it.
- **A4. Request-id idempotence.** Scope the rule to requests with an explicit client-supplied `idempotency_key`, or defer it
  to the M1 broker. The current M0 clients reuse ids.
- **A5. `world.run` and `world.pause`.** Move to a later gate, or define `world.step` as the M0 pacing mechanism.
- **A6. Jobs.** AGENT_RPC section 2 says `candidate.evaluate` returns an asynchronous test job ID; the M0 implementation is
  synchronous, and `tests.run` / `job.status` now return completed records. Record this as the M0 behaviour.
- **A7. `replay.compare`.** Define the comparison (for example canonical digests of two worlds at the same tick, or two
  replay logs), name the milestone, and decide whether M2 closure requires it (audit 14).
- **A8. Error detail.** Add `error.detail` members (`json_pointer`, `source_path`, `source_sha256`, `capsule_sha256`,
  `system_id`, `expected`, `actual`, `hint`) to AGENT_RPC section 5 as the optional detail object for verification errors.
- **A9. Status codes 18 to 21** (audit 18) and the audit's items 8, 10 (`candidate.evaluate` synchronous; method renames)
  remain owner items and are unchanged by this work.

## Next work

1. Owner decision on A1 and A2 (capability grants and naming). Then, if approved, migrate the 0.2 fixtures.
2. Extend `error.detail` to worker-side verification errors (world.create, candidate.install, migration).
3. Owner decision on A4 and A7 (idempotence scope, replay.compare definition).
4. Dev-profile and clang runs; a leak-checked run of the migration path on a host where LeakSanitizer works.
