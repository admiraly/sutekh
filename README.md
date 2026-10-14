# SUTEKH — Agent-native game execution engine

<!-- SPDX-FileCopyrightText: 2026 admiraly -->
<!-- SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 -->

**An agent-native game execution engine — source-available under PolyForm Perimeter 1.0.1.**

[Specifications](#start-here) · [Contributing](CONTRIBUTING.md) · [Security](SECURITY.md) · [Commercial/OEM licensing](COMMERCIAL_LICENSE.md)

Specification v0.1 • 6 October 2026

**Objective:** maximize verified game functionality delivered per agent-hour, while retaining a path to workload-specific, near-hardware-limit execution. Ordinary gameplay edits must not trigger native project builds, link steps, or executable restarts.

**Status:** Sutekh has an implemented headless C17 runtime, checked JSON gameplay IR, a local structured control plane, worker recovery, generational entities, and a six-system RTS prototype. M0–M2 are owner-accepted with documented caveats; M3 is in progress. There is no full editor, renderer, or Windows-native runtime. Timing observations are not performance qualification, and BFME compatibility has not been established. See [implementation status](docs/IMPLEMENTATION_STATUS.md) and [durable task state](planning/status.json). “Sutekh” remains a working name, not a cleared product name. The owner-authorized [runtime limits v2](planning/handoffs/2026-10-14-m3-limits-v2.md) has validated a million-row load, canonical capture and replacement-worker recovery; this is a correctness result, not a throughput guarantee.

### Start here

On Linux or WSL, build and run the regression suite from the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 2
ctest --test-dir build --output-on-failure
```

The default lane uses the checked reference VM. Native specialization is optional and has separate qualification requirements. Keep this engine checkout separate from existing game projects. The root specifications below remain authoritative for continued development.

The orchestrator should read `AGENTS.md`, the five root specifications below, and `planning/tasks.json`. It should implement the smallest live, headless loop before extending the architecture. Individual workers receive only their task packet, relevant contracts, and the corresponding role prompt—not this entire package on every task.

| Root document | Authority |
|---|---|
| [ENGINE_CONSTITUTION.md](ENGINE_CONSTITUTION.md) | Non-negotiable behavior and scope boundaries |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Processes, runtime tiers, subsystems, shipping model |
| [LIVE_IR_SPEC.md](LIVE_IR_SPEC.md) | Implementable M0 language and later extension rules |
| [AGENT_PROTOCOL.md](AGENT_PROTOCOL.md) | Parallel work, verification, integration, recovery |
| [MILESTONES.md](MILESTONES.md) | Dependency-ordered delivery and acceptance gates |

The `docs/` directory specifies state migration, determinism, storage/ABI, RPC, builds, performance methodology, rendering, security risks, game profiles, dependency policy, and tests. `schemas/` contains machine-readable structures. `examples/` contains a valid movement capsule, fixtures, negative examples, and an explicitly unmeasured report. `planning/` contains the task graph, current status, audits, and reproducible validation evidence.

### Single-file reading copies

[SUTEKH_FULL_SPEC.md](SUTEKH_FULL_SPEC.md) combines the 18 engineering specifications. [SUTEKH_ALL_PROMPTS.md](SUTEKH_ALL_PROMPTS.md) combines all 11 prompts. Individual documents remain authoritative; these copies are generated conveniences, not separate specifications.

### Initial architecture in one paragraph

A small **C17 resident runtime**, with a stable C ABI and optional measured assembly kernels, hosts bounded, typed Live IR. M0 accepts a canonical JSON representation rather than inventing a surface language. A supervisor handles projects, artifacts, and structured agent requests. Isolated simulation workers execute interpreted capsules and preserve state across ordinary code edits. Candidate workers validate proposed revisions. Later native backends compile individual capsules away from the live tick; known-good code stays active until a compatible candidate is ready. The shipping player excludes the development control plane.

### The three development paths

| Change | Required path |
|---|---|
| Gameplay/data capsule | Validate → isolated preview → tick-boundary installation; no native project build |
| Native core/assembly/backend | Targeted compile and tests → integration artifact → controlled worker replacement where required |
| Release, compiler upgrade, broad ABI change | Full relevant matrix in the build lane, not a barrier imposed on every author |

“No build” means **no native project build for supported gameplay edits**. Parsing, semantic checks, bytecode preparation, GPU driver compilation, and expensive data migration still consume time. The engine schedules and bounds that work rather than pretending it disappears.

### First proof

Run a 1,024-row integer movement simulation. Change its capsule, observe the new behavior at a tick boundary, reject an invalid revision without changing active state, and restore a snapshot. The supervisor and active worker must keep their PIDs during ordinary valid code-only replacement. After correctness, measure reload latency; then test 100,000 and 1,000,000 rows separately as stress workloads. Do not make million-entity throughput a prerequisite for the first working loop.

### Prompts

The master prompt coordinates all work. Specialized prompts cover bootstrap, capsule implementation, independent verification/integration, native backend work, BFME compatibility, continuation/recovery, performance optimization, rendering/assets, build brokerage, and adversarial review. They explicitly prohibit fabricated results, endless planning, blanket test skipping, unbounded unverified code, and uncontrolled shared-checkout edits.

### Pack checks

```sh
python3 tools/validate_pack.py
```

The checker requires Python 3.9 or later and uses Python's standard library. When `jsonschema` is installed it also performs Draft 2020-12 validation; otherwise it reports that specific layer as skipped. The core package checks and executable-example checks still run. No C/ASM compilation is performed by this checker.

### Local Windows development setup

The public remote is https://github.com/admiraly/sutekh. The engine remains
unimplemented; dependency setup is not M0 acceptance.

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r tools/requirements-dev.txt
. .\tools\env-windows.ps1
python tools/validate_pack.py
```

`env-windows.ps1` enables the project-local LLVM 23.1.3 toolchain and discovers
installed MSVC headers/libraries and the Windows SDK for x64 compilation.
LLVM is extracted under `local/toolchains/llvm-23.1.3/LLVM`; `.venv` and
`local` are ignored by Git and excluded from handoff archives. Exact installed
tool versions and the verified LLVM installer checksum are recorded in
`planning/toolchain-windows.json`. Setup evidence is in
[the dependency handoff](planning/handoffs/2026-10-07-dependencies.md).

### Licensing and commercial use

Copyright (c) 2026 **admiraly**, the current project owner. Sutekh is
**source-available, not OSI Open Source**, under the
[PolyForm Perimeter License 1.0.1](LICENSE).

Normal use of Sutekh to build games and applications, including commercial
games, is permitted subject to the actual license terms. Providing others a
product that competes with Sutekh—such as a competing engine or platform—is
excluded by the Noncompete and Competition provisions. Distribution, changes,
and notices must comply with the license. This summary grants no additional
rights and does not guarantee that every game, service, or integration qualifies.

Separate [commercial/OEM licenses](COMMERCIAL_LICENSE.md) may be available for
uses requiring rights beyond the community license, especially competitive
engine/platform redistribution and OEM arrangements requiring excluded rights.
No pricing, sales address, or automatic commercial entitlement is published.

Read [NOTICE.md](NOTICE.md) for attribution, historical releases, trademark
boundaries, and licensing scope. The Sutekh name and any logo are not
automatically licensed by the source license; no registered trademark status
is claimed. [Third-party material](THIRD_PARTY_NOTICES.md) retains its original
licenses and notices. User-created game/application content remains separate.

Contributors retain copyright while granting the owner sufficient rights for
commercial relicensing and dual licensing through an explicitly accepted
[CLA](CLA.md). Follow [CONTRIBUTING.md](CONTRIBUTING.md); PR submission alone
is not acceptance. Earlier MIT-licensed revisions and dated specification
license-decision entries are historical, not a competing current grant; rights
already granted in earlier releases are not withdrawn by this change.

The engineering specifications are preserved. Current licensing metadata is
set by `LICENSE` and `NOTICE.md`, superseding archived license-decision entries.
Final branding and paid compute remain owner decisions. Engine implementation
and dependency admission continue through the existing milestone gates.

### Rebuild the handoff archive

```sh
python3 tools/assemble_handoff.py
```

This regenerates the single-file copies, runs the pack checks, and writes a ZIP beside the pack directory with a file index and SHA-256 checksums. It does not compile or implement the native engine.
