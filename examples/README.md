# Executable specification examples

`movement.capsule.json` declares one system's schema/effects. `movement.lir.json` implements `position_x += velocity_x` modulo 2^32. `movement_v2.lir.json` intentionally doubles the velocity contribution and is a different behavior revision for hot-swap testing; it is not an equivalent optimization.

`world.json` contains four rows including a wraparound case. Each case in `movement.test.json` starts from the original fixture independently. The pack checker evaluates these examples with a small Python reference model; it does not implement a native runtime or verify live process replacement.

`invalid/` contains intentionally invalid JSON/IR. `negative_cases.json` names expected rejection codes. Do not “fix” these files to make the whole directory load as valid: successful rejection is their purpose.

`report_unmeasured.json` is schema-valid and deliberately contains no fake engine results. Runtime, performance, and original BFME evidence are not run by this package.
