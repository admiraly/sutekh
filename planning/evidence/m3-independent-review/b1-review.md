# Independent B1 review

Reviewer /root/review_limits_v2, implementation author /root,2026-10-14. Candidate-only audit of generator and three emitted IR/capsules against contractsections5/11, layering on reviewed B3+B2. No source edits.

No blocking implementation defect found under the explicitly recorded liveunit interpretation (0<hp<=3). Goal stores select the team base after normal goal computation, so retreat overrides explicit orders and formation goals. Basecoordinates are valid domain values. Move selects the already-clamped next-position branch regardless of target stop while retreating; ordinary behavior retains B2 stop predicate and B3 boost. Combat replaces only new-fire predicates with hp>3, preserving cooldown countdown, active projectile countdown/arrival and outgoing damage of an earlier shot. It does not erase projectiles when health falls.

Capsules declare hp reads in all three systems and team read in goal. Goal/move beforedamage adds the explicit required order; combat-before-scatter-beforedamage orders its hp read. Existing spatial/goal/combat/move edges remain. Registers use distinct b1 prefixes; stores remain onceperfield. No native opcode/allocation/capability change is introduced. Generator writes candidatefolder only, with assert guarding the specific baseline fire instruction it transforms.

Independent literal IR interpreter checked10,000 seeded rows for each goal,combat,move against direct expectations, including hp0/1/2/3/4/65535, teams0/1, orders0/1/2 and existing projectiles. All30,000 module comparisons passed, seed20261014, exit0. Lowhp arrival cases confirmed existing projectiles still emit damage. Goal/move hp0 retain predecessor semantics. Combat hp0 has no newfire; legal hp0anchors have range0 and no validtarget, so baseline equivalence follows for legal profile anchors.

Inspected recorded hidden fixture evidence: B1 passes all earlierB3/B2 and newB1 checks, baseline rejects newchecks, and B3+B2 predecessor rejects B1 checks while preserving earlierchecks. Fixture author is distinct and locked expectations predate candidate evaluation. Fullschedule installation and required existing suite remain separate acceptance gates.

Interpretation limitation: section11 says unit hp<=3 while hp0 anchors are excluded from hiddenfixtures. Restricting retreat to positivehp liveunits is consistent with unit spawn domains and avoids changing anchorbehavior, but must remain documented. Team0/1 only, as normative profile requires. PriorB3Euclidean metric limitation remains. No wholeM3 acceptance or performanceclaim.

## Reviewed SHA256s

```json
{
  "build_b1_candidate.py": "9a7beff54886cbe393820c069f96eded119df7de9182d6ea92734816cdb0984b",
  "b1-hidden-results.json": "4e7f0581f0165b15cd8523c7b888b0eea40c40c58e3587cc1e02fe7cb52d40fc",
  "hidden/manifest.json": "43c05f8688807a4a2d6f5fd4716a0b65ec5082c41f1ad31a3aa7efb94fc9bd08",
  "candidates/b1/goal_b1.capsule.json": "5d704e51e91ccd3be692a520008d6cf8def1b3b0b7f279869cd40ff09238b7f8",
  "candidates/b1/goal_b1.lir.json": "8853c9c7863a06e34b7163f061d821ee1eaf4f21ec9e90f50361ec820f3b13e9",
  "candidates/b1/combat_b1.capsule.json": "4598fe0372829b49350d2bcca8c9be6dcb794f483f02643625f3fefaee0583ca",
  "candidates/b1/combat_b1.lir.json": "db406e9d4afd8f4abe38eccba9867be9af35a7f94b0a55b7bf45fed1d6614ad8",
  "candidates/b1/move_b1.capsule.json": "5f62070743658421125634a5ca1d74509feaca1fefe8e576979c39b69b73cd89",
  "candidates/b1/move_b1.lir.json": "c879901d9cb0da080a43a538516ae51ffc4629fa4a9af264e8d10665dc74e8e6"
}
```
