# M3-06 runtime mutation campaigns (2026-10-17)

Answers items 1 to 3 and 5 of "Required next actual campaigns" in
`planning/evidence/m3-independent-review/mutation-inventory-review.md`. Item 4 is partly done (M28; M20 exact
variants and the M33 coherent re-verification are still open). Every campaign mutates real source (or the replay
validator) in an isolated copy, requires an unmutated baseline to pass first and the restored tree to pass last, and
reports a compile error as `build_failed`, never as a kill. WSL Ubuntu 26.04, gcc 15.2.0, shared VM. No performance
claim.

| file | campaign | base revision | result |
|---|---|---|---|
| `runtime.json` | `tests/rts/runtime_mutants.py`: M3-M19, M3-M28, M3-M30, M3-M35 (19 source mutants, git worktree, ctest detectors) | `db5af7d` | 18 killed, 1 equivalent, 0 survived, 0 build_failed; baseline and restored pass |
| `replay.json` | `tests/rts/replay/replay_mutants.py`: M3-M18 (6 validator mutants; model and engine replay tests) | `db5af7d` | 6 killed; baseline and restored pass |
| `host-mutants.log` | `tests/scheduler/run_host_mutants.sh`: M3-M26, M3-M29, M3-M32, M3-M34 and the other host-entry mutants (15, scratch copies) | `f548656` | 13 killed by test failure, 2 survived (see below), 0 build failures; baseline ok |
| `gen-mutants.log` | `tests/rts/gen/mutants.py`: M3-M17 generator (15 detect, 3 equivalent) | `db5af7d` | 14 killed by a scene digest, 1 known gap (M04, caught by the PRNG known-answer test), 3 equivalent unchanged |

## Mapping to the review's gap table

| contract mutant | status now | evidence |
|---|---|---|
| M3-M17 | recorded: 14 of 15 killed by scene digests; M04 (final shift 31 to 30 changes only bit 0 of the u32) is digest-insensitive for the committed scenes and is detected by the PRNG KAT only; closing it needs a normative 7.1 change (owner decision) | `gen-mutants.log` |
| M3-M18 | killed, six variants (compare only t = 0, skip the final digest, skip the missing-digest check, ignore seq order in the reader, apply a boundary's commands in reverse order, ignore digest-record order) | `replay.json` |
| M3-M19 | killed for `world.spawn_batch`, `world.set_fields` and `world.spawn` | `runtime.json` M3-M19a to c |
| M3-M26 | killed (`host_dead_rows_not_zeroed`) | `host-mutants.log` |
| M3-M28 | killed for struct cap, events cap and the profile test | `runtime.json` M3-M28a to c |
| M3-M29 | killed (`host_effects_ignored`) | `host-mutants.log` |
| M3-M30 | killed: flight 0, flight above 64, system-owned field, per-field domain in set_fields, writable set (see equivalence) | `runtime.json` |
| M3-M32 | killed (`host_staging_copy_removed`) | `host-mutants.log` |
| M3-M34 | killed: host branch removed and registry lookup unchecked | `host-mutants.log` |
| M3-M35 | killed: omitted reload/damage/flight, omitted kind, set_fields resulting-row rules, formation_gen both directions, formation_ox and formation_oy | `runtime.json` |

## Equivalent or unreachable results (reasoned, not counted as kills)

- `M3-M30c` (set_fields explicit writable-set check removed): `su_rts_ctl_check_value` also returns `SU_E_TYPE` for
  every name outside the writable set, so the explicit check is a redundant first guard. `M3-M30g` removes both and is
  killed.
- `host_field_order_reversed_equivalent`: the harness's own equivalence claim (columns are looked up by id and
  committed whole); it survives as claimed.
- `native_host_guard_removed`: reachable only in a native build, and no host-entry test reaches the worker guard.
  This is a gap, not an equivalence, and stays open.

## What this found

- Two test gaps, closed in `59fa4c4` and `db5af7d` and then re-run to kill: `world.spawn` ignoring the revision check
  survived (no stale-revision case for `world.spawn`/`world.spawn_batch`/`world.despawn`), and the
  `formation_ox/oy/gen` cases named a dead slot, so the Q2 liveness rule refused them first and the rules under test
  were never reached. Three replay-validator gaps (seq order within a boundary, application order, digest-record
  order) were closed the same way.
- A broken harness: `run_host_mutants.sh` copied a stale file set without `profiles/`, so CMake could not configure and
  all 14 mutants were reported "killed (does not build)", including the one declared equivalent. Repaired in `7d23cfb`
  (complete copy, baseline, `BUILD FAILED` classified apart from a kill).

## Still open in the M3-06 inventory

M3-M10 late-arrival, M3-M11 blocked and underflow, M3-M13 double-kill variants; M3-M20 exact projectile-clear
variant; coherent re-verification of M3-M33 on the current source; native-build run of the host harness (the
`native_host_guard_removed` gap); the final-gate statement of M3-06 is not made here.
