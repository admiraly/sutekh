#!/usr/bin/env python3
"""Mutation check for the M3-08a accounting API (test_world_acct).

Each mutant is one exact text edit applied to a scratch copy of the world sources. The
accounting test (compiled with the counting seam) must fail on every killable mutant. Mutants
marked EQUIVALENT are semantically identical to the reference and are expected to survive; they
are reported, not counted as kills.

Usage: python3 -I tests/world/acct_mutants.py <repo_root> <work_dir> [--asan]
Writes only under <work_dir>. Exit status 0 when the baseline passes, every killable mutant is
killed, and every mutant edit applied exactly once.
"""
import os
import shutil
import subprocess
import sys

WORLD = "src/world/su_world.c"
ENTS = "src/world/entities/su_ents.c"
CMDQ = "src/world/entities/su_cmdq.c"

# (id, category, file, old, new, kind)  kind: KILL or EQUIV
MUTANTS = [
    ("K01", "columns_staging", WORLD,
     "if (w->staging[i] != NULL) {",
     "if (1) {", "KILL"),
    ("K02", "columns_committed/working rows", WORLD,
     "rows = w->row_count > 0u ? w->row_count : 1u;",
     "rows = w->row_count;", "KILL"),
    ("K03", "columns_working", WORLD,
     "out->columns_working += col_bytes;",
     "", "KILL"),
    ("K04", "entity_table (working copy)", WORLD,
     "+ su_ents_memory_bytes(w->ents_working);",
     ";", "KILL"),
    ("K05", "metadata (name NUL)", WORLD,
     "meta += (uint64_t)strlen(w->fields[i].name) + 1u;",
     "meta += (uint64_t)strlen(w->fields[i].name);", "KILL"),
    ("K06", "metadata (lineage)", WORLD,
     "meta += (uint64_t)w->lineage_count * sizeof *w->lineage;",
     "", "KILL"),
    ("K07", "metadata (retired IDs)", WORLD,
     "meta += (uint64_t)w->retired_count * sizeof *w->retired;",
     "", "KILL"),
    ("K08", "metadata (writable bindings)", WORLD,
     "meta += rd * sizeof *w->inv_bindings + wr * sizeof *w->out_bindings;",
     "meta += rd * sizeof *w->inv_bindings + 0u * wr * sizeof *w->out_bindings;", "KILL"),
    ("K09", "metadata (schema NUL)", WORLD,
     "meta += (uint64_t)strlen(w->schema_id) + 1u;",
     "meta += (uint64_t)strlen(w->schema_id);", "KILL"),
    ("K10", "total (metadata excluded)", WORLD,
     "out->entity_table + out->metadata;",
     "out->entity_table;", "KILL"),
    ("K11", "entity_table (summary array)", ENTS,
     "n += (e->summary_words > 0u ? e->summary_words : 1u) * sizeof(uint64_t); /* summary */",
     "/* summary omitted */", "KILL"),
    ("K12", "entity_table (alive/avail word count floor)", ENTS,
     "words = su_words_for(e->capacity);",
     "words = e->capacity / 64u;", "KILL"),
    ("K13", "entity_table (chunk arrays, one of two)", ENTS,
     "n += 2u * chunks * sizeof(uint32_t);",
     "n += 1u * chunks * sizeof(uint32_t);", "KILL"),
    ("K14", "cmdq structural (per-tick cap times buffers)", CMDQ,
     "sbuf_n = q->off_s[nb] > 0u ? q->off_s[nb] : 1u;",
     "sbuf_n = q->per_tick_s * nb;", "KILL"),
    ("K15", "cmdq scratch (fin omitted)", CMDQ,
     "3u * q->per_tick_e * sizeof(SuEvent);",
     "2u * q->per_tick_e * sizeof(SuEvent);", "KILL"),
    ("K16", "cmdq bookkeeping (queue struct)", CMDQ,
     "+ (uint64_t)sizeof *q;",
     ";", "KILL"),
    ("K17", "event log ring", CMDQ,
     "return (uint64_t)sizeof *log + log->capacity * sizeof(SuEvent);",
     "return (uint64_t)sizeof *log;", "KILL"),
    ("E01", "equivalent: max(rows,1) spelled as rows + (rows == 0)", WORLD,
     "rows = w->row_count > 0u ? w->row_count : 1u;",
     "rows = w->row_count + (w->row_count == 0u);", "EQUIV"),
    ("E02", "equivalent: total summed in a different order", WORLD,
     "out->total = out->columns_committed + out->columns_working + out->columns_staging +\n"
     "                 out->entity_table + out->metadata;",
     "out->total = out->metadata + out->entity_table + out->columns_staging +\n"
     "                 out->columns_working + out->columns_committed;", "EQUIV"),
    ("E03", "equivalent: ceil(capacity/64) written as quotient plus remainder test", ENTS,
     "words = su_words_for(e->capacity);",
     "words = (e->capacity / 64u) + (e->capacity % 64u != 0u ? 1u : 0u);", "EQUIV"),
]


def apply_mutant(work, mid, fname, old, new):
    path = os.path.join(work, fname)
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    count = text.count(old)
    if count != 1:
        raise SystemExit("mutant %s: pattern found %d times in %s" % (mid, count, fname))
    with open(path, "w", encoding="utf-8") as f:
        f.write(text.replace(old, new))


def build_and_run(repo, work, asan):
    inc = [os.path.join(work, "src"), os.path.join(work, "include")]
    flags = ["-std=c17", "-g", "-O1", "-Wall", "-Wextra", "-pedantic", "-Werror"]
    if asan:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    seam = os.path.join(work, "tests", "world", "acct_seam.h")
    exe = os.path.join(work, "test_world_acct")
    # Seam only for the three world sources: compile them separately, then link.
    objs = []
    for src in [os.path.join(work, WORLD), os.path.join(work, ENTS), os.path.join(work, CMDQ)]:
        obj = src + ".o"
        c = ["cc"] + flags + ["-c"]
        for d in inc:
            c += ["-I", d]
        c += ["-include", seam, src, "-o", obj]
        r = subprocess.run(c, capture_output=True, text=True)
        if r.returncode != 0:
            return "build-failed", r.stderr[-800:]
        objs.append(obj)
    for src in [os.path.join(work, "src", "ir", "su_sha256.c"),
                os.path.join(work, "tests", "world", "test_world_acct.c")]:
        obj = os.path.join(work, os.path.basename(src) + ".test.o")
        c = ["cc"] + flags + ["-c"]
        for d in inc:
            c += ["-I", d]
        c += [src, "-o", obj]
        r = subprocess.run(c, capture_output=True, text=True)
        if r.returncode != 0:
            return "build-failed", r.stderr[-800:]
        objs.append(obj)
    link = ["cc"] + flags + objs + ["-o", exe]
    r = subprocess.run(link, capture_output=True, text=True)
    if r.returncode != 0:
        return "build-failed", r.stderr[-800:]
    r = subprocess.run([exe], capture_output=True, text=True, timeout=600)
    return ("pass" if r.returncode == 0 else "fail"), (r.stdout + r.stderr)[-400:]


def fresh_copy(repo, work):
    if os.path.isdir(work):
        shutil.rmtree(work)
    os.makedirs(work)
    for d in ["src", "include", os.path.join("tests", "world")]:
        shutil.copytree(os.path.join(repo, d), os.path.join(work, d))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    repo = os.path.abspath(sys.argv[1])
    base = os.path.abspath(sys.argv[2])
    asan = "--asan" in sys.argv[3:]
    ok = True

    fresh_copy(repo, os.path.join(base, "baseline"))
    status, out = build_and_run(repo, os.path.join(base, "baseline"), asan)
    print("baseline: %s" % status)
    if status != "pass":
        print(out)
        return 1

    killed = survived_equiv = 0
    for mid, cat, fname, old, new, kind in MUTANTS:
        work = os.path.join(base, "m" + mid)
        fresh_copy(repo, work)
        apply_mutant(work, mid, fname, old, new)
        status, out = build_and_run(repo, work, asan)
        if kind == "KILL":
            if status == "fail":
                killed += 1
                verdict = "KILLED"
            else:
                ok = False
                verdict = "SURVIVED (BAD)"
        else:
            if status == "pass":
                survived_equiv += 1
                verdict = "survived (equivalent, expected)"
            else:
                ok = False
                verdict = "KILLED (equivalent mutant wrongly distinguished, BAD)"
        print("%-4s %-45s %-10s %s" % (mid, cat, status, verdict))
    n_kill = sum(1 for m in MUTANTS if m[5] == "KILL")
    print("killed %d of %d killable mutants; %d equivalents survived as expected"
          % (killed, n_kill, survived_equiv))
    return 0 if ok and killed == n_kill else 1


if __name__ == "__main__":
    sys.exit(main())
