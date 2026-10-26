<!-- SPDX-FileCopyrightText: 2026 admiraly -->
<!-- SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 -->
# Issue 137: game-defined (registered) host operators

## State and scope

Locally verified on `feat/issue-137-registered-operators`, based on `origin/main`
`07d06de` (merge of PR #129). CI and integration are pending.

Exact modified paths:

- `include/sutekh/host.h` (new section "registered host operators"; listing and refusal text of "host operators")
- `src/host/su_host_registered.c` (new: registry, per-world instances, the run adapter)
- `src/host/su_host_internal.h` (`HostOperator`: callbacks receive their operator; `reg`, `last_error`, `record_name`)
- `src/host/su_host_operators.c` (built-in table adapted; listing = built-in, then registered)
- `src/host/su_host.c` (record check with the explicit name, instance creation, operator-data family check,
  failure detail in step errors, refusal text)
- `src/runtime/rts/host_decl.c`, `src/runtime/rts/host_decl.h` (`su_host_decl_build_named`,
  `su_host_load_bound_bytes_named`; NULL name = the previous functions exactly)
- `cmake/SutekhHostOps.cmake` (new source; 8 new ctest entries)
- `tests/host/test_host_registered.c` (new)
- `docs/SPACE3D.md` (section 1, "Registered operators"), `docs/AGENT_RPC.md` (world.create: not visible to the
  worker), `docs/IMPLEMENTATION_STATUS.md`
- `planning/handoffs/2026-10-26-registered-operators.md`

## What was implemented

`su_host_register_operator(const SuHostOperatorDesc *, SuHostError *)`, called before `su_host_world_create`. The
descriptor carries `struct_size`, `flags`, system ID, schema ID, operator name and version, constants hash, the field
table (`SuHostOperatorField`: id, value type u32/i32/i64/q32, access, name), `scratch_size(capacity)`, `run`, optional
`parse_data`/`release_data` and a `user` pointer. Registration copies strings and the field table. Internally each
registration becomes an `SuHostOpDesc` whose `run` is an adapter, so the record loader, the schedule (union table,
`su_sched_add_host_bound`), the effect analysis and the tick path are the ones built-in operators use.

Enforcement, all in `src/host/su_host_registered.c` `reg_run` (no allocation; every buffer is created with the world):

- `run` receives only its declared lanes (`SuHostOperatorRun.columns`, descriptor IDs, also for bound records) and no
  world pointer: other columns are unreachable through the API.
- Read lanes are hashed (XXH64) before and after `run`; a change fails the tick with `SU_E_UNDECLARED_WRITE` and
  `err->message` names the lane. Opt-out: `SU_HOST_OPERATOR_TRUST_INPUTS`.
- Write lanes are pre-filled before `run` (zero for write-only lanes, so staging leaks no undeclared read; the input
  value for read-write lanes, done by `schedule.c run_host`), so every row is defined. The engine does not check that
  `run` stored a row; a row it skips keeps the pre-filled value.
- Scratch is zeroed before every call, always: scratch is not in snapshots, so state kept in it across ticks would
  break rollback and lockstep silently. (A scratch-keeping flag existed in the first revision of this PR and was
  removed in the review round; bit 2 is refused.)
- Identity: `host_semantic_digest` adds `"operator":"<name>"` for named (registered) operators only; built-in digests
  are byte-identical (name NULL). At world creation `reg_identity` then folds the descriptor flags and the scratch
  size returned for the world's capacity into the system's semantic hash:
  `SHA-256("sutekh.host.registered.v1\0" || record semantic hash || u32 flags || u64 scratch bytes)`, little-endian.
  That hash is what `su_host_system_info`, `su_host_world_identity` and replay headers carry.
- Names: system ID, schema ID, operator name and every lane name must be identifiers (`[A-Za-z0-9_.-]`), so the
  canonical JSON text hashed by `host_semantic_digest` (which writes names without escaping) is unambiguous. The
  review built two different declarations with one semantic hash through quotes in names; such names are refused now.
- Errors: malformed descriptor `SU_E_INVALID_ARGUMENT` (with field index); duplicate ID, built-in ID, a reserved prefix
  (`rts.`, `space3d.`, `sutekh.`, compared ignoring letter case) or a reserved operator name (`rts_ops*`,
  `space3d_ops*`, `sutekh*`, any case; this covers every built-in operator name) `SU_E_DUPLICATE_KEY`; full registry
  (64) or no memory `SU_E_RESOURCE_LIMIT`; an unknown record stays `SU_E_CAPABILITY_DENIED`.
- Failing runs: `err->message` names the code with `su_host_status_name` and appends the operator's own reason
  (`SuHostOperatorRun.reason`, 128 bytes, "" at entry), for example `operator run returned SU_E_RESOURCE_LIMIT:
  magazine empty in row 2`.

## Design decisions

- **serve/worker:** registration is in-process `host.h` only. The worker's registry (`host_registry.c`,
  `su_host_find`) is unchanged; `sutekh serve` / `world.create` refuse a registered operator's record as unknown
  (`SU_E_CAPABILITY_DENIED`). No game-specific worker build is provided. Documented in `host.h`, `docs/SPACE3D.md`
  section 1 and `docs/AGENT_RPC.md` section 18.
- **API version:** `SU_HOST_API_VERSION` stays 1, following this header's convention for additive sections ("additive,
  API version 1" on events, rollback operations, host operators, operator data, tiers). Existing structs are
  unchanged; feature detection at compile time: `#ifdef SU_HOST_MAX_REGISTERED_OPERATORS`.
- **Public run interface instead of the internal one:** `SuHostOpDesc.run` takes internal `SuWorld`/`SuInvocation`
  types that are not installed; the public `SuHostOperatorRun` exposes only declared columns, alive/generation views,
  tick, scratch and parsed data.
- **Registry:** process-wide, append-only, lock-protected (static `pthread_mutex`/SRWLOCK), no unregister: an entry
  can never disappear under a live world. Listing (`su_host_operator_count/info`) shows built-ins first, then
  registered operators in registration order. The static library and the DLL are separate copies with separate
  registries (documented in `host.h`).
- **`SU_HOST_OPERATOR_TRUST_INPUTS` kept:** for an operator that honours the contract it changes cost only (the
  results are the same). For one that writes a read lane, it turns a failed tick into silent corruption. It is part of
  the identity, so worlds with and without it never exchange snapshots or replays (tested: `verify flags`).
- **Operator data:** the existing `su_host_set_operator_data` path; one parse per call, refcounted per world, released
  with the last instance. The family check now also requires the same registration (registered operators share one
  callback but not one parser).
- **Versioning:** `struct_size` may be anything from `SU_HOST_OPERATOR_DESC_V1_SIZE` (the size of this release's
  descriptor, through `user`) up. Registration copies at most `struct_size` bytes into a zeroed local descriptor, and
  bytes beyond the library's `sizeof` must be zero: an unknown nonzero field is refused, never ignored. The library
  fills `SuHostOperatorRun.struct_size`. `SuHostOperatorColumn` keeps its layout (no stride field); per-lane additions
  will be new trailing `SuHostOperatorRun` fields (parallel arrays indexed like `columns`).
- **Extension plan for issue 142 (deterministic multithreaded tick), not implemented.** It is also in `host.h`. Every
  part is opt-in by a new descriptor flag plus new trailing descriptor fields. Operators without the flag keep exactly
  the serial whole-range call.
  1. Chunked runs: `run` is called once per canonical row chunk (`row_begin/row_end`, `chunk_index/chunk_count`, which
     already exist and are 0..rows, 0 of 1 today). The chunking depends only on the row count, as for chunked IR
     systems. Each call stores only its rows' writes, and reads stay frozen for the tick.
  2. Per-worker scratch: a new trailing `scratch_size_ex(capacity, workers, chunk_rows, ...)` sizes one scratch per
     worker, and `run->scratch` is the calling worker's. The sizes join the identity as `scratch_size`'s does.
  3. Scatter writes to other rows (for example damage applied to target rows): each chunk writes into its own
     engine-allocated output buffers (declared lanes, allocated at creation). A declared merge/reduce callback then
     combines the buffers in canonical chunk order (0 .. chunk_count - 1) into staging, so the result does not depend
     on thread timing. The merge is part of the operator's identity.

## Local validation

Linux 7.2.9 (Fedora 44 based), Intel Core i7-14700K (20 CPUs available), GCC 16.2.1, Clang 22.1.8, CMake 4.3.0,
Ninja 1.13.2, GNU ld 2.46.1. Renderer and glTF importer were skipped by configure (no Vulkan headers / asset deps
bootstrapped); the same holds for the baseline. Builds used `-j 6`. `SUTEKH_WERROR` is ON by default
(`-Wall -Wextra -pedantic -Werror`).

```sh
cmake -S . -B /tmp/wt-137-build/debug   -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Debug
cmake -S . -B /tmp/wt-137-build/release -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Release
cmake -S . -B /tmp/wt-137-build/asan    -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug \
  "-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" \
  "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined" "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined"
python3 tools/bootstrap_asmjit.py   # pinned commit 0d7f010, lock unchanged
cmake -S . -B /tmp/wt-137-build/native  -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Debug -DSUTEKH_NATIVE=ON
cmake --build <dir> -j 6 && (cd <dir> && ctest -j 6 --output-on-failure)
```

| Configuration | Build | ctest |
|---|---|---|
| GCC Debug | 0 warnings | **135/135 passed**, 1 skipped (`audio_device`: no audio backend on Linux) |
| GCC Release | 0 warnings | **135/135 passed**, same skip |
| GCC Debug `SUTEKH_NATIVE=ON` | 0 warnings | **152/152 passed**, 2 skipped (`native_unwind` Windows only, `audio_device`) |
| Clang Debug ASan+UBSan | 0 warnings | 21 of 135 failed, 1 skipped: **the identical set fails on the baseline** (below) |

(Review round; the first revision had 133 / 133 / 150 / 133 tests. Two cross-process tests were added.)

The 10 new tests pass in every configuration, including ASan+UBSan: `host_registered` (559 checks, static),
`host_registered_dll` (555 checks; the allocation counter is static-only), `host_registered_record`, and
`host_registered_verify_{same,version,constants,name,flags,scratch,none}`. `flags` and `scratch` register the same
operator with `SU_HOST_OPERATOR_TRUST_INPUTS` or a 64-byte larger scratch: the snapshot and the replay of the
recording process are refused (`SU_E_SCHEMA_MISMATCH`). The in-process tests also cover names with quotes, control
characters and non-ASCII bytes in every name slot, reserved prefixes in upper and mixed case, reserved operator
names, the former bit 2, descriptors of the v1 size, larger with a zero tail, larger with a nonzero tail (refused) and
below the v1 size (refused), and a failing run's reason text.

ASan+UBSan baseline: `origin/main` 07d06de extracted with `git archive` to `/tmp/wt-137-base-src` and built with the
same flags: 125 tests, the same 21 failures and the same skip (failure sets compared with `diff`: identical). The
sanitizer reports in both logs are the same: 4 LeakSanitizer reports, one heap-use-after-free in `su_replay_open`
(`src/host/su_replay.c:696`, error path hit by `test_replay` `test_tamper`: `su_replay_close(r)` and then a read
of `in->start_tick`), and two AddressSanitizer allocation failures (15,392,894,357,504 bytes). These failures are
pre-existing, are not touched here and were not diagnosed one by one. The 21 tests: `containment`, `control`, `host_replay`, `host_rollback_dll`, `host_rollback`, `rts_b1_hidden`, `rts_b1_install`, `rts_b2_hidden`, `rts_b2_install`, `rts_b3_hidden`, `rts_b3_install`, `rts_caps_engine`, `rts_ctrl_engine`, `rts_e2e_edges`, `rts_load_engine`, `rts_model_digest`, `rts_projectile_mutant`, `rts_replay_engine`, `rts_world_set_fields`, `rts_world_spawn_batch`, `space3d_serve`.

**No-registration guarantee.** (1) The whole existing suite passes unchanged, including pinned digests
(`example_arena_host*` `--expect-digest`, `host_ops` racer digests from `bench/space3d/expected.json`, replay
record/verify across processes). (2) An explicit cross-build comparison (scratch script, not committed) ran the same
digest-producing commands on the baseline Release build and on this branch's Release build: `arena_host --ticks 900
--rollback-check` with and without `--respond` (final digests `3b1f7e1d...` and `fab7f110...`), the SHA-256 of the
canonical replay file written by `test_replay record` (`232e8f2a5ccf4613...`), every `racer_microcosm --n 8 --ticks 720
--digest-every 120` state digest, and the `minimal_host` digest lines. Output files identical; SHA-256 of both
`02c978ede2dc7baac17b44ba151d27f979962e27e810df63907a8395191ec702`. Rerun on the review-round sources: identical.

**Mutation check** (scratch script, not committed; each mutant built and run, then the source restored and
`cmp`-verified; rerun in the review round): 12 engine mutants, 12 killed.
- Removed input re-hash (5 failures).
- No zero prefill of write-only lanes (contract check).
- No scratch zeroing.
- Flags and scratch size not folded into the identity (killed by `verify flags` and `verify scratch`).
- Lane names not checked as identifiers.
- Reserved prefixes compared case-sensitively.
- A nonzero descriptor tail ignored.
- A `malloc` in the tick (50 allocations counted).
- Operator name not in the semantic hash (killed only by the cross-process `verify name`, as intended).
- Record name not checked.
- Duplicate ID not refused.
- `run->tick` off by one (440 cells differ from the model).

Repository checks: `python3 tools/validate_pack.py --check-report` passed; `python3 tools/check_repository_license.py`
passed; `git diff --check` clean.

## Input-check cost (observations, C10)

Not budgets. Same machine (i7-14700K), Release, single core.
- **Independent review:** with a world stepping, about 0.55-0.62 ms per tick at 100,000 rows x 10 read lanes, about
  34 us at 10,000 x 10. The method is the reviewer's and is not reproduced here.
- **Ours:** a scratch microbenchmark (`hashcost.c`, not committed, SHA-256
  `5de8318e90fc05a1092fd7e88826239d3abf8759c833310f6083442b82959f44`; `gcc -O2`, pinned with `taskset -c 3`). It times
  the two XXH64 passes alone over 10 lanes, 400 ticks after 40 warm-up ticks, 3 runs.
  - 10,000 rows: median 36.7 us, p95 37.2-38.8 us.
  - 100,000 rows: median 365.1-366.1 us, p95 367.4-373.4 us.
- **Reading:** the isolated hash accounts for about 0.37 ms of the reviewer's 0.55-0.62 ms at 100,000 rows. The rest
  is not attributed here (likely cache effects alongside the tick's other work), and no breakdown was measured.
  `SU_HOST_OPERATOR_TRUST_INPUTS` removes the cost and the protection.

## SHA-256 evidence (review round 1 sources; superseded where listed under round 2)

- `include/sutekh/host.h` `5cb7614162d34d12d709d36fe722efe97aa0f2e4b72e36f2e6b6d9e9717060db`
- `src/host/su_host_registered.c` `2fc144002da5a85f73eed4b9d8a67893efebd365091610be07b1ed7a20176f3d`
- `src/host/su_host.c` `94e6cea01a1a04154f7631bc47a69d4fbbbfc881c8d67b06b763895e1e9ca15a`
- `src/host/su_host_internal.h` `62f5c405da95f3f20acd37a359cea3b3966886ad94085299038b16f1c23c0828`
- `src/host/su_host_operators.c` `08b2d5b982bffcd178833a0c7dc6c969de79160b5efd830a8511408a282144b7`
- `src/runtime/rts/host_decl.c` `868cb8354ccb8174811c0e6c99116f84376bc849272fcaa56ac78a62e37fec04`
- `src/runtime/rts/host_decl.h` `d76db14df94c423c305ddf7a5d121a71571f3a9a454257f9d2c6afd7df07e3cb`
- `tests/host/test_host_registered.c` `22c8ae685660c200222bb06569efbd241c8b4c775380a76b12738e0e6e471685`
- `cmake/SutekhHostOps.cmake` `dec6ff1754ef21c93c33b4183957f3d901b070ea8feb58ce9ed2b722a11f0b76`
- Release `libsutekh_host.so` `50ba67d42eb25f73034c80ffb223c07cd0fa19771e7e0c4e5dc1f894ac60dd9a`
- Release `test_host_registered` `638bb4e0664fa2bd9aab7541ecdfcc480f5db34634035297540bb2f17483f47c`

## Review round 2 (final small commit)

Changes:
- `host.h`: callers must zero the whole descriptor before filling it, because the tail check is byte-based.
  `SU_HOST_OPERATOR_DESC_INIT(d)` does it (memset plus `struct_size`; `host.h` now includes `<string.h>`). Fields
  added later are laid out without padding.
- `SU_HOST_OPERATOR_DESC_V1_SIZE` is pinned by `_Static_assert`s in `su_host_registered.c`: 120 bytes with 8-byte
  pointers, 84 with 4-byte pointers, plus the `constants_hash` offset. The test checks the value at run time. ILP32 was
  not compiled here.
- The `err->message` truncation is documented: 255 bytes, shared with the tick, the system ID and the code name.
- The failure reason is cleared before every tick for every registered host entry. The world caches each entry's
  operator at creation (`SuHostWorld.ops`), so the tick takes no registry lock, and a reason can no longer outlive its
  tick attempt (for example after a failure before `run` is reached). No test reaches that internal failure path.
- Duplicate system IDs are compared ignoring letter case. Identifiers need at least one letter or digit (`.`, `-`, `..`,
  `_-_` refused). Reserved operator names fold `-` to `_` (`space3d-ops_v1`, `Rts-Ops-v7` refused).

Validation (same machine and flags as above):
- GCC Debug and Release: **135/135** each, with the `audio_device` skip.
- Native: **152/152**, with 2 platform skips.
- Clang ASan+UBSan, the 10 `host_registered*` tests only: 10/10, 0 diagnostics.
- `host_registered` 568 checks, `host_registered_dll` 564, 0 failures.
- The baseline digest comparison is identical again.
- Three new mutants (case-sensitive duplicates, no alphanumeric rule, `-` not folded) are all killed.
- Pack report, license check and `git diff --check` pass.
- The full ASan suite was not rerun in this round. The round 1 run, identical to main, stands.

Source SHA-256 (round 2):
- `include/sutekh/host.h` `8c6ad57ecc46f20d2367b7156b48ae5879b2c5cae03da06a20abf2f80127c149`
- `src/host/su_host.c` `eed4313ad682f5a0d24709eed2507a67a87768c130d9fcb22c8c158b0d53b70d`
- `src/host/su_host_internal.h` `1dd011d6bac6e16c324156d232aea3f8bf1264a0a3765f6681322323d25da3f8`
- `src/host/su_host_operators.c` `ae911ab52bbbffb755ec84adf00bde111611bd0db1e43b1ead8fade1bcb7fcbf`
- `src/host/su_host_registered.c` `15f8d512bf22348b466a547bcd189a09f5f98079030e94b7b753046d2924706e`
- `tests/host/test_host_registered.c` `7034d0b966c3a37c99471a17c5622ef8ef8e157fbea912ab6788ff8bc0ab824a`

## Unrun checks and limitations

- Not run: Windows (MSVC `/W4 /WX`, clang-cl), macOS, the renderer-on configuration, ThreadSanitizer (the
  concurrent-registration test ran without it), CI. No CI result is claimed.
- Timing: only the input-check observations above. The cost of the per-call scratch zeroing (always on) was not
  measured; it is a memset of `scratch_size(capacity)` bytes per tick.
- Detection limits: the input check is a 64-bit hash (a collision, about 2^-64, is missed) and covers writes through
  the views `run` receives; in-process native code can still write anywhere (C07). With `TRUST_INPUTS` an undeclared
  write is not detected (tested as such).
- The tick allocates nothing: proven by counting `malloc/calloc/realloc` (`-Wl,--wrap`) on the static Linux build
  only; the DLL and Windows builds are not counted.
- Not in scope: operator-owned variable-size state in snapshots (separate request), parallel execution (issue 142,
  plan above), unregistering, registered operators in a worker. `su_host_step` allocates when an input record applies
  (pre-existing, outside the operator path; filed separately by the coordinator). The allocation test steps without
  inputs.
- `SU_E_CAPABILITY_DENIED` text for an unknown host record changed ("built into or registered with this library");
  code, index and behaviour are unchanged.

Next: push the branch, open the PR closing #137, integrate after review and CI.
