# BFME evidence registry (M4-00)

Provisional variant selection, reversible by the owner:

- Variant: BFME II, including the Rise of the Witch-king expansion, latest official patch.
- Status: provisional. The owner chose this ("you decide" on open owner items), and may change it.
- The registry format is variant-agnostic. Changing the variant means editing the `variant` block of `evidence.json` and nothing else.

Rules for the registry:

- Every behavior entry carries a version label (`variant` and `patch`), a status of `confirmed`, `inferred` or `unknown`, and a source reference.
- A `confirmed` entry must name its source. Fidelity claims and regression results are kept in separate sections.
- No original game files, assets or hashes of them go into this repository. Sources point to the owner's local data by label only.
- This directory holds the format and the test harness only. It contains no behavior claims yet, because no evidence has been collected.
