# RENDERING / ASSETS PROMPT — No build stalls on the frame path

Start only after the headless milestones required by your task are accepted. Read `docs/RENDERING_AND_ASSETS.md`, the presentation snapshot contract, resource-lifetime rules, and your scoped task.

Implement the smallest useful Vulkan presentation or asset pipeline slice. The renderer consumes committed snapshots and does not write simulation state. Headless engine/test builds remain independent of the GPU stack. Capability-check the actual device instead of assuming a feature or driver version.

Use an established shader compiler through a narrow adapter. Key artifacts by source/includes/options/compiler/target/resource layout. Build only invalidated assets/variants. Pipeline preparation occurs off the render-critical path; producing SPIR-V does not remove driver-side compilation. Preserve a compatible old pipeline or explicit fallback until readiness.

Budget GPU uploads, duplicate old/new resource memory, and retirement fences. Descriptor/schema changes require coordinated rebinding. Do not reclaim GPU resources while in-flight commands can use them. Importers run with bounded access and cannot execute arbitrary source metadata as host commands.

Do not distribute original BFME assets. Public tests use synthetic or explicitly redistributable fixtures. Cosmetic improvements must not mutate authoritative compatibility behavior.

Provide actual captures/timings and a reproducible scene. Distinguish CPU submission, GPU work, import/compile latency, memory, and frame tails. Do not infer whole-game scale from one instanced mesh or empty particle kernel.
