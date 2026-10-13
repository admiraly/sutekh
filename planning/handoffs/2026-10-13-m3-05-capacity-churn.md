# M3-05: capacity acceptance and RTS entity churn

Base: `9536785`. M3-05 remains in progress; this checkpoint addresses
T-RTS-CAPS-3 and T-RTS-CHURN-1. No milestone closure is claimed.

## Capacity and cap spans

`test_rts_capacity` loads the four committed RTS IR capsules and the two
production host capsule records, builds the actual six-system schedule, and
completes one tick at world capacities 699,051 and 1,000,000. Each world has one
live unit; checks cover preserved position/HP, target semantics, tick completion,
and zero dead-tail columns. This verifies capacity admission and execution,
not a populated million-unit workload or throughput.

At the queue layer, capacities 699,051 and 1,048,576 fit per-system buffers;
the structural allocation report equals one producer's capacity. Uniform
six-system caps refuse both sizes, reproducing the old sizing ceiling. Empty
queues with zero-cap systems commit without callbacks or event records, and a
despawn appended in the final chunk merges correctly. A 1,048,576-row world is
still refused: OD-21 retains `SU_LIMIT_ROWS = 1,000,000`.

The legacy raw-pointer `su_cmdq_create_ex` cannot discover array lengths. Added
`su_cmdq_create_ex_checked` with explicit element counts, rejecting either
short span before reading cap values or allocating. The legacy API/signature
and its caller-provided-length precondition remain intact; entity ticks use
the checked variant. This additive internal helper implements the short-array
acceptance gate under the owner's standing implementation authorization; it
does not change published ABI structures, resource caps, or numeric semantics.
Tests independently shorten each span and check matching spans, including a
zero-allocation refusal. Callers must report actual readable span lengths;
this is not a claim that C pointers can be introspected or made hostile-safe.

## RTS churn

`test_rts_churn` fills a 16,384-capacity world with all 39 RTS fields and applies
100,000 accepted despawn/spawn operations in batches of randomly distributed
holes. An independent model scans its own liveness array to predict lowest-slot
reuse, generations, external IDs, counts, and values. After each removal/refill
batch it compares the entire entity table and every committed field, requiring
dead columns to be zero. Old handles must refuse after reuse. The test performs
233,161 checks, with zero failures. Initial population and stale probes are
additional to the 100,000 accepted churn operations.

This inherits the M2 world-library churn boundary; it does not claim supervisor
RPC churn, event/recovery replay coverage, battle simulation, or Python/engine
digest equality. Those have distinct tests and remaining gates.

## Verification

- Default build: warning-enabled compile; ctest 45/45 passed.
- Native-enabled build: warning-enabled compile; ctest 56/56 passed with only
  the unchanged WSL `native_avx2` XSAVE feasibility probe excluded. That known
  failure is retained in the earlier host-fault evidence; the overall native
  suite is not described as completely passing. Capacity capsules run through
  the VM/host schedule without installing native variants.
- Capacity: 52 checks, zero failures. Admission: 32 checks, zero failures.
- Focused ASan + UBSan: admission, capacity, and RTS churn all passed (3/3).
- Isolated mutations detected: M3-M33 restores the global structural cap;
  bypassing explicit cap-length checks; skipping committed-column zeroing on
  despawn. Each compiles and fails assertions. Original source restored and
  positive tests rerun; no mutation is committed.

Commands, logs, source/input/binary hashes, and restored validation-worktree
details are in `planning/evidence/m3-capacity-churn/result.json`. Modified paths:
`CMakeLists.txt`, `src/runtime/entity_tick.c`, `src/world/entities/su_cmdq.{c,h}`,
`tests/entities/test_cmdq_admission.c`, the two new RTS test files, durable status,
this handoff, evidence, and regenerated pack artifacts/checksums.

Native Windows, independent review, full sanitizer matrix, million-row loading,
and engine-versus-Python-model digest equality were not run here. Elapsed times
are shared-WSL observations only. Architecture/specification files are unchanged.

## Next

T-RTS-LOAD-1 scale/high-water checks and Q8 engine-versus-model digest equality
remain open. Existing scene-admission, snapshot/spool limits, and the
N <= 65,536 recovery claim remain unchanged; queue/world capacities above do
not override those separate limits.
