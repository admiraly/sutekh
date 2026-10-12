#!/bin/sh
# M2-01b mutation proofs for the migration runtime. Each mutant is one targeted edit to a source file; the script
# rebuilds recovery_worker and test_migrate_runtime, requires the test to FAIL (exit non-zero), and restores the file.
# A mutant whose edit does not apply, or whose build fails, is reported as an error (not as a kill).
#
# usage: run_mutants.sh <repo_root> <build_dir>
set -u
REPO=$1
BUILD=$2
SCRATCH="$BUILD/migration_mutant_scratch"
WORKER="$BUILD/recovery_worker"
TEST="$BUILD/test_migrate_runtime"
SUPER="$REPO/src/agent/su_agent.c"
WRK="$REPO/src/runtime/worker.c"
BAK_DIR="$BUILD/migration_mutant_backup"
mkdir -p "$BAK_DIR"
fails=0
kills=0
total=0

restore_all() {
    for f in "$SUPER" "$WRK"; do
        b="$BAK_DIR/$(basename "$f").orig"
        if [ -f "$b" ]; then
            cp "$b" "$f"
        fi
    done
}
trap 'restore_all; exit 130' INT TERM

cp "$SUPER" "$BAK_DIR/su_agent.c.orig"
cp "$WRK" "$BAK_DIR/worker.c.orig"

# run_mutant <name> <file> <kind> <expression...>
#   kind sed: expression is a sed script; kind perl: a multi-line perl substitution.
run_mutant() {
    name=$1
    file=$2
    kind=$3
    expr=$4
    total=$((total + 1))
    cp "$file" "$BAK_DIR/mutant.tmp"
    if [ "$kind" = "sed" ]; then
        sed -i "$expr" "$file"
    else
        perl -0pi -e "$expr" "$file"
    fi
    if cmp -s "$file" "$BAK_DIR/mutant.tmp"; then
        echo "ERROR $name: the mutant edit did not apply"
        cp "$BAK_DIR/mutant.tmp" "$file"
        fails=$((fails + 1))
        return
    fi
    if ! cmake --build "$BUILD" --target recovery_worker test_migrate_runtime -j4 >"$BAK_DIR/build.log" 2>&1; then
        echo "ERROR $name: the mutant does not build (see $BAK_DIR/build.log)"
        cp "$BAK_DIR/mutant.tmp" "$file"
        fails=$((fails + 1))
        return
    fi
    rm -rf "$SCRATCH"
    mkdir -p "$SCRATCH"
    if timeout 900 "$TEST" "$WORKER" "$REPO" "$SCRATCH" >"$BAK_DIR/run.log" 2>&1; then
        echo "SURVIVED $name: the migration test still passes"
        fails=$((fails + 1))
    else
        echo "KILLED $name"
        kills=$((kills + 1))
    fi
    cp "$BAK_DIR/mutant.tmp" "$file"
}

run_mutant "M01 commit does not log LOG_MIGRATE" "$SUPER" sed \
    's/rc = sync_after(a, w, LOG_MIGRATE, &lop, 0, 1);/rc = sync_after(a, w, LOG_NONE, \&lop, 0, 1);/'
run_mutant "M02 plan validity ignored" "$SUPER" sed \
    's/pr->valid = vv != NULL \&\& vv->type == SU_JSON_BOOL \&\& vv->u.boolean != 0;/(void)vv; pr->valid = 1;/'
run_mutant "M03 stale plan not refused" "$SUPER" sed \
    's/if (strcmp(expect, cur) != 0 || pr->rev != w->rev) {/if (0) {/'
run_mutant "M04 SU_EVENT_MIGRATED not kinded" "$WRK" sed \
    's/    ev.kind = SU_EVENT_MIGRATED;/    ev.kind = 0u;/'
run_mutant "M05 recovery ignores migrated base" "$SUPER" sed \
    's/    if (w->ck_count > 0 \&\& w->ck\[w->ck_count - 1\].migrated) {/    if (0) {/'
run_mutant "M06 replay sends wrong target generation" "$SUPER" sed \
    's/su_buf_u64(&req, op->mig.to_gen);/su_buf_u64(\&req, op->mig.to_gen + 1u);/'
run_mutant "M07 rollback pin not kept" "$SUPER" sed \
    's/^    w->pin_has = 1;$/    w->pin_has = 0;/'
run_mutant "M08 post-swap capture seam not armed" "$WRK" sed \
    's/        ww->post_ck = 1;/        ww->post_ck = 0;/'
run_mutant "M09 swap does not arm the capture seam" "$WRK" sed \
    's/    ww->post_swap = 1;/    ww->post_swap = 0;/'
run_mutant "M10 migrated flag set after the checkpoint" "$SUPER" perl \
    's/    w->migrated = 1;\n    t0 = mono_ns\(\);/    t0 = mono_ns();/'

restore_all
echo "mutants: $total run, $kills killed, $fails not killed or not run"
if [ "$fails" -ne 0 ]; then
    exit 1
fi
exit 0
