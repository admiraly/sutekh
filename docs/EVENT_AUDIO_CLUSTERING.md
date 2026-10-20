# Optional persistent directional clustering (A08)

`include/sutekh/audio_cluster.h` is an additive component header implemented in
`src/audio/su_audio_cluster.c` and built as the `su_audio_cluster` static
library on `su_audio`. No shared header, schema or runtime source changed.

Clustering is optional and changes only the spatial representation of objects
that were already rendered. It never drops an input, never alters voice
admission, and never removes a trigger. Every object passed to an update is
rendered by `su_audio_cluster_render`.

## Model

`SuAudioClusterDesc` configures `width_rad` (join angle, (0, pi]),
`leave_ratio` (>= 1; leave angle is `width_rad * leave_ratio`), `hold_frames`
(minimum membership before leaving), `crossfade_frames`, `proximity_m`
(closer objects stay dedicated; 0 disables), `priority_threshold` (objects at
or above it stay dedicated; 0 disables), `output_limit` (clusters plus
dedicated objects) and `max_objects`. All storage is allocated at creation.

Each `SuAudioClusterObject` gives a stable 64-bit ID, a listener-relative
direction (zero means non-directional), distance, RMS, priority, flags,
metadata and its rendered PCM (mono, or stereo interleaved). Until A05 lands,
the caller supplies the logical identity (for example the event instance and
leaf node); IDs must be unique per update.

Roles:

- `BED` flag: passes straight to the stereo bed (stereo unchanged, mono
  centered); never counted against the budget.
- `EXCLUDE` flag, priority at/above the threshold, or distance below proximity:
  a dedicated output object with a stable output ID while it keeps that role.
- Otherwise clustered. Objects with different `metadata` never share a cluster.

## Update

`su_audio_cluster_update` sorts inputs by ID (stable merge sort in prepared
scratch), so every decision and accumulation is independent of array order.

1. Clustered objects stay in their cluster while the angle to its previous
   centroid is within the leave angle, or while `hold_frames` have not passed
   since joining.
2. Remaining objects, in ID order, join the compatible cluster with the smallest
   angle within `width_rad` (ties: lower cluster ID), or create a new cluster
   with a new stable ID.
3. Centroids are RMS-weighted sums of unit directions (unweighted when all RMS
   is zero). Directions that cancel produce a zero, non-directional centroid,
   which the stereo reference renders centered.
4. While clusters plus dedicated objects exceed `output_limit`, the compatible
   pair with the smallest centroid angle merges into the lower ID. If no
   compatible pair exists (for example all inputs excluded or metadata differs)
   `stats.infeasible` is set and all outputs are still rendered.

When an object changes output it crossfades from the previous output's
direction over `crossfade_frames` with complementary linear gains. A cluster
whose last member left keeps `draining` status until those fades finish. If an
object leaves and the budget merges it back into its original cluster within
the same update, the move is undone without a fade. A cluster with a single
member follows that member; clusters merge only for the budget.

Inputs are validated before any state changes: duplicate IDs
(`SU_E_DUPLICATE_KEY`), more than `max_objects`, non-finite values, negative
RMS or unsupported channel counts (`SU_E_INVALID_ARGUMENT`) leave the previous
assignment intact.

## Render and diagnostics

`su_audio_cluster_render(objects, count, frames, out)` takes the same array as
the last update. Spatial stereo inputs are downmixed at -3 dB per channel
(`(L + R) * 0.7071`) and panned with `su_audio_pan` toward their output
direction (cluster centroid or the object's own direction). Objects accumulate
in ID order, so PCM is bit-identical across input permutations. Rendering
advances crossfades and the clock; updates are expected at block boundaries.

`su_audio_cluster_outputs` lists outputs in ascending output ID with kind,
direction, RMS weight, member count, metadata, draining flag and the object ID
for dedicated outputs. `su_audio_cluster_assoc` lists each input in ID order
with its role, output, previous output and current crossfade gains.
`su_audio_cluster_stats` reports counts, the infeasible flag, created clusters,
joins, moves, merges and the prepared payload size.

## Tests and measured cost

`ctest -R '^audio_cluster$'` (`tests/audio/test_audio_cluster.c`, synthetic
signals only) covers descriptor and input validation, close directions merging
with the exact RMS-weighted centroid, an isolated priority object plus
proximity and excluded objects remaining distinct, the -3 dB stereo downmix and
bed pass-through, cancelled directions rendering centered and stable, metadata
incompatibility, all-excluded infeasible budgets with no dropped energy,
closest-pair budget merges, energy (within 0.25 dB) and inter-channel level
difference (within 0.5 dB) against an unclustered render, a moving source
crossing boundaries once with hold, hysteresis and continuous PCM,
complementary crossfade gains and draining, bit-identical membership, outputs
and PCM across shuffled input orders, and zero allocations over 200 steady
update/render cycles (the test links an allocator-instrumented build of the
source).

Measured on Windows 11 x64, Intel Core i5-12400F, Clang 23.1.3 Debug, coarse
`clock()` over 200 iterations with 256 moving objects, limit 16, 512-frame
blocks: update about 25-40 us, stereo reference render about 460 us per block;
prepared payload 92352 bytes for 256 objects. Preparation (create) allocates;
steady state does not. These are observations, not a latency distribution.

## Not covered

Update cost scales with objects times cluster slots (`max_objects`), and budget
merges are quadratic in live clusters; both are bounded by the descriptor. Only
the CPU stereo reference exists: no object/height backend adapter is admitted.
Integration with A05 logical identity and A06 pre-master bus outputs, and RMS
measurement from rendered PCM, remain for the integrator.
