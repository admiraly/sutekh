# Live IR specification
## SU-LIR 0.1 — narrow, executable M0 contract

The initial source format is strict UTF-8 JSON with a `.lir.json` suffix. A human-oriented `.live` language may later lower to the same semantic representation. It is not part of M0. Agents can author JSON immediately, without waiting for a new language toolchain.

## 1. Scope

SU-LIR 0.1 describes a straight-line scalar computation executed once for each row in a fixed table. It supports **u32 fields**, single-assignment virtual registers, constants, field loads/stores, modular arithmetic, comparisons, and selection. It has no user-defined loops, recursion, dynamic allocation, cross-entity addressing, native calls, floating point, strings, file access, or networking.

The row loop is owned by the runtime. A zero-row table executes no body instructions. Iteration order is ascending logical row index. Every register is per-row and freshly defined each invocation; no value survives between rows or ticks except an explicit field store.

This intentionally tiny language proves hot replacement. Navigation, full combat logic, and BFME semantics require later, versioned extensions; agents must not fake those features with undeclared host behavior.

## 2. Module structure

```json
{
  "ir_version": "0.1",
  "system_id": "demo.movement",
  "numeric_profile": "u32_mod_v1",
  "query": "demo.actor",
  "instructions": [
    {"op": "load_u32", "dst": "p", "field": "position_x"},
    {"op": "load_u32", "dst": "v", "field": "velocity_x"},
    {"op": "add_u32", "dst": "next", "a": "p", "b": "v"},
    {"op": "store_u32", "field": "position_x", "src": "next"}
  ]
}
```

The capsule contract is separate and binds `position_x` and `velocity_x` to stable schema field IDs/types. The IR and contract must name the same system/query/numeric profile. The schema file under `schemas/` validates syntax; **semantic verification remains required**.

## 3. Types and arithmetic

`u32` is the mathematical set 0 through 4,294,967,295. `bool` is exactly false or true and cannot be implicitly coerced to u32. JSON integer constants must be within the u32 range. Negative constants and fractional JSON numbers are rejected. The loader must not parse these integers through an imprecise floating representation.

`add_u32(a,b)` and `mul_u32(a,b)` return the result modulo 2^32. `sub_u32(a,b)` is also modulo 2^32, not saturating. All three are total functions. Later saturating arithmetic must have distinct opcode names. Native/C implementations must use well-defined unsigned behavior; they may not rely on signed overflow.

`lt_u32` is an unsigned comparison. `eq_u32` is exact equality. `select_u32` chooses an already-computed u32 register based on a bool register. There is no short-circuit or unexecuted branch in this version. This makes boundedness obvious without a general control-flow verifier.

## 4. Opcode table

| Opcode | Arguments | Result/effect |
|---|---|---|
| `const_u32` | `dst`, `value` | Define a u32 register |
| `load_u32` | `dst`, `field` | Read current row's named u32 field |
| `add_u32` | `dst`, `a`, `b` | u32 modular sum |
| `sub_u32` | `dst`, `a`, `b` | u32 modular difference |
| `mul_u32` | `dst`, `a`, `b` | u32 modular product |
| `eq_u32` | `dst`, `a`, `b` | Define bool equality result |
| `lt_u32` | `dst`, `a`, `b` | Define bool unsigned comparison |
| `select_u32` | `dst`, `cond`, `on_true`, `on_false` | Define selected u32 |
| `store_u32` | `field`, `src` | Stage u32 write to current row |

Unknown opcodes, unknown keys, duplicate object keys, undefined registers, duplicate register definitions, type mismatches, and undeclared field effects are errors. Reject ambiguity rather than guessing an author's intent.

## 5. Read/write semantics

Each system invocation sees the state committed by all prior systems in the schedule. Within that invocation, **loads read the invocation-entry value**, not a previous staged store. At most one store per field per row is allowed by 0.1. This avoids hidden within-row ordering dependencies and makes reference/vector behavior easier to compare.

The verifier requires every declared writable field to be stored exactly once in the instruction body. Read permissions permit zero or more loads; write permission does not imply read permission. Unchanged fields are not copied through registers unnecessarily. Commit staged writes only after the system completes successfully. A detected execution failure must not leave partially committed system output. Later native fault recovery follows the worker checkpoint protocol rather than assuming a C return code always exists.

The M0 implementation may allocate staging columns during world preparation or capsule admission, never per entity or unexpectedly mid-tick. Resource exhaustion rejects admission or aborts a candidate tick before publication.

## 6. Contract and binding

The included contract format contains an array of fields with stable numeric IDs, string names, types, and read/write permissions. Declaration order is not field identity. The prepared execution plan resolves these names once into validated bindings and records a layout generation.

Bindings are only valid for the exact query/table schema and layout generation used during verification. Schema mismatch returns `SU_E_SCHEMA_MISMATCH`; it does not look up a similarly named field or fall back to a raw offset.

M0 contains one table with fixed rows and fields. A later schema change is a transaction that regenerates bindings and invalidates native variants referencing old layouts.

## 7. Resource limits

Initial, configurable defensive limits: 256 KiB IR source, JSON nesting depth 32, 4,096 instructions, 4,096 virtual registers, 256 bound fields, and 1,000,000 rows for the supplied stress harness. These are acceptance/configuration values, not fundamental engine limits. Caps must be checked before proportional allocation and before multiplication of sizes. Oversized input yields a structured resource error.

Execution work is bounded by row count × instruction count. The supervisor also applies a wall-time watchdog to candidate workers. A wall-clock timeout is a tool failure and must never alter authoritative deterministic simulation state or select a gameplay outcome.

## 8. Diagnostics

Errors include `code`, source path, JSON pointer, optional byte span, system ID, relevant field/register, expected/actual values, and one concrete correction hint. Example:

```json
{
  "code": "SU_E_UNDECLARED_WRITE",
  "path": "capsules/movement/system.lir.json",
  "json_pointer": "/instructions/3/field",
  "system_id": "demo.movement",
  "message": "position_x is stored but not declared writable"
}
```

The exact failing source content hash belongs in the surrounding response/evidence record so stale diagnostics cannot attach to a newer edit.

## 9. Canonical semantics and hashes

Original source bytes are retained for diagnostics and source hashing. A semantic hash is computed from a normalized representation: fixed field order for module metadata, instruction order preserved, stable field IDs, canonical decimal integer encodings, opcode/type names, validated effects, numeric profile, and referenced definition hashes. Register names are normalized by definition order. Whitespace and JSON key order alone must not alter the semantic hash.

The hashing algorithm and canonical byte encoding must be pinned before first persisted artifacts. M0 uses SHA-256 for source/artifact identities through a reviewed implementation; a change to normalization or hash format increments the artifact-format version. Hash equality is an indexing mechanism, not a mathematical proof of program equivalence.

## 10. Lowering contract

The native backend must produce the reference result for every input within the supported profile and resource bounds. Arithmetic may be vectorized because 0.1 has no cross-row effects, provided row tails and unaligned data are handled correctly. Zero rows, counts not divisible by SIMD width, maximum u32 values, alias rejection, and generation mismatch are mandatory tests.

No FMA or floating-point semantic question exists in 0.1. Later floating-point support must explicitly define operation rounding, contraction, subnormals, NaNs, signed zero, conversions, and reduction order. A label such as `strict` by itself is insufficient [S06].

## 11. Versioned extension sequence

After M0, add only operations required by an accepted task: bounded conditional blocks, i32 arithmetic with explicit overflow behavior, typed entity handles, bounded event emission, tick/context inputs, deterministic PRNG state, relationship reads, and separately specified floating-point modes. Every addition requires verifier rules, reference implementation, invalid-input tests, cross-backend differential tests, and resource bounds.

Control-flow graphs, a broad optimizing SSA compiler, automatic layout exploration, and CPU/GPU multi-target synthesis are later capabilities. They are not prerequisites for the first executable capsule.

Fixtures: [movement IR](examples/movement.lir.json), [contract](examples/movement.capsule.json), [world](examples/world.json), [test](examples/movement.test.json). Sources: [SOURCE_NOTES.md](docs/SOURCE_NOTES.md).
