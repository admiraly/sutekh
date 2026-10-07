> Historical record. Current licensing is governed by [LICENSE](../../LICENSE) and [NOTICE.md](../../NOTICE.md); prior decisions below are not current grants.

# Dependencies and Windows toolchain — 7 October 2026

Owner authorized required dependency installation. Installed project-local
LLVM 23.1.3 (Clang, clang-cl, LLD) and a Python virtual environment with the
exact packages in `tools/requirements-dev.txt`. Reused installed MSVC
14.29.30133 libraries/headers and Windows SDK 10.0.19041.0. No global PATH,
administrator installation, or engine license decision was needed.

LLVM was downloaded with:

```powershell
winget download --id LLVM.LLVM --exact --version 23.1.3 --download-directory .\local\downloads\llvm --accept-source-agreements --accept-package-agreements --disable-interactivity
```

Winget verified the installer SHA-256. A Windows Installer administrative
extraction (`msiexec /a`, quiet mode, project-local `TARGETDIR`) exited 0.
Compiler/linker versions and artifact hashes are recorded in
`planning/toolchain-windows.json`. Upstream license: Apache-2.0 with LLVM
exceptions, supplied in the extracted distribution. Python package licenses
are preserved in the virtual environment's distribution metadata.

`tools/env-windows.ps1` sets PATH, INCLUDE, LIB, and CC for the current shell.
After dot-sourcing it, a local CMake/Ninja project configured with Clang
23.1.3, compiled as C17 with `-Wall -Wextra -Werror`, linked with LLD, and
ran successfully. It checked four-byte uint32_t and unsigned wraparound.
Commands:

```powershell
. .\tools\env-windows.ps1
cmake -S local/toolchain-smoke -B local/toolchain-smoke/build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld
cmake --build local/toolchain-smoke/build --verbose
.\local\toolchain-smoke\build\smoke.exe
```

All three commands exited 0; execution printed `C17 toolchain smoke passed`.
This verifies dependency readiness, not native engine test T00.

Pack validation now passes all six JSON Schemas and seven schema instances,
as well as the existing fixtures, rejection checks, links, and task DAG.
`PACK_VALIDATION.json` contains the current results. Package file discovery
now excludes Git metadata, virtual environments, local content, secrets,
symlinks, and build outputs; the assembler uses the same file discovery.
This resolves the archive warning in the earlier setup handoff.

Native engine tests, engine performance, and original BFME compatibility
remain not run. No engine task was marked complete. M0-00 is still next;
yyjson and a reviewed native hashing implementation must be pinned when
the native build is created under M0-01.
