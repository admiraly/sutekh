# M3-00 contract r2 independent re-review

Reviewed: `planning/contracts/M3_RTS_CONTRACT.md` (r2, read in full, not edited). Prior review: `planning/audit/M3-00-contract-review.md` (r1 verdict: accept after fixes). Changelog: section R.
Checked against: `src/runtime/schedule.{h,c}`, `src/runtime/jobs/su_effects.{c,h}`, `src/runtime/worker.c` (effect analysis, 592-660), `src/runtime/entity_tick.c`, `src/ir/su_lir.h`, `src/vm/su_vm.c` (`su_vm_context_create`), `src/world/su_world.h`, `src/agent/su_proto.c:322`, `src/world/entities/su_cmdq.{c,h}`, `src/world/snapshot.{h,c}`, `src/agent/su_agent.c` (checkpoint, spool, retain), `src/runtime/checkpoint.h`, `src/platform/sutekh_main.c`, `include/sutekh/{ir,status,entities}.h`, `schemas/capsule.schema.json`, `planning/tasks.json`, `ENGINE_CONSTITUTION.md` (C01, C10), `MILESTONES.md`.

Numeric re-derivations: a targeting probe (scratch Python, outside the repo, not committed, pure-Python exact oracle plus 64-unit ring-order budget query on a uniform 2048 x 2048 centre box, teams interleaved, 600 queriers, `random` PRNG, not the committed generator), plus arithmetic checked by hand and in Python. The probe is not the committed `congested.blob.v1`.

## Verdict: accept after fixes

r2 resolves most of r1. The design is sound, the numeric claims I could check are correct, and most of the code facts it cites are right. Four items must be fixed before owner acceptance. Each has a small fix, and none needs a redesign:

1. **F1 residual (blocking):** the "declaration module" cannot be built, verified or effect-analysed as written. The claim that `su_effects.c` is reused "unchanged in code" is false.
2. **F6 / D11 (blocking):** the "no stall by construction" claim contradicts the OD-3c rule that the contract itself requires.
3. **F4 (blocking, small):** the 1,024-row cap is not a safe bound because `external_id` length is unbounded.
4. **Control-plane domains (blocking, small):** `flight = 0` deadlocks a shooter, and the writable set of `world.set_fields` is undefined.

Everything else is non-blocking and can be fixed in the owning task's first patch.

---

## 1. Verified numeric claims

| claim (contract) | my check | result |
|---|---|---|
| saturating add wraps at 65,538 arrivals of 65,535 (65,537 sums to exactly 0xFFFFFFFF) | 65537 * 65535 = 2^32 - 1 exactly | correct (r1's 65,537 was wrong; r2 fixes it) |
| `sat_add` order independence | min(sum, MAX) for non-negative terms | correct |
| 0xFFFFFFF0 + 0xFFFFFFF0 wraps to 0xFFFFFFE0 | arithmetic | correct |
| 2 * 32767^2 = 2147352578 < 2^31 | arithmetic | correct |
| `rc = (R + 63) >> 6` covers distance R; `rc <= 8` for R <= 512; 17 x 17 = 289 cells | a target at distance <= R is at most `(63 + R) >> 6` cells away | correct; rifle rc = 4 (81 cells), bow rc = 8 (289 cells) |
| grid cost 2 MiB | 262144 * 4 + 262145 * 4 = 2,097,156 bytes | correct |
| K = 128, `truncated = [box_total > K]` | ring scan visits every entry of the box in order, so truncation iff box_total > K | correct, and consistent with 10.1 |
| cooldown and re-fire `max(reload, flight + 1)`; known answers 6/2 -> 6, 1/1 -> 2, 2/5 -> 6 | traced the 5.2 IR by hand (cooldown reaches 0 at t + reload; proj_left hits 1 at t + flight; proj_active clears at end of t + flight) | correct |
| SU_E codes used in sections 10 and 13 | `status.h` has SU_E_TYPE, SU_E_STALE_*, SU_E_CAPABILITY_DENIED, SU_E_INTERNAL, SU_E_MISSING_STORE, SU_E_UNDECLARED_*, SU_E_DUPLICATE_REGISTER, SU_E_UNDEFINED_REGISTER, SU_E_UNKNOWN_OPCODE | all present |
| 37 `mod` dereference sites in schedule.c, entity_tick.c, native_versions.c, su_effects.c, worker.c | grep: 7 + 10 + 9 + 7 + 4 | correct |
| 1 MiB cap on the whole request line | `su_proto.c:322`: `len > SU_LIMIT_PROTOCOL_MESSAGE` | correct (1,048,576 accepted, one more refused) |
| `SU_CMDQ_MAX_TICK_CAP` = 1,048,576, default 4096 | `su_cmdq.h:50,55` | correct |
| snapshot row guard 838,860 at 39 fields | `snapshot.c`: `hw * (fields+1) > 64 MiB / 2`; 33,554,432 / 40 = 838,860.8 | correct (guard is on high water, not capacity) |
| library spool 32 MiB, binary 512 MiB | `su_agent.c:62`, `sutekh_main.c:83` | correct |
| `world.spawn` emits one SPAWNED per row | `worker.c:1077` | correct |
| `su_world_build_invocation / build_outputs / commit_staging / zero_dead` exist | `su_world.h:157-173` | correct |
| 1,024 * 926 + envelope = 948,380 bytes (F4) | my recompute: 39 fields with 10-digit values, sorted compact JSON, 900 bytes per row, 1,024 rows = 922,726 bytes | same order, below cap. **But** see F4 below: `external_id` is not counted |
| canonical snapshot 175 / 446 bytes per row, 90 bytes per event | not reproduced (needs the real encoder) | unverified, labelled as measured in r2 |
| 65,536-row worst case: 446 * 65,536 + 90 * 65,536 ~ 35 MB | arithmetic | correct; needs the 512 MiB binary spool, as stated |
| targeting: 16k bow, K = 128, 64-unit cells: ~1.5% not nearest, 0 missed | my probe: 16384 rows, bow, K=128: valid 600/600, missed 0, not_nearest 10/600 (1.7%), ratio p50 1.23, p95 1.32 | reproduced (non-generator scene) |
| 1,024 bow, K = 128 | probe: missed 0, not_nearest 0/600 | consistent |
| 16k rifle, K = 128 | probe: missed 0, not_nearest 6/600, p95 2.01 | consistent |
| 16k bow, K = 64 (r2 says 89/600 not nearest) | probe: 80/600 not nearest, p95 5.55 | consistent (r2's K=64 row is the reason for K=128) |
| instruction estimates (60/110/50/25) | not measured here | unverifiable until M3-03; contract labels them estimates (correct) |

## 2. F1-F9 status

| id | r2 status | evidence | residual |
|---|---|---|---|
| F1 | **Partially resolved. Blocking residual.** | `su_sched_add` and `su_effects_build` need an `SuLirModule` with a real verified body. `su_effects_derive` (`su_effects.c:39-57`) derives reads and writes only from LOAD/STORE instructions. `su_effects_build` calls `su_effects_check_declared` on every entry (`su_effects.c:254`), which fails with SU_E_SCHEMA_MISMATCH when declared access differs from the derived access. A declaration module with no instructions therefore fails for any declared read or write. `su_vm_context_create` (`su_vm.c:~195`) rejects `access == 0`, so the module cannot simply drop its access bits. `su_lir.h` exposes only `su_lir_verify` (from bytes) and `su_lir_module_free`; there is no constructor. The worker effect analysis (`worker.c:600-650`) reads phase, `after`, `before` from the **capsule file** (`read_schedule_facts(paths[i].capsule, ...)`), so a host entry needs a capsule record as well. | Fix in section 4 (blocking item A). |
| F2 | **Resolved.** | My probe reproduces the r2 numbers (section 1). The 4.2 gates are stated. Residual: the gate margin is thin (1.5 to 1.7% against 2%), and the measurement used 600 queriers and not the committed generator. | Non-blocking item N-a. |
| F3 | **Resolved.** | Arithmetic and order argument verified; mutant M3-M08 and the saturation fixture are consistent. | None. |
| F4 | **Partially resolved. Blocking residual (small).** | Base row is about 900 bytes at worst-case values; 1,024 rows is about 922 KB. `rows:[{values, external_id?}]` in OD-3b has no `external_id` length bound. A 64-character `external_id` makes the same 1,024-row request 1,005,670 bytes (margin about 4%). Any longer id breaks the bound. T-RTS-LOAD-1 says "all-worst-case" without defining the id. | Fix in section 4 (blocking item C). |
| F5 | **Resolved for the recovery claim.** Residuals are non-blocking. | N <= 65,536 claim and named limits are consistent with code. Residuals: (a) the spool admission uses `snapshot_bound` = (fields + 1) * 11 = 440 bytes per row times (retain + 1), and the default retain is `AGENT_DEFAULT_RETAIN` = 4 (`su_agent.c:57`), not 1. M3 must set `checkpoint_retain` (config key exists, `su_agent.h:40`). (b) 440 bytes per row is below the contract's own worst-case 446. (c) `sutekh_main.c:81-82` says a 1,000,000-row world fits in 28 MB per checkpoint. That contradicts the row guard (838,860) and the 2-bytes-per-value lower bound (80 bytes per row). (d) A periodic capture refused by the row guard reaches `checkpoint_world` as CK_BAD, which calls `mark_unrecoverable` (`su_agent.c:1068`, `2904`) and returns SU_E_INTERNAL, not SU_E_RESOURCE_LIMIT as section 12 says. | Non-blocking items N-b, N-c. |
| F6 | **Partially resolved. Blocking.** | The per-row structural and death bounds are right (one despawn per row per tick). The cap check at the boundary is `v.n_events + v.n_cmds > per_tick_e` (`su_cmdq.c:643`), so commands and events share the counter. But OD-3c, which the contract marks as **required**, counts control-plane host events against the same cap. With that rule, a control `spawn_batch` of s rows adds s events and s structural commands at one boundary, and the same rows can die in the next tick. Concrete: capacity 16, empty world, spawn 16 rows (16 events, accepted), then all 16 die in the next tick, so the tick sees 16 + 16 = 32 events against a cap of 16. The tick aborts and retries identically (stall). The stall-freedom claim in D11, F6 and 16.1 is therefore false under the required rule. The obvious fix, `max_events_per_tick = 2 * capacity`, breaks `SU_CMDQ_MAX_TICK_CAP` above capacity 524,288, which conflicts with D11's 1,048,576 bound. | Fix in section 4 (blocking item B). |
| F7 | **Resolved**, with the writable set left undefined (item D). | Batch shape, one revision check, one recovery entry and no event are all consistent with the table. 1,024 * 886 bytes = 907 KB is under the cap. | See item D. |
| F8 | **Resolved.** | Single generator, C generator removed, T-RTS-SCENE-2 consistent with 7.5. | None. |
| F9 | **Resolved on paper.** | I traced the r2 baselines: B1 unit fires at baseline (fails expectation); B2 mode-2 with valid target does not move at baseline (fails); B3 member moves 96 at baseline, expected 192 (fails). Write scopes now include `rts.move` for all three. | Non-blocking item N-d (B1-B3 all change `rts.move` and must be ordered as separate candidates). |

## 3. N1-N16 status

| id | status | note |
|---|---|---|
| N1 | Resolved | Derivation and three known answers verified (section 1). |
| N2 | Resolved | Every condition in 5.1 to 5.4 is `eq`/`lt`; flags go through `eq(flag, 1)`. |
| N3 | Resolved | `truncated = [box_total > K]` is exact and matches 10.1. |
| N4 | Partially resolved | Per-category formulas present, but the total does not match its own table. 352 + 12 + 4 + 32 = 400 bytes per row before the event log, not "about 440" (9.3). Non-blocking item N-e. |
| N5 | Resolved | `SU_E_TYPE` at control, `SU_E_INTERNAL` only on invariant breaks. |
| N6 | Partially resolved | `M3-04a`, `M3-08a` and the leases exist. Missing leases are listed in item A (`src/runtime/jobs/su_effects.*`, `src/runtime/worker.c`, `src/ir/su_lir.*` or a runtime module-builder, `capsules/rts/` host records). `tasks.json` `M3-00` scope still differs from the contract's table (orchestrator item). |
| N7 | Partially resolved | Anchor-only orders and the remainder rule are right. The acceptance check "at tick 0, before any tick runs, goal = anchor + offset" cannot hold: `goal` and `anchor_valid` are computed by phases 0 and 1, so the check must be after phase 1 of tick 0. Non-blocking item N-f. |
| N8 | Resolved | `operator_manifest_hash` in the header; fails closed on mismatch. |
| N9 | Resolved | Fault seams and T-RTS-REPLAY-3 extension. Cadence maps to the existing `checkpoint_interval_ticks` (`su_agent.c:5722`). |
| N10 | Resolved | Table matches formulas (checked column by column). |
| N11 | Partially resolved | `kind = 1, hp = 0` rule is right. The general validation gap is item D. |
| N12 | Resolved | Scene identity by regeneration and digest. |
| N13 | Resolved | Labelled as estimates; oracle budget left to M3-02 measurement. |
| N14 | Partially resolved | M3-M21 to M28 added. **M3-M27 is not detected by T-RTS-REPLAY-1**: that test compares a record and a replay from the same (mutated) tree, so both agree. Its detector must be the Python-model comparison of M3-06. Non-blocking item N-g. |
| N15 | Resolved, with OD-16 needed | `schemas/capsule.schema.json` has `fields`, `schedule` (after/before/parallel/phase), `limits` (including `max_struct_per_row` and `max_events_per_row`), `numeric_profile`, `tests`. It lacks `system_rev` and `capabilities`, which 5.5 requires. OD-16 must therefore be invoked, not left conditional. Non-blocking item N-h. |
| N16 | Resolved | Multiply-high bias bound is correct. |

## 4. Blocking items and fixes

**A. F1 declaration module (blocking; owner approval of OD-1b must wait for this).**
- Name the builder. Either add an `su_lir` function that constructs a host declaration `SuLirModule` (no instructions, nonzero access bits, `host` flag), or build it in `src/runtime/rts/`. Add that file to the M3-04a lease.
- Change the effect path. `su_effects_derive` and `su_effects_check_declared` must take the declared set for host entries (for example `if (m->host) use the descriptor's sets`), or `su_effects_build` must skip the derived check for host entries and read the descriptor. Add `src/runtime/jobs/su_effects.c` and `su_effects.h` to the M3-04a lease. Without this, every host entry fails `su_effects_build` with SU_E_SCHEMA_MISMATCH.
- Add host capsule records (`capsules/rts/spatial.capsule.json`, `scatter.capsule.json`, or a host variant), because `worker.c` reads phase and ordering from capsule files. Add `src/runtime/worker.c` to the lease.
- State that `su_vm_context_create` gets a context for a host entry with zero instructions (or that host entries get no VM context). Name which.
- Acceptance: a test that a host entry passes `su_effects_build` with its declared edges, and that a candidate replacing it is refused.

**B. F6 stall-freedom (blocking).** Choose one and state it in D11, 9.1 header and OD-3c:
- (i) Keep `max_events_per_tick = capacity` and exclude control-plane records (spawn, set_fields, despawn) from the per-tick counter. Record that exclusion in the replay header. The per-tick bound then holds for deaths alone; or
- (ii) `max_events_per_tick = max_struct_per_tick = 2 * capacity` and cap RTS capacity at 524,288 (so it fits `SU_CMDQ_MAX_TICK_CAP`). Revise D11 and 16.1 to say so; or
- (iii) bound control-plane admissions so that spawns applied at a boundary plus the deaths that can follow them stay within the cap, e.g. refuse a spawn call when `alive + 2 * rows > capacity`. Give the exact rule and its test.
Whichever is chosen, add T-RTS-CAPS-2: spawn to capacity, then kill every row in the next tick, must complete.

**C. F4 external_id bound (blocking, small).** Add to OD-3b: `external_id` at most 64 bytes (or another stated bound), with a byte-level check on the whole request that refuses a `spawn_batch` whose bytes exceed `SU_LIMIT_PROTOCOL_MESSAGE`. Define "all-worst-case" in T-RTS-LOAD-1 as the 39 named fields at maximum domain values plus an `external_id` of the stated maximum. Record the encoder used for the 926-byte figure (my recompute gives 900 bytes per row without an id).

**D. Control-plane domains and writable set (blocking, small).**
- `flight = 0`: fire sets `proj_left = 0`, arrival tests `proj_left == 1`, so `proj_active` never clears and the shooter never fires again. Reject `flight` outside 1..64 with SU_E_TYPE (the table says 1..64 but 13 does not list it).
- Likewise state the domains enforced on `spawn`, `spawn_batch` and `set_fields` for `reload` (1..1024), `damage` (1..65535), `speed` (0..1024), `order_mode` (0..2), `team` (0..1), `formation_ox/oy` (-16384..16383 after bias), and `hp` (>= 1 for kind 1). Each violation is SU_E_TYPE.
- Name the control-writable set. The contract's `set_fields` writes "non-key fields", which includes derived or system-owned fields (`incoming`, `proj_*`, `dmg_*`, `target_*`). Define the writable list explicitly; reject writes to system-owned fields with SU_E_TYPE.

## 5. Non-blocking items (fix in the owning task's first patch)

- **N-a (F2 gate margin).** The 2% `valid_but_not_nearest` gate sits against measured 1.5 to 1.7% on 600 non-generator queriers. Measure on the committed generator with 1,024 queriers before M3-06 closure. Make 10.6 and 4.2 use the same querier count.
- **N-b (F5 config).** Name the config keys: `checkpoint_retain` = 1, `checkpoint_interval_ticks` = 32, `spool_bytes_max`. The 9.3 spool note should say so. The reservation is `snapshot_bound * (retain + 1)`, and `snapshot_bound` (440 bytes per row) is below the worst-case 446, so the admission figure is not an upper bound. State that.
- **N-c (F5 stop rule).** A periodic capture refused by the 838,860-row guard currently becomes an unrecoverable world with SU_E_INTERNAL. M3-05 must refuse `spawn_batch` that would push high water above 838,860 with SU_E_RESOURCE_LIMIT before admission, so that section 12 `snapshot_cap` is reached cleanly. Correct the comment at `src/platform/sutekh_main.c:81-82` in M3-08a or M3-08.
- **N-d (F9 ordering).** B1, B2 and B3 each change `rts.move` (and B1 also `rts.goal` and `rts.combat`). Specify the candidate order and that each fixture is run against the r2 baseline and the previous accepted candidate.
- **N-e (N4 arithmetic).** Replace "about 440 bytes per row" with the table sum, or state the 32-byte buffer figure as unmeasured, so the total is 400 bytes plus event log and spool.
- **N-f (N7 acceptance).** Change "at tick 0, before any tick runs" to "after phase 1 of tick 0".
- **N-g (N14 detector).** M3-M27 is detected by the Python-model comparison (M3-06), not by T-RTS-REPLAY-1. Change section 15.
- **N-h (N15 schema).** Invoke OD-16 explicitly: add `system_rev` and `capabilities` to `capsule.schema.json`, and the field-table digest key in 5.5.
- **N-i (10.1 ring fixture).** "must still find the ring-1 enemy within budget or report truncated" is not a deterministic expectation. With 200 friendly entries in ring 0 and K = 128, the expected result is `target_valid = 0`, `truncated = 1`. Write that exact outcome.
- **N-j (section 17).** Owner decisions list omits OD-14, OD-16 and OD-17.
- **N-k (F1 dispatch).** `entity_tick.c:546` calls `su_sched_run_system`, so the host branch in `run_one_system` covers it. Say so in 3.1 so the dispatch site is not searched for twice.
- **N-l (control semantics).** Section 5.1 `order_mode` values other than 0, 1, 2 fall through as attack-move. Covered by item D validation.
- **N-m (C01 / AGENTS.md).** Targeting constants and the scatter rule are native (`rts_ops_v1`). C01 covers capsule edits and excludes native C, so this is consistent with the constitution. Owner should still record that gameplay tuning of K, the cell size and the scatter rule needs a native build (OD-5, N8).
- **N-n (tasks.json).** Orchestrator item: `M3-00` in `tasks.json` lists `profiles/rts/`, `capsules/rts/`, `bench/rts/`, not the contract path. Add the M3-01 to M3-10 entries through the orchestrator only.

## 6. New contradictions introduced by r2 (checked)

- F6 "stall-free by construction" vs OD-3c required rule (item B, blocking).
- F1 "reuses su_effects.c unchanged" vs `su_effects_check_declared` (item A, blocking).
- F4 "1,024 rows is safe" vs unbounded `external_id` (item C, blocking).
- N7 tick-0 check vs phase order (N-f).
- N14 M3-M27 detector (N-g).
- N4 total vs table (N-e).
- sutekh_main.c comment vs row guard (N-c).
- 9.3 "retain = 1" vs `AGENT_DEFAULT_RETAIN` = 4 (N-b).
- M3-M08 meaning change, B2 and B3 redefinition, OD-6 and OD-3/3b signatures are all documented in section R. I found no undocumented change of meaning.
- Task ids: M3-04a and M3-08a are new and documented. Dependencies match section 14 and `tasks.json` (`M3-00` depends on `M2-01`, which exists). No existing id is renumbered.
- OD numbering is unique (OD-1, OD-1b, OD-2 to OD-19 with OD-3b and OD-3c).

## 7. Not verified by this review

- Snapshot bytes per row (175 / 446) and checkpoint encoder output: needs the real encoder; not reproduced.
- Instruction counts per IR system: needs M3-03.
- Operator timings, memory peaks and VmHWM: out of scope for a contract review.
- The committed `congested.blob.v1` digest and the 1,024-querier gate: needs M3-01 and M3-02.
- The tick-level behaviour of the event-cap refusal under OD-3c: needs M3-05.
