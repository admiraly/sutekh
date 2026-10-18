# WIP handoff: presentation-assets (stopped on owner request, usage budget)

Branch `feat/presentation-assets` (worktree `C:\Users\dev\Documents\trinity\engines\sutekh-worktrees\presentation-assets`),
head `c2e7c47` plus this file, pushed. Draft PR: https://github.com/admiraly/sutekh/pull/33. `origin/main` merged up to
PR #32 (`ef610d5`). The full report with APIs, exact commands, results, measurements and unrun items is
`planning/handoffs/2026-10-18-presentation-assets.md`; read it first. This file adds where I stopped and the original
brief verbatim.

## Done (all six brief items have a tested first implementation)

| Command (after `. .\tools\env-windows.ps1`, from the worktree) | Result at `c2e7c47` |
|---|---|
| `ctest --test-dir build-win -j 8` (Debug) | 95 tests: 93 passed, 0 failed, 2 skipped (`path_fifo`, `containment`, POSIX-only) |
| `build-win\test_asset_pak.exe` | 103,522 checks, 0 failures |
| `build-win\test_asset_bc.exe` | 29 checks, 0 failures (BC1 gradient 38.54 dB, BC4 51.14 dB, BC5 normals mean 0.25 deg) |
| `build-win\test_asset_gltf.exe build-win\asset_gltf_work` | 158 checks, 0 failures |
| `build-win\test_render_portable.exe` | 84 checks, 0 failures |
| `build-win\test_render_assets.exe build-win\render_assets_work` | 114 checks, 0 failures on the Quadro P600 |
| `build-win\test_audio.exe tests\fixtures\audio\tone440_stereo.ogg` | 220 checks, 0 failures |
| `build-win\test_audio_device.exe` | ok (RDP "Remote Audio" endpoint) |
| `space_demo --offscreen ... --frames 240 --rollback-check --expect-digest fa02b76e...defd` | digest matches, 59 drills 0 differed, 1 gate sound for 1 gate event, 0 replayed |
| Validation layer (default and sync), 12 runs, at `76e63ac` | 0 errors, 0 warnings (`planning/evidence/presentation-assets/validation-runs.log`) |
| CI run 39137189595 (`a13974f`) | all five jobs passed (Linux renders `render_assets` on lavapipe) |

The commit `c2e7c47` (BC compression, single-pass open, stress scene) has not been seen by CI yet, and the validation
layer has not been re-run after it. Check both first.

## Where I stopped

- Just committed `c2e7c47`: cook-time BC1/BC3/BC4/BC5 (`SU_ASSET_COOK_BC`, `asset_cook --bc`), `SU_ASSET_PAK_VERIFY_ENTRIES`
  (open now hashes once by default), sRGB threshold search in mip generation, `tools/gen_stress_gltf.py`,
  `planning/evidence/presentation-assets/stress-runs.log`.
- Not yet done for that commit: (a) update `planning/handoffs/2026-10-18-presentation-assets.md` with the BC section and
  the stress numbers (stress-runs.log: 729 MB RGBA8 pack vs 125 MB BC pack; loader 2.7 s vs 0.45 s; upload 44 vs 8
  frames at 16 MiB/frame; 1080p GPU median 2.78 ms with 215 of 240 stress nodes in view); (b) re-run validation
  (`C:\Users\dev\sutekh-scratch\record_assets.ps1 -Root <worktree>` regenerates runs.log and validation-runs.log);
  (c) note in `THIRD_PARTY_NOTICES.md`/docs nothing new is needed (the BC encoder is project-authored); (d) the PR
  description does not mention BC yet.
- Known, reported, not mine: `native_avx2` (Linux native tier, sim-runtime stream) failed twice on this PR at its
  YMM-state probe (`test_native_avx2.c:1057`) and passed on re-run.
- Helpers outside the repo: glslang `C:\Users\dev\sutekh-scratch\glslang-16.6.0\bin\glslang.exe`
  (`python -I tools/compile_shaders.py --compiler <it>` after editing any shader); validation layer
  `C:\Users\dev\sutekh-scratch\vvl-build\layers` (`VK_ADD_LAYER_PATH`, `SUTEKH_RENDER_VALIDATION=1` for
  `test_render_assets`, `--validation` for the demo); `C:\Users\dev\sutekh-scratch\ppm2png.py`; throwaway venv with
  python-soundfile `C:\Users\dev\sutekh-scratch\oggvenv`. Bootstrap after a fresh clone: `python -I
  tools/bootstrap_vulkan_headers.py` and `python -I tools/bootstrap_asset_deps.py`.
- Tooling gotcha: in this environment the Bash tool's heredocs mangle backslash escapes (`\n`, `\0`); write patch scripts
  with the Write tool and run them with `.venv\Scripts\python.exe`.

## Suggested next steps (beyond the brief's first implementation)

1. CI and validation for `c2e7c47`; update the main handoff and the PR text (BC, stress numbers).
2. Cook a real downstream model locally (outside the repository) with `asset_cook --bc` and record cook time, pack size,
   upload frames and frame times.
3. Bloom for emissive lights/engines; sun shadows; GPU memory sub-allocation; a dedicated transfer queue; BC7 encoding.
4. Audio: changing pitch after start (`su_audio_voice_update` carries only position, velocity, gain), streaming, a Linux
   backend.
5. Arc A770 physical-console run.

## Original brief (verbatim)

You are a dedicated engineer on **Sutekh**, the owner's (GitHub: admiraly) custom C17 headless, agent-native game engine. You develop it in exactly the ways a downstream game needs. Work autonomously over a long session; you have standing permission to commit and push. Never idle waiting on CI: push, keep working, check later.

## Where you work
- Worktree: `C:\Users\dev\Documents\trinity\engines\sutekh-worktrees\presentation-assets`, branch `feat/presentation-assets`.
  - It is based on `origin/feat/presentation-input` (8c30b31), which is PR #31 and is about to be merged to main by the parent.
  - When it lands, merge `origin/main` into your branch. Never rebase or force-push.
  - Regenerate `PACK_VALIDATION.json` with `tools/validate_pack.py --write-report` instead of merging it.
- The canonical checkout `engines\sutekh` is the parent's; don't edit it.
- `local\` (LLVM 23.1.3) and `.venv\` are junctions. Dot-source `tools\env-windows.ps1`.
- Shell: PowerShell 5.1 (Bash also available). Git: `C:\Program Files\Git\cmd\git.exe`. gh: `C:\Program Files\GitHub CLI\gh.exe`.
- Machine: i5-12400F, 16 GB RAM, NVIDIA Quadro P600 2 GB. The user connects over RDP; Vulkan works on the physical GPU through RDP, but vsync is paced by the remote display.
- **The Vulkan SDK is NOT installed.** Its winget install needs a UAC elevation nobody approved. The previous agent compiled shaders with glslang 16.6.0 (offline, embedded SPIR-V) and built the Khronos validation layer from source; reuse whatever it left. Its handoff tells you where.

**Parallel agent:** another agent works in `...\sutekh-worktrees\sim-runtime` (branch `feat/sim-runtime`). It owns the IR/VM/native backend/scheduler/space3d and rollback re-simulation speed. Don't touch those. You own presentation, assets and audio. Keep shared-file edits small.

## Read first
- `planning/handoffs/2026-10-18-presentation-input.md` (the previous agent's full report: `render.h`, `input.h`, host events, the demo).
- Plus `2026-10-18-numeric-3d.md` and `2026-10-18-windows-native-host.md`.
- `docs/RENDERING_AND_ASSETS.md`, `MILESTONES.md` (M5), `ENGINE_CONSTITUTION.md`, `AGENTS.md`, `THIRD_PARTY_NOTICES.md`, and the downstream-work amendment in `planning/amendments/` (extend it).

Follow Sutekh's process rules:
- real tests, no fake PASS;
- exact commands and results;
- a dated handoff (today is 2026-10-18);
- timings labelled as observations;
- the renderer and audio never write authoritative state.

## What the downstream game needs (priority order)
The downstream game is a 6DOF space racer. It is fast-moving craft through large space structures (stations, gates, debris fields), with engine trails, weapon effects and a HUD. Its art comes from converted game meshes that the downstream project will provide locally as glTF 2.0. Sutekh must load generic glTF; no third-party art ever enters this repo.

1. **Asset pipeline: glTF 2.0 → runtime meshes.**
   - Load .gltf/.glb meshes: positions, normals, tangents, UV0/UV1, indices, multiple primitives/materials, node hierarchy flattened to mesh instances. Textures are PNG/KTX2 (an uncompressed or BCn path is enough).
   - Use a pinned, permissively licensed parser (e.g. cgltf) and image decoder (e.g. stb_image), recorded in `THIRD_PARTY_NOTICES.md`.
   - Add an offline cook step to a simple versioned, hash-addressed binary format with stable resource IDs.
   - Upload asynchronously, never synchronously in the frame-critical path (an M5 rule), with safe resource retirement.
   - Tests use small procedurally generated glTF files written by the test itself. No downloaded models committed; if you use any Khronos sample model locally, keep it out of git.
2. **Materials and lighting good enough for space scenes.**
   - A metallic-roughness PBR-lite forward pass: one directional "sun", ambient/IBL-lite from a procedural or cubemap sky, normal maps, emissive (glowing station lights and engines matter).
   - Tonemapping (ACES-like) and sRGB-correct output.
   - Keep it simple and fast enough for the 1080p 60 FPS floor on a mid-range GPU. Measure on the P600 and label results as observations.
3. **Scale and frame-critical essentials.**
   - Frustum culling and distance LOD selection.
   - Large-world rendering: camera-relative rendering from Q32.32 world positions, so tens-of-km tracks don't jitter in float32.
   - Pipeline cache persistence.
   - Instancing for debris fields of thousands of objects.
4. **Effects.** GPU particle or ribbon trails for engine exhaust (ribbon trails following interpolated transforms), additive billboards for weapon and boost effects. Effects are presentation-only.
5. **Text/HUD.**
   - A bitmap or SDF font from a freely licensed font (record its licence), or a tiny generated font.
   - Screen-space quads for a HUD: position, lap, speed, timers.
   - An immediate-mode-style API is fine.
6. **Audio event consumption.**
   - A small audio module (WASAPI or XAudio2 on Windows; null backend elsewhere) that plays generated/test tones and decoded WAV/OGG clips (pinned stb_vorbis or similar).
   - 3D panning/attenuation and Doppler from interpolated transforms.
   - Triggered from host events read through the event API.
   - Never touches the sim.
   - Tests: mixer correctness offline (render to a buffer); device output only as a smoke test.

Update the `examples/presentation` demo progressively to show these with procedural content plus an optional local glTF path argument. Ship coherent, tested commits; push regularly; open a draft PR to main for CI after item 1. If verification debt grows, stop and repair.

## Hard rules
- Sutekh is a **PUBLIC** repo (PolyForm Perimeter; SPDX headers on new files).
  - Never add EVE Online / CCP assets, names or any third-party art.
  - Don't mention the downstream game's name or specifics; use generic terms only.
- No force-push, no history rewrites, no deleting others' work, no licence changes.
- Commit with `-c user.name=admiraly -c user.email=249692417+admiraly@users.noreply.github.com`. End each message with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Don't merge to main; the parent integrates.
- If something needs admin elevation (UAC), don't start it and leave it waiting. Note it in your report instead, because nobody is there to approve it.

## Final report
- What's done, with exact test commands and results.
- The new API surface.
- Measurements (observations).
- What's unrun and why.
- Commits and the PR URL.
- Contract changes for the integrator or the other agent.
- Next steps.
