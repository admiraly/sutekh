# BFME PROMPT — Build a compatibility slice, not a lookalike assumption

Begin when the orchestrator assigns BFME profile work after the required engine gates. Read `docs/DETERMINISM_AND_BFME.md`, `docs/GAME_PROFILES.md`, the task packet, and the relevant recorded evidence.

The target direction is BFME II and RotWK, multiplayer skirmish first, with mechanics/balance/UI and observed quirks preserved for the selected variant; War of the Ring is excluded. This is separate from the vanilla 1.06 accelerator and from byte-exact Open-BFME reconstruction. Do not overwrite or silently migrate the existing Rust remake.

Identify the exact game/patch/data baseline for every fixture. Do not mix vanilla 1.06 facts with RotWK or other patches. Keep original assets in a configured local installation and exclude them from public commits/CI artifacts.

Start with one evidence-backed behavior slice: a horde's command/formation/targeting/weapon/damage behavior as assigned. Separate confirmed observations, reconstruction evidence, inference, unknowns, and intentional differences. Original definition parsing is not sufficient evidence that engine-side behavior is reproduced.

Implement the behavior through the existing profile/IR interfaces. Unknown data or unsupported semantics must produce explicit compatibility gaps, not silent ignores. Do not replace numeric behavior with fixed point, change tie/order rules, merge member logic, or substitute navigation algorithms merely for speed while still claiming 1:1 compatibility.

Tests distinguish engine regression consistency, cross-backend determinism, and original-game fidelity. Only original evidence supports the third. When original-game testing is unavailable, state that limitation and implement synthetic/reference tests without relabeling them as fidelity proof.

Provide a small reproducible fixture, evidence references and hashes, compatible versus unsupported cases, actual test outcomes, and performance observations. Optimize only after the behavior has a trustworthy reference. Keep BFME-specific semantics out of the universal kernel.
