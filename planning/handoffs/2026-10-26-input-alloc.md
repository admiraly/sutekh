<!-- SPDX-FileCopyrightText: 2026 admiraly -->
<!-- SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 -->
# Issue 149: host step allocates when applying per-tick input records (C11)

## State and scope

Locally verified on `fix/issue-149-input-alloc`, based on `origin/main` `9bbd6f7` (merge of PR #151). Integration
is pending.

Modified paths:

- `src/host/su_host_internal.h`: input application storage in `struct SuHostWorld` (`in_cells`, `in_vals`,
  `in_col`, `in_max`, `in_rows`).
- `src/host/su_host.c`: `input_storage_init` (called by `su_host_world_create`), `write_cells`, `apply_record`,
  `undo_record` and `su_host_step` use that storage; `su_host_world_destroy` frees it.
- `include/sutekh/host.h`: `su_host_step` documents the allocation behaviour and the exhaustion code.
- `tests/host/test_host_rollback.c`: glibc malloc/calloc/realloc interposition (static and DLL builds; off under
  AddressSanitizer); `test_no_alloc_inputs`, `test_no_alloc_full_record`; `test_inplace_equivalence` frees two
  snapshots it leaked (found by ASan).
- `tests/host/test_host_registered.c`: `test_no_alloc` also counts 50 ticks carrying records; the failing-operator
  loop (`g_bad_mode` 1..3) pushes an input on the failing tick and counts the apply + undo.
- `planning/handoffs/2026-10-26-input-alloc.md` (this file).

## Design

Before: `apply_record` allocated cells and values (`r->count` each), `write_cells` a column buffer (capacity
cells), and `undo_record` an old-value array plus another column buffer: 3 allocations per tick with a record, 5
for a failing tick with a record (measured below on `origin/main`).

After: the world allocates the storage once at creation, from bounds that already exist, so no new descriptor
field is needed and existing callers are unchanged:

- cells and values: `min(SU_HOST_INPUT_MAX_WRITES, capacity x field count)` entries. That is the tight bound: a
  record's writes are unique (slot, field) cells (`queue_writes` refuses duplicates and more than
  `SU_HOST_INPUT_MAX_WRITES` = 4096 with `SU_E_RESOURCE_LIMIT`), so it cannot exceed either factor. At most
  4096 x 16 B = 64 KiB per world; a 16-row, 2-field world takes 32 entries.
- one column buffer of capacity cells (the world's capacity is fixed for its life; restores of another capacity
  are refused by world identity).
- undo reuses the values array for the old values (the apply's values are no longer needed once written).

Exhaustion: a record with more writes than the storage, which the push functions cannot produce, fails its tick
with `SU_E_RESOURCE_LIMIT` (err->index `UINT32_MAX`, err->tick the tick) before anything is written: the committed
root is unchanged and the record stays queued, as for any failing tick. A column longer than the buffer (not
reachable, defensive) is `SU_E_RESOURCE_LIMIT` from `write_cells`. Creation fails with `SU_E_RESOURCE_LIMIT` if the
storage cannot be allocated.

The write order, the column-per-field write through `su_world_put_committed`, and the undo are unchanged, so the
committed bytes are the same.

Out of scope, found while testing: the first tick after `su_host_snapshot_restore` (the allocating restore) makes
61 allocations with or without inputs. `su_host_snapshot_restore` builds a new entity runtime, whose tick cache
(command queues, job pool, `src/runtime/entity_tick.c`) is built in its first tick, as after world creation.
`su_host_snapshot_restore_inplace` (the documented allocation-free rollback) keeps the runtime and allocates
nothing. The test measures the allocating-restore re-simulation from its second tick; `host.h` now says so.

## Local validation

GCC 16.2.1 and Clang 22.1.8 (Fedora 44), Linux x86-64, `-j 6`, builds in a scratch directory.

```sh
cmake -S . -B <scratch>/debug  -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Debug
cmake -S . -B <scratch>/rel    -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Release
cmake -S . -B <scratch>/native -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Debug -DSUTEKH_NATIVE=ON
cmake -S . -B <scratch>/asan   -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug \
  "-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" \
  "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined" "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined"
cmake --build <dir> -j 6 && (cd <dir> && ctest -j 6 --output-on-failure)
# asan: only the touched tests: ctest -R '^host_(rollback|registered|api)'
```

| Configuration | Build | ctest |
|---|---|---|
| GCC Debug | 0 warnings | **148/148 passed**, 1 skipped (`audio_device`: no audio backend) |
| GCC Release | 0 warnings | **148/148 passed**, same skip |
| GCC Debug `SUTEKH_NATIVE=ON` (AsmJit via `python3 -I tools/bootstrap_asmjit.py`, lock unchanged) | 0 warnings | **165/165 passed**, 2 skipped (`native_unwind` Windows only, `audio_device`) |
| Clang Debug ASan+UBSan, touched tests (`host_rollback*`, `host_registered*`, `host_api*`; `host_rollback_bench_smoke` excluded: its bench binary is not built in that target set) | 0 warnings | **14/14 passed**, no sanitizer report |
| Clang Release, the four touched test binaries | 0 warnings | 0 failures each |

The test count is unchanged (148 on `origin/main` too): the new checks are in existing tests. The first ASan run
reported a leak that predates this change (`test_inplace_equivalence` compared two snapshots it never freed); the
test now frees them.

### The new tests fail on main

The two test files of this branch, compiled against `origin/main` `9bbd6f7`'s libraries (Release):

| Check | `origin/main` | this branch |
|---|---|---|
| 16 ticks with records of up to 64 writes (`test_host_rollback`, static and DLL) | 48 allocations | 0 |
| the same after in-place restore | 48 | 0 |
| the same after allocating restore, ticks 2..16 | 45 | 0 |
| one tick with a 4096-write record | 3 | 0 |
| refused tick (input names a despawned entity) | 2 (counted as a failure) | 0 |
| 50 ticks with records, registered operator (`test_host_registered`) | 150 | 0 |
| failing operator tick with an input (apply + undo), 3 modes | 5 each | 0 |

`test_host_rollback`: 5 failures on main, 0 on the branch (static 96074 checks, DLL 96055).
`test_host_registered`: 4 failures on main, 0 on the branch (628 checks; the DLL variant has no counter: 570).

### Byte-identical output

- `test_host_rollback` prints the digest of its 16-tick input run: `db241156b0d1acd7063debc1c76cef38bc151ef0167a4bd732a94b75c9322ec5`
  on main and on the branch, static and DLL.
- A scratch differential program (not committed: it only links the public DLL API) builds a 64-row world of the
  `emit_then_despawn` fixture with 48 entities, queues 96 ticks of records (positions, velocities, 24 despawns by
  the system), records a replay (keyframe 16, digest every tick) and writes the per-tick digests, the replay and the
  final snapshot. Linked against main's and the branch's `libsutekh_host.so`, all three files are identical
  (`cmp`): replay 107696 bytes (SHA-256 prefix `118deaef489cf4f0`), snapshot 4869 bytes (`2f7a83ebde87131e`),
  digests file (`be29d61c083c6505`).
- `test_host_registered record` (snapshot, replay, digests of the registered-operator world) gives identical files
  on main and on the branch.
- The suites' golden digests (`host_schedule`, `host_compat`, replay tests) pass unchanged.

## Unrun checks and limitations

- The allocation counter needs glibc (it interposes `__libc_malloc` etc.); elsewhere, and under AddressSanitizer,
  the tests print a note and check behaviour only. Windows was not run locally (CI covers the build).
- `aligned_alloc`/`posix_memalign` are not counted; nothing in `src/` uses them.
- Daytona/GitHub CI: see the PR.

## Next work

- Optional: build the entity runtime's tick cache during `su_host_snapshot_restore` so the first tick after an
  allocating restore allocates nothing either (separate issue if wanted).
