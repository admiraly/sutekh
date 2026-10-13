# Handoff: M3-05 slice, world.spawn_batch worker handler and RTS spawn rules (D1, D3, OD-3b)

Base: `2b42f4d`. Scope: worker side only. `world.spawn_batch` is dispatched by the worker but **not yet
forwarded by the agent** (`su_agent.c`), the same state `world.set_fields` is in.

## Change
- `src/runtime/rts/control_domains.{c,h}`: `su_rts_ctl_spawn_row`, the 2.2 spawn domains and D1 defaults of one row,
  ported rule by rule from `validate_spawn` in `bench/rts/ref/model.py`. A rule on a field the world schema lacks is
  skipped, so the 18-field spatial schema is checked on the fields it has.
- `src/runtime/worker.c`:
  - `spawn_row_parse` (shared by `world.spawn` and `world.spawn_batch`): name lookup, u32 check, and on a world whose
    `schema_id` is `rts` the spawn rules plus NONE defaults for omitted `formation_slot`, `target_slot`, `dmg_target`,
    `proj_target`. Non-RTS worlds behave as before.
  - `spawn_precheck`: free slots (`rows <= capacity - alive`) and high water (`hw + rows <= 838,860`), both
    `SU_E_RESOURCE_LIMIT`. Now also applied to `world.spawn` (one row).
  - `h_world_spawn_batch`: rows 1..1024 (else `SU_E_RESOURCE_LIMIT`, as `spawn_batch_limits.check_row_count`), every
    row validated, then prechecks, then spawn in row order, one `SU_EVENT_SPAWNED` per row, one revision bump. Reply:
    `{world_id, world_revision, spawned, entities:[handles]}` (not yet in `docs/AGENT_RPC.md`; needs approval).
  - `field_id_by_name` moved above the spawn handlers (no change).
- `tests/rts/control/test_world_spawn_batch.c` + CMake target `rts_world_spawn_batch`.

## Evidence (WSL Ubuntu 26.04, gcc 15.2.0, cmake 4.2.3)
- Dev build, no warnings. `ctest` 38/38 (was 37/37 at base on the same machine).
- `test_world_spawn_batch`: 73 checks, 0 failures. Covers T-RTS-LOAD-1(g) on the engine worker (over-capacity last
  row and domain-invalid last row refuse with no revision, alive, high-water or event-log change), every D1/2.2 spawn
  rule, stale revision, slot order, NONE defaults, external_id, reply handles.
- Mutant: removing the batch precheck call makes the test fail (15 failures, incl. `SU_E_INTERNAL` partial spawn).
- Native lane (`-DSUTEKH_NATIVE=ON`, AsmJit via `tools/bootstrap_asmjit.py`): build clean, ctest 49/50. The one
  failure is `native_avx2` line 1078, the upper-YMM XSAVE probe ("dirty state not observed") under WSL2/Hyper-V; that
  test links neither the worker nor the agent, so it is environmental, not this change. Not confirmed on a bare-metal
  host.
- `tools/validate_pack.py --check-report` and `tools/check_repository_license.py`: pass.

## Unrun / known gaps
- High-water bound at 838,860 not exercised in C (needs a near-million-row world); Python model only.
- Spool precheck (12) not applied: the worker lacks the supervisor's spool settings. Belongs in the agent slice.
- T-RTS-LOAD-1 (a)-(c), (e) still need agent forwarding.
- Model divergence: `validate_spawn` checks `formation_ox/oy` domains only when `formation_slot != NONE`; the engine
  checks them always (contract 2.2 lists the domain unconditionally). Model should be aligned (bench/rts/ref owner).
- A failure of `su_world_spawn` after passed prechecks returns `SU_E_INTERNAL` with the world partly spawned
  (revision bumped); by contract D3 this is an invariant break.

## Next work
1. Agent forwarding + recovery-log entries for `world.set_fields` and `world.spawn_batch` (the inline
   `LogOp.spawn_values` cannot hold a 1 MiB batch; needs a heap payload), spool precheck, T-RTS-LOAD-1 (a)-(c).
2. `docs/AGENT_RPC.md` entries (approval).
3. Q2 liveness refusal; gates T-RTS-CTRL, CAPS.
