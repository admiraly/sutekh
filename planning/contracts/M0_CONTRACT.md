# M0-00 contract, generation 1 (ABI v1, SU-LIR 0.1, artifact format 1)

Headers: `include/sutekh/{status,ir,abi}.h`. Consumers implement against these; any change needs a new generation and revalidation of dependents (owner: contract owner / integrator).

## 0. Revision log
- r3.1 (rulings from M0-02/M0-03): negative/fractional/>2^32-1 integers are `SU_E_TYPE` (not PARSE). Capsule field-level shape errors are stage 4; capsule `limits.max_instructions` overrun is `SU_E_RESOURCE_LIMIT` at stage 5. Entity IDs are u32; field id 0 is allowed; allocation failure maps to `SU_E_RESOURCE_LIMIT`; access 0 is rejected; field names are capped at 255 bytes.
- r3: closes r2-review blockers (error ordering stages, header cross-references, LIVE_IR section 5 note, `SU_LIMIT_ROWS` is a default).
- r1: initial draft (43dba6d). r2: resolves blocking findings 1-9 of `planning/handoffs/2026-10-11-m0-00-review.md`; adds `SU_E_MISSING_STORE` (20). Headers are the layout authority; this document is the semantic authority.

## 1. Numeric and effect contract
- Profile `u32_mod_v1`: add/sub/mul modulo 2^32, unsigned `lt`, exact `eq` (both operands u32, result bool), `select` on bool; bool never coerces to u32. Unsigned C arithmetic only.
- Registers: per-row, single assignment, numbered from 0 in definition order.
- Loads read system-entry values. **Every declared-writable field must be stored exactly once** (unchanged values are copied with load+store; "no needless copy" in the spec means no extra registers beyond that). A second store of one field -> `SU_E_UNDECLARED_WRITE`; a writable field never stored -> `SU_E_MISSING_STORE`; store/load of a field without the permission -> `SU_E_UNDECLARED_WRITE` / `SU_E_UNDECLARED_READ`.
- Verifier reports the **first** error only, by this fixed stage order (capsule and IR are both checked; capsule first within a stage):
  1. size/depth limits -> `SU_E_RESOURCE_LIMIT`
  2. JSON syntax / UTF-8 -> `SU_E_PARSE`
  3. duplicate object keys -> `SU_E_DUPLICATE_KEY`
  4. top-level document shape (missing/unknown top-level keys, wrong JSON types of top-level values, `ir_version`/profile unsupported) -> `SU_E_TYPE`
  5. capsule/schema semantics: duplicate field id or name, `system_id`/`query`/`numeric_profile` mismatch between IR and capsule -> `SU_E_SCHEMA_MISMATCH`
  6. instructions in ascending index; within one instruction in this order: `op` unknown -> `SU_E_UNKNOWN_OPCODE`; instruction object keys missing/unknown/wrong JSON type, constant not an integer in 0..2^32-1 -> `SU_E_TYPE`; undefined register -> `SU_E_UNDEFINED_REGISTER`; duplicate dst -> `SU_E_DUPLICATE_REGISTER`; operand register type mismatch (e.g. bool where u32 expected) -> `SU_E_TYPE`; load of unknown or unreadable field -> `SU_E_UNDECLARED_READ`; store to unknown, unwritable or already-stored field -> `SU_E_UNDECLARED_WRITE`
  7. end of body: a declared-writable field never stored -> `SU_E_MISSING_STORE`
  Multi-fault fixtures therefore expect the earliest stage/position. The IR file's instruction-count cap (4096) is stage 1.
- Execution: row loop owned by runtime, ascending order, zero rows run nothing, whole table per call.

## 2. Schema and field identity
M0 has no separate schema file. The **capsule `fields` array is the authoritative schema declaration** for table `schema.id` (field id, name, type, permission). IR `query` must equal `schema.id`, and IR/capsule `system_id` and `numeric_profile` must match (`SU_E_SCHEMA_MISMATCH`). Field IDs are unique within a capsule (duplicate id or name -> `SU_E_SCHEMA_MISMATCH`). A world binds fixture column names to field IDs through the first installed capsule; a later capsule is code-only only if every shared name has the same id/type and layout_generation (else `SU_E_SCHEMA_MISMATCH`).
The **verified field table** is the capsule's fields sorted ascending by field id; `SuInsn` field operands index this table. IR source names resolve to table indices once, at verification.

## 3. Lifetimes, wrapper checks, and commit
- Pointers in the structs are borrowed for one `SuInvokeV1Fn` call. Input `data` is read-only to the executor.
- `SuExecContext` is created/destroyed by the executor implementation (M0-04); NULL ctx -> `SU_E_INVALID_ARGUMENT`.
- Wrapper checks, all before any access: NULL pointer, `struct_size`, `abi_version`, `stride_bytes != 4`, `elem_type`, binding/staging counts -> `SU_E_INVALID_ARGUMENT`; `layout_generation` mismatch or a binding `row_count != row_count` or missing/extra field_id -> `SU_E_SCHEMA_MISMATCH`; any byte-range overlap between staging and any other column (alias) -> `SU_E_INVALID_ARGUMENT`; `row_count > SU_LIMIT_ROWS` (runtime-configurable default) -> `SU_E_RESOURCE_LIMIT`.
- Staging columns are preallocated at world preparation/admission. After `SU_OK` the runtime **copies** staging into the working root; on failure it discards staging.

## 4. Tick and publish (M0)
Copy committed columns into the working root; run systems serially, each system's committed staging visible to later systems; publish a new committed root only if the whole tick succeeds (T09). Code-only swaps install at a tick boundary after revalidating expected world revision (`SU_E_STALE_REVISION`) and layout generation (`SU_E_SCHEMA_MISMATCH`).

Error mapping beyond section 3: malformed JSON/UTF-8 in a protocol line -> `SU_E_PARSE`; duplicate keys in a protocol line -> `SU_E_DUPLICATE_KEY` (same as IR files; ruling r3.2); message over 1 MiB or collection limits -> `SU_E_RESOURCE_LIMIT`; path escape or denied path -> `SU_E_CAPABILITY_DENIED`; file errors -> `SU_E_IO`; worker death/timeout -> `SU_E_WORKER_FAILED`. Codes 18-20 are engine-wide additions to the RPC error list.

## 5. Identities and canonical hash
- `source_hash`: SHA-256 of the exact `.lir.json` bytes. `capsule_hash`: SHA-256 of exact capsule bytes. Both lowercase hex.
- `semantic_hash` = SHA-256 of the byte string below. All integers are u32 little-endian; a *string* is u32 byte length + UTF-8 bytes, no NUL; type codes are `SU_TYPE_*`; access is the `SU_ACCESS_*` bitmask.
  1. ASCII bytes `SULIR-SEM` + one 0x00 byte
  2. artifact format (`SU_ARTIFACT_FORMAT_VERSION`)
  3. strings: numeric_profile, system_id, query(=schema.id)
  4. field count, then per field of the verified table (ascending id): id, type, access. Names, layout_generation and file paths are **excluded** (rename or relayout is not a behavior change; layout invalidation is tracked separately).
  5. instruction count, then per instruction the five `SuInsn` words, registers numbered from 0, field operands replaced by field **id**, unused slots 0xFFFFFFFF.
  - M0 has no referenced external definitions; LIVE_IR section 9 "referenced hashes" is empty.
- Whitespace and key order never change `semantic_hash`. Any change here bumps the artifact format version.

## 6. Snapshot (M0, JSON)
`snapshot_format` = `"0.1"`; `world_revision` (string); `tick` (decimal u64 as JSON integer); `schema_id`; `schema_version`; `layout_generation`; `numeric_profile`; `fields` [{id,name,type}] ascending id; `semantic_manifest` [{system_id, semantic_hash}] in schedule order; `entity_ids`; `columns` keyed by field **name**, values JSON integers 0..2^32-1. Restore is `world.restore` with expected current revision; it sets tick, columns and manifest from the snapshot and assigns a **new** world revision, recording `restored_from` = snapshot id (T08 compares everything except the revision string).

## 7. Control plane
JSON-lines v0.1 per `docs/AGENT_RPC.md`, stdio only, 1 MiB messages. Executable roles `sutekh serve|worker`. Unimplemented methods return `SU_E_UNSUPPORTED_METHOD`.

## 8. Ownership
Contract owner: M0-00 (integrator). M0-01 build skeleton, M0-02 parser/verifier, M0-03 world storage, M0-04 VM, M0-05/06 worker+control: consume only these headers.

## 9. Checks run
r2: gcc 13.3 and clang 18.1 `-std=c17 -Wall -Wextra -Werror -pedantic -Iinclude planning/contracts/abi_check.c` compile and run OK (statuses=21). Independent review of r1 done (accept-with-changes); r2 changes are not yet independently re-reviewed, so M0-00 stays `submitted`. Non-blocking review items still open: fixtures with several faults, no fixtures for several error codes.
