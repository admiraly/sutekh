#!/usr/bin/env python3
"""Small-agent workflow metrics from this repository's own history (M1-05).

Sources (all local, no network): .git/worktrees/<agent>/ birth time (launch proxy), the last commit
time on each `worktree-agent-<id>` branch (finish proxy), `git diff --shortstat` against the merge
base, whether/when the branch tip became reachable from the main checkout's first-parent history
(integration latency), and the orchestrator-recorded outcome classification in
planning/evidence/m1/agent_outcomes.json. Timing is wall clock; launch is a PROXY (worktree creation)
and finish is the last commit (agents may idle after it). Nothing here measures tokens: token and
tool-call counts were only reported in session notifications and are not stored, so they are null.
"""
import json, os, statistics, subprocess, sys
from datetime import datetime, timezone

ROOT = subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True).strip()
OUT = os.path.join(ROOT, "planning/evidence/m1/agent_workflow.json")

def git(*a):
    return subprocess.check_output(["git", "-C", ROOT, *a], text=True).strip()

def birth(path):
    r = subprocess.run(["stat", "-c", "%W", path], capture_output=True, text=True)
    v = int(r.stdout.strip() or 0)
    return v if v > 0 else None

def iso(ts):
    return datetime.fromtimestamp(ts, timezone.utc).isoformat() if ts else None

def pct(vals, p):
    if not vals: return None
    s = sorted(vals); k = max(0, min(len(s) - 1, int(round(p / 100 * (len(s) - 1)))))
    return s[k]

def main():
    with open(os.path.join(ROOT, "planning/evidence/m1/agent_outcomes.json")) as fh:
        outcomes = json.load(fh)["outcomes"]
    main_branch = git("rev-parse", "--abbrev-ref", "HEAD")
    fp = [l.split() for l in git("log", main_branch, "--first-parent", "--format=%H %ct").splitlines()]
    fp.reverse()  # oldest first
    rows = []
    for ref in git("for-each-ref", "--format=%(refname:short)", "refs/heads/worktree-agent-*").splitlines():
        aid = ref.replace("worktree-agent-", "")
        wt = os.path.join(ROOT, ".git/worktrees", "agent-" + aid)
        launch = birth(wt) if os.path.exists(wt) else None
        tip = git("rev-parse", ref)
        # Own commits = commits reachable from the branch tip made after the worktree was created.
        # (Branch bases and tips from before the history rewrites are no longer ancestors of main, so
        # merge-base arithmetic would be wrong; time since launch is robust.)
        own = git("rev-list", "--after=%d" % ((launch or 0) - 1), ref).split() if launch else []
        ncommits = len(own)
        last = int(git("log", "-1", "--format=%ct", own[0])) if own else None
        stat = git("diff", "--shortstat", own[-1] + "^", own[0]) if own else ""
        # Integration time: first commit on main's first-parent history that contains the tip, or, when the
        # tip predates a history rewrite, that contains a commit with the same subject and author date.
        integ = None
        subj = git("log", "-1", "--format=%s|%at", own[0]) if own else None
        for h, ct in fp:
            if subprocess.run(["git", "-C", ROOT, "merge-base", "--is-ancestor", tip, h]).returncode == 0:
                # a fast-forward keeps the agent's own commit as the integrating commit: its time is the
                # agent's, not the integrator's, so no latency can be derived from git in that case
                integ = int(ct) if h != tip else None
                break
        if integ is None and subj and not any(
                subprocess.run(["git", "-C", ROOT, "merge-base", "--is-ancestor", tip, h]).returncode == 0 for h, _ in fp[-1:]):
            s_, at_ = subj.rsplit("|", 1)
            hit = git("log", main_branch, "--format=%H|%s|%at", "--no-merges").splitlines()
            m = [x.split("|") for x in hit if x.split("|")[1] == s_ and x.split("|")[2] == at_]
            if m:
                for h, ct in fp:
                    if subprocess.run(["git", "-C", ROOT, "merge-base", "--is-ancestor", m[0][0], h]).returncode == 0:
                        integ = int(ct); break
        o = outcomes.get(aid, {"task": "UNCLASSIFIED", "kind": "unknown", "outcome": "unclassified"})
        rows.append({
            "agent": aid, "task": o["task"], "kind": o["kind"], "outcome": o["outcome"],
            "launch_proxy_utc": iso(launch), "last_commit_utc": iso(last),
            "agent_wall_s": (last - launch) if (launch and ncommits and last >= launch) else None,
            "commits": ncommits, "diffstat": stat or None,
            "integrated_utc": None, "integration_latency_s": None,  # not derivable from git history reliably (see caveats)
            "tokens": None, "tool_calls": None,
        })
    rows.sort(key=lambda r: r["launch_proxy_utc"] or "")
    done = [r for r in rows if r["outcome"] != "in_progress"]
    impl = [r for r in done if r["kind"] == "impl"]
    rev = [r for r in done if r["kind"] == "review"]
    def cnt(rs, k): return sum(1 for r in rs if r["outcome"] == k)
    walls = [r["agent_wall_s"] for r in done if r["agent_wall_s"]]
    lat = [r["integration_latency_s"] for r in done if r["integration_latency_s"] is not None]
    # concurrency from launch/finish intervals (finish = last commit; no-work agents excluded: no finish)
    ev = []
    for r in done:
        if r["launch_proxy_utc"] and r["last_commit_utc"] and r["agent_wall_s"]:
            ev.append((datetime.fromisoformat(r["launch_proxy_utc"]).timestamp(), 1))
            ev.append((datetime.fromisoformat(r["last_commit_utc"]).timestamp(), -1))
    ev.sort(); cur = peak = 0
    for _, d in ev:
        cur += d; peak = max(peak, cur)
    summary = {
        "tasks_total": len(done), "implementation_tasks": len(impl), "review_or_audit_tasks": len(rev),
        "impl_accepted_as_is": cnt(impl, "accepted_as_is"),
        "impl_accepted_with_integrator_fix": cnt(impl, "accepted_with_integrator_fix"),
        "impl_partial": cnt(impl, "partial"), "impl_no_work": cnt(impl, "no_work"),
        "reviews_that_found_issues": cnt(rev, "review_found_issues"),
        "impl_first_pass_accept_rate": round(cnt(impl, "accepted_as_is") / len(impl), 3) if impl else None,
        "impl_rework_rate": round((cnt(impl, "accepted_with_integrator_fix") + cnt(impl, "partial") + cnt(impl, "no_work")) / len(impl), 3) if impl else None,
        "agent_wall_s": {"n": len(walls), "min": min(walls) if walls else None, "median": statistics.median(walls) if walls else None,
                         "p95": pct(walls, 95), "max": max(walls) if walls else None},
        "integration_latency_s": {"n": len(lat), "min": min(lat) if lat else None, "median": statistics.median(lat) if lat else None,
                                  "p95": pct(lat, 95), "max": max(lat) if lat else None},
        "peak_concurrent_agents_by_interval_proxy": peak,
        "caveats": [
            "single orchestrator session, model tier fixed (Haiku-class workers), one repository; no controlled comparison and no baseline of other agent counts",
            "launch is a proxy (worktree creation); finish is last commit; integration latency is only derivable when the integrator made derivable from git history; it was not recorded at merge time, so it is null for all rows",
            "outcome classes are the orchestrator's judgement, recorded in agent_outcomes.json before this script ran",
            "tokens/tool calls were not stored; reported as null",
            "this is an observation and does not justify raising the agent count by itself; verification debt and rework rate are the gating metrics"
        ],
    }
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w") as fh:
        json.dump({"report_format": "0.1", "kind": "workflow_observation", "head": git("rev-parse", "HEAD"),
                   "summary": summary, "agents": rows}, fh, indent=2); fh.write("\n")
    print(json.dumps(summary, indent=2))
    unc = [r["agent"] for r in rows if r["outcome"] == "unclassified"]
    if unc:
        print("UNCLASSIFIED:", unc, file=sys.stderr); return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
