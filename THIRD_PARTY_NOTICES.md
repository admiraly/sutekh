# Third-party dependencies, tooling, and assets

<!-- SPDX-FileCopyrightText: 2026 admiraly -->
<!-- SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 -->

The Sutekh community license applies to project-authored material, not to
third-party components. No third-party engine library or proprietary game
asset is currently vendored in this checkout. Installed tooling in `.venv`
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
