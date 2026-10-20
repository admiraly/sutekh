# A07 scoped parameters, switches and music checkpoint

Date: 2026-10-20. Builds on main `12b02d8` (A01-A04) without changing any
shared header, schema or event runtime source. Changed paths:
`include/sutekh/audio_parameter.h`, `src/audio/su_audio_parameter.c`,
`src/audio/su_audio_music.c`, `tests/audio/test_audio_parameter.c`,
`docs/EVENT_AUDIO_PARAMETERS.md`, an additive `su_audio_parameter` library and
`audio_parameter` test registration in `cmake/SutekhPresentation.cmake` and this
handoff. `tools/validate_pack.py --write-report` left the pack report unchanged.

Delivered: typed global/emitter/instance parameters with clamp/reject policies,
linear smoothing and bounded timestamped segments; switches with first-only,
continue and fade policies; graph expansion snapshots for switch and blend
nodes; parameter-to-control bindings (gain, pitch, low-pass, bus gain) with a
reference voice; interactive music with integer tempo maps, beat/bar/cue/end
boundaries, late policies, sample-exact crossfades, synchronized stems and
stingers.

Local evidence: `test_audio_parameter` passes 1380 checks with strict Clang
23.1.3 Debug; the three sources compile cleanly under MSVC 19.44 `/W4 /WX`
and Clang `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`. GCC and Linux
results come from PR CI. Costs and exact behavior are in the component note.

Open: the event runtime does not yet consume parameter snapshots or bindings;
that needs a coordinated, versioned runtime change. Bus gain awaits A06.
