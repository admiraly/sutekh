# Game profiles and scope boundaries

The core is a reusable execution system. A profile selects numeric semantics, scheduling, authority, data families, budgets, and permitted specialization. Profiles share infrastructure without pretending all games need the same simulation model.

## BFME II / RotWK remake

Priority is exact gameplay behavior for the chosen variant, multiplayer skirmish first, then eventual campaign/UI completeness, excluding War of the Ring. Hordes, formations, commands, weapons, upgrades, original-data interpretation, and determinism are profile responsibilities. Assets come from a local installation. Original-client network compatibility and original save/replay formats are not assumed requirements.

Do not migrate or replace the existing Rust remake until the new engine has an independently useful proved slice and a deliberate integration plan. Reuse evidence and tests before rewriting already understood systems. The new engine is also separate from binary optimization of the original 32-bit game.

## Shatterfront

A large RTS profile with asymmetric factions, commanders, economy, construction, strategic AI, formations, fog of war, and large battles. It may choose improved pathfinding or modernized mechanics that would be impermissible in a BFME 1:1 compatibility profile. Share RTS infrastructure where semantics truly match, not by coupling faction-specific logic to the core.

## RED HORIZON

A large-scale co-op FPS/RTS battlefield profile: direct player control plus many AI actors, projectiles, explosions, audio, and effects. Distinguish authoritative combat actors from lower-cost visual population. Use explicit simulation/animation/AI level-of-detail policies when allowed by gameplay. Those approximations are not automatically acceptable in BFME.

The game project's GPLv3 decision remains attached to that project. Engine licensing is a separate owner decision. No specific future unit/particle count or frame rate is guaranteed by these architecture documents.

## Slingshot

A latency-focused physics/projectile FPS profile with readable high-speed movement, ricochets, prediction, and replay/rollback investigation. High tick rates are a workload/physics budget decision, not a blanket engine default. Deterministic or reconcilable collision rules need their own tests; author iteration must not force renderer or networking restarts for every gameplay edit.

## MMO

A headless server/zone profile with long-lived state, snapshot/replication policies, authority, persistence, security, and migrations across versions. This is later work. It must not impose databases, distributed consensus, cross-zone ownership, or live production migrations on M0.

## Shared contracts

All profiles use stable schema identities, bounded effects, structured diagnostics, a reference path, semantic versioned artifacts, isolated candidate validation, and resource-aware scheduling. Rendering and AI quality may vary by profile; authoritative rules cannot silently degrade under load unless the game explicitly defines that behavior.

These profile descriptions carry forward the user's project direction. They do not assert that those games or their subsystems have been implemented in Sutekh.
