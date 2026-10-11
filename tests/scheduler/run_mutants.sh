#!/bin/sh
# Mutation check for tests/scheduler/test_scheduler.c (M1-04).
#
# Each mutant is one deliberate bug applied to a SCRATCH copy of src/ and include/
# (never to the repository). The scheduler test must fail to build or fail at run
# time for every mutant. A mutant that still passes means the test misses that bug.
#
# Usage, from the repository root:
#   sh tests/scheduler/run_mutants.sh <scratch-dir>
# <scratch-dir> must be outside the repository; it is created if missing.
set -u

if [ $# -ne 1 ]; then
    echo "usage: $0 <scratch-dir>" >&2
    exit 2
fi
SCRATCH=$1
REPO=$(pwd)
if [ ! -f "$REPO/tests/scheduler/test_scheduler.c" ]; then
    echo "run from the repository root" >&2
    exit 2
fi
mkdir -p "$SCRATCH" || exit 2

CFLAGS="-std=c17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic -pthread"
SRCS="src/ir/su_json.c src/ir/su_lir.c src/ir/su_sha256.c src/world/su_world.c src/vm/su_vm.c src/runtime/jobs/su_jobs.c src/runtime/jobs/su_effects.c"

# mutant NAME FILE PERL_SUBSTITUTION
mutant() {
    name=$1
    file=$2
    expr=$3
    dir="$SCRATCH/$name"
    rm -rf "$dir"
    mkdir -p "$dir" || exit 2
    cp -r "$REPO/src" "$REPO/include" "$dir/" || exit 2
    perl -0pi -e "$expr" "$dir/src/$file" || exit 2
    if cmp -s "$REPO/src/$file" "$dir/src/$file"; then
        echo "MUTANT $name: substitution did not apply (SURVIVED)"
        return
    fi
    # shellcheck disable=SC2086
    if ! (cd "$dir" && gcc $CFLAGS -Iinclude -Isrc $SRCS "$REPO/tests/scheduler/test_scheduler.c" \
        -o "$dir/test_scheduler") >"$dir/build.log" 2>&1; then
        echo "MUTANT $name: killed (does not build; see $dir/build.log)"
        return
    fi
    if (cd "$REPO" && "$dir/test_scheduler") >"$dir/run.log" 2>&1; then
        echo "MUTANT $name: SURVIVED (test passed)"
    else
        echo "MUTANT $name: killed ($(tail -1 "$dir/run.log"))"
    fi
}

# Last writer wins on failure status: the merge keeps scanning and reports the highest failing chunk.
mutant last_writer_wins_on_failure runtime/jobs/su_jobs.c \
    's/failed = i;\n\s*break;/failed = i;/'
# Off-by-one chunk boundary: every chunk after the first starts one row late.
mutant chunk_boundary_off_by_one runtime/jobs/su_jobs.c \
    's/b = index \* eff;/b = index * eff + (index > 0u ? 1u : 0u);/'
# Commit on failure: staging is copied to working even when a chunk failed.
mutant commit_on_failure runtime/jobs/su_jobs.c \
    's/st = status\[failed\];/(void)su_world_commit_staging(w, write_ids, nout); st = status[failed];/'
# No completion gate: the completion order is not forced, so the seeded log check must fail.
mutant no_completion_gate runtime/jobs/su_jobs.c \
    's/while \(p->completed != pos\)/while (0)/'
# No conflict rejection for parallel systems.
mutant no_conflict_check runtime/jobs/su_effects.c \
    's/if \(g->conf\[i \* n \+ j\] == 0u \|\|/if (1 ||/'
# No ordering-cycle rejection (Kahn then stalls and reports an internal error).
mutant no_cycle_check runtime/jobs/su_effects.c \
    's/if \(g->reach\[i \* n \+ i\] != 0u\) \{/if (0) {/'
