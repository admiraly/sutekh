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

## Extension, 18 October 2026: same-tick operator results, proximity, binding diagnostics, consumer build

Same authorization and boundaries (owner-authorized downstream work; no M3 gate is closed by it). Scope:

8. Post-query ordering: an IR system may order itself after a host operator of an earlier phase (`after` edge),
   verified by the effect analysis, so a response system consumes the operator's results in the same tick, through
   `sutekh serve` and `host.h` alike, with snapshots, rollback and replays unchanged in kind. Schedules without such
   an edge are unaffected (their acceptance, order and digests do not change).
9. A proximity variant of the space3d operator (`space3d.query_prox`, a separate descriptor and record): per mover,
   the closest collidable within a per-mover margin (slot, generation, distance), deterministic tie-break. The
   existing `space3d.query` descriptor, constants hash and results are unchanged.
10. Clearer verifier/schedule messages for q32/i64 lane binding and for read/write flags that differ from the IR's
    loads and stores; documentation of both rules (`docs/SPACE3D.md`).
11. Consumer build ergonomics: `SUTEKH_TOOLCHAIN_HOME` for `tools/env-windows.ps1`; `SUTEKH_PYTHON`,
    `SUTEKH_VULKAN_INCLUDE_DIR` and `Sutekh::vulkan_headers` visible to `add_subdirectory` consumers.

The IR/opcode contract (`LIVE_IR_SPEC.md`, `include/sutekh/ir.h`) is not changed: item 8 is a scheduling rule of the
effect analysis, not an IR change.

## Extension, 18 October 2026: streamed audio, voice offsets and loop points, translucent instances

Same authorization and boundaries (owner-authorized downstream work; no M3 gate is closed by it; M5 is not closed
by it). Requests from the downstream project, stated generically:

14. Streamed playback of long Ogg Vorbis and WAV clips with bounded memory (`SuAudioStream`), next to the existing
    whole-decode `SuAudioClip` for short sounds; an offline render of a streamed clip equals the whole decode.
15. Per-voice start offset and loop start/end (frames or seconds), exact seeking and position readback, for both
    clip kinds.
16. Per-instance translucency: `SuRenderObject.tint[3] < 1` draws any material see-through (a "ghost" copy) in the
    blended pass with depth-correct back-to-front ordering.

Note, not an engine feature: a per-entity "collision immunity after respawn" window is game logic. It is a timer
field on the entity that the IR collision-response system tests before applying a response (decremented once per
tick), so it needs no engine support and no change to the operator or the IR contract.

Audio and the renderer still read committed state only; neither writes a world, and the demo's pinned digest is
unchanged.

## Extension, 18 October 2026: capsule movers

Same authorization and boundaries (owner-authorized downstream work; no M3 gate is closed by it). Request from a
downstream 6DOF racing project, stated generically: a mover is a sphere of `radius` about its position, which fits
long or flat craft poorly (one measured hull: bounding sphere 40.9 m for half extents 18.3 x 13.7 x 32.8 m, so side
contacts fire up to 22.6 m from the surface, 4.35x the box volume; a capsule along local z fits it at r 24.3 m,
1.45x the box volume).

17. An optional capsule shape for space3d movers, as a fourth descriptor and record (`space3d.query_shape`: the
    `space3d.query_prox` lanes plus q32 read lanes `z0` (72) and `z1` (74), the core segment on the mover's local +z
    axis rotated by its `rot` lanes). Sweeps against spheres, boxes, capsules and other movers with the same
    toi/normal/contact contract and accuracy domain; broadphase bounds and proximity cover the capsule. Rotation
    during the tick is handled by holding the end-of-tick orientation for the whole tick (translation-only sweep),
    documented with its error bound in `docs/SPACE3D.md`. A mover with `z0 = z1 = 0` takes the sphere path bit for
    bit; `space3d.query` and `space3d.query_prox` (descriptors, hashes, results, digests) are unchanged.

The IR/opcode contract is not changed: the new lanes are an operator declaration, bound like the existing ones.

## Extension, 18 October 2026: HDR bloom, exposure, glare

Same authorization and boundaries. Request from the same downstream project: engine glows, running lights and
emissive surfaces should bleed light like a camera.

18. Presentation only (`su_render`; nothing in the simulation, IR or host contracts changes): `SuRenderPost` on
    `SuRenderFrameDesc` (zero = off, existing frames unchanged): thresholded soft-knee bloom over the existing
    RGBA16F scene target (13-tap down, tent up, half resolution; threshold, knee, intensity, radius, levels), a
    stateless deterministic optional auto exposure (key, clamp), `SuRenderDeviceInfo.bloom` and a plain-tonemap
    fallback when the HDR format cannot be filtered, `su_fx_glare` (optional cheap streaks, host-called), and
    stats (`bloom_levels_active`, `post_passes`). Per-material (`emissive_strength`) and per-instance
    (`SuRenderObject.emissive`) emissive multipliers already existed and feed it. `examples/presentation` shows it
    (`--no-bloom`, `--no-glare`, `--auto-exposure`). Tests: `test_render_bloom`. Not done: temporal exposure
    adaptation, a lens-dirt or anamorphic flare, a compute-based chain.
