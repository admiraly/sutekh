# Device callback timing and underrun recovery

Two output backends report `SuAudioDeviceStats` through `su_audio_device_stats`,
`SuAudioSystemStats.device`, and the integrated `audio.inspect` response's
`device` object: WASAPI on Windows and SDL3 on other platforms (Linux, and
wherever the SDL3 headers were found at configure time). This applies to OWNER
render-ahead (RING) and PULL rendering, as well as the standalone mixer device.
Stats are cumulative, bounded, allocation-free atomic counters. Concurrent
reads are approximate: individual counters are race-free, but the snapshot is
not a transaction. Inspect a PULL system under its owner lock, as usual.
The API returns zeroed stats for a null device or the unsupported backend.

## What is measured

- `callback_us`: microseconds from the device thread returning from its event
  wait through a successful WASAPI buffer submission. Includes padding/buffer
  acquisition, ring consumption or PULL rendering (including owner-lock waits),
  recovery fades, and buffer release. Not time spent waiting for the event.
- `interval_us`: successive event-wait return intervals, including timeout
  wakeups and wakeups that find no writable frames. This exposes scheduling
  stalls separately from callback service cost. The first interval is omitted.
- Both histograms have 24 logarithmic bins: bin 0 is [0, 2) microseconds, bin
  n is [2^n, 2^(n+1)), and bin 23 is [2^23, infinity). Percentiles are bin
  bounds, not exact quantiles. Timing uses QueryPerformanceCounter and truncates
  to whole microseconds. Raw histograms, callback sum/count, maxima and the
  interval maximum are available; no unbounded sample buffer is retained.
- `callback_over_budget`: service time exceeds the duration of the frames
  submitted by that callback. This is a work/audio-duration comparison, not a
  guaranteed hardware deadline or an acoustic latency measurement.
- `empty_buffers`: writable callbacks observing zero WASAPI padding (including
  startup). This is a starvation indicator, not a precise hardware glitch count.
  `wait_timeouts` and `backend_errors` distinguish event/device trouble from
  integrated `underrun_frames`, which counts missing source-ring frames.
- `stream_latency_ms`: the backend's GetStreamLatency value, not measured
  command-to-speaker latency. The C stats also identify endpoint name, sample
  rate, and device buffer size. Clock resolution, OS scheduling, backend latency,
  render-ahead occupancy and acoustic latency are different quantities.

## The SDL3 backend (non-Windows)

`src/audio/su_audio_sdl3.c` opens the default playback device through SDL3 and
lets SDL convert float32 stereo at the mixer's rate to the device's format and
rate. SDL owns the audio thread: it calls the backend's stream callback once per
device period, so there is no render thread of ours. The SDL audio stream is the
render-ahead buffer this backend owns, the counterpart of the WASAPI device
buffer's padding.

- Each callback tops the stream up to two device periods of input, so one
  period stays queued while the device consumes one. `additional_amount` is not
  used; `SDL_GetAudioStreamQueued` gives the queue directly, so a device whose
  period changes after open is handled without reallocating the render buffer
  (allocated once at open, sized four periods plus the recovery fade).
- `callbacks`, `callback_us`, `callback_max_us`, `callback_over_budget` and
  `frames` count submissions: time from callback entry through
  `SDL_PutAudioStreamData`, compared against the audio duration submitted, as on
  WASAPI. `interval_us` samples the interval between callback entries, so a
  stalled device thread or SDL's own scheduling shows up there.
- `empty_buffers` counts a callback that found the stream empty (startup
  included) — the same starvation signal as zero WASAPI padding. A partially
  drained stream is not counted, as WASAPI does not count a partial buffer.
- `wait_timeouts` is always 0: this backend waits on no event, SDL's thread
  does the pacing. `backend_errors` counts a failed stream write and a device
  buffer that outgrew the render buffer allocated at open.
- `buffer_frames` and `stream_latency_ms` are in the stream's (the mixer's)
  frames, so `buffer_frames / sample_rate` is a duration. SDL reports no latency,
  so `latency_ms` is the device buffer plus the one period kept queued, computed
  at open; it is not a measured command-to-speaker latency. The endpoint name
  carries SDL's audio driver in parentheses (`pipewire`, `pulseaudio`, `alsa`,
  or `dummy`), which is what identifies the path a run took.
- The recovery policy below applies unchanged: a callback that finds the stream
  empty fades the newly rendered block in from zero, in mixer and PULL mode
  alike. Nothing is allocated, locked (beyond the stream's own reentrant lock) or
  blocked inside the callback.

## Recovery policy

Only live device PCM is modified; offline output and direct ring reads retain
their exact existing PCM behavior. The integrated device reader counts missing
source frames and applies a bounded 64-frame stereo linear transition (1.33 ms
at 48 kHz):

1. A partially fulfilled callback with at least 64 available frames tapers its
   last available frames to zero. Shorter prefixes retain their PCM, then ramp
   the held last sample into the missing region rather than compressing a fade
   into a single frame.
2. A wholly missing callback ramps the last output sample to zero over up to
   64 missing frames, carrying that ramp across short callbacks. Remaining
   missing frames are silence; no old audio is replayed or ring time advanced.
3. Recovery crossfades from the last emitted tail into new PCM over 64 available
   frames. An interrupted fade restarts from its actual emitted value, avoiding
   an abrupt jump if data returns before the tail reaches zero.
4. On a subsequent zero-padding WASAPI callback, the backend fades newly
   submitted PCM in from zero. This also covers PULL/device scheduling stalls.

Already-played audio cannot be faded retroactively: a hardware gap caused by
an unscheduled/blocked device thread may still click at its leading edge.
Recovery is de-click concealment, not reconstruction of missing audio.
PULL renders requests larger than its staging ring in bounded chunks rather
than manufacturing an underrun at the ring capacity. Its owner lock can still
block the callback; timing includes that delay. No new callback allocation or
additional lock is introduced.

## Optional adaptive render-ahead

`su_audio_system_ring_adaptive(system, 1)` opts an OWNER-rendered RING/DEVICE
system into a 256-frame initial target. On the next owner update after missing
source frames, the target doubles up to the configured ring capacity. After
five seconds of consumed source frames without another underrun, it decreases
by one 128-frame quantum, never below 256. Repeated starvation at maximum
capacity remains counted; adaptation cannot hide an arbitrarily long stall.
Shrinking stops production until existing occupancy drains; queued PCM is never
discarded. Disabling restores the configured target. `ring_target`,
`ring_capacity` and `ring_fill` are separately inspectable.

Storage is allocated once at creation: this is effective ring sizing, not
callback-time reallocation. Enable/disable on the serialized owner thread;
PULL and OFFLINE reject it. It is disabled by default. The policy deliberately
trades increased render-ahead/command latency for fewer owner-starvation gaps.

## Reproducible long-run device profile

Build `test_audio_system` with the normal Windows toolchain. This opt-in mode
is not a CI timing gate and accepts 1..86400 seconds:

```powershell
. ./tools/env-windows.ps1
python tools/bootstrap_asset_deps.py
cmake -S . -B build-i120 -G Ninja -DCMAKE_BUILD_TYPE=Release -DSUTEKH_NATIVE=OFF -DSUTEKH_RENDER=OFF -DSUTEKH_ASSET_IMPORT=AUTO
cmake --build build-i120 -j 4
ctest --test-dir build-i120 -R '^audio_' --output-on-failure
./build-i120/test_audio_system.exe --device-profile 3600 owner adaptive > build-i120/owner.json
./build-i120/test_audio_system.exe --device-profile 3600 pull fixed > build-i120/pull.json
```

`owner fixed` is also supported; adaptive PULL is rejected. The profile uses
the existing synthetic spatial/event fixture at 48 kHz, updates the owner about
every 5 ms, and injects a 200 ms owner sleep (without holding the lock) every
five seconds. It emits one JSON result containing histograms, source underruns,
ring state and the full inspection snapshot; endpoint name goes to stderr.
It fails on backend errors or no completed callbacks and returns 77 when no
device is available. Timing is observational, not a hard latency pass threshold.
Record compiler/build flags, hardware/OS, source and executable hashes, duration,
endpoint, histogram percentiles and maxima alongside any published result.

Tests cover logarithmic bucket boundaries/saturation, partial/full gaps,
stereo symmetry, recovery across calls and an interrupted tail, adaptive
growth/shrink/ceiling, oversized PULL reads, inspection fields, unchanged
offline/ring replay PCM and allocation-free steady state. The existing threaded
PULL test checks journal preservation against its initial command count rather
than assuming the host schedules a fixed number of owner updates in one second.

## Local measurement status

No device numbers are published yet: the development host used for this change
had no stable default render endpoint (a remote-session endpoint appeared once,
then `0x80070490`), so the profile returned 77. Run it on real hardware.

## Remaining platform work

Linux output is implemented (SDL3, above) and exercised on PipeWire and on SDL's
dummy driver; PulseAudio, ALSA, JACK and sndio go through the same SDL path but
were not run here. macOS uses the same backend and is untested. Offline paths and
zero-valued unsupported-device stats remain available. Physical loopback/acoustic
latency, endpoint-loss recovery, and multi-hour qualification on multiple device
drivers are not established by this profile.
