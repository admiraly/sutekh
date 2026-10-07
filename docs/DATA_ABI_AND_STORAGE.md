# Data, storage, and native ABI

## 1. Identity is not address

M0 uses fixed rows and stable fixture entity IDs. Later entity identity is a generational handle with explicitly sized index and generation fields; zero is reserved as invalid. Exhausted generations are retired rather than silently wrapped into a potentially live stale ID. Physical rows, chunk indices, addresses, and GPU instance indices are not entity identity.

Schema and field IDs remain stable across names and layouts. A field's type, default, numeric interpretation, and persistence role belong in schema metadata. A capsule binds to stable IDs once during admission; lookup does not occur by string on every entity.

## 2. Storage progression

M0 is one fixed-row SoA table with u32 columns. Each table has a row count, stable entity ordering, immutable schema generation, and allocated column capacities. All hot-path sizes are validated before pointer arithmetic.

M1 may add chunks and parallel current-row iteration. M2 introduces generational entities, tombstones or swap/compaction policies, relationships, structural command buffers, and schema migration. Choose a single deterministic storage policy per profile before benchmarking alternatives. A physical reorder must not change an order-sensitive gameplay result.

Do not implement a general archetype engine before the live-loop gate. Do not automatically pack every horde's members into consecutive global entity IDs; deaths, replacements, attachments, and upgrades make identity and locality different problems. Use explicit membership ranges/indirection and measured packing policies in the RTS profile.

## 3. ABI boundary

The public ABI is C with explicit-width integers, opaque context handles, versioned structures, structure-size fields, status returns, and borrowed spans. Avoid compiler-dependent enums/bitfields, STL types, variadic calls, ownership across allocators, and implicit exceptions. Internal private C structs may evolve without becoming a public interface.

The native invocation receives validated column bindings and row spans. It does not own those buffers or retain their addresses. Each binding includes field ID, element type/stride, access permissions, length, and layout generation. Native code must either check supplied generations in its wrapper or be callable only through a wrapper that does.

Illustrative signature, to be finalized by the contract owner before workers implement it:

```c
typedef struct SuExecContext SuExecContext;
typedef struct SuInvocation SuInvocation;
typedef struct SuOutputs SuOutputs;
/* uint32_t status; no longjmp/exception propagation through this boundary. */
uint32_t su_invoke_v1(SuExecContext *ctx,
                      const SuInvocation *input,
                      SuOutputs *staged_output);
```

This is an interface sketch, not a completed header. M0-00 owns freezing the actual fields, alignment assertions, error codes, and lifetime rules. Changes after that point require a new generation and dependent tests.

## 4. Calling conventions

Use the host platform's C ABI, selected explicitly by the backend. Windows x64 register/stack rules differ from Unix System V conventions; the Windows ABI includes register arguments and caller-provided shadow space [S04]. Do not reuse earlier conversational `rdi/rsi` examples as a universal ABI.

Assembly kernels must preserve required nonvolatile registers, stack alignment, and floating-point control state. Add platform-specific ABI tests and unwind/debug metadata where required. Never claim Windows support solely because Linux-generated x86-64 bytes execute in one test.

## 5. Alignment, aliasing, and SIMD

Aligned fast paths require proven alignment. Provide an unaligned path or explicit rejection at a trusted internal boundary. Handle zero length, short arrays, nonmultiple vector tails, and overlapping ranges. The verifier or wrapper establishes nonaliasing assumptions; source annotations alone do not authorize out-of-bounds vector loads.

SIMD dispatch is per capability and supported OS register state, not merely CPU marketing name. A baseline scalar path always exists. AVX-512 and architecture-specific tuning are deferred until measurements justify their maintenance cost.

## 6. Serialization

Snapshots and network/replay data use a specified canonical encoding: versioned headers, explicit widths, little-endian integer values, length-prefixed byte/string data, stable entity/field ordering, and maximum lengths before allocation. Never serialize raw C structs or padding. The normalizer rejects duplicate field IDs and inconsistent counts.

M0 fixtures remain JSON for transparency. Binary snapshot encoding becomes necessary only when M1/M2 workload measurements justify it. The representation version and numeric profile are included in every state fingerprint. State equality compares canonical bytes or fields, not hash output alone when reporting a divergence.

## 7. Layout specialization

A later release optimizer may explore SoA, AoSoA, hot/cold splits, and system fusion. It works on a snapshot of the access graph and produces a new layout artifact plus validated bindings. It cannot change public field IDs or invalidate live addresses because capsules never retain them.

Layout candidates are compared on representative end-to-end workloads, not one kernel. Count migration/copy costs, rendering reads, snapshotting, networking, and memory peak. Dynamic data-layout retuning during a match is off by default.

## 8. Allocation model

Use lifetime-based arenas for prepared plans, world columns, tick scratch, and candidate state. Capacities are explicit. Growth is an out-of-tick transaction or a recorded resource failure. The first implementation favors easy-to-audit ownership over a custom allocator zoo.

Reference snapshot staging may use more memory than release mode. Report committed state bytes, reserved capacity, working copies, snapshots, retired code, candidate workers, and asset/device allocations separately.

Sources: [SOURCE_NOTES.md](SOURCE_NOTES.md).
