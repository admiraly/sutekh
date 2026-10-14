# M3 runtime limits v2 — owner-authorized development amendment

Authorization, 14 October 2026: owner selected "Prepare a versioned fix and validate it" for OD6/OD15, then stated "you may change limits if it helpy development". This amendment records the concrete changes; it does not rewrite the root architecture/specification files. Baseline failures remain in the earlier evidence records.

## OD6 — versioned operator budget and sampling

`rts_ops_v2`, operator_version2, raises the examined-entry budget from128 to160; cell size64, grid512, ring order, inclusive range, lowest-slot tie break, and saturating scatter behavior stay fixed. Both operator constants texts and constants_hash values receive v2 identities. The native host loader requires a name consistent with descriptor operator_version, and the capsule schema permits v1/v2 names. Test-only v1 descriptors still load; production v2 descriptors refuse old v1 capsules. No cross-version replay/recovery equivalence is claimed. Standalone model operator identity also changes to0.2, explicitly pinning v2/K160.

Candidates128,160,192,256,384,512 were measured on the unchanged committed16k scene, same seed20261012, same1024 queriers. K128 missed2 of938 oracle-positive; K160 and higher missed0 and chose no non-nearest targets. K160 is the smallest tested improvement, not a proof for arbitrary scenes. The0.1%,2%,p95<=3 gates remain unchanged. No tuning by changing seeds or scene locks.

Sampling is **up to1024 distinct armed units without replacement**, using all eligible units when fewer exist. The committed1024-row scene has964 eligible units; actual sample counts are always reported. This resolves the impossible literal1024-unique-querier requirement without repeating samples or modifying the scene. Thin-margin reporting remains intact.

Boundary fixtures now use the declared K for exact K/K+1 admission. The clipping fixture uses K+2 far-cell occupants so a wrapped-box bug remains detectable after the budget changes. Original assertions are retained, not waived. All original scene hashes remain unchanged.

## OD15 — bounded JSON snapshots and recoverable loading

Snapshot0.2 encoding remains identical; its input/output cap rises from64MiB to512MiB. Prior0.1/0.2/0.3 parsing and canonical strictness remain supported. The binary defaults to a2GiB spool, one retained checkpoint, and a32-tick checkpoint interval. Library configuration defaults remain unchanged. `SUTEKH_CHECKPOINT_RETAIN=1..8` provides an explicit binary override; invalid retention values fail startup. Spool reservations and atomic admission checks remain enforced.

The spawn high-water ceiling becomes1,000,000, matching the retained SU_LIMIT_ROWS ABI limit; SU_LIMIT_ROWS is not raised. Lowest-slot growth is computed exactly: reusable holes = high_water-alive-retired, then hw_after = hw+max(0,rows-holes). Free capacity excludes retired slots. Thus a despawned slot can be reused at the ceiling while retired slots cannot. The Python precheck mirrors this formula. At default fixed1M capacity, a separate above-ceiling condition is redundant with capacity validation; the old M3-M31 detector/gate must not be falsely claimed unchanged.

Updated scale acceptance:1M legal rows through977 spawn_batch calls of1024 rows except the final short call; retained spawn events/dropped/sequence counts match; full canonical capture matches all39 initial columns and entity slots; a full-capacity extra batch refuses atomically; despawn/reuse at the ceiling succeeds. Capture at838,860 is checked at library level, with1M supervisor capture covering the larger boundary. The old838,860 refusal is superseded, not silently treated as passed.

The512MiB byte bound and2GiB spool admission are budgets, not claims that arbitrary largest-schema snapshots fit memory or meet time targets. Worker address-space default remains4GiB. Recovery claims are restricted to actually validated cases. The long broker job requests a real1M replacement-worker restore and tick; any resource failure remains a failed gate. No timing qualification, unlimited memory, or abstract performance guarantee.

## Evidence and integration

Development is isolated in /root/sutekh-limits and /root/sutekh-scale-v2. Native compilation and focused tests precede the long scale job. Immutable artifact/source identities and failed attempts are retained in planning/evidence/m3-versioned-limits and m3-scale-v2. No acceptance declaration precedes validation. Contributor/IP policies and third-party notices are unchanged.

## Restore resource validation

The first complete Release load accepted 1,000,000 rows and captured 127,345,544 canonical bytes, but replacement-worker recovery failed. A direct restore under the unchanged 4 GiB address-space limit reproduced an out-of-memory error: the header-schema parse tree remained live while a second full parse tree was built. The repair copies the bounded schema names, releases the first tree, then performs the strict canonical parse. It also enforces the byte cap before the header parse. No encoding or canonical validation is relaxed.

The repaired direct million-row capture/restore/re-encode test passed all 20 checks, with peak resident memory 2,456,420 KiB and total elapsed time 9.84 seconds. This is library evidence, not supervisor recovery acceptance. The binary request window is explicitly 60,000 ms (the existing supported maximum), while library defaults remain unchanged. The full broker rerun uses immutable source/artifact hashes and must pass replacement-worker recovery before that gate is accepted. The earlier failed run remains recorded.

Independent reviewer `/root/review_limits_v2` found no blocking source defect and passed ten prerequisite-inclusive focused tests. Approval remains conditional on full recovery and dependent validation; suggested oversize and sanitizer checks are pending.
