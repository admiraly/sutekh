# A01 integration checkpoint

Base: `7e3c6ebda67f236fc1ccfee22d5e0b048b190ba6`. Date: 2026-10-20.
The owner authorized the public event-audio foundation and ordered PR merges.
Issue bodies #87 through #97 were rewritten in generic terminology while
retaining the technical proposal. The shared design and future write scopes are
in `docs/EVENT_AUDIO.md`; no engine milestone closure is implied.

## Exact patch paths

`include/sutekh/audio_asset.h`, `include/sutekh/audio_stream_async.h`,
`src/audio/su_audio_asset.c`, `src/audio/su_audio_stream.c`,
`src/audio/su_audio_int.h`, `src/audio/su_audio_mix.c` (sample-miss hooks only),
`tests/audio/test_audio_asset.c`, `cmake/SutekhPresentation.cmake`,
`docs/EVENT_AUDIO.md`, `docs/EVENT_AUDIO_ASSETS.md`,
`planning/leases/event-audio-foundation.json`, this handoff, `PACK_VALIDATION.json`.

## Evidence

Windows 11 x64, Intel Core i5-12400F, Clang 23.1.3. The exact staged A01
snapshot was exported with `git checkout-index --all --force --prefix=build-pack/`
to avoid validating later uncommitted layers as A01. From the isolated worktree,
after `. ./tools/env-windows.ps1`:

```powershell
cmake -S build-pack -B build-a01-accepted -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug -DSUTEKH_NATIVE=OFF -DSUTEKH_RENDER=OFF -DSUTEKH_ASSET_IMPORT=OFF
cmake --build build-a01-accepted --target test_audio_asset test_audio_callback test_audio test_audio_stream test_audio_lowpass test_audio_adpcm test_audio_device -j 4
ctest --test-dir build-a01-accepted -R '^audio_' --output-on-failure
python build-pack/tools/validate_pack.py --write-report
```

Configure/build/test returned zero: seven audio tests passed, including actual
WASAPI device coverage on this machine and instrumented callback validation.
The report is generated from the staged snapshot, then copied to the worktree.
The final report includes this handoff as well; Markdown without links does not
change report counts. This is not evidence for an async device stress workload.

The cumulative in-progress integration additionally passed all eleven audio
tests under Clang Debug, Clang Release with pinned Ogg enabled, and MSVC 19.44
`/W4 /WX`. Those runs include later A02-A04 files and are not an A01-only proof.
An earlier full-suite run had 109 completed/passing-or-skipped tests and one
audio assertion failure while the output-underrun hook was still incomplete.
The exact staged rerun above includes that repair. Full cross-platform CI is a
pending integration gate, not a local result.

Staged Git object IDs:

| Path | Git object |
| --- | --- |
| `include/sutekh/audio_asset.h` | `2b73b60118f47994d84579eb0058c3b4e956e9f3` |
| `include/sutekh/audio_stream_async.h` | `81acc3c150165181de3a6f8c4f04d7b12940d4f0` |
| `src/audio/su_audio_asset.c` | `6faded45005c8ef1c69e53b4207392419cf44b56` |
| `src/audio/su_audio_stream.c` | `fee9a71fdf78a20e20a780c968e512742e353ead` |
| `src/audio/su_audio_mix.c` | `b932ff5120ba11a3c8adba27cb6c0cdb8476c39d` |

`build-a01-accepted/test_audio_callback.exe` SHA-256:
`56428701AF54E9CEF962ED5ECE8156E4440F3F46A43AABE312AA01325301F216`.
Component source SHA-256 and direct-test evidence are in the asset note.

## Observations and next work

Mono async ring: 8192 bytes; stream resident payload: 12680 bytes. Stereo Ogg
stream resident payload: 223800 bytes. OS worker stack and allocator overhead
are excluded. The instrumented callback reports zero allocation/free, file,
mutex/condition-wait or sleep operations, with a legacy synchronous positive
control that detects file calls. A paused worker proves whole-output-frame
silence on a missing interpolation partner. Cursor time advances on underrun.

Remaining: cross-platform CI, longer concurrent/thread-failure stress, and
owner-device async profiling. No latency distribution is claimed. Next ready
step is the additive spatial component A02, then bounded graph A03 and event
runtime A04. Later components do not edit the shared header set without the
integrator's coordination.

## Review repair and main integration

An independent review reproduced primary-frame starvation at interpolation
window boundaries. The first guarded source fetch now latches primary demand
through both interpolation reads; neither adjacent nor loop-head partners can
overwrite it. The regression failed 25 checks before repair. Final direct
strict Clang evidence: 33153 checks without Ogg, 77286 with Ogg, and 77297
instrumented checks across five runs, all zero failures. Instrumentation still
records zero callback side effects. Updated source/artifact hashes are in the
asset note; the earlier table above identifies the original staged checkpoint.

Main advanced to `9971957` during CI. Both validate jobs correctly rejected the
stale pack report on the combined tree. Main was merged normally, without a
rebase or force-push, and the report regenerated from the exact A01 snapshot.
Incoming render files are upstream integration, not audio-authored changes.
