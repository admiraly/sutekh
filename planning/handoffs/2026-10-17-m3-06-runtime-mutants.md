# Handoff: M3-06 slice, runtime and replay mutation campaigns

Base: `2b32abd` (start), campaigns at `db5af7d` and `f548656`. Evidence and mapping:
`planning/evidence/m3-runtime-mutants/README.md`.

## Change
- `tests/rts/runtime_mutants.py`: real-source control-plane mutants in a git worktree (M3-M19, M3-M28, M3-M30,
  M3-M35), ctest detectors, baseline and restored runs, `build_failed` apart from `killed`.
- `tests/rts/replay/replay_mutants.py`: M3-M18 validator mutants against the model and engine replay tests.
- `tests/scheduler/run_host_mutants.sh` repaired (it omitted `profiles/`, so every mutant was falsely "killed";
  now baseline-checked, `BUILD FAILED` classified apart) and extended with the M3-M34 registry mutant.
- Tests strengthened where a mutant survived: stale `expected_world_revision` for `world.spawn`, `world.spawn_batch`,
  `world.despawn` (`test_ctrl_engine.py`); formation tests name a live slot so `formation_ox/oy/gen` are tested
  rather than masked by Q2; replay tests for seq order within a boundary, application order and digest-record order
  (`test_replay.py`).

## Results
18 killed, 1 equivalent (M3-M30c, redundant guard), 0 survived (runtime); 6 of 6 replay mutants killed; host harness
13 killed, 2 survived (the declared-equivalent field-order mutant and the native-only guard gap); generator 14 of
15 killed by digests, M04 a documented known gap caught by the KAT. Default ctest after all edits: 56/56 (WSL
Ubuntu, gcc 15.2.0). The native lane was not re-run; no native-compiled source changed in this slice.

## Open (M3-06 stays `implementing`)
M3-M10 late-arrival, M3-M11 blocked/underflow, M3-M13 double-kill variants; M3-M20 exact projectile-clear variant;
M3-M33 coherent current-source re-verification; native-build host harness (`native_host_guard_removed`); an
independent reviewer pass over this evidence. No whole-inventory PASS is claimed.
