# SUTEKH — Agent-native game execution engine

<!-- SPDX-FileCopyrightText: 2026 admiraly -->
<!-- SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 -->

**Sutekh is a source-available C17 game execution engine for deterministic,
agent-driven simulation and presentation.** It combines a checked gameplay IR,
a resident runtime, safe live revision, rollback-capable state, native host
operators, and optional rendering and audio backends.

[Build and test](#build-and-test) · [Capabilities](#capabilities) ·
[Documentation](#documentation) · [Contributing](CONTRIBUTING.md) ·
[Security](SECURITY.md) · [Commercial/OEM licensing](COMMERCIAL_LICENSE.md)

## Current status

Sutekh is well beyond its original headless-runtime milestone. The current
engine is an implemented M3-stage runtime with a six-system RTS vertical slice
and production-shaped C APIs. It supports deterministic checked-mode execution,
structured control-plane requests, isolated candidate evaluation, tick-boundary
installation, snapshots, replay, worker replacement after faults, and
allocation-aware world updates.

## Capabilities

Recent integrated capabilities include:

- **World and simulation:** generational entities, multiple declared tables in
  one world, 64-system schedules, schema-bound fixtures, cross-table handles,
  deterministic events, rollback state operations, and replay/digest identity.
- **Gameplay IR and host extensions:** IR 0.4 bit operations, row filters and
  cross-row reads; registered game-defined host operators; operator-owned
  variable-size state in snapshots, digests, and replays; deterministic host
  events; and `handle_live` validation.
- **RTS runtime:** atomic `world.set_fields` and `world.spawn_batch`, formation
  and spatial/scatter operators, structural queues and caps, replay drills,
  churn tests, and replacement-worker recovery.
- **Geometry and numeric execution:** fixed-q32 numeric support, `space3d`
  queries, compound movers, static meshes, host-operator data, broadphase v2,
  and declared pair budgets.
- **Presentation and content:** Vulkan rendering with offscreen readback and
  windowed presentation, procedural and imported assets, glTF/GLB cooking,
  PBR-lite materials, HDR and effects, HUD text, input mapping, and GPU/CPU
  telemetry.
- **Audio:** event-driven buses, parameters, switches, music and streamed
  segments, clustering, voice policy, offline inspection/rendering, WAV/Ogg and
  PS-ADPCM assets, WASAPI, and SDL3 output on Linux and other non-Windows
  platforms.
- **Integration:** CMake consumer support with namespaced `Sutekh::*` targets,
  public-header isolation, optional dependency bootstrapping, sanitizer builds,
  cross-compiler determinism checks, and CI validation.

Recorded validation includes the full current existing suite at 55/55,
Windows Clang validation at 92 passed with 2 POSIX-only skips, model comparisons,
hidden fixture experiments, replay checks, sanitizer checks, and a checked-mode
million-row load/capture/recovery run. These are correctness results and
engineering evidence, not a claim of production throughput or a performance
qualification.

M3 is still in progress. Remaining work includes final evidence/inventory
reconciliation, stress and optimization gates, and independent review of a
small number of native/test coverage gaps. Original BFME compatibility has not
been run. A full editor and a qualified Windows-native runtime are not yet
shipped.

## Build and test

On Linux or WSL, from the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 2
ctest --test-dir build --output-on-failure
```

For the broader package and documentation checks:

```sh
python3 tools/validate_pack.py
python3 tools/check_repository_license.py
```

The default build includes the checked reference VM and the repository tests.
Native specialization, Vulkan, SDL3, and asset dependencies are optional and
have separate bootstrap and qualification requirements. See
[BUILD_AND_VALIDATION.md](docs/BUILD_AND_VALIDATION.md).

## Use from another CMake project

Sutekh can be included with `add_subdirectory` and consumed through namespaced
targets:

```cmake
add_subdirectory(extern/sutekh)
target_link_libraries(game PRIVATE Sutekh::host)

if(TARGET Sutekh::render)
    target_link_libraries(game PRIVATE Sutekh::render)
endif()
```

The public API is under `include/sutekh/`. Available interfaces cover the host
runtime, entities, IR, replay, input, windowing, rendering, assets, audio, and
platform clocks. Engine-internal directories remain private to consumers.

## Documentation

- [Implementation status](docs/IMPLEMENTATION_STATUS.md) — dated feature and
  validation record.
- [Architecture](ARCHITECTURE.md) — runtime tiers, processes, and shipping
  model.
- [Live IR specification](LIVE_IR_SPEC.md) — checked gameplay representation.
- [Agent protocol](AGENT_PROTOCOL.md) — parallel work, verification, and
  recovery.
- [Build and validation](docs/BUILD_AND_VALIDATION.md) — supported build lanes
  and evidence rules.
- [Rendering and assets](docs/RENDERING_AND_ASSETS.md) — content and GPU path.
- [Event audio](docs/EVENT_AUDIO_INTEGRATION.md) — integrated audio runtime.
- [Multi-table worlds](docs/MULTI_TABLE_WORLDS.md) — world/table model.
- [Determinism and BFME](docs/DETERMINISM_AND_BFME.md) — compatibility scope
  and determinism requirements.
- [Security](SECURITY.md) — reporting and security boundaries.

The combined [SUTEKH_FULL_SPEC.md](SUTEKH_FULL_SPEC.md) and
[SUTEKH_ALL_PROMPTS.md](SUTEKH_ALL_PROMPTS.md) files are generated reading
copies; the individual specifications remain authoritative.

## License

Sutekh is **source-available, not OSI Open Source**, under the
[PolyForm Perimeter License 1.0.1](LICENSE). Normal use to build games and
applications, including commercial games, is subject to the actual license
terms. Competitive engine/platform redistribution and some OEM uses may require
separate rights; see [COMMERCIAL_LICENSE.md](COMMERCIAL_LICENSE.md).

Read [NOTICE.md](NOTICE.md) for attribution, historical licensing decisions,
third-party notices, and trademark boundaries. Contributors retain copyright
under the explicitly accepted [CLA](CLA.md); see [CONTRIBUTING.md](CONTRIBUTING.md).
