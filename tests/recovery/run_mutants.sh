#!/bin/sh
# Mutation check for the C07 containment filters, the worker self-sandbox, and the supervisor's digest and
# spool guards (M1 closure D).
#
# Each mutant is one deliberate bug applied to a SCRATCH copy of the repository (never to the repository
# itself). The copy is configured and built (default, Release, -Werror through the CMake warning set); the
# mutant must still build, and the named ctest selection must fail for it. A mutant whose selection still
# passes SURVIVED. A mutant whose anchor does not occur exactly once, or whose substitution does not change
# the file, is INVALID and the script exits non-zero.
#
# Usage, from the repository root:
#   sh tests/recovery/run_mutants.sh <scratch-dir>
# <scratch-dir> must be outside the repository; it is created if missing.
# MUTANT_JOBS=N sets the build parallelism (default 8). MUTANT_ONLY="M-ARCH M-X32" runs only the named mutants.
set -u

if [ $# -ne 1 ]; then
    echo "usage: $0 <scratch-dir>" >&2
    exit 2
fi
SCRATCH=$1
REPO=$(pwd)
case "$SCRATCH" in
    "$REPO"|"$REPO"/*) echo "scratch dir must be outside the repository" >&2; exit 2 ;;
esac
if [ ! -f "$REPO/tests/recovery/run_mutants.sh" ] || [ ! -f "$REPO/CMakeLists.txt" ]; then
    echo "run from the repository root" >&2
    exit 2
fi
JOBS=${MUTANT_JOBS:-8}
ONLY=${MUTANT_ONLY:-}
mkdir -p "$SCRATCH" || exit 2
SUMMARY="$SCRATCH/summary.txt"
: >"$SUMMARY"
rc_all=0

# mutant NAME FILE ANCHOR SED_PATTERN SED_REPLACEMENT CTEST_SELECTION
# ANCHOR is a fixed string that must occur exactly once in FILE. SED_PATTERN is a basic regular
# expression for the same text, SED_REPLACEMENT the mutated text. The sed delimiter is '#'.
mutant() {
    name=$1
    file=$2
    anchor=$3
    pat=$4
    rep=$5
    sel=$6
    case " $ONLY " in
        *" $name "*) ;;
        *) if [ -n "$ONLY" ]; then return; fi ;;
    esac
    dir="$SCRATCH/$name"
    rm -rf "$dir"
    mkdir -p "$dir" || exit 2
    (cd "$REPO" && tar --exclude=.git --exclude=./.claude --exclude=./build -cf - .) | (cd "$dir" && tar -xf -) || exit 2
    hits=$(grep -cF -- "$anchor" "$dir/$file")
    # A seventh argument "first" mutates only the first matching line (the anchor may repeat in the file).
    if [ "${7:-}" = "first" ]; then
        if [ "$hits" -lt 1 ]; then
            echo "MUTANT $name: INVALID anchor not found in $file" | tee -a "$SUMMARY"
            rc_all=1
            return
        fi
        sed -i "0,/$pat/s#$pat#$rep#" "$dir/$file" || exit 2
    else
        if [ "$hits" != "1" ]; then
            echo "MUTANT $name: INVALID anchor occurs $hits times in $file" | tee -a "$SUMMARY"
            rc_all=1
            return
        fi
        sed -i "s#$pat#$rep#" "$dir/$file" || exit 2
    fi
    if cmp -s "$REPO/$file" "$dir/$file"; then
        echo "MUTANT $name: INVALID substitution did not change $file" | tee -a "$SUMMARY"
        rc_all=1
        return
    fi
    if ! cmake -S "$dir" -B "$dir/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        >"$dir/configure.log" 2>&1 ||
       ! cmake --build "$dir/build" -j"$JOBS" >"$dir/build.log" 2>&1; then
        echo "MUTANT $name: INVALID mutated tree does not build (see $dir/build.log)" | tee -a "$SUMMARY"
        rc_all=1
        return
    fi
    if (cd "$dir/build" && ctest -R "$sel" --timeout 900 -j2) >"$dir/ctest.log" 2>&1; then
        echo "MUTANT $name: SURVIVED (ctest -R '$sel' passed)" | tee -a "$SUMMARY"
        rc_all=1
    else
        echo "MUTANT $name: killed (ctest -R '$sel': $(grep -E 'tests passed|tests failed' "$dir/ctest.log" | tail -1))" | tee -a "$SUMMARY"
    fi
}

# M-ARCH: the wrong-architecture verdict of the network filter is ALLOW instead of KILL.
mutant M-ARCH src/agent/su_proc.c \
    'SECCOMP_RET_KILL_PROCESS); /* wrong arch */' \
    'SECCOMP_RET_KILL_PROCESS); /\* wrong arch \*/' \
    'SECCOMP_RET_ALLOW); /* wrong arch */' \
    'containment'
# M-X32: the x32 guard threshold is 0x80000000, so x32 syscall numbers (0x40000000 | nr) pass unchecked.
mutant M-X32 src/agent/su_proc.c \
    '0x40000000u,' \
    '0x40000000u,' \
    '0x80000000u,' \
    'containment'
# M-SOCKETPAIR: socketpair (53) is no longer in the network denial list.
mutant M-SOCKETPAIR src/agent/su_proc.c \
    '50u,  53u,  288u,' \
    '50u,  53u,  288u,' \
    '50u,  288u,' \
    'containment'
# M-URING: io_uring_setup (425) is no longer denied: the IORING_OP_SOCKET bypass is open again.
mutant M-URING src/agent/su_proc.c \
    '425u, 426u, 427u,' \
    '425u, 426u, 427u,' \
    '426u, 427u,' \
    'containment'
# M-EXECVE: the worker self-sandbox allows execve (the first exec-denied number is removed).
mutant M-EXECVE src/agent/su_sandbox.c \
    '{57u, 58u, 59u, 322u};' \
    '{57u, 58u, 59u, 322u};' \
    '{57u, 58u, 322u};' \
    'containment'
# M-CLONE: the self-sandbox allows every clone, including fork-style clones without CLONE_THREAD.
mutant M-CLONE src/agent/su_sandbox.c \
    '(unsigned char)(clone_check_at - i - 1u), 0u);' \
    '(unsigned char)(clone_check_at - i - 1u), 0u);' \
    '(unsigned char)(allow_at - i - 1u), 0u);' \
    'containment'
# M-DIGEST-SHA: the digest comparison ignores the expected snapshot SHA-256.
mutant M-DIGEST-SHA src/agent/su_agent.c \
    '(sha_want == NULL || strcmp(sha_got, sha_want) == 0)' \
    '(sha_want == NULL || strcmp(sha_got, sha_want) == 0)' \
    '(sha_want == NULL || strcmp(sha_got, sha_want) == 0 || 1)' \
    'hardening'
# M-DIGEST-REV: the digest comparison ignores the revision string. (The replacement keeps the operands in use,
# so the -Werror build of the mutant still succeeds.)
mutant M-DIGEST-REV src/agent/su_agent.c \
    'strcmp(rev_got, rev_want) == 0 && tick_got == tick_want' \
    'strcmp(rev_got, rev_want) == 0 && tick_got == tick_want' \
    '(strcmp(rev_got, rev_want) == 0 || 1) \&\& tick_got == tick_want' \
    'hardening'
# M-DIGEST-TICK-RERUN (closure C M06): the digest comparison ignores the tick. The replacement keeps tick_got and
# tick_want in use, so the -Werror build still succeeds. The call at su_agent.c (quarantine rollback, sha NULL)
# compares only the tick and revision, so this mutant is not masked by the snapshot SHA there.
mutant M-DIGEST-TICK-RERUN src/agent/su_agent.c \
    'strcmp(rev_got, rev_want) == 0 && tick_got == tick_want &&' \
    'strcmp(rev_got, rev_want) == 0 && tick_got == tick_want &&' \
    'strcmp(rev_got, rev_want) == 0 \&\& (tick_got == tick_want || 1) \&\&' \
    'hardening'
# M-INSTALL-REV: the install handler (su_reload_install, src/runtime/reload.c) no longer rejects a stale expected
# world revision at its entry check (step 2, the first of the two identical lines; the step-8 admission recheck
# on the second line is left in place). Selected by the first matching line.
mutant M-INSTALL-REV src/runtime/reload.c \
    'if (strcmp(req->expected_revision, req->current_revision) != 0) {' \
    'if (strcmp(req->expected_revision, req->current_revision) != 0) {' \
    'if (strcmp(req->expected_revision, req->current_revision) != 0 \&\& 0) {' \
    'reload' first
# M-SPOOL-MODE: the spool directory's group and world access check is removed.
mutant M-SPOOL-MODE src/agent/su_spool.c \
    '|| (st.st_mode & 077) != 0) {' \
    '|| (st.st_mode & 077) != 0) {' \
    '|| 0) {' \
    'hardening'

echo "summary: $SUMMARY"
cat "$SUMMARY"
exit $rc_all
