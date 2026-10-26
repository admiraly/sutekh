<!-- SPDX-FileCopyrightText: 2026 admiraly -->
<!-- SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 -->
# Issue 153: world-level budget for operator-state memory

## State and scope

Verified locally on `feat/issue-153-state-budget`, based on `origin/main` `d002dac` (merge of PR #156). CI and
integration are still pending.

Modified paths:

- `include/sutekh/host.h`: `SuHostWorldDesc.state_budget_bytes` added at the end, `SU_HOST_STATE_BUDGET_DEFAULT`
  (1 GiB), `SU_HOST_WORLD_DESC_V2_SIZE` (88 bytes with 8-byte pointers; the descriptor is now 96), the versioning text,
  the admission paragraph at `su_host_world_create`, and the exact per-operator formula in "operator state", Bounds.
- `src/host/su_host_state.c`, `src/host/su_host_state.h`: `su_hstate_reservation`. The sizes come from a single
  `layout()`, and `su_hstate_create` allocates from that same layout. Static pins: 24-byte entry cells, and a state
  header under 256 bytes.
- `src/host/su_host_registered.c`, `src/host/su_host_internal.h`: `su_host_operator_state_reservation(op)`. It is 0
  for built-in operators and for operators without state.
- `src/host/su_host.c`: `admit_state_budget`, called after the capsule loop and before `su_sched_finalize`. At that
  point nothing of the world's state, columns or operator scratch has been allocated yet. The v2 size is accepted, and
  new size pins were added.
- `tests/host/test_host_opstate.c`: section 11 (`test_budget`). The record builder was factored out into
  `record_json`. The allocation wrap also records the largest single request.
- `docs/IMPLEMENTATION_STATUS.md`, this handoff.

## Behaviour

- Reservation per stateful operator: `4 x state_max_bytes + 52 x state_max_entries + sizeof(SuHostState)`. These are
  the exact bytes `su_hstate_create` callocs, from one function.
- World creation sums the reservations in schedule (capsule) order. The first operator that brings the sum over the
  budget gets `SU_E_RESOURCE_LIMIT`, with `err->index` set to its capsule and this message:
  `capsule N: the operator state of <system> (<r> bytes) brings the world to <total> bytes, over its operator-state
  budget of <budget> bytes`. `*out` is NULL.
- Budget 0, or a descriptor of size v1 or v2, uses the 1 GiB default (0 does not mean unlimited). A size other than
  v1, v2 or at least `sizeof` is `SU_E_INVALID_ARGUMENT`; that includes a size ending inside the budget. A nonzero
  tail is refused as before.
- The budget is not stored in the world, so identity, snapshots and replays are unchanged. Worlds without stateful
  operators skip the check entirely.

## Commands and results

Out-of-tree build directories, `-j 6`.

- Debug (`-DSUTEKH_NATIVE=OFF`): `ctest -j 6` gave 161/161 passed (audio_device skipped).
- Release: 161/161 passed (audio_device skipped).
- Native Debug (`-DSUTEKH_NATIVE=ON`, AsmJit from `tools/bootstrap_asmjit.py`): 179/179 passed (native_unwind and
  audio_device skipped).
- Clang `-fsanitize=address,undefined` Debug, run on the touched tests: test_host_opstate (3083 checks) and its DLL
  variant (3069), test_host_schedule (208 / 197), and test_host_registered (628 / 570). All had 0 failures and no
  sanitizer reports. GCC's libasan is not installed on this machine, so ASan was built with Clang.
- Byte identity against `origin/main` `d002dac` (Release builds of both trees), compared with `diff -r`. Everything
  was identical: minimal_host output, the arena_host output for `--ticks 900 --rollback-check` with and without
  `--respond`, the `test_host_registered record` files (stateless registered operator: snapshot, replay, digests), and
  the `test_host_opstate record` files (stateful world admitted under the default).
- `python3 tools/validate_pack.py --check-report` and `python3 tools/check_repository_license.py` both exited 0.

## Tests added (test_host_opstate section 11)

- Exact reservation, read from the refusal message: it equals the formula plus a header under 256 bytes, and the
  header is the same for two different declarations. A budget equal to the reservation is admitted, and one byte less
  is refused, with the full message checked.
- One operator fits and one doesn't: game.hull plus budget.r1 fit in r_hull + r_r1. Under the same budget,
  budget.big (64 times the bytes) is refused with the exact message.
- Four stateful operators are refused one byte short of their sum, naming budget.r3 (capsule 4). A smaller budget is
  crossed earlier, by budget.r2. The raised budget admits the world, and it steps.
- Default: an operator reserving just over 1 GiB is refused with "budget of 1073741824 bytes" for budget 0, for the
  v2 size and for the v1 size. The v1 and v2 cases carry UINT64_MAX in the uncovered field, to show it is ignored. A
  size ending inside the budget field is refused.
- No allocation (static Linux wrap): during the refused 1 GiB world, the largest single request is below
  state_max_bytes, half an arena. An admitted world does request its arena.
- Identity is the same for budgets 0, exact and UINT64_MAX, and for the v1 descriptor.

## Not run or blocked

- CI on the PR: pending at the time of writing.
- Windows/MSVC and 32-bit builds were not run locally. The `_Static_assert` size pins are guarded by `sizeof(void *)`.

## Next work

- None required. If one shared budget across several worlds is ever wanted (process-wide), that would be a separate
  request.
