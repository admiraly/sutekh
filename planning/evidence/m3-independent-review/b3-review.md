# Independent review: B3 formation catch-up candidate

Reviewer: /root/review_limits_v2; implementation author: /root. Reviewed 2026-10-14. Scope is candidate generation and emitted IR/capsule, contract sections 5.3 and 11, locked hidden fixture expectations and recorded real candidate evaluations. No source or canonical gameplay capsule was modified by reviewer.

## Verdict

No blocking implementation defect found under the explicitly stated Euclidean-distance interpretation. Conditional approval for B3 experiment integration, subject to full existing-suite and full-schedule installation evidence. This is a behavior experiment, not an optimization or engine-wide semantic replacement.

The candidate boosts only anchor_valid=1, mode0 and squared distance strictly greater than4096. Mode1 continues to ignore enemies, while mode0/mode2 retain the normative target_valid stop gate. For equal position/goal, the simplified negative branch subtracts zero and is equivalent to baseline. Per-axis min(distance,effective_speed) prevents overshoot; each selected coordinate stays between old position and in-domain goal. Speed is bounded0..1024, so double speed<=2048. Axis differences<=32767 and sum of squares<=2,147,352,578, below2^32. Eagerly evaluated unselected modular add/sub expressions do not escape through select. The candidate declares precisely the additional anchor_valid read, with spatial-before-move already ordered, and writes only pos_x/pos_y. No new allocation, capability or native dependency is introduced. Generator writes only its explicitly requested output folder; canonical capsules remain inputs.

## Independent checks and inspected evidence

Reviewer ran a separately authored literal IR0.1 interpreter over10,000 legal-domain rows, seed20261014, comparing emitted IR to direct arithmetic B3 expectations. All matched; script exit0. This validates the chosen metric, not the contract's unstated metric.

Locked hidden manifest SHA25643c05f8688807a4a2d6f5fd4716a0b65ec5082c41f1ad31a3aa7efb94fc9bd08 identifies a different fixture author, with candidate/model/output reading denied during authorship. Inspected actual candidate.evaluate results: both B3 fixtures reject baseline with column_mismatch:pos_x[0], and candidate passes both cases. These test boundary64/65, cap, negative direction, nonformation/order exclusions and target stop. Phase-only fixtures do not prove schedule installation.

Inspected author live-install record; it supports the proposed pipeline but remains author evidence. Root must provide reproducible full-schedule installation and unchanged worker PID/artifact evidence alongside suite checks before declaring M3-07 B3 acceptance.

## Interpretation limitation

Section11 does not define diagonal distance metric. Euclidean squared distance is consistent with the profile's targeting metric and ordinary distance terminology, and keeps section5.3 per-axis movement. It is a reasonable versioned experiment choice, not a proven unique reading. The blind fixture author deliberately excluded diagonal ambiguity. The author diagonal48/48 probe establishes chosen semantics only. Document Euclidean threshold plus per-axis clamping explicitly in the behavior record; do not describe it as vector-normalized travel or claim a metric-independent hidden proof.

No claim is made for malformed out-of-profile rows, unsupported arbitrary speed values, full suite success, B2/B1 acceptance, or native performance.

## Reviewed SHA256s

```json
{
  "build_b3_candidate.py": "b48e4164d75bcd0e8861ddf0209b47a89658ae5af61ee54466b1f4e34f34d3e3",
  "candidates/b3/move_b3.capsule.json": "2da86c59ba5730e5abc2c79beb3976e6ee2fd995b5ac7312bc1bc85357a49671",
  "candidates/b3/move_b3.lir.json": "11f7941c2fedff45ef0f851ebae6fd4c3e076cea7d00b7131e74177949c52039",
  "hidden/manifest.json": "43c05f8688807a4a2d6f5fd4716a0b65ec5082c41f1ad31a3aa7efb94fc9bd08",
  "b3-hidden-results.json": "d562b3cece167e9630f7703a1a69284ce3bfd40f64cbfa517d3a708c8b970cda",
  "b3-install-author.json": "e2b369882330613e3dd8401a7c25e081d69cf025fbf686d86b03d88f371d8def"
}
```
