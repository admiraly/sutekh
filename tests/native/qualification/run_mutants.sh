#!/bin/sh
# Mutation check for the native qualification tests (M1 closure).
#
# Each mutant is one deliberate bug applied to a SCRATCH copy of the repository (never to the
# repository itself). The copy is configured and built (native, Release, -Werror through the CMake
# warning set); the mutant must still build, and the relevant ctest selection must fail for it.
# A mutant whose selection still passes SURVIVED. A mutant that does not build, or whose anchor
# does not occur exactly once, or whose substitution does not change the file, is INVALID and the
# script exits non-zero.
#
# Usage, from the repository root:
#   sh tests/native/qualification/run_mutants.sh <scratch-dir>
# <scratch-dir> must be outside the repository; it is created if missing.
# MUTANT_JOBS=N sets the build parallelism (default 8).
# MUTANT_ONLY="H1 L2" runs only the named mutants.
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
if [ ! -f "$REPO/tests/native/qualification/run_mutants.sh" ] || [ ! -f "$REPO/CMakeLists.txt" ]; then
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
# expression for the same text (brackets and dots escaped as needed), SED_REPLACEMENT the mutated
# text. The sed delimiter is '#'; '&' in a replacement is escaped as '\&'.
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
    if ! cmake -S "$dir" -B "$dir/build" -G Ninja -DSUTEKH_NATIVE=ON -DCMAKE_BUILD_TYPE=Release \
        >"$dir/configure.log" 2>&1 ||
       ! cmake --build "$dir/build" -j"$JOBS" >"$dir/build.log" 2>&1; then
        echo "MUTANT $name: INVALID mutated tree does not build (see $dir/build.log)" | tee -a "$SUMMARY"
        rc_all=1
        return
    fi
    case "$sel" in
        harness:*)
            # A binary-level mutant: the production binary of the mutated tree must fail harness TX6.
            tx=${sel#harness:}
            if (cd "$dir" && python3 -I tests/harness/run_m0.py --binary "$dir/build/sutekh" --no-build \
                --only "$tx" --report-dir "$dir/harness") >"$dir/harness.log" 2>&1; then
                echo "MUTANT $name: SURVIVED (harness $tx passed)" | tee -a "$SUMMARY"
                rc_all=1
            else
                echo "MUTANT $name: killed (harness $tx failed: $(grep -E '^totals' "$dir/harness.log" | tail -1))" | tee -a "$SUMMARY"
            fi
            return
            ;;
    esac
    if (cd "$dir/build" && ctest -R "$sel" --timeout 900 -j2) >"$dir/ctest.log" 2>&1; then
        echo "MUTANT $name: SURVIVED (ctest -R '$sel' passed)" | tee -a "$SUMMARY"
        rc_all=1
    else
        echo "MUTANT $name: killed (ctest -R '$sel': $(grep -E 'tests passed|tests failed' "$dir/ctest.log" | tail -1))" | tee -a "$SUMMARY"
    fi
}

# H1: artifact-hash equality in su_nv_trust_admitted (native_versions.c).
mutant H1 src/runtime/native_versions.c \
    'if (strcmp(a->artifact_hash, admitted_hash) != 0) {' \
    'if (strcmp(a->artifact_hash, admitted_hash) != 0) {' \
    'if (strcmp(a->artifact_hash, admitted_hash) != 0 \&\& 0) {' \
    specialize_admission
# L1: layout-generation check at admission (worker.c, h_internal_specialize_admit).
mutant L1 src/runtime/worker.c \
    'if ((uint64_t)gen != exp_gen) {' \
    'if ((uint64_t)gen != exp_gen) {' \
    'if ((uint64_t)gen != exp_gen \&\& 0) {' \
    specialize_admission
# L2: semantic-hash check at admission (worker.c, h_internal_specialize_admit).
mutant L2 src/runtime/worker.c \
    'if (strcmp(sched.sys[0].semantic_hash, exp_sem) != 0) {' \
    'if (strcmp(sched.sys\[0\].semantic_hash, exp_sem) != 0) {' \
    'if (strcmp(sched.sys[0].semantic_hash, exp_sem) != 0 \&\& 0) {' \
    specialize_admission
# Five further M1 audit mutants (planning/audit/M1-audit.md, section 10).
# S1: scalar ADD emits sub.
mutant S1 backends/native/su_native_asmjit.cpp \
    'a.add(eax, dword_ptr(rsp, sb));' \
    'a.add(eax, dword_ptr(rsp, sb));' \
    'a.sub(eax, dword_ptr(rsp, sb));' \
    'native_scalar|native_avx2'
# S3: scalar MUL emits add.
mutant S3 backends/native/su_native_asmjit.cpp \
    'a.imul(eax, dword_ptr(rsp, sb));' \
    'a.imul(eax, dword_ptr(rsp, sb));' \
    'a.add(eax, dword_ptr(rsp, sb));' \
    'native_scalar|native_avx2'
# S6: scalar EQ inverted.
mutant S6 backends/native/su_native_asmjit.cpp \
    'a.sete(al);' \
    'a.sete(al);' \
    'a.setne(al);' \
    'native_scalar|native_avx2'
# W2: wrapper input layout-generation check disabled (su_native.c).
mutant W2 backends/native/su_native.c \
    'if (input->layout_generation != a->info.layout_generation) {' \
    'if (input->layout_generation != a->info.layout_generation) {' \
    'if (input->layout_generation != a->info.layout_generation \&\& 0) {' \
    'native_scalar|native_avx2'
# P1: the published mapping stays RWX (the final mprotect to R+X is weakened to R+W+X).
mutant P1 backends/native/su_native.c \
    'mprotect(p, map, PROT_READ | PROT_EXEC) != 0) {' \
    'mprotect(p, map, PROT_READ | PROT_EXEC) != 0) {' \
    'mprotect(p, map, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {' \
    'native_scalar|native_versions|specialize_churn'

# M1 closure C: safe retirement and the quarantine key (test_safe_retire, and the containment test).
# M07: the decrement of the in-flight count on the run path (the first of two identical lines).
mutant M07 src/runtime/native_versions.c \
    '    a->inflight--;' \
    '    a->inflight--;' \
    '    a->inflight -= 0u;' \
    'safe_retire|specialize_inflight' first
# R1: su_nv_release frees at once, whatever is in flight (ignore the in-flight count).
mutant R1 src/runtime/native_versions.c \
    '    if (a->inflight == 0u) {' \
    '    if (a->inflight == 0u) {' \
    '    if (1) {' \
    'safe_retire'
# R2: su_nv_release frees when only the caller's own call is in flight (one call early).
mutant R2 src/runtime/native_versions.c \
    '    if (a->inflight == 0u) {' \
    '    if (a->inflight == 0u) {' \
    '    if (a->inflight <= 1u) {' \
    'safe_retire'
# M10: quarantine key compares only the first 8 characters of the artifact hash.
mutant M10 src/agent/su_agent.c \
    'strcmp(a->quar[i].artifact_hash, hash) == 0' \
    'strcmp(a->quar\[i\]\.artifact_hash, hash) == 0' \
    'strncmp(a->quar[i].artifact_hash, hash, 8) == 0' \
    'safe_retire'
# R3: N > 1 row order. Chunk k (k >= 1) reads the input rows of chunk k-1 while it writes the output rows
# of chunk k (an input/output offset mismatch). Bounds stay valid: the input range ends inside the table.
mutant R3 src/runtime/native_versions.c \
    'nv_slice(&j->base_in[i], &si[i], begin, len);' \
    'nv_slice(&j->base_in\[i\], &si\[i\], begin, len);' \
    'nv_slice(\&j->base_in[i], \&si[i], chunk >= 1u ? begin - j->chunk_rows : begin, len);' \
    'native_chunked'
# C1: the child does not install the network denial (seccomp and the namespace fallback are both skipped).
mutant C1 src/agent/su_proc.c \
    'if (limits != NULL && limits->deny_network) {' \
    'if (limits != NULL && limits->deny_network) {' \
    'if (0) {' \
    'containment'
# C2: RLIMIT_FSIZE not applied to children.
mutant C2 src/agent/su_proc.c \
    'setrlimit(RLIMIT_FSIZE, &rl);' \
    'setrlimit(RLIMIT_FSIZE, &rl);' \
    '(void)rl;' \
    'containment'
# C3: SIGXFSZ not ignored (an over-limit write then kills the child instead of failing with EFBIG).
mutant C3 src/agent/su_proc.c \
    'signal(SIGXFSZ, SIG_IGN);' \
    'signal(SIGXFSZ, SIG_IGN);' \
    '(void)0;' \
    'containment'
# C4: RLIMIT_NOFILE not applied to children.
mutant C4 src/agent/su_proc.c \
    'setrlimit(RLIMIT_NOFILE, &rl);' \
    'setrlimit(RLIMIT_NOFILE, &rl);' \
    '(void)rl;' \
    'containment'

# M-QUAR-FIND: the quarantine lookup never matches (quar_find, su_agent.c). The replacement keeps every
# operand in use, so the -Werror build of the mutant still succeeds.
mutant M-QUAR-FIND src/agent/su_agent.c \
    'if (a->quar[i].used && strcmp(a->quar[i].artifact_hash, hash) == 0) {' \
    'if (a->quar\[i\]\.used && strcmp(a->quar\[i\]\.artifact_hash, hash) == 0) {' \
    'if (a->quar[i].used \&\& strcmp(a->quar[i].artifact_hash, hash) == 0 \&\& 0) {' \
    'safe_retire|native_fault|specialize_churn|specialize_inflight|specialize_admission'

# M12: the active and disposable workers' address-space bound is 1 GiB instead of 4 GiB (binary level; the
# harness TX6 checks the exact 4 GiB value in /proc/<pid>/limits).
mutant M12 src/platform/sutekh_main.c \
    'cfg.worker_address_space_bytes = UINT64_C(4) * 1024u * 1024u * 1024u;' \
    'cfg.worker_address_space_bytes = UINT64_C(4) \* 1024u \* 1024u \* 1024u;' \
    'cfg.worker_address_space_bytes = UINT64_C(1) * 1024u * 1024u * 1024u;' \
    'harness:TX6'

echo "summary: $SUMMARY"
cat "$SUMMARY"
exit $rc_all
