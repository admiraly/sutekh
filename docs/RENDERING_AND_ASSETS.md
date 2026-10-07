# Rendering, assets, and the shipping player
**Later-stage contracts. Do not implement these before the headless gates.**

## 1. Simulation/presentation boundary

The renderer consumes committed presentation snapshots and effect events. It cannot mutate authoritative simulation state through entity pointers or GPU readbacks. Interpolation, cosmetic particles, camera effects, and animation presentation can run at a different rate from the semantic simulation tick.

GPU scene data uses stable object/asset IDs and explicit lifetime generations. The renderer owns GPU buffers, descriptors, pipelines, fences, and retirement. Their handles never appear in canonical simulation snapshots.

## 2. Initial rendering profile

Use Vulkan with a tested baseline capability profile; Vulkan 1.3 is a candidate initial desktop baseline, subject to actual device/driver support. Record required versus optional features. Headless builds never depend on Vulkan. Introduce a minimal instanced diagnostic scene before adding a GPU-driven frame graph, compute culling, animation, shadows, or large particles.

Do not promise that every unit type collapses into one draw. Material state, mesh variants, skinning, visibility, shadows, transparency, and rendering passes determine batching. The engine should reduce per-object CPU work while measuring actual GPU and CPU cost.

## 3. Shader/pipeline edits

Shader source is an asset with explicit include dependencies and compiler/target/resource-layout fingerprints. Compile only affected variants using an established compiler adapter. Prepare driver pipelines away from the render-critical path. Keep the last compatible pipeline active until the new one is ready; use a documented fallback when an interface change makes that impossible.

Khronos documents internal shader compilation during pipeline creation and its potential frame-time cost [S11]. Pipeline cache-control features can avoid unexpectedly compiling in a guarded creation attempt on supporting devices [S12]. These mechanisms help schedule work; they do not guarantee a zero-cost first-use pipeline.

Changing descriptor/resource layouts is an interface migration, not just replacing a shader pointer. Revalidate bindings and dependent materials. Retire old GPU resources only after relevant device work completes. Pipeline caches include implementation/device compatibility metadata and are never trusted across arbitrary driver changes.

## 4. Asset pipeline

Asset identities are content- and dependency-addressed. Each importer declares source formats, dependency discovery, output version, target settings, and importer/tool version. Changing one model invalidates that model and its affected derived artifacts—not the entire project. Changes to importer logic correctly invalidate all affected outputs.

Imports run in isolated bounded workers. Malformed or hostile files cannot write outside the artifact store or execute arbitrary scripts. Large texture/mesh conversion can remain pending while the game uses the previous artifact. A viewport must expose stale/loading/error state rather than pretending an import completed.

Budget CPU staging, upload bandwidth, GPU memory peak, and resource retirement. Replacing a large texture can temporarily need both old and new allocations. A live asset edit is allowed to be rejected or deferred for insufficient resources.

## 5. BFME assets

The compatibility adapter reads original assets from a configured local installation. Importer tests use synthetic/minimal redistributable fixtures in public CI. Do not commit original archives, extracted textures/models/audio, or user-installation paths. Unknown format behavior is tracked as a compatibility gap with evidence.

Preserve semantic separation between visual upgrades and original gameplay. Changing unit mesh/particle density is not permission to change target selection, collision, attack timing, or authoritative visibility.

## 6. Audio

Audio consumes committed effect events. Use an established device/mixing backend initially; do not rewrite codecs in assembly. Real-time audio callbacks cannot block on the agent control plane, memory allocation, asset import, or GPU work. Asset updates and voice lifetime changes are prepared outside the audio callback.

Speculative candidates do not play real sounds. Development effect IDs and commit indices prevent duplicate externally visible effects after controlled recovery where supported.

## 7. Release player

The shipped player/server excludes agent RPC, source watchers, untrusted compilation, test imports, and development secrets. It loads a pinned semantic/content manifest and validated platform artifacts. JIT may be disabled where unnecessary or disallowed. Offline native optimization and shader preparation are packaging operations, not normal gameplay edit operations.

Maintain debug/source-map artifacts separately for profiling and crash diagnosis. Distribution size and startup time are measured alongside runtime performance. Dependency notices and the final license are owner-approved release requirements.

Sources: [SOURCE_NOTES.md](SOURCE_NOTES.md).
