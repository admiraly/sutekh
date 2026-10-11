# M0-00 contract, generation 1 (ABI v1, SU-LIR 0.1, artifact format 1)

Headers: `include/sutekh/{status,ir,abi}.h`. Consumers implement against these; any change needs a new generation and revalidation of dependents (owner: contract owner / integrator).

## 1. Numeric and effect contract
- Profile `u32_mod_v1`: add/sub/mul modulo 2^32, unsigned `lt`, exact `eq`, `select` on bool; bool never coerces to u32. Use unsigned C arithmetic only.
- Registers are per-row, single assignment, numbered by definition order. Loads read system-entry values; each writable field is stored exactly once; reads and writes must match the capsule declaration (`SU_E_UNDECLARED_READ/WRITE`).
- Execution: row loop owned by runtime, ascending order, zero rows run nothing, whole table per call.

## 2. Lifetimes
- Bindings/invocation/output pointers are borrowed for one `SuInvokeV1Fn` call only. Wrapper checks sizes, ABI version, generations, lengths, no staging/input aliasing, rows <= `SU_LIMIT_ROWS`.
- Staging columns are allocated at world preparation/admission, never per row or mid-tick. Runtime commits staging only after `SU_OK`; any failure discards it.

## 3. Tick and publish (M0)
Copy committed columns into the working root; run systems serially, each system's committed staging visible to later systems; publish a new committed root only if the full tick succeeds (T09). Code-only swaps install at a tick boundary after revalidating expected world revision and layout generation (`SU_E_STALE_REVISION` / `SU_E_SCHEMA_MISMATCH`).

## 4. Errors
Codes in `status.h` are the spec set (1..17) plus `SU_E_INVALID_ARGUMENT` (18) and `SU_E_INTERNAL` (19). Mapping: bad JSON/UTF-8/oversize message framing -> `SU_E_PARSE`; limits -> `SU_E_RESOURCE_LIMIT`; path escape -> `SU_E_CAPABILITY_DENIED`; file errors -> `SU_E_IO`; worker death/timeout -> `SU_E_WORKER_FAILED`. Diagnostics carry the fields in LIVE_IR_SPEC section 8. Negative fixtures: `examples/negative_cases.json`.

## 5. Identities
- `source_hash`: SHA-256 of original bytes. `semantic_hash`: SHA-256 over the canonical encoding below. Both lowercase hex in JSON.
- Canonical semantic encoding (all integers little-endian u32): ASCII `SULIR-SEM\0`, artifact format, numeric profile length+bytes, system_id length+bytes, query length+bytes, layout_generation, field table count then per field (id, type, access bits) sorted by field id, instruction count, then per instruction the 5 `SuInsn` words with registers in definition order and field operands replaced by field id.
- Whitespace/key order must not change the hash. Changing this encoding bumps `SU_ARTIFACT_FORMAT_VERSION`.

## 6. Snapshot (M0, JSON)
Fields: `snapshot_format`, `world_revision`, `tick`, `schema_id`, `schema_version`, `layout_generation`, `numeric_profile`, `semantic_manifest` (system_id + semantic_hash per installed system, schedule order), `entity_ids`, `columns` (names resolved to ids; values as decimal u32). Restore must reproduce all of it exactly (T08).

## 7. Control plane
JSON-lines v0.1 per `docs/AGENT_RPC.md`, stdio only, 1 MiB messages. Executable roles `sutekh serve|worker`. Unimplemented methods return `SU_E_UNSUPPORTED_METHOD`.

## 8. Ownership
Contract owner: M0-00 (integrator). M0-01 build skeleton, M0-02 parser/verifier, M0-03 world storage, M0-04 VM, M0-05/06 worker+control: consume only these headers.

## 9. Checks run
`gcc` and `clang` `-std=c17 -Wall -Wextra -Werror -pedantic -Iinclude planning/contracts/abi_check.c` compile and run OK (static layout asserts included). Independent fixture review (acceptance item 2): not yet done.
