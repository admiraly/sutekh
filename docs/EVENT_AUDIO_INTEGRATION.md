# Integrated event-audio runtime

The A01-A10 components are wired into one runtime here. Without a hook, the
event runtime still mixes through its private legacy mixer exactly as before
(`audio_event`, `audio_offline` and `audio_inspect` tests are unchanged). The
legacy `sutekh/audio.h` mixer and device API are untouched. Games normally use
the single owner object below; the per-component hooks underneath it are
documented in the following sections. Moving a game from the legacy mixer:
[EVENT_AUDIO_MIGRATION.md](EVENT_AUDIO_MIGRATION.md).

## One system object

`sutekh/audio_system.h`: `su_audio_system_create(&desc, &system, message)`
builds and wires everything from one descriptor and owns it: scene (with the
authored curves), voice pool and policy rules, optional bus tree, parameters,
music (with its segments) and cluster, the event runtime attached to them, an
optional command journal and A10 telemetry ring, and the output:

- `SU_AUDIO_OUTPUT_OFFLINE`: `su_audio_system_render(system, out, frames)`
  pulls frames; any chunking gives the same stream.
- `SU_AUDIO_OUTPUT_RING`: `su_audio_system_update` renders whole quanta until a
  render-ahead ring of `ring_frames` is full; `su_audio_system_ring_read` reads
  it (silence and an `underrun_frames` count when it runs dry).
- `SU_AUDIO_OUTPUT_DEVICE`: as `RING`, read by the output device thread (WASAPI
  pull mode). The owner calls `update` once per game frame; the device thread
  only touches the lock-free single-producer/single-consumer ring. A host
  without a device gets `SU_E_UNSUPPORTED_METHOD` and a message.

Presentation commands go through journaled wrappers with the component
semantics: `post`, `stop` (instance, emitter or scope), `param_set`,
`switch_set`, `emitter_create/update/destroy`, `listener_set`,
`music_transition` and `bus_gain`. Each journal entry records the number of
frames the runtime had produced when it was issued, so
`su_audio_system_replay` reproduces the session in a fresh `OFFLINE` system.
The real-time path (ring or device) and the offline path (journal replay) give
identical PCM within one build (`audio_system`, `audio_acceptance`).

`su_audio_system_checkpoint_save/load` wrap the direct checkpoints below with a
16-byte header that records how many journal commands preceded the save.
`su_audio_system_checkpoint_verify(desc, journal, count, blob, bytes, frames)`
is the replay verifier: it replays the journal to the checkpoint in one fresh
system, loads the checkpoint into another, renders `frames` more in both
(applying later commands) and requires identical PCM.

`su_audio_system_control(system, method, params, len, out, capacity,
&written)` implements the control-plane methods for a host's structured
control plane (`su_audio_system_method(i)` lists them):

| Method | Result |
| --- | --- |
| `audio.inspect` | One JSON object: clock, ring and journal state, payload bytes, the A10 snapshot of instances/voices/emitters/buses, voice-pool stats with reason names, bus meters, parameter and cluster diagnostics |
| `audio.trace.read` | Drains up to 1024 telemetry entries as A10 trace JSON (posts, stops, emitter moves and listener changes with stable reason codes) |
| `audio.trace.stats` | Telemetry capacity, queued, recorded, dropped, drained and per-reason counts |
| `audio.render.offline` | `{"frames":N}` (default: frames produced so far, at most ten minutes): replays the journal into a fresh offline system and reports frames, command count, PCM SHA-256, sample peak, 4x true peak and BS.1770 integrated loudness |

Outputs carry numeric IDs and handles only. `render.offline` allocates its
temporary system and PCM; the other methods use storage allocated at creation.
The engine's agent control plane (world and capsule methods) does not host an
audio system, so these methods are not registered there; a host exposes them.

## Voices and spatial filtering

`su_audio_events_attach(events, &hook)` (hook version
`SU_AUDIO_EVENTS_HOOK_VERSION`, appended fields selected by `struct_size`)
replaces the private mixer of an idle runtime with an A05 voice pool. The pool
must be idle, at the runtime's sample rate (or 0) and hold at least
`max_logical_voices` logical voices, so an admitted leaf always gets a logical
voice. Each admitted leaf becomes one logical voice:

- Start and stop are sample exact. `su_audio_voice_add_at` starts a voice at an
  absolute pool frame inside a quantum; `su_audio_voice_stop_at` begins its
  release at an exact frame. A stop at or before a leaf's start cancels it
  (stops still precede starts at equal timestamps). In integrated mode a stop
  with `release_frames == 0` uses the voice policy release instead of a hard
  cut.
- Policies come from the hook: the first `SuAudioVoicePolicyRule` whose
  `node_id` and `bus_id` (0 = any) match the graph leaf, else
  `default_policy`. Looping leaves (`flags` bit `SU_AUDIO_LOOP`) get
  `SU_AUDIO_VOICE_LOOP`. Trigger sequence is admission order, then expansion
  order.
- Every quantum the runtime evaluates each live leaf with the same law as the
  legacy mixer: scene resolve (curve gain, scale, curve low-pass), distance
  attenuation, listener-relative equal-power pan and mono downmix for spatial
  voices; leaf pitch times the asset/runtime rate ratio. The result goes to the
  pool through `su_audio_voice_set_controls` and ranks the voice through
  `su_audio_voice_set_audibility(gain, 0)` (the curve already includes
  distance).
- The pool's controlled render path uses a double-precision fractional cursor
  with linear interpolation, a one-pole low-pass whose coefficient glides over
  10 ms, per-channel gains ramped across each quantum and a pitch glide across
  the quantum. Steal fade tails carry the full cursor, filter and gain state,
  and a woken voice fades in from a cleared filter, so virtualise, steal and
  resume do not click (unless a policy asks for `release_frames = 0`).
- A leaf ends when its logical voice dies; its asset pin is released only
  after no steal tail still reads the clip.

For unpitched, pitched, looping, delayed and statically spatialised voices
without a curve low-pass the integrated output is bit-identical to the legacy
path (`test_legacy_equivalence`).

## Parameters, buses, music and the master limiter

The hook's appended fields (read only when `struct_size` covers them,
`SU_AUDIO_EVENTS_HOOK_HAS`) attach optional components; all must outlive the
runtime and are then driven only by it:

- `params`: `su_audio_event_post` builds `su_audio_param_expand_state` for the
  `(emitter, new instance)` target at the trigger time and expands through
  `su_audio_graph_expand_ex`, so switch and blend nodes follow scoped values
  (`FIRST_ONLY` latches at the trigger time). Every quantum the runtime
  advances the parameter clock to the quantum start and evaluates `GAIN_DB`
  (added to the voice gain), `PITCH` (multiplies the pitch) and `LOWPASS_HZ`
  (the lower of curve and binding cut-off) for each voice's
  `(emitter, instance)` target at the quantum end; the gains then ramp across
  the quantum. `BUS_GAIN_DB` bindings, evaluated globally at the quantum end,
  drive their bus faders with a one-quantum ramp; those faders belong to the
  bindings. A retired instance's scoped values are cleared. Graph snapshots may
  now hold up to 4096 parameters and switches regardless of graph size.
- `buses`: routes are the tree's buses in execution order; each voice mixes
  into its leaf `bus_id`, unknown or zero IDs into the root. The runtime mixes
  every route, sets the bound faders and processes the tree once per quantum,
  so sidechain ducking detects the real post-fader bus signals. The tree's
  `max_block_frames` must be at least 128. The authored-loudness window has no
  automatic input yet (`su_audio_bus_set_loudness` is per block).
- `music` / `music_bus_id`: a music player on the runtime clock renders one
  quantum per quantum into its bus (0: the root; without buses it is added to
  the voices).
- `limiter_knee`: the master output passes the legacy soft limiter
  (`knee + (1 - knee) tanh((|x| - knee) / (1 - knee))`, never beyond full
  scale) when the knee is in [0.1, 1); 0 keeps the hard clamp.

## Pull rendering (device-driven)

With `desc.render = SU_AUDIO_RENDER_PULL` (RING or DEVICE output) the reader
renders: `su_audio_system_ring_read`, which the device thread calls, first
renders the quanta it is missing under the system lock, then copies them out.
The owner brackets each batch of calls (posts, parameters, emitters, queries,
component reads) with `su_audio_system_lock` / `su_audio_system_unlock`, and
`su_audio_system_update` does nothing. Rendering therefore never waits for the
owner's frame: an owner stall of any length does not starve the device. Latency
is one device period plus less than a quantum, instead of the render-ahead
ring. Commands still land at their journal positions, so the journal replay
equals the live stream (`test_pull_render`: a device-paced reader, an owner
with a 150 ms stall, zero underruns, identical PCM). Host bus inputs run on the
reader's thread under the lock. Never hold the lock across
`su_audio_system_destroy`; it closes the device first. OWNER mode (the default)
is unchanged.

## Doppler, host bus inputs and bus-filtered bindings

- `doppler` (hook and `SuAudioSystemDesc`, in [0, 1]; 0 = off) multiplies
  the pitch of every spatial voice by the legacy `su_audio_doppler` law
  (listener and emitter velocities, `speed_of_sound`, 0 = 343 m/s), evaluated
  per quantum with the other controls, so the pitch glides like a `PITCH`
  binding.
- `inputs` / `input_count` (at most `SU_AUDIO_EVENTS_MAX_INPUTS`) are host bus
  inputs: once per quantum, inside the render on the owner's thread, each
  `SuAudioBusInput.render(user, stereo, 128, sample_time)` fills a cleared
  stereo buffer that is mixed into its `bus_id` (0: the root; without a bus
  tree: the output) before the tree runs. The input therefore gets the bus
  faders, ducking (as a target and as a sidechain source), meters and the
  master limiter. This is how a game keeps long tracks on the legacy
  `SuAudioStream` path (the music player only plays resident segments) while
  mixing them through the system. Host input is not journaled: replays,
  `audio.render.offline` and checkpoint verification build their systems
  without inputs, so their PCM lacks it.
- Voice-control bindings (`GAIN_DB`, `PITCH`, `LOWPASS_HZ`) with a nonzero
  `bus_id` apply only to voices whose graph leaf `bus_id` equals it, so one
  parameter can drive several layers through different curves (one bus per
  layer). `bus_id` 0 keeps the previous behaviour (every voice).

## Clustering

`cluster` and `cluster_inputs` in the hook feed an A08 cluster once per
quantum, after the bus tree has run:

- `SU_AUDIO_EVENTS_CLUSTER_VOICES`: every rendered voice and steal tail
  becomes an object. Its ID is the logical voice handle (tails set bit 63),
  its direction and distance are the listener-relative values the runtime
  computed for that voice, its PCM is the pool's mono capture after envelope
  and low-pass and before the channel gains, and its RMS is measured from that
  block. Non-spatial voices are beds; `metadata` is the leaf bus ID.
- `SU_AUDIO_EVENTS_CLUSTER_BUSES`: every pre-master bus (a direct child of the
  root) is a stereo bed object with ID `bit 62 | bus_id`, read from the bus
  tree's capture of its processed output.

The cluster's `max_objects` must cover `max_physical + transition_reserve`
plus the pre-master buses. Clustering describes an object representation for
an object renderer (read it with `su_audio_cluster_outputs`/`assoc`/`stats`);
its CPU stereo reference advances the crossfades, and the runtime's stereo
output does not depend on it (`test_cluster_feed` compares both bit for bit).

## Direct checkpoints

`su_audio_events_checkpoint_save(events, out, capacity, &written)` (size
query with `out == NULL`) and `su_audio_events_checkpoint_load(events, data,
bytes)` save and restore the whole integrated state, not a command replay:

| Section | Contents |
| --- | --- |
| Runtime | instance slots and generations, leaves (graph leaf, timing, state, controls, logical voice), queued stops, commit cursor, stats, the partially consumed quantum |
| Voice pool | free list, every live logical voice (policy, cursor, virtual time, fades, release, start/stop frames, filter and gain ramps) and steal tails |
| Bus tree | faders and ramps, window gains, meters, duck envelopes and holds, delayed sidechain rings |
| Parameters / music | timeline slots and clock; players, pending transition, stingers (as asset references) |
| Cluster / scene | assignments, cluster slots, IDs and clock; emitters and the listener |

Clips are written as asset `(id, generation)` and re-pinned on load; no
pointers are written. A 64-byte header carries a magic, version, a layout
fingerprint of the configuration (capacities, bus IDs, parameter counts,
attached components, limiter knee), the body size, an FNV-1a checksum and the
saved clock. Load checks all of them and that every pinned generation is still
resident before it changes anything (`SU_E_SCHEMA_MISMATCH`). The target must
be a runtime attached to identically configured components; the owner restores
its own presentation state (instance handles it kept, its commit sequence).
Restore cost depends on the configured capacities only, never on the position.
The layout is the build's in-memory numeric representation (profile
`audio_float_local_v1`): a checkpoint is valid within the build that wrote it.
Legacy (unattached) runtimes return `SU_E_UNSUPPORTED_METHOD`; their A09
replay checkpoints are unchanged.

Replay stays as the verifier: `test_checkpoints` renders a scripted session
(posts with spatial emitters, switch and parameter changes, a listener turn, an
emitter move, a stop with release, pitch and gain glides, music, ducking,
clustering and virtualisation with three physical voices) once uninterrupted.
Then, at 12 positions including 1, 127 and 128 frames and other positions
inside quanta, it replays to the position, saves, loads into a fresh runtime
and requires the restored continuation to equal the uninterrupted render bit
for bit. The replayed runtime's own continuation must match too. Checkpoint
size varies only with live voices, never with elapsed time.

## Tests

`ctest -R '^audio_runtime$'` (`tests/audio/test_audio_runtime.c`, synthetic
PCM only): hook validation; legacy equivalence under 37- versus 1000-frame
chunking; exact pitch-0.5 interpolation; curve low-pass on a far voice; a
one-physical-voice steal and resume with the largest sample step bounded by
the tone slope plus one envelope increment; exact stop timing, stop-before-start
cancellation and instance retirement. Parameter cases: switch selection before
and after a timed change, exact blend weights, `GAIN_DB` and `PITCH` bindings
on every voice, sidechain ducking of an ambience bus by shots on another bus,
a `BUS_GAIN_DB` binding reaching -12 dB on its fader, music metered on its own
bus, and the soft limiter bending an 18 dB overdrive to at most full scale.
Clustering and checkpoint cases are described above. The suite also passes
under clang AddressSanitizer.

`ctest -R '^audio_system(_alloc)?$'` (`tests/audio/test_audio_system.c`): a
ring session driven per game frame (posts on three emitters, an emitter move,
a stop with release, a bus fader binding, per-bus voice limits, clustering)
read in 441/480/97-frame device chunks equals the journal replay bit for bit
with no underruns; an empty ring delivers counted silence; a checkpoint
restored directly continues like the uninterrupted render and passes the
replay verifier, while a corrupted one fails it; all four control methods,
including the offline render's PCM digest against the live stream; device
output either opens or reports why not; a host bus input is called once per
quantum in order and mixed through its bus (and absent from the offline
replay); an approaching emitter plays 200 Hz at 1.111x with Doppler 1; and a
bus-filtered `GAIN_DB` binding lowers only its bus's voice. The `_alloc` build compiles every
integrated-runtime source with counting allocators and requires zero
allocations after creation over ten seconds of posts, emitter moves,
parameter changes, clustering, telemetry, journaling and checkpoint saves
(payload 1.24 MB for that configuration). `audio_acceptance` adds an
integrated stage that runs the A10 slice through one `SuAudioSystem` (ring
output, per-bus voice limits, ducking, a parameter-driven fader, clustering,
telemetry, inspection and an offline report) and requires the real-time and
offline PCM to be identical.

## Measured cost

`bench_audio_system [seconds] [json] [physical]` (`bench/audio`) renders
offline on one thread: 512 logical looping spatial voices on 256 emitters that
move every 2048 frames, 64 physical voices (4 hero-reserved, 16 steal tails),
16 buses with 8 sidechain ducks (half delayed), gain and pitch bindings on
every voice plus one bus-fader binding (parameters move every half second), 3
stereo music stems and clustering (voices and pre-master buses, 16 outputs),
master soft limiter. Clang 23.1.3 `-O3` (CMake Release), Windows 11, Intel
Core i5-12400F, 10 s of audio, CPU time from `clock()`, three runs:

| Configuration | CPU per second of audio |
| --- | --- |
| Full (above) | 90.1 to 91.0 ms (about 9% of one core) |
| Without clustering | 80.6 to 81.7 ms |
| Voices only (no buses, parameters or music; no clustering) | 60.1 to 60.4 ms |
| Full, 32 physical voices | 69.7 ms |
| Full, 128 physical voices | 157.9 ms |

About 37 ms of the voices-only figure is per-quantum control work for the 512
logical voices (scene resolve, curve, pan and ranking every 128 frames) and is
paid even when few voices are physical; the rest scales with physical voices.
These are offline throughput figures, not a device-callback latency
distribution, which remains unmeasured.

## Not covered

- Switch and blend choices are made when an instance expands; `CONTINUE`/`FADE`
  switch changes and blend parameter moves do not re-select the branches of a
  playing instance.
- The bus tree's authored-loudness window has no automatic input from voices.
- Clustering produces object metadata and a reference mix; no object or
  height renderer consumes it, and the stereo output does not depend on it.
- Checkpoints are valid within the build that wrote them. Legacy (unattached)
  runtimes keep the A09 replay checkpoints only.
- Headless agent sessions do not expose audio methods unless their host sets
  `SuAgentConfig.audio_system` (see [AGENT_RPC.md](AGENT_RPC.md)).
- On an underrun the device plays silence and the system counts it; no
  device-callback latency distribution has been measured.
