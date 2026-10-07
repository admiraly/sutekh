# Determinism and BFME compatibility

## 1. Three separate claims

**Repeatability:** the same build and inputs reproduce a result. **Cross-backend determinism:** allowed VM/native/ISA/platform variants reproduce the same specified result. **BFME fidelity:** the remake matches independently established original behavior for the selected game/version.

These claims require different evidence. A million identical self-replays do not establish original BFME fidelity. Byte-perfect Open-BFME reconstruction, a compatible remake, and a performance-modified original executable are also separate projects with separate acceptance rules.

## 2. Numeric profiles

| Profile | Availability | Contract |
|---|---|---|
| `u32_mod_v1` | M0 | Fully specified modular integer operations in SU-LIR 0.1 |
| `fixed_authoritative_v1` | Later, only after written specification | Explicit scale, width, rounding, saturation/overflow, and conversions |
| `fp32_strict_v1` | Later, not yet defined | Exact op semantics, contraction, subnormal/NaN/zero behavior, conversions, reductions |
| `visual_relaxed_v1` | Rendering/cosmetics later | Documented acceptable numeric error; never silently authoritative |
| `bfme_compat_<variant>` | Profile work | Original behavior/rounding/order backed by evidence; not guessed from engine branding |

Do not rewrite BFME's simulation into fixed point merely because fixed point seems convenient for lockstep. Establish original behavior first. Likewise do not replace multiplication-plus-addition with fused operations or change summation order in a strict profile without equivalence evidence. Compiler floating-point modes distinguish contraction and other transformations [S06].

## 3. Deterministic scheduling

The semantic tick has a fixed duration and recorded command ordering; wall-clock pacing is outside simulation state. M0's demo tick rate is a harness setting, not a claim about BFME's tick frequency. Stable system ordering is resolved from explicit DAG edges with stable IDs as tie-breakers.

Parallel current-row systems may execute chunks in any physical order only when their effects are disjoint and outputs are merged identically. Event queues commit in a canonical order such as phase, system ID, source entity ID, local event sequence. The exact key is versioned and tested. Random numbers use explicit state/streams; never seed from thread ID, time, memory address, or work-stealing order.

Queries involving nearest neighbors, ties, equal priorities, and floating-point reductions need defined tie-breaks and iteration. Optimizing an order-sensitive algorithm is a semantic change unless evidence proves equivalence.

## 4. Replay evidence

A replay names the initial checkpoint, game/profile version, schema version, semantic manifest, content hashes, seed/state, tick sequence, ordered inputs, and semantic-revision events. The tool can stop at a selected tick, fork both revisions from the same checkpoint, and report the first differing system/entity/field.

Hashes are useful for localization. Store enough context to reconstruct and compare actual divergent values. A coarse per-tick hash does not automatically identify the causal instruction. Bisection and instrumented reruns may be required.

Regression replays test engine behavior. Compatibility fixtures must additionally cite an original-game trace, documented data interpretation, or reviewed reconstruction evidence. Mark evidence as confirmed, inferred, unknown, or intentionally different.

## 5. BFME scope carried forward

The intended remake profile targets **BFME II and Rise of the Witch-king**, with multiplayer skirmishes prioritized. War of the Ring is excluded. Preserve mechanics, balance, UI behavior/appearance, campaigns as part of eventual completeness, and observed original quirks where the remake contract requires them. Original assets are loaded from the user's local installation. Networking internals may differ for performance; compatibility with original network clients is not assumed.

The user's **vanilla 1.06 accelerator** is a different project. Do not infer from that project's target that every future remake fixture must use vanilla 1.06. Every compatibility run records its exact BFME II/RotWK edition, patch, data set, and enabled modifications. Select one documented baseline per fixture set rather than mixing editions.

Existing Rust remake code is not discarded. Evaluate reusable format knowledge, behavioral tests, schemas, and private importer utilities at defined adapter boundaries. Do not require wholesale language migration before proving the new substrate.

## 6. Profile boundaries

The generic core owns columns, scheduling, handles, bounded effects, replay, and execution. The RTS profile owns hordes, formations, spatial indexing, navigation patterns, visibility, and command semantics. The BFME adapter owns interpretation of original definitions and corresponding behaviors: weapons, armor, locomotors, upgrades, experience, powers, command sets, production, economy, construction, and scripts.

Original INI-like data may drive these systems, but parsing it does not reconstruct engine-side behavior automatically. Unknown keys or unresolved behaviors produce explicit diagnostics and compatibility gaps; silently ignoring them cannot pass fidelity acceptance.

Do not replace horde/member navigation with flow fields, merged weapon logic, or different collision rules and label it 1:1 without differential evidence. A separate enhanced profile may intentionally change behavior, but it must not contaminate compatibility mode.

## 7. Compatibility fixture format

A fixture records game variant, input command/state, relevant original data hashes, expected observed outputs, tolerance policy where justified, evidence reference, confidence, exclusions, and reason for any intentional difference. Public fixtures use synthetic assets/data where possible. Proprietary files and extracted assets remain private and are not uploaded by CI.

A useful first set covers one horde's orders, formation changes, target selection ties, attack timing, damage/armor interaction, experience/upgrade transition, death/member replacement, and a simple deterministic replay. These are test categories, not assertions that the original algorithms are already known.

## 8. Networking

Introduce recorded command playback before real networking. Then test multiple headless peers under controlled delay, loss, duplication, and reorder. The session pins semantic/content versions and compares state. Transport reliability, malicious input validation, authentication, command limits, and resynchronization are separate requirements from determinism.

Lockstep reduces the need to replicate all unit transforms but can couple advancement to command availability. Snapshot/server-authoritative and prediction-based profiles are available for other games. No engine-level claim that one model eliminates all latency is valid.

## 9. Acceptance rule

An optimization-only BFME patch may not change confirmed observed semantics. A fidelity correction may deliberately change previous remake results, but must carry new evidence and reviewed expectation changes. Distinguish `regression_pass`, `cross_backend_pass`, and `original_compatibility_pass` in reports; never substitute one for another.

Sources for numeric/tooling facts: [SOURCE_NOTES.md](SOURCE_NOTES.md). The BFME scope above is a user requirement, not a claim derived from current original-game implementation inspection.
