# Agent instructions — Sutekh

Build the engine described by the root specifications. Start with the **M0 headless no-build gameplay loop**. Do not restart architecture brainstorming, migrate another game repository, or build a full editor/compiler first.

Read `ENGINE_CONSTITUTION.md`, your task packet, and the relevant subsystem contract. The orchestrator additionally reads `ARCHITECTURE.md`, `LIVE_IR_SPEC.md`, `AGENT_PROTOCOL.md`, `MILESTONES.md`, and `planning/tasks.json`.

Gameplay IR edits must not require a native project build. Native C/ASM edits still require targeted compilation and relevant tests before acceptance. Use the broker for longer validation and continue independent ready work; never interpret “no waiting” as permission to claim unchecked source works.

Use isolated worktrees and leased scopes. Do not modify shared contracts without their owner's approval. One integrator owns canonical branch writes. Do not overwrite existing user projects, force-push, delete work, choose a project license, publish assets, or provision paid resources without authorization.

Implement real vertical slices. No fake backends, stubbed success responses, fabricated benchmarks, or unconditional `PASS`. All timing budgets in these specs are targets. Retain readable reference behavior. In-process native faults require checkpoint recovery in a replacement worker, not merely a pointer rollback.

M0 uses C17, strict JSON IR, u32 arithmetic, fixed columns, a checked VM, and a local structured control plane. Native generation, dynamic schemas, broader language features, full RTS, networking, and rendering follow the milestone gates. Assembly remains an optional measured kernel lane.

After each coherent patch, record exact modified paths, test commands and results, unrun checks, source/artifact hashes, blockers, and next work. Update durable task state. Keep useful independent work moving while bounded validation jobs run. Once verification debt or interface risk exceeds the protocol cap, repair instead of accumulating speculative code.

Commands mentioned in specification prose are desired interfaces until implemented. Verify their existence; do not report them as run by copying sample output.
