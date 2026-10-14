# Independent B2 review

Reviewer /root/review_limits_v2, implementation author /root, 2026-10-14. Review is candidate-only, contract section11 B2 with section5.3 move and prior B3 review. No candidate source writes.

No blocking defect found. The additional predicate is exactly target_valid AND cooldown==0; mode1 still overrides stop. Cooldown is consumed after combat by the existing combat-before-move edge, so a precombat cooldown1 becoming0 correctly stops on that tick; a fired projectile leaves positive cooldown, enabling kiting immediately. Declared cooldown field9 read introduces a real ordered conflict; spatial/goal edges and all B3 fields remain. Output writes are unchanged pos_x/pos_y; numeric profile/IRversion/capabilities/allocation remain unchanged. Candidate system revision3 names accumulated B3+B2 semantics.

The generator inserts five scalar instructions after the target predicate and replaces its downstream select condition with the new stop predicate. The existing B3 boost, Euclidean threshold, per-axis clamp and legal-domain overflow reasoning remain intact. Register names do not collide with B3 registers. This generator depends on the exact B3 instruction shape; failures locating predicates are loud rather than silently generating a partial candidate. Its output is candidate folder only.

Independent literal IR interpreter checked10,000 seeded legal-domain rows against directly derived B3+B2 arithmetic, including cooldown0,1,3,4,65535 and maximumu32. All passed. On rows where B2 should not change B3 (cooldown0, no target or mode1), outputs were compared with predecessor IR and all matched. Seed20261014; script exit0. This is review-specific independent semantic checking, not full schedule validation.

Inspected b2-hidden-results.json: independent blind fixture author is distinct from implementation/model author; baseline and B3 reject the new B2 fixture, while B2 passes it and both earlier B3 fixtures. Locked hidden manifest and result hashes below identify the evidence. Runtime evaluate evidence was authored/executed by integrator, independently inspected here.

Conditional approval pending reproducible full6system schedule installation and required existing-suite checks. Phase fixture results do not independently establish worker-PID stability, checkpoint recovery or general battle behavior. Prior B3 diagonal metric limitation remains explicitly documented, not reopened or silently broadened by B2. No performance or whole M3 acceptance claim.

## Reviewed SHA256s

```json
{
  "build_b2_candidate.py": "33444f7a7b7cb14fc3f2edebefdf30c325f72349105648a7432c9911a5200f11",
  "candidates/b2/move_b2.capsule.json": "ea8a7057010aa967af8c643a2424733b1162fa95e5abca43cd8c2396ca22b573",
  "candidates/b2/move_b2.lir.json": "0fa719f728ec34e571f09ec532ad74e44ac16211c6963f74df858cf5e10029ad",
  "candidates/b3/move_b3.lir.json": "11f7941c2fedff45ef0f851ebae6fd4c3e076cea7d00b7131e74177949c52039",
  "hidden/manifest.json": "43c05f8688807a4a2d6f5fd4716a0b65ec5082c41f1ad31a3aa7efb94fc9bd08",
  "b2-hidden-results.json": "32b2046ceea906f207726e5f753c7830b2a2b09d078acfcea2708c173cab7d17"
}
```
