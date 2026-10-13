# M3-05: refuse impossible queue geometry before allocating

Base: `0e27c80`. This is a resource-admission repair discovered while validating
the host-fault recovery patch. M3-05 remains in progress.

## Problem and change

The uniform-cap wrapper allocated and filled two `nsystems` arrays before the
extended constructor checked `nsystems * nchunks` against `SU_CMDQ_MAX_BUFFERS`.
The existing invalid-input test requests 2^30 systems: the wrapper allocated
approximately 8 GiB despite the configuration being impossible. On this WSL
host it exhausted available RAM and paged heavily before finally refusing.
The extended constructor also scanned cap-array inputs before its buffer-count
resource check.

`buffer_dimensions` now validates slot/chunk geometry and bounds the product
with division before either constructor allocates proportional storage or the
extended constructor reads per-system caps. It preserves existing queue limits
and accepted geometry, returns the relevant invalid-argument/resource status,
and leaves the output NULL on refusal. No ABI, cap, or milestone gate changed.

`test_cmdq_admission` recompiles the queue with counted allocation wrappers. It
requires zero allocation calls for oversized system counts, oversized chunk
counts, and an overflowing buffer product; it supplies only a single cap value
for unadmittable geometry, ensuring the extended constructor refuses without
walking that array. Valid small queues still allocate, append, and merge.
Wrappers deny allocations above 1 MiB so a guard regression fails assertions
without paging the host. Production builds do not use the wrappers.

Modified: `src/world/entities/su_cmdq.c`, `CMakeLists.txt`,
`tests/entities/cmdq_admission_seam.h`, `tests/entities/test_cmdq_admission.c`,
`planning/status.json`, this handoff, and `planning/evidence/m3-cmdq-admission/`.
Generated pack artifacts and checksums regenerated at the checkpoint.

## Evidence

Commands, exact source/artifact hashes, and logs are recorded in
`planning/evidence/m3-cmdq-admission/result.json`.

- Warning-enabled dev and native builds passed.
- `ctest --test-dir build -j 4 --output-on-failure`: 43/43 passed.
- `ctest --test-dir build-native -j 4 --output-on-failure -E '^native_avx2$'`:
  54/54 passed. The unchanged upper-YMM XSAVE feasibility probe was not rerun;
  its known WSL failure is retained in the preceding host-fault evidence. This
  is not an all-native-tests-pass claim.
- `ctest --test-dir build -R '^cmdq_admission$' -V`: 30 checks, zero failures.
- The same admission test under ASan + UBSan: 30/0, no sanitizer report.
- An isolated-worktree mutation bypassing the uniform wrapper's refusal is
  detected by allocation-counter assertions. The mutant was restored and
  positive checks rerun. The pre-guard queue also failed the new assertions;
  its ordinary oversized test was stopped rather than repeatedly paging.

Shared-WSL timings are observations only, not performance qualification.
Native Windows, independent review, and a full sanitizer matrix were not run.
The dedicated sanitizer configuration/build commands and restored worktree
path are recorded in the result file.

## Next

Continue M3-05 capacity/world-scale tests, RTS churn, and engine-versus-model
comparison. The existing snapshot/recovery size claim is unchanged.
