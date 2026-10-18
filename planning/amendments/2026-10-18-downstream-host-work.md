# Downstream-driven host and platform work — owner-authorized amendment

Authorization, 18 October 2026: the owner authorized work driven by a downstream game outside the current
milestone order (M3, the RTS microcosm, remains in progress and no M3 gate is closed by this work). The downstream
workload is a single-player-first, later server-authoritative, fixed-step 6DOF racing simulation on Windows: a
120 Hz fixed simulation, a presentation host that renders with interpolation, per-tick canonical input records,
replays, state checksums, and snapshot/restore for prediction and reconciliation. This amendment records the scope;
it does not rewrite the root specifications, the IR contract or the milestone table.

## Scope, in the owner's priority order

1. Native Windows build and default test suite with Clang and the MSVC libraries, including the supervisor/worker
   control plane (`serve`, `worker`, process spawning, pipes, fault recovery in a replacement worker) ported through
   the platform layer with Win32 primitives. The POSIX path is kept and not weakened. Tests that are inherently
   POSIX are registered as explicitly skipped on Windows with the reason; none is reported as passed.
2. An in-process host embedding API: a small C ABI (`include/sutekh/host.h`, static library and DLL) that a game host
   links and drives from one thread without the supervisor: world create/destroy from a schema and capsule set,
   fixed tick rate, per-tick generic input records, stepping, zero-copy and copying reads, snapshot capture/restore
   to memory, a canonical state digest, structured error codes. The out-of-process control plane remains the
   agent/development path.
3. A fixed-step host clock helper (accumulator, ticks to run, interpolation alpha, spiral-of-death clamp).
4. Command-stream replay: header (engine and format versions, schema and capsule semantic hashes, numeric profile,
   seed, initial snapshot digest), per-tick input records and sparse keyframe snapshots, with digest-verified playback
   and seek by keyframe.

## Boundaries

- The IR/opcode contract (`LIVE_IR_SPEC.md`, the opcode and type tables of `include/sutekh/ir.h`), the verifier and
  VM semantics are owned by the parallel numeric/3D work and are not changed here except for minimal build fixes.
- Determinism claims are limited to what tests show: identical digests when the same runtime build re-simulates the
  same inputs. Cross-platform or cross-compiler bit identity is not claimed by this amendment.
- Timing figures recorded under this amendment are observations on the development machine, not qualification.
- Windows containment differs from Linux and is reported as such (see `src/agent/su_proc.h`): a job object (no process
  creation, commit limit, user-time limit, kill on supervisor exit, no crash dialogs) and a low-integrity token for
  read-only disposable workers; there is no network denial and no file-size or handle-count limit on Windows.

## Extension, 18 October 2026: host events, rollback-grade state operations, native input, first presentation slice

Same authorization and boundaries as above (owner-authorized downstream work; no M3 gate is closed by it). The
downstream workload adds keyboard/mouse and gamepad control, presentation at 60-120 FPS with interpolation over
the 120 Hz simulation, and later prediction with rollback/reconciliation. Scope, in priority order:

5. Host access to the events systems emit (bounded, ordered, per tick and by cursor), consistent with the event
   log that is already part of snapshots and digests; snapshot restore into the existing world storage without
   reallocation; a fast state digest that is a separately named, non-canonical function, with the SHA-256
   digest kept as the reference; observations of both.
6. A native input layer (`include/sutekh/input.h`, Win32 Raw Input for keyboard and mouse, XInput for
   gamepads): timestamped device state, and a data-driven mapping from device state to a fixed-layout per-tick
   record of integer axes and buttons (dead zones, curves and the mouse virtual stick live in the mapping, not
   in the simulation). Replays store the mapped record, never raw device events.
7. The first M5 slice: a Win32 window and a Vulkan 1.2+ swapchain, presentation decoupled from the simulation
   tick with the clock's interpolation factor, instanced procedural meshes, debug lines and primitives, a
   chase/free camera, CPU and GPU frame-time telemetry, device-capability handling, graceful failure without
   Vulkan, and offscreen rendering with readback as evidence where a presentable surface is unavailable. The
   renderer reads committed state through `host.h` and never writes simulation state. Procedural geometry
   only; no third-party or copyrighted assets.

Rendering here is a first diagnostic slice of M5 (`docs/RENDERING_AND_ASSETS.md` section 2); it does not
close M5 and makes no frame-rate claim beyond recorded observations. Headless builds keep no Vulkan dependency.

## Extension, 18 October 2026: assets, materials and lighting, large scenes, effects, HUD, audio

Same authorization and boundaries as above (owner-authorized downstream work; no M3 gate is closed by it; M5 is
not closed by it either). The downstream workload adds fast craft moving through large space structures (stations,
gates, debris fields) built from meshes the downstream project converts to glTF 2.0 locally, engine trails, weapon
and boost effects, a HUD, and positional sound. Scope, in priority order:

8. An asset pipeline: a glTF 2.0 importer (positions, normals, tangents, two UV sets, indices, several primitives
   and materials, the node hierarchy flattened to placed meshes; PNG/JPEG and uncompressed or BCn KTX2 images) built
   on pinned, permissively licensed single-file libraries fetched at bootstrap (not vendored); an offline cook step
   to a versioned, hash-addressed binary pack with stable resource ids, validated completely on load; background
   loading; asynchronous, budgeted GPU upload outside the frame-critical path; deferred resource retirement.
   Tests generate their own glTF, GLB, PNG and KTX2 files; no downloaded or third-party model enters the
   repository.
9. Materials and lighting for space scenes: a metallic-roughness PBR-lite forward pass with one directional sun,
   ambient and reflections from a procedural sky, normal maps, occlusion, emissive, alpha mask and blend, an HDR
   target, ACES-like tonemapping and sRGB output; timings on the development GPU recorded as observations.
10. Scale: frustum culling, distance LOD, camera-relative rendering from Q32.32 world positions, a persistent
    pipeline cache, and instancing for debris fields of thousands of objects.
11. Effects (presentation only): ribbon trails from interpolated transforms and additive billboards and beams.
12. A HUD: immediate-mode screen quads and text in a project-authored bitmap font.
13. Audio event consumption: a mixer with 3D panning, distance attenuation and Doppler from interpolated
    transforms, WAV and Ogg Vorbis clips, a WASAPI output on Windows and an offline (buffer) path everywhere,
    triggered from committed host events read through the event API, at most once per event even when rollback
    re-emits it. Audio never touches the simulation.

The renderer and the mixer read committed state and interpolated transforms only; nothing they compute is written
to a world. The demo keeps its pinned digest and its shadow-world and rollback checks. No original game asset,
third-party art or downstream-specific name enters the repository; downstream content stays in the downstream
project.
