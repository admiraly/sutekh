# Physical voices and virtual logical playback (A05)

`sutekh/audio_voice_policy.h` adds a bounded logical voice pool whose members
compete for a smaller set of physical (rendered) slots. It is a separate,
single-owner presentation component: every call, including rendering, is
serialized by its owner. The legacy `su_audio` mixer and its
`SuAudioVoiceDesc.priority` stealing are unchanged; legacy applications never
see this policy. Attaching the pool to the A04 event runtime is an integrator
change to shared runtime code and is not done here; the descriptor already
carries the A04 handoff identifiers (event, graph leaf node, bus, emitter,
instance and trigger sequence).

## API

| Call | Purpose |
| --- | --- |
| `su_audio_voice_pool_create/destroy` | Bounded pool: `max_logical` (<= 65536), `max_physical`, `hero_reserve`, `transition_reserve`, `audibility_floor`. All storage is allocated here. |
| `su_audio_voice_add` | Admits a logical voice; it is ranked at the next 128-frame boundary. A full pool returns `SU_E_RESOURCE_LIMIT` (counted as `refused`); logical voices are never implicitly stolen. |
| `su_audio_voice_set_audibility` | Ranking input: `audibility = gain * gain_linear / max(1, distance_m + distance_offset)`. |
| `su_audio_voice_stop` | Physical voices release over the given (or policy) frames; virtual voices end at once. |
| `su_audio_voice_info` | State, last reason code, clip cursor, frames spent virtual, audibility. |
| `su_audio_voice_pool_render` | Interleaved stereo float, any chunk size; internal quanta are cached. |
| `su_audio_voice_pool_stats` | Current/peak logical, physical, virtual and fade-tail counts; admitted, refused, stolen, woken, expired and deferred totals; per-reason transition counters; payload bytes. |
| `su_audio_voice_reason_name` | Stable names for `SU_AUDIO_REASON_*`. |

`SuAudioVoicePolicy` holds `priority`, `distance_offset`, `physical_limit`,
`limit_scope` (global/event/emitter/bus/node), `virtual_mode`,
`wake_deadline_frames`, `release_frames` and flags (`LOOP`, `HERO`).

## Post-time admission in the integrated runtime

`SuAudioVoicePolicyRule` (passed through `SuAudioSystemDesc.policies` or the
event hook) selects leaves by `node_id`, `bus_id` and `event_id`; zero is a
wildcard and the first matching rule wins for both admission and ranking.
The appended admission fields default to zero, preserving existing behavior:

- `min_retrigger_frames`: minimum interval between successful posts matching
  that rule. The rule owns one shared timeline, across emitters and matching
  leaves. Select an `event_id` for an event cooldown, or a `node_id` for a node
  cooldown. A wildcard can deliberately share a cooldown across a family.
- `group_id`: a nonzero exclusive-group ID. Any live matching leaf with that
  ID refuses the entire new post, regardless of priority or audibility.
  Different rules can share a group; different groups remain independent.
  This is refuse-new, not steal-oldest; physical ranking is unchanged.

Admission happens in `su_audio_event_post` after expansion and before asset
pins or logical-voice allocation. All leaves of one post are admitted or
refused together; layers within that post may share a group or cooldown rule.
Cooldown uses the trigger's absolute `sample_time`, not the render clock,
wall time or a leaf's delayed start. The exact interval boundary is allowed;
out-of-order timestamps earlier than the last admitted timestamp are refused
when cooldown is enabled. Only a successful post changes history. Completion,
stops and commit-session changes do not reset cooldown history.

Groups reserve admission immediately, including scheduled/delayed starts,
virtual voices and stop-release/steal tails, until their leaves finish and
the runtime renders/reaps them. Posting a future timestamp does not predict
completion or execute queued stops. Use rendering to advance occupancy.
The group check does not rank a replacement or let an old line resume later.

Refusals return `SU_E_AUDIO_COOLDOWN` (22) or `SU_E_AUDIO_GROUP_BUSY` (23),
leave the instance handle zero, increment event `refused`, and do not advance
the commit watermark. System telemetry records the stable reasons `cooldown`
and `group_busy`, also counted by `audio.trace.stats`; journal replay checks
the returned statuses. If multiple leaves fail, expansion order selects the
first failure (cooldown precedes the group check for each leaf).

Direct event checkpoints are version 2: they include each rule's last
successful timestamp and initialized flag, even after every instance has
finished. Groups derive occupancy from the saved leaves, so no redundant
group counters exist. Admission selectors/settings enter the configuration
fingerprint; a mismatched configuration or older blob is rejected. The
separate replay-based offline checkpoint format is unchanged.

`audio_system` tests cover sample-zero initialization, exact boundaries,
backdated posts, independent events/nodes/groups, higher-priority busy
refusal, pending/playing/releasing occupancy, stop and natural completion,
trace reasons, journal verification, byte-repeatable saves, idle/active
checkpoint history and chunk-independent PCM continuation.

## Ranking

At every quantum boundary, after expiry, live logical voices are ordered by
priority (descending), audibility (descending), trigger sequence, graph node ID
and finally slot index. This is a total order, so the result does not depend on
admission order or on how the owner chunks render calls. Walking that order, a
voice becomes physical unless its audibility is at or below the floor
(`inaudible`), the physical slots are full (`rank`), only hero-reserved slots
remain and it is not a hero (`hero_reserved`), or its `physical_limit` scope
already holds that many accepted voices (`scope_limit`). Voices in an owner-stop
release keep their slot and count as non-hero.

Demotions are applied lowest rank first. A demoted voice that has been heard
moves its remaining audio into a fade-out tail of `release_frames`; there are
at most `transition_reserve` tails. If no tail is free the demotion is deferred
and the voice keeps playing, and the voice that would replace it waits with
reason `transition_full` (counted once in `deferred`). Steals therefore never
hard-cut audio unless the policy asks for `release_frames = 0`. Logical
virtualization is not spatial clustering; A08 owns clustering.

## Virtual playback

While virtual, `advance` moves the cursor 128 frames per quantum (wrapping
loops), `pause` freezes it, `restart` resumes from frame 0 and `discard` ends
the voice on demotion. A woken voice that has already been heard, or whose
cursor moved, fades in over `release_frames`; a voice that becomes physical for
the first time at frame 0 starts without a fade. A one-shot whose cursor passes
its end while virtual expires (`expired_end`) and is never replayed; any voice
virtual for `wake_deadline_frames` expires (`expired_deadline`). A one-shot
that plays to its end finishes.

Voices without controls play at 1x with a balance pan law (centre is unity on
both channels) and linear gain. `su_audio_voice_set_controls` switches a voice
to the integrated path (fractional cursor, per-channel gains, one-pole
low-pass), and `su_audio_voice_add_at`/`su_audio_voice_stop_at` start and stop
voices at exact frames inside a quantum; see
[EVENT_AUDIO_INTEGRATION.md](EVENT_AUDIO_INTEGRATION.md). Clip sample rate must
equal the pool rate when one is given.

## Tests

`ctest --test-dir build-a05 -R '^audio_voice_policy$' --output-on-failure`
covers: nearest quiet versus far loud ranking and distance offset; priority
preemption and hero reservation under saturation; insertion-order-independent
equal-score ordering; family (event) versus emitter limits and release of a
scope count on stop; exact resume positions for advance/pause/restart/discard
using a synthetic ramp, both hard-switched and with exact 64-frame fade
envelopes; fade continuity (maximum per-sample step equals one envelope
increment) for steal, wake and owner stop; one-shot and deadline expiry
without replay; logical pool exhaustion; a one-slot transition reserve that
defers the second steal until the first tail finishes; and bit-identical output
and trace hash under irregular render chunking.

## Measured cost

Windows 11 x64, Intel Core i5-12400F, Clang 23.1.3 Debug: 1024 logical voices,
64 physical slots, 16 fade tails, looping mono clips, audibility updated for
every voice every quantum, 48,000 frames: 85 ms CPU (`clock()`), with
5984 steals, 5988 wakes and 6490 deferred promotions; pool payload 191,648
bytes. This is one observation of a deliberately churning worst case in an
unoptimized build, not a latency distribution or a performance gate.
