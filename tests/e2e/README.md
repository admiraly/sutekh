# M0 end-to-end checks: zero-wait live edit and the T12 no-build probe

Everything here drives the real `build/sutekh` executable over its JSON-lines control
protocol. Standard library Python only. Run every command from the repository root.

## 1. Build the native engine (once, or after native source changes)

```sh
python3 tools/dev.py build
```

Expected: a CMake configure line, a Ninja build, and the last line
`[41/41] Linking C executable test_reload` (the count varies with what is stale).
Exit status 0. Gameplay capsule edits never need this step.

## 2. Live-edit demonstration

```sh
python3 tools/demo_zero_wait.py
```

It starts `build/sutekh serve --root <repo>`, creates a world from `examples/world.json` and
`examples/movement.capsule.json`, steps with v1 (position += velocity), writes a v2 capsule
(position += 2 * velocity) into a scratch copy under `build/demo-scratch/` (removed at exit
unless `--keep-scratch`), submits and evaluates it, steps to the planned effective tick E,
installs at E, steps on under v2, submits a capsule that writes a read-only field (rejected
with `SU_E_UNDECLARED_WRITE`), and runs a snapshot / two steps / restore / one step round trip.

Every request and reply is printed with a host monotonic timestamp (ms since start). Each
expected value is computed with `tests/harness/reference_eval.py`, never taken from the engine.
Any difference stops the run with exit status 1.

Options: `--ticks-before N` (default 3), `--ticks-after M` (default 3), `--out PATH`
(JSON evidence; default `~/.cache/sutekh/demo_zero_wait/<UTC>-<pid>.json`, outside the repo),
`--keep-scratch`, `--binary PATH`.

Expected output (abridged from a real run; the timestamps, PIDs and paths differ per run):

```
== phase create_world
    check ok   tick 0: ...
== phase install
    check ok   install state: expected 'installed' actual 'installed'
    check ok   install effective tick: expected 4 actual 4
    check ok   worker_pid unchanged: expected 3945 actual 3945
    check ok   worker process start time unchanged (same process, not a recycled pid): ...
== phase invalid_edit_rejected
    check ok   invalid capsule refused with SU_E_UNDECLARED_WRITE: ...
    check ok   world revision unchanged: ...
== phase snapshot_restore
    check ok   restored canonical state equals snapshot state byte-for-byte: ...
status: passed
```

The step table shows the committed columns after each row. With the default parameters the
first v2 row is tick 5 (`position_x` [6, 13, 5, 7] from [4, 9, 3, 7] at E = 4): the install
takes effect after tick E, and the same worker process keeps running.

## 3. T12 probe: no compiler, assembler, linker or build tool during the edit

```sh
python3 -I tests/e2e/no_build_probe.py
```

Method: `strace -f -ttt -e trace=execve,execveat -s 4096` wraps the whole demo, including the
supervisor's worker children. A successful exec of a build tool (cc, gcc, clang, as, ld, lld,
cmake, ninja, make and versioned names) is a violation. Violations are counted both in the
edit window (candidate submit through install, from the demo's evidence) and in the whole run.

Before the result is trusted, two positive controls run under the same detector: `cc --version`
resolved through PATH, and `<absolute path to cc> --version`. The second matters because a PATH
shim cannot see an absolute-path exec (the M13 mutant). If either control is not detected, the
verdict is `inconclusive` and the exit status is 2. With no compiler installed, a stub `cc` is used.

The T12 harness test (`tests/harness/run_m0.py`) uses this probe as its primary evidence and runs
it against the binary under test (`--binary PATH` is passed to the demo). An `inconclusive` probe
makes T12 `not_run`, never `passed`. Under AddressSanitizer the probe is `inconclusive`: the
ASan `serve` process exits 1 at shutdown when LeakSanitizer runs under ptrace (observed on this
host), so use the plain build for this probe.

Static scan: the linked binary's printable strings and dynamic imports, plus `src/`, are
checked for build-tool names and for `system`/`popen`/`exec*`/`fork` call sites.

Requires `strace` (`apt install strace` on Debian/Ubuntu). No fallback is implemented; without
strace the probe reports `inconclusive`.

Exit status: 0 pass; 1 a build tool was executed by the traced tree; 2 inconclusive.
Evidence JSON defaults to `~/.cache/sutekh/no_build_probe/<UTC>-<pid>.json`.

Expected output (from a real run):

```
positive control (real cc --version): detected=True
traced exec attempts: 43 (successful 7); executed basenames: git, python3, sutekh, uname
edit window 56.367 ms: build-tool execs in window = 0
build-tool execs in whole run = 0
verdict: pass (positive control detected; zero build-tool execs in the whole traced run, including the capsule edit/install window)
```

Limits: the probe sees exec-family syscalls only. It cannot see code run in-process without an
exec, so a source review and the static scan cover that part. `strace` slows the traced tree,
so use the timings from `demo_zero_wait.py` (untraced), not the traced run.

## 4. M0 acceptance harness (T00 to T15)

```sh
python3 -I tests/harness/run_m0.py
```

Add `--no-build` to skip T00's build command. Exit status 0 when no test failed. The report is
written to `build/m0_harness/`. Expected: `unavailable` results are reported with the engine's
own reply and are not counted as passes.

Expected output (from a real run at this commit):

```
M0 harness 43f3e71e1334 against build/sutekh (sha256 790708a46e7864ba6728fd75f48e2a2621f4f51ab56138f9d6c4f2ee374a8421)
T00  passed       Clean bootstrap
T04  unavailable  Invalid input matrix
       7 of 14 negative cases cannot be expressed: ...
T12  passed       No native gameplay build
T15  passed       Structured protocol
totals: passed=15, failed=0, not_run=0, unavailable=1
```
