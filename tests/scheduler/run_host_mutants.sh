#!/bin/sh
# Mutation check for the M3-04a host-entry tests (tests/scheduler/test_host_entry.c, contract 10.7 and 15).
#
# Each mutant is one deliberate bug applied to a SCRATCH copy of the tree (never to the repository). The
# test_host_entry target must fail to build or fail at run time for every mutant. A mutant that still passes
# is reported as SURVIVED: either an equivalent mutant (recorded as such by the caller) or a gap in the tests.
# A mutant whose substitution does not apply is reported as NOT APPLIED and counts as unrun.
#
# Usage, from the repository root:
#   sh tests/scheduler/run_host_mutants.sh <scratch-dir>
# <scratch-dir> must be outside the repository; it is created if missing. Each mutant builds its own tree
# there (Debug, CMake target test_host_entry), so the run takes several minutes.
set -u

if [ $# -ne 1 ]; then
    echo "usage: $0 <scratch-dir>" >&2
    exit 2
fi
SCRATCH=$1
REPO=$(pwd)
if [ ! -f "$REPO/tests/scheduler/test_host_entry.c" ]; then
    echo "run from the repository root" >&2
    exit 2
fi
mkdir -p "$SCRATCH" || exit 2

copy_tree() {
    cp -r "$REPO/CMakeLists.txt" "$REPO/cmake" "$REPO/src" "$REPO/include" "$REPO/tests" "$REPO/capsules" \
        "$REPO/profiles" "$REPO/examples" "$1/" || exit 2
}

# The unmutated tree must configure, build and pass both host tests; otherwise no mutant result means anything.
baseline() {
    dir="$SCRATCH/_baseline"
    rm -rf "$dir"
    mkdir -p "$dir" || exit 2
    copy_tree "$dir"
    if ! cmake -S "$dir" -B "$dir/build" -DCMAKE_BUILD_TYPE=Debug >"$dir/cfg.log" 2>&1 ||
        ! cmake --build "$dir/build" --target test_host_entry test_host_restore -j4 >"$dir/build.log" 2>&1 ||
        ! (cd "$dir" && ./build/test_host_entry "$dir/run") >"$dir/run.log" 2>&1 ||
        ! (cd "$dir" && ./build/test_host_restore "$dir/run_restore") >"$dir/run_restore.log" 2>&1; then
        echo "BASELINE FAILED: the unmutated tree does not build or pass (see $dir/*.log); no mutant was run" >&2
        exit 3
    fi
    echo "BASELINE ok: unmutated tree builds and both host tests pass"
}
baseline

# mutant NAME FILE PERL_SUBSTITUTION
mutant() {
    name=$1
    file=$2
    expr=$3
    dir="$SCRATCH/$name"
    rm -rf "$dir"
    mkdir -p "$dir" || exit 2
    copy_tree "$dir"
    perl -0pi -e "$expr" "$dir/$file" || exit 2
    if cmp -s "$REPO/$file" "$dir/$file"; then
        echo "MUTANT $name: substitution did not apply (NOT APPLIED)"
        return
    fi
    if ! cmake -S "$dir" -B "$dir/build" -DCMAKE_BUILD_TYPE=Debug >"$dir/cfg.log" 2>&1 ||
        ! cmake --build "$dir/build" --target test_host_entry test_host_restore -j4 >"$dir/build.log" 2>&1; then
        echo "MUTANT $name: BUILD FAILED (a compile or link error, not a semantic kill; see $dir/build.log)"
        return
    fi
    if (cd "$dir" && ./build/test_host_entry "$dir/run") >"$dir/run.log" 2>&1 &&
        (cd "$dir" && ./build/test_host_restore "$dir/run_restore") >"$dir/run_restore.log" 2>&1; then
        echo "MUTANT $name: SURVIVED (both host tests passed)"
    else
        echo "MUTANT $name: killed ($(tail -1 "$dir/run.log" "$dir/run_restore.log" 2>/dev/null | grep -v '^$' | tail -1))"
    fi
}

# Host effects derived as empty (M3-M29): the declared access is ignored, so no conflict is seen.
mutant host_effects_ignored src/runtime/jobs/su_effects.c \
    's/if \(e->host_declared != 0u\) \{\n(\s+)\/\* Host entry/if (0) {\n$1\/* Host entry/'
# Dispatch: the host branch is skipped, so the host entry runs through a VM path with no context.
mutant dispatch_host_branch_skipped src/runtime/schedule.c \
    's/if \(s->sys\[i\]\.kind == SU_SYS_KIND_HOST\) \{/if (0) {/'
# Declared writes are not committed.
mutant host_commit_skipped src/runtime/schedule.c \
    's/return su_world_commit_staging\(w, wid, nw\);/return su_world_commit_staging(w, wid, 0u);/'
# Dead rows written by a host entry are not zeroed after its commit.
mutant host_dead_rows_not_zeroed src/runtime/schedule.c \
    's/if \(st == SU_OK\) \{\n(\s+)\/\* Dead rows are canonically zero/if (st == SU_OK \&\& s->sys[i].kind != SU_SYS_KIND_HOST) {\n$1\/* Dead rows are canonically zero/'
# A failing host run is ignored and its staging is committed.
mutant host_failure_ignored src/runtime/schedule.c \
    's/st = d->run\(w, &inv, &out, d->host_ctx\);\n    if \(st != SU_OK\) \{\n        return st;\n    \}/st = d->run(w, \&inv, \&out, d->host_ctx);\n    st = SU_OK;/'
# Candidate for a host entry is accepted.
mutant candidate_host_accepted src/runtime/schedule.c \
    's/if \(idx >= 0 && s->sys\[idx\]\.kind == SU_SYS_KIND_HOST\) \{/if (idx >= 0 \&\& s->sys[idx].kind == 99u) {/'
# Host entry gets a VM context built from its declaration.
mutant host_gets_vm_context src/runtime/schedule.c \
    's/e->mod = decl;\n    e->ctx = NULL;/e->mod = decl;\n    (void)su_vm_context_create(decl, gen, \&e->ctx);/'
# Capsule constants hash not compared.
mutant load_constants_hash_unchecked src/runtime/rts/host_decl.c \
    's/strcmp\(jstr\(jmember\(host, "constants_hash"\)\), hex\) != 0/0/'
# Declared access not compared.
mutant load_access_unchecked src/runtime/rts/host_decl.c \
    's/access_of_name\(acc\) != d->fields\[i\]\.access/access_of_name(acc) == 0u/'
# Field count not compared (a matching prefix followed by extra fields is accepted).
mutant load_field_count_unchecked src/runtime/rts/host_decl.c \
    's/fields->u\.array\.count != d->field_count/0/'
# Equivalent: the host runs its declared fields in descending order. The order of views and of the commit
# list does not change the state (the operator looks columns up by ID and every column is committed whole).
mutant host_field_order_reversed_equivalent src/runtime/schedule.c \
    's/for \(i = 0; i < d->field_count; i\+\+\) \{\n        const SuHostField \*f = &d->fields\[i\];/for (i = d->field_count; i-- > 0u;) {\n        const SuHostField *f = \&d->fields[i];/'
# Restore: the host branch of rebuild_schedule is removed (M3-M34, T-HOST-11).
mutant restore_host_branch_removed src/runtime/worker.c \
    's/if \(c->kind == SU_SYS_KIND_HOST\) \{/if (0) {/'
# No working-to-staging copy before run (M3-M32, T-HOST-13 a): non-target rows keep stale staging.
mutant host_staging_copy_removed src/runtime/schedule.c \
    's/memcpy\(full_out\.bindings\[j\]\.data, full_inv\.bindings\[k\]\.data,/if (0) memcpy(full_out.bindings[j].data, full_inv.bindings[k].data,/'
# Gap (native build only, no M3-04a test reaches the worker guard): the native install refusal is removed.
mutant native_host_guard_removed src/runtime/worker.c \
    's/if \(ww->sched\.sys\[j\]\.kind == SU_SYS_KIND_HOST\) \{\n(\s+)\/\* A host entry has no IR/if (0) {\n$1\/* A host entry has no IR/'

echo "host mutants finished; equivalents and gaps are judged by the caller from the output"
