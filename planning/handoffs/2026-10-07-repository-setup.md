> Historical record. Current licensing is governed by [LICENSE](../../LICENSE) and [NOTICE.md](../../NOTICE.md); prior decisions below are not current grants.

# Repository setup and orientation — 7 October 2026

Created the private GitHub repository https://github.com/admiraly/sutekh and
connected this checkout through `origin`, with `main` as the initial branch.
Moved the supplied specification package contents to the checkout root, as
directed by its README. Added `.gitignore`, recorded the remote in
`planning/status.json`, and refreshed the package inventory and checksums.
No engine implementation task has started; all 24 tasks remain planned.
The engine license remains an owner decision.

Read all authoritative root specifications, the 12 subsystem documents,
the task graph and status, all 11 role prompts, and the example guide.
The first deliverable is M0: fixed SoA u32 state, strict JSON SU-LIR 0.1,
a checked C17 VM, local structured control, isolated candidate evaluation,
same-worker tick-boundary replacement, and canonical snapshot restore.
Native acceleration follows M0; dynamic state, RTS/BFME, and rendering are
later gates. Timing budgets are targets and have no engine measurements yet.

## Evidence

`python tools/validate_pack.py` passed on Python 3.14.7: strict JSON,
24-node task DAG, 10 positive fixtures, 7 invalid-IR rejection cases,
4 additional opcode known answers, and 45 internal Markdown links.
Optional Draft 2020-12 schema validation was skipped because `jsonschema`
is not installed. Final check output is stored in `PACK_VALIDATION.json`.
Source file SHA-256 hashes are stored in `SHA256SUMS` and `PACK_INDEX.json`.

Git 2.55.0.windows.3 and authenticated GitHub CLI are available. CMake and
Ninja resolve on PATH. `Get-Command clang,clang-cl,lld-link` found no PATH
entries; compiler/SDK discovery remains part of M0-01. This was an initial
PATH check, not a complete installed-toolchain inventory.

Native compilation, runtime tests T00–T15, engine benchmarks, and original
BFME compatibility checks were not run: this checkout contains specs and a
Python example evaluator, with no native engine source or build graph.

## Next work

Execute M0-00 to freeze only the required ABI, lifetimes, error codes,
numeric/effect rules, and fixtures. Then perform M0-01 toolchain/dependency
inventory and build the C17 skeleton before proceeding through the M0 DAG.

Before using `tools/assemble_handoff.py` in this Git checkout, exclude
`.git` and local/build artifacts from its recursive inventory and ZIP.
The supplied script was written for a standalone pack and currently walks
every file under its root. It was not run during repository setup.
