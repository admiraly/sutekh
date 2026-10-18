# Third-party dependencies, tooling, and assets

<!-- SPDX-FileCopyrightText: 2026 admiraly -->
<!-- SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 -->

The Sutekh community license applies to project-authored material, not to
third-party components. No third-party engine library or proprietary game
asset is currently vendored in this checkout (the renderer's Vulkan headers are
fetched at bootstrap like AsmJit; its SPIR-V is generated from project-authored shaders;
the asset importer's cgltf and stb files are fetched at bootstrap, pinned by SHA-256). Installed tooling in `.venv`
and `local/` is excluded from Git and handoff archives. References to libraries
in the specification are planned dependencies, not evidence of bundled code.

## Current development environment

| Component | Recorded version | Original license / notice location |
| --- | --- | --- |
| LLVM / Clang / LLD | 23.1.3 | Apache-2.0 WITH LLVM-exception; upstream license and component notices in the downloaded distribution |
| CMake Python distribution | 4.3.4 | Apache-2.0 packaging; bundled CMake BSD-3-Clause and component-specific licenses in `cmake/data/doc/cmake` and distribution metadata |
| Ninja Python distribution | 1.13.0 | Apache-2.0; license and author notices in distribution metadata |
| attrs | 26.1.0 | MIT; distribution `licenses/LICENSE` |
| jsonschema | 4.26.0 | MIT; distribution `licenses/COPYING` |
| jsonschema-specifications | 2025.9.1 | MIT; distribution `licenses/COPYING` |
| referencing | 0.37.0 | MIT; distribution `licenses/COPYING` |
| rpds-py | 2026.9.1 | MIT; distribution `licenses/LICENSE` |
| Python interpreter | 3.14.7 | Python and bundled-component licenses in the installed interpreter distribution |
| MSVC headers/libraries and Windows SDK | 14.29.30133 / 10.0.19041.0 | Microsoft's applicable product and redistribution terms; not licensed by Sutekh |
| Khronos Vulkan-Headers (build-time headers of the optional renderer `su_render`, fetched at bootstrap, not redistributed in this repository) | tag `v1.4.365`, commit `c46850864f4661461b0f6cb9922c058ffea4915e`, from https://github.com/KhronosGroup/Vulkan-Headers.git | Apache-2.0 OR MIT, Copyright The Khronos Group Inc.; LICENSE.md sha256 `a2d4407962c909f84807774cbfe9fad05a1ebe972472c08ed5ad43d482afd288`; recorded in [third_party/vulkan-headers.lock.json](third_party/vulkan-headers.lock.json) by `tools/bootstrap_vulkan_headers.py` |
| glslang (offline shader compiler; tool only, not linked or redistributed) | 16.6.0 Windows release `glslang-16.6.0-windows-x86_64-release.zip`, sha256 `82bf434e69b9bb4829de7e2b4bc2c5e7a7861e53d66cf75e5cc70f5f694a8d9b` | licenses as listed in upstream https://github.com/KhronosGroup/glslang/blob/16.6.0/LICENSE.txt (the release archive carries no license file); it compiled the project-authored shaders in `src/render/shaders` into the generated SPIR-V of `src/render/su_render_shaders.h` (`tools/compile_shaders.py`) |
| Khronos Vulkan-ValidationLayers (development tool only, built locally outside the repository, not linked or redistributed) | tag `vulkan-sdk-1.4.363.0`, commit `15c64e73db67e27ff0215708fbe48684a35c6827`, from https://github.com/KhronosGroup/Vulkan-ValidationLayers.git | Apache-2.0 for most files, some files under more permissive licenses (upstream LICENSE.txt); loaded at run time through `VK_ADD_LAYER_PATH` for validation runs |
| Vulkan loader (`vulkan-1.dll`) | provided by the installed GPU driver (observed: loader 1.4.309, NVIDIA 582.16) | opened at run time; not linked or redistributed |
| cgltf (glTF 2.0 parser of the optional asset importer `su_asset_import`, fetched at bootstrap, not redistributed in this repository) | 1.15, commit `360db1a95480fe102ae9c69b27c5d101167ff5ba`, from https://github.com/jkuhlmann/cgltf; `cgltf.h` sha256 `e378a21c084bf1f288bb799de827bb26906efb024255f1ecf1705ea13f11c6ec` | MIT, Copyright (c) 2018-2021 Johannes Kuhlmann; LICENSE sha256 `f619925f80ef862497aaf8e8155ef218fa6a2190055129523ca3df9119a9ba95`; pinned and verified by `tools/bootstrap_asset_deps.py`, recorded in [third_party/asset-deps.lock.json](third_party/asset-deps.lock.json) |
| stb_image 2.30 and stb_vorbis 1.22 (PNG/JPEG decoding in the asset importer; Ogg Vorbis decoding in the audio module; fetched at bootstrap, not redistributed) | commit `2c980bb59875b0d32144a71867fbdebb2f77cd20` of https://github.com/nothings/stb; `stb_image.h` sha256 `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3`, `stb_vorbis.c` sha256 `4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636` | MIT (Copyright (c) 2017 Sean Barrett) or public domain (Unlicense), at the user's choice; LICENSE sha256 `bebfe904b14301657e4e5d655c811d51fd31b97c455b9cc2d8600d6bac6cff63`; same bootstrap tool and lock file |
| python-soundfile 0.14.0 with its bundled libsndfile 1.2.2 (tool only: encoded the synthetic test signal `tests/fixtures/audio/tone440_stereo.ogg` computed by `tools/gen_audio_fixture.py`; installed in a throwaway environment outside the repository, not a project dependency, not redistributed) | 0.14.0 / 1.2.2 from PyPI | python-soundfile: BSD-3-Clause; libsndfile: LGPL-2.1-or-later (and the codecs it bundles under their own licenses); the fixture's signal is project-generated |
| AsmJit (optional native backend, fetched at bootstrap, not redistributed in this repository) | commit `0d7f01052f2d3f6d6b9bb6714ef32bd7b80177eb` (library 1.23.0) from https://github.com/asmjit/asmjit.git | zlib License, Copyright (c) 2008-2025 Petr Kobalicek; LICENSE.md sha256 `13870bd53bce53fb9a9bd72e72dcceeffc547c84d9a00d3d4833737a79651828`; recorded in [third_party/asmjit.lock.json](third_party/asmjit.lock.json) |

Versions are recorded in [tools/requirements-dev.txt](tools/requirements-dev.txt)
and [planning/toolchain-windows.json](planning/toolchain-windows.json).
This table is an inventory, not a replacement for original license texts or
a grant to redistribute tooling. Preserve the complete original notices and
check component-specific redistribution terms before shipping any dependency.
The PolyForm license text in `LICENSE` is reproduced from the
[upstream PolyForm project](https://github.com/polyformproject/polyform-licenses/blob/76a278c402bc43b8d2b561da140b0f3e17263015/PolyForm-Perimeter-1.0.1.md)
without modifying its terms.

## Adding or distributing material

Record each dependency's source, exact revision, checksum, copyright holder,
license, and modifications. Keep original license files and headers next to
vendored code, and include required notices in packages. Do not apply Sutekh's
SPDX header to third-party files or imply that their rights are owned by the
project. Acceptance under the CLA does not override third-party restrictions.

Repository examples are synthetic project material under the community license.
Original BFME archives, models, textures, audio, and installation data are not
included and acquire no rights from the Sutekh license. Future external assets
must have their own provenance and redistribution permission recorded before
inclusion. Game and application content authored by users remains separate
from the engine's licensing obligations.
