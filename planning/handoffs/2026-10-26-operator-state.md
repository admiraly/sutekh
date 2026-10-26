<!-- SPDX-FileCopyrightText: 2026 admiraly -->
<!-- SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 -->
# Issue 139: operator-owned variable-size state in snapshots, digests and replays

## State and scope

Locally verified on `feat/issue-139-operator-state`. The first revision was based on `origin/main` `31192d9` (merge
of PR #147, issue 137). Review round 1 is rebased onto `70ce82d` (merge of PR #150). Integration is pending. The
results below "Local validation" are from the first revision; review round 1 is at the end.

Exact modified paths:

- `include/sutekh/host.h`: new section "operator state"; `SuHostOperatorDesc` version 2 (four trailing u32:
  `state_flags`, `state_max_entries`, `state_max_bytes`, `state_reserved`; `SU_HOST_OPERATOR_DESC_V2_SIZE`);
  `SuHostOperatorRun.state` (trailing); snapshot comment (version 2); the run contract's determinism bullet.
- `src/host/su_host_state.c`, `src/host/su_host_state.h` (new): the engine-owned store and the public
  `su_host_state_*` functions.
- `src/host/su_host_registered.c`: state fields validated at registration, created per world instance, handed to
  `run`, folded into the semantic hash when declared; the step message says when a state cap was reached.
- `src/host/su_host.c`: snapshot version 2 and the `SUOPSTAT` section (write, size, both restores through one
  `snapshot_split`), commit/abort around every tick in `su_host_step`, `su_host_operator_state`.
- `src/host/su_host_internal.h` (`HostOperator.state`, `SuHostWorld.opstate[]`), `src/host/su_host_operators.c`
  (built-in table: the new callback is NULL).
- `cmake/SutekhHostOps.cmake` (new source; 9 new ctest entries).
- `tests/host/test_host_opstate.c` (new).
- `docs/SPACE3D.md` (section 1, "Operator state"), `docs/IMPLEMENTATION_STATUS.md`.
- `planning/handoffs/2026-10-26-operator-state.md`.

## Design decision: engine-owned bytes, mutated through a bounded API

Chosen over operator serialize/deserialize hooks. Reasons:

1. **Canonicality by construction.** The engine keeps the state as a map from a key (u64 entity, u64 sub) to a value
   of 1+ bytes, always sorted by key. The encoding is that map in key order, so the bytes are a function of the
   logical content only. Insert/erase order, replacement history, compactions and arena layout never reach the
   bytes. With hooks, canonicality would be an operator promise the engine cannot check; here it is an engine
   invariant, tested (`test_canonical`: other orders, put instead of modify, junk entries, compactions give the
   same snapshot bytes).
2. **Rollback is simple and exact.** The engine owns a committed side C and a working side W. W is created by the
   first change in a tick (a copy of C's entry table; values are shared and copied on write). It becomes C when the
   tick publishes (an O(1) swap) and is dropped when the tick fails, anywhere in the tick. Hooks would need operator
   code to undo partial changes or to copy itself per tick.
3. **Strict, allocation-free restores.** The engine parses the section itself (sorted unique keys, lengths, totals,
   caps), straight into W, which is preallocated. The in-place restore stays allocation-free and accepts exactly
   what the allocating restore accepts.
4. **Bounded (C11) with defined exhaustion.** Everything is allocated at world creation: two arenas of
   2 x `state_max_bytes` each (review round 1; before: 1 x), two entry tables, one compaction permutation. A change past `state_max_entries` or
   `state_max_bytes` is `SU_E_RESOURCE_LIMIT` and changes nothing. Whether a change fits depends only on the logical
   size, because the engine compacts. A compaction never writes C's arena: it copies into the spare arena, or works
   in place once W has its own arena.
5. **#142 compatible** (below). Every call is a keyed operation that can be journaled per chunk and merged in chunk
   order. No function returns memory that must outlive a call.

Cost model (documented in `host.h`), by construction:
- lookup O(log n); inserting or erasing a key moves the entry table (O(n) x 24 bytes);
- the first change in a tick copies the entry table;
- value bytes are copied only on write: once per value per tick, or reused in place when rewritten at the same
  length in the same tick;
- compactions: each arena is 2 x `state_max_bytes`, so a compaction (copies at most the live bytes, at most
  `state_max_bytes`) leaves at least `state_max_bytes` free. Compactions therefore copy at most as many bytes as
  were written since the previous one: amortized O(1) per written byte, also with the live state at the cap. The
  compacting change itself pays O(live bytes) at once. Memory cost: 4 x `state_max_bytes` of virtual memory per
  operator and world (calloc'd: pages are resident only once written);
- `SU_HOST_STATE_DROP_DEAD` purge: O(n) only on a publish after which an entity may have died (a
  `su_host_despawn` since the last purge, a despawn applied by the tick, or a restore); O(events of the tick) to
  find out; nothing otherwise.

Timing was not measured, and no performance claim is made.

## Other decisions

- **Snapshot format: version bump only for worlds with state.** A world with operator state writes `SUHSNAP`
  version 2: the version 1 parts, then `u64 length | "SUOPSTAT" 0x00 | u32 version 1 | u32 system count |` per system
  with state, in schedule order, `u32 schedule index | u32 n | u64 bytes | n x (u64 entity | u64 sub | u32 len) |
  values`. A world without state writes the version 1 bytes exactly. Each kind accepts only its own version. Replay
  keyframes are snapshots, so they carry the state; the replay format is unchanged (version 1). Header checks are
  now one function (`snapshot_split`). Magic or an unknown version is `SU_E_PARSE`, then the identity is checked
  (`SU_E_SCHEMA_MISMATCH`), then the version must match the world (`SU_E_PARSE`). Before, any version other than 1
  was `SU_E_PARSE` ahead of the identity check. The visible change: version-2 bytes given to a different world are
  now `SU_E_SCHEMA_MISMATCH` instead of `SU_E_PARSE`.
- **Identity.** A declared state folds `state_flags`, `state_max_entries` and `state_max_bytes` into the system's
  semantic hash (`SHA-256("sutekh.host.state.v1\0" || registered hash || 3 x u32 LE)`), and through it into the
  world identity and replay headers. Without state the hash is the #137 hash, unchanged.
- **API version stays 1**, following `host.h`'s convention for additive sections. Descriptor version 2 follows the
  #137 rules: four u32, no padding, 136 bytes LP64 and 100 bytes ILP32, pinned by `_Static_assert`. A v1-size
  descriptor (120 bytes) has no state.
- **Built-in operators** have no state; their table entry has a NULL `state` callback. Their records, identities,
  snapshots and digests are untouched.
- **Entity handles (C05).** Keys are stable IDs: `entity` is normally a generational handle, valid across snapshot,
  restore and replay because the entity table is restored with the state. Values are opaque bytes. An operator that
  stores references in them must store handles or its own IDs, never pointers.
- **Despawned entities: opt-in engine help.** With `SU_HOST_STATE_DROP_DEAD`, put/alloc refuse a key whose entity
  is not alive in the tick's working entity table (`SU_E_STALE_HANDLE`). When a tick publishes, every entry whose
  entity is not alive in the committed entity table is erased, as part of that tick (despawned during the tick, or
  by `su_host_despawn` before it). Restored keys must have a generation in 1..0xFFFFFFFE. Without the flag, `entity`
  is an opaque u64 and the operator is responsible (`su_host_state_erase_entity`). Both rules depend only on state
  plus entity table, so both are deterministic. Entries of an entity despawned between ticks stay visible (and in
  snapshots) until the next tick publishes. That is documented. The purge scan only runs when an entity may have
  died since the last one: `su_host_despawn` marks the states, `su_host_step` looks for `SU_EVENT_DESPAWNED` among
  the tick's event records (or assumes a death when the ring evicted some of them before they were read), and a
  restore marks them too. An IR event that uses that kind value only causes an extra scan.
- **Changes only inside `run`.** Changes through a view (`su_host_operator_state`) or after `run` returns are
  `SU_E_INVALID_ARGUMENT`, even with the const cast away, so every state change is a function of the tick and
  replays reproduce it. `put` from the state's own memory is refused.
- **Cap failures:** `run` may handle `SU_E_RESOURCE_LIMIT` or return it. The tick then fails with that code, nothing
  is committed, and the step message ends with "(operator state cap reached)", only when the returned code is
  `SU_E_RESOURCE_LIMIT` and the run saw a cap refusal.
- **Writable pointers:** only those of `su_host_state_alloc` and `su_host_state_modify`. The `get`/`entry`/view
  pointers may point at bytes shared with the committed state, so writing through them corrupts committed state
  undetected (documented as a contract violation, C07).

## Plan for issue 142 (parallel chunked operators; not implemented, also in `host.h`)

Rewritten in review round 1. The first version was unsound for lockstep: its results depended on whether the
engine chose chunked or serial runs, which depends on the worker count.

- **Selection** is by declaration only: a descriptor flag that is part of the identity. Without it, an operator always
  gets today's serial call (read-your-writes, `modify` never refused for space), on any number of workers. With it,
  the operator always gets chunked semantics, also on one thread and also for a single chunk (`chunk_count == 1`).
- **Partition:** a function of the world's capacity only: fixed-size row chunks, the chunk size an engine-version
  constant like `SU_CHUNK_SLOTS`. Workers only decide which thread runs which chunk.
- **Reads:** every chunk sees the state as it was when the operator's run began in this tick, frozen, and never
  another chunk's changes. A chunk sees its own changes through `get` and `modify` (its journal is consulted first).
  `count`, `lower_bound` and `entry` enumerate the frozen state only.
- **Writes:** `put`/`alloc`/`modify`/`erase`/`erase_entity` append to the chunk's journal; `modify` copies the value
  in. Each journal has a declared capacity (operations and bytes, new trailing descriptor fields), allocated at
  world creation for every chunk of the capacity. A change that does not fit is `SU_E_RESOURCE_LIMIT`, `modify`
  included. That refusal depends only on the chunk's own calls, so it is deterministic. The journal capacities are
  declared caps and join the semantic hash.
- **Merge:** after the last chunk the journals are applied in chunk order (0..chunk_count-1), each in call order.
  The same key changed by two chunks keeps the later chunk's result, or a declared merge callback (part of the
  identity) combines them in chunk order. `state_max_*` are checked on the merged result; exceeding one fails the
  tick, nothing committed.
- An operator keying per-entity data by handle and changing only its own rows' keys never conflicts. No signature
  changes.

## Local validation

Linux 7.2.9 (Fedora 44 based), Intel Core i7-14700K (20 CPUs), GCC 16.2.1, Clang 22.1.8, CMake 4.3.0, Ninja 1.13.2.
Builds `-j 6`, `SUTEKH_WERROR` ON (`-Wall -Wextra -pedantic -Werror`). Build directories and `TMPDIR` under a scratch
directory outside the tree (the root file system was full). Renderer and glTF importer skipped by configure, as on
the baseline. AsmJit via `python3 tools/bootstrap_asmjit.py` (lock unchanged).

```sh
cmake -S . -B <scratch>/debug   -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Debug
cmake -S . -B <scratch>/release -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Release
cmake -S . -B <scratch>/native  -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Debug -DSUTEKH_NATIVE=ON
cmake -S . -B <scratch>/asan    -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug \
  "-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" \
  "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined" "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined"
cmake --build <dir> -j 6 && (cd <dir> && ctest -j 6 --output-on-failure)
```

| Configuration | Build | ctest |
|---|---|---|
| GCC Debug | 0 warnings | **145/145 passed**, 1 skipped (`audio_device`: no audio backend on Linux) |
| GCC Release | 0 warnings | **145/145 passed**, same skip |
| GCC Debug `SUTEKH_NATIVE=ON` | 0 warnings | **162/162 passed**, 2 skipped (`native_unwind` Windows only, `audio_device`) |

Baseline `origin/main` has 136 tests (`ctest -N` on its Release build); the 9 new ones are the `host_opstate*`
entries.

New tests (`tests/host/test_host_opstate.c`; operator, reference model and IR system in the test TU):
- `host_opstate`: 2,212 checks, static, with allocation counting.
- `host_opstate_dll`: 2,202 checks.
- `host_opstate_record`, then `host_opstate_verify_same_{dll,static}`, `_entries_static`, `_bytes_dll`,
  `_flags_static`, `_nostate_dll`.

They cover:
- **Registration rules:** each zero/nonzero mismatch, unknown flag, reserved field, both caps above the maximum; the
  v2 size.
- **Views and versions:** read-only views; version 2 snapshots with the section, version 1 for a registered operator
  without state.
- **Rollback:** 40 ticks against an independent model (state and output columns every tick). Restore to tick 10 with
  both restores, twice each, then re-simulate 30 ticks that changed the state: SHA-256 and fast digests equal every
  tick.
- **Digests see the state:** a state-only change (columns equal) changes both digests.
- **Canonicality:** 150 ticks in another mutation order (descending rows, heat first, get+put instead of modify,
  junk insert/erase, so more compactions): identical snapshot bytes.
- **Caps:**
  - a failing churn tick (300 replacements, copy and in-place compactions) leaves the committed state intact;
  - the next tick equals a twin world that never ran the failed tick;
  - a successful churn keeps every other entry byte for byte;
  - a shuffle (values re-put longer in reverse key order, then compacted in place) gives the expected bytes;
  - a strict flood past the cap fails the tick with `SU_E_RESOURCE_LIMIT` and the message, world unchanged and twin
    equal;
  - a lenient flood stops at a cap;
  - a churn at the byte cap still succeeds.
- **Corruption** (both restores refuse with `SU_E_PARSE`, world unchanged): truncated, trailing byte, keys out of
  order, duplicate key, zero length, lengths against the total, a well-formed section with more entries than
  declared, a count above the cap, section magic, system index, snapshot version 1, a truncated section, a value
  larger than `state_max_bytes`.
- **Replay:** keyframes every 5 ticks, digests every tick. Seek 23 and 35, seek 7 and play to the end, all equal to
  the recording. All 9 keyframes carry the section. One flipped value byte in keyframe 5 is a desync at tick 6.
- **DROP_DEAD:** dead entity's entries erased at publish, exactly those (compared with the flagless world); stale
  handle refused; respawn in the slot starts empty; generation-0 key refused on restore. Without the flag, entries
  stay.
- **Allocation:** 0 allocations in 600 ticks across two worlds, with state changes, compactions (more bytes placed
  than the arena), a purge and a repeated in-place restore (`-Wl,--wrap`, static Linux build).
- **Misuse:** changes through a view and NULL handles refused; `put` from the state's own memory refused.
- **Cross-process:** the same declaration restores and replays to the recording process's digests, through the
  static library and the DLL. Other `state_max_entries`, `state_max_bytes`, `state_flags`, or no state:
  `SU_E_SCHEMA_MISMATCH` for both restores and the replay.

ASan+UBSan (Clang, the configuration above): `host_opstate*` and `host_registered*`, 19 tests, all pass, 0
diagnostics. The full ASan suite was not run. #137 recorded 21 pre-existing failures on main in that configuration,
and they are not touched here.

**Byte-identical without operator state.** First, the whole existing suite passes unchanged, including the pinned
digests: `example_arena_host*` `--expect-digest`, `host_ops` racer digests, and the replay record/verify across
processes. Second, a scratch script (not committed) ran the same digest-producing commands on a Release build of
`origin/main` `31192d9` (`git archive`) and on this branch's Release build. It compares:
- `arena_host --ticks 900 --rollback-check`, with and without `--respond`;
- the canonical replay file of `test_replay record`;
- the snapshot, replay and digest files of `test_host_registered record` (a registered operator without state);
- the final state and event-log digests plus every state digest of `racer_microcosm --n 8 --ticks 720
  --digest-every 120`;
- the `minimal_host` output.

All files are identical. SHA-256 of the list of their hashes: `49a3d1717eb3f5d6a0a07f275998a8260ac3cf71f8cd5a3e57d037a3d157f418`, the same for both builds. The racer JSON line itself differs only
in its timing fields.

**Mutation check** (scratch script, not committed). Each mutant is applied, built and run (in-process test, then
record/verify), and the source is restored and compared. 22 engine mutants, **22 killed**:
- no commit at publish;
- no abort on a failed tick;
- `modify` writes shared bytes;
- compaction inside the committed arena;
- caps by arena top (fragmentation-dependent);
- no entry cap;
- load accepts unsorted keys;
- load ignores the entry cap;
- caps not in the identity (killed by the cross-process verifies);
- no dead-entity purge;
- in-place restore keeps the old state;
- W not copied from C;
- no DROP_DEAD check at put;
- a `malloc` in the tick (435 counted);
- no alias check;
- entries appended instead of sorted;
- the allocating restore skips the state;
- generation 0 accepted;
- the section zeroed;
- in-place compaction in table order;
- copy compaction keeps the replaced value (killed by a glibc heap abort from the arena overrun, SIGABRT);
- a same-length rewrite in place even when shared.

Repository checks: `python3 tools/validate_pack.py --check-report` passed, `python3
tools/check_repository_license.py` passed, `git diff --check` clean.

## Review round 1

The independent review returned PASS-WITH-NITS with no correctness bug; CI passed (7 jobs). Fixes:

1. **Compaction near the byte cap.** Each arena is now 2 x `state_max_bytes`. Before, with the live state near the
   cap, the first rewrite of every tick compacted all of W, costing O(live) per tick. Now a compaction leaves at
   least `state_max_bytes` free, so it is amortized O(1) per written byte. Memory: 4 x `state_max_bytes` virtual per
   operator and world (was 2x), resident only once written. `SuHostStateInfo.compactions` (a diagnostic counter,
   not state) makes it testable: at 4,024 of 4,096 live bytes, 100 changing ticks compact at most 4 times (101
   without the slack, mutant M27).
2. **DROP_DEAD purge only after possible deaths.** Triggers: `su_host_despawn`, a `SU_EVENT_DESPAWNED` record in the
   tick's events (or events evicted before being read), or a restore. Otherwise the publish does no scan. Costs are
   documented in `host.h`. New tests: a despawn applied inside the tick (`game.clear` is now IR 0.2 with
   `despawn_self`), and a restored snapshot holding entries that were purged after it was taken.
3. **"(operator state cap reached)"** is appended only when `run` returned `SU_E_RESOURCE_LIMIT` (tested with a run
   that sees refusals and then fails with another code).
4. **Pointers** (`host.h`): only `alloc`/`modify` pointers are writable. Writing through `get`/`entry`/view pointers
   corrupts committed state undetected.
5. **#142 plan rewritten** (`host.h` and the section above):
   - selection by declaration only, journal semantics even for one chunk;
   - the partition depends only on capacity;
   - reads are frozen, plus the chunk's own changes through `get`/`modify`;
   - per-chunk journal caps with deterministic `SU_E_RESOURCE_LIMIT`, `modify` included;
   - the caps are part of the identity.
   The `modify` comment now limits "never refused" to the serial call.

Also tracked separately (not touched here): the `su_replay_open` use-after-free on an error path is #148; the
allocation when an input record applies in `su_host_step` is #149.

Test changes:
- canonicality runs 300 ticks, and only the reordered world's placed bytes are counted (more than one arena, so it
  compacts);
- the allocation test counts each world separately: 600 ticks each, each placing more than an arena, 0
  allocations in 1,200 ticks plus the in-place restore.

Validation (same machine and commands as above):

- GCC Debug **145/145**, GCC Release **145/145** (skip: `audio_device`), GCC Debug `SUTEKH_NATIVE=ON` **162/162** (skips:
  `native_unwind`, `audio_device`); 0 warnings.
- `host_opstate` 3040 checks, `host_opstate_dll` 3030, 0 failures.
- Clang ASan+UBSan, `host_opstate*` and `host_registered*`: 19/19, 0 diagnostics.
- Byte-identical comparison repeated against the new base `70ce82d` (Release, `git archive`, 136 tests): all files
  identical, list hash `49a3d1717eb3f5d6a0a07f275998a8260ac3cf71f8cd5a3e57d037a3d157f418`, unchanged.
- Pack report, license check and `git diff --check` pass.

Mutation check, round 1:
- the 21 earlier mutants (M1-M20, M22) plus five new ones: tick despawns not detected, host despawn not noted, a
  restore not re-arming the purge, the cap note on any failure code, and the arena without slack;
- **26 of 26 killed**;
- M21 (the copy compaction keeps the value being replaced) is now an equivalent mutant: the slack absorbs it, and it
  only wastes space until the next compaction. It was dropped.

Source SHA-256 (round 1):
- `include/sutekh/host.h` `b46e9b217868b9b6a8c2ac105cb00b9a629fd931566e577d4065f8234ae6a39d`
- `src/host/su_host_state.c` `6f4f09038e813d98efa69a42f95f204c8112850804c832b9f8178f5fa634e58c`
- `src/host/su_host_state.h` `d8b4a4d0bc23cebfa4404e835ebc8eb6725d40cec30fafb56cb8552610e00aa1`
- `src/host/su_host.c` `1fe8fb1f6524e251a8275aa2c111a14e6b45d2d8561140cbee6fbc7c69ce1f24`
- `src/host/su_host_registered.c` `9da7d37f7b6599cc6aeb7f93296d2516461704591d0dc48a94b9af4fded83ce2`
- `tests/host/test_host_opstate.c` `525003be287832a1e5b649c4ca9bc2d2179ddbb2e2d3835e325f72933429d89f`

## Unrun checks and limitations

- **Not run:** Windows (MSVC, clang-cl), macOS, renderer-on, ThreadSanitizer, the full ASan suite, ILP32 (only the
  `_Static_assert` values are stated for it). No CI result is claimed.
- **No timing measured.** The cost model above is by construction, not measured.
- **Allocation counting** covers the static Linux build only, as in #137. `su_host_step` still allocates when an input
  record applies (pre-existing, outside this change, #149), so the counted ticks run without inputs.
- **In-process only**, as registered operators: the worker and `sutekh serve` do not see them.
- **Not in scope:** state for built-in operators, host-side mutation between ticks (deliberately absent, so every
  change is part of a tick and replays reproduce it), the #142 journal (planned above).
- `SuHostStateInfo` and `SuHostStateEntry` are fixed-layout output structs. A later field would need a new function.

Next: push, open the PR closing #139, independent review, CI, integration.
