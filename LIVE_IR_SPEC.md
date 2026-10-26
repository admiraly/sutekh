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

The verifier requires every declared writable field to be stored exactly once in the instruction body. Read permissions permit zero or more loads; write permission does not imply read permission. Unchanged fields are not copied through registers unnecessarily (M0 contract r3: a declared-writable field that keeps its value is still stored exactly once, via load+store). Commit staged writes only after the system completes successfully. A detected execution failure must not leave partially committed system output. Later native fault recovery follows the worker checkpoint protocol rather than assuming a C return code always exists.

The M0 implementation may allocate staging columns during world preparation or capsule admission, never per entity or unexpectedly mid-tick. Resource exhaustion rejects admission or aborts a candidate tick before publication.

## 6. Contract and binding

The included contract format contains an array of fields with stable numeric IDs, string names, types, and read/write permissions. Declaration order is not field identity. The prepared execution plan resolves these names once into validated bindings and records a layout generation.

Bindings are only valid for the exact query/table schema and layout generation used during verification. Schema mismatch returns `SU_E_SCHEMA_MISMATCH`; it does not look up a similarly named field or fall back to a raw offset.

M0 contains one table with fixed rows and fields. A later schema change is a transaction that regenerates bindings and invalidates native variants referencing old layouts.

## 7. Resource limits

Initial, configurable defensive limits: 256 KiB IR source, JSON nesting depth 32, 4,096 instructions, 4,096 virtual registers, 1,024 bound field lanes (256 before 2026-10-19; see "IR 0.4"), and 1,000,000 rows for the supplied stress harness. These are acceptance/configuration values, not fundamental engine limits. Caps must be checked before proportional allocation and before multiplication of sizes. Oversized input yields a structured resource error.

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

## IR 0.2 (M2)

Status: implemented in the reference verifier and VM (M2-00c2). Contract: `planning/contracts/M2_ENTITIES_CONTRACT.md` sections 2, 3 and 6. The 0.1 rules above are unchanged; a 0.1 module keeps its semantic hash byte-for-byte.

**Versions.** `ir_version` is `"0.1"` or `"0.2"`; any other value is `SU_E_TYPE` (stage 4). The capsule may declare an optional top-level `ir_version`; absent means `"0.1"`. The IR and capsule versions must agree, else `SU_E_SCHEMA_MISMATCH` (stage 5).

**Operations** (JSON keys; `SuInsn` mapping from `include/sutekh/ir.h`):

| op | JSON keys | dst | a | b | c |
|---|---|---|---|---|---|
| `slot_u32` (10) | `op`, `dst` | u32 reg (row slot) | NONE | NONE | NONE |
| `gen_u32` (11) | `op`, `dst` | u32 reg (row generation) | NONE | NONE | NONE |
| `tick_u32` (12) | `op`, `dst` | u32 reg ((committed+1) mod 2^32) | NONE | NONE | NONE |
| `despawn_self` (13) | `op`, `cond` | cond bool reg (source) | NONE | NONE | NONE |
| `emit_event` (14) | `op`, `cond`, `kind`, `a`, `b` | cond bool reg (source) | kind constant | payload reg A (u32) | payload reg B (u32) |

Only `"0.2"` modules may contain these; in a 0.1 module they are `SU_E_UNKNOWN_OPCODE` (stage 6). `emit_event` `kind` must be below `SU_EVENT_KIND_RESERVED_MIN` (`0xFFFFFF00`), else `SU_E_TYPE` (stage 6).

**Capsule limits** (optional, integers 0..64, default 0): `limits.max_struct_per_row`, `limits.max_events_per_row`. A 0.1 module that declares either is `SU_E_TYPE` (stage 4). In stage 5 (after `max_instructions`), a 0.2 module whose count of `despawn_self` exceeds `max_struct_per_row`, or whose count of `emit_event` exceeds `max_events_per_row`, is `SU_E_RESOURCE_LIMIT`. Undeclared caps are 0, so any despawn or emit in such a module is refused. Conditions are ignored in the count (straight-line worst case).

**Hash.** The 0.1 encoding is unchanged. For 0.2 the artifact format is 2 (`SU_ARTIFACT_FORMAT_VERSION_0_2`), passed as a parameter of the encoding. After the field table, the encoding adds the `ir_version` string (u32 length + bytes, `"0.2"`), `max_struct_per_row` (u32) and `max_events_per_row` (u32), then the instruction count and words as before.

**VM semantics.** `su_vm_context_set_entity_view` sets, for subsequent invocations: `alive` (NULL = all alive; a dead row is skipped entirely, with no staged writes, no emissions and no store check, in 0.1 modules too), `generations` (NULL = 1), `base_slot` (`slot_u32` = base + row index), `tick_low` (value of `tick_u32`) and the emitter. Emission callbacks receive a per-row `seq` (0-based, shared by `despawn_self` and `emit_event`, advanced only when the op fires). A callback returning nonzero fails the call with `SU_E_RESOURCE_LIMIT` and sets `failed_insn`; records already emitted are not retracted. A module containing `despawn_self` or `emit_event` requires an emitter (`SU_E_INVALID_ARGUMENT` otherwise). Without a view, slot = row index, generation = 1, tick = 0.

**Native.** Any 0.2 module is refused by `su_native_compile` with `SU_E_CAPABILITY_DENIED` (reason `ir_0_2_not_lowered`). 0.1 modules are unaffected.

**Not in 0.2:** spawn from IR, entity-typed registers, event consumption by systems, native lowering of 0.2 ops, dynamic growth. The world integration that builds the entity view for `su_vm_run_system` is not part of this section.

## IR 0.3 (numeric profile `fixed_q32_v1`)

Status: implemented in the reference verifier and VM (2026-10-18; owner-authorized amendment `planning/amendments/2026-10-18-numeric-3d.md`). Numeric semantics and the decision record: [docs/NUMERIC_FIXED_Q32.md](docs/NUMERIC_FIXED_Q32.md). The 0.1 and 0.2 rules are unchanged; their modules keep their semantic hashes byte for byte (checked over all 83 committed capsule/IR pairs against the pre-0.3 verifier).

**Versions and profile.** `ir_version` `"0.3"` in both documents. A 0.3 module must declare `numeric_profile` `"fixed_q32_v1"`; a 0.1 or 0.2 module must declare `"u32_mod_v1"`. Any other pairing is `SU_E_TYPE` at `/numeric_profile` (stage 4); documents that are valid alone but disagree are `SU_E_SCHEMA_MISMATCH` (stage 5). Every 0.1 and 0.2 operation, including the entity operations and their capability and per-row cap rules, is valid in 0.3.

**Fields and storage lanes.** A 0.3 capsule field has `type` `"u32"`, `"i32"`, `"i64"` or `"q32"` (`SU_E_TYPE` at `/fields/<i>` otherwise; 0.1/0.2 capsules still accept only `"u32"`). World storage stays u32 SoA. `u32` and `i32` occupy one lane with the field's ID and name. An `i64` or `q32` field with ID `I` and name `N` occupies two lanes: ID `I`, name `N.lo` (bits 0..31) and ID `I+1`, name `N.hi` (bits 32..63), with the field's access on both. Stage 5 rejects with `SU_E_SCHEMA_MISMATCH` a 64-bit field with ID `0xFFFFFFFF` or whose `I+1` is the ID of another declared field, and with `SU_E_RESOURCE_LIMIT` a lane count above 256. The verified table (`SuLirModule.fields`) lists lanes ascending by ID with `type` `SU_TYPE_U32` (storage), `value_type` and `lane`. IR operands always name the declared field (`N`), never a lane name. The schedule's union table carries `value_type`, and two systems that bind one lane with different value types are `SU_E_SCHEMA_MISMATCH`, like a type or name conflict.

**Register types.** `u32`, `bool`, `i32`, `i64`, `q32`, `v3` (three q32) and `quat` (four q32, order w, x, y, z). No implicit conversion exists between any two types.

**Operations.** The JSON name, frozen code (`include/sutekh/ir.h`), defined type and operands of every 0.3 operation are the table in `src/ir/su_ops03.c`, which the verifier, the VM shape check and the hash all read. Summary (T is `i32`, `i64` or `q32`; registers unless marked):

| Group | Operations (JSON keys) | Result |
|---|---|---|
| constants | `const_i32` (`value`), `const_i64` (`value`), `const_q32` (`num` i64, `den` 1..2^64-1) | i32, i64, q32 = RNE(num * 2^32 / den); out of range is `SU_E_TYPE` |
| fields | `load_T`/`store_T` (`field`, `src`); `load_v3`/`store_v3` (`x`, `y`, `z` q32 fields, `src`) | the field's value type must equal T (`SU_E_TYPE`); `load_u32`/`store_u32` need a `u32` field |
| bool | `and_bool`, `or_bool` (`a`, `b`), `not_bool` (`a`) | bool |
| arithmetic | `add_T sub_T mul_T div_T min_T max_T` (`a`, `b`); `neg_T abs_T` (`a`); `clamp_T` (`x`, `lo`, `hi`) | T |
| compare/select | `eq_T lt_T le_T` (`a`, `b`); `select_T` (`cond`, `on_true`, `on_false`) | bool; T |
| shift | `shl_T shr_T` (`a`, `n` u32) | T |
| root | `sqrt_q32`, `rsqrt_q32` (`a`) | q32 |
| conversion | `i32_from_u32 u32_from_i32 i64_from_i32 i32_from_i64 q32_from_i32 q32_from_i64 q32_to_i32_floor q32_to_i32_round q32_from_raw q32_raw q32_to_i64_floor` (`a`) | as named |
| vec3 | `make_v3` (`x`,`y`,`z`), `v3_get` (`v`, `axis` 0..2), `v3_add v3_sub v3_dot v3_cross` (`a`,`b`), `v3_scale` (`v`,`s`), `v3_neg v3_len v3_len_sq v3_normalize` (`a`), `v3_clamp_len` (`v`,`max`), `select_v3` | v3 or q32 |
| quaternion | `make_quat` (`w` q32, `v` v3), `quat_get` (`q`, `axis` 0..3), `quat_mul` (`a`,`b`), `quat_conj quat_normalize` (`a`), `quat_rotate` (`q`,`v`), `quat_integrate` (`q`, `w` v3, `dt` q32), `select_quat` | quat, q32 or v3 |

Integer families wrap; q32 saturates and rounds to nearest even; division by zero, shifts by large counts and every conversion are defined in NUMERIC_FIXED_Q32.md section 4. No trigonometric operation exists in 0.3.

**Verifier rules.** Unchanged stage order. Within one 0.3 instruction: unknown or missing keys and JSON types (`SU_E_TYPE`), constant ranges (`axis`, the q32 value of `num/den`; `SU_E_TYPE`), undefined registers, duplicate `dst`, operand register types (`SU_E_TYPE`), then each field operand in key order: declared (`SU_E_UNDECLARED_READ`/`_WRITE`), permission, one store per field per row (a 64-bit store stores both lanes; `store_v3` with a repeated field is a second store), value type (`SU_E_TYPE`). Stage 7 reports a writable field with no store by its declared name. 0.3 names are `SU_E_UNKNOWN_OPCODE` in 0.1 and 0.2 modules.

**Encoding.** `SuInsn` operand slots come from the table. Field operands hold the lane-0 table index. `const_i64` and `const_q32` hold the low word in `a` and the high word in `b`; `const_q32` stores the rounded raw value, so `1/2` and `2/4` encode (and hash) identically. `store_v3` holds the source register in `dst` and the three field indices in `a`, `b`, `c`.

**Hash.** Artifact format 3 (`SU_ARTIFACT_FORMAT_VERSION_0_3`). As format 2, plus `value_type` and `lane` (u32 each) after `access` for every lane of the field table; the version string is `"0.3"`; every field operand of a 0.3 instruction is replaced by the lane-0 field ID, as for `load_u32`/`store_u32`.

**VM.** Row, entry-state and staging semantics are unchanged. 0.3 registers live in a second register file (four 64-bit words per register, allocated at context creation for 0.3 modules only). Every read checks the register tag; a hand-built module whose field operands do not name a lane-0 entry with the required value type, access and adjacent lane-1 entry is refused at context creation (`SU_E_INVALID_ARGUMENT`).

**VM tiers (2026-10-18).** The VM has two execution tiers with identical observable behaviour (staged lanes, emissions in order, status, `failed_insn`): the reference interpreter described above, and a *planned* tier (default) built per context when one static pass proves the reference's per-row checks redundant for the straight-line body (every register read follows a write of its type earlier in the row; every writable lane is stored exactly once). The planned tier pre-decodes instructions, evaluates row-invariant instructions (constants and pure operations on them whose register is defined once) when the plan is built, uses the exact inline arithmetic of `src/ir/su_num_fast.h`, and skips rows that a symbolic pass proves store their own loaded values and emit nothing (an identity-row guard over boolean requirements on `select` conditions, `make_v3`/`v3_get`, `make_quat`/`quat_get`, `and`/`or`/`not`); those rows' writable lanes are copied from the inputs. Modules without a plan run on the reference tier. Selection: `su_vm_set_default_tier`, `su_vm_context_set_tier`, environment `SUTEKH_VM_TIER=reference`; inspection: `su_vm_context_tier_info` (`src/vm/su_vm.h`). Evidence: `vm_tiers`, `ir03_diff` (every program on both tiers, guarded programs included), `num_fast`/`num_fast_portable`, and the full test suite run with each tier.

**Native.** (2026-10-18) `su_native_compile` lowers a 0.3 module without entity operations with the scalar capability set (`backends/native/su_native_lower03.cpp`): constants, loads, stores, bool and u32 operations, i32/i64 arithmetic, compares, selects, saturating q32 add/sub/neg/abs, the q32 multiply and the v3/quat moves are emitted inline; division, shifts, square roots and the vector and quaternion operations call C helpers (`su_native_ops03.c`) built on the exact functions of `src/ir/su_num_fast.h`. When the VM plan builder proves an identity-row guard, the kernel uses it too. An AVX2 request for a 0.3 module is refused with `SU_E_CAPABILITY_DENIED`, reason `ir_0_3_avx2_not_lowered` (`world.specialize` with backend `auto` selects scalar); a 0.3 module with entity operations is refused like 0.2 (`ir_0_2_not_lowered`) and stays on the VM tier. Evidence: `ir03_native_diff` (600 random programs, kernel lanes against the VM) and the racer digests with `racer_microcosm --native`.

**Resource bounds.** The 0.1 limits apply (4,096 instructions and registers, 256 lanes; 1,024 since 2026-10-19). The 0.3 register file adds at most 4,096 x 32 bytes per context. Execution remains bounded by rows x instructions; the most expensive operations (`v3_len`, `v3_normalize`, `quat_normalize`, `quat_integrate`) run a fixed-size integer square root (at most 64 bit-pair steps) and 128/64 divisions.

**Evidence.** `test_num` (numeric library), `num_model` (independent Python model, 2,460,000 records), `ir03` (verifier rejections with exact codes and pointers, lane layout, hash invariants and a pinned golden, VM known answers, VM defensive checks), `ir03_diff` (300 seeded random programs covering all 100 operations of a 0.3 module, real verifier and VM against an independent interpreter, corpus SHA-256 printed).

**Not in 0.3:** trigonometry, control flow, cross-row reads, floating point, native lowering of 0.3 operations, typed fields in host capsule records (see `docs/SPACE3D.md` for the host operator path), and migrations that add, rename or transform typed lanes (untested; a migration sees lanes as u32 fields).

## IR 0.4 (bit operations, row filter, cross-row reads)

Status: implemented in the verifier, both VM tiers and (except cross-row reads and `handle_live`) the native scalar lowering (2026-10-19; `handle_live` 2026-10-26). Every 0.1, 0.2 and 0.3 rule is unchanged; modules of those versions keep their semantic hashes byte for byte, and an operation or key added here is refused in them (`SU_E_UNKNOWN_OPCODE`, or `SU_E_TYPE` at `/row_filter`).

**Version and profile.** `ir_version` `"0.4"` in both documents, numeric profile `fixed_q32_v1`, field types as in 0.3. Every 0.3 operation is valid in 0.4.

**Operations.** Frozen codes in `include/sutekh/ir.h`; operand table rows in `src/ir/su_ops03.c` (`su_op_min_minor` is 4 for them).

| op (code) | JSON keys | result |
|---|---|---|
| `and_u32` (48), `or_u32` (49), `xor_u32` (50) | `a`, `b` (u32) | u32 bitwise |
| `not_u32` (51) | `a` | u32 complement |
| `shl_u32` (52), `shr_u32` (53) | `a`, `n` (u32) | `n >= 32`: 0. Else `a << n` mod 2^32, logical `a >> n` |
| `div_u32` (54), `rem_u32` (55) | `a`, `b` | `b = 0`: div 0, rem `a`. Else truncating quotient and remainder (so `div * b + rem = a` always) |
| `and_i64` (56), `or_i64` (57), `xor_i64` (58) | `a`, `b` (i64) | i64 bitwise on the two's-complement bits |
| `not_i64` (59) | `a` | i64 complement |
| `gather_u32` (240), `gather_i32` (241), `gather_i64` (242), `gather_q32` (243) | `field`, `slot` (u32) | the field's value in the row at `slot` in the system's entry state; 0 when `slot` is not a row of the table |
| `find_eq_u32` (245) | `field` (u32 field), `key` (u32), `span` (constant 1..4096) | the lowest slot `s < min(span, rows)` whose `field` equals `key` in the entry state, else `0xFFFFFFFF` |
| `handle_live` (246) | `slot` (u32), `gen` (u32) | bool: `slot` is a slot of the entity table, alive, and its generation equals `gen`, in the system's entry state (2026-10-26, see "Entity handles" below) |

A v3 of another row is three `gather_q32`. The field of a gather or `find_eq_u32` must be declared readable (`SU_E_UNDECLARED_READ` otherwise) and have the operation's type (`SU_E_TYPE`); it counts as a read of that field for effects and scheduling. `span` outside 1..4096 is `SU_E_TYPE` at `/instructions/<i>/span`.

**Entity handles (`handle_live`, 2026-10-26, issue 162).** A stored entity handle is the pair (slot, generation) of `SU_ENTITY_MAKE` (`include/sutekh/entities.h`), kept in two u32 fields (for example from `slot_u32`/`gen_u32` or the host's handle). `handle_live(slot, gen)` is true exactly when all of: `gen != 0`; `slot` is below the entity table's slot count (the world capacity; out-of-range slots, `0xFFFFFFFF` included, are false); the slot is alive; its stored generation equals `gen`. So a handle to a despawned entity is false, and after the slot is reused (the generation grows on despawn) the old handle stays false while the new occupant's handle is true; retired slots (generation `0xFFFFFFFF`) are never alive. The table read is the system's entry state, which for the entity table is the tick-start state: structural changes (despawns, IR or host) apply only at the tick boundary, so every system and chunk of a tick sees the same answers, independent of chunking, workers and tier. Verifier rules: both operands are u32 registers defined earlier (`SU_E_TYPE` at the operand's pointer otherwise, `SU_E_UNDEFINED_REGISTER` for an undefined one), the result is a bool register, and the op is `SU_E_UNKNOWN_OPCODE` below 0.4. It reads no field, so it adds no field read to `su_effects_derive` and needs no capability or per-row cap; no system can write the entity table during a tick, so it adds no scheduling hazard. It is an entity operation: `uses_entity_ops` is 1, so a schedule containing it runs through the entity runtime, which supplies the live table; `SuLirModule.uses_handle_live` records it. Encoding: `dst`, `a` = slot register, `b` = generation register; operands hash as registers like every other op (no new artifact format). VM: `su_vm_context_set_entity_table(ctx, alive, generations, slot_count)` (`src/vm/su_vm.h`) gives the context the whole table (alive NULL = all alive, generations NULL = 1, the entity-view defaults); without it the call's entity view over the call's rows is the table, which requires `base_slot` 0 (`SU_E_INVALID_ARGUMENT` otherwise, so a chunk never answers from its own slots). The entity runtime fills one whole-table copy per system per tick and sets it on every chunk; `su_jobs_run_system_chunked` (no entity view) sets an all-alive generation-1 table over the whole row range. Both tiers compute it with `su_handle_live` (`src/ir/su_ops03.h`); the planned tier never hoists it (it may appear in an identity-row guard's slice, which evaluates it per row). Snapshots need nothing new: the entity table is already world state, and nothing is cached between invocations.

**Row filter.** An optional top-level IR key `"row_filter": {"field": F, "equals": V, "mask": M}` (`mask` optional, default `0xFFFFFFFF`). A live row runs the body only when `(F & M) == V`, with `F` read from the entry state. Any other live row executes no instruction, emits nothing, and keeps the entry values of every writable lane (the executor copies them from the inputs, so staging stays fully defined). Dead rows are skipped as before. Rules (all stage 5, before the instructions): unknown or missing keys and JSON types are `SU_E_TYPE` under `/row_filter`; `V & ~M != 0` (no row could run) is `SU_E_TYPE` at `/row_filter/equals`; `F` undeclared or not readable is `SU_E_UNDECLARED_READ` at `/row_filter/field`; `F` not `u32` is `SU_E_TYPE` there; with a filter every writable field must also be readable (`SU_E_UNDECLARED_READ` at `/row_filter`). The filter reads `F` and every writable lane, so `su_effects_derive` lists them as reads. The filter tests one masked value: "kind is A or B" is expressible only when the kinds are bit flags sharing a bit no other kind has (`mask` = that bit, `equals` = that bit); otherwise select in the body.

**Encoding and hash.** `gather_*`: `dst`, `a` = field table index (lane 0), `b` = slot register. `find_eq_u32`: `dst`, `a` = field index, `b` = key register, `c` = span. Artifact format 4 (`SU_ARTIFACT_FORMAT_VERSION_0_4`): the format 3 encoding with version string `"0.4"` and, after `max_events_per_row`, the row filter (u32 present flag; when present the field's lane-0 ID, the mask and the value). Field operands of gathers hash as field IDs like loads. `SuLirModule` records `has_row_filter`, `row_filter_field` (table index), `row_filter_mask`, `row_filter_value` and `uses_gather`.

**Whole-table view.** Chunked execution invokes the VM on slices of the table, but a gather must see every row. `su_vm_context_set_table_view(ctx, table)` (`src/vm/su_vm.h`) gives the context an `SuInvocation` over the whole table (the module's readable fields, row index = slot), checked at each invocation of a module that gathers: shape, generation, one row count, no overlap with any staging column. Without a view the invocation is the table, which is accepted only when it starts at slot 0 (`base_slot` 0), so a chunk can never silently gather from itself (`SU_E_INVALID_ARGUMENT`). `su_vm_run_system` uses its own whole-table invocation; the chunked runners (`su_jobs_run_system_chunked`, the entity runtime) set the view per chunk.

**Determinism.** Every IR 0.4 read is a pure function of the system's entry state (the committed state after the previous system), which no chunk of the same system can modify: staging never aliases the inputs, and the wrapper checks that the table view does not overlap staging. Results therefore do not depend on chunk size, worker count, completion order or tier. `find_eq_u32` scans slots in ascending order and returns the first match, so it is a deterministic order statistic, not a race. Rollback and replay re-run systems on restored state and reproduce the same values; snapshots need nothing new because nothing is cached between invocations.

**Shared state between rows ("claims").** A system can still write only its own row; IR 0.4 makes cross-row protocols expressible as a sequence of systems, each reading the previous one's committed output:

1. claimants (filter: their kind) store the slot of the resource they want, or a sentinel, in a `want` field;
2. resources (filter: resource kind) compute `owner = find_eq_u32(want, slot_u32, span)`, the lowest-slot claimant, and update their own state (for example taken, respawn timer);
3. claimants read `gather_u32(owner, want)` and know whether they won (`== slot_u32`).

The winner is "first claimant by slot order", identical on every machine, tier and chunking. Costs: `find_eq_u32` scans at most `span` (<= 4,096) rows per executing row, so a system's work stays bounded by rows x instructions x 4,096; use a row filter so only resource rows pay it. Dead rows hold zeros, so a key of 0 can match a dead slot: use keys that are never 0 (for example slot + 1) or check the found slot. Not in 0.4: general reductions (count, min, sum over matching rows), more than one match per scan, and writes to other rows.

**VM.** Both tiers implement every 0.4 operation with the same arithmetic (`su_u32_shl`/`shr`/`div`/`rem` in `src/ir/su_ops03.h`). Gathers are never hoisted and never part of an identity-row guard; the filter test runs before the guard, and filtered rows are copied like guarded rows. `ir03_diff --ir04` checks both tiers against the independent interpreter.

**Native.** `su_native_compile` lowers 0.4 modules without cross-row reads: the bit operations and div/rem inline (shift counts >= 32 and zero divisors selected explicitly), and the row filter as a per-row mask test that skips to the next row after the writable lanes were copied as whole columns. A module with `gather_*` or `find_eq_u32` is refused with `SU_E_CAPABILITY_DENIED`, reason `ir_0_4_gather_not_lowered` (the kernel ABI carries only the call's rows), and stays on the VM tiers. A module with `handle_live` is refused the same way, reason `ir_0_4_handle_live_not_lowered` (the kernel ABI carries no entity table; checked before the IR 0.2 entity-op refusal so the reason names it), and stays on the VM tiers (not lowered: sutekh#164 tracks native gathers, which would carry the same whole-table data). Evidence: `ir04_native_diff` (400 programs: the 200 without cross-row reads compile and match the VM lane for lane, the 200 with them are refused).

**Field budget.** `SU_LIMIT_FIELDS` is 1,024 storage lanes per capsule and per world (256 before 2026-10-19). No persisted format stores the limit (snapshots, replays and digests carry the actual field count), so existing snapshots, replays and hashes are unchanged; a world with more than 256 lanes cannot be loaded by an older build.

**Evidence.** `ir04` (verifier rejections with exact codes and pointers, hash coverage of the filter, VM known answers on both tiers and through `su_jobs_run_system_chunked` with 3-row chunks, table-view wrapper checks), `host04` (the claim pattern through the host API over three entity chunks, digests of two identical runs equal), `ir04_diff` and `ir04_native_diff` (`tests/ir03/ir03_diff.py --ir04`: random programs over every 0.4 operation, half of them filtered, against an independent Python interpreter). `handle_live`: `ir04_handle_live` (`tests/ir04/test_handle_live.c`: verifier rejections with exact codes and pointers, flags and hash, known answers on both tiers for despawned, reused, retired, generation-0 and out-of-range handles, the entity-table defaults and wrapper rule, chunked calls against the whole table, and a seeded reference-vs-planned differential of 400 tables against an independent oracle, including a guarded program; with the native backend, the refusal reason), `host_handle_live` (`tests/ir04/test_host_handle.c`: through the host API over three entity chunks, despawn and slot reuse, snapshot restore in place and into a fresh world, both tiers, native refused), and the determinism matrix workload `handles04`. `ir03_diff.py` does not generate `handle_live` (its driver has no entity table).
