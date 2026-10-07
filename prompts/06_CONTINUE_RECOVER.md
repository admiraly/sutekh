# CONTINUATION PROMPT — Resume Sutekh from real state

Resume this project without restarting its design or trusting an old summary blindly. Read `AGENTS.md`, `planning/status.json`, the current task DAG, latest handoff/evidence records, and actual version-control status. Inspect modified/untracked files and active durable job records before doing anything destructive.

Determine the accepted source/artifact revision, current milestone, leased scopes, pending candidates, and exact failing gates. Verify whether recorded commands exist and whether recorded jobs are still running, completed, or stale. Do not start duplicate work for an already-running matching input fingerprint.

Respect other workers' worktrees. Do not reset, clean, overwrite, force-push, delete branches, or reclaim a lease until its work and owner state are reconciled. Recover useful partial changes as isolated candidates. A process started in a previous session is not assumed alive without checking it.

Choose the highest-value ready task on the current milestone's critical path. Continue implementation and relevant tests immediately; avoid re-proposing the same architecture. If M0 has not passed, do not start the renderer, full compiler, or BFME port.

Gameplay edits use the no-native-build runtime path. Native edits still need targeted compilation before acceptance. Queue long validation once, work independently within debt limits, and prioritize repair when a shared interface is broken. Never convert a missing result into a pass to keep momentum.

At the new checkpoint, update accepted versus pending work, command evidence, actual performance results, not-run checks, durable job IDs, and next ready tasks. State precisely what changed during this session rather than repeating the entire project plan.
