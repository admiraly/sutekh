# Per-voice low-pass

`su_audio_voice_set_lowpass(mixer, voice, 1000.0f)` sets a 1 kHz cutoff on a
playing or queued voice. Call from the game thread, like the gain and pitch
setters. The bounded command queue applies it at the next render. A stale
handle returns `SU_E_STALE_HANDLE`; a full queue returns `SU_E_RESOURCE_LIMIT`.
Negative or non-finite cutoffs return `SU_E_INVALID_ARGUMENT`.

Filtering is off on every new voice. Zero or a cutoff at/above the mixer's
Nyquist frequency disables it. When it has always been disabled, samples are
bit-identical to the original mixer. After disabling a previously active filter,
the exact bypass resumes after the smoothing interval.

Each channel has an independent one-pole history, after resampling/pitch and
spatial downmix, before voice gain and the master limiter. The coefficient gives
-3.01 dB at the requested cutoff, using the mixer output rate rather than the
source rate. Coefficients glide linearly over 10 ms of output samples, including
enabling and disabling, independent of block size. Retargeting starts from the
current coefficient; resending the same target does not restart the ramp.
The render path is allocation-free; coefficient design runs on the game thread.
Very small filtered outputs (below 1e-20) become zero to avoid denormal tails.
Histories and settings reset on reuse/steal; seeking retains history.

Distance/occlusion policy belongs to the caller: compute a cutoff from source
distance and send it when it changes. For example, use zero inside the full-band
radius and progressively lower positive cutoffs farther away. No automatic
distance policy or high-pass is introduced.

Run `ctest --test-dir build-win -R audio --output-on-failure` for correctness;
`build-win/test_audio_lowpass.exe --bench` measures seven paired bypass/filtered
trials (64 looping mono noise voices, stereo output, 48 kHz, 256-frame blocks,
100 warm-up blocks and 2000 measured blocks). Results are ns per voice per
output frame, including the rest of the mixer; the difference estimates filter
overhead. This is an observational benchmark, not a timing gate.
