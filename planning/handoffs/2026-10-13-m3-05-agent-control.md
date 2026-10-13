# Handoff: M3-05 slice, agent forwarding and recovery of world.set_fields / world.spawn_batch; host systems in world.create

Base: `1eb240f` (worker-side spawn_batch). Scope: supervisor (`su_agent.c`), worker restore entry parsing, recovery
test, `docs/AGENT_RPC.md` section 18 (owner granted standing permission on 2026-10-13).

## Change
- `src/agent/su_agent.c`
  - `world.set_fields` and `world.spawn_batch` are routed and advertised (`h_world_control`). The batch array is
    canonicalised (`ctl_canon`: item keys checked, u32 values, members in order received), stored in the spool under
    ext `control` *before* forwarding (a full spool refuses with no change), then forwarded. An accepted call is one
    `LOG_CONTROL` replay-log entry (`ctl`, `ctl_sha`); `sync_after` requires rev + 1 and an unchanged tick, as for
    spawn/despawn. Recovery reads the spooled batch and replays it verbatim; GC keeps entries the log names.
  - `world.create` `capsule_paths` accepts `{capsule_path, host_operator}` items (host systems, M3-04a). Host entries
    are recognised by `source_sha == sha256("")` (what the worker reports for a host entry; no IR module has an empty
    source) and their `system_id` is the operator name, so `SysRef` is unchanged. `write_capsules`,
    `write_capsules_disk` and the evaluate path emit `host_operator` and no source for them.
- `src/runtime/worker.c` `restore_entry_fill`: a `host_operator` entry becomes a `SU_SYS_KIND_HOST` `CkptSys`
  (operator must equal `system_id`, no `source_path`). Before, `kind` was never set from the supervisor's list, so a
  host system could not be restored from a supervisor checkpoint.
- `tests/recovery/test_recovery.c` `scenario_control_batches` (twice: replay-log only, and from a checkpoint): an RTS
  world created through the supervisor with the spatial host capsule; four despawns, refused batches (capacity,
  domain, unknown item key), one accepted `spawn_batch`, refused and accepted `set_fields`; SIGKILL; the replacement
  must match the control session's digest, tick, revision and event log, then both continue.

## Evidence (WSL Ubuntu 26.04, gcc 15.2.0)
- Dev: build clean, ctest 38/38. `test_recovery` 3642 checks, 0 failures.
- Mutants (each in a separate worktree, then discarded):
  - control batches not logged (`LOG_NONE` in `h_world_control`): 25 failures.
  - `restore_entry_fill` host branch removed: 25 failures.
  - The first version of the test took a digest (world.snapshot = checkpoint) before the kill, which hid the first
    mutant; the pre-kill digest was removed and the mutant then failed as above.
- Native lane (`-DSUTEKH_NATIVE=ON`): build clean, ctest 49/50; the failure is `native_avx2`'s upper-YMM XSAVE probe
  under WSL2, as on the base revision (that test links neither worker nor supervisor).

## Unrun / gaps
- Spool precheck formula of contract 12 not implemented (documented as "not yet" in AGENT_RPC 18).
- T-RTS-LOAD-1 (a) 1,024-row near-maximum request, (b)/(c) the exact 1 MiB boundary through the supervisor: not run.
- A 1 MiB client line is forwarded at most as long as received (canonical form is never longer than compact input);
  a client line with whitespace near the cap is fine, but the bound is not tested.
- Host systems in `candidate.evaluate` against a world use an empty source path; not exercised.

## Next work
1. T-RTS-LOAD-1 (a)-(c) through the supervisor; spool precheck; T-RTS-CTRL-1 full matrix on the 39-field schema.
2. Q2 liveness refusal; T-RTS-CAPS-1..3; T-RTS-REPLAY-1..3.
