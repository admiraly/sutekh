# M3-00 contract r3 independent review

Reviewed: `planning/contracts/M3_RTS_CONTRACT.md` at r3 (commit 067d8ba), read in full. Not edited. Inputs: `planning/audit/M3-00-contract-rereview.md` (r2 review, blocking items A-D) and changelog R.1. Checked against the tree: `src/ir/su_lir.{h,c}`, `src/runtime/jobs/su_effects.{c,h}`, `src/runtime/schedule.{c,h}`, `src/runtime/worker.c`, `src/runtime/reload.c`, `src/runtime/entity_tick.c`, `src/runtime/native_versions.c`, `src/vm/su_vm.c`, `src/world/entities/su_cmdq.{c,h}`, `src/world/entities/su_ents.c`, `src/world/su_world.c`, `src/agent/su_agent.c`, `src/agent/su_proto.c`, `src/platform/sutekh_main.c`, `include/sutekh/{abi,entities,ir,status}.h`, `schemas/capsule.schema.json`, `planning/tasks.json`.

Scratch arithmetic (outside the repo, not committed): row and array byte counts from the 39 field names in table 2, the 14-field `set_fields` entity, the cmdq allocation formula, the staging count from the write sets in sections 3 and 5.

## Verdict: accept after fixes

The r3 design holds. The stall invariant (B) is correct as code. The row-size arithmetic (C) is correct. The control domains and writable set (D) are consistent with the formulas. The host-entry mechanism (A) is the right shape, but it does not cover every path that reads a schedule entry, and two sites in the contract are wrong about where the refusal goes. One new blocker surfaced (E): the cmdq sizing makes capacities above 699,050 fail at the first tick, which contradicts the 1,048,576 claim. Blocking items are A, D (small), E and the lease fixes in section 4. None needs a redesign; all are text plus small code changes, some needing owner approval.

---

## 1. Blocker status (A-D from r2 and the r3 claims)

| item | r3 claim | result | evidence |
|---|---|---|---|
| A (F1) host entry | declaration module via existing struct; `su_effects` host branch; no VM context; dispatch before native and chunked branches | **Mostly correct; incomplete.** See section 2 | `su_lir.h:40-68`, `su_lir_module_free` (`su_lir.c:874-890`) frees exactly what the declaration allocates; `su_vm_context_create` rejects `access == 0` (`su_vm.c:203`); `su_effects_check_declared` fails for a zero-instruction module with any declared access (`su_effects.c`, step 1 call at :254); `su_sched_finalize` (`schedule.c:~170`) takes the union from `mod->fields`, so a decl with fields works; `entity_tick.c:546` reaches `su_sched_run_system`. |
| B (F6/D11) stall | control records excluded; merge count bounded by alive rows at tick start | **Correct.** See section 3 | `su_cmdq_commit` (`su_cmdq.c:643`) tests `n_events + n_cmds`; control spawn calls `su_world_spawn` and `ent_log_one` and never enters the cmdq (`worker.c:1015-1080`); `su_world_spawn` requires `IDLE` (`su_world.c:737`), so no spawn can run inside a tick; the VM skips dead rows (`su_vm.c:541`); one `despawn_self` per row (`max_struct_per_row = 1`, per-row cap at `su_cmdq.c:~283`). |
| C (F4) row bytes | 925 B per row, 948,225 for 1,024 rows, 100,351 headroom | **Arithmetic correct; wording wrong.** See section 4 | 39 names plus `external_id` at 10 digits: 925 bytes without the separator. 1,024 rows with `,` separators and brackets: 925 * 1024 + 1023 + 2 = 948,225. Headroom 1,048,576 - 948,225 = 100,351. The r2 figure of 900 without the id is consistent (925 - 25). |
| D (F6/D11) domains and writable set | 14 writable fields, `flight` 1..64, domains per field | **Domains consistent with the formulas.** Three gaps (D1-D3 in section 5). | Writable list matches the table rows marked "yes" (14). `flight = 0` deadlock is removed. |

---

## 2. Item A: host-entry mechanism (blocking)

Verified correct: the `SuLirModule` fields and free path, `su_vm_context_create` rejecting `access == 0`, `su_effects_build` calling `check_declared` for every entry, the union table, `entity_tick.c:546` dispatch, and that `SuSysEntry` gains only additive fields.

Blocking gaps:

**A1. World restore cannot rebuild a host entry (blocks T-RTS-REPLAY-3).** `restore_core` (`worker.c:2228`) calls `rebuild_schedule` (`worker.c:2171`), which runs `su_reload_load(c->paths.capsule, c->paths.source, ...)` (`worker.c:2193`) and then `su_sched_add` (`:2200`). A host system has no `source_path` (3.1), so `paths.source` is empty and the restore fails with `SU_E_SCHEMA_MISMATCH`. The recovery claim in 10.3 and 12 needs this path. Fix: in 3.1, add a host branch to `rebuild_schedule` that finds the descriptor by `system_id`, calls `su_host_load(paths.capsule, ...)` and `su_sched_add_host`. The `CkptSys` record already keeps `paths.capsule`.

**A2. Other sites that treat every entry as IR.** The contract names only the loader, dispatch and candidate refusal. The following also reload or lower entries from `paths` or `mod`:
- `h_internal_specialize_install` (`worker.c:3770`): calls `spec_prepare(ww->sched.sys[j].mod, ...)` for an installed system. `spec_prepare` (`worker.c:3295`) forwards to `su_nv_prepare` (`native_versions.c:194`), which takes only the module and cannot tell that it is a host entry. So the 3.1 wording "refuse in `native_versions.c` before reading `mod`" cannot be implemented as written. Fix: refuse in `h_internal_specialize_install` when `sched.sys[j].kind == SU_SYS_KIND_HOST`, with `SU_E_CAPABILITY_DENIED`, before `spec_prepare`. Drop `native_versions.c` from the M3-04a lease (or add a kind parameter, which is a shared signature change and not preferred).
- `h_internal_world_migrate_prepare` (`worker.c:4359`, `:4372`): rebuilds all installed systems from their recorded paths. M3 has no migration (section 2 preamble), so state that a world with a host entry refuses `world.migrate` with `SU_E_CAPABILITY_DENIED`.
- `h_candidate_evaluate` (`worker.c:1868`, `:1873`) and `h_internal_specialize_admit` (`worker.c:3510`, `:3521`) build standalone schedules from a request capsule. State that a request naming a host `system_id` is refused with `SU_E_CAPABILITY_DENIED`.

**A3. Host hashes are empty, but the manifests depend on them.** 3.1 says the declaration's hash strings are empty. `semantic_hash` and `capsule_hash` are read by `sched_manifest` (`worker.c:2024`), the checkpoint record (`:2073-2075`), restore comparisons (`:2189`, `:2279`), the registry (`:957`, `:970`), `system.list`-style output (`:479-483`), and `system_manifest_hash` (9.1). Fix: define both in 3.1. `semantic_hash` is the SHA-256 of the canonical descriptor (operator version, constants hash, field table with ids, names, types and access). `capsule_hash` is the SHA-256 of the host capsule bytes, returned by `su_host_load`. `su_sched_add_host` copies both into `SuSysEntry`.

**A4. Required statements that are missing (host ABI).**
- `su_world_commit_staging` copies every row of each listed column (`su_world.c:591`). A host run must therefore write staging for every row of each declared write field, including non-target rows. `rts.scatter` (4.3) writes field 39 only for targets. State that it copies working to staging for all rows first, or the commit overwrites `incoming` everywhere. This is the most likely correctness trap in M3-04.
- `su_world_build_invocation` exposes the world's readable columns, not the descriptor's declared reads. State whether the invocation is restricted to the declared set. Under OD-5 the declared set is what is tested, so the simplest rule is to restrict it.

**A5. Lease text is narrower than the required edits (M3-04a).** The `src/runtime/worker.c` entry allows only "the host loader branch ... and the `host_declared` line". A1, A2 and A3 need edits at `rebuild_schedule`, `h_internal_specialize_install`, `h_internal_world_migrate_prepare`, the checkpoint record fill and the manifest. Widen the entry to name those functions. Remove or re-justify the `native_versions.c` entry (A2).

Non-blocking within A: `read_schedule_facts` (`worker.c:514`) returns OK with phase `""` for a capsule without a `schedule` block. T-HOST-05 requires a refusal, so the host branch must check the block itself, which the text already says.

---

## 3. Item B: stall rule (verified, accept)

Checked against the code:
- Commit: `su_cmdq_commit` (`su_cmdq.c:641-644`) refuses when `n_events + n_cmds > per_tick_e`. Records are one per command (`fin` is sized `per_tick_e`, `rec` is sized `per_tick_s`).
- Append-time caps: `su_cmdq_append_struct` checks `tick_s` against `per_tick_s` (`:289`), and events against `per_tick_e` (`:334`).
- Config: `SU_CMDQ_MAX_TICK_CAP` = 1,048,576 (`su_cmdq.h:50`) and `su_cmdq_create` refuses larger caps (`su_cmdq.c:134-135`). RTS capacity at most 1,048,576 satisfies this.
- Spawns: `su_world_spawn` refuses unless `IDLE` (`su_world.c:737`). Spawns therefore never run inside a tick. The 9.4 text says spawn records are appended "at the boundary"; they are appended at command time, in the idle phase. Correct the wording and cite `su_world.c:737` as the reason.
- Deaths: `rts.damage` is the only producer, `max_struct_per_row = 1` (per-row cap, `su_cmdq.c:~283`), and the VM skips dead rows (`su_vm.c:541`, alive view from `su_world_alive_view`, `entity_tick.c:233`). One request per alive row at tick start, so `n_cmds <= alive(tick start) <= capacity = per_tick_e`.
- T-RTS-CAPS-2 (spawn 16, kill 16 in one tick, capacity 16) passes: `n_cmds = 16 <= 16`.

The invariant holds. Item E below is about sizing, not the stall rule.

---

## 4. Item C: row bytes (accept, fix wording)

- 925 bytes per row without separator, 948,225 for 1,024 rows (`{"values":{39 pairs},"external_id":4294967295}`). Verified by script.
- `set_fields` entity: `{"entity":18446744073709551615,"values":{14 pairs}}` is 354 bytes, as stated. For 1,024 entities with separators and brackets the array is 362,521 bytes; the contract's 362,496 excludes separators and brackets. Harmless, but state the rule.
- Fix wording: T-RTS-LOAD-1 says "measured by the generator". No generator exists yet (M3-01), so say "computed". "925 bytes per row plus one separator" is ambiguous. Write "925 bytes per row without the separator; 948,225 bytes for the array of 1,024 rows including separators and brackets".
- The 1 MiB cap is checked on the request line (`su_proto.c:322`, `len > SU_LIMIT_PROTOCOL_MESSAGE`), so the 100,351 headroom is for the envelope, which is small. Correct. Depth of the row (5 levels) is under `SU_PROTO_MAX_DEPTH` = 8.
- `external_id` is parsed as u32 with at most 10 digits (`worker.c:1019-1042`), so the bound is real.

---

## 5. Item D: control domains and writable set (blocking-small)

Verified: the 14-name writable list matches the "yes" rows of table 2.2. `kind`, `team`, `hp` and system-owned fields are refused with `SU_E_TYPE`. Domains are consistent with the formulas (`reload - 1` does not wrap at reload 1; `flight` 1 arrives at `t + 1`; `speed` 0 never moves). `flight = 0` is refused. Fixes needed:

- **D1 (blocking-small): spawn defaults.** Fields not named at spawn take `fields[k].def` = 0 (`su_world.c:~750`). Three consequences the contract does not state:
  - `reload`, `damage` and `flight` have domains that exclude 0 on every row. An omitted field must be `SU_E_TYPE`, not silently 0. Say so in 2.2 and in T-RTS-CTRL-1.
  - `formation_slot` has idle value NONE, but the default is 0, which is a real slot. An omitted `formation_slot` silently joins slot 0. It is harmless only because `formation_gen = 0` never validates (generations start at 1). Require the field to be named, or make NONE the spawn default for this field.
  - `formation_ox` and `formation_oy` default to 0, which is offset -16384. Require them when `formation_slot != NONE`.
- **D2 (blocking-small): rules over the resulting row.** For `set_fields`, the rules "formation_gen = 0 when formation_slot = NONE" and "range_sq = 0 on kind 2" depend on fields the call may not name. State that they are checked on the row after applying the payload to the committed row.
- **D3 (blocking-small): batch atomicity.** `su_world_spawn` mutates committed state for each row (`su_world.c:732-770`). Slot exhaustion mid-batch returns `SU_E_RESOURCE_LIMIT` (`su_ents.c:~309`) after earlier rows were committed. The "no state change" rule of 2.2 and the atomicity of T-RTS-LOAD-1 require the handler to validate every row, and to precheck free slots (`rows <= capacity - alive`), before the first `su_world_spawn`. Add a test: a batch that exhausts capacity on its last row changes nothing.
- **D4 (text): `hp` row.** The kind-2 entry reads "0 (proposed bound 65535, OD-9)". Make it "0 only". The bound applies to kind 1.
- Minor: "`target_slot`, `dmg_target`, `proj_target` accept 0 or NONE" is ambiguous, since 0 is a real slot. Harmless (each is overwritten before read or guarded by an inactive flag). Say "any value, idle semantics unchanged" or require NONE.

---

## 6. Item E: cmdq sizing and the memory table (new, blocking)

`su_ent_run_ticks` calls `su_cmdq_create(s->count, rows, ...)` (`entity_tick.c`, the cmdq creation in `su_ent_run_ticks`). `nsystems` is `s->count`, which is every scheduled system, not only the entity-op ones. `rows` is `w->row_count`, the capacity (`su_world.c:400`).

- `nbuf = nsystems * nchunks` (`su_cmdq.c:~150`). Every buffer gets `min(chunk_size * per_row_struct_cap, per_tick)` entries, and `per_row_struct_cap` is a single global value. For the RTS schedule (6 systems) the structural buffers total `6 * capacity` entries of 32 bytes.
- `tot_s > SU_CMDQ_MAX_ENTRIES` (4,194,304, `su_cmdq.h`) refuses with `SU_E_RESOURCE_LIMIT` (`su_cmdq.c:~174`). With 6 systems that is `6 * capacity > 4,194,304`, so **every tick at capacity 699,051 or more fails** before any row runs. The contract's sweep to 1,000,000 and the capacity cap of 1,048,576 (D11, 9.4, risk 1) are therefore false in the current code. The stop rule would also record this as a generic limit, not a named one.
- Memory, from `su_cmdq_create` (`su_cmdq.c:188-193`): the per-tick scratch arrays are sized to the per-tick caps (= capacity): `m_cmd` 32 B, `t_cmd` 32 B, `rec` 40 B (SuEvent), `m_evt`, `t_evt`, `fin` 40 B each. That is 224 B per row, plus the structural buffers at 6 * 32 = 192 B per row. Together 416 B per row. 9.3 lists "structural and event buffers: 32 bytes per row".
- The staging column is also undercounted. `su_world.c:447-461` allocates staging for every field with WRITE access in the world's union table. Writes across the six systems cover 25 fields (3, 4, 5, 9, 19, 20, 21-30, 31-39). 9.3 uses 10 (combat only), so staging is 100 B per row, not 40.
- Corrected per-row total (excluding event log, spool, 2 MiB grid): columns 312 + staging 100 + entity table 12 (not verified) + grid 4 + cmdq 416 = **844 B per row** with the current code, or 684 B per row with the fix below. The contract says 400.

Fix (owner approval, `src/world/entities/su_cmdq.{c,h}` is an M2-00b shared file and is in no task lease):
1. Give `su_cmdq_create` a per-system per-row cap, zero for systems without `uses_entity_ops`, so only the entity-op system gets buffers. For RTS this makes the structural buffers `1 * capacity` (32 B per row), and capacity up to 1,048,576 fits `SU_CMDQ_MAX_ENTRIES` (4,194,304). Add a test: 6-system schedule with one entity-op system at capacity 1,048,576 ticks.
2. Or, if the fix is refused, state `capacity <= 699,050` for the 6-system RTS schedule and make the 12 sweep and D11 say so.
3. Recompute 9.3 from the real allocations, with the scratch arrays listed, and state that the per-tick scratch depends on the caps, not on the used rows.
4. Note: `SU_LIMIT_ROWS` is 1,000,000 by default (`include/sutekh/ir.h:20`). Capacities 1,000,001 to 1,048,576 need `SU_LIMIT_ROWS` configured higher, and `world.create` checks that. Say so in 9.4 and 12.

---

## 7. Lease completeness and task ids (blocking-small for M3-05, otherwise non-blocking)

- **M3-05 lease lacks `src/runtime/worker.c`.** The validators are there: `h_world_spawn` (`worker.c:1015`) checks domains and calls `su_world_spawn`. `su_agent.c:4464-4534` forwards the request (`req_begin(..., "world.spawn")`). New domain checks, `set_fields`, the batch atomicity of D3 and the admission prechecks (12) all go in `worker.c`. Add it, and state that `su_agent.c` changes are forwarding and recovery-log only. The statement "M3-05 has no lease on `src/runtime/`" in section 14 is wrong.
- **M3-05 lacks `src/world/entities/su_cmdq.*`** if fix E is taken (no task owns it).
- **M3-04 lacks the host registry.** Production descriptors must be registered where `worker.c` finds them by `system_id` (3.1). The registry is in `src/runtime/rts/host_decl.c`, which is M3-04a. M3-04 has no lease on it. Name the registry file in M3-04a and let M3-04 register descriptors there, or give M3-04 the file.
- **M3-08 lacks `src/platform/sutekh_main.c`.** The stale comment (`sutekh_main.c:81-83`, N-c) is in neither M3-08 nor M3-08a. Add it to one of them.
- **Replay recorder and replayer location (OD-17) is unassigned.** M3-05 lists `tests/rts/replay/` and `bench/rts/load/`, which are tests and loaders. Name the code location.
- **N-j is incomplete.** Section 17 lacks OD-14 (serial spatial and scatter). OD-16 and OD-17 were added; OD-14 was not. Add it.
- **N-n is open.** `planning/tasks.json` still has only `M3-00` (`:567-580`, with `write_scope` profiles/capsules/bench). The split in section 14 is not yet in the orchestrator file. Orchestrator item, not a contract defect.
- **M3-03 writes `bench/rts/gen_ir.py`,** outside `bench/rts/gen/` (M3-01). Harmless, but say that `gen_ir.py` is an IR authoring tool and not a scene generator, so D10 (one normative scene generator) stays true.
- Task ids and dependencies are consistent with section 3.2 and with `tasks.json` (`M3-00` depends on `M2-01`, which exists). T-HOST-01..10, T-RTS-CAPS-2, M3-M29..M31, M3-03a, M3-04a and M3-08a are unique. OD numbering is unique (OD-1, OD-1b, OD-2..OD-19 with OD-3b and OD-3c). Every SU_E code cited exists in `include/sutekh/status.h` or the runtime headers (checked by script).

---

## 8. Non-blocking items (fix in the owning task's first patch)

- **N-1 (9.3 and 12 spool arithmetic).** The N for `2 * (446 N + 90 * 65,536) + 16,384 <= 536,870,912` is 588,631, not 588,650 (the +16,384 is in the 12 formula). 9.3 uses `retained_events`, 12 uses `event_log_max`. Pick one and state it.
- **N-2 (snapshot bound).** `su_agent.c:978-984` adds fixed overhead (`16384 + fields * 512 + MAX_SYSTEMS * 512`) to `(fields + 1) * 11` per row. The "440 bytes per row" is the per-row part only. Say so.
- **N-3 (M3-M28 detection).** T-RTS-CAPS-1 detects a too-small cap only if a run actually exceeds it. Add an explicit `world.create` refusal test for an RTS config with cap below capacity, which is the mutant the table names.
- **N-4 (host capsule schema).** `capsule.schema.json` requires `capsule_format`, `schema`, `numeric_profile`, `limits` and `tests` (and `schedule`). Section 5.5 says "source, ir_version and fields required only for the IR variant", but does not say what the host record puts in `limits` (`max_instructions` has minimum 1) or `schema`. Give the host values.
- **N-5 (R.1 wording).** "`su_effects_derive` ... consumes `e->mod->fields[k].access` for the union" is wrong. `su_effects_derive` reads only LOAD and STORE instructions (`su_effects.c:39-57`). The union comes from `su_sched_finalize`. Fix R.1.
- **N-6 (3.2 edges).** The edge `rts.combat` before `rts.move` is not required by any conflict in the r3 sets (combat writes 9 and 31-39; move reads 4-6, 12, 19, 20, 27). It is harmless and documents "combat decides with tick-start position". Say it is documentary, so B2 (which reads `cooldown`) is not mistaken for the reason.
- **N-7 (B1 edges).** B1 makes `combat` read `hp`. Its required order is `damage` after `combat`, which holds transitively through `scatter`. Say so in 3.2.
- **N-8 (9.4 proof step 2).** It says "dead rows zeroed, 9.4 proof step 2" but refers to itself. Rewrite as "the VM skips dead rows (`su_vm.c:541`)".
- **N-9 (T-RTS-CTRL-1).** Add the `formation_slot` default and the D1 cases to the test list.

Verified without change (no action): 3.2 edges cover every conflict between the six systems' read and write sets (checked pair by pair); T-HOST-04 and M3-M29 consistent; `su_sched_check_candidate` is the single gate and `reload.c:162` calls it before any swap; `su_vm_context_create` `access == 0` rejection; 2.1 squared-distance bound 2 * 32767^2 = 2,147,352,578 < 2^31; 4.1 `rc <= 8` and `truncated = [box_total > K]` consistent with the ring scan; 5.2 known answers (reload 6 flight 2 gives re-fire at t + 6, reload 1 flight 1 gives period 2, reload 2 flight 5 gives re-fire at t + 6) traced by hand.

---

## 9. Required changes, in order

1. (A1) Add the host branch to `rebuild_schedule` (restore) in 3.1; state that `paths.source` is not used for host entries.
2. (A2) Refuse host entries at `h_internal_specialize_install`, `h_internal_world_migrate_prepare` and candidate and admit paths; drop the `native_versions.c` guard.
3. (A3) Define host `semantic_hash` and `capsule_hash` and where they come from.
4. (A4) State the staging-for-all-rows rule for `rts.scatter` and the invocation scope.
5. (A5, section 7) Widen the M3-04a and M3-05 leases (`worker.c` functions, `su_cmdq.*` if E is taken, the host registry file, `sutekh_main.c` for N-c).
6. (E) Choose fix 1 or fix 2 in section 6, correct the cmdq sizing claim, and recompute 9.3 (844 or 684 B per row, not 400).
7. (D1-D3) Spawn defaults, resulting-row rules, batch atomicity with its test.
8. (C, D4, section 4 and 5 wording) Fix the wording items listed.
9. Non-blocking items in section 8 and N-j (OD-14) in the first owning patch.

After 1-8, the contract is ready for owner acceptance.

## 10. Not verified

- The memory figures for the entity table (12 B per row) and the snapshot 446 B per row: needs the real encoder and `su_ents` layout (M3-08a).
- Instruction counts, operator timings and the 1,024-querier targeting gate: M3-02, M3-03, M3-06.
- No code was compiled or run. The cmdq sizing finding (E) is from reading `su_cmdq_create` and `su_ent_run_ticks`; M3-05 should confirm it with a test at capacity 699,051 before the fix is merged.
