# A02 integration checkpoint

Date: 2026-10-20. Based on A01 commit `bb2850f`.
Changed paths: `include/sutekh/audio_spatial.h`,
`src/audio/su_audio_spatial.c`, `tests/audio/test_audio_spatial.c`,
`docs/EVENT_AUDIO_SPATIAL.md`, `cmake/SutekhPresentation.cmake`,
`planning/leases/event-audio-foundation.json`, this handoff and the generated
pack report if its counts change.

The additive API supplies bounded scene-local emitter generations, copied
curves, camera listener snapshots and a versioned voice descriptor extension.
Scale affects distance once, with explicit non-recursive curve-scale lookup;
it is not an extra amplitude multiplier. No legacy audio ABI layout changes.
HPF/spread are evaluated metadata, not newly claimed legacy DSP.

Exact source/dependency and executable SHA-256 values and direct commands are
recorded in `docs/EVENT_AUDIO_SPATIAL.md`. Default and optimized strict Clang
component checks: spatial 4791, legacy mixer 236 and low-pass 865040, all zero
failures. The old mixer count is from that component's dependency snapshot,
not a claim about the later Ogg-enabled integration count. C++17 header check
passes. x64 ABI: emitter 8, curve ID 4, legacy voice descriptor 136 unchanged,
V2 descriptor 160, emitter snapshot 48 and camera listener 56 bytes.

The staged A02 snapshot is exported to `build-pack` and checked with:

```powershell
cmake --build build-a01-accepted --target test_audio_spatial test_audio_asset test_audio_callback test_audio test_audio_stream test_audio_lowpass test_audio_adpcm test_audio_device -j 4
ctest --test-dir build-a01-accepted -R '^audio_' --output-on-failure
python build-pack/tools/validate_pack.py --write-report
```

This reuses the isolated A01 snapshot build configuration; CMake regenerates
against the updated staged sources. Eight audio tests pass, including device
coverage. The cumulative A01-A04 integration also passes eleven audio CTests
under Clang Debug, Clang Release/Ogg and MSVC 19.44 `/W4 /WX`; cross-platform CI
remains a PR gate. No performance/latency qualification is claimed.

Next: A03 strict immutable graph preparation. A04 owns automatic live attachment
and private mixer attenuation updates; direct legacy users resolve and forward
presentation snapshots themselves. A05-A10 write scopes remain in the shared
design note. No unresolved component blocker is recorded.
