# Independent M3-06 mutation inventory review

Reviewer /root/review_limits_v2; audit 2026-10-14 at canonical main 2b32abd0a38e0791e2e33737cc297bb081bd8dd5. Audit-only, no source changes or heavy campaigns. Read normative section 15, scripts, recorded results and v2 amendment. Existing test execution is distinguished from mutation execution; no missing campaign is inferred passed.

## Finding

M3-06 complete mutation inventory is not yet closed. Strong recorded hostoperator, reference and IR-program campaigns exist; live runtime/control/replay and schedulerhost mutations remain. Some contract entries specify multiple alternatives, and a single killed alternative does not prove every variant. See exact scope below.

Evidence identities: native v2 campaign log is authored in /root/sutekh-limits, candidate based5de9782 with operator 2/K160,25 total, 23 killed, 2 equivalent. Historical mutation refresh result names base cfe4f46 and source/artifact hashes; reference-mutants.json is historical K128, while v2-reference-final.log shows current K160 reference suite22 tests passed, including mutation test expectations. Native checked-IR records contain21 named kills plus systematiccampaign results, independently distinct from Python IR evaluator mutations. CTest rts_ref_m3_mutants invokes six reference control mutants, not runtime handler mutations. M27 actualsupervisor acceptedIR mutation is preserved in immutable historical JSON and a current runnable CTest detector; refreshed ordinary tests do not rewrite its historical artifact identity.

Legend: model means mutated Python semantic model; IR-program means mutation interpreted by reference evaluator unless native checkedIR is explicitly stated. Kill does not automatically imply real RPC mutation or unchanged-current-source acceptance. Missing persisted output means evidence gap even when a ready harness exists.

| Contract mutant | Recorded status | Exact scope / caveat |
|---|---|---|
| M3-M01 | Native v2 kill and Python model kill | tie reverse; native test_rts_ops/diff; ref check_spatial_known_answers |
| M3-M02 | Native v2 kill and Python model kill | column+1; native knownanswer/diff; ref spatial/duel/oracle |
| M3-M03 | Native v2 kill and Python model kill | strict range <; knownanswer equality |
| M3-M04 | Native v2 kill and Python model kill | ignore budget; additional nativeM04b truncated at exactK killed |
| M3-M05 | Native v2 gather and scatter generation kills; model gather kill | M05s scatter plus modelM05b despawn generation variant |
| M3-M06 | Model kill; native equivalent at published boundary | native deadtarget write erased by zero_dead before publish; defence retained |
| M3-M07 | Native v2 kill and model kill | accept kind2 targets |
| M3-M08 | Native v2 M3-ADD kill and model kill | wrapping addition |
| M3-M09 | Native v2 kill and model kill | drop all but first contribution |
| M3-M09b | Equivalent in native and model | descending saturating-add order, not detected |
| M3-M10 | Model kill for earlierarrival only | proj_left==2 instead1; literal laterarrival alternative not evidenced; nativeVM gameplay runtime mutation not recorded |
| M3-M11 | Model reload-offbyone and IR-program reload-offbyone kills | blockedcooldown/underflow variants not separately evidenced |
| M3-M12 | Model and IR-program kills; native checked-IR differential N03/N10 record | incoming notzero/store dropped; droppedstore may be structural invalidity, not acceptedsemantic kill |
| M3-M13 | Model anchor-kill and IR-program kills; native checkedIR N07 record | doublekill variant unrun; N08 missingkill extra variant recorded |
| M3-M14 | Model/IR-program kill; native checkedIR N01 record | wrapped-negative clamp removal |
| M3-M15 | Model/IR-program kill; native checkedIR N05 record | step min removed/reversed |
| M3-M16 | Model/IR-program kill; native checkedIR N06 record | mode2stop removed |
| M3-M17 | Generator mutation harness exists; no isolated persisted campaign results found | PRNGconstant and modulo variants listed; positive scene lock validation is insufficient to claim mutantkills |
| M3-M18 | No actual replay code mutation result found | missingdigest/out-of-order input refusal tests exist; cannot equate inputnegative tests with verifier skip/ignore code mutation |
| M3-M19 | No actual runtime revision-bypass mutation result found | stale revision regressionpositive exists |
| M3-M20 | Reference deadsendergate kill; related runtime broadzerooff kill | reference explicitly not literal staleprojectile clearing mutant; runtime su_zero_slot bypass caught rts_churn, but exact projectile-only variant needs named evidence |
| M3-M21 | Native v2 kill and model kill | index kind2 rows |
| M3-M22 | Native v2 kill and model kill | same-team filter |
| M3-M23 | Native v2 row and column clipping kills; model columnwrap kill | v2modelfixture increased toK+2 for same defect; all assertions retained |
| M3-M24 | Lowlevel native/model kill on nonadmitted state; reachablecontrol equivalence remains | forged deadslot currentgeneration bypasses inputadmission; Q2 refuses it; do not overclaim reachablegameplay kill |
| M3-M25 | Model and native checkedIR N04 kill | dmg_out everytick; model/IR reference differential evidence |
| M3-M26 | No actual native schedule deadrowzeroing mutation result found | host_dead_rows_not_zeroed harness exists; lowlevel scatter campaigns do not modify schedule zeroing |
| M3-M27 | Real supervisor acceptedIR semantic mutant kill | retained proj_target_gen arrival; recorded per-tick modelcomparison mismatch and restoredpositive exit0 |
| M3-M28 | No actual capbelowcapacity mutation result found | rts_caps_engine positive cap/maxdeath checks not mutation evidence |
| M3-M29 | No persisted host-effects-ignore campaign result found | run_host_mutants.sh host_effects_ignored exists; regressionpositive hosteffects conflict test not mutantkill |
| M3-M30 | Reference spawnflight0/system-ownedfield kills via CTest; runtime variants unrun | m3_control_mutants model only; set_fields domain/writable bypass needs actualnative/RPC mutation |
| M3-M31 | Ownerauthorized v2 superseded/redundant guard | 1M highwater ceiling equals SU_LIMIT_ROWS; no admitted capacity permits exceeding it; old 838860 mutant detector invalidated explicitly, not countedkilled |
| M3-M32 | No persisted staging-copy-removal campaign result found | run_host_mutants.sh host_staging_copy_removed exists; positive hostrestore/entry tests not mutation evidence |
| M3-M33 | Actual runtime capacity mutant kill recorded | entity_tick ps[i] global1 instead per-systemmax; rts_capacity exit8; restored/sanitizer exit0; historical pre-v2 source |
| M3-M34 | No persisted hostrestore branch mutation result found | run_host_mutants.sh restore_host_branch_removed covers host paths.source variant; skipregistry alternative needs separate evidence |
| M3-M35 | Reference spawnreload/formationgen variants via CTest; runtime set_fields variants unrun | m3_control_mutants six modelmutations; omittedreload/formationgen runtime mutation campaign needs exactcoverage |

## Reachability and version corrections

M24: old reference/native log prose says deadcurrentgeneration is reachable through control; that rationale became stale after Q2 liveness enforcement. A retained formation handle becomes stale when anchor despawns; forging the bumped currentgeneration fails liveness admission. Lowlevel operator tests may construct this defensive state and legitimately kill the alivecheck removal, but that is not a reachable live-control detector. Preserve logs and annotate this distinction, rather than replacing genuine old measurements. No claim that all hypothetical snapshots/plugins preserve this admission invariant.

M06: model computes/stores different deadrow intermediate state and detects missingtargetalive gate. Actual native publish path erases deadrow columns; recorded native mutation equivalence is honest within that observation boundary. It does not justify removing defensivecheck.

M31: ownerauthorized limitsv2 replaces old 838860 refusal with 1M ceiling, exactholegrowth and fullcapacity refusal/reuse gates. Since fixedABI capacity <= 1M, removing only highwater ceiling cannot create above 1M admittedworld. Mark old gate superseded and new singlecheck mutant redundant, not killed. Spoolreservation and retired/freecapacity mutations are independent meaningful admission targets.

## Required next actual campaigns

1. Isolated scheduler/host campaign: M26 deadrowzeroing, M29 effectsignore, M32 stagingcopyremove, M34 restorehostbranch/registry variants. Existing shell harness includes several, but archive source/artifact hashes, exit outcomes and restoredpositive checks. A compiler failure should be classified separately from a semanticdetector kill.
2. Isolated live control campaign: M19 revisioncheck, M30 flightdomain/system-ownedset_fields, M35 reloadrequired/formationgen set_fields. Add exact required alternatives; Pythonmodel mutants are supporting oracle coverage.
3. Replay validator code campaign: M18 skipdigest and ignoresequence, using real replayrecords; inputcorruptiontests alone do not prove detector sensitivity.
4. Capconfig M28 and native/runtimeprojectileclear M20 exactvariants. M33 historicalkill is already evidenced but toucheddependent integration requires coherentcurrent artifact verification.
5. Record generator M17 exact constant/modulo mutation outcomes; harness exists but no standalone persisted campaign result was located. Complete missingM10 late arrival, M11 blocked/underflow, M13 double kill variants if inventory means all enumerated alternatives.

No whole inventory PASS recommendation until these are resolved, explicitly equivalent with reason, or owner-versioned superseded. Source/artifacthashes belong to exact campaigns; current green regression cannot be retroactively called a mutantkill.

## Reviewed evidence/source SHA256s

```json
{
  "planning/contracts/M3_RTS_CONTRACT.md": "4208f9264bb44135d56408df8a4f26abf98957c6b7d5539ceff2210429a585d4",
  "planning/amendments/2026-10-14-m3-runtime-limits-v2.md": "797e5e8e279cda307db5330737198fdfe72a96ced667abbfb281f1ca2f337138",
  "planning/evidence/m3-versioned-limits/native-mutants-v2.log": "c0b93ef4dedc6006495217792167c6a1ba3194a0bb0781a19ca2d1b87f9662ce",
  "planning/evidence/m3-versioned-limits/v2-reference-final.log": "a649dabf297c6ad1087dba5093900c4936c38f1e9a60d6c4189933bc696bcf95",
  "planning/evidence/m3-native-mutation refresh/reference-mutants.json": "e1736573cbf40b96e9d2e1351bf16f52b536d83b9ac97184dde67eeda6fadadf",
  "planning/evidence/m3-native-mutation refresh/result.json": "ba249b5ff7a8c14db758cb0314665ce8d989f9a46251d163b47f2e76d9a0f762",
  "planning/evidence/m3-capacity-churn/mutations-final.json": "d82fa0cac9e8c97e3a59aa1d2bf85370649b812e348fe9fbf7c457186366f82c",
  "planning/evidence/m3-e2e-targeting/M3-M27.json": "b7be19f49663bcd11fd78fa8865459833e1b7b53a58ecd7ba2e6edcefbc7a865",
  "tests/rts/ir/records/mutants.json": "adfc1efdbf60bc760e53af285eba18d5c4b0f599506b95dcba40f1a7640763db",
  "tests/rts/ref/m3_control_mutants.py": "83d48f78669707ac26fe8bf5aaaf212f65635c449b1032ac9a70feb764f0b7f4",
  "tests/scheduler/run_host_mutants.sh": "3dca69ad633142e739476e44e81fc8a712f73f89029416ab16ab2512a83ddfa6",
  "tests/rts/gen/mutants.py": "e2fd48e799f777d0690158380e6fedacfc7fa0585bf4ae875c3c11dea38ecf53",
  "tests/rts/replay/test_replay.py": "15a8824662c192f60ce406b9fe0567ee25748ba274c15f53d0bba41eb0c332cf"
}
```
