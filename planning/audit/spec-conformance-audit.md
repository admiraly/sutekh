# Spec-conformance audit (read-only)

Date: 2026-10-12. Auditor: read-only conformance pass, no source, spec or status file was changed.

## 0. Baseline and method

- **Original specification:** the documents as first committed in `d5f7271` ("Initialize Sutekh specification and implementation handoff"). Edits made later by implementers were compared against that baseline (`git diff d5f7271 HEAD`).
- **Audited tree:** worktree HEAD `b839066` ("Record M1 acceptance and M3 owner decisions"), branch of worktree `agent-a08504836ac13be44`.
- **Read in full:** `SUTEKH_FULL_SPEC.md` (convenience copy), `ENGINE_CONSTITUTION.md`, `ARCHITECTURE.md`, `LIVE_IR_SPEC.md`, `AGENT_PROTOCOL.md`, `MILESTONES.md`, `AGENTS.md`, `docs/*.md` (all 15 spec documents including `docs/TEST_SPEC.md` and `docs/AGENT_RPC.md`), `planning/contracts/M0_CONTRACT.md`, `M1_NATIVE_CONTRACT.md`, `M2_*`, `M3_RTS_CONTRACT.md` (decision table and sections 3, 4, 6, 13, 14). `SUTEKH_ALL_PROMPTS.md` was used as context only. Status, audit, handoff and task files were read where they bear on a requirement.
- **Run here (scratch builds outside the repository, deleted at the end):**
  - Release build from a clean `cmake -G Ninja` configure: 24/24 ctest.
  - Native build (`-DSUTEKH_NATIVE=ON`, AsmJit fetched in a scratch copy of HEAD by `tools/bootstrap_asmjit.py`): 36/36 ctest.
  - `tests/harness/run_m0.py --no-build` against both binaries.
  - `tools/dev.py build --profile release` and `run_m0.py --only T00` in a scratch copy of HEAD.
  - `tools/validate_pack.py` in a scratch copy of HEAD.
  - `sha256sum -c SHA256SUMS` in the worktree.
  - Live `serve` probes (engine.info, unimplemented methods, path policy, a deliberately invalid IR capsule).
- **Not repeated:** dev profile, sanitizers, clang, Windows, fuzzing (see section 6).
- **Statuses:** *Conforms* = observed or cited in code and tests; *Deviates* = implementation or contract differs from the original text (the note says whether it is documented and approved); *Not implemented* = named by the spec, absent from code; *Unverifiable* = needs evidence this pass did not produce.
- **Gate column:** the milestone that owns the requirement. M0-M3 are claimed done or in progress; M4 and M5 are not started.

Binaries observed (SHA-256): default release `95d6593b...412d02` (identical from the clean `dev.py` build and the CMake build); native release `2b9ea6cd...83ab4ff`. Toolchain: GCC 13.3.0, CMake 3.28.3, Ninja 1.11.1, Python 3.13.16. Host: 4 vCPU shared VM.

## 1. Summary counts

Counts are for the matrix in section 3 (one row per normative item; rows that name several items count once).

| Status | Count |
|---|---|
| Conforms | 139 |
| Deviates (includes partial and approved) | 49 |
| Not implemented | 18 |
| Unverifiable | 15 |
| **Total rows** | **221** |

Of the Deviates rows, the documented-and-approved ones (owner statement or owner decision recorded in `planning/status.json`) are marked **(approved)**. The rest are undocumented or documented without an approval record. The ranked list in section 2 covers the undocumented ones.

Headline results:

1. All 24 default ctest targets and 36 native ctest targets pass at HEAD. The harness entries (T00-T15 and TX1-TX11, 27 in all) give 23 passed, 0 failed, 1 not_run and 3 unavailable on the default build, and 26 passed, 0 failed, 1 not_run on the native build. The not_run entry is T00 under `--no-build`; T00 was run separately in a scratch copy and passed. The three default `unavailable` entries (TX3, TX4, TX10) need a native build, as the spec predicts for a non-native build.
2. The core no-build (C01), code-only swap (C02), and rejection (C03/T06) claims hold in code and tests.
3. The largest gaps are not in the M0 loop. They are: (a) the original text of several normative documents has been edited in place by implementers, and the combined copy was never regenerated; (b) code-only installs and migration commits do not re-check effects (C04, C06, C09); (c) M3 host operators run outside the verifier by design, which C04 and LIVE_IR section 1 forbid; (d) the capsule schema declares `capabilities`, `system_rev` and `host` that the engine does not read; (e) structured diagnostics are not produced; (f) several named RPC methods are absent or renamed without amendment.
4. Milestone closure: M0 was closed by owner decision (2026-10-11) and M1 and M2 by owner statements (2026-10-12), after independent audits concluded "cannot close" (M0, M1) or "NOT ACCEPT" (M2, before fixes3). The owner decisions are real governance acts, but the audits' open blockers are only partly recorded. `planning/status.json` and `planning/tasks.json` also disagree with each other (section 2, item 5).

## 2. Ranked list of undocumented or unapproved deviations

Ranked by impact on the original specification. "Remedy" names the code change or the amendment needed. Amendments to normative documents need owner approval; the code changes are within the M0-M2 invariants and do not need new approval unless noted.

| # | Deviation | Spec basis | Documented? | Approval on record | Remedy |
|---|---|---|---|---|---|
| 1 | Normative documents were edited in place by implementers. `LIVE_IR_SPEC.md` gained an "IR 0.2 (M2)" section (commit `52aaed4`) and a parenthetical in the 0.1 read/write paragraph; `docs/DATA_ABI_AND_STORAGE.md` gained sections 6.1 and 6.2 (`009224a`, `2426017`); `docs/AGENT_RPC.md` grew from about 70 lines to about 630 (sections 8 to 17). `schemas/lir.schema.json` (+106) and `schemas/capsule.schema.json` (+140) changed. The combined `SUTEKH_FULL_SPEC.md` was not regenerated, so a reviewer comparing against it sees a different specification. | Spec pack header: "Edit individual documents, then regenerate this copy." Constitution preamble: normative. AGENTS.md: shared contracts need owner approval. | Partly: section 6 of AGENT_RPC and the M2 contract cite the change. Not in the spec pack. | M2 owner acceptance covers the M2 behaviour, not the wording of the spec. M3 OD-16 covers the schema edit. | **Spec amendment (owner).** Either ratify these sections as a numbered amendment of the spec, or move them to `planning/contracts/` and restore the original text. Regenerate `SUTEKH_FULL_SPEC.md` and `SHA256SUMS`. |
| 2 | Code-only installs (`candidate.install`) and migration commits do not run the effects analysis. The install path (`src/runtime/reload.c:104`, `su_reload_install`) has no effects call. The migration commit (`src/runtime/worker.c:4469`) calls only `su_sched_finalize`. `check_system_effects` (`worker.c:597`) runs only from `build_schedule` (`worker.c:765`), i.e. at `world.create`. | C04 (verifier derives effects and checks them), C06 (activation as one versioned transaction), C09 (dependency revalidation), HOT_RELOAD section 3 ("prepare the new IR, effects, field bindings ... and replacement schedule outside the tick-critical section"). | Yes: `docs/AGENT_RPC.md` section 12 ("Installs (`candidate.install`) are not re-checked against the schedule in M1") and handoff `2026-10-12-m1-closure-b.md`. | None recorded. Closure B was accepted under "accept all". | **Code change.** Call `su_effects_build` on the new schedule at install and migration commit, and refuse with `SU_E_SCHEMA_MISMATCH` (as at create). The function exists; the change is a few lines plus a T-HOST-04-style negative test. Do this before any M3 parallel scheduling. |
| 3 | M3 host operators (`rts.spatial`, `rts.scatter`, and the spatial query) execute as engine-resident C with declared, not derived, effects. | C04 ("the verifier derives effects from IR"; "a `parallel: true` annotation or an agent's assurance is not evidence"); LIVE_IR section 1 ("agents must not fake those features with undeclared host behavior"); section 11 (extensions need verifier rules and differential tests); C01/C12 (native change is outside the no-build guarantee). | Yes: M3 contract sections 4.4 and 6.3, OD-1 and OD-5. | OD-1 is accepted in `planning/status.json`. OD-5 (the C04 exception) is **not** in the explicit accepted list. The "accept all, keep going" statement may cover it; this is ambiguous. | **Owner decision, then spec amendment.** Confirm OD-5 in writing. Add an explicit exception to C04 and LIVE_IR section 1 for host operators, with the instrumented-debug assertion from M3 contract 4.4 as the required test. Keep host operators out of the release claim of "verified IR". |
| 4 | The capsule schema declares `capabilities`, `system_rev` and a `host` variant (`schemas/capsule.schema.json:11-28, 92`), but the engine's capsule key whitelist (`src/ir/su_lir.c:326-329`) has none of them, and nothing in `src/` reads `capabilities` or `system_rev`. C04 requires capability declarations to be checked. | C04 (capabilities declared and enforced). | Schema change: yes (M3-03a). Engine enforcement: not documented. | OD-16 accepted (schema edit). Enforcement not covered. | **Code change** (enforce `capabilities` against IR use, e.g. `despawn_self` needs `despawn`) or **schema change** to remove the unenforced keys. Prefer code change; the schema text already says "absent means none". |
| 5 | Governance and status records disagree with each other and with the independent audits. (a) `planning/audit/M0-audit.md:6-8` and `M0-reaudit.md:8` say "M0 cannot close"; `status.json` says "closed by owner decision". (b) `M1-audit.md:10`, `M1-final-audit.md:7` and `M1-reaudit.md:11` say "M1 cannot close"; `status.json` says `closed_owner_accepted`, and its `m2` note says "M1 owner acceptance ... remains pending". (c) `M2-final-recheck.md:9` says "NOT ACCEPT", and `status.json` says `m2.open: []`, with the note "M2 closure not declared" beside `closed_owner_accepted`. (d) `status.json` top level is stale: `current_milestone` M1, `next_ready_tasks` M1-00, `accepted_source_revision` `9af40cc` (HEAD is `b839066`), `runtime_tests` "ctest 7/7". (e) `planning/tasks.json` lists M1-00 to M1-05 as `submitted` while M2-00 depends on M1-05. | C20 ("a milestone closes only with runnable commands and stored evidence"); AGENT_PROTOCOL section 9 (integrator verifies dependency assumptions). | Owner statements are recorded; the audits' open blockers are partly recorded as caveats. | Owner statements dated 2026-10-11 and 2026-10-12. | **Records only.** Update `status.json` to HEAD and to the actual state of each blocker; mark M1-00 to M1-05 `integrated` or `blocked`; list every open audit blocker in the milestone's `caveats`. No owner decision is needed for the records themselves. |
| 6 | Diagnostics lack the structured fields the spec requires. A verification error is a free-text `message` with the pointer embedded (`src/agent/su_agent.c:2490-2496`, `call_fail` at `su_agent.c:346`). Live probe, invalid capsule `velocity_x` store: `{"code":"SU_E_UNDECLARED_WRITE","status":8,"message":"verification failed: field 'velocity_x' is not writable at /instructions/3/field","retry":false}`. There is no source path, source hash, `system_id`, expected/actual pair or correction hint. The verifier's internal `SuDiag` (`src/ir/su_lir.h:32-38`) has `json_pointer` and `system_id` but they are not exported. | LIVE_IR section 8 (fields and hint required); AGENT_RPC section 5 ("Errors identify request/candidate/source hash, relevant JSON pointer, expected/actual values, and whether retry is meaningful"). | No. The M0 contract (section 4) does not mention structured diagnostics. | None. | **Code change.** Add an additive `error.detail` object (`json_pointer`, `source_path`, `source_sha256`, `system_id`, `expected`, `actual`, `hint`) carried from `SuDiag`. Protocol 0.1 is additive; no spec amendment is required if the field is optional. |
| 7 | Named M0 "core" methods are not implemented. `world.run` and `world.pause` return `SU_E_UNSUPPORTED_METHOD` (live probe); `tests.run` and `job.status` likewise. They are in the spec's core M0 table. | AGENT_RPC section 2 (core M0 methods). | Only in `planning/handoffs/2026-10-11-m0-05.md` ("a later-milestone method (`world.run`)"). Not in the planning contract or status. | None. | **Spec amendment or code.** Either amend section 2 to move these to a later gate (owner) or implement `job.status` and `tests.run` as a thin wrapper over the existing synchronous evaluation. |
| 8 | `candidate.evaluate` is synchronous and returns the result directly (`src/agent/su_agent.c:2646`; harness `ENGINE_METHODS` at `tests/harness/run_m0.py:723-727`: "synchronous; no job id"). AGENT_RPC section 2 says "Asynchronous test job ID". | AGENT_RPC section 2. | Handoff `2026-10-11-m0-05.md:68` only. | None. | **Spec amendment** (synchronous evaluation is an acceptable M0 choice; record it). |
| 9 | Request-id idempotence is not implemented. Retrying a request id with the same payload must return the previous outcome; a different payload must be an error. No retention exists in `src/agent/su_agent.c` or `su_proto.c` (grep: no request-id store). | AGENT_RPC section 4. | No. | None. | **Code change** (bounded table keyed by id and canonical payload hash), or **spec amendment** deferring it to the M1 broker. |
| 10 | Method renames without amendment: `schema.plan_migration` and `schema.commit_migration` became `world.migrate_plan`, `world.migrate_commit` and `world.migrate_rollback`; `native.specialize` became `world.specialize`. `capsule.submit` takes only `capsule_path`, with no expected base. | AGENT_RPC sections 2-3. | Yes, in AGENT_RPC sections 9 and 17 (the spec text was not amended). | None for the renames. | **Spec amendment** (rename the spec entries; the implemented names are more specific). |
| 11 | Undocumented technology choices. (a) Vendored yyjson is the spec's "native JSON default" (BACKENDS section 1); the engine uses a custom parser (`src/ir/su_json.c`). The 2026-10-07 dependency handoff says yyjson "must be pinned when the native build is created under M0-01"; no later record. (b) Linux should use Clang with LLD (ARCHITECTURE section 3; BUILD section 1); `CMakeLists.txt` selects no linker, and the accepted artifact is GCC 13.3 (`planning/status.json` `accepted_artifact_note`). (c) SHA-256 is an in-house FIPS 180-4 implementation (`src/ir/su_sha256.c`); LIVE_IR section 9 asks for "a reviewed implementation". (d) The AsmJit source is fetched, not vendored, and the BACKENDS ledger still says "candidate; not yet adopted" (the M1 contract says owner approval of fetch-vs-vendor is open). | ARCHITECTURE section 3; BACKENDS sections 1, 2, 7; BUILD section 1; LIVE_IR section 9. | No decision record for (a)-(c). (d) is in M1-00 contract section 6 as "Owner picks". | (d) accepted by M1 owner statement; the ledger was not updated. | **Owner decision + spec amendment** for (a)-(c) (or a code change to adopt yyjson and a Clang/LLD lane, which is larger). Update the ledger for (d). |
| 12 | Snapshots are not exportable and their format is not checked through the control plane. `world.snapshot` returns an ID and metadata; no RPC returns the document, so the harness cannot compare `snapshot_format` (`tests/harness/run_m0.py:1682-1685`). Snapshots live in supervisor memory and are lost when the supervisor exits. | DATA_ABI section 6 (versioned, canonical encoding); ARCHITECTURE section 9. | Yes, in the harness notes only. | None. | **Code change** (return the canonical document or a spool reference with its SHA-256 and format) or **spec amendment** stating that snapshots are process-local in M0-M2. |
| 13 | Containment is partial (C07). Disposable children are resource-bounded and network-denied, and cannot write files. They can read anything the uid can read. The active worker has no CPU bound and keeps write access. The M1 independent re-audit (before closure C/D) found bypasses (io_uring socket path, exec, ptrace); closure D says they are closed (`AGENT_RPC` sections 13-14, harness TX6), but no independent re-audit has covered closure D (`status.json` `m1.blockers_from_last_audit`). | C07 ("bounded CPU, memory, file, and network access"); ARCHITECTURE section 2 (a native fault recovers by replacement). | Yes: AGENT_RPC section 14 "Not contained"; status caveats. | M1 owner statement "accept all". | **Independent re-audit of closure D** (process), and a written owner waiver for filesystem reads and the active worker's CPU and write access. Code change optional (read-only bind of a scratch root). |
| 14 | M2 replay comparison is missing. `replay.compare`, `dependency.query`, `artifact.inspect` and `benchmark.run` are absent (`SU_E_UNSUPPORTED_METHOD`). M2 closure evidence requires "replay comparison across permitted backends". TX8 compares canonical digests across tick-worker counts only. | MILESTONES M2 closure; DETERMINISM section 4; C16. | Partly: AGENT_RPC section 3 lists them as "added only in their milestone". M2 closure is not recorded as declared (`status.json` `m2.note`). | M2 "accept M2" statement; the note says closure is not declared. | **Code change** (`replay.compare` over the existing replay log and canonical digests), or a written owner waiver recording that M2 closes without it. |
| 15 | Code-generation cache is an M1 deliverable (MILESTONES M1) and is absent. No cache exists in `backends/native` or `src/runtime` (grep). The broker deduplicates jobs by key but does not cache artifacts. | MILESTONES M1; ARCHITECTURE section 3 (artifact identity). | Not in M1-00 contract; no waiver. | None. | **Owner waiver** or **code change** (cache keyed by `artifact_hash`). |
| 16 | Windows. Only wine/mingw evidence exists for Win64 ABI (`planning/evidence/m1/platform-windows-cross.json`); no Windows-native run. Windows is "not implemented" for the control plane (AGENT_RPC section 8). MILESTONES M1 requires Windows ABI and platform smoke tests; DATA_ABI section 4 forbids claiming Windows support from Linux-generated bytes. | MILESTONES M1; DATA_ABI section 4; BUILD section 1. | Yes: `status.json` caveats. | M1 "accept all". | **Owner waiver** recording "Windows native: not run", or a Windows lane run. |
| 17 | Tick workers are opt-in (`SUTEKH_TICK_WORKERS` default 1). ARCHITECTURE section 6 says "M1 parallelizes disjoint chunks while preserving stable merge order". | ARCHITECTURE section 6. | Yes: AGENT_RPC section 12 "Decision: tick workers stay opt-in ... not made here." | M1 "accept all". | **Owner decision** (state the default). Code change not needed. |
| 18 | Diagnostic codes and stage order are fixed in the contract; `SU_E_MISSING_STORE` (20) and `SU_E_INVALID_ARGUMENT` (18), `SU_E_INTERNAL` (19), `SU_E_STALE_HANDLE` (21) are additions. | LIVE_IR section 8 and AGENT_RPC section 5 (initial set of 17). | Yes: M0 contract r2 and `include/sutekh/status.h:23-25`; 21 in AGENT_RPC section 15 only. | Contract owner (M0-00 r2). | **Spec amendment** to add 18-21 to AGENT_RPC section 5 (cheap, owner). |
| 19 | T13 latency decomposition does not separate queue, boundary and publish, which TEST_SPEC T13 requires ("real prepare, queue, boundary, publish, and first-effect distributions"). The engine reports `prepare_ns` and `critical_section_ns` only. | TEST_SPEC T13; PERFORMANCE section 4. | Yes: handoff `2026-10-11-m0-06c.md:44`. | M0 owner closure with caveats. | **Code change** (instrument the boundary wait and publish inside `su_reload_install`) or owner waiver. |
| 20 | Stress sizes differ. PERFORMANCE section 7 specifies 100,000 and 1,000,000 rows; the runs use 1,000,000 and 250,000 (`bench/reload/bench_reload.py:940`). | PERFORMANCE section 7. | Not as a deviation. | None. | Run 100,000, or amend to 250,000 (owner). |
| 21 | Preview-response target not measured under its own conditions. PERFORMANCE section 2 and T13 say "reusable worker"; `candidate.evaluate` spawns a disposable worker per request (`su_agent.c:2646`). The verdict is recorded as `not_comparable`. | PERFORMANCE section 2. | Yes: IMPLEMENTATION_STATUS 2.1 and status caveats. | M0 owner closure. | **Owner decision**: a persistent evaluation worker (a code change with its own containment review) or re-define the target. |
| 22 | Engine reports `runtime_mode` as `headless-m0-posix`; the four spec modes (`dev_checked`, `dev_native`, `perf_trusted`, `release`) are not reported. | ARCHITECTURE section 7. | No. | None. | **Code change** (report the mode) or amendment. Low effort. |
| 23 | Candidate lifecycle names differ (`received`, `effects_verified`, `passed`, `installed`). `PREPARED` and `PENDING_BOUNDARY` are not reported. | HOT_RELOAD section 2. | No. | None. | **Spec amendment** (state the implemented states) or code change. |
| 24 | Tasks DAG states are `planned / submitted / integrated` rather than the protocol's `ready → leased → implementing → submitted → checking → verified → integration_check → integrated`. Harness statuses (`not_run`, `running`, `passed`, `failed`, `skipped`, `unavailable`) do match. | AGENT_PROTOCOL section 4. | No. | None. | Process; amend or map. |
| 25 | Planning ledger and licence text conflict. `docs/BACKENDS_AND_DEPENDENCIES.md` section 7 records "Engine license: MIT, selected by owner on 7 October 2026". `planning/status.json`, the combined spec header and `LICENSE` record PolyForm Perimeter 1.0.1. AGENTS.md says do not choose a project license. | AGENTS.md; C19. | Conflicting. | Owner (status.json). | **Owner confirmation**; correct the ledger line. |
| 26 | `SHA256SUMS` is stale. `sha256sum -c` reports 13 mismatches: `.gitignore`, `docs/AGENT_RPC.md`, `docs/BACKENDS_AND_DEPENDENCIES.md`, `docs/DATA_ABI_AND_STORAGE.md`, `LIVE_IR_SPEC.md`, `PACK_VALIDATION.json`, `planning/status.json`, `planning/tasks.json`, `schemas/capsule.schema.json`, `schemas/lir.schema.json`, `schemas/task.schema.json`, `THIRD_PARTY_NOTICES.md`, `tools/validate_pack.py`. `tools/validate_pack.py` passes without checking it, and `tools/assemble_handoff.py` regenerates it. | Pack integrity; spec header. | No. | None. | **Regenerate** with `tools/assemble_handoff.py` after item 1 is decided. |
| 27 | `PACK_VALIDATION.json` says scope "specification_pack_and_python_example_model_only" with `native_runtime_tests: not_run`. It predates the engine and is not updated. | Pack integrity. | No. | None. | Regenerate. |

Notes on items that look like deviations but are documented and approved, or are conforming: M3 numeric profile `u32_mod_v1` used where the M3 contract text says `rts_u32_v1` (contract deviation 1; owner OD-9 "u32_mod_v1 retained in host records"); M2 IR 0.2 and snapshot 0.2/0.3 (owner accepted M2); M2 N4 and N5 policies (owner accepted, but N5 wording still reads as a deviation from contract B5); M1 chunked-scheduling opt-in (item 17).

## 3. Conformance matrix

Columns: **ID**, requirement (short), **Gate**, **Status**, **Evidence or citation**, **Note**. Citations are `path:line` in the audited tree; "live" means reproduced in this pass; "harness" means `tests/harness/run_m0.py` in the stated run.

### 3.1 Engine constitution (C01-C20)

| ID | Requirement | Gate | Status | Evidence | Note |
|---|---|---|---|---|---|
| C01 | Capsule-only edit runs without rebuild or relink | M0 | Conforms | harness T12 passed (release, native); `tests/e2e/no_build_probe.py`; `tools/demo_zero_wait.py` | strace-based; under ASan inconclusive (IMPLEMENTATION_STATUS 2.3) |
| C02a | Valid code-only swap keeps world state and worker PID | M0 | Conforms | harness T05 (23 checks) passed; `src/runtime/reload.c:104` | |
| C02b | Parsing, tests, codegen not in tick; old revision stays available | M0-M1 | Conforms | `src/agent/su_agent.c:2646` (evaluate in disposable worker); T06 passed | |
| C02c | Native fault replaces worker, no same-process claim | M1 | Conforms | TX2 passed; AGENT_RPC sections 8, 10 | |
| C03 | Unchecked work not accepted; install needs evidence | M0 | Conforms | T06, T07 passed; `su_agent.c:3652` (install) | |
| C04a | Effects derived from IR and checked against declaration (IR systems) | M0-M2 | Conforms | `src/runtime/jobs/su_effects.c:39` (derive from LOAD/STORE), `:63` (check) | Only at `world.create` (C04c) |
| C04b | Host operators' effects verified | M3 | Deviates (item 3) | M3 contract 4.4, OD-5 | Declared-and-tested only |
| C04c | Effects re-checked at install and migration commit | M0-M2 | Deviates (item 2) | `reload.c:104`, `worker.c:4469`, `worker.c:597,765` | Documented in AGENT_RPC 12 |
| C04d | Capsule declares capabilities, bounds, numeric profile, tests | M0-M3 | Deviates (item 4) | `schemas/capsule.schema.json:11-28`; `src/ir/su_lir.c:326-329` | `capabilities`, `system_rev`, `host` not read |
| C04e | Declared `parallel` is not trusted as evidence | M1 | Conforms | `src/runtime/worker.c:641` (all systems declared parallel) and `:648` (conflict check) | Check at create only |
| C05 | No escaping runtime addresses; stable IDs and handles | M0-M2 | Conforms | entity handle `(gen<<32)|slot` (AGENT_RPC 15); snapshots by field id and name (`src/world/snapshot.c`) | |
| C06a | Entry swap is a single publication at a boundary | M0 | Conforms | `src/runtime/reload.c:104-…`; T05, T11 | |
| C06b | Old native code retained until quiescent, then retired | M1 | Conforms | `safe_retire` tests (AGENT_RPC 13); native ctest 36/36 | |
| C06c | Schedule and effects activated as one transaction | M0-M2 | Deviates (item 2) | `reload.c:104` | |
| C07a | Unreviewed native candidates in disposable least-privilege workers | M1 | Conforms (Linux x86-64) | `src/agent/su_proc.c:411-438` (RLIMIT), seccomp and Landlock (AGENT_RPC 13-14); harness TX6 passed | |
| C07b | Bounded CPU, memory, file and network | M1 | Deviates (documented, item 13) | AGENT_RPC 14 "Not contained" | Reads and active worker unbounded |
| C07c | Control plane never executes uploaded native code | M1 | Conforms | admission in disposable worker (AGENT_RPC 9); active worker compiles only admitted hash | |
| C08 | Determinism named and tested; cross-backend equality only for a specified profile | M0-M1 | Conforms | harness T03, T14; TX5, TX8 passed; `test_native_chunked` (AGENT_RPC 13) | `u32_mod_v1` only |
| C09a | Additive field with default is the first migration class | M2 | Conforms | `add_field` (DATA_ABI 6.2); harness TX11 passed; `src/world/migration/su_migrate.c` | |
| C09b | Destructive changes: logic, revalidation, admission, rollback evidence | M2 | Deviates (items 2, 14) | plan `resource` and rollback in TX11; commit does not re-run effects (`worker.c:4469`); N5 open | |
| C10 | Performance claims carry hashes, hardware, mode, distribution, units | M0-M1 | Conforms (caveats) | `planning/evidence/m0-latency-*.json`; IMPLEMENTATION_STATUS 2; n=30 and n=720 | Shared VM; provisional |
| C10b | Unavailable counters null with a reason | M0-M1 | Unverifiable | not checked field by field | |
| C11a | No hot-path capsule allocation in M0 | M0 | Conforms | `src/ir/su_lir.c:440-441` (`allocation` must be "forbidden") | |
| C11b | No irreversible effects in speculation | M0 | Conforms | no external effects in M0; Landlock on disposable children | |
| C12 | Assembly optional; scalar baseline; stable C ABI | M1 | Conforms | scalar and AVX2 backends (`backends/native/`); no `kernels/x86_64/` | No assembly required |
| C13 | Specialization follows numeric and observable semantics | M1 | Conforms | TX3 (144 checks) passed; corpus of 42 cases plus own cases (AGENT_RPC 9, 11) | |
| C14 | Isolated worktrees, leases, one integrator | process | Unverifiable | `planning/status.json` `active_leases: []` | Process |
| C15 | Build debt bounded (two pending batches) | process | Unverifiable | `tools/broker/broker.py` supersedes queued jobs; no cap test | |
| C16 | Structured diagnostics, dependency queries, test handles, replay differences | M0-M2 | Not implemented (partial) | `dependency.query`, `replay.compare` return `SU_E_UNSUPPORTED_METHOD` (live probe); `tools/dev.py` has configure, build, clean only | Items 6, 14 |
| C17 | No hidden global pipeline; cache hits verified by full fingerprint | M1 | Not implemented (cache) | no code-generation cache (item 15); broker keys by sha256 | |
| C18 | Generality earned; BFME semantics kept in profile | M0-M3 | Conforms (note) | no BFME identifiers in `src/`, `include/`; RTS host operators in `src/runtime/rts/` rather than `profiles/rts/` (ARCHITECTURE 10) | Location only |
| C19 | External projects and assets protected | all | Conforms | `git ls-files` lists no game assets; `.gitignore` excludes `/original-assets/` | |
| C20 | Completion auditable; no unsupported "all tests pass" | all | Deviates (item 5) | audits `M0-audit.md:6-8`, `M1-audit.md:10`, `M2-final-recheck.md:9` | Owner closures |

### 3.2 Architecture

| ID | Requirement | Gate | Status | Evidence | Note |
|---|---|---|---|---|---|
| A1 | Capsule edit installed inside the active worker; no respawn | M0 | Conforms | harness T05 PID checks (live via harness) | |
| A2 | Native crash recovered by worker replacement from checkpoint | M1 | Conforms | TX2 passed; AGENT_RPC 8 | |
| A3 | `serve` and `worker` roles in one executable | M0 | Conforms | `src/platform/sutekh_main.c` | |
| A4 | C17 resident core | M0 | Conforms | `CMakeLists.txt:8-9` (`CMAKE_C_STANDARD 17`) | |
| A5 | Linux: Clang and LLD, versions pinned | M0 | Deviates (item 11b) | no linker selection in `CMakeLists.txt`; GCC 13.3 artifact | Undocumented |
| A6 | CMake and Ninja; configure once per toolchain | M0 | Conforms | `tools/dev.py`; CMake 3.28.3, Ninja 1.11.1 | |
| A7 | Strict JSON with vendored yyjson | M0 | Deviates (item 11a) | `src/ir/su_json.c`; no yyjson in tree | Undocumented |
| A8 | JSON-lines control over local stdio, length-bounded | M0 | Conforms | `src/agent/su_proto.c:323, 352-362`; harness T15 | |
| A9 | Checked scalar VM first | M0 | Conforms | `src/vm/su_vm.c`; T01-T03 | |
| A10 | AsmJit behind a C ABI; C++ confined to adapter | M1 | Conforms | `backends/native/su_native_asmjit.cpp`; `include/sutekh/native/abi.h` has no AsmJit headers; `third_party/asmjit.lock.json` commit `0d7f0105…` | Fetched, not vendored (item 11d) |
| A11 | No second native backend (no Cranelift) | M1 | Conforms | none present | |
| A12 | Tiers T0 VM, T1 native scalar and AVX2 | M0-M1 | Conforms | `execution_tier` values (AGENT_RPC 9); TX3 passed | T0b, T2, GPU not started |
| A13 | Version identities distinct: source_revision, semantic_hash, layout_generation, artifact_hash, world_revision | M0-M1 | Conforms | `capsule.submit` result (`source_hash`, `capsule_hash`, `semantic_hash`); `world.inspect` `layout_generation`; `world.specialize` `artifact_hash`; live `capsule.submit` | |
| A14 | Runtime modes `dev_checked`, `dev_native`, `perf_trusted`, `release` reported | all | Deviates (item 22) | live `engine.info`: `runtime_mode: "headless-m0-posix"` | |
| A15 | Storage: stable-row SoA; creation and destruction deferred to M2 | M0, M2 | Conforms | M0 fixed rows; M2 spawn and despawn with generations (AGENT_RPC 15) | |
| A16 | Scheduler serial in M0; M1 parallel chunks with canonical merge | M0-M1 | Deviates (item 17, approved-by-statement) | live `engine.info` `tick_workers.workers: 1`; AGENT_RPC 12-13 | |
| A17 | Backpressure, bounded queues, durable task status, queue latency reported | M1-M2 | Unverifiable (partial) | broker is durable (`tools/broker/broker.py`); engine candidate table of 16 slots; no queue-latency report | |
| A18 | Recovery: checkpoint plus log; repeated failure quarantines | M1 | Conforms | AGENT_RPC 8 (`quarantine_after` 2); TX2 | |
| A19 | Snapshot is a bounded deep copy; no constant-time claim | M0 | Conforms | `src/world/snapshot.c`; AGENT_RPC 16 | |
| A20 | Repository topology; directories materialized with first task | all | Conforms (absence); note | `kernels/`, `profiles/`, `render/` absent | M3 places RTS code under `src/runtime/rts/` |
| A21 | Explicitly not promised (no assembly-outperforms claim, no GPU, no unlimited agents) | all | Conforms | no such claims in `planning/status.json` or handoffs | |

### 3.3 Live IR (SU-LIR 0.1) and IR 0.2

| ID | Requirement | Gate | Status | Evidence | Note |
|---|---|---|---|---|---|
| L1 | Straight-line; no loops, recursion, allocation, native calls, floats, strings, files, networking | M0 | Conforms | opcode table in `src/ir/su_lir.c:58-100` (nine 0.1 ops) | |
| L2 | Module keys: `ir_version`, `system_id`, `numeric_profile`, `query`, `instructions` | M0 | Conforms | `src/ir/su_lir.c` module key list | |
| L3a | u32 range; exact integers; negative and fractional rejected | M0 | Conforms | `src/ir/su_json.c:808`; `examples/negative_cases.json` (`negative_u32`); harness T04 | |
| L3b | `bool` never coerces to `u32` | M0 | Conforms | `su_lir.c:700-703` (type mismatch `SU_E_TYPE`); fixture `wrong_store_type` | |
| L3c | Modular add, sub, mul; unsigned `lt`; exact `eq`; `select`; no short-circuit | M0 | Conforms | harness T03 (18 checks) | |
| L4 | Unknown opcode, unknown key, duplicate key, undefined and duplicate register, undeclared field: errors | M0 | Conforms | `su_lir.c:261, 656, 700-730`; `examples/negative_cases.json` (7 cases) | |
| L5a | Loads read invocation-entry values; at most one store per field per row | M0 | Conforms | `su_lir.c:724-737` (`store_count`); T09 | |
| L5b | Declared-writable field stored exactly once; never-stored is an error | M0 | Conforms (documented addition) | `su_lir.c:1033-1040` (`SU_E_MISSING_STORE`); M0 contract section 1; `status.h:25` | |
| L5c | Commit only after success; no partial output | M0 | Conforms | harness T09 (11 checks) | |
| L5d | Read and write permissions independent | M0 | Conforms | `su_lir.c:712-737` | |
| L6 | Bindings by stable id; mismatch is `SU_E_SCHEMA_MISMATCH`, no name fallback | M0 | Conforms | M0 contract section 2; `su_lir.c` sorts fields by id before binding | |
| L7a | Limits: 256 KiB source, depth 32, 4,096 instructions, 4,096 registers, 256 fields, 1,000,000 rows | M0 | Conforms | `include/sutekh/ir.h:15-20` (exact values) | Live: values match |
| L7b | Caps checked before proportional allocation | M0 | Conforms | `src/ir/su_json.c:200` (length first), `:218` (depth) | |
| L7c | Wall-time watchdog for candidate workers; timeout never alters state | M0 | Conforms | harness T10 (15 checks); `candidate.evaluate` `timeout_ms` | |
| L8a | Diagnostic fields: code, path, pointer, byte span, system id, field, expected and actual, correction hint | M0 | Deviates (item 6) | `src/ir/su_lir.h:32-38`; live error object | Undocumented |
| L8b | Failing source content hash in the response | M0 | Deviates (item 6) | live error object has no hash | |
| L9a | Semantic hash ignores whitespace and key order; fixed encoding | M0 | Conforms | `src/ir/su_lir.c:793-840`; harness T14 | |
| L9b | SHA-256 "through a reviewed implementation" | M0 | Unverifiable | in-house `src/ir/su_sha256.c`; handoff `2026-10-11-m0-02.md` lists it | Item 11c |
| L10 | Native equals reference; zero rows, tails, max u32, alias rejection, generation mismatch tested | M1 | Conforms (coverage partial) | AGENT_RPC 9 (row counts 0, 1, 7, 8, 9, 17, 1024; unaligned columns); TX3 | Generation-mismatch test not located |
| L11a | Each IR addition: verifier rules, reference, invalid-input tests, differential tests, bounds | M2 | Deviates (item 1) | IR 0.2 section in `LIVE_IR_SPEC.md` (commit `52aaed4`); refused by native (`ir_0_2_not_lowered`) | Spec text edited in place |
| L11b | No undeclared host behaviour; features need verified ops | M3 | Deviates (item 3, owner OD-1) | M3 contract 6.3; `status.json` accepted OD-1 | |
| L12 | IR 0.2: `slot_u32`, `gen_u32`, `tick_u32`, `despawn_self`, `emit_event` | M2 | Conforms | `include/sutekh/ir.h`; TX7, TX8 passed | Spawn from IR not implemented (by design) |

### 3.4 Agent protocol

| ID | Requirement | Gate | Status | Evidence | Note |
|---|---|---|---|---|---|
| P1 | Roles: orchestrator, implementer, verifier, integrator, broker | process | Unverifiable | handoffs | |
| P2 | Task packets with ID, scopes, tests, evidence | process | Conforms (partial) | `planning/tasks.json` (`id`, `status`, `depends_on`) | Scope fields not uniform |
| P4 | Work states and distinct statuses | process | Deviates (item 24) | `tasks.json` uses planned, submitted, integrated | Harness statuses match |
| P7 | Two pending native batches per write scope | process | Unverifiable | no cap record | |
| P8 | Evidence: tree hash, toolchain, commands, exit codes, timestamps | all | Conforms (partial) | `planning/evidence/*.json`; harness report records `repo_head`, binary SHA-256 | |
| P9 | Integrator verifies dependency assumptions | process | Deviates (item 5e) | `M2-00` depends on `M1-05` (`submitted`) | |
| P12 | Handoffs state changed, evidence, unrun, blocked, next | all | Conforms | `planning/handoffs/*.md` | |

### 3.5 Milestones

| ID | Requirement | Gate | Status | Evidence | Note |
|---|---|---|---|---|---|
| M0-1 | Headless fixed u32 world; JSON loader; checked VM; isolated candidate; same-worker replacement; snapshot; honest timing | M0 | Conforms | harness T01-T15 | |
| M0-2 | T00-T15 genuine results | M0 | Conforms | release harness: 23 passed, 0 failed, 1 not_run (T00, `--no-build`); T00 passed in scratch copy | Native: 26 passed |
| M0-3 | Explicit timing target comparison | M0 | Deviates (item 21) | harness: `ir_prepare` met, `install_critical_section` met, `warm_edit_to_first_effect` met; `preview_response` and `native_incremental_check` not_comparable | Owner closed |
| M0-4 | Instrumented capsule edit invokes no compiler | M0 | Conforms | harness T12 | |
| M0-5 | Stored commands for a new developer | M0 | Conforms | `docs/BUILD_AND_VALIDATION.md`; `tools/dev.py build --profile release` reproduced in scratch | |
| M0-6 | "Not M0" list respected at closure | M0 | Conforms (at closure) | M1-M2 features were added later under owner acceptance | |
| M1-1 | AsmJit scalar backend behind C ABI | M1 | Conforms | native ctest 36/36; TX3, TX4 passed | |
| M1-2 | AVX2 with capability checks, after scalar | M1 | Conforms | TX3 `auto` and `avx2` cases; `backends/native/avx2/su_native_caps.c:4-5, 36` (XGETBV) | |
| M1-3 | Differential corpus; capability fallback; stale artifact rejection; safe retirement; worker-fault recovery | M1 | Conforms | AGENT_RPC 9-13; native ctest; TX2, TX3 | |
| M1-4 | Real native build latency measurements | M1 | Conforms | `planning/evidence/m1/native-build-latency.json` | Shared VM |
| M1-5 | Small multi-agent experiment with measured rejection and rework | M1 | Deviates (partial, approved-by-statement) | `planning/evidence/m1/agent_workflow.json`: integration latency, tokens, tool calls n = 0 (per M1-final-audit) | Caveat in status |
| M1-6 | Windows ABI and platform smoke | M1 | Deviates (item 16) | `platform-windows-cross.json` (wine only) | |
| M1-7 | Code-generation cache | M1 | Not implemented (item 15) | grep | |
| M1-8 | Bounded validation and build queues | M1 | Unverifiable | `tools/broker/broker.py` (dedup, supersede, run-once); not exercised here | |
| M1-9 | Disjoint chunk scheduling only after effects tests | M1 | Conforms (opt-in) | `su_effects` tests in ctest; AGENT_RPC 12 | Item 17 |
| M2-1 | Generational entities; bounded structural and event queues; chunked storage | M2 | Conforms | harness TX7 (62 checks), TX9 passed; `test_churn` | |
| M2-2 | Additive and explicit destructive migrations | M2 | Conforms | harness TX11 (27 checks) passed; `test_migrate`; allocation sweeps (AGENT_RPC 17) | N5 open |
| M2-3 | Stronger snapshot and replay tooling | M2 | Deviates (item 14) | snapshot 0.2 and 0.3 (DATA_ABI 6.1-6.2); no replay tool | |
| M2-4 | Numeric profiles defined before authoritative floating point | M2 | Conforms | no floating point added | |
| M2-5 | Entity churn, stale handle rejection, migration failure and rollback, layout invalidation, stable event order across worker counts | M2 | Conforms (layout partial) | TX7, TX8 (N=1,2,4,8), TX11; M2-final-recheck 3.3 "partly resolved" | |
| M2-6 | Replay comparison across permitted backends | M2 | Not implemented (item 14) | no `replay.compare` | |
| M2-7 | M2 closure declared | M2 | Deviates (item 5c) | `status.json` `m2.note`: "M2 closure not declared" | |
| M3-1 | Synthetic hordes, formations, movement, targeting, projectiles, damage, spatial load | M3 | Not implemented (in progress) | M3-00, 03a, 04a integrated; 01, 02, 03, 04, 05, 06, 07, 08, 10 planned (`tasks.json`) | Gate open |
| M3-2 | Owner decisions OD-1 to OD-21 recorded | M3 | Deviates (partial) | `status.json` `m3.owner_decisions.accepted` omits OD-2, 4, 5, 6, 7, 8, 10, 11, 12, 13, 15, 18 | Item 3 |
| M4-1 | BFME evidence registry, importer boundary, multi-peer experiments | M4 | Not implemented | no `profiles/bfme`, no networking, no registry | Gate open |
| M5-1 | Minimal Vulkan presentation and asset pipeline | M5 | Not implemented | no renderer | Gate open |

### 3.6 AGENT_RPC (method table, envelope, errors)

| ID | Requirement | Gate | Status | Evidence | Note |
|---|---|---|---|---|---|
| R1 | One strict UTF-8 JSON object per line; max 1 MiB; reject duplicate keys, unknown keys, oversized collections, path escapes before allocation | M0 | Conforms | `su_proto.c:323, 352-362`; `su_agent.c:5707`; harness T15 (15 checks, oversize line) | |
| R2 | Request and response envelope (`protocol`, `id`, `ok`, `result`) | M0 | Conforms | live `engine.info`; `unsupported or missing protocol version` for `protocol` absent | |
| R3 | `ok:false` error identifies hash, pointer, expected and actual, retry meaning | M0 | Deviates (item 6) | live: `{code,status,message,retry}` only | |
| R4 | `engine.info` (versions, capabilities, runtime mode, process identity) | M0 | Conforms (mode name, item 22) | live `engine.info` | |
| R5 | `world.create` | M0 | Conforms | harness `ENGINE_METHODS` 735-738; T01 | Params use `fixture_path`, `capsule_path` |
| R6 | `world.inspect` (bounded canonical state and tick) | M0 | Conforms | harness T01, T08 | |
| R7 | `world.step` (exact tick count; reject during free run) | M0 | Conforms | harness `world.step` `ticks` 0..100000; T01 | No free-run mode (see R8) |
| R8 | `world.run` and `world.pause` | M0 | Deviates (item 7) | live: `SU_E_UNSUPPORTED_METHOD` | Undocumented |
| R9 | `world.snapshot` (immutable ID and metadata) | M0 | Conforms | harness T08 (`snapshot_id`, `sha256`, `bytes`) | Snapshot format not compared by RPC (harness note at `run_m0.py:1682-1685`) |
| R10 | `world.restore` (expected revision; mismatch error) | M0 | Conforms | harness T08 | |
| R11 | `capsule.submit` (source or allowed path, expected base; returns candidate ID and hashes) | M0 | Deviates (item 10) | `su_agent.c:2445`; `run_m0.py:711-715` (path only, no expected base) | Undocumented |
| R12 | `candidate.status` | M0 | Conforms | harness `ENGINE_METHODS` 716-721 | |
| R13 | `candidate.evaluate` returns asynchronous test job ID | M0 | Deviates (item 8) | `su_agent.c:2646`; `run_m0.py:723-727` ("synchronous; no job id") | Handoff only |
| R14 | `candidate.install` (expected revision; allowed effective tick) | M0 | Conforms | `su_agent.c:3652` (`allowed_effective_tick`); T07 (past-tick refusals) | Effects not re-checked (item 2) |
| R15 | `events.read` (cursor, max, bounded, ordered) | M2 | Conforms | `max` 1..256; TX7 | |
| R16 | `tests.run` | M0 | Deviates (item 7) | live: unsupported | Undocumented |
| R17 | `job.status` | M0 | Deviates (item 7) | live: unsupported | Undocumented |
| R18 | `schema.plan_migration`, `schema.commit_migration` | M2 | Deviates (item 10) | renamed `world.migrate_plan`, `world.migrate_commit`, `world.migrate_rollback` (`su_agent.c`) | Documented in AGENT_RPC 17 |
| R19 | `native.specialize` | M1 | Deviates (item 10) | renamed `world.specialize` (AGENT_RPC 9) | Documented |
| R20 | `replay.compare`, `benchmark.run`, `dependency.query`, `artifact.inspect`, `task.lease` | M2-M5 | Not implemented | live: `SU_E_UNSUPPORTED_METHOD` (replay.compare checked) | Gated later (item 14 for replay) |
| R21 | Unsupported methods answer `SU_E_UNSUPPORTED_METHOD`; never placeholder success | M0 | Conforms | `src/agent/su_agent.c:5650`; live | |
| R22 | Mutations carry an expected revision or are creation-only | M0 | Conforms | `world.step`, `world.restore`, `candidate.install` take `expected_world_revision`; `capsule.submit` creation-only | |
| R23 | Retry of the same request id returns the previous outcome; different payload is an error | M0 | Not implemented (item 9) | no store in `su_agent.c` or `su_proto.c` | Undocumented |
| R24 | Jobs immutable; cancellation best-effort; cancelled results not installed | M0-M1 | Not implemented | no job API | Gate partly open |
| R25 | Events use monotonic sequence independent of tick | M2 | Conforms | `event_seq` (AGENT_RPC 15); TX7 | |
| R26 | Initial error set: 17 codes | M0 | Conforms | `include/sutekh/status.h:6-17` | |
| R27 | Additional codes documented | M0-M2 | Conforms (documented addition, item 18) | `status.h:18-21`; M0 contract section 4 (18-20); AGENT_RPC 15 (21) | |
| R28 | Path policy: normalize, reject traversal and symlink escape with `SU_E_CAPABILITY_DENIED` | M0 | Conforms | live: `../etc/passwd` and `/etc/passwd` → `SU_E_CAPABILITY_DENIED`; harness T15 | |
| R29 | Network and native compile not exposed to gameplay capsules; broker uses validated argv | M0-M1 | Conforms | capsule schema refuses host variant in IR (`schemas/capsule.schema.json:250-264`); `tools/broker/broker.py` argv list | |
| R30 | Report host monotonic durations and simulation ticks separately; `job.status` says `not_run` or `unavailable` | M0 | Deviates (partial, item 7) | timings separated in T13; `job.status` absent | |
| R31 | Worker failure response carries `SU_E_WORKER_FAILED`, retry flag, recovery object | M1 | Conforms | AGENT_RPC 8; TX2 passed | |
| R32 | Migration plan, commit and rollback semantics | M2 | Conforms | TX11 passed; AGENT_RPC 17 (N4, N5 owner items) | |

### 3.7 Acceptance tests (TEST_SPEC)

| ID | Requirement | Gate | Status | Evidence | Note |
|---|---|---|---|---|---|
| T00 | Clean bootstrap with documented command | M0 | Conforms | `tools/dev.py build --profile release` in scratch copy; `run_m0.py --only T00` passed (9 checks) | |
| T01 | Reference movement after 1 and 3 ticks | M0 | Conforms | harness T01 (6 checks) | |
| T02 | Rows 0, 1, 7, 8, 9, 1024 | M0 | Conforms | `run_m0.py:1224` (counts list); 36 checks | |
| T03 | Numeric edges | M0 | Conforms | harness T03 (18 checks) | |
| T04 | Invalid-input matrix: unknown op, bad type, duplicate key or register, undefined register, denied access, oversize | M0 | Conforms | harness T04 (37 checks); fixture matrix | |
| T05 | Successful code-only swap; behaviour changes at recorded tick; PIDs preserved | M0 | Conforms | harness T05 (23 checks) | |
| T06 | Failed swap leaves active hash and state unchanged | M0 | Conforms | harness T06 (7 checks) | |
| T07 | Stale candidate rejected | M0 | Conforms | harness T07 (7 checks) | |
| T08 | Snapshot round trip restores state, tick and manifest | M0 | Conforms (format not compared) | harness T08 (10 checks); `run_m0.py:1683-1685` | |
| T09 | Working-root failure leaves published root unchanged | M0 | Conforms | harness T09 (11 checks) | |
| T10 | Candidate isolation: kill or time-limit does not stop active simulation | M0 | Conforms | harness T10 (15 checks) | |
| T11 | Repeated reload: bounded retained memory across 1,000 revisions | M0 | Conforms | harness T11 (6,350 checks) | |
| T12 | No native build during capsule-only swaps | M0 | Conforms | harness T12 (11 checks) | |
| T13 | Latency decomposition: prepare, queue, boundary, publish, first effect | M0 | Deviates (item 19) | queue, boundary and publish not separated (`2026-10-11-m0-06c.md:44`) | |
| T14 | Deterministic fixture repeat | M0 | Conforms | harness T14 (8 checks) | |
| T15 | Structured protocol; oversize and invalid messages; path escapes | M0 | Conforms | harness T15 (15 checks) | |
| T-M1 | Backend qualification: seeded generator with independent known answers | M1 | Unverifiable | `tests/native/qualification/`; corpus and known-answer split not checked | |
| T-SCHED | Scheduling: varied physical completion order; conflicts and cycles refused | M1-M2 | Unverifiable | conflicts and cycles refused at create (AGENT_RPC 12); completion-order variation not located | |
| T-SCHEMA | Schema tests: default, rename, reject removal, retype, insufficient memory, rollback, noninvertible via snapshot | M2 | Conforms (retype via transform) | TX11; `test_migrate`; `test_migrate_runtime` | |
| T-RTS | RTS suite and BFME suite | M3-M4 | Not implemented | gated | |
| T-MIN | Failure minimization | M1+ | Unverifiable | not located | |
| T-PERF | Acceptance versus performance labels | M0 | Conforms | harness verdict labels `met`, `not_comparable`, provisional | |

### 3.8 Data, storage and ABI (DATA_ABI, DETERMINISM)

| ID | Requirement | Gate | Status | Evidence | Note |
|---|---|---|---|---|---|
| D1 | Identity is not address; zero generation invalid | M2 | Conforms | generation 0 always stale (AGENT_RPC 15) | |
| D1b | Exhausted generations retired, not wrapped | M2 | Conforms | slot retired at `0xFFFFFFFE` (AGENT_RPC 15); `test_entities` | |
| D2 | M0: one fixed SoA table; sizes validated before pointer arithmetic | M0 | Conforms | M0 contract section 3 | |
| D3 | M1: chunks; boundaries depend on row count only | M1 | Conforms | `SU_CHUNK_SLOTS` 1024; AGENT_RPC 12 | |
| D4 | Public ABI: explicit widths, opaque contexts, struct-size fields, status returns | M0 | Conforms | `include/sutekh/abi.h`, `status.h` | |
| D5 | Calling conventions per platform; Windows native tests | M1 | Deviates (item 16) | wine only | |
| D6 | SIMD dispatch by capability and OS state | M1 | Conforms | `backends/native/avx2/su_native_caps.c:4-5, 36` | |
| D7 | Serialization: canonical, length-prefixed, little-endian; M0 JSON | M0-M2 | Conforms | M0 JSON snapshots; M2 `SUWSTATE` (DATA_ABI 6.1) | |
| D8 | Arenas or preallocated buffers with explicit capacities | M2 | Conforms | AGENT_RPC 16 | |
| D9 | Snapshot 0.1 key set | M0 | Conforms | `src/world/snapshot.c`; T08 | |
| DT1 | Three claims kept separate (repeatability, cross-backend, BFME fidelity) | all | Conforms | harness labels; `status.json` `original_bfme_compatibility: not_run` | |
| DT2 | Numeric profiles: `u32_mod_v1` in M0 | M0 | Conforms | `numeric_profile` values in `su_lir.c` | |
| DT2b | M3 `rts_u32_v1` name | M3 | Deviates (approved) | M3 contract deviation 1; OD-9 "u32_mod_v1 retained" | |
| DT3 | System order from explicit DAG edges with stable tie-breaks | M0-M1 | Deviates (documented, not approved) | capsule order: AGENT_RPC 12; closure-B handoff | Narrowing |
| DT3b | Random numbers via explicit state and streams | M3 | Not implemented | no PRNG | Gate open |
| DT4 | Replay evidence: initial checkpoint, ordered inputs, first divergence report | M2-M3 | Not implemented (tool) | supervisor replay log exists; no tool (item 14) | |
| DT7 | Compatibility fixture format | M4 | Not implemented | none | Gate open |
| DT8 | Networking: recorded command playback; multi-peer tests | M4 | Not implemented | none | Gate open |

### 3.9 Lifecycle, native lifetime, build and performance

| ID | Requirement | Gate | Status | Evidence | Note |
|---|---|---|---|---|---|
| H1 | Guarantees by change class | M0-M2 | Conforms | T05, T06, T07, T08; TX11 | |
| H2 | Candidate lifecycle states RECEIVED to RETIRED | M0 | Deviates (item 23) | live `status` values | |
| H3 | Code-only install prepares IR, effects, bindings and schedule outside the tick | M0 | Deviates (item 2) | `reload.c:104` | |
| H3b | Behaviour-changing swap recorded with effective tick and semantic hash | M0 | Conforms | `semantic_manifest` in `world.inspect`; `effective_tick` in `candidate.install` result | |
| H4 | Committed and working roots | M0 | Conforms | T09 | |
| H5 | Checkpoints and log; retention limits | M1 | Conforms | AGENT_RPC 8 (`checkpoint_retain` 4, max 8; `log_max_entries` 256) | |
| H6 | Migration: noninvertible change rolls back to checkpoint, no guessed inverse | M2 | Conforms | AGENT_RPC 17 (pinned checkpoint); TX11 | |
| H7 | Native code: writable then executable, never RWX; cache sync | M1 | Conforms | `backends/native/su_native.c:153-159`; `__builtin___clear_cache` at `:163` | |
| H8 | Multiplayer lockstep policy | M4 | Not implemented | none | Gate open |
| H9 | Required failure tests: stale schema generation, memory unavailable, preparation cancellation | M0-M2 | Deviates (partial, undocumented) | T06, T07; allocation sweeps (AGENT_RPC 17); no cancel RPC | Cancellation absent; item 9 family |
| B1 | Bootstrap inventory and pinning (versions, hashes, licences) | M0 | Deviates (partial, item 11) | AsmJit lock and licence hash conform; no yyjson pin; no compiler hashes | |
| B1b | CMake and Ninja; configure once; no unity TU | M0 | Conforms | `CMakeLists.txt`; `tools/dev.py` | |
| B1c | C17 with warnings; sanitizers in separate lanes | M0 | Conforms | `-Werror` in build; `planning/evidence/m0-asan-*` | |
| B1d | LLD as initial linker | M0 | Deviates (item 11b) | none | |
| B2 | Validation lanes L0-L4 | M0-M1 | Not implemented (partial) | `tools/dev.py check --changed` absent; `validate_pack.py` covers L0 for the pack | |
| B2b | Native interactive budget 2 s; longer jobs under broker | M1 | Conforms (reference machine not used) | native incremental check measured 198-712 ms on shared VM (`bench`; harness) | Not_comparable |
| B3 | Broker deduplicates by fingerprint; durable records; refuses stale requests | M1 | Unverifiable | `tools/broker/broker.py` docstring | Not run here |
| B4 | Planned command surface: `check --changed`, `submit-build`, `job`, `test --suite`, `benchmark --suite` | M0-M1 | Not implemented | `tools/dev.py:5-7` (configure, build, clean only) | Spec says these are desired, not present |
| B5 | Header change invalidates consumers; new cache namespace on toolchain change | M1 | Unverifiable | | |
| B6 | CI policy: no paid runners | all | Unverifiable | `.github/workflows/repository-checks.yml` not run | |
| PF1 | Seven proposed latency targets | M0 | Conforms (measured; two not comparable) | harness timing table | |
| PF1b | Warm edit broken into prepare, queue, worker, boundary | M0 | Deviates (item 19) | submit, evaluate, install and first-step times reported; queue and boundary not split | |
| PF2 | Reference environment recorded; not claimed | M0 | Conforms | IMPLEMENTATION_STATUS section 2 | |
| PF3 | PMU counters optional, null with reason | M0-M1 | Unverifiable | not checked | |
| PF4 | Latency decomposition from capture to first tick | M0 | Deviates (item 19) | | |
| PF5 | Warm and cold distributions published separately | M0 | Conforms | warm n=720; cold n=60 (`planning/evidence/m0-latency-*.json`) | |
| PF6 | Stress at 100,000 and 1,000,000 rows | M0 | Deviates (item 20) | runs use 1,000,000 and 250,000 | |
| PF9 | Agent-throughput metrics reported separately | M1 | Deviates (partial, item 5/M1-5) | nulls in `agent_workflow.json` | |
| PF10 | No fabricated cycles-per-entity thresholds | all | Conforms | none present | |
| RR1 | Fake hot reload risk (PID and state checks) | M0 | Conforms | T05 | |
| RR2 | Fake zero-wait risk | M0 | Conforms | T12 | |
| RR3 | Process mistaken for a sandbox | M1 | Conforms (documented) | AGENT_RPC 14 "Not contained" | |
| RR4 | Counter telemetry invented | M0-M1 | Conforms | counters null or absent | |
| GP1 | BFME and other profiles not claimed | all | Conforms | no profile code | |
| GP2 | Shared contracts: stable schema IDs, bounded effects, structured diagnostics | all | Deviates (item 6) | diagnostics | |

### 3.10 Spec pack integrity

| ID | Requirement | Gate | Status | Evidence | Note |
|---|---|---|---|---|---|
| K1 | Combined copy equals individual specs ("regenerate this copy") | — | Deviates (item 1) | `SUTEKH_FULL_SPEC.md` lacks LIVE_IR "IR 0.2" section, DATA_ABI 6.1-6.2, the AGENT_RPC 0.2 and M2 text, and the M0 r3 wording | |
| K2 | `SHA256SUMS` matches files | — | Deviates (item 26) | 13 mismatches | |
| K3 | `tools/validate_pack.py` passes | — | Conforms | run in scratch copy of HEAD: status passed, 229 JSON files, 37 DAG nodes, 83 links | Does not check K2 |
| K4 | Schemas change only with owner approval | — | Conforms (OD-16 for capsule; M2 for IR 0.2) | `schemas/*.json` diffs | `lir.schema.json` +106 lines, approval via M2 acceptance |
| K5 | Licence record consistent | — | Deviates (item 25) | BACKENDS section 7 vs `status.json` | |

## 4. Other findings (not deviations from the original text)

1. **Undocumented narrowing of the no-build gameplay claim.** M3 host operators are native C compiled into the engine (`src/runtime/rts/`). An edit to `rts.spatial` or `rts.scatter` requires a native build. This is consistent with C01 (native change is outside the guarantee) but the spec does not say so for host operators. Covered by item 3.
2. **Good coverage evidence.** Negative fixtures (`examples/negative_cases.json`, 7 cases), T04 (37 checks), T15 (15 checks) and the default ctest (24 targets) all pass at HEAD.
3. **Process evidence for accepted M1 native work exists.** `planning/evidence/m1/` contains native build latency, platform matrix (Linux native, Win64 under wine), throughput and stress files.
4. **Harness integrity.** The harness computes expected values from `tests/harness/reference_eval.py`, compares error codes exactly, and checks PIDs against `/proc`. The T13 gate facts are observed rather than hard-coded (checked at `run_m0.py:2425-2440`).

## 5. Priority remedies

Code changes that are small and within existing invariants (do first): item 2 (effects check at install and migration commit), item 4 (enforce or remove `capabilities`/`system_rev`), item 6 (structured `error.detail`), item 9 (request-id idempotence), item 22 (report the runtime mode), item 19 (split T13 timing).

Owner decisions and amendments (do not start without approval): item 1 (ratify or move the in-place spec edits and regenerate the combined copy); item 3 and OD-5 (host operators and the C04 exception); items 7, 8, 10 (method table amendments); item 11 (yyjson, LLD, SHA-256, AsmJit default); items 13, 15, 16, 17, 20, 21 (waivers or scope); item 25 (licence statement).

Records to correct without owner approval: item 5 (status and tasks files), item 26 and 27 (checksums, validation record).

## 6. Unrun checks

- Dev profile at HEAD: full ctest and harness (this pass ran release and native only).
- Sanitizers (ASan, UBSan, TSan) and clang builds at HEAD. Historical sanitizer logs exist only for M0.
- Installed-effects gap (item 2) by mutation: insert a conflicting system through `candidate.install` and confirm acceptance. Verified by source reading only.
- Capsule `capabilities` and `system_rev` (item 4): submit a capsule that uses them and check the response. Not run; source reading only.
- Request-id idempotence (item 9): not run; no retention found by source reading.
- Broker tests (`tools/broker/`), `tools/check_repository_license.py`, `tools/assemble_handoff.py`, `tools/dev.py check` (absent), schema validation of `planning/evidence/*.json`.
- AsmJit `--build-probe` mode (`tools/bootstrap_asmjit.py --build-probe`).
- Wine Win64 harness, non-x86-64 hosts, Windows native.
- Full harness with a clean build of the native profile (T00 was run only for the default profile).
- Native release timing and PMU counters; all timing figures in this report are the existing shared-VM observations, not new measurements.
- Live probes for item 6 (structured diagnostics) used one capsule; other negative fixtures were not probed.
- Generation-mismatch (L10) and independent known-answer corpus (T-M1) coverage were not located or run.

## 7. Scratch and cleanup

Scratch builds (CMake trees, the AsmJit-fetched copy of HEAD and the `dev.py` build) were created outside the repository under the session scratchpad and removed at the end of this pass. No file in the repository was modified other than this report. Nothing was pushed.
