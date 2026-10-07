# Build and validation policy
## No native project build for ordinary gameplay; fast, mandatory native checks when needed

## 1. Bootstrap once

The bootstrap agent inventories actual OS, compiler, linker, SDK, assembler, Python, available memory, and permitted network access. It resolves dependency versions to reviewed commits/releases and records their hashes and licenses. No speculative version numbers appear in this spec.

Create a small CMake/Ninja graph with independent native targets for IR, VM, world, runtime/control, and platform tests. Configure once per toolchain/build mode. Compile third-party libraries once per fingerprint. Do not create giant unity translation units, mandatory LTO, template-heavy public headers, or runtime-dependent code generation for every source edit.

Use C17 with warnings, debug symbols, and a fast development optimization setting established by bootstrap measurements. Keep sanitizers in separately fingerprinted lanes. Use precise floating-point flags in any future authoritative FP target rather than an accidental project-wide fast-math setting. Release optimization, LTO, PGO, and full platform matrices are batch lanes.

LLD is the initial cross-platform linker family [S07]. A Linux mold substitution may be benchmarked, but an unrelated project's published link time is not our budget. Windows uses its own ABI, object format, SDK, and tests.

## 2. Validation lanes

| Lane | Purpose | Author waits? |
|---|---|---|
| L0 | JSON/IR/effects/types, changed-file sanity | Yes, bounded immediate feedback |
| L1 | Targeted native compile/assemble; small unit fixtures | Briefly within budget; otherwise enqueue |
| L2 | Affected capsule/integration/replay/differential tests | No global author barrier; acceptance awaits result |
| L3 | Sanitizers, fuzzing, multi-platform, long runs | Independent build/CI workers |
| L4 | Isolated performance qualification and release builds | Dedicated controlled worker |

A gameplay change to a supported capsule goes through L0 and runtime tests without compiling the engine. A native change requires at least a real affected-target compilation and relevant tests. “Looks syntactically correct” is not an equivalent gate.

The default interactive native-check budget is **2 seconds**, a proposed scheduling threshold, not a promised compiler speed. Longer jobs keep running under the broker with durable records. Do not kill useful validation merely because it exceeded the author waiting budget.

## 3. Build broker

The broker deduplicates requests using source tree/subgraph hash, generated headers, compiler/linker versions, flags, target triple, dependency revisions, and build mode. Every job has a durable ID, state, log, artifact directory, and exit status. It refuses stale or underspecified requests.

Limit native compile jobs by observed memory and machine load. A sensible bootstrap default is one build pool and one verification pool, then tune from measurements. Do not multiply `-j all_cores` by the number of agents. Coalesce superseded edits, retain reusable outputs, and cancel obsolete work that cannot produce an admissible artifact.

The broker may be a small Python process over local job manifests. Do not build a distributed scheduler before the local queue works. Remote builders are optional and receive pinned, secret-free build inputs.

## 4. Planned command surface

These are desired interfaces to implement, **not commands that exist in this package**:

```sh
python3 tools/dev.py configure --profile dev
python3 tools/dev.py check --changed --json
python3 tools/dev.py submit-build --task M0-02 --json
python3 tools/dev.py job --id JOB_ID --json
python3 tools/dev.py test --suite zero_wait --json
python3 tools/dev.py benchmark --suite reload --json
```

Keep a thin human CLI and machine JSON output. Error exit codes remain meaningful. Do not hide a full dependency rebuild behind `check --changed`. A developer can run direct Ninja targets when diagnosing the broker.

## 5. Correct invalidation

A header change invalidates all actual consumers. A schema/ABI change invalidates affected bindings and compiled artifacts. A code generator change invalidates its outputs. Toolchain or flag changes produce a new cache namespace. Results from an unknown fingerprint are misses, never assumed hits.

Prefer the existing build tool's dependency graph to a second incomplete hand-maintained graph. Capsule dependencies still need their own semantic graph because they are runtime artifacts rather than C translation units.

## 6. CI policy

Use public-repository standard GitHub-hosted runners for correctness and build checks where appropriate. Standard public-repository runner usage is free under GitHub's documented policy, while larger runners are charged; concurrency, storage, execution, and other service limits still apply [S08, S09]. Therefore CI is neither unlimited parallel capacity nor a subsecond interactive loop.

Use affected-target selection, grouped matrices, dependency caches, artifact retention limits, and cancellation of superseded runs. Pin third-party actions by reviewed immutable revisions. Never expose secrets or persistent personal machines to untrusted pull-request code. A dedicated performance runner accepts only authorized trusted workloads and does not use broad privileges merely to obtain counters.

Do not provision paid larger/GPU runners, change billing settings, or assume existing IBM/Vast credits remain active without authorization and verification.

## 7. Evidence artifacts

Each run stores a machine-readable result containing exact commands, environment fingerprint, source/artifact hash, test corpus revision, actual duration, return codes, and explicit skipped/unavailable fields. CI artifacts distinguish a specification target from an observed result.

Build failure feedback identifies the smallest failing target and relevant diagnostics. The repair worker owns the failing scope; unrelated capsule work may continue. Repeated ABI failures stop dependent work until the contract owner repairs the interface.

## 8. Bootstrap escape clauses

If a compiler is missing, report that fact and continue with useful spec/example/parser work that can be validated using available tools; do not fabricate native results. If network access is missing, use existing pinned dependencies or record the missing dependency and work on independent tasks. Do not substitute an enormous new toolchain to avoid a small setup issue.

These are truthful degraded modes, not a declaration that uncompiled code is complete. M0 cannot close until the native loop actually runs.

Sources: [SOURCE_NOTES.md](SOURCE_NOTES.md).
