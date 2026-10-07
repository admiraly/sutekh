# BOOTSTRAP PROMPT — Prove the no-build gameplay loop

Implement the first native Sutekh vertical slice using the specification pack. You are writing working code and tests, not a design essay.

Read `AGENTS.md`, `LIVE_IR_SPEC.md`, `docs/HOT_RELOAD_AND_STATE.md`, `docs/AGENT_RPC.md`, and the M0 tasks in `planning/tasks.json`. Respect the contract owner's frozen headers. If you own M0-00, freeze only the fields and error codes needed by current consumers; do not invent a broad future API.

Use C17 for the resident core, a strict pinned JSON library, small build targets, and a reference interpreter for SU-LIR 0.1. The language is the JSON form already specified. Do not build a new parser syntax, SSA optimizer, JIT, renderer, or general ECS.

Deliver a small native executable with supervisor and worker roles, fixed-row u32 state, the included movement capsule, canonical inspect/snapshot data, and a structured local control path. Implement synchronous stepping before paced running, then implement prepared code replacement at an actual tick boundary without recreating the active worker.

The initial exact fixture is intentionally tiny; expand to 1,024 rows once basic results pass. Test zero rows and arithmetic edges. Preserve invocation-entry loads and staged output semantics. Preallocate tick/staging buffers. A failed working tick must not publish partial state.

Do not execute candidate code in the supervisor. Evaluate proposed capsules on an isolated fixture worker before admission. M0 contains no arbitrary native candidate API and no external side effects. Invalid parse/type/effect/schema input leaves the current implementation and committed root untouched.

Implement the smallest needed subset of the planned RPC methods honestly; unsupported methods return an explicit error rather than success stubs. Make source capture immutable and tied to content hash. Include expected world revision in installation to reject stale candidates.

Required evidence is T00–T15 in `docs/TEST_SPEC.md`. In particular, record supervisor and active worker PID before and after a valid ordinary code swap, record the switch tick and new semantic hash, reject a malformed revision, restore a snapshot, and instrument child-process invocations to show no native project build occurs during capsule editing.

Run a targeted compile whenever you change C/ABI code and run focused tests. Longer checks may use the broker while you implement independent ready work. Do not claim M0 is complete without native execution. If tools are missing, record the exact blocker and continue only useful independently verifiable work.

Use real timing samples; do not print specification targets as results. Report warm preparation, preview, boundary, installation, and native build times separately. Do not optimize a million-row workload before the first correct live loop.

At completion, provide the exact build/run/demo/test commands, changed paths, hashes, actual observations, unrun gates, and next task. Leave a reproducible demonstration rather than screenshots of invented output.
